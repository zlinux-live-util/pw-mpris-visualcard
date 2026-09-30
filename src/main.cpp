// pw-mpris-visualcard native - single process: MPRIS -> cairo rendering -> PipeWire video node
// Usage: see README.md (English, default) or README.zh-CN.md; --help prints a summary.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cairo/cairo.h>

#include "analyser.hpp"
#include "assetcache.hpp"
#include "audio.hpp"
#include "card.hpp"
#include "cairo_util.hpp"
#include "mpris.hpp"
#include "pwvideo.hpp"
#include "types.hpp"

namespace {

using namespace oms;

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr double kPiDemo = 3.14159265358979323846;

/** FFT window for the spectrum, in samples. 2048 at 48 kHz is a 42.7 ms window with 23.4 Hz bins,
 *  which is what the log-spaced low bands in analyser.cpp are built around; the cost is flat in
 *  the window size because the hop is a quarter of it. */
constexpr int kVizFftSize = 2048;

void usage() {
  std::printf(
      "pw-mpris-visualcard (native)\n"
      "\n"
      "  --size WxH      Output size. 540 = 540x540; 360x540 = portrait; default 360x360\n"
      "                  Layout scales by height; the width sets the side margins\n"
      "  --fps N         Frame-rate ceiling, default 30 (a consumer may go lower, never higher)\n"
      "  --bg MODE       Card background: none (default, fully transparent) | solid | #rrggbb\n"
      "  --progress 0|1  Progress ring, default 1\n"
      "  --time 0|1      Show time, default 0\n"
      "  --album 0|1     Show album, default 0\n"
      "  --lyrics N      Lyric lines, default 0 (off)\n"
      "  --spin SEC      Seconds per full cover rotation, 0 = no rotation, default 24\n"
      "  --viz 0|1       Radial spectrum ring around the cover, default 0 (off)\n"
      "  --viz-bars N    Bars in the ring, 8..256, default 72\n"
      "  --viz-source S  Capture this PipeWire audio node / app instead of the MPRIS player\n"
      "  --viz-fx 0|1    Ring post-processing: cava-style bar motion, band shaping, auto-gain.\n"
      "                  default 1 (on) -- 0 shows the measured spectrum unchanged\n"
      "  --viz-gain DB   Expansion in dB before everything else, default 0\n"
      "  --viz-gravity N 0..100, how heavy the bars are: slower fall, more momentum.\n"
      "                  Cava's noise_reduction; 10 or below turns it off. Default 77\n"
      "  --viz-shape N   0..100, blend towards a blur along the band axis, default 50.\n"
      "                  At 100 a lone tall band becomes a three-band mound\n"
      "  --viz-norm MS   Sliding-window auto-gain length in ms, default 2000, 0 = off.\n"
      "                  Keeps the ring filling its band across quiet and loud passages\n"
      "  --idle last|hide  Keep the last track after playback stops, default hide\n"
      "  --node NAME     PipeWire node name, default pw-mpris-visualcard\n"
      "  --desc TEXT     Node description, default Music Card\n"
      "  --dump FILE     Render one sample to PNG and exit (for layout tuning)\n"
      "  --demo          Use fake data, no D-Bus connection (for layout tuning)\n"
      "  --verbose, -v   Log negotiation and frame pushes\n"
      "  --help\n");
}

/** Returns true when the program should run, false when it already did its job (--help) and
 *  should exit successfully. Anything wrong with the arguments throws. */
bool parseArgs(int argc, char** argv, Config& cfg, std::string& dump, bool& demo,
               bool& verbose) {
  auto next = [&](int& i) -> std::string {
    if (i + 1 >= argc) throw std::runtime_error("missing argument value");
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage();
      return false;
    } else if (a == "--size") {
      const std::string v = next(i);
      const size_t x = v.find_first_of("xX*");
      if (x == std::string::npos) {
        cfg.width = cfg.height = std::max(64, std::stoi(v));
      } else {
        cfg.width = std::max(64, std::stoi(v.substr(0, x)));
        cfg.height = std::max(64, std::stoi(v.substr(x + 1)));
      }
    } else if (a == "--fps") {
      cfg.fps = std::max(1, std::stoi(next(i)));
    } else if (a == "--bg") {
      cfg.bg = next(i);
    } else if (a == "--progress") {
      cfg.showProgress = next(i) != "0";
    } else if (a == "--time") {
      cfg.showTime = next(i) != "0";
    } else if (a == "--album") {
      cfg.showAlbum = next(i) != "0";
    } else if (a == "--lyrics") {
      cfg.lyricLines = std::max(0, std::stoi(next(i)));
    } else if (a == "--spin") {
      cfg.spinSeconds = std::max(0.0, std::stod(next(i)));
    } else if (a == "--viz") {
      cfg.showViz = next(i) != "0";
    } else if (a == "--viz-bars") {
      cfg.vizBars = std::clamp(std::stoi(next(i)), 8, Card::kMaxVizBars);
    } else if (a == "--viz-source") {
      cfg.vizSource = next(i);
    } else if (a == "--viz-fx") {
      cfg.vizFx = next(i) != "0";
    } else if (a == "--viz-gain") {
      cfg.vizGainDb = std::clamp(std::stod(next(i)), -24.0, 24.0);
    } else if (a == "--viz-gravity") {
      cfg.vizGravity = std::clamp(std::stod(next(i)) / 100.0, 0.0, 1.0);
    } else if (a == "--viz-shape") {
      cfg.vizShape = std::clamp(std::stod(next(i)) / 100.0, 0.0, 1.0);
    } else if (a == "--viz-norm") {
      cfg.vizNormMs = std::clamp(std::stod(next(i)), 0.0, 60000.0);
    } else if (a == "--idle") {
      cfg.idleLast = next(i) == "last";
    } else if (a == "--node") {
      cfg.nodeName = next(i);
    } else if (a == "--desc") {
      cfg.nodeDescription = next(i);
    } else if (a == "--dump") {
      dump = next(i);
    } else if (a == "--demo") {
      demo = true;
    } else if (a == "--verbose" || a == "-v") {
      verbose = true;
    } else {
      // Thrown rather than "print and stop": returning here used to exit 0, which under
      // Restart=always turns a typo into a silent restart loop that reports SUCCESS.
      throw std::runtime_error("unknown argument: " + a);
    }
  }
  return true;
}

