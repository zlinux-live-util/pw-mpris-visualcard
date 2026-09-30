// Card rendering.
//
// Performance notes:
//   1) Cairo linear-gradient fills are very slow (about 0.6ms per 360x360 pass), so everything
//      that does not change per frame (card base, cover shadow, cover disc gradient, progress-ring
//      track) is baked into the static layer and only blitted each frame.
//   2) Text shaping + shadow stroking is equally expensive, so it joins the same static layer as
//      the above, keyed by content, and is redone only on a track change, lyric page flip, or a
//      second tick.
//
// Background --bg:
//   none (default) -- fully transparent. Cover enlarged to 92% width / vertical budget, secondary
//                     text brightened to .9, text and cover carry their own shadows; otherwise they
//                     smear over a bright game frame.
//   solid          -- opaque dark background #16171c.
//   #rrggbb        -- explicit background colour.
#include "card.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "cairo_util.hpp"  // pwvideo::SurfacePtr, pwvideo::CairoSurfaceDeleter
#include "text.hpp"        // pwvideo::TextRenderer, LabelSpec, LabelMetrics, Rgba

namespace oms {

/** The cairo/pango state. Kept out of the header so card.hpp stands on its own. */
struct Card::Impl {
  /** Baked card background, cover shadow, cover disc, progress track, ring groove and all text.
   *  One blit per frame while the key is unchanged. */
  pwvideo::SurfacePtr layer_;
  std::string layerKey_;

  /** Rotated cover layer (circular clip + edge antialiasing are baked in; one blit per frame).
   *  Uses a hand-written row-stepping rotation, about 30% faster than cairo's general transform
   *  path. */
  pwvideo::SurfacePtr coverLayer_;
  double coverAngle_ = 1e9;

  pwvideo::SurfacePtr artScaled_;  // cover scaled to coverD, with a 1px border
  std::string artScaledKey_;

  pwvideo::TextRenderer text_;
};

namespace {

constexpr double kPi = 3.14159265358979323846;

// Layout constants, all based on a source height of 360px; scaled proportionally by height/360.
constexpr double kBaseSize = 360.0;
constexpr double kPadPx = 12.0;
constexpr double kRadiusPx = 14.0 * 1.7;
constexpr double kGapPx = 11.0 * 0.9;
constexpr double kMetaGapPx = 2.0;
constexpr double kRingGapPx = 6.0;
constexpr double kRingWPx = 3.0;
constexpr double kLyricMarginTopPx = 3.0;

// Radial spectrum ring (--viz). Read only while the ring is on, so with it off the layout is
// bit-for-bit what it always was.
// Radial thickness. Bars are grown 1.5x over the first cut of this feature: at the old value the
// ring read as a thin fringe around the cover rather than as a spectrum.
constexpr double kVizBandFrac = 0.052;   // radial thickness, as a fraction of height
constexpr double kVizBandMinPx = 12.0;   // and the bounds it is clamped to, at 360px
constexpr double kVizBandMaxPx = 30.0;
constexpr double kVizClearPx = 1.0;      // clearance between the progress ring and the bars
constexpr double kVizDuty = 0.68;        // bar width as a fraction of its angular slot
constexpr double kVizMoatPx = 3.0;       // text-free gap between the bar tips and the first line
constexpr double kVizSpinRatio = 4.0;    // one bar-ring turn per N cover turns

// Colours (matching :root)
constexpr double kSolidR = 0x16 / 255.0, kSolidG = 0x17 / 255.0, kSolidB = 0x1c / 255.0;
constexpr double kFgDimSolid = 0.62;
constexpr double kFgDimNone = 0.90;   // brightened without a plate, otherwise translucent white text smears
constexpr double kRingColor = 0.09;
constexpr double kTrackColor = 0.16;
constexpr double kAccentColor = 0.92;
// The ring is monochrome like the rest of the card: near-white bars, one faint groove at their
// base so the ring still reads as a ring when nothing is playing.
constexpr double kVizBarColor = 0.94;
constexpr double kVizTrackColor = 0.15;

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** "none" / "transparent" / "0" → no background; "solid" / "dark" → default dark; "#rgb" / "#rrggbb" → explicit colour */
bool parseBg(const std::string& v, double& r, double& g, double& b) {
  std::string s;
  for (char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (s.empty() || s == "none" || s == "transparent" || s == "0") return false;
  if (s == "solid" || s == "dark" || s == "1") {
    r = kSolidR; g = kSolidG; b = kSolidB;
    return true;
  }
  if (s[0] == '#') s.erase(0, 1);
  if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
  if (s.size() != 6) { r = kSolidR; g = kSolidG; b = kSolidB; return true; }
  const int rr = hexVal(s[0]) * 16 + hexVal(s[1]);
  const int gg = hexVal(s[2]) * 16 + hexVal(s[3]);
  const int bb = hexVal(s[4]) * 16 + hexVal(s[5]);
  if (rr < 0 || gg < 0 || bb < 0) { r = kSolidR; g = kSolidG; b = kSolidB; return true; }
  r = rr / 255.0; g = gg / 255.0; b = bb / 255.0;
  return true;
}

double maxOf(double a, double b) { return a > b ? a : b; }

/** Two-channel packed interpolation: R+B in one pass, G+A in another; the weight sum lies in 0..256.
 *  Safe only when the weight sum is 256. Writing the weight sum as 65536 overflows the red channel's left shift and blows it out. */
inline uint32_t lerp2(uint32_t a, uint32_t b, uint32_t w) {
  const uint32_t iw = 256 - w;
  const uint32_t lo = (((a & 0x00FF00FF) * iw + (b & 0x00FF00FF) * w) >> 8) & 0x00FF00FF;
  const uint32_t hi =
      ((((a >> 8) & 0x00FF00FF) * iw + ((b >> 8) & 0x00FF00FF) * w) >> 8) & 0x00FF00FF;
  return lo | (hi << 8);
}

void roundRect(cairo_t* cr, double x, double y, double w, double h, double r) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -kPi / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, kPi / 2);
  cairo_arc(cr, x + r, y + h - r, r, kPi / 2, kPi);
  cairo_arc(cr, x + r, y + r, r, kPi, 1.5 * kPi);
  cairo_close_path(cr);
}

std::string fmtTime(int64_t ms) {
  if (ms < 0) ms = 0;
  const int64_t s = ms / 1000;
  const int64_t h = s / 3600;
  const int64_t m = (s % 3600) / 60;
  const int64_t ss = s % 60;
  char buf[32];
  if (h > 0)
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)h, (long long)m,
                  (long long)ss);
  else
    std::snprintf(buf, sizeof buf, "%lld:%02lld", (long long)m, (long long)ss);
  return buf;
}

