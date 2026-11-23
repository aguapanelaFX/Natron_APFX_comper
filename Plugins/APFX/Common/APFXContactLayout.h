/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// Contact sheet layouts for images of any shape (square, 16:9, 21:9, portrait ...). Images are
// never stretched: every placement keeps the aspect of its image.
//  rows (auto): justified rows like a photo gallery. Images keep their order, every row is as tall as
//               it needs to fill the width, the number of rows is picked to show the images biggest
//  grid (auto): equal cells, rows / columns picked to show the images biggest
//  grid: given rows / columns
// Coordinates: output pixels, y up (OFX), origin bottom-left. Order: top-left first.

#ifndef APFX_CONTACTLAYOUT_H
#define APFX_CONTACTLAYOUT_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace APFXCS {

enum Mode { eModeRows = 0, eModeAutoGrid, eModeGrid };
enum Fit { eFitFit = 0, eFitFill, eFitNone };

struct Rect
{
    double x1 = 0., y1 = 0., x2 = 0., y2 = 0.;
    double w() const { return x2 - x1; }
    double h() const { return y2 - y1; }
};

struct Placement
{
    Rect dst;      // where the image goes (output pixels)
    Rect clip;     // cell (fill mode crops to it)
};

struct Settings
{
    int mode = eModeRows;
    int fit = eFitFit;
    int rows = 0, cols = 0;    // grid mode (0 = auto for that one)
    double gap = 16., margin = 16.;
    bool center = true;
};

// image of aspect a (width / height) placed in a cell
inline Placement
fitInCell(const Rect& cell, double a, double srcW, int fit, bool center)
{
    Placement p;
    p.clip = cell;
    double w, h;
    if (fit == eFitNone) {
        w = srcW;
        h = srcW / a;
    } else {
        const bool wide = a * cell.h() > cell.w();
        if ( (fit == eFitFit) == wide ) {
            w = cell.w();
            h = w / a;
        } else {
            h = cell.h();
            w = h * a;
        }
    }
    const double ox = center ? 0.5 * (cell.w() - w) : 0.;
    const double oy = center ? 0.5 * (cell.h() - h) : (cell.h() - h);   // not centred: top-left
    p.dst.x1 = cell.x1 + ox;
    p.dst.y1 = cell.y1 + oy;
    p.dst.x2 = p.dst.x1 + w;
    p.dst.y2 = p.dst.y1 + h;

    return p;
}

// shown area of a grid of r x c cells
inline double
gridArea(const std::vector<double>& a, double W, double H, int r, int c, double gap)
{
    const double cw = (W - gap * (c - 1)) / c, ch = (H - gap * (r - 1)) / r;
    if ( (cw <= 0.) || (ch <= 0.) ) {
        return 0.;
    }
    double s = 0.;
    for (size_t i = 0; i < a.size(); ++i) {
        const double w = std::min(cw, ch * a[i]);
        s += w * (w / a[i]);
    }

    return s;
}

// splits the sequence of aspects into k rows with balanced aspect sums (linear partition)
inline std::vector<int>
partition(const std::vector<double>& a, int k)
{
    const int n = (int)a.size();
    k = std::max( 1, std::min(k, n) );
    std::vector<double> pre(n + 1, 0.);
    for (int i = 0; i < n; ++i) {
        pre[i + 1] = pre[i] + a[i];
    }
    // dp[j][i]: best (minimal largest row) for the first i images in j rows
    const double INF = 1e300;
    std::vector<std::vector<double> > dp( k + 1, std::vector<double>(n + 1, INF) );
    std::vector<std::vector<int> > cut( k + 1, std::vector<int>(n + 1, 0) );
    dp[0][0] = 0.;
    for (int j = 1; j <= k; ++j) {
        for (int i = j; i <= n; ++i) {
            for (int s = j - 1; s < i; ++s) {
                const double v = std::max(dp[j - 1][s], pre[i] - pre[s]);
                if (v < dp[j][i]) {
                    dp[j][i] = v;
                    cut[j][i] = s;
                }
            }
        }
    }
    std::vector<int> starts(k);
    int i = n;
    for (int j = k; j >= 1; --j) {
        starts[j - 1] = cut[j][i];
        i = cut[j][i];
    }

    return starts;   // first image index of each row
}

