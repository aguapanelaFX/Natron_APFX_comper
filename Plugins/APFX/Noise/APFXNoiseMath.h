/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX noise math: hash-based gradient / cellular / convolution noises in 3D and 4D
// (4D is used for seamless time loops), plus fractal layering.
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

// ---------------------------------------------------------------- hashing

inline uint32_t
hash32(uint32_t x)
{
    // lowbias32 (Chris Wellons)
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;

    return x;
}

template <int D>
inline uint32_t
hashCell(const int* c,
         uint32_t seed)
{
    uint32_t h = hash32(seed * 0x9e3779b9U + 0x632be5abU);
    for (int i = 0; i < D; ++i) {
        h = hash32( h ^ ( (uint32_t)c[i] * 0x85ebca6bU + (uint32_t)i * 0xc2b2ae35U ) );
    }

    return h;
}

inline float
toUnit(uint32_t h)        // [0, 1)
{
    return (h >> 8) * (1.0f / 16777216.0f);
}

inline double
fade(double t)
{
    return t * t * t * ( t * (t * 6. - 15.) + 10. );
}

inline double
lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

// ---------------------------------------------------------------- Perlin (improved, any dimension)

template <int D>
inline double
gradDot(uint32_t h,
        const double* d)
{
    // gradients: vectors with one zero component and the others +-1
    // (12 directions in 3D, 32 in 4D, like Ken Perlin's improved noise)
    const int zero = (int)(h % (uint32_t)D);
    uint32_t bits = h >> 8;
    double s = 0.;
    for (int i = 0; i < D; ++i) {
        if (i == zero) {
            continue;
        }
        s += (bits & 1) ? -d[i] : d[i];
        bits >>= 1;
    }

    return s;
}

template <int D>
inline double
perlin(const double* p,
       uint32_t seed)
{
    int c0[D];
    double f[D], u[D];
    for (int i = 0; i < D; ++i) {
        double fl = std::floor(p[i]);
        c0[i] = (int)fl;
        f[i] = p[i] - fl;
        u[i] = fade(f[i]);
    }
    // corner values, then multilinear interpolation
    double vals[1 << D];
    for (int corner = 0; corner < (1 << D); ++corner) {
        int c[D];
        double d[D];
        for (int i = 0; i < D; ++i) {
            const int bit = (corner >> i) & 1;
            c[i] = c0[i] + bit;
            d[i] = f[i] - bit;
        }
        vals[corner] = gradDot<D>(hashCell<D>(c, seed), d);
    }
    int n = 1 << D;
    for (int i = 0; i < D; ++i) {
        n >>= 1;
        for (int k = 0; k < n; ++k) {
            vals[k] = lerp(vals[2 * k], vals[2 * k + 1], u[i]);
        }
    }
    // the improved-noise range is about +-1 in 3D; 4D is a bit smaller
    const double norm = (D == 4) ? 1.15 : 1.0;

    return std::max( -1., std::min(1., vals[0] * norm) );
}

// ---------------------------------------------------------------- Simplex (3D and 4D, after S. Gustavson)

inline double
simplexCorner3(const int* c,
               double x, double y, double z,
               uint32_t seed)
{
    double t = 0.6 - x * x - y * y - z * z;
    if (t < 0.) {
        return 0.;
    }
    const double d[3] = { x, y, z };
    t *= t;

    return t * t * gradDot<3>(hashCell<3>(c, seed), d);
}

inline double
simplex3(const double* p,
         uint32_t seed)
{
    const double F3 = 1. / 3., G3 = 1. / 6.;
    const double s = (p[0] + p[1] + p[2]) * F3;
    const int i = (int)std::floor(p[0] + s), j = (int)std::floor(p[1] + s), k = (int)std::floor(p[2] + s);
    const double t = (i + j + k) * G3;
    const double x0 = p[0] - (i - t), y0 = p[1] - (j - t), z0 = p[2] - (k - t);
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
    const int c0[3] = { i, j, k }, c1[3] = { i + i1, j + j1, k + k1 }, c2[3] = { i + i2, j + j2, k + k2 }, c3[3] = { i + 1, j + 1, k + 1 };
    double n = simplexCorner3(c0, x0, y0, z0, seed);
    n += simplexCorner3(c1, x0 - i1 + G3, y0 - j1 + G3, z0 - k1 + G3, seed);
    n += simplexCorner3(c2, x0 - i2 + 2. * G3, y0 - j2 + 2. * G3, z0 - k2 + 2. * G3, seed);
    n += simplexCorner3(c3, x0 - 1. + 3. * G3, y0 - 1. + 3. * G3, z0 - 1. + 3. * G3, seed);

    return std::max( -1., std::min(1., 32. * n) );
}

