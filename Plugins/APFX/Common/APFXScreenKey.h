/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX ScreenKey: alpha extraction from a coloured screen (green, blue or any colour),
// made for compressed and generated footage. Matte only: the colours are never changed.
//
//  1. input repair (measurement only): chroma rebuilt at full resolution by a guided filter on the
//     luma (blocky 4:2:0 / 4:2:2 colour edges), screen noise smoothed on near-screen pixels
//  2. clean plate: the screen without the subject (auto from the shot, a plate input, or flat)
//  3. colour difference against the plate, pixel by pixel. The difference axis is the chroma of the
//     screen colour (grey-invariant): for a green screen it is G - (R + B) / 2, for any other colour
//     the same thing rotated; 'balance' tilts it along the other chroma axis
//  4. edge matte (screen gain, clips) + optional core matte (harder, shrunk), combined
//  5. halo flatten: in a band just outside the solid subject, smooth (low-detail) partial alpha that is
//     brighter (or darker) than the plate is removed; textured detail (hair) is kept
//  6. edge snap (guided filter on the repaired luma), shrink / grow (sub-pixel), soften
//  7. inside / outside masks

#ifndef APFX_SCREENKEY_H
#define APFX_SCREENKEY_H

#include <algorithm>
#include <cmath>
#include <vector>

#include "APFXImageOps.h"

