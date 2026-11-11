/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// Small radix-2 complex FFT (iterative, in place) + 2D helpers for image convolution.

#ifndef APFX_FFT_H
#define APFX_FFT_H

#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace APFXFFT {

typedef std::complex<float> cf;

inline int
nextPow2(int n)
{
    int p = 1;
    while (p < n) {
        p <<= 1;
    }

    return p;
}

// in-place FFT of n (power of 2) values with stride; inverse not normalised
inline void
fft1d(cf* a, int n, int stride, bool inverse, std::vector<cf>& tmp)
{
    tmp.resize(n);
    for (int i = 0; i < n; ++i) {
        tmp[i] = a[(size_t)i * stride];
    }
    // bit reversal
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(tmp[i], tmp[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2. * M_PI / len * (inverse ? 1. : -1.);
        const cf wl( (float)std::cos(ang), (float)std::sin(ang) );
        for (int i = 0; i < n; i += len) {
            cf w(1.f, 0.f);
            const int half = len >> 1;
            for (int k = 0; k < half; ++k) {
                const cf u = tmp[i + k], v = tmp[i + k + half] * w;
                tmp[i + k] = u + v;
                tmp[i + k + half] = u - v;
                w *= wl;
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        a[(size_t)i * stride] = tmp[i];
    }
}

// parallelFor(fn(begin, end), count)
typedef std::function<void(const std::function<void(int, int)>&, int)> PF;

// 2D FFT of a W x H (powers of 2) row-major buffer
inline void
fft2d(std::vector<cf>& a, int W, int H, bool inverse, const PF& pf)
{
    pf([&](int y0, int y1) {
        std::vector<cf> tmp;
        for (int y = y0; y < y1; ++y) {
            fft1d(&a[(size_t)y * W], W, 1, inverse, tmp);
        }
    }, H);
    pf([&](int x0, int x1) {
        std::vector<cf> tmp;
        for (int x = x0; x < x1; ++x) {
            fft1d(&a[x], H, W, inverse, tmp);
        }
    }, W);
}
} // namespace APFXFFT

#endif // APFX_FFT_H
