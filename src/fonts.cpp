// Custom font support: registering font files with fontconfig, and checking family names.
//
// Ordering constraint, see fonts.hpp: this must run before the first PangoContext exists in the
// process, otherwise the font map has already snapshotted the old family list.
//
// Family availability is checked through fontconfig rather than by listing pango families, because
// the answer has to include localised names ("WenQuanYi Zen Hei" appears as a Chinese name too),
// metric-compatible substitutions (Palatino is answered with Palatino Linotype) and generic
// families. A name counts as available when the font fontconfig matches for it carries that name,
// or when that font differs from the one an impossible name resolves to; a name nothing matches
// quietly lands on the system default, which is exactly the case worth warning about.
#include "fonts.hpp"

// fcfreetype.h needs the Fc* types, so fontconfig.h has to come first despite the alphabetical order
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace oms {
namespace {

/** Expands a leading "~". A systemd unit is not run through a shell, so "~" reaches us literally. */
std::string expandTilde(const std::string& p) {
  const char* home = std::getenv("HOME");
  if (!home) return p;
  if (p == "~") return home;
  if (p.rfind("~/", 0) == 0) return std::string(home) + p.substr(1);
  return p;
}

/** ASCII case-insensitive compare; family names are compared this way because fontconfig matches
 *  them case-insensitively. */
bool iequals(const std::string& a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i]; ++i) {
    const int ca = std::tolower(static_cast<unsigned char>(a[i]));
    const int cb = std::tolower(static_cast<unsigned char>(b[i]));
    if (ca != cb) return false;
  }
  return i == a.size() && b[i] == '\0';
}

/** A family name goes straight into pango, which expects UTF-8. Font files are not always sane. */
bool isUtf8(const char* s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  for (; *p; ++p) {
    int extra;
    unsigned int cp;
    if (*p < 0x80) continue;
    else if ((*p & 0xE0) == 0xC0) { extra = 1; cp = *p & 0x1F; }
    else if ((*p & 0xF0) == 0xE0) { extra = 2; cp = *p & 0x0F; }
    else if ((*p & 0xF8) == 0xF0) { extra = 3; cp = *p & 0x07; }
    else return false;
    for (int i = 0; i < extra; ++i)
      if ((p[1 + i] & 0xC0) != 0x80) return false;
    p += extra;
    // Surrogates and overlongs would be rejected by glib, but they are not the concern here
    (void)cp;
  }
  return true;
}

bool statPath(const std::string& path, bool& isDir) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  isDir = S_ISDIR(st.st_mode);
  return true;
}

/** fontconfig's generic families. They always resolve, so there is nothing to look up: the point of
 *  asking for sans-serif is to get whatever the system considers the default. */
bool isGenericAlias(const std::string& f) {
  static const char* kGeneric[] = {
      "sans-serif", "serif",   "monospace", "cursive",  "fantasy", "system-ui",
      "system",     "math",    "emoji",     "ui-serif", "ui-rounded", "ui-sans-serif",
  };
  for (const char* g : kGeneric)
    if (f == g) return true;
  return false;
}

/** A family name nothing can match, used as the "unknown name" baseline. */
constexpr const char* kNoSuchFamily = "__pw-mpris-visualcard no such font__";

/** The family names of the font fontconfig picks for a single-family pattern, plus its file.
 *  Returns false only when no match could be made at all; a missing FC_FILE is not a failure (a
 *  font can be registered from memory), it just leaves `file` empty. */
