// MPRIS client implementation (sdbus-c++ persistent connection)
#include "mpris.hpp"

#include <sdbus-c++/sdbus-c++.h>

// Compatibility layer for sdbus-c++ 1.x (e.g. Ubuntu 24.04 / Debian)
#if defined(SDBUS_CPP_MAJOR) && SDBUS_CPP_MAJOR < 2
namespace sdbus {
struct ServiceName : std::string {
  using std::string::string;
  ServiceName(const std::string& s) : std::string(s) {}
  ServiceName(std::string&& s) : std::string(std::move(s)) {}
};
}  // namespace sdbus
#endif

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <thread>

namespace oms {
namespace {

constexpr const char* kBusService = "org.freedesktop.DBus";
constexpr const char* kBusPath = "/org/freedesktop/DBus";
constexpr const char* kBusIface = "org.freedesktop.DBus";
constexpr const char* kMprisPrefix = "org.mpris.MediaPlayer2.";
constexpr const char* kMprisPath = "/org/mpris/MediaPlayer2";
constexpr const char* kPlayerIface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kPropsIface = "org.freedesktop.DBus.Properties";

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const char* kCreditWords[] = {
    "作词", "作曲", "编曲", "混音", "母带", "调音", "曲绘", "制作", "录音", "吉他",
    "贝斯", "和声", "监制", "出品", "发行", "企划", "统筹", "封面", "美术", "字幕",
    "翻译", "校对", "OP",   "SP",   "PV",   "Produced", "Composed", "Written",
    "Lyrics", "Mixed", "Mastered"};

bool isSpaceByte(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

size_t skipSpace(const std::string& s, size_t i) {
  while (i < s.size() && isSpaceByte(s[i])) ++i;
  return i;
}

/** UTF-8 code point count in s[0, byteEnd). Full validation is unnecessary; counting is enough. */
size_t codePoints(const std::string& s, size_t byteEnd) {
  size_t n = 0;
  for (size_t i = 0; i < byteEnd && i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++n;
  }
  return n;
}

bool isColonAt(const std::string& s, size_t i) {
  if (i >= s.size()) return false;
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c == ':') return true;
  return c == 0xEF && i + 2 < s.size() &&
         static_cast<unsigned char>(s[i + 1]) == 0xBC &&
         static_cast<unsigned char>(s[i + 2]) == 0x9A;  // U+FF1A ：
}

/** Production-credit lines such as "lyricist: xxx" within the first 20 seconds of a track */
bool isCredit(const std::string& text) {
  const size_t start = skipSpace(text, 0);
  const std::string s = text.substr(start);

  size_t colon = std::string::npos;
  for (size_t i = 0; i < s.size(); ++i) {
    if (isColonAt(s, i)) {
      colon = i;
      break;
    }
  }

  for (const char* kw : kCreditWords) {
    const size_t klen = std::char_traits<char>::length(kw);
    const size_t p = s.find(kw);
    if (p == std::string::npos) continue;
    if (colon != std::string::npos && p > colon) continue;
    if (codePoints(s, p) > 8) continue;
    size_t q = skipSpace(s, p + klen);
    if (q >= s.size()) continue;
    if (isColonAt(s, q) || s[q] == '/') return true;
  }
  return false;
}

/** "short label: value". Near the track head these are mostly production info, not lyrics. */
bool isShortLabel(const std::string& text) {
  const size_t start = skipSpace(text, 0);
  const std::string s = text.substr(start);
  for (size_t i = 0; i < s.size(); ++i) {
    if (!isColonAt(s, i)) continue;
    const size_t n = codePoints(s, i);
    if (n < 1 || n > 10) return false;
    size_t q = i + (s[i] == ':' ? 1 : 3);
    q = skipSpace(s, q);
    return q < s.size();  // only counts when non-whitespace follows the colon
  }
  return false;
}

std::string trim(const std::string& s) {
  const size_t b = skipSpace(s, 0);
  size_t e = s.size();
  while (e > b && isSpaceByte(s[e - 1])) --e;
  return s.substr(b, e - b);
}

template <class T>
std::optional<T> pick(const std::map<std::string, sdbus::Variant>& m,
                      const std::string& key) {
  const auto it = m.find(key);
  if (it == m.end()) return std::nullopt;
  try {
    return it->second.get<T>();
  } catch (...) {
    return std::nullopt;
  }
}

/** Duration fields such as mpris:length: most players give int64 microseconds, a few give uint64 */
std::optional<int64_t> pickInt(const std::map<std::string, sdbus::Variant>& m,
                               const std::string& key) {
  if (auto v = pick<int64_t>(m, key)) return v;
  if (auto v = pick<uint64_t>(m, key))
    return static_cast<int64_t>(*v);
  if (auto v = pick<uint32_t>(m, key))
    return static_cast<int64_t>(*v);
  if (auto v = pick<int32_t>(m, key))
    return static_cast<int64_t>(*v);
  return std::nullopt;
}

/** xesam:artist is specified as as, but some players give a single string in practice */
std::string pickArtists(const std::map<std::string, sdbus::Variant>& m) {
  if (auto v = pick<std::vector<std::string>>(m, "xesam:artist")) {
    std::string out;
    for (const auto& a : *v) {
      if (a.empty()) continue;
      if (!out.empty()) out += ", ";
      out += a;
    }
    return out;
  }
  if (auto v = pick<std::string>(m, "xesam:artist")) return *v;
  return {};
}

Track trackFromMetadata(const std::map<std::string, sdbus::Variant>& meta) {
  Track t;
  if (auto v = pick<sdbus::ObjectPath>(meta, "mpris:trackid"))
    t.id = static_cast<const std::string&>(*v);
  else if (auto v2 = pick<std::string>(meta, "mpris:trackid"))
    t.id = *v2;
  t.title = pick<std::string>(meta, "xesam:title").value_or("");
  t.artist = pickArtists(meta);
  t.album = pick<std::string>(meta, "xesam:album").value_or("");
  t.artUrl = pick<std::string>(meta, "mpris:artUrl").value_or("");
  if (auto len = pickInt(meta, "mpris:length"))
    t.duration = static_cast<int>(std::max<int64_t>(0, *len / 1000));
  if (auto lrc = pick<std::string>(meta, "xesam:asText")) t.lyrics = parseLrc(*lrc);
  return t;
}

}  // namespace

/* ------------------------------------------------------------------ */
/* LRC                                                                 */
/* ------------------------------------------------------------------ */

std::vector<Lyric> parseLrc(const std::string& raw) {
  std::vector<Lyric> out;
  if (trim(raw).empty()) return out;

  // Timestamps are pure ASCII, so byte semantics == character semantics; std::regex is safe
  static const std::regex stamp(R"(\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\])");
  static const std::regex anyTag(R"(\[[^\]]*\])");

  size_t pos = 0;
  while (pos <= raw.size()) {
    size_t nl = raw.find('\n', pos);
    if (nl == std::string::npos) nl = raw.size();
    std::string line = raw.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    std::string text = trim(std::regex_replace(line, anyTag, ""));
    const bool credit = isCredit(text);

    auto begin = std::sregex_iterator(line.begin(), line.end(), stamp);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      const std::smatch& m = *it;
      const int mm = std::stoi(m[1].str());
      const int ss = std::stoi(m[2].str());
      int frac = 0;
      if (m[3].matched) {
        std::string f = m[3].str();
        while (f.size() < 3) f.push_back('0');
        frac = std::stoi(f.substr(0, 3));
      }
      const int t = mm * 60000 + ss * 1000 + frac;
      if (credit) continue;
      if (t < 20000 && isShortLabel(text)) continue;
      out.push_back(Lyric{t, text});
    }
  }

