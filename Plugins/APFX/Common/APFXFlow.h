/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX optical flow: dense motion vectors between two frames, written for speed.
//
//  - coarse-to-fine image pyramid of a log-luminance image (brightness changes and flicker
//    become offsets, removed by zero-mean patches)
//  - per level: patch inverse search (8x8 patches on a grid, inverse-compositional Gauss-Newton),
//    densified with per-pixel photometric weights, then an edge-aware, confidence-weighted
//    smoothing that fills weak / masked areas from their neighbours
//  - the finest level is chosen by 'detail' (1 = full resolution, 0.5 = half, ...)
//  - optional per-pixel weights (mattes): motion is only measured where the weight is high
//  - a process-wide cache of frame-pair flows keyed by the image contents, so scrubbing,
//    replays and neighbouring frames reuse what is already computed
//
// Helpers for frame interpolation (occlusion-aware, forward-splatted intermediate flows) and
// for warping neighbour frames onto a frame (frame blending / motion smoothing).

#ifndef APFX_FLOW_H
#define APFX_FLOW_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "APFXImageOps.h"

namespace APFXFlow {

using APFXOps::ParallelFor;
using APFXOps::Plane;

// ---------------------------------------------------------------- images

struct Gray
{
    int w = 0, h = 0;
    std::vector<float> d;

    void resize(int w_, int h_, float v = 0.f) { w = w_; h = h_; d.assign( (size_t)w * h, v ); }
    float at(int x, int y) const
    {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);

        return d[(size_t)y * w + x];
    }
    // bilinear, pixel centres on integers, clamped to the edge
    float bil(float x, float y) const
    {
        const float fx = std::floor(x), fy = std::floor(y);
        const int ix = (int)fx, iy = (int)fy;
        const float ax = x - fx, ay = y - fy;
        if ( (ix >= 0) && (iy >= 0) && (ix + 1 < w) && (iy + 1 < h) ) {
            const float* p = &d[(size_t)iy * w + ix];
            const float t = p[0] + (p[1] - p[0]) * ax;
            const float b = p[w] + (p[w + 1] - p[w]) * ax;

            return t + (b - t) * ay;
        }
        const float a00 = at(ix, iy), a10 = at(ix + 1, iy), a01 = at(ix, iy + 1), a11 = at(ix + 1, iy + 1);
        const float t = a00 + (a10 - a00) * ax, b = a01 + (a11 - a01) * ax;

        return t + (b - t) * ay;
    }
};

// 2x box downsample (odd sizes keep the last row / column)
inline Gray
half(const Gray& g, const ParallelFor& pf)
{
    Gray o;
    o.resize( std::max(1, (g.w + 1) / 2), std::max(1, (g.h + 1) / 2) );
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < o.w; ++x) {
                o.d[(size_t)y * o.w + x] = 0.25f * ( g.at(2 * x, 2 * y) + g.at(2 * x + 1, 2 * y) + g.at(2 * x, 2 * y + 1) + g.at(2 * x + 1, 2 * y + 1) );
            }
        }
    }, o.h);

    return o;
}

// log luminance in about 0..1 for 0..1 scene-linear input (HDR keeps going, slowly)
inline float
flowLuma(float r, float g, float b)
{
    const float l = std::max(0.f, 0.2126f * r + 0.7152f * g + 0.0722f * b);

    return ( std::log(l + 0.02f) + 3.912023f ) * 0.2541f;
}

// stable 64-bit hash of a gray image (content key of the cache)
inline uint64_t
hashGray(const Gray& g)
{
    uint64_t h = 1469598103934665603ULL ^ ( (uint64_t)g.w << 32 ) ^ (uint64_t)g.h;
    const uint32_t* p = reinterpret_cast<const uint32_t*>( g.d.data() );
    const size_t n = g.d.size();
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ULL;
    }

    return h;
}

// ---------------------------------------------------------------- flow fields

// dense 2D vectors on a grid 'scale' times coarser than the image; values in grid pixels
struct Field
{
    int w = 0, h = 0;
    float scale = 1.f;
    std::vector<float> u, v;

    void resize(int w_, int h_, float s = 1.f) { w = w_; h = h_; scale = s; u.assign( (size_t)w * h, 0.f ); v.assign( (size_t)w * h, 0.f ); }
    bool empty() const { return w <= 0 || h <= 0; }
    void sample(float x, float y, float* fu, float* fv) const
    {
        if ( (w < 2) || (h < 2) ) {
            *fu = u.empty() ? 0.f : u[0];
            *fv = v.empty() ? 0.f : v[0];

            return;
        }
        x = std::max( 0.f, std::min( (float)(w - 1), x ) );
        y = std::max( 0.f, std::min( (float)(h - 1), y ) );
        const int ix = std::min(w - 2, (int)x), iy = std::min(h - 2, (int)y);
        const float ax = x - ix, ay = y - iy;
        const size_t i = (size_t)iy * w + ix;
        const float u0 = u[i] + (u[i + 1] - u[i]) * ax, u1 = u[i + w] + (u[i + w + 1] - u[i + w]) * ax;
        const float v0 = v[i] + (v[i + 1] - v[i]) * ax, v1 = v[i + w] + (v[i + w + 1] - v[i + w]) * ax;
        *fu = u0 + (u1 - u0) * ay;
        *fv = v0 + (v1 - v0) * ay;
    }
    // vector in image pixels at image pixel position (x, y) relative to the image origin
    void atImage(float x, float y, float* fu, float* fv) const
    {
        const float is = 1.f / scale;
        sample( (x + 0.5f) * is - 0.5f, (y + 0.5f) * is - 0.5f, fu, fv );
        *fu *= scale;
        *fv *= scale;
    }
};