bool matchFamilies(const std::string& family, std::vector<std::string>& fams, std::string& file) {
  fams.clear();
  file.clear();
  FcPattern* want = FcPatternCreate();
  if (!want) return false;
  FcPatternAddString(want, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
  FcConfigSubstitute(nullptr, want, FcMatchPattern);
  FcDefaultSubstitute(want);
  FcResult res = FcResultNoMatch;
  FcPattern* got = FcFontMatch(nullptr, want, &res);
  bool ok = false;
  if (got) {
    // Every family name, not just index 0: a font lists its localised names alongside the English
    // one, and index 0 is not necessarily the name that was asked for.
    for (int i = 0;; ++i) {
      FcChar8* f = nullptr;
      if (FcPatternGetString(got, FC_FAMILY, i, &f) != FcResultMatch || !f) break;
      fams.emplace_back(reinterpret_cast<const char*>(f));
    }
    FcChar8* p = nullptr;
    if (FcPatternGetString(got, FC_FILE, 0, &p) == FcResultMatch && p)
      file.assign(reinterpret_cast<const char*>(p));
    ok = !fams.empty();
    FcPatternDestroy(got);
  }
  FcPatternDestroy(want);
  return ok;
}

}  // namespace

bool registerFontFile(const std::string& pathIn, std::string& err) {
  const std::string path = expandTilde(pathIn);
  bool isDir = false;
  if (!statPath(path, isDir)) {
    err = "font file not found: " + pathIn;
    return false;
  }
  // FcConfigGetCurrent() is null until fontconfig initialises itself; it is usually pango that does
  // it, and pango has not run yet at this point.
  if (!FcConfigGetCurrent()) FcInit();
  FcConfig* cfg = FcConfigGetCurrent();
  if (!cfg) {
    err = "fontconfig has no current config, cannot register " + path;
    return false;
  }
  const FcChar8* p = reinterpret_cast<const FcChar8*>(path.c_str());
  const FcBool ok = isDir ? FcConfigAppFontAddDir(cfg, p) : FcConfigAppFontAddFile(cfg, p);
  if (!ok) {
    err = "fontconfig refused the font (unreadable or not a font): " + path;
    return false;
  }
  return true;
}

std::string familyOfFontFile(const std::string& pathIn) {
  const std::string path = expandTilde(pathIn);
  bool isDir = false;
  if (!statPath(path, isDir) || isDir) return {};  // a directory has no single family name
  // Face 0 of a collection. Index 0 of FC_FAMILY is the family in English for the fonts that ship
  // more than one localised name; a font whose own naming differs is still overridable via --font.
  FcPattern* pat =
      FcFreeTypeQuery(reinterpret_cast<const FcChar8*>(path.c_str()), 0, nullptr, nullptr);
  if (!pat) return {};
  std::string out;
  FcChar8* fam = nullptr;
  if (FcPatternGetString(pat, FC_FAMILY, 0, &fam) == FcResultMatch && fam &&
      isUtf8(reinterpret_cast<const char*>(fam)))
    out = reinterpret_cast<const char*>(fam);
  FcPatternDestroy(pat);
  return out;
}

bool familyAvailable(const std::string& family) {
  if (family.empty()) return false;
  if (isGenericAlias(family)) return true;
  if (!FcInit()) return true;  // cannot tell; let fontconfig decide at match time

  std::vector<std::string> fams, missFams;
  std::string file, missFile;
  // A failed match means there is no font to match at all (an empty font set), which is exactly
  // the "nothing here is installed" answer this function is asked for.
  if (!matchFamilies(family, fams, file)) return false;
  for (const std::string& f : fams)
    if (iequals(f, family.c_str())) return true;

  // Not one of the matched font's own names: the request may still be resolved through a
  // substitution rule that points at a family which does exist (metric-compatible aliases). A name
  // nothing matches quietly lands on the default font, so compare with what a name that cannot
  // exist resolves to and treat any difference as a real resolution.
  if (!matchFamilies(kNoSuchFamily, missFams, missFile)) return false;
  return fams != missFams || file != missFile;
}

std::vector<std::string> splitFontList(const std::string& spec) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : spec) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  for (std::string& s : out) {  // trim the padding of "A, B"
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    s = s.substr(b, e - b);
  }
  out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& s) { return s.empty(); }),
            out.end());
  return out;
}

}  // namespace oms
