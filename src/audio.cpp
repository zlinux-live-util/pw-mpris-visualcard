// PipeWire audio capture implementation.
//
// How a player's stream is reached
// --------------------------------
// A player publishes a Stream/Output/Audio node. That node has no ports a third party can read,
// so the graph is asked for its monitor side: an input stream whose target.object names the node
// makes PipeWire create (or reuse) that node's monitor ports and link them to us. What comes out
// is the signal the player submitted, which is exactly what a visualiser wants. No source, no
// sink, no mixer, nothing to re-route.
//
// Getting from "MPRIS says musicfox is playing" to "node 83" is the part with no API. It is done
// by enumerating the registry and matching the player name against the audio nodes' identity
// properties. The matching rules and the measurements behind them are in docs/internals.md.
//
// Threading
// ---------
// One thread loop owns everything below. Its realtime callback only mixes the channels of one
// buffer down to mono and writes a ring; nothing else may happen there. Node resolution and
// (re)connection run on a timer on that same loop, so a target change never blocks a renderer.
#include "audio.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oms {
namespace {

/** How often the loop thread reconsiders the target: fast enough that a player change shows up
 *  within a frame or two, slow enough to be free. */
constexpr int kResolveIntervalMs = 250;

/** How a candidate is ranked. A stream is the player's own output, a sink is what it plays into,
 *  an input stream is something it records; monitoring the stream is the signal itself, so the
 *  three are scored rather than filtered, which also keeps a player that only exposes a sink
 *  visible instead of silently missing. */
int classScore(const std::string& mediaClass) {
  if (mediaClass == "Stream/Output/Audio") return 3;
  if (mediaClass == "Audio/Sink") return 2;
  if (mediaClass == "Stream/Input/Audio") return 1;
  return 0;
}

/** Lowercase copy. Only used when the target changes, so allocation costs nothing. */
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

/** Whole-word, case-insensitive containment: "musicfox" is a word of both "alsa_playback.musicfox"
 *  and "PipeWire ALSA [musicfox]", but not of "musicfoxd". */
bool hasWord(const std::string& haystack, const std::string& word) {
  if (haystack.empty() || word.empty()) return false;
  const std::string h = lower(haystack), w = lower(word);
  for (size_t pos = h.find(w); pos != std::string::npos; pos = h.find(w, pos + w.size())) {
    const bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(h[pos - 1]));
    const size_t end = pos + w.size();
    const bool rightOk = end == h.size() || !std::isalnum(static_cast<unsigned char>(h[end]));
    if (leftOk && rightOk) return true;
  }
  return false;
}

struct NodeInfo {
  std::string serial;      // object.serial: unambiguous, and what the session manager resolves first
  std::string name;        // node.name
  std::string description; // node.description
  std::string mediaName;   // media.name
  std::string appName;     // application.name
  std::string appId;       // application.id
  std::string mediaClass;
};

}  // namespace

struct AudioTap::Impl {
  bool verbose = false;

  pw_thread_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_registry* registry = nullptr;
  pw_stream* stream = nullptr;  // loop thread only; liveStream is what the audio thread reads
  spa_hook registryHook{};
  spa_source* timer = nullptr;
  bool started = false;

  /* ---- registry view. Written and read on the loop thread only. ---- */
  std::unordered_map<uint32_t, NodeInfo> nodes;

  /* ---- stream identity, written on the loop thread under its lock ---- */
  std::string resolved;  // target.object the current stream is connected to

  /* ---- read by the audio thread ---- */
  std::atomic<pw_stream*> liveStream{nullptr};
  std::atomic<int> rate{0};
  std::atomic<int> channels{0};
  std::atomic<bool> streaming{false};

  /* ---- single-producer ring: the audio callback writes, the render thread reads ---- */
  std::vector<float> ring;
  size_t ringMask = 0;
  std::atomic<uint64_t> written{0};
  uint64_t readPos = 0;

  /* ---- target selection: written by the render thread, read by the loop timer ---- */
  mutable std::mutex wantMu;
  std::string wantTarget;
  bool wantActive = false;

  /* ---- what the log lines and status() report ---- */
  mutable std::mutex statusMu;
  std::string statusNode;
  bool reportedMissing = false;