inline float
bilMap(const std::vector<float>& m, int w, int h, float x, float y)
{
    x = std::max( 0.f, std::min( (float)(w - 1), x ) );
    y = std::max( 0.f, std::min( (float)(h - 1), y ) );
    if ( (w < 2) || (h < 2) ) {
        return m.empty() ? 0.f : m[0];
    }
    const int ix = std::min(w - 2, (int)x), iy = std::min(h - 2, (int)y);
    const float ax = x - ix, ay = y - iy;
    const size_t i = (size_t)iy * w + ix;
    const float a = m[i] + (m[i + 1] - m[i]) * ax, b = m[i + w] + (m[i + w + 1] - m[i + w]) * ax;

    return a + (b - a) * ay;
}

// ---------------------------------------------------------------- estimation

struct Params
{
    float detail = 0.5f;       // finest level: 1 = full resolution, 0.5 = half, 0.25 = quarter
    float smoothness = 0.5f;   // 0 = raw patch vectors, 1 = very smooth fields
    int iterations = 10;       // Gauss-Newton steps per patch
};

typedef std::vector<Gray> Pyramid;

inline Pyramid
buildPyramid(const Gray& g, int levels, const ParallelFor& pf)
{
    Pyramid p;
    p.reserve(levels);
    p.push_back(g);
    while ( (int)p.size() < levels ) {
        p.push_back( half(p.back(), pf) );
    }

    return p;
}

inline int
pyramidLevels(int w, int h)
{
    int L = 1;
    while ( L < 9 && ( (w >> L) >= 24 ) && ( (h >> L) >= 24 ) ) {
        ++L;
    }

    return L;
}

inline int
finestLevel(float detail, int levels)
{
    detail = std::max( 0.02f, std::min(1.f, detail) );
    const int f = (int)std::floor( -std::log2(detail) + 0.5f );

    return std::max( 0, std::min(levels - 1, f) );
}

namespace detail {

// gradient of I (central differences)
inline void
gradients(const Gray& I, Gray& gx, Gray& gy, const ParallelFor& pf)
{
    gx.resize(I.w, I.h);
    gy.resize(I.w, I.h);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < I.w; ++x) {
                const size_t i = (size_t)y * I.w + x;
                gx.d[i] = 0.5f * ( I.at(x + 1, y) - I.at(x - 1, y) );
                gy.d[i] = 0.5f * ( I.at(x, y + 1) - I.at(x, y - 1) );
            }
        }
    }, I.h);
}

// patch grid positions along one axis (last patch flush with the border)
inline std::vector<int>
grid(int n, int ps, int st)
{
    std::vector<int> g;
    if (n <= ps) {
        g.push_back(0);

        return g;
    }
    for (int p = 0; p + ps <= n; p += st) {
        g.push_back(p);
    }
    if (g.back() != n - ps) {
        g.push_back(n - ps);
    }

    return g;
}

struct PatchResult
{
    float u, v, m0, m1, ok;
};

