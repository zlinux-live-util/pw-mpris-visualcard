// Spectrum post-processing implementation.
//
// The motion model is cava's (github.com/karlstav/cava, cavacore.c), reproduced with its own
// constants. It is worth being explicit about why this is not a one-pole attack/release envelope,
// because that is the part that makes cava's bars look the way they do:
//
//   While a band is RISING, the measurement is used directly -- there is no attack smoothing at
//   all, so a transient reaches full height the instant it arrives.
//
//   While it is FALLING, the measurement is discarded. The output is recomputed from the peak of
//   the current rising run, as  peak * (1 - fall^2 * gravity_mod),  with `fall` advancing 0.028
//   per frame. Because fall is squared, the bar resists near its apex and then accelerates
//   downward -- the same shape as gravity. A falling bar is therefore a smooth synthetic curve
//   and cannot jitter, however noisy the audio is. The run ends when the curve drops below the
//   measurement: the measurement then takes the bar back over, which is what stops a slow, still
//   audible decay from being run all the way to zero by the curve.
//
//   An integral then adds momentum:  out = previous * mem + measured * (1 - mem),  where
//   mem = clamp(gravity / (66/fps)^0.1, 0, 0.98). The complementary weight is what holds that
//   stage's DC gain at 1; carrying the memory term alone inflates every sustained level.
//
// Chain order, and why:
//
//   gain -> motion (gravity + integral) -> 1-2-1 blur along the band axis -> sliding-window gain
//
// The blur sits between the motion and the gain so the level the gain measures is already the
// shaped one; measuring first would make the gain over-correct for peaks the blur is about to
// remove. The gain is last because it is only a final "keep the ring filling its band" scalar.
#include "fx.hpp"

#include <algorithm>
#include <cmath>

