/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX noise math: gradient / cellular / convolution noises in 3D and 4D
// (4D is used for seamless time loops), plus fractal layering.
// Speed: lattice hashing uses a seeded permutation table (like Ken Perlin's
// reference noise) and per-cell random tables, all in float.
// Every basis returns a SIGNED value, roughly in [-1, 1].

#ifndef APFX_NOISEMATH_H
#define APFX_NOISEMATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace APFXNoise {

enum BasisEnum
{
    eBasisPerlin = 0,
    eBasisSimplex,
    eBasisSparseConvolution,
    eBasisAlligator,
    eBasisWorleyF1,
    eBasisWorleyF2,
    eBasisWorleyF2F1,
    eBasisWhite,
    eBasisCount
};

enum MetricEnum
{
    eMetricEuclidean = 0,
    eMetricManhattan,
    eMetricChebyshev
};

enum FractalEnum
{
    eFractalNone = 0,
    eFractalFBM,
    eFractalTurbulence,
    eFractalRidged,
    eFractalTerrain,
    eFractalHybridTerrain
};

// ---------------------------------------------------------------- seeded tables

inline uint32_t
hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;

    return x;
}

struct Perm
{
    uint8_t p[512];        // permutation of 0..255, repeated
    float r0[256], r1[256], r2[256], r3[256], r4[256]; // per-cell random numbers in [0, 1)

    explicit Perm(uint32_t seed = 0)
    {
        uint32_t s = hash32(seed * 0x9e3779b9U + 0x632be5abU);
        for (int i = 0; i < 256; ++i) {
            p[i] = (uint8_t)i;
        }
        for (int i = 255; i > 0; --i) {
            s = hash32(s + 0x6d2b79f5U);
            const int j = (int)(s % (uint32_t)(i + 1));
            std::swap(p[i], p[j]);
        }
        for (int i = 0; i < 256; ++i) {
            p[256 + i] = p[i];
        }
        float* tabs[5] = { r0, r1, r2, r3, r4 };
        for (int t = 0; t < 5; ++t) {
            for (int i = 0; i < 256; ++i) {
                s = hash32(s + 0x27d4eb2dU);
                tabs[t][i] = (s >> 8) * (1.0f / 16777216.0f);
            }
        }
    }

    inline int h3(int x, int y, int z) const
    {
        return p[ p[ p[x & 255] + (y & 255) ] + (z & 255) ];
    }

    inline int h4(int x, int y, int z, int w) const
    {
        return p[ p[ p[ p[x & 255] + (y & 255) ] + (z & 255) ] + (w & 255) ];
    }
};

inline float
fade(float t)
{
    return t * t * t * ( t * (t * 6.f - 15.f) + 10.f );
}

inline float
lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline int
fastFloor(float x)
{
    const int i = (int)x;

    return (x < (float)i) ? i - 1 : i;
}

// ---------------------------------------------------------------- Perlin (improved)

inline float
grad3(int h, float x, float y, float z)
{
    // Ken Perlin's 12 edge gradients (16 entries)
    switch (h & 15) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    case 11: return -y - z;
    case 12: return y + x;
    case 13: return -y + z;
    case 14: return y - x;
    default: return -y - z;
    }
}

inline float
grad4(int h, float x, float y, float z, float w)
{
    // 32 gradients: one zero component, the others +-1
    const int k = h & 31;
    const float a = (k & 1) ? -1.f : 1.f, b = (k & 2) ? -1.f : 1.f, c = (k & 4) ? -1.f : 1.f;
    switch (k >> 3) {
    case 0: return a * y + b * z + c * w;
    case 1: return a * x + b * z + c * w;
    case 2: return a * x + b * y + c * w;
    default: return a * x + b * y + c * z;
    }
}