// one pyramid level: refines 'flow' (dense, in level pixels) from I0 to I1
inline void
refineLevel(const Gray& I0, const Gray& I1, const Gray* W, Field& flow, std::vector<float>& conf,
            const Params& prm, bool finest, const ParallelFor& pf)
{
    const int w = I0.w, h = I0.h;
    const int ps = std::min( 8, std::min(w, h) );
    const int st = finest ? std::max(1, ps / 2 - 1) : std::max(1, ps / 2);   // 3 at the finest level, 4 above
    Gray gx, gy;
    gradients(I0, gx, gy, pf);
    const std::vector<int> xs = grid(w, ps, st), ys = grid(h, ps, st);
    const int nx = (int)xs.size(), ny = (int)ys.size();
    std::vector<PatchResult> pr( (size_t)nx * ny );
    const int iters = std::max(1, prm.iterations);
    const float half = 0.5f * (ps - 1);

    // ---- patch inverse search
    pf([&](int r0, int r1) {
        std::vector<float> T( (size_t)ps * ps ), GX(T.size()), GY(T.size()), WW(T.size()), S(T.size());
        for (int r = r0; r < r1; ++r) {
            for (int c = 0; c < nx; ++c) {
                PatchResult& out = pr[(size_t)r * nx + c];
                const int px = xs[c], py = ys[r];
                double sw = 0., st0 = 0.;
                double h11 = 0., h12 = 0., h22 = 0.;
                for (int j = 0, k = 0; j < ps; ++j) {
                    const size_t row = (size_t)(py + j) * w + px;
                    for (int i = 0; i < ps; ++i, ++k) {
                        const float wt = W ? W->d[row + i] : 1.f;
                        T[k] = I0.d[row + i];
                        GX[k] = gx.d[row + i];
                        GY[k] = gy.d[row + i];
                        WW[k] = wt;
                        sw += wt;
                        st0 += wt * T[k];
                        h11 += wt * GX[k] * GX[k];
                        h12 += wt * GX[k] * GY[k];
                        h22 += wt * GY[k] * GY[k];
                    }
                }
                float iu, iv;
                flow.sample(px + half, py + half, &iu, &iv);
                if ( sw < 0.05 * ps * ps ) {
                    out = { iu, iv, 0.f, 0.f, 0.f };
                    continue;
                }
                const float m0 = (float)(st0 / sw);
                // tiny regularisation: flat patches keep their initial vector
                const double reg = 1e-6 * sw + 1e-9;
                h11 += reg;
                h22 += reg;
                const double det = h11 * h22 - h12 * h12;
                if (det <= 1e-18) {
                    out = { iu, iv, m0, m0, 0.f };
                    continue;
                }
                const double i11 = h22 / det, i12 = -h12 / det, i22 = h11 / det;
                auto evalCost = [&](float u, float v, float* mean1) {
                    double s1 = 0.;
                    for (int j = 0, k = 0; j < ps; ++j) {
                        for (int i = 0; i < ps; ++i, ++k) {
                            S[k] = I1.bil(px + i + u, py + j + v);
                            s1 += WW[k] * S[k];
                        }
                    }
                    const float m1 = (float)(s1 / sw);
                    double cost = 0.;
                    for (int k = 0; k < ps * ps; ++k) {
                        const float e = (S[k] - m1) - (T[k] - m0);
                        cost += WW[k] * e * e;
                    }
                    *mean1 = m1;

                    return cost;
                };
                float bm1;
                double c0 = evalCost(iu, iv, &bm1);
                // candidates from the neighbourhood of the coarser estimate (catches objects whose
                // motion the coarse level blurred into their surroundings)
                {
                    const float off = (float)ps * 1.5f;
                    const float cxs[8] = { off, -off, 0.f, 0.f, off, -off, off, -off };
                    const float cys[8] = { 0.f, 0.f, off, -off, off, off, -off, -off };
                    float cu = iu, cv = iv, cm1 = bm1;
                    for (int q = 0; q < 8; ++q) {
                        float qu, qv;
                        flow.sample(px + half + cxs[q], py + half + cys[q], &qu, &qv);
                        if ( (std::fabs(qu - cu) < 0.25f) && (std::fabs(qv - cv) < 0.25f) ) {
                            continue;
                        }
                        float m1q;
                        const double cq = evalCost(qu, qv, &m1q);
                        if (cq < c0 * 0.98) {
                            c0 = cq;
                            cu = qu;
                            cv = qv;
                            cm1 = m1q;
                        }
                    }
                    iu = cu;
                    iv = cv;
                    bm1 = cm1;
                    evalCost(iu, iv, &bm1);   // S back to the chosen start
                }
                float u = iu, v = iv, bu = iu, bv = iv;
                double best = c0;
                for (int it = 0; it < iters; ++it) {
                    // S holds I1 at the current vector (from the last evalCost)
                    double s1 = 0.;
                    for (int k = 0; k < ps * ps; ++k) {
                        s1 += WW[k] * S[k];
                    }
                    const float m1 = (float)(s1 / sw);
                    double bx = 0., by = 0.;
                    for (int k = 0; k < ps * ps; ++k) {
                        const float e = WW[k] * ( (S[k] - m1) - (T[k] - m0) );
                        bx += GX[k] * e;
                        by += GY[k] * e;
                    }
                    float du = (float)(i11 * bx + i12 * by), dv = (float)(i12 * bx + i22 * by);
                    // limit the step (robust on strong non-linearity)
                    const float sl = du * du + dv * dv;
                    if (sl > 4.f) {
                        const float k = 2.f / std::sqrt(sl);
                        du *= k;
                        dv *= k;
                    }
                    u -= du;
                    v -= dv;
                    float m1n;
                    const double c = evalCost(u, v, &m1n);
                    if (c < best) {
                        best = c;
                        bu = u;
                        bv = v;
                        bm1 = m1n;
                    }
                    if (du * du + dv * dv < 1e-4f) {
                        break;
                    }
                }
                // a patch that ran far away from its start is most likely wrong
                if ( (bu - iu) * (bu - iu) + (bv - iv) * (bv - iv) > (float)(ps * ps) ) {
                    bu = iu;
                    bv = iv;
                    evalCost(bu, bv, &bm1);
                }
                out = { bu, bv, m0, bm1, 1.f };
            }
        }
    }, ny);

    // ---- densify: every pixel averages the patches covering it, weighted by its own match error
    std::vector<int> cx0(w), cx1(w), cy0(h), cy1(h);
    for (int x = 0, a = 0; x < w; ++x) {
        while ( (a < nx) && (xs[a] + ps <= x) ) {
            ++a;
        }
        int b = a;
        while ( (b < nx) && (xs[b] <= x) ) {
            ++b;
        }
        cx0[x] = a;
        cx1[x] = b;
    }
    for (int y = 0, a = 0; y < h; ++y) {
        while ( (a < ny) && (ys[a] + ps <= y) ) {
            ++a;
        }
        int b = a;
        while ( (b < ny) && (ys[b] <= y) ) {
            ++b;
        }
        cy0[y] = a;
        cy1[y] = b;
    }
    Field dense;
    dense.resize(w, h, flow.scale);
    conf.assign( (size_t)w * h, 0.f );
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                const float wpix = W ? W->d[i] : 1.f;
                double su = 0., sv = 0., sw = 0.;
                const float i0 = I0.d[i];
                for (int r = cy0[y]; r < cy1[y]; ++r) {
                    for (int c = cx0[x]; c < cx1[x]; ++c) {
                        const PatchResult& p = pr[(size_t)r * nx + c];
                        if (p.ok <= 0.f) {
                            continue;
                        }
                        const float e = std::fabs( (I1.bil(x + p.u, y + p.v) - p.m1) - (i0 - p.m0) );
                        const float wt = wpix / std::max(e, 0.004f);
                        su += wt * p.u;
                        sv += wt * p.v;
                        sw += wt;
                    }
                }
                if (sw > 0.) {
                    dense.u[i] = (float)(su / sw);
                    dense.v[i] = (float)(sv / sw);
                    conf[i] = (float)( sw / (sw + 25.) );
                } else {
                    dense.u[i] = flow.u[i];
                    dense.v[i] = flow.v[i];
                }
            }
        }
    }, h);

    // ---- edge-aware smoothing: data term = confidence, neighbours weighted by image similarity
    const int nSmooth = (int)std::lround( 1.f + prm.smoothness * (finest ? 6.f : 4.f) );
    const float lambda = 0.15f + prm.smoothness * 1.6f;
    const float inv2s = 1.f / (2.f * 0.03f * 0.03f);
    std::vector<float> nw( (size_t)w * h * 2 );   // weights to the right and down neighbours
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                const float a = I0.d[i];
                const float dr = (x + 1 < w) ? I0.d[i + 1] - a : 0.f;
                const float dd = (y + 1 < h) ? I0.d[i + w] - a : 0.f;
                nw[i * 2] = (x + 1 < w) ? lambda * std::exp(-dr * dr * inv2s) : 0.f;
                nw[i * 2 + 1] = (y + 1 < h) ? lambda * std::exp(-dd * dd * inv2s) : 0.f;
            }
        }
    }, h);
    Field cur = dense, nxt = dense;
    for (int it = 0; it < nSmooth; ++it) {
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = (size_t)y * w + x;
                    const float c = conf[i];
                    float su = c * dense.u[i], sv = c * dense.v[i], sw = c;
                    if (x > 0) { const float k = nw[(i - 1) * 2]; su += k * cur.u[i - 1]; sv += k * cur.v[i - 1]; sw += k; }
                    if (x + 1 < w) { const float k = nw[i * 2]; su += k * cur.u[i + 1]; sv += k * cur.v[i + 1]; sw += k; }
                    if (y > 0) { const float k = nw[(i - w) * 2 + 1]; su += k * cur.u[i - w]; sv += k * cur.v[i - w]; sw += k; }
                    if (y + 1 < h) { const float k = nw[i * 2 + 1]; su += k * cur.u[i + w]; sv += k * cur.v[i + w]; sw += k; }
                    if (sw > 1e-9f) {
                        nxt.u[i] = su / sw;
                        nxt.v[i] = sv / sw;
                    } else {
                        nxt.u[i] = cur.u[i];
                        nxt.v[i] = cur.v[i];
                    }
                }
            }
        }, h);
        std::swap(cur.u, nxt.u);
        std::swap(cur.v, nxt.v);
    }
    flow.u.swap(cur.u);
    flow.v.swap(cur.v);
}