/** Which lyric line is playing (binary search); -1 means the first line has not been reached. */
int activeLyric(const std::vector<Lyric>& L, int64_t ms) {
  int lo = 0, hi = static_cast<int>(L.size()) - 1, ans = -1;
  while (lo <= hi) {
    const int mid = (lo + hi) >> 1;
    if (L[static_cast<size_t>(mid)].t <= ms) {
      ans = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return ans;
}

}  // namespace

Card::Card(Config cfg)
    : cfg_(std::move(cfg)), impl_(std::make_unique<Impl>()), fx_(cfg_.vizBars) {
  SpectrumFxOptions fx;
  fx.enabled = cfg_.vizFx;
  fx.gainDb = cfg_.vizGainDb;
  fx.gravity = cfg_.vizGravity;
  fx.shape = cfg_.vizShape;
  fx.normMs = cfg_.vizNormMs;
  fx_.setOptions(fx);

  hasBg_ = parseBg(cfg_.bg, bgR_, bgG_, bgB_);

  const double W = cfg_.width, H = cfg_.height;
  const double k = H / kBaseSize;   // all dimensions scale proportionally with height

  m_.pad = hasBg_ ? kPadPx * k : 0.0;
  m_.radius = kRadiusPx * k;
  m_.gap = kGapPx * k;
  m_.metaGap = kMetaGapPx * k;
  m_.ringGap = kRingGapPx * k;
  m_.ringW = kRingWPx * k;
  m_.titleSize = maxOf(11.0 * k, 0.041 * H);
  m_.subSize = maxOf(9.0 * k, 0.029 * H);
  m_.lyricSize = maxOf(10.0 * k, 0.030 * H);
  m_.timeSize = maxOf(9.0 * k, 0.027 * H);

  // Radial spectrum ring. Everything below this block is conditional on --viz, so with it off the
  // layout reduces to exactly the one that shipped before the ring existed.
  m_.vizGap = 0.0;
  m_.vizBand = 0.0;
  if (cfg_.showViz) {
    m_.vizBand = std::clamp(kVizBandFrac * H, kVizBandMinPx * k, kVizBandMaxPx * k);
    // Bars start just outside the progress ring, so the two never touch.
    m_.vizGap = (cfg_.showProgress ? m_.ringGap + m_.ringW : 0.0) + kVizClearPx * k;
    // The ring needs clearance all the way round, and the bottom of the ring is exactly where the
    // text starts. Rather than letting the bars grow over the first line, the cover-to-text gap is
    // widened until they clear it. The ring's radius is charged to the other three sides below.
    m_.gap = std::max(m_.gap, m_.vizGap + m_.vizBand + kVizMoatPx * k);
  }

  // Cover diameter: the smaller of "width allows" and "vertical remainder".
  // On a square canvas the vertical limit binds first, so the sides are necessarily left empty --
  // to tighten, reduce the width of --size.
  // The vertical budget reserves for the lyric-present worst case, so the cover does not suddenly
  // rescale when switching to a track with lyrics.
  const int nLy = cfg_.lyricLines;
  const double titleBox = 2.0 * 1.3 * m_.titleSize;
  const double subBox = 1.35 * m_.subSize;
  double metaH = titleBox + m_.metaGap + subBox;
  if (nLy > 0) {
    const double lyricBox = nLy * 1.35 * m_.lyricSize + (nLy - 1) * m_.metaGap +
                            kLyricMarginTopPx * k;
    metaH += m_.metaGap + lyricBox;
  }
  double restH = m_.gap + metaH;
  if (cfg_.showTime) restH += m_.gap + 1.35 * m_.timeSize;
  double byWidth = 0.92 * W;
  double byHeight = 0.96 * H - restH;
  if (cfg_.showViz) {
    // The ring sits outside the cover all the way round, so the vertical budget has to reserve it
    // too, not just the width: the width budget gives it a margin on both sides and the widened gap
    // above already covers it at the bottom. The top was the one side left unpaid, and without this
    // the ring's outer edge is drawn off the top of the canvas.
    byWidth -= 2.0 * vizRingR();
    byHeight -= vizRingR();
  }
  // The floor is the fraction of the height the cover would like to keep, which a narrow canvas can
  // legitimately push past; the min with byWidth keeps clamp's precondition rather than relying on
  // it holding. With the ring on it is additionally capped at the vertical remainder, so the floor
  // cannot hand back the room the ring needs at the top and push its outer edge off the canvas.
  // The 1px outside all of it keeps the radius positive where the remainder is negative outright.
  double lo = std::min(0.20 * H, byWidth);
  if (cfg_.showViz) lo = std::min(lo, byHeight);
  m_.coverD = std::max(1.0, std::clamp(std::min(byWidth, byHeight), lo, byWidth));
}

Card::~Card() = default;

std::string Card::textKey(const NowPlaying& np, int64_t pos) const {
  const Track& t = np.track;
  const bool showTimes = cfg_.showTime && t.duration > 0;
  const int nLy = cfg_.lyricLines > 0
                      ? std::min<int>(cfg_.lyricLines, static_cast<int>(t.lyrics.size()))
                      : 0;
  const int act = activeLyric(t.lyrics, pos);
  std::string key;
  key.reserve(256);
  key += t.title;
  key.push_back('\x1f');
  key += t.artist;
  key.push_back('\x1f');
  key += t.album;
  key.push_back(cfg_.showAlbum ? '1' : '0');
  key.push_back('\x1f');
  key += showTimes ? fmtTime(pos) : std::string();
  key.push_back('\x1f');
  key += std::to_string(act);
  for (int i = 0; i < nLy; ++i) {
    const int idx = (act >= 0 ? act : 0) + i;
    if (idx >= static_cast<int>(t.lyrics.size())) break;
    key.push_back('\x1e');
    key += t.lyrics[static_cast<size_t>(idx)].text;
  }
  return key;
}

cairo_surface_t* Card::staticLayer(const NowPlaying& np, int64_t pos, double k, double W,
                                   double H, double cy, double yMeta, double textW) {
  const int SZ = static_cast<int>(H);
  std::string key = std::to_string(cfg_.width) + "x" + std::to_string(SZ) + "#" +
                    std::to_string(static_cast<int>(cy * 2)) + "#" + textKey(np, pos);
  if (impl_->layer_ && key == impl_->layerKey_) return impl_->layer_.get();

  cairo_surface_t* surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cfg_.width, SZ);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return nullptr;
  }
  cairo_t* cr = cairo_create(surf);
  const double cx = W / 2.0;
  const double coverR = m_.coverD / 2.0;

  if (hasBg_) {
    // Outer shadow 0 6px 22px rgba(0,0,0,.32): approximated by several decreasing strokes (visible only outside the card's rounded corners)
    const double dy = 6.0 * k, blur = 22.0 * k;
    static const double widths[] = {1.0, 0.72, 0.45, 0.22};
    static const double alphas[] = {0.02, 0.05, 0.08, 0.11};
    for (int i = 0; i < 4; ++i) {
      cairo_set_line_width(cr, blur * widths[i]);
      cairo_set_source_rgba(cr, 0, 0, 0, alphas[i]);
      roundRect(cr, 0, dy, W, H, m_.radius + blur * widths[i] / 2.0);
      cairo_stroke(cr);
    }
    roundRect(cr, 0, 0, W, H, m_.radius);
    cairo_set_source_rgb(cr, bgR_, bgG_, bgB_);
    cairo_fill(cr);
    roundRect(cr, 0.5, 0.5, W - 1, H - 1, m_.radius - 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1, 1, 1, kRingColor);
    cairo_stroke(cr);
  } else {
    // No plate: the cover must carry its own soft shadow, otherwise its edge smears over a bright frame.
    // Approximated by several outward-spreading, progressively fainter strokes as a drop-shadow.
    static const double spread[] = {1.5, 3.4, 5.6};
    static const double alpha[] = {0.22, 0.14, 0.07};
    for (int i = 0; i < 3; ++i) {
      cairo_set_line_width(cr, 3.0 * k);
      cairo_set_source_rgba(cr, 0, 0, 0, alpha[i]);
      cairo_arc(cr, cx, cy + 1.5 * k, coverR + spread[i] * k, 0, 2 * kPi);
      cairo_stroke(cr);
    }
  }

  // Cover disc: CSS linear-gradient(150deg, --track, transparent). The most expensive pass in the whole frame.
  {
    const double ang = 150.0 * kPi / 180.0;
    const double dx = std::sin(ang), dy = -std::cos(ang);
    const double len = m_.coverD * (std::abs(dx) + std::abs(dy));
    cairo_pattern_t* g = cairo_pattern_create_linear(cx - dx * len / 2, cy - dy * len / 2,
                                                     cx + dx * len / 2,
                                                     cy + dy * len / 2);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, kTrackColor);
    cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0.0);
    cairo_arc(cr, cx, cy, coverR, 0, 2 * kPi);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
  }
  // art-wrap's 1px inner stroke
  cairo_arc(cr, cx, cy, coverR - 0.5, 0, 2 * kPi);
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgba(cr, 1, 1, 1, kRingColor);
  cairo_stroke(cr);

  // The progress-ring track is static too (stroking is expensive; do not draw it every frame)
  if (cfg_.showProgress) {
    const double ringR = coverR + m_.ringGap - m_.ringW / 2.0;
    cairo_arc(cr, cx, cy, ringR, 0, 2 * kPi);
    cairo_set_line_width(cr, m_.ringW);
    cairo_set_source_rgba(cr, 1, 1, 1, kTrackColor);
    cairo_stroke(cr);
  }

  // The groove the spectrum bars grow out of. Static like everything else above, so the ring still
  // reads as a ring when nothing is playing or no audio node could be found.
  if (cfg_.showViz) {
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1, 1, 1, kVizTrackColor);
    cairo_arc(cr, cx, cy, coverR + m_.vizGap, 0, 2 * kPi);
    cairo_stroke(cr);
  }

  // Text joins the same layer (including the shadow stroke when there is no plate)
  drawTexts(cr, np, pos, yMeta, textW);

  cairo_destroy(cr);
  impl_->layer_ = pwvideo::SurfacePtr(surf, pwvideo::CairoSurfaceDeleter{});
  impl_->layerKey_ = std::move(key);
  return impl_->layer_.get();
}

