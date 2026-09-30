#pragma once
// Optional display-side post-processing for the spectrum ring. On by default (--viz-fx 0 turns it
// off): the motion model is the part that makes the bars look like they have weight rather than
// jumping with every frame.
//
// The motion model is cava's, not an attack/release envelope. Cava (github.com/karlstav/cava,
// cavacore.c) ignores the measurement while a bar is falling and recomputes it from the peak of
// the current rising run, so a falling bar is a smooth synthetic curve rather than whatever the
// audio happened to do that frame. That is the reason its bars look like they have weight. An
// integral then adds momentum. The constants below are cava's, with its frame-rate terms written
// in terms of dt instead of a frame counter.
//
// Deliberately absent: noise reduction and noise gates. Cava has a knob called `noise_reduction`
// and it is *not* that -- it is smoothing strength, and it is used here under an honest name.
//
// None of it runs when --viz-fx is off: apply() then returns on its first branch and the ring shows
// exactly what the analyser measured.
#include <cstddef>
#include <vector>

namespace oms {

struct SpectrumFxOptions {
  /** Master switch, --viz-fx. */
  bool enabled = true;

  /** dB added to every band before anything else. */
  double gainDb = 0.0;

  /** Cava's `noise_reduction`: how heavy the bars are, 0..1. Higher means a slower fall and more
   *  momentum. 0.1 or less disables the motion model entirely (cava's own threshold; bars then
   *  track the audio exactly). Cava ships 77 / 100. */
  double gravity = 0.77;

  /** 0..1 blend towards a 1-2-1 blur along the band axis, so a lone band reads as a three-band
   *  mound instead of a spike. This one is ours; cava has no equivalent. */
  double shape = 0.5;

  /** Length of the sliding window, in milliseconds, that the auto-gain looks back over. 0 disables
   *  it, leaving the fixed dB window in analyser.cpp in sole charge of the levels. */
  double normMs = 2000.0;
};

class SpectrumFx {
 public:
  SpectrumFx() = default;
  explicit SpectrumFx(int bars) { resize(bars); }

  /** Sizes the per-band state. Existing state is reset, because it is indexed per band. */
  void resize(int bars);
  void reset();
  void setOptions(SpectrumFxOptions o) { opts_ = o; }
  const SpectrumFxOptions& options() const { return opts_; }

  /** Applies the chain to `count` levels in place. `dt` is the time since the previous call in
   *  seconds; values <= 0 are read as one frame at 66 fps, cava's reference rate (upstream's
   *  `cavacore.c` really does use 66, not 60).
   *
   *  `levels` must hold at least `count` entries; a count larger than size() is clamped down, but
   *  the caller cannot be protected from lying about its own buffer.
   *
   *  IMPORTANT: the caller must overwrite `levels` with freshly measured values before every
   *  call. The motion model is stateful and reads this same array, so passing back a buffer that
   *  has already been through apply() makes it chase its own output and the integral run away.
   *  Card::setSpectrum() is what guarantees this. */
  void apply(float* levels, int count, double dt);

  int size() const { return static_cast<int>(peak_.size()); }
  /** Current auto-gain reference, i.e. the level the ring is being scaled against. Diagnostic. */
  float reference() const { return ref_; }

 private:
  SpectrumFxOptions opts_;
  std::vector<float> peak_;      // peak of the current rising run; the anchor for the fall
  std::vector<float> fall_;      // how far into the fall this band is
  std::vector<float> prevOut_;   // previous output, for the integral
  std::vector<float> prevMotion_;  // previous pre-integral output; the rise/fall test, cava's
                                   // prev_cava_out (deliberately NOT the previous measurement)
  std::vector<float> scratch_;   // blur pass, per band
  std::vector<float> win_;       // sliding window of per-frame maxima
  size_t winPos_ = 0;
  size_t winFill_ = 0;
  float ref_ = 0.0f;             // smoothed auto-gain reference
};

}  // namespace oms