  static void onGlobal(void* data, uint32_t id, uint32_t perms, const char* type, uint32_t version,
                       const spa_dict* props);
  static void onGlobalRemove(void* data, uint32_t id);
  static void onStreamState(void* data, pw_stream_state old, pw_stream_state st, const char* err);
  static void onStreamParam(void* data, uint32_t id, const spa_pod* param);
  static void onStreamProcess(void* data);
  static void onTimer(void* data, uint64_t expirations);

  /** Picks the node a target name refers to and, with `id`, its registry id. */
  bool findTarget(const std::string& target, NodeInfo& out, uint32_t& id) const;
  void resolveAndConnect();
  void disconnect();
  void reportMissing(const std::string& target);
};

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

void AudioTap::Impl::onGlobal(void* data, uint32_t id, uint32_t, const char* type, uint32_t,
                              const spa_dict* props) {
  // Only nodes matter here, and the registry event already carries their full property set, so
  // there is no reason to bind one proxy per node just to read these seven strings.
  if (!type || std::strcmp(type, "PipeWire:Interface:Node") != 0 || !props) return;
  NodeInfo n;
  for (uint32_t i = 0; i < props->n_items; ++i) {
    const char* k = props->items[i].key;
    const char* v = props->items[i].value;
    if (!k || !v) continue;
    if (!std::strcmp(k, PW_KEY_OBJECT_SERIAL)) n.serial = v;
    else if (!std::strcmp(k, PW_KEY_NODE_NAME)) n.name = v;
    else if (!std::strcmp(k, PW_KEY_NODE_DESCRIPTION)) n.description = v;
    else if (!std::strcmp(k, PW_KEY_MEDIA_NAME)) n.mediaName = v;
    else if (!std::strcmp(k, PW_KEY_APP_NAME)) n.appName = v;
    else if (!std::strcmp(k, PW_KEY_APP_ID)) n.appId = v;
    else if (!std::strcmp(k, PW_KEY_MEDIA_CLASS)) n.mediaClass = v;
  }
  static_cast<Impl*>(data)->nodes[id] = std::move(n);
}

void AudioTap::Impl::onGlobalRemove(void* data, uint32_t id) {
  static_cast<Impl*>(data)->nodes.erase(id);
}