void Card::setSpectrum(const float* levels, int count) {
  const int n = levels ? std::clamp(count, 0, kMaxVizBars) : 0;
  for (int i = 0; i < n; ++i) viz_[i] = levels[i];
  for (int i = n; i < vizCount_; ++i) viz_[i] = 0.0f;  // the ring shrank: do not leave stale bars
  vizCount_ = n;
}

void Card::drawSpectrum(cairo_t* cr, double cx, double cy, double coverR) {
  const int n = vizCount_;
  if (n <= 0) return;
  const double r0 = coverR + m_.vizGap;
  const double slot = 2 * kPi / n;
  const double half = slot * kVizDuty * 0.5;
  // cos/sin of the half slot are the same for every bar.
  const double ch = std::cos(half), sh = std::sin(half);

  cairo_set_source_rgba(cr, 1, 1, 1, kVizBarColor);
  for (int i = 0; i < n; ++i) {
    const double v = viz_[i];
    if (v <= 0.0f) continue;  // silence: leave a gap rather than a stub
    // Levels may exceed 1 after --viz-gain or the auto-gain; the band is the hard ceiling.
    const double r1 = r0 + m_.vizBand * std::min(v, 1.0);
    // The sides are chords, not arcs. At this radius and slot width the arc's deviation from the
    // chord is a fraction of a pixel, and measured over 72 bars a chord is 1.2x the speed of two
    // cairo_arc calls (0.24 vs 0.29 ms). cos/sin of the slot are constant, so each bar costs two
    // trig calls and four multiplies rather than eight.
    //
    // One fill per bar, not one fill for the whole ring: measured the other way round, batching all
    // 72 quads into a single path and filling once is 1.4x SLOWER (0.34 vs 0.25 ms), because cairo
    // tessellates the combined path as one unit while each 4-gon on its own is trivial.
    const double a = vizAngle_ + slot * i;
    const double ca = std::cos(a), sa = std::sin(a);
    cairo_new_path(cr);
    cairo_move_to(cr, cx + (ca * ch + sa * sh) * r0, cy + (sa * ch - ca * sh) * r0);
    cairo_line_to(cr, cx + (ca * ch - sa * sh) * r0, cy + (sa * ch + ca * sh) * r0);
    cairo_line_to(cr, cx + (ca * ch - sa * sh) * r1, cy + (sa * ch + ca * sh) * r1);
    cairo_line_to(cr, cx + (ca * ch + sa * sh) * r1, cy + (sa * ch - ca * sh) * r1);
    cairo_close_path(cr);
    cairo_fill(cr);
  }
}

