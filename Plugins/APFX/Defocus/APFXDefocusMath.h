/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Defocus math: fast convex-bokeh convolution.
// Any convex aperture (disc, polygon blades, rounded blades, any aspect ratio) covers,
// on each kernel row, one horizontal span. With per-row running sums (prefix sums)
// each span costs O(1), so a pixel costs O(kernel height) instead of O(kernel area).

#ifndef APFX_DEFOCUSMATH_H
#define APFX_DEFOCUSMATH_H

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace APFXDefocus {

struct KernelRow
{
    int dy;          // row offset
    float left;      // span start (pixels, relative to the output pixel, continuous)
    float right;     // span end
    float weight;    // row weight (2 when every other row is skipped)
};

struct Kernel
{
    std::vector<KernelRow> rows;
    double area = 1.;  // total weight, for normalisation
    int rx = 0, ry = 0; // max extent in pixels (bbox growth)
};

struct ShapeParams
{
    double radius = 10.;    // pixels (vertical radius)
    double aspect = 1.;     // horizontal / vertical
    int blades = 0;         // 0 = disc, else polygon with this many blades
    double rotation = 0.;   // degrees
    double roundness = 0.;  // 0 = straight blades, 1 = disc
    bool fast = false;      // skip every other row
};

// horizontal span of the unit polygon (vertices on the unit circle) at height v
inline bool
polygonSpan(int n,
            double rotRad,
            double v,
            double* ul,
            double* ur)
{
    double lo = 1e9, hi = -1e9;
    for (int k = 0; k < n; ++k) {
        const double a0 = rotRad + 2. * M_PI * k / n + M_PI / 2.;
        const double a1 = rotRad + 2. * M_PI * (k + 1) / n + M_PI / 2.;
        const double x0 = std::cos(a0), y0 = std::sin(a0);
        const double x1 = std::cos(a1), y1 = std::sin(a1);
        if ( (v < std::min(y0, y1)) || (v > std::max(y0, y1)) || (y0 == y1) ) {
            continue;
        }
        const double x = x0 + (v - y0) * (x1 - x0) / (y1 - y0);
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    if (lo > hi) {
        return false;
    }
    *ul = lo;
    *ur = hi;

    return true;
}

inline Kernel
buildKernel(const ShapeParams& sp)
{
    Kernel k;
    const double ry = std::max(0., sp.radius);
    const double rx = ry * std::max(0.01, sp.aspect);
    k.ry = (int)std::ceil(ry);
    k.rx = (int)std::ceil(rx) + 1;
    if (ry < 0.5) {
        KernelRow r = { 0, -0.5f, 0.5f, 1.f };
        k.rows.push_back(r);
        k.area = 1.;

        return k;
    }
    const double rot = sp.rotation * M_PI / 180.;
    const int step = sp.fast ? 2 : 1;
    double area = 0.;
    for (int dy = -k.ry; dy <= k.ry; dy += step) {
        const double v = dy / ry; // row centre in unit shape space (y up = dy up)
        if (std::fabs(v) > 1.) {
            continue;
        }
        double ul = -std::sqrt(std::max(0., 1. - v * v)), ur = -ul; // disc
        if (sp.blades >= 3) {
            double pl, pr;
            if ( !polygonSpan(sp.blades, rot, v, &pl, &pr) ) {
                continue;
            }
            const double r = std::max( 0., std::min(1., sp.roundness) );
            ul = pl + (ul - pl) * r;
            ur = pr + (ur - pr) * r;
        }
        KernelRow row;
        row.dy = dy;
        row.left = (float)(ul * rx);
        row.right = (float)(ur * rx);
        if (row.right - row.left < 1.f) {
            // keep at least one pixel so thin rows (top/bottom) still count
            const float c = 0.5f * (row.left + row.right);
            row.left = c - 0.5f;
            row.right = c + 0.5f;
        }
        row.weight = (float)step;
        area += (row.right - row.left) * row.weight;
        k.rows.push_back(row);
    }
    k.area = std::max(1e-6, area);

    return k;
}

// Prefix sums of one input row: P[i] = sum of v[0..i-1] (pixel i covers [i-0.5, i+0.5))
// Integral over a continuous interval [a, b] of the piecewise constant row.
inline float
spanSum(const float* P,
        const float* v,
        int n,
        float a,
        float b,
        bool extend)
{
    // shift so that pixel i covers [i, i+1)
    a += 0.5f;
    b += 0.5f;
    float s = 0.f;
    if (extend) {
        // outside the row, repeat the edge pixels
        if (a < 0.f) {
            s += v[0] * ( std::min(b, 0.f) - a );
        }
        if (b > (float)n) {
            s += v[n - 1] * ( b - std::max(a, (float)n) );
        }
    }
    if (b <= 0.f || a >= (float)n) {
        return s;
    }
    a = std::max(a, 0.f);
    b = std::min(b, (float)n);
    const int ia = (int)a, ib = (int)b;
    if (ia == ib) {
        return s + v[ia] * (b - a);
    }
    s += P[ib] - P[ia + 1];                  // whole pixels ia+1 .. ib-1
    s += v[ia] * ( (float)(ia + 1) - a );    // partial first pixel
    if (ib < n) {
        s += v[ib] * ( b - (float)ib );      // partial last pixel
    }

    return s;
}


// ---------------------------------------------------------------- 4-channel buffer convolution

// Input buffer (interleaved RGBA float) with per-row prefix sums.
struct Buffer
{
    int x0 = 0, y0 = 0, w = 0, h = 0;  // pixel coordinates of the buffer origin, size
    std::vector<float> V;              // w * h * 4
    std::vector<float> P;              // (w + 1) * h * 4, P[x] = sum of V[0 .. x-1]

    void allocate(int x0_, int y0_, int w_, int h_)
    {
        x0 = x0_; y0 = y0_; w = w_; h = h_;
        V.assign( (size_t)w * h * 4, 0.f );
        P.assign( (size_t)(w + 1) * h * 4, 0.f );
    }

    float* row(int r) { return &V[(size_t)r * w * 4]; }
    const float* row(int r) const { return &V[(size_t)r * w * 4]; }
    const float* prow(int r) const { return &P[(size_t)r * (w + 1) * 4]; }

    void prefixRows(int r0, int r1)
    {
        for (int r = r0; r < r1; ++r) {
            const float* v = row(r);
            float* p = &P[(size_t)r * (w + 1) * 4];
            p[0] = p[1] = p[2] = p[3] = 0.f;
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 4; ++c) {
                    p[(x + 1) * 4 + c] = p[x * 4 + c] + v[x * 4 + c];
                }
            }
        }
    }
};

