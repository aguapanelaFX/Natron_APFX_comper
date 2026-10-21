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

} // namespace APFXDefocus

#endif // APFX_DEFOCUSMATH_H