// coarse field -> next finer level size (vectors doubled)
inline Field
upsample(const Field& f, int w, int h, const ParallelFor& pf)
{
    Field o;
    o.resize(w, h, f.scale * 0.5f);
    const float kx = (float)f.w / w, ky = (float)f.h / h;
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                float u, v;
                f.sample( (x + 0.5f) * kx - 0.5f, (y + 0.5f) * ky - 0.5f, &u, &v );
                o.u[(size_t)y * w + x] = u / kx;
                o.v[(size_t)y * w + x] = v / ky;
            }
        }
    }, h);

    return o;
}
} // namespace detail

// flow from A to B: for a pixel of A, where it is in B. Weights (optional) are a pyramid
// of per-pixel weights of A (0 = do not measure here). Returns the field at the finest level.
inline Field
computeFlow(const Pyramid& A, const Pyramid& B, const Pyramid* W, const Params& prm, const ParallelFor& pf)
{
    const int levels = (int)std::min( A.size(), B.size() );
    const int fin = finestLevel(prm.detail, levels);
    Field f;
    const int top = levels - 1;
    f.resize(A[top].w, A[top].h, (float)(1 << top) );
    std::vector<float> conf;
    for (int L = top; L >= fin; --L) {
        if (L != top) {
            f = detail::upsample(f, A[L].w, A[L].h, pf);
        }
        f.scale = (float)(1 << L);
        detail::refineLevel( A[L], B[L], W ? &(*W)[L] : NULL, f, conf, prm, L == fin, pf );
    }

    return f;
}