inline double
simplex4(const double* p,
         uint32_t seed)
{
    const double F4 = (std::sqrt(5.) - 1.) / 4., G4 = (5. - std::sqrt(5.)) / 20.;
    const double s = (p[0] + p[1] + p[2] + p[3]) * F4;
    int c[4];
    for (int a = 0; a < 4; ++a) {
        c[a] = (int)std::floor(p[a] + s);
    }
    const double t = (c[0] + c[1] + c[2] + c[3]) * G4;
    double x0[4];
    for (int a = 0; a < 4; ++a) {
        x0[a] = p[a] - (c[a] - t);
    }
    // rank the coordinates to find the simplex
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
    double n = 0.;
    for (int corner = 0; corner < 5; ++corner) {
        int cc[4];
        double d[4];
        for (int a = 0; a < 4; ++a) {
            const int o = (corner == 0) ? 0 : ( (corner == 4) ? 1 : (rank[a] >= 4 - corner ? 1 : 0) );
            cc[a] = c[a] + o;
            d[a] = x0[a] - o + corner * G4;
        }
        double tt = 0.6 - d[0] * d[0] - d[1] * d[1] - d[2] * d[2] - d[3] * d[3];
        if (tt > 0.) {
            tt *= tt;
            n += tt * tt * gradDot<4>(hashCell<4>(cc, seed), d);
        }
    }

    return std::max( -1., std::min(1., 27. * n) );
}

// ---------------------------------------------------------------- cellular helpers

template <int D>
inline double
metricDistance(const double* d,
               int metric)
{
    double r = 0.;
    if (metric == eMetricManhattan) {
        for (int i = 0; i < D; ++i) {
            r += std::fabs(d[i]);
        }
    } else if (metric == eMetricChebyshev) {
        for (int i = 0; i < D; ++i) {
            r = std::max( r, std::fabs(d[i]) );
        }
    } else {
        for (int i = 0; i < D; ++i) {
            r += d[i] * d[i];
        }
        r = std::sqrt(r);
    }

    return r;
}

// iterate over the 3^D neighbour cells
template <int D, class F>
inline void
forNeighbours(const int* base,
              F fn)
{
    int n = 1;
    for (int i = 0; i < D; ++i) {
        n *= 3;
    }
    for (int k = 0; k < n; ++k) {
        int c[D];
        int r = k;
        for (int i = 0; i < D; ++i) {
            c[i] = base[i] + (r % 3) - 1;
            r /= 3;
        }
        fn(c);
    }
}

// feature point of a cell (jitter 0 = cell centre, 1 = anywhere in the cell)
template <int D>
inline void
featurePoint(const int* c,
             uint32_t seed,
             double jitter,
             double* out,
             uint32_t* hOut)
{
    uint32_t h = hashCell<D>(c, seed);
    *hOut = h;
    for (int i = 0; i < D; ++i) {
        h = hash32(h + 0x27d4eb2dU);
        out[i] = c[i] + 0.5 + (toUnit(h) - 0.5) * jitter;
    }
}

// Worley: distances to the closest (F1) and second closest (F2) feature points
template <int D>
inline void
worley(const double* p,
       uint32_t seed,
       double jitter,
       int metric,
       double* F1,
       double* F2)
{
    int base[D];
    for (int i = 0; i < D; ++i) {
        base[i] = (int)std::floor(p[i]);
    }
    double f1 = 1e9, f2 = 1e9;
    forNeighbours<D>(base, [&](const int* c) {
        double fp[D], d[D];
        uint32_t h;
        featurePoint<D>(c, seed, jitter, fp, &h);
        for (int i = 0; i < D; ++i) {
            d[i] = fp[i] - p[i];
        }
        const double r = metricDistance<D>(d, metric);
        if (r < f1) {
            f2 = f1;
            f1 = r;
        } else if (r < f2) {
            f2 = r;
        }
    });
    *F1 = f1;
    *F2 = f2;
}

// Alligator: each feature point carries a random height with a smooth falloff;
// the result is the difference between the two highest bumps (plates with creases)
template <int D>
inline double
alligator(const double* p,
          uint32_t seed,
          double jitter,
          int metric)
{
    int base[D];
    for (int i = 0; i < D; ++i) {
        base[i] = (int)std::floor(p[i]);
    }
    double b1 = 0., b2 = 0.;
    forNeighbours<D>(base, [&](const int* c) {
        double fp[D], d[D];
        uint32_t h;
        featurePoint<D>(c, seed, jitter, fp, &h);
        for (int i = 0; i < D; ++i) {
            d[i] = fp[i] - p[i];
        }
        const double r = metricDistance<D>(d, metric);
        double fall = 1. - r;
        if (fall <= 0.) {
            return;
        }
        fall = fall * fall * (3. - 2. * fall);
        const double b = toUnit( hash32(h ^ 0x5bd1e995U) ) * fall;
        if (b > b1) {
            b2 = b1;
            b1 = b;
        } else if (b > b2) {
            b2 = b;
        }
    });

    return std::max( -1., std::min(1., (b1 - b2) * 2.6 - 1.) );
}