bool AudioTap::Impl::findTarget(const std::string& target, NodeInfo& out, uint32_t& id) const {
  // MPRIS hands back the bus name suffix, which may carry an instance suffix
  // ("firefox.instance12"); the full name is tried first, so it wins over any prefix match.
  std::vector<std::string> words{target};
  const size_t dot = target.rfind('.');
  if (dot != std::string::npos && dot + 1 < target.size()) words.push_back(target.substr(0, dot));

  for (const std::string& word : words) {
    const NodeInfo* best = nullptr;
    uint32_t bestId = 0;
    int bestScore = 0;
    for (const auto& [nid, n] : nodes) {
      const int score = classScore(n.mediaClass);
      if (score == 0) continue;
      // Identity is checked strongest first: the application id is set by the player itself,
      // node.name last, because it is the field players most often derive from the app name, so
      // a match there is the weakest evidence.
      const bool hit = hasWord(n.appId, word) || hasWord(n.appName, word) ||
                       hasWord(n.mediaName, word) || hasWord(n.description, word) ||
                       hasWord(n.name, word);
      if (!hit || score <= bestScore) continue;
      best = &n;
      bestId = nid;
      bestScore = score;
    }
    if (best) {
      out = *best;
      id = bestId;
      return true;
    }
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* Stream                                                              */
/* ------------------------------------------------------------------ */

void AudioTap::Impl::disconnect() {
  // Published as null first so the audio callback cannot dequeue from a stream that is going away.
  liveStream.store(nullptr, std::memory_order_release);
  if (stream) {
    pw_stream_destroy(stream);
    stream = nullptr;
  }
  resolved.clear();
  rate.store(0, std::memory_order_relaxed);
  channels.store(0, std::memory_order_relaxed);
  streaming.store(false, std::memory_order_relaxed);
  written.store(0, std::memory_order_relaxed);
  readPos = 0;
}

void AudioTap::Impl::reportMissing(const std::string& target) {
  std::lock_guard lk(statusMu);
  if (reportedMissing) return;
  reportedMissing = true;
  statusNode.clear();
  std::fprintf(stderr,
               "[audio] no PipeWire audio node matches \"%s\"; the ring stays empty. "
               "Check the node name with `pw-dump | grep -A20 media.class`, or pass it "
               "explicitly with --viz-source.\n",
               target.c_str());
}

void AudioTap::Impl::resolveAndConnect() {
  std::string target;
  bool active = false;
  {
    std::lock_guard lk(wantMu);
    target = wantTarget;
    active = wantActive;
  }

  if (!active || target.empty()) {
    disconnect();
    return;
  }

  NodeInfo node;
  uint32_t id = 0;
  if (!findTarget(target, node, id)) {
    disconnect();
    reportMissing(target);
    return;
  }
  {
    std::lock_guard lk(statusMu);
    reportedMissing = false;
  }

  // object.serial is unambiguous and node.name is the fallback for graphs that publish no serial.
  // Reconnecting to the same live node is skipped; the node going away is detected through the
  // registry, which is what makes a player restart recover on its own.
  const std::string targetId = !node.serial.empty() ? node.serial : node.name;
  if (liveStream.load(std::memory_order_acquire) && targetId == resolved && nodes.count(id)) return;
  disconnect();

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
      PW_KEY_MEDIA_CLASS, "Stream/Input/Audio", PW_KEY_NODE_NAME, "pw-mpris-visualcard-audio",
      PW_KEY_APP_NAME, "pw-mpris-visualcard", PW_KEY_TARGET_OBJECT, targetId.c_str(),
      PW_KEY_STREAM_IS_LIVE, "true", PW_KEY_NODE_AUTOCONNECT, "true", nullptr);

  static const pw_stream_events kStreamEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = &Impl::onStreamState;
    e.param_changed = &Impl::onStreamParam;
    e.process = &Impl::onStreamProcess;
    return e;
  }();
  pw_stream* st = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "pw-mpris-visualcard-audio",
                                       props, &kStreamEvents, this);
  if (!st) {
    std::fprintf(stderr, "[audio] pw_stream_new_simple failed\n");
    return;
  }

  // Only f32 is offered, and the rate is a range rather than a value: the graph already has one,
  // and asking for a rate it is not running leaves the stream unconnected. (A bitmask built with
  // SPA_POD_CHOICE_FLAGS_Int here looks reasonable and is silently fatal -- see internals.md.)
  uint8_t buf[512];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
  spa_pod_frame f{};
  spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio), 0);
  spa_pod_builder_add(&b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
  spa_pod_builder_add(&b, SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_F32), 0);
  spa_pod_builder_add(&b, SPA_FORMAT_AUDIO_rate, SPA_POD_CHOICE_RANGE_Int(48000, 8000, 192000), 0);
  spa_pod_builder_add(&b, SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(2, 1, 8), 0);
  const spa_pod* params[1] = {static_cast<spa_pod*>(spa_pod_builder_pop(&b, &f))};

  const int rc = pw_stream_connect(
      st, PW_DIRECTION_INPUT, PW_ID_ANY, static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS),
      params, 1);

  // An input stream is handed preallocated buffers, but it still has to declare what it needs.
  // Without this the size fields stay at zero and every chunk reads as empty.
  uint8_t bbuf[512];
  spa_pod_builder pb = SPA_POD_BUILDER_INIT(bbuf, sizeof bbuf);
  spa_pod_frame pf{};
  spa_pod_builder_push_object(&pb, &pf, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
  spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 4, 32), 0);
  spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), 0);
  spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(8192, 1024, 1 << 20), 0);
  spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(0, 4, 4096), 0);
  spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_dataType,
                      SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd), 0);
  const spa_pod* bparams[1] = {static_cast<spa_pod*>(spa_pod_builder_pop(&pb, &pf))};
  pw_stream_update_params(st, bparams, 1);

  if (rc < 0) {
    std::fprintf(stderr, "[audio] pw_stream_connect to %s failed (%d)\n", targetId.c_str(), rc);
    pw_stream_destroy(st);
    return;
  }

  stream = st;
  liveStream.store(st, std::memory_order_release);
  resolved = targetId;
  {
    std::lock_guard lk(statusMu);
    statusNode = node.name;
  }
  std::fprintf(stderr, "[audio] capturing %s (%s)\n", node.name.c_str(), node.mediaClass.c_str());
}

void AudioTap::Impl::onStreamState(void* data, pw_stream_state, pw_stream_state st, const char* err) {
  Impl& s = *static_cast<Impl*>(data);
  s.streaming.store(st == PW_STREAM_STATE_STREAMING, std::memory_order_relaxed);
  if (st == PW_STREAM_STATE_ERROR && err)
    std::fprintf(stderr, "[audio] stream error: %s\n", err);
}