inline float
perlin3(const float* q, const Perm& P)
{
    const int X = fastFloor(q[0]), Y = fastFloor(q[1]), Z = fastFloor(q[2]);
    const float x = q[0] - X, y = q[1] - Y, z = q[2] - Z;
    const float u = fade(x), v = fade(y), w = fade(z);
    const int A = P.p[X & 255] + (Y & 255), AA = P.p[A] + (Z & 255), AB = P.p[A + 1] + (Z & 255);
    const int B = P.p[(X + 1) & 255] + (Y & 255), BA = P.p[B] + (Z & 255), BB = P.p[B + 1] + (Z & 255);
    const float r = lerpf( lerpf( lerpf( grad3(P.p[AA], x, y, z), grad3(P.p[BA], x - 1, y, z), u ),
                                  lerpf( grad3(P.p[AB], x, y - 1, z), grad3(P.p[BB], x - 1, y - 1, z), u ), v ),
                           lerpf( lerpf( grad3(P.p[AA + 1], x, y, z - 1), grad3(P.p[BA + 1], x - 1, y, z - 1), u ),
                                  lerpf( grad3(P.p[AB + 1], x, y - 1, z - 1), grad3(P.p[BB + 1], x - 1, y - 1, z - 1), u ), v ), w );

    return std::max( -1.f, std::min(1.f, r) );
}

inline float
perlin4(const float* q, const Perm& P)
{
    const int X = fastFloor(q[0]), Y = fastFloor(q[1]), Z = fastFloor(q[2]), W = fastFloor(q[3]);
    const float f[4] = { q[0] - X, q[1] - Y, q[2] - Z, q[3] - W };
    const float u[4] = { fade(f[0]), fade(f[1]), fade(f[2]), fade(f[3]) };
    float vals[16];
    for (int c = 0; c < 16; ++c) {
        const int bx = c & 1, by = (c >> 1) & 1, bz = (c >> 2) & 1, bw = (c >> 3) & 1;
        vals[c] = grad4( P.h4(X + bx, Y + by, Z + bz, W + bw), f[0] - bx, f[1] - by, f[2] - bz, f[3] - bw );
    }
    for (int k = 0; k < 8; ++k) {
        vals[k] = lerpf(vals[2 * k], vals[2 * k + 1], u[0]);
    }
    for (int k = 0; k < 4; ++k) {
        vals[k] = lerpf(vals[2 * k], vals[2 * k + 1], u[1]);
    }
    for (int k = 0; k < 2; ++k) {
        vals[k] = lerpf(vals[2 * k], vals[2 * k + 1], u[2]);
    }
    const float r = lerpf(vals[0], vals[1], u[3]) * 0.9f;

    return std::max( -1.f, std::min(1.f, r) );
}

// ---------------------------------------------------------------- Simplex (after S. Gustavson)

inline float
simplex3(const float* q, const Perm& P)
{
    const float F3 = 1.f / 3.f, G3 = 1.f / 6.f;
    const float s = (q[0] + q[1] + q[2]) * F3;
    const int i = fastFloor(q[0] + s), j = fastFloor(q[1] + s), k = fastFloor(q[2] + s);
    const float t = (i + j + k) * G3;
    const float x0 = q[0] - (i - t), y0 = q[1] - (j - t), z0 = q[2] - (k - t);
    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else {
        if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }
    const float xs[4] = { x0, x0 - i1 + G3, x0 - i2 + 2.f * G3, x0 - 1.f + 3.f * G3 };
    const float ys[4] = { y0, y0 - j1 + G3, y0 - j2 + 2.f * G3, y0 - 1.f + 3.f * G3 };
    const float zs[4] = { z0, z0 - k1 + G3, z0 - k2 + 2.f * G3, z0 - 1.f + 3.f * G3 };
    const int hs[4] = { P.h3(i, j, k), P.h3(i + i1, j + j1, k + k1), P.h3(i + i2, j + j2, k + k2), P.h3(i + 1, j + 1, k + 1) };
    float n = 0.f;
    for (int c = 0; c < 4; ++c) {
        float tt = 0.6f - xs[c] * xs[c] - ys[c] * ys[c] - zs[c] * zs[c];
        if (tt > 0.f) {
            tt *= tt;
            n += tt * tt * grad3(hs[c], xs[c], ys[c], zs[c]);
        }
    }

    return std::max( -1.f, std::min(1.f, 32.f * n) );
}