// Sparse convolution: random impulses in every cell convolved with a smooth kernel
template <int D>
inline double
sparseConvolution(const double* p,
                  uint32_t seed)
{
    int base[D];
    for (int i = 0; i < D; ++i) {
        base[i] = (int)std::floor(p[i]);
    }
    const int kImpulses = 3;
    double sum = 0.;
    forNeighbours<D>(base, [&](const int* c) {
        uint32_t h = hashCell<D>(c, seed);
        for (int k = 0; k < kImpulses; ++k) {
            double r2 = 0.;
            for (int i = 0; i < D; ++i) {
                h = hash32(h + 0x165667b1U);
                const double d = c[i] + toUnit(h) - p[i];
                r2 += d * d;
            }
            h = hash32(h + 0x9e3779b9U); // always advance: next impulses must not depend on this one
            if (r2 < 1.) {
                const double w = toUnit(h) * 2. - 1.;
                const double t = 1. - r2;
                sum += w * t * t * t;
            }
        }
    });

    return std::max( -1., std::min(1., sum * 0.75) );
}

// White: one random value per cell
template <int D>
inline double
white(const double* p,
      uint32_t seed)
{
    int c[D];
    for (int i = 0; i < D; ++i) {
        c[i] = (int)std::floor(p[i]);
    }

    return toUnit( hashCell<D>(c, seed) ) * 2. - 1.;
}

// ---------------------------------------------------------------- one octave

struct BasisParams
{
    int basis = eBasisPerlin;
    int metric = eMetricEuclidean;
    double jitter = 1.;
};

template <int D>
inline double
basis(const double* p,
      uint32_t seed,
      const BasisParams& bp)
{
    switch (bp.basis) {
    case eBasisSimplex:
        return (D == 4) ? simplex4(p, seed) : simplex3(p, seed);
    case eBasisSparseConvolution:
        return sparseConvolution<D>(p, seed);
    case eBasisAlligator:
        return alligator<D>(p, seed, bp.jitter, bp.metric);
    case eBasisWorleyF1:
    case eBasisWorleyF2:
    case eBasisWorleyF2F1: {
        double f1, f2;
        worley<D>(p, seed, bp.jitter, bp.metric, &f1, &f2);
        double v;
        if (bp.basis == eBasisWorleyF1) {
            v = f1 * 1.25;
        } else if (bp.basis == eBasisWorleyF2) {
            v = f2 * 0.9;
        } else {
            v = (f2 - f1) * 1.6;
        }

        return std::max( -1., std::min(1., v * 2. - 1.) );
    }
    case eBasisWhite:
        return white<D>(p, seed);
    case eBasisPerlin:
    default:
        return perlin<D>(p, seed);
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

// Returns a value in about [0, 1].
template <int D>
inline double
fractal(const double* pIn,
        uint32_t seed,
        const BasisParams& bp,
        const FractalParams& fp)
{
    double p[D];
    for (int i = 0; i < D; ++i) {
        p[i] = pIn[i];
    }
    const int oct = (fp.fractal == eFractalNone) ? 1 : std::max(1, fp.octaves);
    double amp = 1., ampSum = 0., sum = 0.;

    if ( (fp.fractal == eFractalTerrain) || (fp.fractal == eFractalHybridTerrain) ) {
        // F. K. Musgrave's heterogeneous terrain / hybrid multifractal
        const double offset = 0.7;
        double value = 0., weight = 1.;
        for (int o = 0; o < oct; ++o) {
            const double n = basis<D>(p, seed + (uint32_t)o * 1013U, bp) + offset;
            if (fp.fractal == eFractalTerrain) {
                // heterogeneous terrain: higher areas get more detail
                const double inc = (o == 0) ? n : n * amp * value;
                value += inc;
            } else {
                // hybrid multifractal: smooth valleys, rough peaks
                const double signal = n * amp;
                if (o == 0) {
                    value = signal;
                    weight = signal;
                } else {
                    weight = std::max( 0., std::min(1., weight) );
                    value += weight * signal;
                    weight *= signal;
                }
            }
            ampSum += amp;
            amp *= fp.gain;
            for (int i = 0; i < D; ++i) {
                p[i] *= fp.lacunarity;
            }
        }
        const double norm = (fp.fractal == eFractalTerrain) ? (1. + offset) * (1. + offset) * ampSum : (1. + offset) * ampSum;

        return std::max(0., value / std::max(1e-6, norm) );
    }

    double prevRidge = 1.;
    for (int o = 0; o < oct; ++o) {
        const double n = basis<D>(p, seed + (uint32_t)o * 1013U, bp);
        switch (fp.fractal) {
        case eFractalTurbulence:
            sum += amp * std::fabs(n);
            break;
        case eFractalRidged: {
            double r = 1. - std::fabs(n);
            r *= r;
            r *= prevRidge;
            prevRidge = std::min(1., r * 2.);
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
        amp *= fp.gain;
        for (int i = 0; i < D; ++i) {
            p[i] *= fp.lacunarity;
        }
    }
    sum /= std::max(1e-6, ampSum);
    if ( (fp.fractal == eFractalTurbulence) || (fp.fractal == eFractalRidged) ) {
        return sum;          // already 0..1
    }

    return sum * 0.5 + 0.5;  // signed -> 0..1
}
} // namespace APFXNoise

#endif // APFX_NOISEMATH_H
