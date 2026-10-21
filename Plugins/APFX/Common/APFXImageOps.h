/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX image operations shared by EdgeExtend, ExpoGlow and DifScreenColour:
// planes, fast erode (van Herk / Gil-Werman), fast blur (repeated box), and image
// pyramids (2x down / up with tent filters): pull-push fill and multi-scale glow.
// Every operation is O(pixels), independent of its size parameter.

#ifndef APFX_IMAGEOPS_H
#define APFX_IMAGEOPS_H

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace APFXOps {

// runs fn(begin, end) over [0, n) on several threads (set by the caller)
typedef std::function<void(const std::function<void(int, int)>&, int)> ParallelFor;

inline void
serialFor(const std::function<void(int, int)>& fn, int n)
{
    fn(0, n);
}

struct Plane
{
    int w = 0, h = 0, c = 0;
    std::vector<float> d;

    Plane() {}
    Plane(int w_, int h_, int c_, float v = 0.f) : w(w_), h(h_), c(c_), d( (size_t)w_ * h_ * c_, v ) {}
    void resize(int w_, int h_, int c_, float v = 0.f) { w = w_; h = h_; c = c_; d.assign( (size_t)w * h * c, v ); }
    float* px(int x, int y) { return &d[( (size_t)y * w + x ) * c]; }
    const float* px(int x, int y) const { return &d[( (size_t)y * w + x ) * c]; }
};

// ---------------------------------------------------------------- 1D helpers

// running min over a window of 2r+1 (van Herk / Gil-Werman), clamped borders
inline void
minLine(const float* in, int n, int stride, int r, float* out, int ostride, std::vector<float>& g, std::vector<float>& hb)
{
    if (r <= 0) {
        for (int i = 0; i < n; ++i) {
            out[i * ostride] = in[i * stride];
        }

        return;
    }
    const int k = 2 * r + 1;
    const int m = n + 2 * r;
    const int mp = ( (m + k - 1) / k ) * k;
    g.resize(mp);
    hb.resize(mp);
    auto at = [&](int i) {
        i -= r;
        i = std::max( 0, std::min(n - 1, i) );

        return in[i * stride];
    };
    for (int b = 0; b < mp; b += k) {
        g[b] = at(b);
        for (int i = 1; i < k; ++i) {
            g[b + i] = std::min( g[b + i - 1], at(b + i) );
        }
        hb[b + k - 1] = at(b + k - 1);
        for (int i = k - 2; i >= 0; --i) {
            hb[b + i] = std::min( hb[b + i + 1], at(b + i) );
        }
    }
    for (int i = 0; i < n; ++i) {
        // window [i, i + 2r] in padded coordinates
        out[i * ostride] = std::min( hb[i], g[i + k - 1] );
    }
}

// box blur of radius r along a line (running sum, clamped borders)
inline void
boxLine(const float* in, int n, int stride, int r, float* out, int ostride)
{
    if (r <= 0) {
        for (int i = 0; i < n; ++i) {
            out[i * ostride] = in[i * stride];
        }

        return;
    }
    const float inv = 1.f / (2 * r + 1);
    auto at = [&](int i) { return in[std::max( 0, std::min(n - 1, i) ) * stride]; };
    float s = 0.f;
    for (int i = -r; i <= r; ++i) {
        s += at(i);
    }
    for (int i = 0; i < n; ++i) {
        out[i * ostride] = s * inv;
        s += at(i + r + 1) - at(i - r);
    }
}

// ---------------------------------------------------------------- 2D operations (single channel planes)

// square erosion of radius r (min filter)
inline void
erode(Plane& p, int r, const ParallelFor& pf = serialFor)
{
    if (r <= 0 || p.c != 1) {
        return;
    }
    Plane tmp(p.w, p.h, 1);
    pf([&](int y0, int y1) {
        std::vector<float> g, hb;
        for (int y = y0; y < y1; ++y) {
            minLine(p.px(0, y), p.w, 1, r, tmp.px(0, y), 1, g, hb);
        }
    }, p.h);
    pf([&](int x0, int x1) {
        std::vector<float> g, hb;
        for (int x = x0; x < x1; ++x) {
            minLine(tmp.px(x, 0), p.h, p.w, r, p.px(x, 0), p.w, g, hb);
        }
    }, p.w);
}

// one horizontal box pass (radius r) of rows [y0, y1), interleaved channels, clamped borders
inline void
boxRows(const Plane& in, Plane& out, int r, int y0, int y1)
{
    const int w = in.w, c = in.c;
    const float inv = 1.f / (2 * r + 1);
    std::vector<float> sum(c);
    for (int y = y0; y < y1; ++y) {
        const float* row = in.px(0, y);
        float* o = out.px(0, y);
        for (int k = 0; k < c; ++k) {
            float s = 0.f;
            for (int i = -r; i <= r; ++i) {
                s += row[std::max( 0, std::min(w - 1, i) ) * c + k];
            }
            sum[k] = s;
        }
        for (int x = 0; x < w; ++x) {
            const int xa = std::min(w - 1, x + r + 1) * c, xs = std::max(0, x - r) * c;
            for (int k = 0; k < c; ++k) {
                o[x * c + k] = sum[k] * inv;
                sum[k] += row[xa + k] - row[xs + k];
            }
        }
    }
}

// one vertical box pass (radius r) over the columns [x0, x1) (in elements = x * c), all rows
inline void
boxCols(const Plane& in, Plane& out, int r, int e0, int e1)
{
    const int h = in.h, stride = in.w * in.c, n = e1 - e0;
    const float inv = 1.f / (2 * r + 1);
    std::vector<float> sum(n, 0.f);
    auto row = [&](int y) { return &in.d[(size_t)std::max( 0, std::min(h - 1, y) ) * stride + e0]; };
    for (int i = -r; i <= r; ++i) {
        const float* rr = row(i);
        for (int j = 0; j < n; ++j) {
            sum[j] += rr[j];
        }
    }
    for (int y = 0; y < h; ++y) {
        float* o = &out.d[(size_t)y * stride + e0];
        const float* ra = row(y + r + 1);
        const float* rs = row(y - r);
        for (int j = 0; j < n; ++j) {
            o[j] = sum[j] * inv;
            sum[j] += ra[j] - rs[j];
        }
    }
}

// approximate gaussian blur (3 box passes) of a plane with any channel count
inline void
blur(Plane& p, float sigma, const ParallelFor& pf = serialFor)
{
    if (sigma < 0.3f) {
        return;
    }
    // box radius for 3 passes: variance of a box of radius r is r(r+1)/3
    const int r = std::max( 1, (int)std::floor(std::sqrt(sigma * sigma + 1.f) - 0.5f) );
    Plane tmp(p.w, p.h, p.c);
    const int elems = p.w * p.c;
    for (int pass = 0; pass < 3; ++pass) {
        pf([&](int y0, int y1) { boxRows(p, tmp, r, y0, y1); }, p.h);
        // columns in chunks of 64 elements per task
        const int chunks = (elems + 63) / 64;
        pf([&](int c0, int c1) {
            boxCols( tmp, p, r, c0 * 64, std::min(elems, c1 * 64) );
        }, chunks);
    }
}

// ---------------------------------------------------------------- pyramid

// 2x downsample with a separable [1 3 3 1]/8 tent (anti-aliased), clamped borders
inline Plane
downsample(const Plane& in, const ParallelFor& pf = serialFor)
{
    const int w = std::max(1, (in.w + 1) / 2), h = std::max(1, (in.h + 1) / 2), c = in.c;
    Plane out(w, h, c);
    const float k[4] = { 1.f / 8.f, 3.f / 8.f, 3.f / 8.f, 1.f / 8.f };
    const int rowElems = in.w * c;
    pf([&](int y0, int y1) {
        std::vector<float> tmp(rowElems);
        for (int y = y0; y < y1; ++y) {
            // vertical: 4 input rows -> one row
            const float* r[4];
            for (int t = 0; t < 4; ++t) {
                r[t] = in.px( 0, std::max( 0, std::min(in.h - 1, 2 * y - 1 + t) ) );
            }
            for (int j = 0; j < rowElems; ++j) {
                tmp[j] = k[0] * r[0][j] + k[1] * r[1][j] + k[2] * r[2][j] + k[3] * r[3][j];
            }
            // horizontal
            float* o = out.px(0, y);
            for (int x = 0; x < w; ++x) {
                const int x0 = std::max(0, 2 * x - 1) * c, x1 = std::min(in.w - 1, 2 * x) * c;
                const int x2 = std::min(in.w - 1, 2 * x + 1) * c, x3 = std::min(in.w - 1, 2 * x + 2) * c;
                for (int ch = 0; ch < c; ++ch) {
                    o[x * c + ch] = k[0] * tmp[x0 + ch] + k[1] * tmp[x1 + ch] + k[2] * tmp[x2 + ch] + k[3] * tmp[x3 + ch];
                }
            }
        }
    }, h);

    return out;
}

// bilinear 2x upsample of one output row y (width w) of 'in' into 'out' (w * in.c floats)
struct UpsampleX
{
    std::vector<int> i0, i1;
    std::vector<float> a;
    void init(int inW, int outW)
    {
        i0.resize(outW); i1.resize(outW); a.resize(outW);
        for (int x = 0; x < outW; ++x) {
            float sx = (x + 0.5f) * 0.5f - 0.5f;
            sx = std::max( 0.f, std::min( (float)inW - 1.f, sx ) );
            i0[x] = (int)sx;
            i1[x] = std::min(inW - 1, i0[x] + 1);
            a[x] = sx - i0[x];
        }
    }
};

inline void
upsampleRow(const Plane& in, const UpsampleX& ux, int y, int outW, float* out)
{
    float sy = (y + 0.5f) * 0.5f - 0.5f;
    sy = std::max( 0.f, std::min( (float)in.h - 1.f, sy ) );
    const int y0 = (int)sy, y1 = std::min(in.h - 1, y0 + 1);
    const float ay = sy - y0;
    const float* r0 = in.px(0, y0);
    const float* r1 = in.px(0, y1);
    const int c = in.c;
    for (int x = 0; x < outW; ++x) {
        const int a0 = ux.i0[x] * c, a1 = ux.i1[x] * c;
        const float ax = ux.a[x];
        for (int ch = 0; ch < c; ++ch) {
            const float top = r0[a0 + ch] + (r0[a1 + ch] - r0[a0 + ch]) * ax;
            const float bot = r1[a0 + ch] + (r1[a1 + ch] - r1[a0 + ch]) * ax;
            out[x * c + ch] = top + (bot - top) * ay;
        }
    }
}

// bilinear 2x upsample of 'in' to (w, h) — the matching up filter of downsample()
inline void
upsampleAt(const Plane& in, float fx, float fy, float* out)
{
    float sx = (fx + 0.5f) * 0.5f - 0.5f, sy = (fy + 0.5f) * 0.5f - 0.5f;
    sx = std::max( 0.f, std::min( (float)in.w - 1.f, sx ) );
    sy = std::max( 0.f, std::min( (float)in.h - 1.f, sy ) );
    const int x0 = (int)sx, y0 = (int)sy;
    const int x1 = std::min(in.w - 1, x0 + 1), y1 = std::min(in.h - 1, y0 + 1);
    const float ax = sx - x0, ay = sy - y0;
    const float* a = in.px(x0, y0);
    const float* b = in.px(x1, y0);
    const float* c = in.px(x0, y1);
    const float* d = in.px(x1, y1);
    for (int ch = 0; ch < in.c; ++ch) {
        const float top = a[ch] + (b[ch] - a[ch]) * ax;
        const float bot = c[ch] + (d[ch] - c[ch]) * ax;
        out[ch] = top + (bot - top) * ay;
    }
}

// Pull-push fill. 'cw' holds colour * weight (cc channels) followed by the weight
// (so cc + 1 channels). Returns the filled straight colour (cc channels) everywhere:
// known pixels keep their colour, unknown ones get a smooth extension from the nearest
// known colours, whatever the distance.
inline Plane
pullPush(Plane cw, int maxLevels = 32, const ParallelFor& pf = serialFor)
{
    const int cc = cw.c - 1;
    std::vector<Plane> lv;
    lv.reserve(32);
    lv.push_back( std::move(cw) );
    while ( (int)lv.size() < maxLevels && (lv.back().w > 1 || lv.back().h > 1) ) {
        lv.push_back( downsample(lv.back(), pf) );
    }
    // coarsest level: normalised colour
    Plane fill( lv.back().w, lv.back().h, cc );
    {
        const Plane& L = lv.back();
        for (int y = 0; y < L.h; ++y) {
            for (int x = 0; x < L.w; ++x) {
                const float* s = L.px(x, y);
                float* o = fill.px(x, y);
                const float wgt = s[cc];
                for (int ch = 0; ch < cc; ++ch) {
                    o[ch] = (wgt > 1e-8f) ? s[ch] / wgt : 0.f;
                }
            }
        }
    }
    // push: finer level = its own colour where it has weight, else the coarser fill
    for (int li = (int)lv.size() - 2; li >= 0; --li) {
        const Plane& L = lv[li];
        Plane next(L.w, L.h, cc);
        const bool base = (li == 0);
        UpsampleX ux;
        ux.init(fill.w, L.w);
        pf([&](int y0, int y1) {
            std::vector<float> up( (size_t)L.w * cc );
            for (int y = y0; y < y1; ++y) {
                upsampleRow(fill, ux, y, L.w, up.data());
                for (int x = 0; x < L.w; ++x) {
                    const float* s = L.px(x, y);
                    float* o = next.px(x, y);
                    const float* u = &up[(size_t)x * cc];
                    const float wgt = s[cc];
                    // confidence of this level's own colour
                    const float a = base ? std::max( 0.f, std::min(1.f, wgt) ) : std::max( 0.f, std::min(1.f, wgt * 2.f) );
                    const float inv = (wgt > 1e-8f) ? a / wgt : 0.f;
                    for (int ch = 0; ch < cc; ++ch) {
                        o[ch] = s[ch] * inv + u[ch] * (1.f - a);
                    }
                }
            }
        }, L.h);
        fill.d.swap(next.d);
        fill.w = next.w;
        fill.h = next.h;
    }

    return fill;
}

// Multi-scale (exponential) glow of a plane: sum of the pyramid levels 1..levels,
// each lightly blurred, weighted by falloff^(level-1) (like stacking blurs of doubling
// size). Returns a plane of the input size.
inline Plane
expoGlow(const Plane& src, int levels, float falloff, const ParallelFor& pf = serialFor)
{
    levels = std::max(1, levels);
    std::vector<Plane> lv;
    lv.push_back( downsample(src, pf) );
    while ( (int)lv.size() < levels && (lv.back().w > 2 || lv.back().h > 2) ) {
        lv.push_back( downsample(lv.back(), pf) );
    }
    const int n = (int)lv.size();
    std::vector<float> wgt(n);
    float wsum = 0.f;
    for (int i = 0; i < n; ++i) {
        wgt[i] = std::pow(std::max(0.f, falloff), (float)i);
        wsum += wgt[i];
    }
    (void)wsum;
    for (int i = 0; i < n; ++i) {
        blur(lv[i], 1.f, pf); // soften each level: no blocky upsampling
    }
    // accumulate from the coarsest level: acc(i) = w(i) * level(i) + up(acc(i+1))
    Plane acc = lv[n - 1];
    for (float& v : acc.d) {
        v *= wgt[n - 1];
    }
    for (int i = n - 2; i >= 0; --i) {
        const Plane& L = lv[i];
        Plane next(L.w, L.h, L.c);
        UpsampleX ux;
        ux.init(acc.w, L.w);
        pf([&](int y0, int y1) {
            std::vector<float> up( (size_t)L.w * L.c );
            for (int y = y0; y < y1; ++y) {
                upsampleRow(acc, ux, y, L.w, up.data());
                const float* s = L.px(0, y);
                float* o = next.px(0, y);
                const int n = L.w * L.c;
                for (int j = 0; j < n; ++j) {
                    o[j] = s[j] * wgt[i] + up[j];
                }
            }
        }, L.h);
        acc.d.swap(next.d);
        acc.w = next.w;
        acc.h = next.h;
    }
    // back to full resolution
    Plane out(src.w, src.h, src.c);
    UpsampleX ux;
    ux.init(acc.w, src.w);
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            upsampleRow( acc, ux, y, src.w, out.px(0, y) );
        }
    }, src.h);

    return out;
}
} // namespace APFXOps

#endif // APFX_IMAGEOPS_H