/* ---------------- Demo data (--demo) ---------------- */

struct DemoTrack {
  const char* title;
  const char* artist;
  const char* album;
  int duration;
};

const DemoTrack kDemo[] = {
    {"人造卫星", "三省, 星尘", "人造卫星", 192340},
    {"Night Drive", "Mirage Tape", "Neon Hours", 245000},
    {"沉溺于一场没有你的雨", "V.A.", "雨声收集者", 173000},
};

std::vector<Lyric> demoLyrics() {
  const char* lines[] = {"我是人造卫星", "绕着你旋转不停",
                         "穿过大气层的余温", "只为看你一眼"};
  std::vector<Lyric> out;
  for (int i = 0; i < 4; ++i) out.push_back(Lyric{2000 + i * 3000, lines[i]});
  return out;
}

NowPlaying demoState(int64_t now) {
  constexpr int64_t kStep = 10000;  // Switch tracks every 10 seconds, to inspect the layout quickly
  const int idx = static_cast<int>((now / kStep) % 3);
  const DemoTrack& d = kDemo[idx];
  NowPlaying np;
  np.status = "Playing";
  np.player = "demo";
  np.track.title = d.title;
  np.track.artist = d.artist;
  np.track.album = d.album;
  np.track.duration = d.duration;
  np.track.id = "/demo/" + std::to_string(idx);
  np.track.lyrics = demoLyrics();
  np.position = (now % kStep) * 8;  // Pretend to fast-forward
  np.rate = 1.0;
  np.sampledAt = now;
  return np;
}

/* ---------------- Demo spectrum (--demo --viz) ---------------- */

