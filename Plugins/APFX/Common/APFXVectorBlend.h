/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// Motion-compensated frame blending and motion smoothing (APFX VectorBlend).
//
// Neighbour frames are pushed onto the current frame along their motion vectors, then
// combined per pixel (average / median / min / max / plus): flicker, grain and noise average
// out while the picture stays sharp. Motion smoothing treats the vectors to the neighbours as
// samples of each pixel's path through time, fits a smooth path (average, laplacian, gaussian
// or a local quadratic fit) and moves the pixel onto it: jitter and wobble go, real motion
// (including acceleration with the quadratic fit) stays.

#ifndef APFX_VECTORBLEND_H
#define APFX_VECTORBLEND_H

#include <algorithm>
#include <cmath>
#include <vector>

#include "APFXFlow.h"

namespace APFXVB {

using APFXFlow::Field;
using APFXOps::ParallelFor;
using APFXOps::Plane;

enum BlendOp { eBlendAverage = 0, eBlendMedian, eBlendMin, eBlendMax, eBlendPlus };
enum SmoothOp { eSmoothAverage = 0, eSmoothLaplacian, eSmoothGaussian, eSmoothQuadratic };

struct Settings
{
    int blendRadius = 2;          // 0 = no blending
    int blendOp = eBlendAverage;
    int smoothRadius = 0;         // 0 = no motion smoothing
    int smoothOp = eSmoothGaussian;
    float sigma = 2.5f;
    int filter = 1;               // 0 bilinear, 1 clamped bicubic
    bool logSpace = true;         // combine colours in log space (no filter overshoot, flicker = offsets)
    bool unpremult = false;       // rgb unpremultiplied (edges extended) while warping
    bool keepAlpha = false;
    bool clampAlpha = false;
};

// smoothed offset of a path sampled at offsets t[i] (t = 0 sample is the origin, value 0)
inline float
smoothPath(const float* d, const float* t, const bool* ok, int n, int op, float sigma)
{
    sigma = std::max(0.25f, sigma);
    if (op == eSmoothAverage) {
        double s = 0.;
        int c = 0;
        for (int i = 0; i < n; ++i) {
            if (ok[i]) { s += d[i]; ++c; }
        }

        return c ? (float)(s / c) : 0.f;
    }
    if (op == eSmoothGaussian) {
        double s = 0., w = 0.;
        for (int i = 0; i < n; ++i) {
            if (!ok[i]) { continue; }
            const double k = std::exp( -0.5 * (t[i] / sigma) * (t[i] / sigma) );
            s += k * d[i];
            w += k;
        }

        return w > 0. ? (float)(s / w) : 0.f;
    }
    if (op == eSmoothLaplacian) {
        // symmetric pairs: half the second difference around 0; constant velocity gives 0,
        // a spike on this frame gives minus the spike
        double s = 0., w = 0.;
        for (int i = 0; i < n; ++i) {
            if ( !ok[i] || (t[i] <= 0.f) ) { continue; }
            for (int j = 0; j < n; ++j) {
                if ( ok[j] && (std::fabs(t[j] + t[i]) < 1e-3f) ) {
                    const double k = std::exp( -0.5 * (t[i] / sigma) * (t[i] / sigma) );
                    s += k * 0.5 * (d[i] + d[j]);
                    w += k;
                }
            }
        }

        return w > 0. ? (float)(s / w) : 0.f;
    }
    // local quadratic least squares y = a + b t + c t^2, value at 0 = a
    double S0 = 0., S1 = 0., S2 = 0., S3 = 0., S4 = 0., Y0 = 0., Y1 = 0., Y2 = 0.;
    int c = 0;
    for (int i = 0; i < n; ++i) {
        if (!ok[i]) { continue; }
        const double x = t[i], x2 = x * x;
        S0 += 1.; S1 += x; S2 += x2; S3 += x2 * x; S4 += x2 * x2;
        Y0 += d[i]; Y1 += x * d[i]; Y2 += x2 * d[i];
        ++c;
    }
    if (c < 5) {
        return c ? (float)(Y0 / c) : 0.f;
    }
    // solve the 3x3 normal equations (Cramer) for a
    const double m00 = S0, m01 = S1, m02 = S2, m11 = S2, m12 = S3, m22 = S4;
    const double det = m00 * (m11 * m22 - m12 * m12) - m01 * (m01 * m22 - m12 * m02) + m02 * (m01 * m12 - m11 * m02);
    if (std::fabs(det) < 1e-12) {
        return (float)(Y0 / c);
    }
    const double da = Y0 * (m11 * m22 - m12 * m12) - m01 * (Y1 * m22 - m12 * Y2) + m02 * (Y1 * m12 - m11 * Y2);

    return (float)(da / det);
}

// working copy of a frame: unpremultiplied (with edge extension) and / or log
inline Plane
prepare(const Plane& src, const Settings& st, const ParallelFor& pf)
{
    Plane p = src;
    const size_t n = (size_t)p.w * p.h;
    if (st.unpremult) {
        Plane cw(p.w, p.h, 4);
        for (size_t i = 0; i < n; ++i) {
            const float a = p.d[i * 4 + 3];
            const float w = a > 1e-3f ? 1.f : 0.f;
            for (int c = 0; c < 3; ++c) {
                cw.d[i * 4 + c] = w * (a > 1e-3f ? p.d[i * 4 + c] / a : 0.f);
            }
            cw.d[i * 4 + 3] = w;
        }
        Plane filled = APFXOps::pullPush(cw, 32, pf);
        for (size_t i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                p.d[i * 4 + c] = filled.d[i * 3 + c];
            }
        }
    }
    if (st.logSpace) {
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * p.w; i < (size_t)y1 * p.w; ++i) {
                for (int c = 0; c < 3; ++c) {
                    float& v = p.d[i * 4 + c];
                    v = std::log1p( std::max(-0.99f, v) );
                }
            }
        }, p.h);
    }

    return p;
}

