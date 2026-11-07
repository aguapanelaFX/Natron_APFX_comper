/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// Shared helpers for the APFX lens / glow / colour nodes (no OFX dependency)

#ifndef APFX_LENSOPS_H
#define APFX_LENSOPS_H

#include <algorithm>
#include <cmath>
#include <vector>

#include "APFXImageOps.h"

namespace APFXLens {

using APFXOps::Plane;
using APFXOps::ParallelFor;

inline float
clampf(float v, float a, float b) { return std::max( a, std::min(b, v) ); }

inline float
smoothstep(float e0, float e1, float x)
{
    if (e1 == e0) {
        return x < e0 ? 0.f : 1.f;
    }
    const float t = clampf( (x - e0) / (e1 - e0), 0.f, 1.f );

    return t * t * (3.f - 2.f * t);
}

inline float
luma709(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// bilinear sample of channel ch (clamped to the edge, or 0 outside when black=true)
inline float
sampleBilinear(const Plane& p, float x, float y, int ch, bool black = false)
{
    // x, y in pixel units, pixel centres at i + 0.5
    float fx = x - 0.5f, fy = y - 0.5f;
    if ( black && ( (fx < -1.f) || (fy < -1.f) || (fx > p.w) || (fy > p.h) ) ) {
        return 0.f;
    }
    fx = clampf(fx, 0.f, (float)(p.w - 1));
    fy = clampf(fy, 0.f, (float)(p.h - 1));
    const int x0 = (int)fx, y0 = (int)fy;
    const int x1 = std::min(x0 + 1, p.w - 1), y1 = std::min(y0 + 1, p.h - 1);
    const float ax = fx - x0, ay = fy - y0;
    const float a = p.px(x0, y0)[ch], b = p.px(x1, y0)[ch], c = p.px(x0, y1)[ch], d = p.px(x1, y1)[ch];

    return (a * (1.f - ax) + b * ax) * (1.f - ay) + (c * (1.f - ax) + d * ax) * ay;
}

// gaussian blur of any size: blurs on a pyramid level, then bilinear back to full size
inline Plane
bigBlur(const Plane& src, float sigma, const ParallelFor& pf = APFXOps::serialFor)
{
    if (sigma <= 0.3f) {
        return src;
    }
    std::vector<Plane> levels;
    const Plane* cur = &src;
    float s = sigma;
    int L = 0;
    while ( (s > 6.f) && (cur->w > 8) && (cur->h > 8) ) {
        levels.push_back( APFXOps::downsample(*cur, pf) );
        cur = &levels.back();
        // the [1 3 3 1] downsample itself blurs by ~0.6 px of the finer level
        s = std::sqrt( std::max(0.f, s * s - 0.36f) ) / 2.f;
        ++L;
    }
    Plane low = *cur;
    APFXOps::blur(low, s, pf);
    if (L == 0) {
        return low;
    }
    Plane out(src.w, src.h, src.c);
    const float k = 1.f / (float)(1 << L);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* o = out.px(0, y);
            const float sy = (y + 0.5f) * low.h / (float)src.h;
            for (int x = 0; x < src.w; ++x) {
                const float sx = (x + 0.5f) * low.w / (float)src.w;
                for (int c = 0; c < src.c; ++c) {
                    o[x * src.c + c] = sampleBilinear(low, sx, sy, c);
                }
            }
        }
    }, src.h);
    (void)k;

    return out;
}

// --------------------------------------------------------------------------- colour

inline void
rgbToHsv(float r, float g, float b, float* h, float* s, float* v)
{
    const float mx = std::max( r, std::max(g, b) ), mn = std::min( r, std::min(g, b) );
    const float d = mx - mn;
    *v = mx;
    *s = mx > 0.f ? d / mx : 0.f;
    if (d <= 0.f) {
        *h = 0.f;

        return;
    }
    float hh;
    if (mx == r) {
        hh = (g - b) / d;
    } else if (mx == g) {
        hh = 2.f + (b - r) / d;
    } else {
        hh = 4.f + (r - g) / d;
    }
    hh /= 6.f;
    if (hh < 0.f) {
        hh += 1.f;
    }
    *h = hh;
}

inline void
hsvToRgb(float h, float s, float v, float* r, float* g, float* b)
{
    if (s <= 0.f) {
        *r = *g = *b = v;

        return;
    }
    h = (h - std::floor(h)) * 6.f;
    const int i = (int)h;
    const float f = h - i, p = v * (1.f - s), q = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
    switch (i % 6) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}
} // namespace APFXLens

#endif // APFX_LENSOPS_H