namespace APFXSK {

using APFXOps::ParallelFor;
using APFXOps::Plane;

enum Screen { eScreenGreen = 0, eScreenBlue, eScreenCustom };
enum PlateMode { ePlateAuto = 0, ePlateInput, ePlateFlat };
enum Ring { eRingBright = 0, eRingDark, eRingBoth };

struct Params
{
    int screen = eScreenGreen;
    bool autoColour = true;
    float colour[3] = { 0.1f, 0.6f, 0.15f };   // screen colour (custom / when auto is off)
    int plateMode = ePlateAuto;
    int plateSize = 10;          // subject grown by this many pixels before the plate is filled
    float plateSmooth = 4.f;
    bool chromaRebuild = true;
    int chromaRadius = 1;
    float denoise = 0.5f;        // screen noise smoothing amount
    float denoiseSize = 2.f;
    float gain = 1.05f;          // screen gain
    float balance = 0.f;         // -1..1 tilt along the other chroma axis
    float clipBlack = 0.f, clipWhite = 1.f;
    bool core = false;
    float coreGain = 1.5f;
    float coreClipWhite = 0.7f;
    int coreShrink = 2;
    float halo = 0.f;            // halo flatten amount
    int haloWidth = 8;
    float haloDetail = 2.f;      // detail size kept (pixels)
    int ring = eRingBright;
    float snap = 0.f;            // edge snap amount
    int snapRadius = 2;
    float grow = 0.f;            // shrink (< 0) / grow (> 0), pixels, fractional
    float soften = 0.f;
};

struct Result
{
    Plane alpha;     // 1 channel
    Plane plate;     // 3 channels (clean plate)
    Plane removed;   // 1 channel: alpha removed by halo flatten
    Plane raw;       // 1 channel: edge matte before halo / refine (screen matte)
};

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

inline float luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// box filter (radius r) of every channel of a plane, in place
inline void
boxPlane(Plane& p, int r, const ParallelFor& pf)
{
    if (r <= 0) {
        return;
    }
    Plane tmp(p.w, p.h, p.c);
    pf([&](int y0, int y1) { APFXOps::boxRows(p, tmp, r, y0, y1); }, p.h);
    const int elems = p.w * p.c;
    const int chunks = (elems + 63) / 64;
    pf([&](int c0, int c1) { APFXOps::boxCols( tmp, p, r, c0 * 64, std::min(elems, c1 * 64) ); }, chunks);
}

// dilation (max filter) of a 1-channel plane
inline void
dilate(Plane& p, int r, const ParallelFor& pf)
{
    if (r <= 0) {
        return;
    }
    for (float& v : p.d) { v = 1.f - v; }
    APFXOps::erode(p, r, pf);
    for (float& v : p.d) { v = 1.f - v; }
}

// difference axis of a screen colour: n . c = screen dominance of c (0 on greys)
struct Axis
{
    float n[3];
    void build(const float* s, float balance)
    {
        const float m = (s[0] + s[1] + s[2]) / 3.f;
        float u[3] = { s[0] - m, s[1] - m, s[2] - m };
        float uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
        if (uu < 1e-10f) {               // grey screen: fall back to green
            u[0] = -0.5f; u[1] = 1.f; u[2] = -0.5f;
            uu = 1.5f;
        }
        const float k = std::sqrt(1.5f / uu);   // same scale as G - (R + B) / 2
        for (int i = 0; i < 3; ++i) { u[i] *= k; }
        // other chroma axis: grey x u, scaled like (R - B)
        float v[3] = { u[1] - u[2], u[2] - u[0], u[0] - u[1] };   // (1,1,1) x u
        const float vv = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        const float kv = vv > 1e-10f ? std::sqrt(2.f / vv) : 0.f;
        for (int i = 0; i < 3; ++i) {
            n[i] = u[i] - 0.5f * balance * v[i] * kv;
        }
    }
    float dom(const float* c) const { return n[0] * c[0] + n[1] * c[1] + n[2] * c[2]; }
};

// average colour of clearly screen-coloured pixels (green / blue presets)
inline void
estimateScreen(const Plane& m, int screen, float* out)
{
    const int sc = (screen == eScreenBlue) ? 2 : 1;
    const int o1 = (sc == 1) ? 0 : 0, o2 = (sc == 1) ? 2 : 1;
    double s[3] = { 0., 0., 0. }, w = 0.;
    const size_t n = (size_t)m.w * m.h;
    const size_t step = std::max<size_t>(1, n / 400000);   // enough samples, bounded cost
    for (size_t i = 0; i < n; i += step) {
        const float* c = &m.d[i * m.c];
        const float other = std::max(c[o1], c[o2]);
        if ( (c[sc] > 0.02f) && (c[sc] > 1.25f * other) ) {
            const float k = c[sc] - other;   // stronger screen pixels count more
            for (int j = 0; j < 3; ++j) { s[j] += k * c[j]; }
            w += k;
        }
    }
    if (w > 0.) {
        for (int j = 0; j < 3; ++j) { out[j] = (float)(s[j] / w); }
    } else if (sc == 1) {
        out[0] = 0.1f; out[1] = 0.6f; out[2] = 0.15f;
    } else {
        out[0] = 0.08f; out[1] = 0.2f; out[2] = 0.7f;
    }
}

// guided filter of chroma by luma: rebuilds colour edges lost to chroma subsampling
inline void
chromaRebuild(Plane& m, int r, const ParallelFor& pf)
{
    const int w = m.w, h = m.h;
    Plane s(w, h, 6);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float* c = &m.d[i * 3];
            const float Y = luma(c), cb = c[2] - Y, cr = c[0] - Y;
            float* o = &s.d[i * 6];
            o[0] = Y; o[1] = cb; o[2] = cr; o[3] = Y * cb; o[4] = Y * cr; o[5] = Y * Y;
        }
    }, h);
    Plane orig = s;
    boxPlane(s, r, pf);
    // regulariser: edges below this luma contrast are smoothed (compression noise)
    const float eps = 1e-4f;
    Plane ab(w, h, 4);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float* q = &s.d[i * 6];
            const float var = std::max(0.f, q[5] - q[0] * q[0]);
            const float a1 = (q[3] - q[0] * q[1]) / (var + eps), a2 = (q[4] - q[0] * q[2]) / (var + eps);
            float* o = &ab.d[i * 4];
            o[0] = a1; o[1] = q[1] - a1 * q[0]; o[2] = a2; o[3] = q[2] - a2 * q[0];
        }
    }, h);
    boxPlane(ab, r, pf);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float Y = orig.d[i * 6];
            const float* q = &ab.d[i * 4];
            const float cb = q[0] * Y + q[1], cr = q[2] * Y + q[3];
            float* c = &m.d[i * 3];
            // back to rgb with the same luma
            c[0] = Y + cr;
            c[2] = Y + cb;
            c[1] = (Y - 0.2126f * c[0] - 0.0722f * c[2]) / 0.7152f;
        }
    }, h);
}

