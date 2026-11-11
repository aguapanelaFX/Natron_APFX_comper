/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX 3D: ScanlineRender core (game-engine style, CPU, multithreaded, no platform code).
//  1. raster pass per sample: triangle id + perspective-correct barycentrics + depth (z-buffer)
//  2. shading pass (deferred): texture / projections, lighting, environment reflections
//     (split-sum style: the lat-long environment is prefiltered into mip levels picked by roughness)
//  3. supersampling (sample grid) accumulates rgba -> anti-aliased edges
// Outputs rgba or a data pass (depth, world position, world normal, uv).

#ifndef SOLVFX_SOLVSCANLINE_H
#define SOLVFX_SOLVSCANLINE_H

#include <functional>
#include <memory>
#include <vector>

#include "SolvScene.h"

namespace Solv3D {

// a float RGBA image (rows bottom to top, like Natron); x1, y1 = pixel coordinates of the first pixel
struct TexImage
{
    int x1 = 0, y1 = 0, w = 0, h = 0;
    std::vector<float> data;               // w * h * 4, owned copy (render thread safe)
    // the texture's format in pixels (uv 0..1 maps onto it)
    double fx1 = 0, fy1 = 0, fx2 = 1, fy2 = 1;

    void sample(double px, double py, float* out) const; // bilinear, transparent outside

    // mip-mapped (trilinear) lookup: lod 0 = full resolution; build the mips once before rendering
    void buildMips();
    void sampleLod(double px, double py, double lod, float* out) const;
    std::vector<TexImage> mips;            // level 1, 2, ... (half size each)
};

// reconstruction filter applied to the samples (anti-aliasing of edges)
enum class ScanlineFilter { Box = 0, Triangle, Gaussian, Mitchell, BlackmanHarris };

enum class ScanlineOutput { RGBA = 0, Depth, Position, Normal, UV };

struct ScanlineSettings
{
    int width = 0, height = 0;               // output size in pixels (render window = whole format)
    int originX = 0, originY = 0;            // pixel coordinate of the first output pixel
    double formatX1 = 0, formatY1 = 0, formatX2 = 1, formatY2 = 1; // format in pixels (projection maps onto it)
    double pixelAspect = 1.;
    int samples = 16;                         // per pixel, stratified square grid (1 .. 64)
    ScanlineFilter filter = ScanlineFilter::BlackmanHarris;
    double filterWidth = 2.;                  // filter diameter in pixels (1 = within the pixel)
    bool textureFiltering = true;             // mip-mapped textures (no shimmering when small)
    ScanlineOutput output = ScanlineOutput::RGBA;
    double ambient = 0.15;                    // headlight shading floor for lit materials
    bool transparency = true;                 // keep the texture alpha (off: every surface is opaque)
    bool zBuffer = true;                      // off: no depth test, later prims draw over earlier ones
    // 0 render camera, 1 perspective, 2 orthographic (forced on the camera), 3 uv (unwrap: the uv square
    // fills the format), 4 spherical (lat-long around the camera position)
    int projectionMode = 0;
    int threads = 0;                          // 0 = hardware concurrency
};

typedef std::function<const TexImage*(const std::shared_ptr<void>&)> TextureResolver;

// renders 'scene' through the camera prim 'cameraPath' (or a default camera when empty / missing)
// into out (width * height * 4, premultiplied rgba)
void scanlineRender(const Scene& scene,
                    const std::string& cameraPath,
                    const ScanlineSettings& s,
                    const TextureResolver& textures,
                    const TexImage* environment,
                    std::vector<float>* out);
} // namespace Solv3D

#endif // SOLVFX_SOLVSCANLINE_H