bool Card::visible(const NowPlaying& np) const {
  if (np.track.title.empty() && np.track.artist.empty()) return false;
  return np.playing() || np.paused() || cfg_.idleLast;
}

void Card::drawLine(cairo_t* cr, const std::string& s, double size, double alpha,
                    double x, double width, double yTop, double boxH, int maxLines,
                    bool bold) {
  if (s.empty() || width <= 0 || boxH <= 0) return;

  pwvideo::LabelSpec spec;
  spec.sizePx = size;
  spec.bold = bold;
  spec.widthPx = width;
  spec.maxLines = maxLines;
  // Empty means the caller never set a font: keep the pango default (sans-serif). A comma-separated
  // chain is handed over as-is; pango resolves it, each name through fontconfig.
  if (!cfg_.font.empty()) spec.family = cfg_.font;

  PangoLayout* l = impl_->text_.layout(cr, s, spec);
  const pwvideo::LabelMetrics m = pwvideo::TextRenderer::measure(l);
  const double ty = yTop + (boxH - m.height) / 2.0;

  // Without a plate, stroke first to fake the shadow: two layers from outside in, progressively
  // denser, approximating CSS's three-layer text-shadow.
  if (!hasBg_) {
    const double k = cfg_.height / kBaseSize;
    static const double glowW[] = {7.0, 3.5};
    static const double glowA[] = {0.35, 0.60};
    for (int i = 0; i < 2; ++i)
      pwvideo::TextRenderer::outline(cr, l, x, ty, glowW[i] * k,
                                     pwvideo::Rgba{0, 0, 0, glowA[i]}, 1.5 * k);
  }

  pwvideo::TextRenderer::fill(cr, l, x, ty, pwvideo::Rgba{1, 1, 1, alpha});
}