// generic (border) span sum for 4 channels; extend repeats the edge pixels
inline void
spanSum4(const float* P, const float* v, int n, float a, float b, bool extend, float w, float* out)
{
    a += 0.5f;
    b += 0.5f;
    if (extend) {
        if (a < 0.f) {
            const float k = w * ( std::min(b, 0.f) - a );
            for (int c = 0; c < 4; ++c) {
                out[c] += v[c] * k;
            }
        }
        if (b > (float)n) {
            const float k = w * ( b - std::max(a, (float)n) );
            for (int c = 0; c < 4; ++c) {
                out[c] += v[(n - 1) * 4 + c] * k;
            }
        }
    }
    if (b <= 0.f || a >= (float)n) {
        return;
    }
    a = std::max(a, 0.f);
    b = std::min(b, (float)n);
    const int ia = (int)a, ib = (int)b;
    if (ia == ib) {
        for (int c = 0; c < 4; ++c) {
            out[c] += w * v[ia * 4 + c] * (b - a);
        }

        return;
    }
    const float fa = (float)(ia + 1) - a, fb = b - (float)ib;
    for (int c = 0; c < 4; ++c) {
        float s = P[ib * 4 + c] - P[(ia + 1) * 4 + c] + v[ia * 4 + c] * fa;
        if (ib < n) {
            s += v[ib * 4 + c] * fb;
        }
        out[c] += w * s;
    }
}

// Convolves output row y (absolute pixel row) for x in [x1, x2) into acc (4 floats per pixel, overwritten).
inline void
convolveRow(const Buffer& B, const Kernel& K, bool extend, int y, int x1, int x2, float* acc)
{
    const int n = x2 - x1;
    std::fill(acc, acc + (size_t)n * 4, 0.f);
    if ( (B.w <= 0) || (B.h <= 0) ) {
        return;
    }
    for (const KernelRow& r : K.rows) {
        int ry = y + r.dy - B.y0;
        if ( (ry < 0) || (ry >= B.h) ) {
            if (!extend) {
                continue;
            }
            ry = std::max( 0, std::min(B.h - 1, ry) );
        }
        const float* P = B.prow(ry);
        const float* v = B.row(ry);
        // a = bx + left + 0.5: integer part moves with x, fraction is constant on this row
        const float A = r.left + 0.5f, Bf = r.right + 0.5f;
        const int ia0 = (int)std::floor(A), ib0 = (int)std::floor(Bf);
        const float fa = (float)(ia0 + 1) - A, fb = Bf - (float)ib0, wgt = r.weight;
        const bool single = (ib0 == ia0);
        // interior: 0 <= bx + ia0 and bx + ib0 <= w - 1
        const int bxS = std::max(x1 - B.x0, -ia0), bxE = std::min(x2 - B.x0, B.w - 1 - ib0);
        const int xi1 = std::max( x1, std::min(x2, B.x0 + bxS) );            // interior start
        const int xi2 = std::max( xi1, std::min(x2, B.x0 + bxE) );           // interior end
        for (int x = x1; x < xi1; ++x) {
            const int bx = x - B.x0;
            spanSum4(P, v, B.w, bx + r.left, bx + r.right, extend, wgt, acc + (size_t)(x - x1) * 4);
        }
        if (single) {
            const float k = wgt * (Bf - A);
            for (int x = xi1; x < xi2; ++x) {
                const float* va = v + (x - B.x0 + ia0) * 4;
                float* o = acc + (size_t)(x - x1) * 4;
                for (int c = 0; c < 4; ++c) {
                    o[c] += va[c] * k;
                }
            }
        } else {
            const float wa = wgt * fa, wb = wgt * fb;
            for (int x = xi1; x < xi2; ++x) {
                const int bx = x - B.x0;
                const float* pb = P + (bx + ib0) * 4;
                const float* pa = P + (bx + ia0 + 1) * 4;
                const float* va = v + (bx + ia0) * 4;
                const float* vb = v + (bx + ib0) * 4;
                float* o = acc + (size_t)(x - x1) * 4;
                for (int c = 0; c < 4; ++c) {
                    o[c] += wgt * (pb[c] - pa[c]) + va[c] * wa + vb[c] * wb;
                }
            }
        }
        for (int x = xi2; x < x2; ++x) {
            const int bx = x - B.x0;
            spanSum4(P, v, B.w, bx + r.left, bx + r.right, extend, wgt, acc + (size_t)(x - x1) * 4);
        }
    }
}
} // namespace APFXDefocus

#endif // APFX_DEFOCUSMATH_H