// flows of a frame pair, both directions
struct PairFlows
{
    Field f01, f10;
};

// ---------------------------------------------------------------- cache

struct PairKey
{
    long long fa = 0, fb = 0;            // frames x 1000
    int w = 0, h = 0;
    uint64_t ha = 0, hb = 0, hm = 0;     // contents of A, B and the weights
    int detailQ = 0, smoothQ = 0, layer = 0;

    bool operator<(const PairKey& o) const
    {
        if (fa != o.fa) { return fa < o.fa; }
        if (fb != o.fb) { return fb < o.fb; }
        if (w != o.w) { return w < o.w; }
        if (h != o.h) { return h < o.h; }
        if (ha != o.ha) { return ha < o.ha; }
        if (hb != o.hb) { return hb < o.hb; }
        if (hm != o.hm) { return hm < o.hm; }
        if (detailQ != o.detailQ) { return detailQ < o.detailQ; }
        if (smoothQ != o.smoothQ) { return smoothQ < o.smoothQ; }

        return layer < o.layer;
    }
};

typedef std::shared_ptr<const PairFlows> PairPtr;

// process-wide, shared by every node instance; the same pair is never computed twice at once
class FlowCache
{
public:
    static FlowCache& instance()
    {
        static FlowCache c;

        return c;
    }

    PairPtr get(const PairKey& k, const std::function<PairPtr()>& compute)
    {
        std::shared_future<PairPtr> fut;
        std::shared_ptr<std::promise<PairPtr> > mine;
        {
            std::lock_guard<std::mutex> l(_m);
            std::map<PairKey, Entry>::iterator it = _map.find(k);
            if ( it != _map.end() ) {
                it->second.use = ++_clock;
                fut = it->second.fut;
            } else {
                mine = std::make_shared<std::promise<PairPtr> >();
                Entry e;
                e.fut = mine->get_future().share();
                e.use = ++_clock;
                _map[k] = e;
                fut = e.fut;
                evict();
            }
        }
        if (mine) {
            try {
                mine->set_value( compute() );
            } catch (...) {
                {
                    std::lock_guard<std::mutex> l(_m);
                    _map.erase(k);
                }
                mine->set_exception( std::current_exception() );
            }
        }

        return fut.get();
    }

    // stores a ready result unless the key is already there (never waits)
    void put(const PairKey& k, const PairPtr& p)
    {
        std::lock_guard<std::mutex> l(_m);
        if ( _map.find(k) != _map.end() ) {
            return;
        }
        std::promise<PairPtr> pr;
        pr.set_value(p);
        Entry e;
        e.fut = pr.get_future().share();
        e.use = ++_clock;
        _map[k] = e;
        evict();
    }

    void setBudgetBytes(size_t b) { std::lock_guard<std::mutex> l(_m); _budget = b; }

private:
    struct Entry
    {
        std::shared_future<PairPtr> fut;
        uint64_t use = 0;
    };

    static size_t bytesOf(const Entry& e)
    {
        if ( e.fut.wait_for( std::chrono::seconds(0) ) != std::future_status::ready ) {
            return 0;
        }
        try {
            const PairPtr p = e.fut.get();

            return p ? (p->f01.u.size() + p->f10.u.size()) * 2 * sizeof(float) : 0;
        } catch (...) {
            return 0;
        }
    }

    void evict()
    {
        size_t total = 0;
        for (std::map<PairKey, Entry>::const_iterator it = _map.begin(); it != _map.end(); ++it) {
            total += bytesOf(it->second);
        }
        while ( (total > _budget) && (_map.size() > 2) ) {
            std::map<PairKey, Entry>::iterator oldest = _map.end();
            for (std::map<PairKey, Entry>::iterator it = _map.begin(); it != _map.end(); ++it) {
                if ( ( bytesOf(it->second) > 0 ) && ( (oldest == _map.end()) || (it->second.use < oldest->second.use) ) ) {
                    oldest = it;
                }
            }
            if ( oldest == _map.end() ) {
                break;
            }
            total -= bytesOf(oldest->second);
            _map.erase(oldest);
        }
    }