void Card::rotateInto(const uint32_t* src, int sw, int sh, uint32_t* dst, int dpitch,
                      int side, double radius, double angle) {
  const double cx = side / 2.0, cy = side / 2.0, R = radius;
  const double scx = sw / 2.0, scy = sh / 2.0;
  const double inner = (R - 0.5) * (R - 0.5);

  // Source coordinates and rotation both use 16.16 fixed point: saves a double and a floor per pixel.
  // Measured 43% faster than the per-channel double-precision version (0.754 → 0.431 ms @327px).
  const int32_t ca = static_cast<int32_t>(std::llround(std::cos(angle) * 65536.0));
  const int32_t sa = static_cast<int32_t>(std::llround(std::sin(angle) * 65536.0));
  const int32_t SCX = static_cast<int32_t>(std::llround(scx * 65536.0));
  const int32_t SCY = static_cast<int32_t>(std::llround(scy * 65536.0));

  for (int y = 0; y < side; ++y) {
    const double dy = (y + 0.5) - cy;
    if (std::abs(dy) >= R) continue;
    const double half = std::sqrt(R * R - dy * dy);
    int xa = static_cast<int>(std::ceil(cx - half - 0.5));
    int xb = static_cast<int>(std::floor(cx + half - 0.5));
    if (xa < 0) xa = 0;
    if (xb >= side) xb = side - 1;

    uint32_t* drow = dst + static_cast<size_t>(y) * dpitch;
    const int32_t DY = static_cast<int32_t>(std::llround(dy * 65536.0));
    const int32_t DX0 = static_cast<int32_t>(std::llround(((xa + 0.5) - cx) * 65536.0));
    int32_t sx = static_cast<int32_t>((static_cast<int64_t>(ca) * DX0 +
                                       static_cast<int64_t>(sa) * DY) >> 16) + SCX;
    int32_t sy = static_cast<int32_t>((static_cast<int64_t>(-sa) * DX0 +
                                       static_cast<int64_t>(ca) * DY) >> 16) + SCY;

    for (int x = xa; x <= xb; ++x, sx += ca, sy += -sa) {
      const double px = (x + 0.5) - cx;
      const double d2 = px * px + dy * dy;
      double cov = 1.0;
      if (d2 > inner) {  // compute sqrt only on the 1px ring
        const double d = std::sqrt(d2);
        cov = R + 0.5 - d;
        if (cov <= 0.0) continue;
        if (cov > 1.0) cov = 1.0;
      }
      const int32_t fx = sx - 32768;  // subtract the 0.5 pixel offset
      const int32_t fy = sy - 32768;
      const int ix = fx >> 16;        // arithmetic right shift = floor
      const int iy = fy >> 16;
      const uint32_t tx = static_cast<uint32_t>((fx >> 8) & 0xFF);
      const uint32_t ty = static_cast<uint32_t>((fy >> 8) & 0xFF);
      // The source carries a 1px border and normally lands in [-1, sw-2]; this check is only a fallback.
      if (ix < -1 || iy < -1) continue;
      const uint32_t* r0 = src + static_cast<size_t>(iy) * sw + ix;
      uint32_t out = lerp2(lerp2(r0[0], r0[1], tx), lerp2(r0[sw], r0[sw + 1], tx), ty);

      if (cov < 1.0) {  // round-edge feathering; premultiplied alpha, all four channels scaled together
        const uint32_t c = static_cast<uint32_t>(cov * 256.0);
        const uint32_t lo = (((out & 0x00FF00FF) * c) >> 8) & 0x00FF00FF;
        const uint32_t hi = ((((out >> 8) & 0x00FF00FF) * c) >> 8) & 0x00FF00FF;
        out = lo | (hi << 8);
      }
      drow[x] = out;
    }
  }
}

