/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// The image processing of EdgeExtend, ExpoGlow, DifScreenColour and DifScreenKey,
// on whole planes (RGBA, premultiplied in / out unless said otherwise).
// Shared by the OFX nodes and the offline tests.

#ifndef APFX_KEYOPS_H
#define APFX_KEYOPS_H

#include "APFXImageOps.h"

namespace APFXKey {

using APFXOps::Plane;
using APFXOps::ParallelFor;

inline float
clamp01(float v)
{
    return std::max( 0.f, std::min(1.f, v) );
}

// ================================================================ EdgeExtend

enum EdgeOutputEnum
{
    eEdgeOutputExtended = 0,   // the source (premultiplied) over its extended edge colours, alpha
    eEdgeOutputEdgesOnly,      // only the extension, premultiplied by where it shows
    eEdgeOutputAreaAffected    // where the extension shows, in all channels
};

struct EdgeExtendParams
{
    float intensity = 1.f;       // 0..1 how much of the extension is applied
    bool erodeIncoming = true;   // erode the alpha before taking the edge
    int width = 2;               // erode width in pixels
    float edgeBlur = 2.f;        // smoothing of the pushed edge colours (pixels)
    bool pullInward = false;     // also replace the eroded band inside the alpha
    float pullInwardBlur = 2.f;  // softness of the inward band
    bool erodeAlpha = false;     // output the eroded alpha
    int output = eEdgeOutputExtended;
};

// For every pixel, the index of the nearest core pixel (exact euclidean, Felzenszwalb
// lower envelope, O(pixels)); -1 when there is no core at all. The nearest core pixel
// lies along the edge normal, so looking colours up through this field pushes the edge
// pixels straight outward (the UV / vector edge extend).
inline void
nearestCore(const std::vector<unsigned char>& core, int w, int h, std::vector<int>& idx, const ParallelFor& pf)
{
    const int INF = 1 << 29;
    std::vector<int> ny( (size_t)w * h );
    // columns: nearest core row in the same column (two sweeps), parallel over column ranges
    pf([&](int x0, int x1) {
        std::vector<int> last(x1 - x0, -INF);
        for (int y = 0; y < h; ++y) {
            const unsigned char* c = &core[(size_t)y * w];
            int* o = &ny[(size_t)y * w];
            for (int x = x0; x < x1; ++x) {
                if (c[x]) {
                    last[x - x0] = y;
                }
                o[x] = last[x - x0];
            }
        }
        std::fill(last.begin(), last.end(), INF);
        for (int y = h - 1; y >= 0; --y) {
            const unsigned char* c = &core[(size_t)y * w];
            int* o = &ny[(size_t)y * w];
            for (int x = x0; x < x1; ++x) {
                if (c[x]) {
                    last[x - x0] = y;
                }
                const int up = last[x - x0];
                if ( (up < INF) && ( (o[x] <= -INF) || (up - y < y - o[x]) ) ) {
                    o[x] = up;
                }
            }
        }
    }, w);
    idx.assign( (size_t)w * h, -1 );
    // rows: lower envelope of the parabolas (x - q)^2 + dy(q)^2
    pf([&](int y0, int y1) {
        std::vector<double> f(w), z(w + 1);
        std::vector<int> v(w);
        for (int y = y0; y < y1; ++y) {
            const int* r = &ny[(size_t)y * w];
            int k = -1;
            for (int q = 0; q < w; ++q) {
                if ( (r[q] <= -INF) || (r[q] >= INF) ) {
                    continue;
                }
                const double dy = r[q] - y;
                f[q] = dy * dy;
                double sx = -1e30;
                while (k >= 0) {
                    const int pq = v[k];
                    sx = ( (f[q] + (double)q * q) - (f[pq] + (double)pq * pq) ) / (2. * (q - pq));
                    if (sx <= z[k]) {
                        --k;
                    } else {
                        break;
                    }
                }
                ++k;
                v[k] = q;
                z[k] = (k == 0) ? -1e30 : sx;
            }
            if (k < 0) {
                continue;
            }
            int* o = &idx[(size_t)y * w];
            int j = 0;
            for (int x = 0; x < w; ++x) {
                while ( (j < k) && (z[j + 1] <= x) ) {
                    ++j;
                }
                const int q = v[j];
                o[x] = r[q] * w + q;
            }
        }
    }, h);
} // nearestCore

// src: RGBA premultiplied, dst: RGBA (see EdgeOutputEnum).
// 1) the edge is taken from the (eroded) incoming alpha: core = eroded alpha >= 0.5
// 2) the core colours (unpremultiplied, smoothed along the edge by edge blur) are pushed
//    outward along the edge normals through the nearest-core field
// 3) the pushed colours are smoothed by edge blur (soft streaks, no seams at corners)
// 4) the source goes over them; pull inward also replaces the eroded band inside
inline void
edgeExtend(const Plane& src, const EdgeExtendParams& p, Plane& dst, const ParallelFor& pf = APFXOps::serialFor)
{
    const int w = src.w, h = src.h;
    const size_t N = (size_t)w * h;
    // 1) alpha, eroded alpha, core
    Plane a0(w, h, 1), ae;
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            a0.d[i] = clamp01(src.d[i * 4 + 3]);
        }
    }, h);
    ae = a0;
    if (p.erodeIncoming && (p.width > 0) ) {
        APFXOps::erode(ae, p.width, pf);
    }
    std::vector<unsigned char> core(N);
    // core colours unpremultiplied, weighted by the eroded alpha (rgb * w, w)
    Plane cc(w, h, 4);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float k = ae.d[i];
            core[i] = (k >= 0.5f);
            float* o = &cc.d[i * 4];
            if (core[i]) {
                const float* s = &src.d[i * 4];
                const float inv = 1.f / std::max(a0.d[i], 1e-6f);
                o[0] = s[0] * inv * k; o[1] = s[1] * inv * k; o[2] = s[2] * inv * k; o[3] = k;
            }
        }
    }, h);
    // smoothing along the edge (normalised, core only) so single noisy edge pixels do not streak
    if (p.edgeBlur >= 0.3f) {
        APFXOps::blur(cc, p.edgeBlur, pf);
    }
    // 2) push outward along the normals
    std::vector<int> idx;
    nearestCore(core, w, h, idx, pf);
    Plane ext(w, h, 4);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const int j = idx[i];
            float* o = &ext.d[i * 4];
            if (j < 0) {
                continue;
            }
            const float* c = &cc.d[(size_t)j * 4];
            const float inv = (c[3] > 1e-6f) ? 1.f / c[3] : 0.f;
            o[0] = c[0] * inv; o[1] = c[1] * inv; o[2] = c[2] * inv; o[3] = 1.f;
        }
    }, h);
    // 3) soft streaks
    if (p.edgeBlur >= 0.3f) {
        APFXOps::blur(ext, p.edgeBlur, pf);
    }
    // 4) inward band
    Plane wIn;
    if (p.pullInward) {
        wIn.resize(w, h, 1);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                wIn.d[i] = clamp01(a0.d[i] - ae.d[i]);
            }
        }, h);
        APFXOps::blur(wIn, p.pullInwardBlur, pf);
    }
    const float I = clamp01(p.intensity);
    dst.resize(w, h, 4);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float* s = &src.d[i * 4];
            const float* e = &ext.d[i * 4];
            const float a = a0.d[i];
            const float has = e[3] > 1e-6f ? 1.f : 0.f;
            float E[3] = { 0.f, 0.f, 0.f };
            if (has > 0.f) {
                E[0] = e[0] / e[3]; E[1] = e[1] / e[3]; E[2] = e[2] / e[3];
            }
            // extension shows under the source: (1 - a), plus the inward band
            float m = (1.f - a) * has;
            float c[3] = { s[0] + E[0] * m, s[1] + E[1] * m, s[2] + E[2] * m };
            float area = m;
            if (p.pullInward && (has > 0.f) ) {
                const float b = wIn.d[i];
                for (int ch = 0; ch < 3; ++ch) {
                    c[ch] += (E[ch] - c[ch]) * b;
                }
                area = std::max(area, b);
            }
            // intensity: mix with the source
            for (int ch = 0; ch < 3; ++ch) {
                c[ch] = s[ch] + (c[ch] - s[ch]) * I;
            }
            area *= I;
            float* o = &dst.d[i * 4];
            switch (p.output) {
            case eEdgeOutputEdgesOnly:
                o[0] = E[0] * area; o[1] = E[1] * area; o[2] = E[2] * area; o[3] = area;
                break;
            case eEdgeOutputAreaAffected:
                o[0] = o[1] = o[2] = o[3] = area;
                break;
            case eEdgeOutputExtended:
            default:
                o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
                o[3] = p.erodeAlpha ? ae.d[i] : s[3];
                break;
            }
        }
    }, h);
} // edgeExtend