  if (out.size() < 2) return {};
  std::stable_sort(out.begin(), out.end(),
                   [](const Lyric& a, const Lyric& b) { return a.t < b.t; });
  std::vector<Lyric> dedup;
  dedup.reserve(out.size());
  for (const auto& l : out) {
    if (!dedup.empty() && dedup.back().t == l.t && dedup.back().text == l.text) continue;
    dedup.push_back(l);
  }
  return dedup;
}

/* ------------------------------------------------------------------ */
/* Sampling                                                          */
/* ------------------------------------------------------------------ */

struct MprisClient::Impl {
  struct PlayerState {
    std::string status = "Stopped";
    Track track;
    int64_t position = 0;  // milliseconds
    double rate = 1.0;
    int64_t at = 0;
  };

  std::unique_ptr<sdbus::IConnection> conn;
  std::unique_ptr<sdbus::IProxy> bus;
  std::map<std::string, std::unique_ptr<sdbus::IProxy>> proxies;
  std::map<std::string, PlayerState> states;
  std::vector<std::string> services;
  std::string activeService;
  int64_t listedAt = 0;

  mutable std::mutex mu;
  NowPlaying current;
  Track lastTrack;
  bool connected = false;

  std::thread worker;
  std::atomic<bool> running{false};
  std::atomic<bool> healthy{false};

  sdbus::IProxy* proxyFor(const std::string& svc) {
    auto it = proxies.find(svc);
    if (it != proxies.end()) return it->second.get();
    auto p = sdbus::createProxy(*conn, sdbus::ServiceName{svc},
                                sdbus::ObjectPath{kMprisPath});
    sdbus::IProxy* raw = p.get();
    proxies.emplace(svc, std::move(p));
    return raw;
  }