void Card::drawTexts(cairo_t* cr, const NowPlaying& np, int64_t pos, double yMeta,
                     double textW) {
  const Track& t = np.track;
  const bool none = !hasBg_;
  const double fgDim = none ? kFgDimNone : kFgDimSolid;
  const double lyricIdle = fgDim * (none ? 0.85 : 0.6);

  const double titleBox = 2.0 * 1.3 * m_.titleSize;
  const double subBox = 1.35 * m_.subSize;
  const int nLy = cfg_.lyricLines > 0
                      ? std::min<int>(cfg_.lyricLines, static_cast<int>(t.lyrics.size()))
                      : 0;

  double y = yMeta;
  drawLine(cr, t.title, m_.titleSize, 1.0, m_.pad, textW, y, titleBox, 2, true);
  y += titleBox + m_.metaGap;

  std::string sub = t.artist;
  if (cfg_.showAlbum && !t.album.empty()) {
    if (!sub.empty()) sub += " \u00b7 ";
    sub += t.album;
  }
  drawLine(cr, sub, m_.subSize, fgDim, m_.pad, textW, y, subBox, 1, false);
  y += subBox + m_.metaGap;

  if (nLy > 0) {
    y += kLyricMarginTopPx * (cfg_.height / kBaseSize);
    const int act = activeLyric(t.lyrics, pos);
    const int start = act >= 0 ? act : 0;
    for (int i = 0; i < nLy; ++i) {
      const int idx = start + i;
      if (idx >= static_cast<int>(t.lyrics.size())) break;
      const bool isActive = (idx == act);
      drawLine(cr, t.lyrics[static_cast<size_t>(idx)].text, m_.lyricSize,
               isActive ? 1.0 : lyricIdle, m_.pad, textW, y, 1.35 * m_.lyricSize, 1,
               isActive);
      y += 1.35 * m_.lyricSize + m_.metaGap;
    }
    y -= m_.metaGap;
  }

  if (cfg_.showTime && t.duration > 0) {
    y += m_.gap;
    drawLine(cr, fmtTime(pos) + " / " + fmtTime(t.duration), m_.timeSize, fgDim, m_.pad,
             textW, y, 1.35 * m_.timeSize, 1, false);
  }
}