namespace oms {
namespace {

/** Cava's reference frame rate, and the 0.028 per-frame fall step, both straight from
 *  cavacore.c. Written against dt so the fall takes the same wall-clock time at any frame rate. */
constexpr double kRefFps = 66.0;
constexpr double kFallStep = 0.028;

/** Below this, cava skips its motion model entirely rather than dividing by ~0. */
constexpr double kMinGravity = 0.1;

/** Cava's frame-rate compensation is a single pow(66/fps, 2.5) term, which holds well down to
 *  about 20 fps and then collapses: the whole fall finishes inside one frame, so bars blink. dt is
 *  therefore clamped to this for the motion model only, which turns the failure into "the fall
 *  takes a few frames longer" instead. The auto-gain is unaffected -- it has its own time
 *  constants and is well behaved at any dt. */
constexpr double kMaxMotionDt = 1.0 / 20.0;

/** Highest frame rate the sliding-window ring is sized for. The window length itself is computed
 *  from the real dt, so this only bounds the buffer. */
constexpr double kWindowMaxFps = 120.0;
constexpr size_t kWindowMaxSlots = 1024;

/** The auto-gain reference never falls below this, so digital silence is not amplified up into a
 *  full-height ring, and the gain it can apply is capped. */
constexpr float kRefFloor = 0.03f;
constexpr float kMaxAutoGain = 8.0f;

/** Time constants for the auto-gain reference, in seconds. */
constexpr double kRefRiseS = 0.03;
constexpr double kRefFallS = 0.60;

}  // namespace

void SpectrumFx::resize(int bars) {
  bars = std::max(0, bars);
  const size_t n = static_cast<size_t>(bars);
  peak_.assign(n, 0.0f);
  fall_.assign(n, 0.0f);
  prevOut_.assign(n, 0.0f);
  prevMotion_.assign(n, 0.0f);
  scratch_.assign(n, 0.0f);
  reset();
}

void SpectrumFx::reset() {
  std::fill(peak_.begin(), peak_.end(), 0.0f);
  std::fill(fall_.begin(), fall_.end(), 0.0f);
  std::fill(prevOut_.begin(), prevOut_.end(), 0.0f);
  std::fill(prevMotion_.begin(), prevMotion_.end(), 0.0f);
  std::fill(scratch_.begin(), scratch_.end(), 0.0f);
  std::fill(win_.begin(), win_.end(), 0.0f);
  winPos_ = 0;
  winFill_ = 0;
  ref_ = 0.0f;
}

void SpectrumFx::apply(float* levels, int count, double dt) {
  if (!opts_.enabled || !levels || count <= 0) return;
  const int n = std::min(count, size());
  if (n <= 0) return;
  if (!(dt > 0.0)) dt = 1.0 / kRefFps;  // cava's reference rate

  // 1) User offset. Deliberately not clamped: everything below is a scalar multiply or a blend,
  //    and clamping here would flatten anything meant to pass full scale.
  if (opts_.gainDb != 0.0) {
    const float g = static_cast<float>(std::pow(10.0, opts_.gainDb / 20.0));
    for (int i = 0; i < n; ++i) levels[i] *= g;
  }

  // 2) Cava's motion model. framerate_mod is cava's 66/framerate, written in terms of dt.
  const double gravity = opts_.gravity;
  if (gravity > kMinGravity) {
    const double motionDt = std::min(dt, kMaxMotionDt);
    const double framerateMod = kRefFps * motionDt;
    const double gravityMod = std::pow(framerateMod, 2.5) * 2.0 / gravity;
    const double integralMod = std::pow(framerateMod, 0.1);
    // The integral is a memory term: a fraction of the previous output is carried forward. It
    // needs the complementary fraction of the new value, or it is an IIR filter whose DC gain is
    // 1/(1-c) and every sustained level comes out inflated -- at the default 30 fps that is
    // 1/0.2884 = 3.47x, so a steady 0.6 band drew as full height and rose further the longer it
    // was held.
    // Clamped below 1 because a consumer may negotiate a frame rate high enough to push the
    // coefficient to or past unity, which would be unstable rather than merely loud.
    const float mem = static_cast<float>(std::clamp(gravity / integralMod, 0.0, 0.98));
    const float feed = 1.0f - mem;
    for (int i = 0; i < n; ++i) {
      const size_t k = static_cast<size_t>(i);
      const double x = levels[i];
      double y;
      // The comparison is against the previous *motion* value, not the previous measurement --
      // this is cava's prev_cava_out, and it is what bounds the fall. Against measurements, any
      // monotonically decreasing stretch keeps this branch alive, so the curve runs to zero after
      // ~20 frames however slowly the level is really falling, and a note that is still audible
      // goes dark. Against the curve, the measurement takes the bar back over as soon as it is the
      // higher of the two.
      if (x < prevMotion_[k]) {
        // Falling: ignore the measurement, run the gravity curve off the run's peak.
        fall_[k] += static_cast<float>(kFallStep);
        y = peak_[k] * (1.0 - static_cast<double>(fall_[k]) * fall_[k] * gravityMod);
        if (y < 0.0) y = 0.0;
      } else {
        // Rising, level, or the curve has dropped below the measurement: track the measurement and
        // re-anchor the fall.
        peak_[k] = static_cast<float>(x);
        fall_[k] = 0.0f;
        y = x;
      }
      prevMotion_[k] = static_cast<float>(y);
      // The integral: a fraction of the previous output carried forward, which is what gives the
      // bars momentum. Cava uses the same knob as the gravity term. `feed` on the new value is
      // what keeps the stage's DC gain at 1 -- see the note where mem is computed.
      y = prevOut_[k] * mem + static_cast<float>(y) * feed;
      prevOut_[k] = static_cast<float>(y);
      levels[i] = static_cast<float>(y);
    }
  }

  // 3) Shape along the band axis: a single 1-2-1 pass, blended, so one loud band becomes a small
  //    three-band mound. Edges replicate rather than wrap, so the first and last bars are not
  //    smeared together across the ring.
  if (opts_.shape > 0.0 && n >= 3) {
    const float s = static_cast<float>(std::min(1.0, opts_.shape));
    for (int i = 0; i < n; ++i) {
      const float prev = levels[i > 0 ? i - 1 : 0];
      const float next = levels[i + 1 < n ? i + 1 : n - 1];
      scratch_[static_cast<size_t>(i)] = 0.25f * prev + 0.5f * levels[i] + 0.25f * next;
    }
    for (int i = 0; i < n; ++i)
      levels[i] = levels[i] * (1.0f - s) + scratch_[static_cast<size_t>(i)] * s;
  }

  // 4) Sliding-window auto-gain: the loudest frame within the window, brought toward quickly and
  //    released slowly, so a quiet passage opens up without the gain pumping on every transient.
  //    Window length comes from the real dt, so it is milliseconds of audio at any frame rate.
  if (opts_.normMs > 0.0) {
    const size_t want = std::min<size_t>(
        kWindowMaxSlots, static_cast<size_t>(opts_.normMs / 1000.0 * kWindowMaxFps) + 2);
    if (win_.size() < want) {
      win_.assign(want, 0.0f);
      winPos_ = 0;
      winFill_ = 0;
    }

    float fmax = 0.0f;
    for (int i = 0; i < n; ++i) fmax = std::max(fmax, levels[i]);
    win_[winPos_] = fmax;
    winPos_ = (winPos_ + 1) % win_.size();
    if (winFill_ < win_.size()) ++winFill_;

    const size_t span = std::clamp<size_t>(
        static_cast<size_t>(std::llround(opts_.normMs / (dt * 1000.0))), 1, winFill_);
    float wmax = 0.0f;
    for (size_t j = 0; j < span; ++j)
      wmax = std::max(wmax, win_[(winPos_ + win_.size() - 1 - j) % win_.size()]);

    const float ref = std::max(wmax, kRefFloor);
    // The reference rises quickly and falls lazily: opening up on a quiet passage should feel
    // immediate, while backing off again should not flinch at every transient. Independent of the
    // gravity knob, which is about bar motion, not about the gain.
    const float k = ref > ref_ ? static_cast<float>(1.0 - std::exp(-dt / kRefRiseS))
                               : static_cast<float>(1.0 - std::exp(-dt / kRefFallS));
    ref_ += k * (ref - ref_);
    const float gain = std::min(kMaxAutoGain, 1.0f / std::max(ref_, kRefFloor));
    for (int i = 0; i < n; ++i) levels[i] *= gain;
  }
}

}  // namespace oms
