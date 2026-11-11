/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// USD import / export through the SOLVFX USD bridge (solvusd library, loaded at run time from the
// application folder). Without the bridge, only the built-in .usda writer is available.
// Scenes are converted to SOLVFX scale (1 unit = 1 cm) and Y up.

#ifndef SOLVFX_SOLVUSD_H_ENGINE
#define SOLVFX_SOLVUSD_H_ENGINE

#include <string>
#include <utility>
#include <vector>

#include "SolvScene.h"

namespace Solv3D {

bool usdAvailable(std::string* why = 0);
// reads the stage at a time code (frames); prims go under 'root' ("" = at the top)
bool readUsd(const std::string& file, double timeCode, const std::string& root, Scene* scene, std::string* error);
bool usdTimeRange(const std::string& file, double* start, double* end, double* fps);
// writes one or more frames (time code, scene); one frame = static file
bool writeUsd(const std::string& file, const std::vector<std::pair<double, const Scene*> >& frames, double fps, std::string* error);

} // namespace Solv3D

#endif // SOLVFX_SOLVUSD_H_ENGINE
