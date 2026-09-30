// Spectrum analysis implementation.
//
// Windowing
// ---------
// A Hann window is the minimum for a usable spectrum: a rectangular window smears every partial
// over ~9 bins (its main lobe is four bins wide), which turns the band edges into ramps and makes
// adjacent bars look correlated. The cost is the same, so there is no reason to skip it.
//
// The coherent gain of a Hann window is exactly 0.5, so the amplitude of a bin is
// A = 4*|X[k]|/N. Folding a band is a root mean square over its bins, which is the honest band
// level; taking the maximum instead would make the display jump between two bins of the same
// partial on every frame.
//
// Levels
// ------
// Bands are mapped onto 0..1 through a fixed dBFS window. Fixed, not adaptive: a rolling
// maximum would make a quiet passage fill the ring and a loud one sit at the bottom, i.e. it
// would normalise the material away. The window is the same for every track, so bar height is
// comparable between tracks and stays proportional to what is playing.
#include "analyser.hpp"

#include <algorithm>
#include <cmath>

namespace oms {
namespace {

constexpr double kPi = 3.14159265358979323846;

/** Fixed display window, in dBFS relative to full scale. Measured over 25 s of live playback at
 *  48 kHz with 72 bands and a 2048-point window (the table is in docs/internals.md): band levels
 *  ran p05 -97, p50 -68, p90 -50, p99 -35 dBFS against a broadband RMS of -35 dBFS. A floor above
 *  -85 empties the quiet end of the ring, a ceiling near -20 pins the bass. Fixed, not adaptive
 *  -- see the note above the class. */
constexpr double kFloorDb = -85.0;
constexpr double kCeilDb = -30.0;

/** Band layout: the bottom sixth of the bars is linear and the rest logarithmic. Purely
 *  logarithmic spacing asks for 3 Hz bands at 40 Hz, which no window short enough to follow the
 *  music can resolve, so the first few bars would all repeat one bin. A linear bottom gives each
 *  low bar a bin of its own. */
constexpr double kLowHz = 45.0;
constexpr double kLinTopHz = 300.0;
constexpr int kLinearBandsDiv = 6;
constexpr double kHighHz = 14000.0;

/** Never spend more than this many transforms on one feed() call, so a long stall in the caller
 *  cannot turn into a long stall in the render callback. */
constexpr int kMaxTransformsPerFeed = 8;

}  // namespace

Analyser::Analyser(int bands, int fftSize) : fftSize_(fftSize) {
  bands = std::clamp(bands, 1, 256);
  fftSize_ = 1;
  while (fftSize_ < fftSize) fftSize_ <<= 1;
  hop_ = std::max(1, fftSize_ / 4);

  levels_.assign(static_cast<size_t>(bands), 0.0f);
  binLo_.assign(static_cast<size_t>(bands), 0);
  binHi_.assign(static_cast<size_t>(bands), 0);

  hist_.assign(static_cast<size_t>(fftSize_), 0.0f);
  histMask_ = fftSize_ - 1;

  window_.resize(static_cast<size_t>(fftSize_));
  for (int i = 0; i < fftSize_; ++i)
    window_[static_cast<size_t>(i)] =
        static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / (fftSize_ - 1))));

  re_.resize(static_cast<size_t>(fftSize_));
  im_.resize(static_cast<size_t>(fftSize_));

  // Bit-reversal permutation and one full turn of twiddles, both index-pattern independent of the
  // data, so they are built once here instead of per transform.
  int bits = 0;
  while ((1 << bits) < fftSize_) ++bits;
  rev_.resize(static_cast<size_t>(fftSize_));
  for (int i = 0; i < fftSize_; ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b)
      if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    rev_[static_cast<size_t>(i)] = r;
  }
  twRe_.resize(static_cast<size_t>(fftSize_ / 2));
  twIm_.resize(static_cast<size_t>(fftSize_ / 2));
  for (int i = 0; i < fftSize_ / 2; ++i) {
    const double a = -2.0 * kPi * i / fftSize_;
    twRe_[static_cast<size_t>(i)] = static_cast<float>(std::cos(a));
    twIm_[static_cast<size_t>(i)] = static_cast<float>(std::sin(a));
  }
}

void Analyser::setRate(int rate) {
  if (rate == rate_ || rate < 1) return;
  rate_ = rate;
  buildBands();
}