    std::mutex _m;
    std::map<PairKey, Entry> _map;
    uint64_t _clock = 0;
    size_t _budget = (size_t)1536 << 20;   // 1.5 GB of vectors
};

// gray + pyramid of one frame, built only when a pair has to be computed
struct FrameData
{
    Gray gray;
    uint64_t hash = 0;
    std::shared_ptr<Pyramid> pyr;
    Gray weights;                         // optional per-pixel weights (empty = none)
    uint64_t whash = 0;
    std::shared_ptr<Pyramid> wpyr;

    const Pyramid& pyramid(int levels, const ParallelFor& pf)
    {
        if ( !pyr || ( (int)pyr->size() < levels ) ) {
            pyr = std::make_shared<Pyramid>( buildPyramid(gray, levels, pf) );
        }

        return *pyr;
    }
    const Pyramid* weightPyramid(int levels, const ParallelFor& pf)
    {
        if ( weights.d.empty() ) {
            return NULL;
        }
        if ( !wpyr || ( (int)wpyr->size() < levels ) ) {
            wpyr = std::make_shared<Pyramid>( buildPyramid(weights, levels, pf) );
        }

        return wpyr.get();
    }
};

// both flows of a pair (cached). layer: 0 plain, 1 background (weights = 1 - matte), 2 foreground
inline PairPtr
pairFlows(double fa, double fb, FrameData& A, FrameData& B, const Params& prm, int layer, const ParallelFor& pf)
{
    PairKey k;
    k.fa = (long long)std::llround(fa * 1000.);
    k.fb = (long long)std::llround(fb * 1000.);
    k.w = A.gray.w;
    k.h = A.gray.h;
    k.ha = A.hash;
    k.hb = B.hash;
    k.hm = A.whash * 31u + B.whash;
    k.detailQ = (int)std::lround(prm.detail * 1000.f);
    k.smoothQ = (int)std::lround(prm.smoothness * 1000.f) * 64 + prm.iterations;
    k.layer = layer;
    // the reversed pair is the same computation
    PairKey kr = k;
    std::swap(kr.fa, kr.fb);
    std::swap(kr.ha, kr.hb);
    kr.hm = B.whash * 31u + A.whash;

    return FlowCache::instance().get(k, [&]() -> PairPtr {
        const int levels = pyramidLevels(A.gray.w, A.gray.h);
        const Pyramid& pa = A.pyramid(levels, pf);
        const Pyramid& pb = B.pyramid(levels, pf);
        std::shared_ptr<PairFlows> r = std::make_shared<PairFlows>();
        r->f01 = computeFlow( pa, pb, A.weightPyramid(levels, pf), prm, pf );
        r->f10 = computeFlow( pb, pa, B.weightPyramid(levels, pf), prm, pf );
        // the reversed pair shares the result
        std::shared_ptr<PairFlows> rev = std::make_shared<PairFlows>();
        rev->f01 = r->f10;
        rev->f10 = r->f01;
        FlowCache::instance().put(kr, rev);

        return r;
    });
}

// ---------------------------------------------------------------- sampling

// samples a plane (c channels) at a pixel position (centres on integers), clamped to the edge.
// filter 0 = bilinear, 1 = bicubic clamped to its 2x2 neighbourhood (sharp, no overshoot)
inline void
samplePlane(const Plane& p, float x, float y, int filter, float* out)
{
    const int c = p.c;
    x = std::max( 0.f, std::min( (float)(p.w - 1), x ) );
    y = std::max( 0.f, std::min( (float)(p.h - 1), y ) );
    const int ix = std::min( p.w - 1, (int)x ), iy = std::min( p.h - 1, (int)y );
    const int ix1 = std::min(p.w - 1, ix + 1), iy1 = std::min(p.h - 1, iy + 1);
    const float ax = x - ix, ay = y - iy;
    const float* a = p.px(ix, iy);
    const float* b = p.px(ix1, iy);
    const float* cc = p.px(ix, iy1);
    const float* d = p.px(ix1, iy1);
    if ( (filter == 0) || (p.w < 4) || (p.h < 4) ) {
        for (int k = 0; k < c; ++k) {
            const float t = a[k] + (b[k] - a[k]) * ax, bo = cc[k] + (d[k] - cc[k]) * ax;
            out[k] = t + (bo - t) * ay;
        }

        return;
    }
    // Catmull-Rom weights
    auto wts = [](float t, float* w) {
        const float t2 = t * t, t3 = t2 * t;
        w[0] = -0.5f * t3 + t2 - 0.5f * t;
        w[1] = 1.5f * t3 - 2.5f * t2 + 1.f;
        w[2] = -1.5f * t3 + 2.f * t2 + 0.5f * t;
        w[3] = 0.5f * t3 - 0.5f * t2;
    };
    float wx[4], wy[4];
    wts(ax, wx);
    wts(ay, wy);
    int xs[4], ys[4];
    for (int i = 0; i < 4; ++i) {
        xs[i] = std::max( 0, std::min(p.w - 1, ix - 1 + i) );
        ys[i] = std::max( 0, std::min(p.h - 1, iy - 1 + i) );
    }
    for (int k = 0; k < c; ++k) {
        float acc = 0.f;
        for (int j = 0; j < 4; ++j) {
            float row = 0.f;
            for (int i = 0; i < 4; ++i) {
                row += wx[i] * p.px(xs[i], ys[j])[k];
            }
            acc += wy[j] * row;
        }
        const float lo = std::min( std::min(a[k], b[k]), std::min(cc[k], d[k]) );
        const float hi = std::max( std::max(a[k], b[k]), std::max(cc[k], d[k]) );
        out[k] = std::max( lo, std::min(hi, acc) );
    }
}