// ================================================================ ExpoGlow

struct GlowParams
{
    float falloff = 0.75f;  // weight ratio between successive glow sizes (0 tight .. 1 huge haze)
    float intensity = 1.f;
    float size = 200.f;     // reach in pixels (at the current render scale)
    bool addSource = false;
};

inline int
glowLevels(float size)
{
    return std::max( 1, std::min( 14, (int)std::ceil( std::log2( std::max(2.f, size) ) ) ) );
}

// src: RGBA premultiplied, dst same size (crop / expand is the caller's business)
inline void
expoGlow(const Plane& src, const GlowParams& p, Plane& dst, const ParallelFor& pf = APFXOps::serialFor)
{
    Plane g = APFXOps::expoGlow(src, glowLevels(p.size), p.falloff, pf);
    dst.resize(src.w, src.h, 4);
    const size_t row = (size_t)src.w * 4;
    pf([&](int y0, int y1) {
        for (size_t i = y0 * row; i < y1 * row; ++i) {
            const float v = g.d[i] * p.intensity;
            dst.d[i] = p.addSource ? src.d[i] + v : v;
        }
    }, src.h);
}

// ================================================================ difference screen key

enum ScreenEnum
{
    eScreenGreen = 0,
    eScreenBlue
};

// screen "dominance": how much the screen channel exceeds the weighted others
inline float
dominance(const float* c, int screen, float redWeight, float otherWeight)
{
    if (screen == eScreenBlue) {
        return c[2] - (c[0] * redWeight + c[1] * otherWeight);
    }

    return c[1] - (c[0] * redWeight + c[2] * otherWeight);
}