// justified rows; returns the shown area, fills 'out' when not NULL
inline double
rowsLayout(const std::vector<double>& a, const Rect& area, int k, double gap, bool center, std::vector<Placement>* out)
{
    const int n = (int)a.size();
    const std::vector<int> starts = partition(a, k);
    k = (int)starts.size();
    const double W = area.w(), H = area.h();
    std::vector<double> rowH(k);
    double sumH = 0.;
    for (int r = 0; r < k; ++r) {
        const int i0 = starts[r], i1 = (r + 1 < k) ? starts[r + 1] : n;
        double s = 0.;
        for (int i = i0; i < i1; ++i) {
            s += a[i];
        }
        rowH[r] = (W - gap * (i1 - i0 - 1)) / std::max(1e-9, s);
        sumH += rowH[r];
    }
    const double availH = H - gap * (k - 1);
    if ( (availH <= 0.) || (W <= 0.) ) {
        return 0.;
    }
    const double sc = std::min(1., availH / sumH);   // too tall: everything smaller (rows narrower, centred)
    double used = 0.;
    for (int r = 0; r < k; ++r) {
        const int i0 = starts[r], i1 = (r + 1 < k) ? starts[r + 1] : n;
        for (int i = i0; i < i1; ++i) {
            const double h = rowH[r] * sc;
            used += h * h * a[i];
        }
    }
    if (out) {
        out->assign( n, Placement() );
        const double totalH = sumH * sc + gap * (k - 1);
        double y = area.y2 - (center ? 0.5 * (H - totalH) : 0.);
        for (int r = 0; r < k; ++r) {
            const int i0 = starts[r], i1 = (r + 1 < k) ? starts[r + 1] : n;
            const double h = rowH[r] * sc;
            double rowW = gap * (i1 - i0 - 1);
            for (int i = i0; i < i1; ++i) {
                rowW += h * a[i];
            }
            double x = area.x1 + (center ? 0.5 * (W - rowW) : 0.);
            for (int i = i0; i < i1; ++i) {
                Placement& p = (*out)[i];
                p.dst.x1 = x;
                p.dst.x2 = x + h * a[i];
                p.dst.y2 = y;
                p.dst.y1 = y - h;
                p.clip = p.dst;
                x = p.dst.x2 + gap;
            }
            y -= h + gap;
        }
    }

    return used;
}

// a: aspect (width / height, pixel aspect included) of every image, srcW: their widths (fit none).
// Returns the placements and the rows / columns used.
inline std::vector<Placement>
layout(const std::vector<double>& a, const std::vector<double>& srcW, double W, double H, const Settings& s,
       int* rowsOut = NULL, int* colsOut = NULL)
{
    std::vector<Placement> out;
    const int n = (int)a.size();
    if (n == 0) {
        return out;
    }
    Rect area;
    area.x1 = s.margin;
    area.y1 = s.margin;
    area.x2 = W - s.margin;
    area.y2 = H - s.margin;
    if ( (area.w() <= 1.) || (area.h() <= 1.) ) {
        area.x1 = 0.; area.y1 = 0.; area.x2 = W; area.y2 = H;
    }
    if (s.mode == eModeRows) {
        int bestK = 1;
        double best = -1.;
        for (int k = 1; k <= n; ++k) {
            const double v = rowsLayout(a, area, k, s.gap, s.center, NULL);
            if (v > best * 1.0001) {
                best = v;
                bestK = k;
            }
        }
        rowsLayout(a, area, bestK, s.gap, s.center, &out);
        if (rowsOut) { *rowsOut = bestK; }
        if (colsOut) { *colsOut = 0; }

        return out;
    }
    int r = s.rows, c = s.cols;
    if (s.mode == eModeAutoGrid) {
        r = c = 0;
    }
    if ( (r <= 0) && (c <= 0) ) {
        double best = -1.;
        for (int cc = 1; cc <= n; ++cc) {
            const int rr = (n + cc - 1) / cc;
            const double v = gridArea(a, area.w(), area.h(), rr, cc, s.gap);
            if (v > best * 1.0001) {
                best = v;
                r = rr;
                c = cc;
            }
        }
    } else if (r <= 0) {
        r = (n + c - 1) / c;
    } else if (c <= 0) {
        c = (n + r - 1) / r;
    }
    const double cw = (area.w() - s.gap * (c - 1)) / c, ch = (area.h() - s.gap * (r - 1)) / r;
    out.resize(n);
    for (int i = 0; i < n; ++i) {
        const int row = i / c, col = i % c;
        Rect cell;
        if (row >= r) {
            out[i].dst = Rect();   // more images than cells: not shown
            continue;
        }
        cell.x1 = area.x1 + col * (cw + s.gap);
        cell.x2 = cell.x1 + cw;
        cell.y2 = area.y2 - row * (ch + s.gap);
        cell.y1 = cell.y2 - ch;
        out[i] = fitInCell(cell, a[i], srcW[i], s.fit, s.center);
    }
    if (rowsOut) { *rowsOut = r; }
    if (colsOut) { *colsOut = c; }

    return out;
}
} // namespace APFXCS

#endif // APFX_CONTACTLAYOUT_H