// ---------------------------------------------------------------- interpolation

struct InterpDebug
{
    Plane occlusion;   // 2 channels: 1 - visibility in A, 1 - visibility in B (image size)
};

// Frame between A (a = 0) and B (a = 1), occlusion aware. A, B: planes of the same size and
// channel count (premultiplied colours). fl: flows of the pair at any grid scale.
inline void
interpolate(const Plane& A, const Plane& B, const PairFlows& fl, float a, int filter, Plane& out,
            const ParallelFor& pf, InterpDebug* dbg = NULL)
{
    const Field& F01 = fl.f01;
    const Field& F10 = fl.f10;
    const int fw = F01.w, fh = F01.h;
    const size_t n = (size_t)fw * fh;
    // forward-backward consistency -> visibility of each grid pixel in the other frame
    std::vector<float> vis0(n), vis1(n);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < fw; ++x) {
                const size_t i = (size_t)y * fw + x;
                float bu, bv;
                F10.sample(x + F01.u[i], y + F01.v[i], &bu, &bv);
                float eu = F01.u[i] + bu, ev = F01.v[i] + bv;
                float tol = 0.35f + 0.06f * std::sqrt(F01.u[i] * F01.u[i] + F01.v[i] * F01.v[i]);
                vis0[i] = std::exp( -(eu * eu + ev * ev) / (tol * tol) );
                F01.sample(x + F10.u[i], y + F10.v[i], &bu, &bv);
                eu = F10.u[i] + bu;
                ev = F10.v[i] + bv;
                tol = 0.35f + 0.06f * std::sqrt(F10.u[i] * F10.u[i] + F10.v[i] * F10.v[i]);
                vis1[i] = std::exp( -(eu * eu + ev * ev) / (tol * tol) );
            }
        }
    }, fh);
    // forward splat of the intermediate flows (t -> A and t -> B); reliable pixels win
    std::vector<float> s0u(n, 0.f), s0v(n, 0.f), s0w(n, 0.f), s1u(n, 0.f), s1v(n, 0.f), s1w(n, 0.f);
    auto splat = [&](const Field& F, const std::vector<float>& vis, float t, std::vector<float>& su, std::vector<float>& sv, std::vector<float>& sw) {
        for (int y = 0; y < fh; ++y) {
            for (int x = 0; x < fw; ++x) {
                const size_t i = (size_t)y * fw + x;
                const float qx = x + t * F.u[i], qy = y + t * F.v[i];
                const int ix = (int)std::floor(qx), iy = (int)std::floor(qy);
                const float ax = qx - ix, ay = qy - iy;
                const float wv = std::max(vis[i], 0.02f);
                const float vu = -t * F.u[i], vv = -t * F.v[i];
                const float ws[4] = { (1.f - ax) * (1.f - ay), ax * (1.f - ay), (1.f - ax) * ay, ax * ay };
                const int dx[4] = { 0, 1, 0, 1 }, dy[4] = { 0, 0, 1, 1 };
                for (int k = 0; k < 4; ++k) {
                    const int xx = ix + dx[k], yy = iy + dy[k];
                    if ( (xx < 0) || (yy < 0) || (xx >= fw) || (yy >= fh) ) {
                        continue;
                    }
                    const float ww = ws[k] * wv;
                    const size_t j = (size_t)yy * fw + xx;
                    su[j] += ww * vu;
                    sv[j] += ww * vv;
                    sw[j] += ww;
                }
            }
        }
    };
    // the two splats are independent: run them on two threads of the host pool
    pf([&](int b0, int b1) {
        for (int b = b0; b < b1; ++b) {
            if (b == 0) {
                splat(F01, vis0, a, s0u, s0v, s0w);
            } else {
                splat(F10, vis1, 1.f - a, s1u, s1v, s1w);
            }
        }
    }, 2);
    // intermediate flows + visibilities at the grid, interleaved (one lookup per pixel)
    std::vector<float> g6(n * 6);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < fw; ++x) {
                const size_t i = (size_t)y * fw + x;
                // linear estimate (holes of the splat): assumes locally constant motion
                const float l0u = -(1.f - a) * a * F01.u[i] + a * a * F10.u[i];
                const float l0v = -(1.f - a) * a * F01.v[i] + a * a * F10.v[i];
                const float l1u = (1.f - a) * (1.f - a) * F01.u[i] - a * (1.f - a) * F10.u[i];
                const float l1v = (1.f - a) * (1.f - a) * F01.v[i] - a * (1.f - a) * F10.v[i];
                const float k = 0.05f;
                float* g = &g6[i * 6];
                g[0] = (s0u[i] + k * l0u) / (s0w[i] + k);
                g[1] = (s0v[i] + k * l0v) / (s0w[i] + k);
                g[2] = (s1u[i] + k * l1u) / (s1w[i] + k);
                g[3] = (s1v[i] + k * l1v) / (s1w[i] + k);
                g[4] = bilMap(vis0, fw, fh, x + g[0], y + g[1]);
                g[5] = bilMap(vis1, fw, fh, x + g[2], y + g[3]);
            }
        }
    }, fh);
    // pixels
    const int W = A.w, H = A.h, c = A.c;
    out.resize(W, H, c);
    if (dbg) {
        dbg->occlusion.resize(W, H, 2);
    }
    const float s = F01.scale, is = 1.f / s;
    pf([&](int y0, int y1) {
        std::vector<float> ca(c), cb(c);
        for (int y = y0; y < y1; ++y) {
            const float gy = std::max( 0.f, std::min( (float)(fh - 1), (y + 0.5f) * is - 0.5f ) );
            const int iy = std::min( std::max(0, fh - 2), (int)gy );
            const float ay = (fh < 2) ? 0.f : gy - iy;
            const int iy1 = std::min(fh - 1, iy + 1);
            for (int x = 0; x < W; ++x) {
                const float gx = std::max( 0.f, std::min( (float)(fw - 1), (x + 0.5f) * is - 0.5f ) );
                const int ix = std::min( std::max(0, fw - 2), (int)gx );
                const float ax = (fw < 2) ? 0.f : gx - ix;
                const int ix1 = std::min(fw - 1, ix + 1);
                const float* p00 = &g6[( (size_t)iy * fw + ix ) * 6];
                const float* p10 = &g6[( (size_t)iy * fw + ix1 ) * 6];
                const float* p01 = &g6[( (size_t)iy1 * fw + ix ) * 6];
                const float* p11 = &g6[( (size_t)iy1 * fw + ix1 ) * 6];
                float g[6];
                for (int k = 0; k < 6; ++k) {
                    const float t = p00[k] + (p10[k] - p00[k]) * ax, b = p01[k] + (p11[k] - p01[k]) * ax;
                    g[k] = t + (b - t) * ay;
                }
                float wa = (1.f - a) * g[4], wb = a * g[5];
                if (wa + wb < 1e-3f) {
                    wa = 1.f - a;
                    wb = a;
                }
                const float inv = 1.f / (wa + wb);
                samplePlane(A, x + g[0] * s, y + g[1] * s, filter, ca.data());
                samplePlane(B, x + g[2] * s, y + g[3] * s, filter, cb.data());
                float* o = out.px(x, y);
                for (int k = 0; k < c; ++k) {
                    o[k] = (wa * ca[k] + wb * cb[k]) * inv;
                }
                if (dbg) {
                    float* d = dbg->occlusion.px(x, y);
                    d[0] = 1.f - g[4];
                    d[1] = 1.f - g[5];
                }
            }
        }
    }, H);
}