struct ScreenColourParams
{
    int screen = eScreenGreen;
    float darks[3] = { 0.f, 0.f, 0.f };  // IBK-like "darks": colour treated as black
    float lights[3] = { 1.f, 1.f, 1.f }; // IBK-like "lights": colour treated as white
    float threshold = 0.f;    // minimum screen dominance (after darks / lights) to count as screen
    float softness = 0.05f;   // dominance ramp above the threshold
    int size = 10;            // removes the foreground: grows it by this many pixels
    int erode = 0;            // extra erosion of the remaining screen (edge colour traces)
    float patchBlack = 5.f;   // 0 = foreground left black, higher = black areas patched with the
                              // surrounding screen colours (each step reaches about 4x further,
                              // like stacking more IBK colour nodes); 5 = everything patched
    float smooth = 0.f;       // blur of the final plate (pixels)
    bool showMask = false;    // output the screen mask instead of the plate
};

// src: shot (RGBA, alpha ignored). dst: clean screen plate (RGBA, alpha 1)
inline void
difScreenColour(const Plane& src, const ScreenColourParams& p, Plane& dst, const ParallelFor& pf = APFXOps::serialFor)
{
    const int w = src.w, h = src.h;
    Plane m(w, h, 1);
    const int sc = (p.screen == eScreenBlue) ? 2 : 1;
    float scale[3], off[3];
    for (int k = 0; k < 3; ++k) {
        off[k] = p.darks[k];
        scale[k] = 1.f / std::max(1e-4f, p.lights[k] - p.darks[k]);
    }
    pf([&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* s = src.px(x, y);
                float n[3];
                for (int k = 0; k < 3; ++k) {
                    n[k] = (s[k] - off[k]) * scale[k];
                }
                // screen pixel = screen channel clearly above both other channels
                const float other = (sc == 1) ? std::max(n[0], n[2]) : std::max(n[0], n[1]);
                const float dom = n[sc] - other;
                float v = clamp01( (dom - p.threshold) / std::max(1e-4f, p.softness) );
                if (n[sc] <= 0.f) {
                    v = 0.f; // darker than "darks": not screen
                }
                m.px(x, y)[0] = v;
            }
        }
    }, h);
    if (p.size + p.erode > 0) {
        APFXOps::erode(m, p.size + p.erode, pf);
    }
    dst.resize(w, h, 4);
    if (p.showMask) {
        for (size_t i = 0; i < m.d.size(); ++i) {
            dst.d[i * 4 + 0] = dst.d[i * 4 + 1] = dst.d[i * 4 + 2] = m.d[i];
            dst.d[i * 4 + 3] = 1.f;
        }

        return;
    }
    if (p.patchBlack <= 0.f) {
        // IBK look before patching: screen colour, foreground black
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                for (int k = 0; k < 3; ++k) {
                    dst.d[i * 4 + k] = src.d[i * 4 + k] * m.d[i];
                }
                dst.d[i * 4 + 3] = 1.f;
            }
        }, h);

        return;
    }
    Plane cw(w, h, 4);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float k = m.d[i];
            cw.d[i * 4 + 0] = src.d[i * 4 + 0] * k;
            cw.d[i * 4 + 1] = src.d[i * 4 + 1] * k;
            cw.d[i * 4 + 2] = src.d[i * 4 + 2] * k;
            cw.d[i * 4 + 3] = k;
        }
    }, h);
    // each pyramid level reaches twice as far: patch black 5 -> 21 levels (whole frame)
    const int levels = 1 + (int)std::ceil(p.patchBlack * 4.f);
    Plane plate = APFXOps::pullPush(std::move(cw), levels, pf);
    APFXOps::blur(plate, p.smooth, pf);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            dst.d[i * 4 + 0] = plate.d[i * 3 + 0];
            dst.d[i * 4 + 1] = plate.d[i * 3 + 1];
            dst.d[i * 4 + 2] = plate.d[i * 3 + 2];
            dst.d[i * 4 + 3] = 1.f;
        }
    }, h);
}

