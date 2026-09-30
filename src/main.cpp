// pw-mpris-visualcard native - single process: MPRIS -> cairo rendering -> PipeWire video node
// Usage: see README.md (English, default) or README.zh-CN.md; --help prints a summary.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cairo/cairo.h>

#include "assetcache.hpp"
#include "card.hpp"
#include "cairo_util.hpp"
#include "fonts.hpp"
#include "mpris.hpp"
#include "pwvideo.hpp"
#include "types.hpp"

namespace {

using namespace oms;

constexpr const char* kDefaultFont = "sans-serif";

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void usage(std::FILE* out) {
  std::fprintf(
      out,
      "pw-mpris-visualcard (native)\n"
      "\n"
      "  --size WxH      Output size. 540 = 540x540; 360x540 = portrait; default 360x360\n"
      "                  Layout scales by height; the width sets the side margins\n"
      "  --fps N         Frame-rate ceiling, default 30 (a consumer may go lower, never higher)\n"
      "  --bg MODE       Card background: none (default, fully transparent) | solid | #rrggbb\n"
      "  --font NAME[,NAME...]  Font family for all card text; a comma list is a fallback\n"
      "                  chain, default sans-serif (the fontconfig default)\n"
      "  --font-file PATH      Register a font file (or a directory of them) with fontconfig\n"
      "                  at startup, so a downloaded .ttf/.otf works without installing it;\n"
      "                  repeatable. Without --font its own family name is used\n"
      "  --progress 0|1  Progress ring, default 1\n"
      "  --time 0|1      Show time, default 0\n"
      "  --album 0|1     Show album, default 0\n"
      "  --lyrics N      Lyric lines, default 0 (off)\n"
      "  --spin SEC      Seconds per full cover rotation, 0 = no rotation, default 24\n"
      "  --idle last|hide  Keep the last track after playback stops, default hide\n"
      "  --node NAME     PipeWire node name, default pw-mpris-visualcard\n"
      "  --desc TEXT     Node description, default Music Card\n"
      "  --dump FILE     Render one sample to PNG and exit (for layout tuning)\n"
      "  --demo          Use fake data, no D-Bus connection (for layout tuning)\n"
      "  --verbose, -v   Log negotiation and frame pushes\n"
      "  --help\n");
}

/** Argument parsing has three outcomes, not two: a bad flag is not the same as a request for help.
 *  Folding them into one bool made an unknown argument exit 0, so a typo was indistinguishable
 *  from success. */
enum class Args { Ok, Help, Error };

Args parseArgs(int argc, char** argv, Config& cfg, std::string& dump, bool& demo,
               bool& verbose) {
  auto next = [&](int& i) -> std::string {
    if (i + 1 >= argc) throw std::runtime_error("missing argument value");
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return Args::Help;
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
    } else if (a == "--font") {
      cfg.font = next(i);
    } else if (a == "--font-file") {
      cfg.fontFiles.push_back(next(i));
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
      // Both the message and the option list go to stderr: this is a failure, so a caller that
      // captures stdout (a pipe, a systemd log) still sees why the run died.
      std::fprintf(stderr, "unknown argument: %s\n\n", a.c_str());
      usage(stderr);
      return Args::Error;
    }
  }
  return Args::Ok;
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

/* ---------------- Font setup ---------------- */

/** Registers the --font-file files and settles on the family chain to render with.
 *
 *  Runs before anything pango-related is constructed: a PangoFcFontMap snapshots the family list
 *  fontconfig reported at construction time, so a file registered after the first context exists
 *  would never be seen. Card owns the only TextRenderer, and main calls this before App, which
 *  keeps that ordering true.
 *
 *  Never fails: a bad path or a family that is not installed is a warning plus a fallback, because
 *  a card that renders in the wrong font beats a card that does not start. */
void applyFontConfig(Config& cfg) {
  std::vector<std::string> fromFiles;
  for (const std::string& path : cfg.fontFiles) {
    std::string err;
    if (!registerFontFile(path, err)) {
      std::fprintf(stderr, "warning: %s\n", err.c_str());
      continue;
    }
    // A file used on its own should not require the user to also know its family name.
    const std::string fam = familyOfFontFile(path);
    if (!fam.empty() && std::find(fromFiles.begin(), fromFiles.end(), fam) == fromFiles.end())
      fromFiles.push_back(fam);
  }

  if (cfg.font.empty() && !fromFiles.empty()) {
    for (const std::string& f : fromFiles) {
      if (!cfg.font.empty()) cfg.font += ',';
      cfg.font += f;
    }
  }
  if (cfg.font.empty()) cfg.font = kDefaultFont;

  // Warn only for the entries before the first one that resolves: a chain is a legitimate way to
  // ask for "this font, else that one", and past the first hit pango does the falling back.
  bool any = false;
  for (const std::string& f : splitFontList(cfg.font)) {
    if (familyAvailable(f)) {
      any = true;
      break;
    }
    std::fprintf(stderr, "warning: font family not installed: %s\n", f.c_str());
  }
  if (!any) {
    std::fprintf(stderr, "warning: no usable family in \"%s\", falling back to %s\n",
                 cfg.font.c_str(), kDefaultFont);
    cfg.font = kDefaultFont;
  }
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
        frame_(cfg_.width, cfg_.height) {}

  int run(const std::string& dumpPath) {
    if (!demo_) {
      mpris_.start();
      // Wait for the first sample, so the opening is not a blank frame
      for (int i = 0; i < 30 && !mpris_.healthy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!dumpPath.empty()) return dump(dumpPath);

    artThread_ = std::thread([this] { artLoop(); });

    pwvideo::Options opt;
    opt.width = cfg_.width;
    opt.height = cfg_.height;
    opt.fpsCap = cfg_.fps;
    opt.nodeName = cfg_.nodeName;
    opt.nodeDescription = cfg_.nodeDescription;
    opt.appName = "pw-mpris-visualcard";
    opt.verbose = verbose_;
    pwvideo::VideoNode video(opt, [this](uint8_t* dst, int stride, int w, int h) {
      renderInto(dst, stride, w, h);
    });
    video.start();
    std::printf(
        "pw-mpris-visualcard (native) started\n"
        "  PipeWire node: %s   [select it as a \"PipeWire Video\" source in OBS]\n"
        "  Size: %dx%d @ %d fps\n"
        "  Font: %s\n",
        cfg_.nodeName.c_str(), cfg_.width, cfg_.height, cfg_.fps, cfg_.font.c_str());
    std::fflush(stdout);

    video.run();  // Blocks until SIGINT/SIGTERM
    stopping_.store(true);
    if (artThread_.joinable()) artThread_.join();
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
    const auto t0 = std::chrono::steady_clock::now();
    card_.render(frame_.cr(), current(), cover.get(), now);
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
    card_.render(frame.cr(), np, cover.get(), steadyMs() + 1000);
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
    switch (parseArgs(argc, argv, cfg, dumpPath, demo, verbose)) {
      case Args::Help: return 0;
      case Args::Error: return 2;  // same code as a malformed value, below
      case Args::Ok: break;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "argument error: %s\n", e.what());
    return 2;
  }

  try {
    applyFontConfig(cfg);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "font setup failed: %s\n", e.what());
    return 1;
  }

  try {
    App app(cfg, demo, verbose);
    return app.run(dumpPath);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }
}
