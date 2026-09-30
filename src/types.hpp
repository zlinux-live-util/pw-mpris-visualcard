#pragma once
// pw-mpris-visualcard / native - shared data types
#include <cstdint>
#include <string>
#include <vector>

namespace oms {

/** One LRC line: t = milliseconds. */
struct Lyric {
  int t = 0;
  std::string text;
};

struct Track {
  std::string id;
  std::string title;
  std::string artist;
  std::string album;
  std::string artUrl;
  int duration = 0;  // milliseconds
  std::vector<Lyric> lyrics;
};

/** One MPRIS sample. position is the position at the sampledAt instant, extrapolated by rate. */
struct NowPlaying {
  std::string status;  // "Playing" | "Paused" | "Stopped"
  std::string player;
  Track track;
  int64_t position = 0;   // milliseconds
  double rate = 1.0;
  int64_t sampledAt = 0;  // steady_clock milliseconds

  bool playing() const { return status == "Playing"; }
  bool paused() const { return status == "Paused"; }
};

/** Render and output configuration (populated entirely from CLI flags). */
struct Config {
  int width = 360;             // output width (px)
  int height = 360;            // output height (px). All proportional scaling is driven by height
  int fps = 30;                // frame push rate ceiling
  // Card background: none / transparent = fully transparent (default); solid / dark = opaque
  // dark background; a literal #rrggbb is also accepted. Transparent mode automatically
  // enlarges the cover, brightens secondary text and adds a drop shadow.
  std::string bg = "none";
  bool showProgress = true;    // progress ring
  bool showTime = false;       // time
  bool showAlbum = false;      // album name
  int lyricLines = 0;          // 0 = no lyrics; N = show N lines
  double spinSeconds = 24;     // seconds per full cover rotation, 0 = no rotation
  bool showViz = false;        // radial spectrum ring around the cover (--viz)
  int vizBars = 72;            // bars in the ring (--viz-bars)
  std::string vizSource;       // audio target override (--viz-source); empty = the MPRIS player
  // Display-side post-processing for the ring (--viz-fx), in the spirit of cava's [cava] section.
  // On by default: the chain is what makes the bars read as having weight instead of flickering
  // with every frame. --viz-fx 0 skips all of it and the ring shows the analyser's own levels.
  // This changes how levels are drawn, never the audio: see fx.hpp.
  bool vizFx = true;
  double vizGainDb = 0.0;      // --viz-gain, dB of expansion applied before everything else
  double vizGravity = 0.77;    // --viz-gravity: cava's noise_reduction, how heavy the bars are
  double vizShape = 0.5;       // --viz-shape, 0..1: blend towards a blur along the band axis
  double vizNormMs = 2000.0;   // --viz-norm, sliding-window auto-gain window, 0 = off
  bool idleLast = false;       // true = keep the last track after playback stops
  std::string nodeName = "pw-mpris-visualcard";
  std::string nodeDescription = "Music Card";
};

}  // namespace oms