/** Stand-in spectrum for --demo, so the ring can be laid out and tuned with no player and no
 *  audio node. Not an analysis: a falling tilt with three drifting formants and a slow beat. */
std::vector<float> demoSpectrum(int n, int64_t nowMs) {
  n = std::max(1, n);
  std::vector<float> out(static_cast<size_t>(n));
  const double t = static_cast<double>(nowMs % 9000) / 9000.0;
  const double beat = 0.70 + 0.30 * std::sin(t * 2 * kPiDemo);
  for (int i = 0; i < n; ++i) {
    const double x = static_cast<double>(i) / n;
    double v = 0.66 - 0.50 * x;
    v += 0.30 * std::exp(-std::pow((x - 0.06 - 0.03 * std::sin(t * 2 * kPiDemo)) / 0.045, 2.0));
    v += 0.24 * std::exp(-std::pow((x - 0.33 - 0.06 * std::cos(t * 2 * kPiDemo)) / 0.07, 2.0));
    v += 0.16 * std::exp(-std::pow((x - 0.70 - 0.05 * std::sin(t * 3 * kPiDemo)) / 0.09, 2.0));
    v *= beat;
    out[static_cast<size_t>(i)] = static_cast<float>(std::clamp(v, 0.0, 1.0));
  }
  return out;
}

/* ---------------- Application ---------------- */

class App {
 public:
  App(Config cfg, bool demo, bool verbose)
      : cfg_(std::move(cfg)),
        demo_(demo),
        verbose_(verbose),
        card_(cfg_),
        // capacity 3, the same user agent and timeouts the previous cover loader used
        loader_(pwvideo::AssetCache::Options{3, "Mozilla/5.0 (pw-mpris-visualcard native)"}),
        frame_(cfg_.width, cfg_.height) {
    // The ring only costs anything when asked for, and --demo never touches PipeWire audio: it
    // feeds the stand-in spectrum instead.
    if (cfg_.showViz) {
      analyser_ = std::make_unique<Analyser>(cfg_.vizBars, kVizFftSize);
      vizBuf_.resize(16384);  // ~340 ms at 48 kHz: far more than one frame ever needs
      if (!demo) tap_ = std::make_unique<AudioTap>(2, verbose);
    }
  }