// frames[k + R] = frame at offset k (NULL = missing); V[k + R] = vectors from this frame to frame k
// (k != 0; NULL = missing). out = result over the same window. corr (optional) = correction (pixels).
inline void
vectorBlend(const std::vector<const Plane*>& frames, const std::vector<const Field*>& V, int R, const Settings& st,
            Plane& out, Plane* corr, const ParallelFor& pf)
{
    const Plane* cur = frames[R];
    const int W = cur->w, H = cur->h;
    out.resize(W, H, 4);
    // working copies of the frames used for blending
    const int br = std::min(R, std::max(0, st.blendRadius));
    std::vector<Plane> work(2 * R + 1);
    std::vector<bool> have(2 * R + 1, false);
    for (int k = -br; k <= br; ++k) {
        if ( frames[k + R] && ( (k == 0) || V[k + R] ) ) {
            work[k + R] = prepare(*frames[k + R], st, pf);
            have[k + R] = true;
        }
    }
    // correction field (motion smoothing), on the grid of the vectors
    const int sr = std::min(R, std::max(0, st.smoothRadius));
    Field C;
    const Field* ref = NULL;
    for (int k = -R; k <= R && !ref; ++k) {
        if ( (k != 0) && V[k + R] ) {
            ref = V[k + R];
        }
    }
    if ( (sr > 0) && ref ) {
        C.resize(ref->w, ref->h, ref->scale);
        pf([&](int y0, int y1) {
            float du[11], dv[11], tt[11];
            bool ok[11];
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < C.w; ++x) {
                    const size_t i = (size_t)y * C.w + x;
                    int n = 0;
                    for (int k = -sr; k <= sr && n < 11; ++k) {
                        tt[n] = (float)k;
                        if (k == 0) {
                            du[n] = dv[n] = 0.f;
                            ok[n] = true;
                        } else if (V[k + R] && (V[k + R]->w == C.w) && (V[k + R]->h == C.h) ) {
                            du[n] = V[k + R]->u[i];
                            dv[n] = V[k + R]->v[i];
                            ok[n] = true;
                        } else {
                            du[n] = dv[n] = 0.f;
                            ok[n] = false;
                        }
                        ++n;
                    }
                    C.u[i] = smoothPath(du, tt, ok, n, st.smoothOp, st.sigma);
                    C.v[i] = smoothPath(dv, tt, ok, n, st.smoothOp, st.sigma);
                }
            }
        }, C.h);
    }
    if (corr) {
        corr->resize(W, H, 4);
    }
    const bool smooth = !C.empty();
    pf([&](int y0, int y1) {
        std::vector<float> vals( (size_t)(2 * br + 1) * 4 );
        float tmp[4];
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < W; ++x) {
                float px = (float)x, py = (float)y;
                float cu = 0.f, cv = 0.f;
                if (smooth) {
                    C.atImage(px, py, &cu, &cv);
                    px -= cu;
                    py -= cv;
                }
                int n = 0;
                for (int k = -br; k <= br; ++k) {
                    if (!have[k + R]) {
                        continue;
                    }
                    float sx = px, sy = py;
                    if (k != 0) {
                        float vu, vv;
                        V[k + R]->atImage(px, py, &vu, &vv);
                        sx += vu;
                        sy += vv;
                    }
                    APFXFlow::samplePlane(work[k + R], sx, sy, st.filter, tmp);
                    for (int c = 0; c < 4; ++c) {
                        vals[(size_t)c * (2 * br + 1) + n] = tmp[c];
                    }
                    ++n;
                }
                float res[4] = { 0.f, 0.f, 0.f, 0.f };
                for (int c = 0; c < 4 && n > 0; ++c) {
                    float* v = &vals[(size_t)c * (2 * br + 1)];
                    float r;
                    switch (st.blendOp) {
                    case eBlendMedian: {
                        std::nth_element(v, v + n / 2, v + n);
                        r = v[n / 2];
                        if ( (n % 2) == 0 ) {
                            const float lo = *std::max_element(v, v + n / 2);
                            r = 0.5f * (r + lo);
                        }
                        break;
                    }
                    case eBlendMin: r = *std::min_element(v, v + n); break;
                    case eBlendMax: r = *std::max_element(v, v + n); break;
                    case eBlendPlus: {
                        r = 0.f;
                        for (int i = 0; i < n; ++i) { r += v[i]; }
                        break;
                    }
                    default: {
                        r = 0.f;
                        for (int i = 0; i < n; ++i) { r += v[i]; }
                        r /= n;
                    }
                    }
                    res[c] = r;
                }
                if (st.logSpace) {
                    for (int c = 0; c < 3; ++c) {
                        res[c] = std::expm1(res[c]);
                    }
                }
                if (st.keepAlpha) {
                    res[3] = cur->px(x, y)[3];
                }
                if (st.clampAlpha) {
                    res[3] = std::max( 0.f, std::min(1.f, res[3]) );
                }
                if (st.unpremult) {
                    for (int c = 0; c < 3; ++c) {
                        res[c] *= res[3];
                    }
                }
                float* o = out.px(x, y);
                o[0] = res[0]; o[1] = res[1]; o[2] = res[2]; o[3] = res[3];
                if (corr) {
                    float* d = corr->px(x, y);
                    d[0] = cu;
                    d[1] = cv;
                    d[2] = 0.f;
                    d[3] = 1.f;
                }
            }
        }
    }, H);
}
} // namespace APFXVB

#endif // APFX_VECTORBLEND_H
