/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include "SolvTexFile.h"

#include <cmath>
#include <map>
#include <mutex>

#include <QFileInfo>
#include <QDateTime>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

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
    if ( stbi_is_hdr( path.c_str() ) ) {
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
                     " (supported: png, jpg, tga, bmp, psd, gif, hdr)";

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