void Analyser::buildBands() {
  const int nBins = fftSize_ / 2;  // bin 0 is DC, which a Hann window does not suppress
  const double nyquist = rate_ * 0.5;
  const double hi = std::max(kLinTopHz * 1.0001, std::min(kHighHz, nyquist * 0.98));
  const int n = static_cast<int>(levels_.size());
  const int nLin = n > 1 ? std::clamp(n / kLinearBandsDiv, 1, n - 1) : 1;
  const double linStep = (kLinTopHz - kLowHz) / nLin;
  const double logLo = std::log(kLinTopHz), logHi = std::log(hi);

  for (int b = 0; b < n; ++b) {
    double f0, f1;
    if (b < nLin) {
      f0 = kLowHz + linStep * b;
      f1 = kLowHz + linStep * (b + 1);
    } else if (n == nLin) {
      f0 = f1 = kLinTopHz;
    } else {
      const double t0 = static_cast<double>(b - nLin) / (n - nLin);
      const double t1 = static_cast<double>(b + 1 - nLin) / (n - nLin);
      f0 = std::exp(logLo + (logHi - logLo) * t0);
      f1 = std::exp(logLo + (logHi - logLo) * t1);
    }
    int k0 = static_cast<int>(f0 * fftSize_ / rate_);
    int k1 = static_cast<int>(f1 * fftSize_ / rate_);
    if (k0 < 1) k0 = 1;
    if (k1 > nBins) k1 = nBins;
    if (k1 <= k0) k1 = std::min(nBins, k0 + 1);  // narrow bands would otherwise collapse to nothing
    if (k1 <= k0) k0 = std::max(1, k1 - 1);
    binLo_[static_cast<size_t>(b)] = k0;
    binHi_[static_cast<size_t>(b)] = k1;
  }
}

void Analyser::transform() {
  const int n = fftSize_;
  for (int i = 0; i < n; ++i) {
    const int r = rev_[static_cast<size_t>(i)];
    if (i < r) {
      std::swap(re_[static_cast<size_t>(i)], re_[static_cast<size_t>(r)]);
      std::swap(im_[static_cast<size_t>(i)], im_[static_cast<size_t>(r)]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const int half = len >> 1;
    const int step = n / len;
    for (int base = 0; base < n; base += len) {
      for (int j = 0; j < half; ++j) {
        const size_t t = static_cast<size_t>(j * step);
        const float wr = twRe_[t], wi = twIm_[t];
        const size_t a = static_cast<size_t>(base + j);
        const size_t b = a + static_cast<size_t>(half);
        const float tr = re_[b] * wr - im_[b] * wi;
        const float ti = re_[b] * wi + im_[b] * wr;
        re_[b] = re_[a] - tr;
        im_[b] = im_[a] - ti;
        re_[a] += tr;
        im_[a] += ti;
      }
    }
  }
}

void Analyser::analyse(uint64_t end) {
  const int nFft = fftSize_;
  for (int i = 0; i < nFft; ++i) {
    const uint64_t src = end - static_cast<uint64_t>(nFft) + static_cast<uint64_t>(i);
    re_[static_cast<size_t>(i)] = hist_[static_cast<size_t>(src & histMask_)] *
                                  window_[static_cast<size_t>(i)];
    im_[static_cast<size_t>(i)] = 0.0f;
  }
  transform();

  // |X[k]| -> amplitude, using the Hann coherent gain of exactly 0.5.
  const float ampScale = 4.0f / static_cast<float>(nFft);
  const int nBands = static_cast<int>(levels_.size());
  for (int b = 0; b < nBands; ++b) {
    const int k0 = binLo_[static_cast<size_t>(b)];
    const int k1 = binHi_[static_cast<size_t>(b)];
    if (k1 <= k0) {
      levels_[static_cast<size_t>(b)] = 0.0f;
      continue;
    }
    double sum = 0.0;
    for (int k = k0; k < k1; ++k) {
      const size_t i = static_cast<size_t>(k);
      const double mag = std::hypot(re_[i], im_[i]) * ampScale;
      sum += mag * mag;
    }
    const double rms = std::sqrt(sum / static_cast<double>(k1 - k0));
    double db = 20.0 * std::log10(rms + 1e-9);
    const double t = (db - kFloorDb) / (kCeilDb - kFloorDb);
    levels_[static_cast<size_t>(b)] =
        static_cast<float>(t <= 0.0 ? 0.0 : (t >= 1.0 ? 1.0 : t));
  }
}

void Analyser::feed(const float* samples, size_t count) {
  if (!samples || count == 0) return;
  if (rate_ < 1) return;

  for (size_t i = 0; i < count; ++i)
    hist_[static_cast<size_t>((written_ + i) & static_cast<uint64_t>(histMask_))] = samples[i];
  written_ += count;

  // Windows advance by one hop and each one ends as late as it can, so the displayed spectrum
  // lags by one window and no more. The per-call cap bounds the work; when it is hit while still
  // behind, the backlog is dropped rather than replayed -- otherwise a caller that hands over a
  // large block falls permanently behind and the ring shows a spectrum that grows staler every
  // frame instead of the music being played now.
  const uint64_t fft = static_cast<uint64_t>(fftSize_);
  const uint64_t hop = static_cast<uint64_t>(hop_);
  const uint64_t step = std::max(hop, fft);
  for (int n = 0; n < kMaxTransformsPerFeed; ++n) {
    if (written_ < fft || written_ - analysed_ < hop) break;
    const uint64_t end = std::min(written_, analysed_ + step);
    if (end < fft) break;
    analyse(end);
    analysed_ = end;
  }
  if (written_ >= fft && written_ - analysed_ > 2 * fft) analysed_ = written_ - fft;
}

}  // namespace oms
