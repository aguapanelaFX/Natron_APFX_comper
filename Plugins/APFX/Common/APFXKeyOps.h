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
    eEdgeOutputExtended = 0,   // extended colours (unpremultiplied), alpha
    eEdgeOutputEdgesOnly,      // only the changed band, premultiplied by its weight
    eEdgeOutputAreaAffected    // the weight of the change, in all channels
};

struct EdgeExtendParams
{
    float intensity = 1.f;       // 0..1 how much the extended colour replaces the edge colour
    bool erodeIncoming = true;   // erode the alpha before extending
    int width = 2;               // erode width in pixels
    float edgeBlur = 1.f;        // softness of the transition (pixels)
    bool pullInward = false;     // also replace the eroded band inside the alpha
    float pullInwardBlur = 2.f;  // softness of the inward band
    bool erodeAlpha = false;     // output the eroded alpha
    int output = eEdgeOutputExtended;
};

// src: RGBA premultiplied. dst: RGBA (unpremultiplied colour for eEdgeOutputExtended).
inline void
edgeExtend(const Plane& src, const EdgeExtendParams& p, Plane& dst, const ParallelFor& pf = APFXOps::serialFor)
{
    const int w = src.w, h = src.h;
    // 1) alpha and eroded alpha
    Plane a0(w, h, 1), ae(w, h, 1);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            a0.d[i] = clamp01(src.d[i * 4 + 3]);
        }
    }, h);
    ae.d = a0.d;
    if (p.erodeIncoming && p.width > 0) {
        APFXOps::erode(ae, p.width, pf);
    }
    // 2) pull-push from the (eroded) core: colour * weight, weight
    Plane cw(w, h, 4);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* s = src.px(x, y);
                const float a = a0.px(x, y)[0];
                const float k = ae.px(x, y)[0];
                float* o = cw.px(x, y);
                const float inv = (a > 1e-6f) ? 1.f / a : 0.f;
                // weight: eroded alpha, sharpened so only solid core colours count fully
                const float wgt = k * k;
                o[0] = s[0] * inv * wgt; o[1] = s[1] * inv * wgt; o[2] = s[2] * inv * wgt;
                o[3] = wgt;
            }
        }
    }, h);
    Plane ext = APFXOps::pullPush(std::move(cw), 32, pf);
    // 3) where to use the extended colour: outside / soft edges, plus (pull inward) the eroded band
    Plane wOut(w, h, 1), wIn(w, h, 1);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            wOut.d[i] = 1.f - a0.d[i];
            wIn.d[i] = p.pullInward ? clamp01(a0.d[i] - ae.d[i]) : 0.f;
        }
    }, h);
    APFXOps::blur(wOut, p.edgeBlur, pf);
    if (p.pullInward) {
        APFXOps::blur(wIn, p.pullInwardBlur, pf);
    }
    dst.resize(w, h, 4);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* s = src.px(x, y);
                const float* e = ext.px(x, y);
                const float a = a0.px(x, y)[0];
                // keep fully solid pixels untouched unless pulled inward
                float m = wOut.px(x, y)[0];
                if (a <= 0.f) {
                    m = 1.f;
                }
                m = std::max( m, wIn.px(x, y)[0] );
                m = clamp01(m * p.intensity);
                const float inv = (a > 1e-6f) ? 1.f / a : 0.f;
                float c[3];
                for (int ch = 0; ch < 3; ++ch) {
                    const float own = s[ch] * inv;
                    c[ch] = own + (e[ch] - own) * m;
                }
                const float alphaOut = p.erodeAlpha ? ae.px(x, y)[0] : s[3];
                float* o = dst.px(x, y);
                switch (p.output) {
                case eEdgeOutputEdgesOnly:
                    o[0] = c[0] * m; o[1] = c[1] * m; o[2] = c[2] * m; o[3] = m;
                    break;
                case eEdgeOutputAreaAffected:
                    o[0] = o[1] = o[2] = o[3] = m;
                    break;
                case eEdgeOutputExtended:
                default:
                    o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = alphaOut;
                    break;
                }
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