void Card::render(cairo_t* cr, const NowPlaying& np, cairo_surface_t* cover,
                  int64_t nowMs) {
  const double W = cfg_.width;
  const double H = cfg_.height;
  const double k = H / kBaseSize;
  const Track& t = np.track;

  // One dt for the frame: the cover spin, the ring's counter-rotation and the --viz-fx chain (bar
  // fall, auto-gain) all advance on it, so they cannot drift apart.
  const double dt =
      lastRenderAt_ != 0 ? static_cast<double>(nowMs - lastRenderAt_) / 1000.0 : 0.0;

  // Cover spin: frozen while paused
  if (lastRenderAt_ != 0 && np.playing() && cfg_.spinSeconds > 0) {
    if (dt > 0 && dt < 1.0) {
      const double turn = dt / cfg_.spinSeconds * 2 * kPi;
      spinAngle_ += turn;
      // The bar ring turns the other way and a quarter as fast, so the two read as separate
      // motions rather than one chasing the other. --spin 0 stops both.
      if (cfg_.showViz) vizAngle_ -= turn / kVizSpinRatio;
    }
  }
  lastRenderAt_ = nowMs;

  if (!visible(np)) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    return;
  }

  /* ---------------- compute layout ---------------- */
  const double coverR = m_.coverD / 2.0;
  const double titleBox = 2.0 * 1.3 * m_.titleSize;
  const double subBox = 1.35 * m_.subSize;
  const int nLy = cfg_.lyricLines > 0
                      ? std::min<int>(cfg_.lyricLines, static_cast<int>(t.lyrics.size()))
                      : 0;
  const double lyricBox =
      nLy > 0
          ? nLy * 1.35 * m_.lyricSize + (nLy - 1) * m_.metaGap + kLyricMarginTopPx * k
          : 0.0;
  const bool showTimes = cfg_.showTime && t.duration > 0;
  const double timeBox = showTimes ? 1.35 * m_.timeSize : 0.0;

  double metaH = titleBox + m_.metaGap + subBox;
  if (nLy > 0) metaH += m_.metaGap + lyricBox;
  // The ring adds a band of radius around the cover, so it enters the centred block as height above
  // it; at the bottom the widened cover-to-text gap already reserves the same band.
  const double ringAllow = vizRingR();
  const double total =
      ringAllow + m_.coverD + m_.gap + metaH + (showTimes ? m_.gap + timeBox : 0.0);

  double y = (H - total) / 2.0;
  const double cx = W / 2.0;
  const double cy = y + ringAllow + coverR;
  const double yMeta = y + ringAllow + m_.coverD + m_.gap;
  const double textW = W - 2 * m_.pad;

  int64_t pos = np.position;
  if (np.playing()) pos += static_cast<int64_t>((nowMs - np.sampledAt) * np.rate);
  const int64_t textPos = showTimes ? std::clamp<int64_t>(pos, 0, t.duration) : pos;

  /* ---------------- static layer (incl. text): covers the whole frame in one step ---------------- */
  {
    cairo_surface_t* bgs = staticLayer(np, textPos, k, W, H, cy, yMeta, textW);
    if (bgs) {
      cairo_save(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_surface(cr, bgs, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    }
  }

  const bool dim = !np.playing() && !np.paused();
  if (dim) cairo_push_group(cr);

  /* ---------------- cover ---------------- */
  if (cover) {
    const int sw = cairo_image_surface_get_width(cover);
    const int sh = cairo_image_surface_get_height(cover);
    if (sw > 0 && sh > 0) {
      const int side = static_cast<int>(m_.coverD) + 2;

      // 1) Scale once to coverD (once per track), with a 1px border
      char ak[96];
      std::snprintf(ak, sizeof ak, "%p#%d", static_cast<const void*>(cover), side);
      if (!impl_->artScaled_ || impl_->artScaledKey_ != ak) {
        cairo_surface_t* ss = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
        if (cairo_surface_status(ss) == CAIRO_STATUS_SUCCESS) {
          cairo_t* sc = cairo_create(ss);
          cairo_translate(sc, 1, 1);
          cairo_scale(sc, m_.coverD / sw, m_.coverD / sh);
          cairo_set_source_surface(sc, cover, 0, 0);
          cairo_pattern_set_filter(cairo_get_source(sc), CAIRO_FILTER_BILINEAR);
          cairo_paint(sc);
          cairo_destroy(sc);
          impl_->artScaled_ = pwvideo::SurfacePtr(ss, pwvideo::CairoSurfaceDeleter{});
          impl_->artScaledKey_ = ak;
          impl_->coverLayer_.reset();
          impl_->coverAngle_ = 1e9;
        } else {
          cairo_surface_destroy(ss);
        }
      }

      // 2) True rotation every frame. Quantising by angle saves 0.4ms, but at a 24-second
      //    revolution it updates at only 7.5Hz -- visibly stuttery -- so no saving without smoothing.
      if (impl_->artScaled_) {
        if (!impl_->coverLayer_ ||
            cairo_image_surface_get_width(impl_->coverLayer_.get()) != side ||
            spinAngle_ != impl_->coverAngle_) {
          if (!impl_->coverLayer_ ||
              cairo_image_surface_get_width(impl_->coverLayer_.get()) != side) {
            cairo_surface_t* cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
            if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
              cairo_surface_destroy(cs);
              cs = nullptr;
            }
            impl_->coverLayer_ = cs ? pwvideo::SurfacePtr(cs, pwvideo::CairoSurfaceDeleter{}) : nullptr;
          }
          if (impl_->coverLayer_) {
            cairo_surface_flush(impl_->coverLayer_.get());
            rotateInto(
                reinterpret_cast<const uint32_t*>(
                    cairo_image_surface_get_data(impl_->artScaled_.get())),
                side, side,
                reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(impl_->coverLayer_.get())),
                cairo_image_surface_get_stride(impl_->coverLayer_.get()) / 4, side, m_.coverD / 2.0,
                spinAngle_);
            cairo_surface_mark_dirty(impl_->coverLayer_.get());
            impl_->coverAngle_ = spinAngle_;
          }
        }
        if (impl_->coverLayer_) {
          cairo_set_source_surface(cr, impl_->coverLayer_.get(), cx - coverR - 1, cy - coverR - 1);
          cairo_paint(cr);
        }
      }
    }
  } else {
    // "\u266a" placeholder of .card.no-art
    drawLine(cr, "\u266a", maxOf(18.0 * k, 0.14 * H), kFgDimSolid, cx - coverR,
             m_.coverD, cy - coverR, m_.coverD, 1, false);
  }

  /* ---------------- radial spectrum ---------------- */
  if (cfg_.showViz) {
    fx_.apply(viz_, vizCount_, dt);
    drawSpectrum(cr, cx, cy, coverR);
  }

  /* ---------------- progress arc ---------------- */
  if (cfg_.showProgress && t.duration > 0) {
    const double ringR = coverR + m_.ringGap - m_.ringW / 2.0;
    const int64_t clamped = std::clamp<int64_t>(pos, 0, t.duration);
    const double p = static_cast<double>(clamped) / static_cast<double>(t.duration);
    if (p > 0) {
      cairo_set_line_width(cr, m_.ringW);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
      cairo_arc(cr, cx, cy, ringR, -kPi / 2, -kPi / 2 + p * 2 * kPi);
      cairo_set_source_rgba(cr, 1, 1, 1, kAccentColor);
      cairo_stroke(cr);
    }
  }

  if (dim) {
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.5);
  }
}

}  // namespace oms
