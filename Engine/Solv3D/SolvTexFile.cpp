/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "SolvTexFile.h"

#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <QFileInfo>
#include <QDateTime>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

// .exr textures (tinyexr, BSD), using the zlib code of stb instead of miniz
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x)
#include "stb_image_write.h"
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_USE_OPENMP 0
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
#ifdef _WIN32
#undef near
#undef far
#endif

namespace Solv3D {

namespace {
struct Entry
{
    qint64 mtime = 0;
    std::shared_ptr<TexImage> image;
};

std::mutex g_m;
std::map<std::string, Entry> g_cache;

float
srgbToLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow( (v + 0.055f) / 1.055f, 2.4f );
}
} // anon

std::shared_ptr<TexImage>
loadTextureFile(const std::string& path, std::string* error)
{
    const QFileInfo fi( QString::fromUtf8( path.c_str() ) );
    if ( !fi.exists() ) {
        *error = "texture not found: " + path;

        return std::shared_ptr<TexImage>();
    }
    const qint64 mt = fi.lastModified().toMSecsSinceEpoch();
    std::lock_guard<std::mutex> l(g_m);
    std::map<std::string, Entry>::iterator it = g_cache.find(path);
    if ( ( it != g_cache.end() ) && (it->second.mtime == mt) ) {
        return it->second.image;
    }
    int w = 0, h = 0, n = 0;
    std::shared_ptr<TexImage> t = std::make_shared<TexImage>();
    stbi_set_flip_vertically_on_load(1); // first row = bottom (v = 0)
    std::string lower = path;
    for (char& c : lower) {
        c = (char)std::tolower( (unsigned char)c );
    }
    if ( (lower.size() > 4) && (lower.compare(lower.size() - 4, 4, ".exr") == 0) ) {
        // linear, premultiplied RGBA as stored (first row of the file = top)
        float* d = 0;
        const char* exrErr = 0;
        if ( (LoadEXR(&d, &w, &h, path.c_str(), &exrErr) != TINYEXR_SUCCESS) || !d ) {
            *error = std::string("cannot read texture ") + path + ": " + (exrErr ? exrErr : "unknown error");
            if (exrErr) {
                FreeEXRErrorMessage(exrErr);
            }

            return std::shared_ptr<TexImage>();
        }
        t->data.resize( (size_t)w * h * 4 );
        for (int y = 0; y < h; ++y) {
            std::memcpy( &t->data[(size_t)y * w * 4], d + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4 * sizeof(float) );
        }
        std::free(d);
    } else if ( stbi_is_hdr( path.c_str() ) ) {
        float* d = stbi_loadf(path.c_str(), &w, &h, &n, 4);
        if (!d) {
            *error = std::string("cannot read texture ") + path + ": " + stbi_failure_reason();

            return std::shared_ptr<TexImage>();
        }
        t->data.assign(d, d + (size_t)w * h * 4);
        stbi_image_free(d);
    } else {
        unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!d) {
            *error = std::string("cannot read texture ") + path + ": " + stbi_failure_reason() +
                     " (supported: exr, png, jpg, tga, bmp, psd, gif, hdr)";

            return std::shared_ptr<TexImage>();
        }
        t->data.resize( (size_t)w * h * 4 );
        float lut[256];
        for (int i = 0; i < 256; ++i) {
            lut[i] = srgbToLinear(i / 255.f);
        }
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const float a = d[i * 4 + 3] / 255.f;
            // premultiplied, like the images of the comp
            t->data[i * 4] = lut[d[i * 4]] * a;
            t->data[i * 4 + 1] = lut[d[i * 4 + 1]] * a;
            t->data[i * 4 + 2] = lut[d[i * 4 + 2]] * a;
            t->data[i * 4 + 3] = a;
        }
        stbi_image_free(d);
    }
    t->x1 = 0;
    t->y1 = 0;
    t->w = w;
    t->h = h;
    t->fx1 = 0;
    t->fy1 = 0;
    t->fx2 = w;
    t->fy2 = h;
    t->buildMips();
    Entry& e = g_cache[path];
    e.mtime = mt;
    e.image = t;

    return t;
}
} // namespace Solv3D