inline float
simplex4(const float* q, const Perm& P)
{
    const float F4 = 0.309016994f, G4 = 0.138196601f;
    const float s = (q[0] + q[1] + q[2] + q[3]) * F4;
    int c[4];
    for (int a = 0; a < 4; ++a) {
        c[a] = fastFloor(q[a] + s);
    }
    const float t = (c[0] + c[1] + c[2] + c[3]) * G4;
    float x0[4];
    for (int a = 0; a < 4; ++a) {
        x0[a] = q[a] - (c[a] - t);
    }
    int rank[4] = { 0, 0, 0, 0 };
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            if (x0[a] > x0[b]) {
                ++rank[a];
            } else {
                ++rank[b];
            }
        }
    }
    float n = 0.f;
    for (int corner = 0; corner < 5; ++corner) {
        int o[4];
        float d[4];
        for (int a = 0; a < 4; ++a) {
            o[a] = (corner == 0) ? 0 : ( (corner == 4) ? 1 : (rank[a] >= 4 - corner ? 1 : 0) );
            d[a] = x0[a] - o[a] + corner * G4;
        }
        float tt = 0.6f - d[0] * d[0] - d[1] * d[1] - d[2] * d[2] - d[3] * d[3];
        if (tt > 0.f) {
            tt *= tt;
            n += tt * tt * grad4( P.h4(c[0] + o[0], c[1] + o[1], c[2] + o[2], c[3] + o[3]), d[0], d[1], d[2], d[3] );
        }
    }

    return std::max( -1.f, std::min(1.f, 27.f * n) );
}

// ---------------------------------------------------------------- cellular family (3D / 4D)

template <int D>
inline int
cellHash(const Perm& P, const int* c)
{
    return (D == 4) ? P.h4(c[0], c[1], c[2], c[3]) : P.h3(c[0], c[1], c[2]);
}

template <int D>
inline float
dist(const float* d, int metric)
{
    if (metric == eMetricManhattan) {
        float r = 0.f;
        for (int i = 0; i < D; ++i) {
            r += std::fabs(d[i]);
        }

        return r;
    }
    if (metric == eMetricChebyshev) {
        float r = 0.f;
        for (int i = 0; i < D; ++i) {
            r = std::max( r, std::fabs(d[i]) );
        }

        return r;
    }
    float r = 0.f;
    for (int i = 0; i < D; ++i) {
        r += d[i] * d[i];
    }

    return std::sqrt(r);
}

// feature point offset in a cell: jitter 0 = centre, 1 = anywhere in the cell
template <int D>
inline void
feature(const Perm& P, int h, float jitter, float* off)
{
    const float* tabs[4] = { P.r0, P.r1, P.r2, P.r3 };
    for (int i = 0; i < D; ++i) {
        off[i] = 0.5f + (tabs[i][h] - 0.5f) * jitter;
    }
}

// visits the 3^D neighbour cells: fn(feature-to-point delta, hash)
// 3D: unrolled loops with incremental hashing (the hot path)
template <class F>
inline void
forCells3(const float* q, const Perm& P, float jitter, F fn)
{
    const int bx = fastFloor(q[0]), by = fastFloor(q[1]), bz = fastFloor(q[2]);
    const float fx = q[0] - bx, fy = q[1] - by, fz = q[2] - bz;
    const float j = jitter, c0 = 0.5f - 0.5f * jitter;
    for (int ox = -1; ox <= 1; ++ox) {
        const int px = P.p[(bx + ox) & 255];
        for (int oy = -1; oy <= 1; ++oy) {
            const int pxy = P.p[px + ( (by + oy) & 255 )];
            for (int oz = -1; oz <= 1; ++oz) {
                const int h = P.p[pxy + ( (bz + oz) & 255 )];
                const float d[3] = { ox + c0 + P.r0[h] * j - fx, oy + c0 + P.r1[h] * j - fy, oz + c0 + P.r2[h] * j - fz };
                fn(d, h);
            }
        }
    }
}