// V(0 -> k) chained from V(0 -> k-1) and the step flow F(k-1 -> k): V + F(x + V)
inline Field
chain(const Field& V, const Field& F, const ParallelFor& pf)
{
    Field o;
    o.resize(V.w, V.h, V.scale);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < V.w; ++x) {
                const size_t i = (size_t)y * V.w + x;
                float fu, fv;
                F.sample(x + V.u[i], y + V.v[i], &fu, &fv);
                o.u[i] = V.u[i] + fu;
                o.v[i] = V.v[i] + fv;
            }
        }
    }, V.h);

    return o;
}

// separable box blur of a field (radius in grid pixels, 2 passes ~ gaussian)
inline void
blurField(Field& f, float radius, const ParallelFor& pf)
{
    const int r = (int)std::lround(radius);
    if ( (r <= 0) || f.empty() ) {
        return;
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (int comp = 0; comp < 2; ++comp) {
            std::vector<float>& m = comp ? f.v : f.u;
            std::vector<float> tmp( m.size() );
            pf([&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    APFXOps::boxLine(&m[(size_t)y * f.w], f.w, 1, r, &tmp[(size_t)y * f.w], 1);
                }
            }, f.h);
            pf([&](int x0, int x1) {
                for (int x = x0; x < x1; ++x) {
                    APFXOps::boxLine(&tmp[x], f.h, f.w, r, &m[x], f.w);
                }
            }, f.w);
        }
    }
}
} // namespace APFXFlow

#endif // APFX_FLOW_H
