#pragma once
// Card rendering: hand-drawn with cairo + pango. All layout size/color constants are grouped at
// the top of card.cpp.
//
// The cairo surfaces and the pango renderer live behind Impl so that this header stands on its
// own: it needs nothing but cairo and the project's own types, and an includer does not have to
// carry the submodule's include path to use it. card.cpp is where those types are reached for.
#include <cairo/cairo.h>

#include <cstdint>
#include <memory>
#include <string>

#include "fx.hpp"     // SpectrumFx, the optional ring post-processing
#include "types.hpp"

namespace oms {

class Card {
 public:
  /** Upper bound on the ring's bar count, so the per-frame buffer is a fixed array and the render
   *  path never allocates. Matches the ceiling --viz-bars clamps to. */
  static constexpr int kMaxVizBars = 256;

  explicit Card(Config cfg);
  ~Card();
  Card(const Card&) = delete;
  Card& operator=(const Card&) = delete;

  /** Whether there is currently anything to display. */
  bool visible(const NowPlaying& np) const;

  /** Draw one frame. The cr canvas must be a cfg.size square; cover may be nullptr. */
  void render(cairo_t* cr, const NowPlaying& np, cairo_surface_t* cover, int64_t nowMs);

  /** Hand over the current spectrum: `count` values in 0..1, lowest frequency first. Copied into
   *  fixed storage, so the caller's buffer need not outlive the frame. Called from the same
   *  thread as render(). Ignored unless --viz is on. */
  void setSpectrum(const float* levels, int count);

  /** The height currently drawn for bar `i`, after any post-processing, as a fraction of the
   *  ring's radial band. Diagnostic: lets a harness or a test check the displayed spectrum without
   *  having to reverse-engineer the layout out of pixels. */
  float level(int i) const { return (i >= 0 && i < vizCount_) ? viz_[i] : 0.0f; }

  int width() const { return cfg_.width; }
  int height() const { return cfg_.height; }
  const Config& config() const { return cfg_; }

 private:
  struct Metrics {
    double pad, radius, gap, metaGap;
    double coverD, ringGap, ringW;
    double titleSize, subSize, lyricSize, timeSize;
    // Radial spectrum ring (--viz). Only read when the ring is on; all zero otherwise.
    double vizGap, vizBand;
  };

  /** Everything that needs a type from the cairo/pango helpers. */
  struct Impl;

  bool transparent() const { return !hasBg_; }

  /** Radius the spectrum ring adds around the cover, i.e. its clearance plus its band. Zero when
   *  the ring is off. The ring surrounds the cover, so the layout has to reserve this on all four
   *  sides: twice in the width budget (a margin inside each edge of --size), once in the vertical
   *  budget for the top, and once in the cover-to-text gap at the bottom. */
  double vizRingR() const { return cfg_.showViz ? m_.vizGap + m_.vizBand : 0.0; }

  /** Static layer: card background / cover drop shadow / cover circular-backdrop gradient /
   *  progress-ring track / the spectrum ring's groove / all text. Text and cover do not overlap,
   *  so merging them into the same layer is entirely safe: one blit per frame. Reused as-is while
   *  the content (including the text key) is unchanged. */
  cairo_surface_t* staticLayer(const NowPlaying& np, int64_t pos, double k, double W,
                               double H, double cy, double yMeta, double textW);

  /** Cache key for the text layer. */
  std::string textKey(const NowPlaying& np, int64_t pos) const;

  void drawTexts(cairo_t* cr, const NowPlaying& np, int64_t pos, double yMeta,
                 double textW);

  /** The bar ring around the cover. One fill per bar; see the comment in card.cpp for the two
   *  measurements behind that and behind drawing chords instead of arcs. */
  void drawSpectrum(cairo_t* cr, double cx, double cy, double coverR);

  /** Draw one text line (clamped to maxLines); with glow, stroke the shadow first, then fill. */
  void drawLine(cairo_t* cr, const std::string& s, double size, double alpha, double x,
                double width, double yTop, double boxH, int maxLines, bool bold);

  Config cfg_;
  Metrics m_;
  bool hasBg_ = true;
  double bgR_ = 0, bgG_ = 0, bgB_ = 0;

  /** Rotate src (a square image with a 1px border) about its center by angle, writing into the
   *  circular region of dst. Within a row the source coordinates are equally spaced, so only the
   *  row's start value needs the two multiplications. */
  static void rotateInto(const uint32_t* src, int sw, int sh, uint32_t* dst, int dpitch,
                         int side, double radius, double angle);

  std::unique_ptr<Impl> impl_;

  /** Ring post-processing (--viz-fx). Holds the per-band state the temporal stages need. */
  SpectrumFx fx_;

  double spinAngle_ = 0.0;
  double vizAngle_ = 0.0;  // bar ring rotation: opposite to the cover, and slower
  float viz_[kMaxVizBars] = {};
  int vizCount_ = 0;
  int64_t lastRenderAt_ = 0;
};

}  // namespace oms