template <int D, class F>
inline void
forCellsN(const float* q, const Perm& P, float jitter, F fn)
{
    int base[D];
    float f[D];
    for (int i = 0; i < D; ++i) {
        base[i] = fastFloor(q[i]);
        f[i] = q[i] - base[i];
    }
    int n = 1;
    for (int i = 0; i < D; ++i) {
        n *= 3;
    }
    for (int k = 0; k < n; ++k) {
        int c[D], o[D];
        int r = k;
        for (int i = 0; i < D; ++i) {
            o[i] = (r % 3) - 1;
            c[i] = base[i] + o[i];
            r /= 3;
        }
        const int h = cellHash<D>(P, c);
        float off[D], d[D];
        feature<D>(P, h, jitter, off);
        for (int i = 0; i < D; ++i) {
            d[i] = o[i] + off[i] - f[i];
        }
        fn(d, h);
    }
}

template <int D, class F>
inline void
forCells(const float* q, const Perm& P, float jitter, F fn)
{
    if (D == 3) {
        forCells3(q, P, jitter, fn);
    } else {
        forCellsN<D>(q, P, jitter, fn);
    }
}

template <int D>
inline void
worley(const float* q, const Perm& P, float jitter, int metric, float* F1, float* F2)
{
    float f1 = 1e9f, f2 = 1e9f;
    if ( (D == 3) && (metric == eMetricEuclidean) ) {
        // 3D Euclidean hot path: own cell first, then skip the neighbour cells
        // that cannot hold a point closer than the current second distance
        const int bx = fastFloor(q[0]), by = fastFloor(q[1]), bz = fastFloor(q[2]);
        const float f[3] = { q[0] - bx, q[1] - by, q[2] - bz };
        const float c0 = 0.5f - 0.5f * jitter, c1 = c0 + jitter; // point range inside a cell
        float lo[3][3]; // [axis][offset+1] squared minimal distance along the axis
        for (int i = 0; i < 3; ++i) {
            const float dm = f[i] - (c1 - 1.f), dp = (1.f + c0) - f[i];
            lo[i][0] = dm > 0.f ? dm * dm : 0.f;
            lo[i][1] = 0.f;
            lo[i][2] = dp > 0.f ? dp * dp : 0.f;
        }
        static const int order[27][3] = {
            {0,0,0}, {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1},
            {-1,-1,0}, {-1,1,0}, {1,-1,0}, {1,1,0}, {-1,0,-1}, {-1,0,1}, {1,0,-1}, {1,0,1}, {0,-1,-1}, {0,-1,1}, {0,1,-1}, {0,1,1},
            {-1,-1,-1}, {-1,-1,1}, {-1,1,-1}, {-1,1,1}, {1,-1,-1}, {1,-1,1}, {1,1,-1}, {1,1,1}
        };
        for (int k = 0; k < 27; ++k) {
            const int ox = order[k][0], oy = order[k][1], oz = order[k][2];
            if (lo[0][ox + 1] + lo[1][oy + 1] + lo[2][oz + 1] >= f2) {
                continue;
            }
            const int h = P.h3(bx + ox, by + oy, bz + oz);
            const float dx = ox + c0 + P.r0[h] * jitter - f[0];
            const float dy = oy + c0 + P.r1[h] * jitter - f[1];
            const float dz = oz + c0 + P.r2[h] * jitter - f[2];
            const float r = dx * dx + dy * dy + dz * dz;
            if (r < f2) {
                if (r < f1) {
                    f2 = f1;
                    f1 = r;
                } else {
                    f2 = r;
                }
            }
        }
        *F1 = std::sqrt(f1);
        *F2 = std::sqrt(f2);

        return;
    }
    if (metric == eMetricEuclidean) {
        // squared distances, one sqrt at the end
        forCells<D>(q, P, jitter, [&](const float* d, int) {
            float r = 0.f;
            for (int i = 0; i < D; ++i) {
                r += d[i] * d[i];
            }
            if (r < f2) {
                if (r < f1) {
                    f2 = f1;
                    f1 = r;
                } else {
                    f2 = r;
                }
            }
        });
        *F1 = std::sqrt(f1);
        *F2 = std::sqrt(f2);

        return;
    }
    forCells<D>(q, P, jitter, [&](const float* d, int) {
        const float r = dist<D>(d, metric);
        if (r < f2) {
            if (r < f1) {
                f2 = f1;
                f1 = r;
            } else {
                f2 = r;
            }
        }
    });
    *F1 = f1;
    *F2 = f2;
}