// guided filter of a 1-channel plane p by a gray guide I (edge snap)
inline void
guidedAlpha(const Plane& I, Plane& p, int r, float eps, const ParallelFor& pf)
{
    const int w = p.w, h = p.h;
    Plane s(w, h, 4);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float g = I.d[i], a = p.d[i];
            float* o = &s.d[i * 4];
            o[0] = g; o[1] = a; o[2] = g * a; o[3] = g * g;
        }
    }, h);
    boxPlane(s, r, pf);
    Plane ab(w, h, 2);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float* q = &s.d[i * 4];
            const float var = std::max(0.f, q[3] - q[0] * q[0]);
            const float A = (q[2] - q[0] * q[1]) / (var + eps);
            ab.d[i * 2] = A;
            ab.d[i * 2 + 1] = q[1] - A * q[0];
        }
    }, h);
    boxPlane(ab, r, pf);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            p.d[i] = clamp01(ab.d[i * 2] * I.d[i] + ab.d[i * 2 + 1]);
        }
    }, h);
}

// src: shot rgb(a) (any channel count >= 3). plateIn: plate input (rgb(a)) or NULL.
inline void
screenKey(const Plane& src, const Plane* plateIn, const Params& p, Result& res, const ParallelFor& pf)
{
    const int w = src.w, h = src.h;
    const size_t n = (size_t)w * h;
    // ---- 1. measurement image (rgb)
    Plane m(w, h, 3);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            for (int k = 0; k < 3; ++k) { m.d[i * 3 + k] = src.d[i * src.c + k]; }
        }
    }, h);
    if ( p.chromaRebuild && (p.chromaRadius > 0) ) {
        chromaRebuild(m, p.chromaRadius, pf);
    }
    // ---- screen colour + axis
    float S[3] = { p.colour[0], p.colour[1], p.colour[2] };
    if ( (p.screen != eScreenCustom) && p.autoColour ) {
        estimateScreen(m, p.screen, S);
    }
    Axis ax;
    ax.build(S, p.balance);
    const float dS = std::max(1e-5f, ax.dom(S));
    // ---- 2. clean plate
    res.plate.resize(w, h, 3);
    if ( (p.plateMode == ePlateInput) && plateIn && (plateIn->w == w) && (plateIn->h == h) ) {
        for (size_t i = 0; i < n; ++i) {
            for (int k = 0; k < 3; ++k) { res.plate.d[i * 3 + k] = plateIn->d[i * plateIn->c + k]; }
        }
    } else if (p.plateMode == ePlateFlat) {
        for (size_t i = 0; i < n; ++i) {
            for (int k = 0; k < 3; ++k) { res.plate.d[i * 3 + k] = S[k]; }
        }
    } else {
        Plane sm(w, h, 1);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                const float r = ax.dom(&m.d[i * 3]) / dS;
                sm.d[i] = clamp01( (r - 0.6f) / 0.3f );   // clearly screen
            }
        }, h);
        if (p.plateSize > 0) {
            APFXOps::erode(sm, p.plateSize, pf);
        }
        Plane cw(w, h, 4);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                const float k = sm.d[i];
                for (int c = 0; c < 3; ++c) { cw.d[i * 4 + c] = m.d[i * 3 + c] * k; }
                cw.d[i * 4 + 3] = k;
            }
        }, h);
        double wsum = 0.;
        for (size_t i = 0; i < n; i += 97) { wsum += sm.d[i]; }
        if (wsum < 1.) {
            for (size_t i = 0; i < n; ++i) {
                for (int k = 0; k < 3; ++k) { res.plate.d[i * 3 + k] = S[k]; }
            }
        } else {
            res.plate = APFXOps::pullPush(std::move(cw), 32, pf);
        }
        APFXOps::blur(res.plate, p.plateSmooth, pf);
    }
    // ---- 3. screen ratio (dominance / plate dominance), denoised near the screen
    Plane ratio(w, h, 1);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            float dp = ax.dom(&res.plate.d[i * 3]);
            if (dp < 0.15f * dS) {
                dp = dS;   // plate not screen-like here: use the screen colour
            }
            ratio.d[i] = ax.dom(&m.d[i * 3]) / dp;
        }
    }, h);
    if ( (p.denoise > 0.f) && (p.denoiseSize >= 0.5f) ) {
        Plane rs = ratio;
        APFXOps::blur(rs, p.denoiseSize, pf);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                // only where the smoothed ratio says "screen" (0.8 .. 0.95 ramp): edges untouched
                const float k = p.denoise * clamp01( (rs.d[i] - 0.8f) / 0.15f );
                ratio.d[i] += (rs.d[i] - ratio.d[i]) * k;
            }
        }, h);
    }
    // ---- 4. edge + core mattes
    Plane& a = res.alpha;
    a.resize(w, h, 1);
    const float cbk = p.clipBlack, cwh = std::max(p.clipBlack + 1e-4f, p.clipWhite);
    pf([&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
            const float raw = clamp01(1.f - p.gain * ratio.d[i]);
            a.d[i] = clamp01( (raw - cbk) / (cwh - cbk) );
        }
    }, h);
    if (p.core) {
        Plane c(w, h, 1);
        const float cw2 = std::max(1e-3f, p.coreClipWhite);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                c.d[i] = clamp01( clamp01(1.f - p.coreGain * ratio.d[i]) / cw2 );
            }
        }, h);
        APFXOps::erode(c, p.coreShrink, pf);
        for (size_t i = 0; i < n; ++i) { a.d[i] = std::max(a.d[i], c.d[i]); }
    }
    res.raw = a;
    // ---- 5. halo flatten
    // A real edge pixel is a mix of the subject colour F and the plate P: its brightness lies between
    // them (Y = a YF + (1 - a) YP). A halo pixel is lighter (or darker) than that mix: the screen was
    // blended toward a light (dark) colour W. With C = (1 - h)(a F + (1 - a) P) + h W, the colour
    // difference measures am = a + h (1 - a) and the luma gives a second equation: both are solved for
    // the true a. Hair and real soft edges (true mixes) do not move.
    res.removed.resize(w, h, 1);
    if ( (p.halo > 0.f) && (p.haloWidth > 0) ) {
        Plane solid(w, h, 1);
        for (size_t i = 0; i < n; ++i) { solid.d[i] = a.d[i] > 0.95f ? 1.f : 0.f; }
        Plane grown = solid;
        dilate(grown, p.haloWidth, pf);
        // only the outer border: also within reach of clear screen (holes inside the subject are not halos)
        Plane scr(w, h, 1);
        for (size_t i = 0; i < n; ++i) { scr.d[i] = a.d[i] < 0.05f ? 1.f : 0.f; }
        dilate(scr, p.haloWidth, pf);
        for (size_t i = 0; i < n; ++i) { grown.d[i] *= scr.d[i]; }
        // subject colour pushed out of the solid area
        Plane cw(w, h, 4);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                const float k = solid.d[i];
                for (int c = 0; c < 3; ++c) { cw.d[i * 4 + c] = m.d[i * 3 + c] * k; }
                cw.d[i * 4 + 3] = k;
            }
        }, h);
        Plane F = APFXOps::pullPush(std::move(cw), 32, pf);
        // brightness excess over the mix (smoothed at the detail size: noise is not a halo)
        Plane ex(w, h, 1);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                const float am = a.d[i];
                const float yf = luma(&F.d[i * 3]), yp = luma(&res.plate.d[i * 3]);
                ex.d[i] = luma(&m.d[i * 3]) - (am * yf + (1.f - am) * yp);
            }
        }, h);
        APFXOps::blur(ex, std::max(0.f, p.haloDetail * 0.5f), pf);
        // halo colour brightness: the brightest (darkest) pixel of the ring around, a little beyond
        Plane yb(w, h, 1), yd(w, h, 1);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                const float band = grown.d[i] * (1.f - solid.d[i]);
                const float y = luma(&m.d[i * 3]);
                yb.d[i] = band > 0.f ? y : 0.f;
                yd.d[i] = band > 0.f ? 1.f - std::min(1.f, y) : 0.f;
            }
        }, h);
        dilate(yb, p.haloWidth, pf);
        dilate(yd, p.haloWidth, pf);
        const float tol = 0.01f;
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                const float band = grown.d[i] * (1.f - solid.d[i]);
                const float am = a.d[i];
                if ( (band <= 0.f) || (am <= 0.f) ) {
                    continue;
                }
                const float e = ex.d[i];
                const bool bright = (e > tol) && (p.ring != eRingDark);
                const bool dark = (e < -tol) && (p.ring != eRingBright);
                if (!bright && !dark) {
                    continue;
                }
                const float yf = luma(&F.d[i * 3]), yp = luma(&res.plate.d[i * 3]);
                const float y = luma(&m.d[i * 3]);
                // halo colour: lighter than everything around (bright) or black (dark)
                const float yw = bright ? std::max( y, yb.d[i] ) * 1.02f + 1e-3f : std::max( 0.f, (1.f - yd.d[i]) * 0.98f );
                // f(a) = (1 - h) Ymix(a) + h Yw - Y, h = (am - a) / (1 - a); f(am) = -e, f(0) has the other sign
                auto f = [&](float aa) {
                    const float hh = (aa < 0.999f) ? (am - aa) / (1.f - aa) : 0.f;
                    const float ym = aa * yf + (1.f - aa) * yp;

                    return (1.f - hh) * ym + hh * yw - y;
                };
                float lo = 0.f, hi = am;
                const float flo = f(lo);
                if ( (flo > 0.f) != bright ) {
                    lo = 0.f;    // whole alpha explained by the halo
                    hi = 0.f;
                } else {
                    for (int it = 0; it < 14; ++it) {
                        const float mid = 0.5f * (lo + hi);
                        const float fm = f(mid);
                        if ( (fm > 0.f) == bright ) {
                            lo = mid;
                        } else {
                            hi = mid;
                        }
                    }
                }
                const float solved = 0.5f * (lo + hi);
                const float k = clamp01(p.halo) * band;
                const float na = am + (solved - am) * k;
                res.removed.d[i] = am - na;
                a.d[i] = na;
            }
        }, h);
    }
    // ---- 6. edge snap, shrink / grow, soften
    if ( (p.snap > 0.f) && (p.snapRadius > 0) ) {
        Plane g(w, h, 1);
        for (size_t i = 0; i < n; ++i) { g.d[i] = luma(&m.d[i * 3]); }
        Plane q = a;
        guidedAlpha(g, q, p.snapRadius, 2e-4f, pf);
        const float k = clamp01(p.snap);
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                // solid and empty areas stay exact: snapping only on the soft edge
                const float edge = 1.f - std::fabs(2.f * a.d[i] - 1.f);
                const float kk = k * std::sqrt(edge);
                a.d[i] += (q.d[i] - a.d[i]) * kk;
            }
        }, h);
    }
    if (p.grow != 0.f) {
        const float g = std::fabs(p.grow);
        const int r0 = (int)std::floor(g), r1 = r0 + 1;
        const float f = g - r0;
        Plane A0 = a, A1 = a;
        if (p.grow > 0.f) {
            dilate(A0, r0, pf);
            dilate(A1, r1, pf);
        } else {
            APFXOps::erode(A0, r0, pf);
            APFXOps::erode(A1, r1, pf);
        }
        for (size_t i = 0; i < n; ++i) { a.d[i] = A0.d[i] + (A1.d[i] - A0.d[i]) * f; }
    }
    if (p.soften > 0.f) {
        APFXOps::blur(a, p.soften, pf);
    }
}
} // namespace APFXSK

#endif // APFX_SCREENKEY_H