void AudioTap::Impl::onStreamParam(void* data, uint32_t id, const spa_pod* param) {
  if (id != SPA_PARAM_Format || !param) return;
  uint32_t mediaType = 0, mediaSubtype = 0;
  if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
  if (mediaType != SPA_MEDIA_TYPE_audio || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;
  spa_audio_info_raw info{};
  if (spa_format_audio_raw_parse(param, &info) < 0) return;
  auto& s = *static_cast<Impl*>(data);
  s.rate.store(static_cast<int>(info.rate), std::memory_order_relaxed);
  s.channels.store(static_cast<int>(info.channels), std::memory_order_relaxed);
  if (s.verbose)
    std::fprintf(stderr, "[audio] negotiated %u Hz, %u ch, format %d\n", info.rate, info.channels,
                 static_cast<int>(info.format));
}

void AudioTap::Impl::onStreamProcess(void* data) {
  Impl& s = *static_cast<Impl*>(data);
  pw_stream* st = s.liveStream.load(std::memory_order_acquire);
  if (!st) return;
  pw_buffer* b = pw_stream_dequeue_buffer(st);
  if (!b) return;

  const int ch = s.channels.load(std::memory_order_relaxed);
  if (ch >= 1 && b->buffer && b->buffer->n_datas >= 1) {
    const uint32_t planes = b->buffer->n_datas;
    // Interleaved arrives as one plane whose stride is a whole frame; planar arrives as one plane
    // per channel. Only interleaved f32 is requested, but the graph may pick planar, so both are
    // read rather than assumed.
    const bool planar = ch > 1 && planes == static_cast<uint32_t>(ch);
    uint64_t w = s.written.load(std::memory_order_relaxed);
    const uint32_t loops = planar ? planes : 1u;
    for (uint32_t di = 0; di < loops; ++di) {
      spa_data& d = b->buffer->datas[di];
      if (!d.data || !d.chunk ||
          d.chunk->stride < static_cast<int32_t>(sizeof(float)) || d.chunk->size == 0)
        continue;
      const uint32_t step = d.chunk->stride / static_cast<uint32_t>(sizeof(float));
      const float* p = reinterpret_cast<const float*>(d.data) + d.chunk->offset / sizeof(float);
      const uint32_t frames = d.chunk->size / d.chunk->stride;
      for (uint32_t i = 0; i < frames; ++i) {
        float v = 0.0f;
        if (planar) {
          v = p[i * step];
        } else {
          for (int c = 0; c < ch; ++c) v += p[i * step + static_cast<uint32_t>(c)];
          v /= static_cast<float>(ch);
        }
        s.ring[w & s.ringMask] = v;
        ++w;
      }
    }
    s.written.store(w, std::memory_order_release);
  }

  pw_stream_queue_buffer(st, b);
}

void AudioTap::Impl::onTimer(void* data, uint64_t) {
  static_cast<Impl*>(data)->resolveAndConnect();
}

/* ------------------------------------------------------------------ */
/* Public surface                                                      */
/* ------------------------------------------------------------------ */

AudioTap::AudioTap(int ringSeconds, bool verbose) : impl_(std::make_unique<Impl>()) {
  impl_->verbose = verbose;
  size_t n = 1024;
  while (n < static_cast<size_t>(std::max(1, ringSeconds)) * 48000u) n <<= 1;
  impl_->ring.assign(n, 0.0f);
  impl_->ringMask = n - 1;
}

AudioTap::~AudioTap() { stop(); }

void AudioTap::start() {
  Impl& s = *impl_;
  if (s.started) return;

  // pw_init/pw_deinit are process-global. pwvideo.cpp reference-counts them properly; this side
  // does not, which is fine only because a process calls this at most once (dump() returns before
  // run() ever reaches here). Adding a pw_deinit needs the same refcount, not a bare pairing.
  pw_init(nullptr, nullptr);

  s.loop = pw_thread_loop_new("pw-mpris-visualcard-audio", nullptr);
  if (!s.loop) throw std::runtime_error("pw_thread_loop_new failed");
  s.context = pw_context_new(pw_thread_loop_get_loop(s.loop), nullptr, 0);
  if (!s.context) throw std::runtime_error("pw_context_new failed");
  s.core = pw_context_connect(s.context, nullptr, 0);
  if (!s.core) throw std::runtime_error("cannot connect to PipeWire (is the daemon running?)");

  pw_thread_loop_start(s.loop);
  s.started = true;

  // Everything from here runs under the loop lock, i.e. on the loop thread.
  pw_thread_loop_lock(s.loop);
  s.registry = pw_core_get_registry(s.core, PW_VERSION_REGISTRY, 0);
  static const pw_registry_events kRegistryEvents = [] {
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = &Impl::onGlobal;
    e.global_remove = &Impl::onGlobalRemove;
    return e;
  }();
  if (s.registry) pw_registry_add_listener(s.registry, &s.registryHook, &kRegistryEvents, &s);

  // The registry keeps the node list fresh on its own events; this timer is the only thing that
  // polls, and it does nothing when the target has not changed.
  s.timer = pw_loop_add_timer(pw_thread_loop_get_loop(s.loop), &Impl::onTimer, &s);
  timespec iv{};
  iv.tv_nsec = static_cast<long>(kResolveIntervalMs) * 1000000L;
  pw_loop_update_timer(pw_thread_loop_get_loop(s.loop), s.timer, nullptr, &iv, false);
  pw_thread_loop_unlock(s.loop);
}

void AudioTap::stop() {
  Impl& s = *impl_;
  if (!s.started) return;

  pw_thread_loop_lock(s.loop);
  s.liveStream.store(nullptr, std::memory_order_release);
  if (s.stream) {
    pw_stream_destroy(s.stream);
    s.stream = nullptr;
  }
  if (s.registry) {
    pw_registry_destroy(s.registry, 0);
    s.registry = nullptr;
  }
  if (s.core) {
    pw_core_disconnect(s.core);
    s.core = nullptr;
  }
  if (s.context) {
    pw_context_destroy(s.context);
    s.context = nullptr;
  }
  pw_thread_loop_unlock(s.loop);

  pw_thread_loop_stop(s.loop);
  pw_thread_loop_destroy(s.loop);
  s.loop = nullptr;
  s.timer = nullptr;
  s.started = false;
}

void AudioTap::setTarget(const std::string& name) {
  std::lock_guard lk(impl_->wantMu);
  impl_->wantTarget = name;
}

void AudioTap::setActive(bool active) {
  std::lock_guard lk(impl_->wantMu);
  impl_->wantActive = active;
}

int AudioTap::rate() const { return impl_->rate.load(std::memory_order_relaxed); }

size_t AudioTap::read(float* out, size_t n) {
  Impl& s = *impl_;
  const uint64_t w = s.written.load(std::memory_order_acquire);
  size_t avail = static_cast<size_t>(w - std::min<uint64_t>(s.readPos, w));
  avail = std::min(avail, s.ring.size());
  const size_t take = std::min(avail, n);
  // KNOWN BUG: this rewinds to the start of the window just returned instead of stepping past
  // its end, so readPos only ever moves when the caller asks for less than is available. Since
  // App asks for vizBuf_.size() (16384) and ~1600 arrive per frame, take == avail every frame and
  // readPos never advances: each call re-delivers the whole history, and Analyser::feed() then
  // runs kMaxTransformsPerFeed transforms instead of one (~8x the FFT cost, ~0.20 vs 0.026
  // ms/frame). The displayed spectrum is still correct -- the last window ends at the newest
  // sample -- so this is wasted work, not a wrong picture. The fix is `readPos = w` (verified).
  // Tracked as a separate change; see the PR description.
  s.readPos = w - take;  // a caller that falls behind loses the oldest samples, never buffers up
  for (size_t i = 0; i < take; ++i)
    out[i] = s.ring[static_cast<size_t>((s.readPos + i) & s.ringMask)];
  return take;
}

AudioTap::Status AudioTap::status() const {
  Status st;
  {
    std::lock_guard lk(impl_->wantMu);
    st.active = impl_->wantActive;
    st.target = impl_->wantTarget;
  }
  st.connected = impl_->liveStream.load(std::memory_order_acquire) != nullptr;
  st.rate = impl_->rate.load(std::memory_order_relaxed);
  st.channels = impl_->channels.load(std::memory_order_relaxed);
  std::lock_guard lk(impl_->statusMu);
  st.node = impl_->statusNode;
  return st;
}

}  // namespace oms