// Alligator: each feature point carries a random height with a smooth falloff;
// the result is the difference between the two highest bumps (plates with creases)
template <int D>
inline float
alligator(const float* q, const Perm& P, float jitter, int metric)
{
    float b1 = 0.f, b2 = 0.f;
    forCells<D>(q, P, jitter, [&](const float* d, int h) {
        float fall = 1.f - dist<D>(d, metric);
        if (fall <= 0.f) {
            return;
        }
        fall = fall * fall * (3.f - 2.f * fall);
        const float b = P.r4[h] * fall;
        if (b > b1) {
            b2 = b1;
            b1 = b;
        } else if (b > b2) {
            b2 = b;
        }
    });

    return std::max( -1.f, std::min(1.f, (b1 - b2) * 2.6f - 1.f) );
}

// Sparse convolution: random impulses (2 per cell) convolved with a smooth kernel
template <int D>
inline float
sparseConvolution(const float* q, const Perm& P)
{
    float sum = 0.f;
    forCells<D>(q, P, 1.f, [&](const float* d, int h) {
        // impulse 1: the cell feature point; impulse 2: mirrored inside the cell
        float r2 = 0.f, r2b = 0.f;
        for (int i = 0; i < D; ++i) {
            r2 += d[i] * d[i];
            const float e = d[i] + (P.r4[(h + 17 * i) & 255] - 0.5f) * 0.8f;
            r2b += e * e;
        }
        if (r2 < 1.f) {
            const float t = 1.f - r2;
            sum += (P.r4[h] * 2.f - 1.f) * t * t * t;
        }
        if (r2b < 1.f) {
            const float t = 1.f - r2b;
            sum += (P.r3[h] * 2.f - 1.f) * t * t * t;
        }
    });

    return std::max( -1.f, std::min(1.f, sum * 0.9f) );
}

// White: one random value per cell
template <int D>
inline float
white(const float* q, const Perm& P)
{
    int c[D];
    for (int i = 0; i < D; ++i) {
        c[i] = fastFloor(q[i]);
    }

    return P.r4[ cellHash<D>(P, c) ] * 2.f - 1.f;
}

// ---------------------------------------------------------------- one octave

struct BasisParams
{
    int basis = eBasisPerlin;
    int metric = eMetricEuclidean;
    double jitter = 1.;
};

