/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#ifndef Engine_SolvfxPathVars_h
#define Engine_SolvfxPathVars_h

// SOLVFX: Houdini-like path variables: $PRJ (project), $SCN (folder of the current comp), $OUT (renders),
// $AST (assets) + user ones. Defined app-wide (Edit > Set Path Variables), not in the comp: a comp only stores
// "$PRJ/plates/a.exr" (plus the full path as a fallback), so it opens on any machine where the variables are set.

#include <map>
#include <string>

#include "Global/Macros.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

namespace SolvfxPathVars {
// name (without $) -> folder. Includes SCN (folder of the current comp, from project) when known.
std::map<std::string, std::string> all(const Project* project);
// the user defined ones (settings), no SCN
std::map<std::string, std::string> userVars();
void setUserVars(const std::map<std::string, std::string>& vars);
// "$PRJ/a/b.exr" -> "D:/jobs/x/a/b.exr". Returns false if str has no known variable at its start.
bool expand(std::string& str, const Project* project);
// true if str starts with $NAME and NAME is not defined
bool hasUnresolved(const std::string& str, const Project* project, std::string* name = 0);
// absolute path inside a variable folder -> "$VAR/rest" (longest match). Returns false if none matches.
bool toVariable(std::string& str, const Project* project);
// SOLVFX project (Maya / Houdini style): $OUT = $PRJ/05_renders, $AST = $PRJ/01_assets unless remapped
extern const char* const kDefaultOutFolder; // "05_renders"
extern const char* const kDefaultAstFolder; // "01_assets"
// Set Project: $PRJ = folder, $OUT / $AST back to their defaults (folders created if missing)
void setProject(const std::string& folder);
// fallback: str = "$VAR/rest" unresolved, absolute = ".../rest": defines VAR for this session from it
bool deriveFromFallback(const std::string& str, const std::string& absolute, const Project* project);
}

NATRON_NAMESPACE_EXIT

#endif // Engine_SolvfxPathVars_h