  int run(const std::string& dumpPath) {
    if (!demo_) {
      mpris_.start();
      // Wait for the first sample, so the opening is not a blank frame
      for (int i = 0; i < 30 && !mpris_.healthy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!dumpPath.empty()) return dump(dumpPath);

    if (tap_) tap_->start();

    artThread_ = std::thread([this] { artLoop(); });

    pwvideo::Options opt;
    opt.width = cfg_.width;
    opt.height = cfg_.height;
    opt.fpsCap = cfg_.fps;
    opt.nodeName = cfg_.nodeName;
    opt.nodeDescription = cfg_.nodeDescription;
    opt.appName = "pw-mpris-visualcard";
    opt.verbose = verbose_;
    // Capture only while a consumer is actually pulling frames, so a card nobody is watching
    // costs nothing on the audio side either.
    opt.onStreaming = [this](bool streaming) {
      if (tap_) tap_->setActive(streaming);
    };
    pwvideo::VideoNode video(opt, [this](uint8_t* dst, int stride, int w, int h) {
      renderInto(dst, stride, w, h);
    });
    video.start();
    std::printf(
        "pw-mpris-visualcard (native) started\n"
        "  PipeWire node: %s   [select it as a \"PipeWire Video\" source in OBS]\n"
        "  Size: %dx%d @ %d fps\n",
        cfg_.nodeName.c_str(), cfg_.width, cfg_.height, cfg_.fps);
    std::fflush(stdout);

    video.run();  // Blocks until SIGINT/SIGTERM
    stopping_.store(true);
    if (artThread_.joinable()) artThread_.join();
    // Released before the node goes away, so PipeWire's process-global init stays paired.
    if (tap_) tap_->stop();
    return 0;
  }

 private:
  NowPlaying current() {
    return demo_ ? demoState(steadyMs()) : mpris_.snapshot();
  }

  /** Render one frame into dst (BGRA, premultiplied alpha) */
  void renderInto(uint8_t* dst, int dstStride, int w, int h) {
    pwvideo::SurfacePtr cover;
    {
      std::lock_guard lk(artMu_);
      cover = art_;
    }
    const int64_t now = steadyMs();
    // One snapshot for the frame: the spectrum and the card then agree on what is playing.
    const NowPlaying np = current();
    if (analyser_) updateViz(np, now);

    const auto t0 = std::chrono::steady_clock::now();
    card_.render(frame_.cr(), np, cover.get(), now);
    const auto t1 = std::chrono::steady_clock::now();

    const auto t2 = std::chrono::steady_clock::now();
    frame_.blitTo(dst, dstStride, w, h);
    const auto t3 = std::chrono::steady_clock::now();
    statRender_ += std::chrono::duration<double, std::milli>(t1 - t0).count();
    statCopy_ += std::chrono::duration<double, std::milli>(t3 - t2).count();
    if (++statN_ == 120) {
      if (verbose_)
        std::fprintf(stderr, "[stat] render %.3f ms/frame   copy %.3f ms/frame\n",
                     statRender_ / statN_, statCopy_ / statN_);
      statRender_ = statCopy_ = 0;
      statN_ = 0;
    }
  }

  /** Reads the newest samples, folds them into the bands and hands them to the card. Runs once per
   *  rendered frame, so the spectrum is sampled at exactly the rate it is drawn and needs no thread
   *  of its own -- and a card with no consumer attached analyses nothing at all. */
  void updateViz(const NowPlaying& np, int64_t now) {
    if (demo_) {
      const std::vector<float> s = demoSpectrum(cfg_.vizBars, now);
      card_.setSpectrum(s.data(), static_cast<int>(s.size()));
      return;
    }
    if (!tap_) return;

    // --viz-source pins the target; otherwise follow whatever MPRIS says is playing.
    const std::string want = cfg_.vizSource.empty() ? np.player : cfg_.vizSource;
    if (want != vizTarget_) {
      vizTarget_ = want;
      tap_->setTarget(want);
    }
    // Lock-free, so asking once per frame costs nothing. The band edges only move when the graph
    // renegotiates its rate, which is rare.
    const int rate = tap_->rate();
    if (rate != vizRate_) {
      vizRate_ = rate;
      analyser_->setRate(rate);
    }
    const size_t n = tap_->read(vizBuf_.data(), vizBuf_.size());
    if (n) {
      vizSamples_ += n;
      analyser_->feed(vizBuf_.data(), n);
    }
    card_.setSpectrum(analyser_->levels(), analyser_->bandCount());
  }

  /** Runs the capture path until samples arrive, so a single --dump frame shows the real spectrum
   *  rather than an empty ring. Bounded: it gives up instead of hanging when nothing is playing. */
  void warmUpViz(const NowPlaying& np, int budgetMs) {
    int64_t until = steadyMs() + budgetMs;
    uint64_t seen = 0;
    int quiet = 0;
    while (steadyMs() < until) {
      updateViz(np, steadyMs());
      if (vizSamples_ == seen) {
        ++quiet;
      } else {
        quiet = 0;
        seen = vizSamples_;
      }
      if (seen > 0 && quiet >= 3) break;  // a few frames with nothing new is close enough
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }

  /** Background cover fetch: fetched only when the URL changes; a failed fetch is retried after
   *  30 seconds */
  void artLoop() {
    std::string want;
    int64_t retryAt = 0;
    while (!stopping_.load()) {
      const std::string url = current().track.artUrl;
      if (url != want) {
        want = url;
        retryAt = 0;
        std::lock_guard lk(artMu_);
        art_.reset();
      } else if (retryAt != 0 && steadyMs() >= retryAt) {
        retryAt = 0;
      } else if (retryAt != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }

      if (want.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }

      pwvideo::SurfacePtr s = loader_.get(want, cfg_.height);
      if (s) {
        std::lock_guard lk(artMu_);
        art_ = std::move(s);
      } else {
        retryAt = steadyMs() + 30000;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  int dump(const std::string& path) {
    const int FW = cfg_.width, FH = cfg_.height;
    pwvideo::CairoFrame frame(FW, FH);
    NowPlaying np = current();
    pwvideo::SurfacePtr cover;
    if (!np.track.artUrl.empty()) cover = loader_.get(np.track.artUrl, FH);
    const int64_t now = steadyMs() + 1000;
    if (analyser_) {
      if (demo_) {
        const std::vector<float> s = demoSpectrum(cfg_.vizBars, now);
        card_.setSpectrum(s.data(), static_cast<int>(s.size()));
      } else if (tap_) {
        tap_->start();
        tap_->setActive(true);
        tap_->setTarget(cfg_.vizSource.empty() ? np.player : cfg_.vizSource);
        warmUpViz(np, 1500);
        tap_->stop();
      }
    }

    // The ring's temporal stages -- the motion model and the auto-gain -- need history, so a
    // single frame cannot show them: on the first frame a falling bar is still anchored to its
    // peak. Render a short run first and keep the last, which puts the dump in the same state a
    // viewer would see. The run ends exactly at `now`, so with --viz-fx off it is still the single
    // frame it always was.
    //
    // SpectrumFx::apply() rewrites the buffer it is handed, so every frame of the run needs the
    // measurement written back first; the analyser's levels are a stable snapshot once the tap is
    // stopped. --demo re-derives its spectrum instead, which also keeps it in step with the frame
    // timestamp.
    const bool fxOn = cfg_.showViz && cfg_.vizFx;
    std::vector<float> still;
    if (fxOn && !demo_ && analyser_) {
      still.assign(analyser_->levels(), analyser_->levels() + analyser_->bandCount());
    }
    const int warm = fxOn ? 12 : 1;
    int64_t t = now - (warm - 1) * 33;
    for (int i = 0; i < warm; ++i) {
      if (fxOn) {
        if (demo_) {
          const std::vector<float> s = demoSpectrum(cfg_.vizBars, t);
          card_.setSpectrum(s.data(), static_cast<int>(s.size()));
        } else if (i && !still.empty()) {
          card_.setSpectrum(still.data(), static_cast<int>(still.size()));
        }
      }
      card_.render(frame.cr(), np, cover.get(), t);
      t += 33;
    }
    if (!frame.writePng(path)) {
      std::fprintf(stderr, "PNG write failed: %s (%s)\n", path.c_str(),
                   cairo_status_to_string(cairo_surface_status(frame.surface())));
      return 1;
    }
    std::printf("Wrote %s (%dx%d)\n", path.c_str(), FW, FH);
    return 0;
  }

  Config cfg_;
  bool demo_;
  bool verbose_;
  Card card_;
  MprisClient mpris_;
  pwvideo::AssetCache loader_;

  // Radial spectrum (--viz). Null unless the ring is on.
  std::unique_ptr<AudioTap> tap_;
  std::unique_ptr<Analyser> analyser_;
  std::vector<float> vizBuf_;
  std::string vizTarget_;
  int vizRate_ = 0;
  uint64_t vizSamples_ = 0;

  std::mutex artMu_;
  pwvideo::SurfacePtr art_;

  pwvideo::CairoFrame frame_;

  std::atomic<bool> stopping_{false};
  std::thread artThread_;
  double statRender_ = 0, statCopy_ = 0;
  int statN_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  std::string dumpPath;
  bool demo = false;
  bool verbose = false;
  try {
    if (!parseArgs(argc, argv, cfg, dumpPath, demo, verbose)) return 0;
  } catch (const std::exception& e) {
    // The message names the offending flag; the usage text then shows what the current build
    // actually accepts, which matters when an argument was renamed or removed.
    std::fprintf(stderr, "argument error: %s\n\n", e.what());
    usage();
    return 2;
  }

  try {
    App app(cfg, demo, verbose);
    return app.run(dumpPath);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }
}