template <int D>
inline float
basis(const float* q, const Perm& P, const BasisParams& bp)
{
    switch (bp.basis) {
    case eBasisSimplex:
        return (D == 4) ? simplex4(q, P) : simplex3(q, P);
    case eBasisSparseConvolution:
        return sparseConvolution<D>(q, P);
    case eBasisAlligator:
        return alligator<D>(q, P, (float)bp.jitter, bp.metric);
    case eBasisWorleyF1:
    case eBasisWorleyF2:
    case eBasisWorleyF2F1: {
        float f1, f2;
        worley<D>(q, P, (float)bp.jitter, bp.metric, &f1, &f2);
        float v;
        if (bp.basis == eBasisWorleyF1) {
            v = f1 * 1.25f;
        } else if (bp.basis == eBasisWorleyF2) {
            v = f2 * 0.9f;
        } else {
            v = (f2 - f1) * 1.6f;
        }

        return std::max( -1.f, std::min(1.f, v * 2.f - 1.f) );
    }
    case eBasisWhite:
        return white<D>(q, P);
    case eBasisPerlin:
    default:
        return (D == 4) ? perlin4(q, P) : perlin3(q, P);
    }
}

// ---------------------------------------------------------------- fractal layering

struct FractalParams
{
    int fractal = eFractalFBM;
    int octaves = 6;           // already limited by nyquist by the caller
    double lacunarity = 2.;
    double gain = 0.5;         // amplitude multiplier per octave (Houdini "roughness")
};

// Returns a value in about [0, 1]. Octaves are decorrelated by an offset.
template <int D>
inline double
fractal(const double* pIn, const Perm& P, const BasisParams& bp, const FractalParams& fp)
{
    float q[D];
    for (int i = 0; i < D; ++i) {
        q[i] = (float)pIn[i];
    }
    const float lac = (float)fp.lacunarity, gain = (float)fp.gain;
    const int oct = (fp.fractal == eFractalNone) ? 1 : std::max(1, fp.octaves);
    float amp = 1.f, ampSum = 0.f, sum = 0.f;

    if ( (fp.fractal == eFractalTerrain) || (fp.fractal == eFractalHybridTerrain) ) {
        // F. K. Musgrave's heterogeneous terrain / hybrid multifractal
        const float offset = 0.7f;
        float value = 0.f, weight = 1.f;
        for (int o = 0; o < oct; ++o) {
            const float n = basis<D>(q, P, bp) + offset;
            if (fp.fractal == eFractalTerrain) {
                value += (o == 0) ? n : n * amp * value;
            } else {
                const float signal = n * amp;
                if (o == 0) {
                    value = signal;
                    weight = signal;
                } else {
                    weight = std::max( 0.f, std::min(1.f, weight) );
                    value += weight * signal;
                    weight *= signal;
                }
            }
            ampSum += amp;
            amp *= gain;
            for (int i = 0; i < D; ++i) {
                q[i] = q[i] * lac + 17.31f * (i + 1);
            }
        }
        const float norm = (fp.fractal == eFractalTerrain) ? (1.f + offset) * (1.f + offset) * ampSum : (1.f + offset) * ampSum;

        return std::max( 0.f, value / std::max(1e-6f, norm) );
    }

    float prevRidge = 1.f;
    for (int o = 0; o < oct; ++o) {
        const float n = basis<D>(q, P, bp);
        switch (fp.fractal) {
        case eFractalTurbulence:
            sum += amp * std::fabs(n);
            break;
        case eFractalRidged: {
            float r = 1.f - std::fabs(n);
            r *= r;
            r *= prevRidge;
            prevRidge = std::min(1.f, r * 2.f);
            sum += amp * r;
            break;
        }
        case eFractalNone:
        case eFractalFBM:
        default:
            sum += amp * n;
            break;
        }
        ampSum += amp;
        amp *= gain;
        for (int i = 0; i < D; ++i) {
            q[i] = q[i] * lac + 17.31f * (i + 1);
        }
    }
    sum /= std::max(1e-6f, ampSum);
    if ( (fp.fractal == eFractalTurbulence) || (fp.fractal == eFractalRidged) ) {
        return sum;          // already 0..1
    }

    return sum * 0.5f + 0.5f;  // signed -> 0..1
}
} // namespace APFXNoise

#endif // APFX_NOISEMATH_H
