/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX 3D: Alembic (.abc) import / export into the Solv3D scene (Ogawa archives).
// Built only with SOLVFX_HAS_ALEMBIC (MSYS2 package mingw-w64-ucrt-x86_64-alembic).

#ifndef SOLVFX_SOLVALEMBIC_H
#define SOLVFX_SOLVALEMBIC_H

#include <string>

#include "SolvScene.h"

namespace Solv3D {

// true when SOLVFX was built with Alembic support
bool alembicAvailable();

// reads the whole archive at 'seconds' under 'root' (xforms, poly meshes, cameras)
bool readAlembic(const std::string& filename, double seconds, const std::string& root, Scene* scene, std::string* error);

// archive time range in seconds (false if static or unreadable)
bool alembicTimeRange(const std::string& filename, double* start, double* end);

// writes one frame (static archive) of the scene: xforms, meshes, cameras
bool writeAlembic(const std::string& filename, const Scene& scene, std::string* error);
} // namespace Solv3D

#endif // SOLVFX_SOLVALEMBIC_H
