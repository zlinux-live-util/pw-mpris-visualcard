#pragma once
// Custom font support: register font files with fontconfig, and check family names before use.
//
// Everything here has to run before the first PangoContext in this process is created. A
// PangoFcFontMap snapshots the family list fontconfig reported when it was built, so a file
// registered afterwards stays invisible even though its family name would resolve at match time.
#include <string>
#include <vector>

namespace oms {

/** Registers one font file, or a whole directory of them, with the process-wide fontconfig config,
 *  so a downloaded .ttf/.otf/.ttc can be used without installing it system-wide. A leading "~/" is
 *  expanded (systemd units do not run through a shell, so they would not expand it).
 *  On failure returns false and fills err with a human-readable reason. */
bool registerFontFile(const std::string& path, std::string& err);

/** The first family name the font file declares, "" if it cannot be read or is a directory. Used to
 *  make --font-file alone sufficient. */
std::string familyOfFontFile(const std::string& path);

/** Whether fontconfig can resolve a family by that name: directly, under one of the matched font's
 *  other (localised) names, or through a substitution rule that points at a real family. Generic
 *  aliases (sans-serif, serif, monospace, ...) always count: they resolve to whatever the system
 *  considers the default, which is exactly what the caller asked for. */
bool familyAvailable(const std::string& family);

/** Splits "A, B" into {"A", "B"}; blank entries are dropped. */
std::vector<std::string> splitFontList(const std::string& spec);

}  // namespace oms