inline float
luma(const float* c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

struct ScreenKeyParams
{
    int screen = eScreenGreen;      // green / blue (the plate, or the pick colour, gives the screen colour)
    float redWeight = 0.5f;
    float otherWeight = 0.5f;       // blue weight on green screens, green weight on blue screens
    bool lumMatch = false;          // luminance match: recover foreground brighter than the screen
    float screenRange = 1.f;        // lower: only clearly brighter than the screen counts (less noise)
    bool lumLevelEnable = false;
    float lumLevel = 0.f;           // strength of the luminance match when enabled
    bool autolevels = false;        // harder key on saturated foreground colours
    bool protectYellow = false, protectCyan = false, protectMagenta = false;
    bool screenSubtraction = true;  // remove the screen colour from the foreground (despill)
    bool useBgLuminance = false;    // edges take the brightness of the bg input
    bool useBgChroma = false;       // edges take the colour of the bg input
    float blackClip = 0.f;
    float whiteClip = 1.f;
    bool premultiply = true;
};

// dominance with IBK-like autolevels: saturated colours use the stronger of the other
// two channels (harder key), except the protected hues
inline float
keyDominance(const float* c, const ScreenKeyParams& p)
{
    float d = dominance(c, p.screen, p.redWeight, p.otherWeight);
    if (!p.autolevels) {
        return d;
    }
    const float mx = std::max( c[0], std::max(c[1], c[2]) ), mn = std::min( c[0], std::min(c[1], c[2]) );
    if (mx <= 1e-6f) {
        return d;
    }
    float sat = clamp01( (mx - mn) / mx );
    const bool yellow = (c[0] > c[2]) && (c[1] > c[2]) && (std::min(c[0], c[1]) > 0.5f * mx);
    const bool cyan = (c[1] > c[0]) && (c[2] > c[0]) && (std::min(c[1], c[2]) > 0.5f * mx);
    const bool magenta = (c[0] > c[1]) && (c[2] > c[1]) && (std::min(c[0], c[2]) > 0.5f * mx);
    if ( (p.protectYellow && yellow) || (p.protectCyan && cyan) || (p.protectMagenta && magenta) ) {
        sat = 0.f;
    }
    const int sc = (p.screen == eScreenBlue) ? 2 : 1;
    const float other = (sc == 1) ? std::max(c[0], c[2]) : std::max(c[0], c[1]);
    const float dHard = c[sc] - other;

    return d + (std::min(d, dHard) - d) * sat;
}

// fg: shot (RGBA), plate: clean screen (RGBA), bg: optional background. dst: keyed foreground
inline void
difScreenKey(const Plane& fg, const Plane& plate, const Plane* bg, const ScreenKeyParams& p, Plane& dst,
             const ParallelFor& pf = APFXOps::serialFor)
{
    const int w = fg.w, h = fg.h;
    dst.resize(w, h, 4);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* f = fg.px(x, y);
                const float* c = plate.px(x, y);
                const float df = keyDominance(f, p);
                const float dc = dominance(c, p.screen, p.redWeight, p.otherWeight);
                // alpha = 1 - (screen difference of the shot / screen difference of the plate)
                float a = (dc > 1e-5f) ? 1.f - df / dc : 1.f;
                a = clamp01(a);
                if (p.lumMatch) {
                    // foreground brighter than the screen is foreground too
                    const float lf = luma(f), lp = luma(c);
                    const float thr = lp / std::max(1e-3f, p.screenRange);
                    float al = clamp01( (lf - thr) / std::max(1e-4f, 1.f - std::min(thr, 0.999f)) );
                    al *= p.lumLevelEnable ? p.lumLevel : 1.f;
                    a = std::max(a, clamp01(al));
                }
                if (p.whiteClip > p.blackClip) {
                    a = clamp01( (a - p.blackClip) / (p.whiteClip - p.blackClip) );
                }
                float o[3];
                for (int ch = 0; ch < 3; ++ch) {
                    // shot = fg * a + plate * (1 - a)  ->  fg * a = shot - plate * (1 - a)
                    o[ch] = p.screenSubtraction ? f[ch] - c[ch] * (1.f - a) : f[ch] * a;
                }
                if ( bg && (p.useBgLuminance || p.useBgChroma) ) {
                    // semi-transparent edges pick up the background's light / colour
                    const float* b = bg->px(x, y);
                    const float lb = luma(b);
                    const float e = 2.f * a * (1.f - a);
                    for (int ch = 0; ch < 3; ++ch) {
                        float add = 0.f;
                        if (p.useBgLuminance) {
                            add += lb;
                        }
                        if (p.useBgChroma) {
                            add += b[ch] - lb;
                        }
                        o[ch] += e * add;
                    }
                }
                for (int ch = 0; ch < 3; ++ch) {
                    if (!p.premultiply) {
                        o[ch] = (a > 1e-6f) ? o[ch] / a : 0.f;
                    }
                }
                float* d = dst.px(x, y);
                d[0] = o[0]; d[1] = o[1]; d[2] = o[2]; d[3] = a;
            }
        }
    }, h);
}
} // namespace APFXKey

#endif // APFX_KEYOPS_H