  void refreshServices() {
    std::vector<std::string> names;
    bus->callMethod("ListNames")
        .onInterface(kBusIface)
        .withTimeout(std::chrono::seconds(3))
        .storeResultsTo(names);
    services.clear();
    for (const auto& n : names)
      if (n.rfind(kMprisPrefix, 0) == 0) services.push_back(n);

    for (auto it = proxies.begin(); it != proxies.end();) {
      if (std::find(services.begin(), services.end(), it->first) == services.end()) {
        states.erase(it->first);
        it = proxies.erase(it);
      } else {
        ++it;
      }
    }
    if (!activeService.empty() &&
        std::find(services.begin(), services.end(), activeService) == services.end()) {
      activeService.clear();
    }
  }

  void sampleOne(const std::string& svc, int64_t now) {
    PlayerState& st = states[svc];
    try {
      std::map<std::string, sdbus::Variant> props;
      proxyFor(svc)
          ->callMethod("GetAll")
          .onInterface(kPropsIface)
          .withArguments(kPlayerIface)
          .withTimeout(std::chrono::seconds(3))
          .storeResultsTo(props);
      st.status = pick<std::string>(props, "PlaybackStatus").value_or("Stopped");
      if (st.status == "Playing" || st.status == "Paused") {
        auto meta = pick<std::map<std::string, sdbus::Variant>>(props, "Metadata");
        if (meta) st.track = trackFromMetadata(*meta);
        if (auto p = pickInt(props, "Position")) st.position = std::max<int64_t>(0, *p / 1000);
        if (auto r = pick<double>(props, "Rate"); r && *r > 0) st.rate = *r;
      }
    } catch (...) {
      st.status = "Stopped";
    }
    st.at = now;
  }

  void pollOnce() {
    const int64_t now = steadyMs();

    if (now - listedAt >= 2000) {
      refreshServices();
      listedAt = now;
    }

    for (const auto& svc : services) {
      const bool isActive = (svc == activeService);
      const int64_t ttl = isActive ? 500 : 2000;
      const auto it = states.find(svc);
      if (it == states.end() || now - it->second.at >= ttl) sampleOne(svc, now);
    }

    // Ranking: Playing > Paused; within the same rank, keep the previously active player
    auto rank = [](const std::string& s) {
      if (s == "Playing") return 2;
      if (s == "Paused") return 1;
      return 0;
    };
    std::string best;
    int bestRank = 0;
    for (const auto& svc : services) {
      const auto it = states.find(svc);
      if (it == states.end()) continue;
      const int r = rank(it->second.status);
      if (r == 0) continue;
      if (r > bestRank || (r == bestRank && svc == activeService)) {
        best = svc;
        bestRank = r;
      }
    }

    NowPlaying np;
    np.sampledAt = now;
    if (best.empty()) {
      np.status = "Stopped";
      np.track = lastTrack;
    } else {
      activeService = best;
      const PlayerState& st = states[best];
      np.status = st.status;
      np.player = best.substr(std::char_traits<char>::length(kMprisPrefix));
      np.track = st.track;
      np.position = st.position;
      np.rate = st.rate;
      lastTrack = st.track;
    }

    std::lock_guard lock(mu);
    current = std::move(np);
  }

  void run() {
    while (running.load(std::memory_order_relaxed)) {
      try {
        pollOnce();
        healthy.store(true, std::memory_order_relaxed);
      } catch (...) {
        // Connection lost: mark unhealthy, keep the old state, retry on the next pass
        healthy.store(false, std::memory_order_relaxed);
      }
      for (int i = 0; i < 50 && running.load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void connect() {
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
      const char* xdg = std::getenv("XDG_RUNTIME_DIR");
      const std::string base =
          xdg ? std::string(xdg) : ("/run/user/" + std::to_string(::getuid()));
      const std::string addr = "unix:path=" + base + "/bus";
      setenv("DBUS_SESSION_BUS_ADDRESS", addr.c_str(), 0);
    }
    conn = sdbus::createSessionBusConnection();
    bus = sdbus::createProxy(*conn, sdbus::ServiceName{kBusService},
                             sdbus::ObjectPath{kBusPath});
  }
};

MprisClient::MprisClient() : impl_(std::make_unique<Impl>()) {}

MprisClient::~MprisClient() { stop(); }

void MprisClient::start() {
  impl_->connect();
  impl_->running.store(true);
  impl_->worker = std::thread([this] { impl_->run(); });
}

void MprisClient::stop() {
  if (!impl_) return;
  impl_->running.store(false);
  if (impl_->worker.joinable()) impl_->worker.join();
}

NowPlaying MprisClient::snapshot() const {
  std::lock_guard lock(impl_->mu);
  return impl_->current;
}

bool MprisClient::healthy() const { return impl_->healthy.load(); }

}  // namespace oms
