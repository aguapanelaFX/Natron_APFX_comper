/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// Texture files referenced by geometry files (USD materials): exr, png, jpg, tga, bmp, psd, gif, hdr.
// Loaded once (cache keyed by path and modification time), converted to linear float rgba, mip-mapped.

#ifndef SOLVFX_SOLVTEXFILE_H
#define SOLVFX_SOLVTEXFILE_H

#include <memory>
#include <string>

#include "SolvScanline.h"

namespace Solv3D {

// null when the file cannot be read (error says why). data: 8-bit files are not converted from sRGB
// (roughness, metalness, normal maps ...)
std::shared_ptr<TexImage> loadTextureFile(const std::string& path, std::string* error, bool data = false);

} // namespace Solv3D

#endif // SOLVFX_SOLVTEXFILE_H
