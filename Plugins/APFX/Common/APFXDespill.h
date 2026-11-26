/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX despill maths (pure C++, no OFX):
//  - classic: the screen channel is limited to a reference made of the other two (average, double
//    average, limit, custom balance), the removed amount can be given back as grey (luminance restore)
//  - toColour: the spill (how much of the screen colour is in the pixel) is replaced by a target colour of
//    the same brightness
//  - vector: the spill is removed along the screen colour's own direction with its brightness kept, so hues
//    stay right (skin, yellow, cyan); the screen colour can vary per pixel (clean plate input), the spill
//    can be replaced by a colour or a background, and limited to the edges of an alpha

#ifndef APFX_DESPILL_H
#define APFX_DESPILL_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace APFXDespill {

enum Screen { eGreen = 0, eBlue = 1 };

inline float
luma(const float* c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

// index of the screen channel, and of the "other" one (blue on green screens, green on blue screens)
inline int screenChannel(int screen) { return screen == eBlue ? 2 : 1; }
inline int otherChannel(int screen) { return screen == eBlue ? 1 : 2; }

// ---------------------------------------------------------------- classic
enum Algorithm { eAverage = 0, eDoubleRed, eDoubleOther, eRedLimit, eOtherLimit, eCustom };

struct ClassicParams
{
    int screen = eGreen;
    int algorithm = eAverage;
    float balance = 0.5f;     // custom: weight of red in the reference (the other channel gets the rest)
    float gain = 1.f;         // fine tune: how much of the spill is removed
    float threshold = 0.f;    // fine tune: spill below this is left alone
    float restore = 0.f;      // gives the removed amount back as grey (keeps the brightness)
    float tint[3] = { 1.f, 1.f, 1.f }; // colour of the restored light
};

// c: rgb in, out: rgb despilled; returns the spill amount (spill matte)
inline float
classic(const float* c, const ClassicParams& p, float* out)
{
    const int s = screenChannel(p.screen), o = otherChannel(p.screen);
    const float r = c[0], q = c[o];
    float ref;
    switch (p.algorithm) {
    case eAverage: ref = 0.5f * (r + q); break;
    case eDoubleRed: ref = (2.f * r + q) / 3.f; break;
    case eDoubleOther: ref = (r + 2.f * q) / 3.f; break;
    case eRedLimit: ref = r; break;
    case eOtherLimit: ref = q; break;
    default: ref = p.balance * r + (1.f - p.balance) * q; break;
    }
    float spill = std::max(0.f, c[s] - ref - p.threshold) * p.gain;
    spill = std::min( spill, std::max(0.f, c[s]) );
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    out[s] -= spill;
    if (p.restore != 0.f) {
        // the brightness of what was removed, back as (tinted) grey
        const float w[3] = { 0.2126f, 0.7152f, 0.0722f };
        const float l = spill * w[s] / ( 0.2126f * p.tint[0] + 0.7152f * p.tint[1] + 0.0722f * p.tint[2] + 1e-6f );
        for (int k = 0; k < 3; ++k) {
            out[k] += l * p.restore * p.tint[k];
        }
    }

    return spill;
}

// ---------------------------------------------------------------- to colour
struct ToColourParams
{
    float screen[3] = { 0.1f, 0.6f, 0.15f };
    float target[3] = { 0.5f, 0.5f, 0.5f };
    float balance = 0.5f;     // weight of red against the other channel (lower: reddish tones less affected)
    float gain = 1.f;         // range: higher catches more colours as spill
};

inline float
toColour(const float* c, const ToColourParams& p, float* out)
{
    const int s = (p.screen[2] > p.screen[1]) ? 2 : 1, o = (s == 2) ? 1 : 2;
    const float domS = p.screen[s] - (p.balance * p.screen[0] + (1.f - p.balance) * p.screen[o]);
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    if (domS <= 1e-6f) {
        return 0.f;
    }
    const float dom = c[s] - (p.balance * c[0] + (1.f - p.balance) * c[o]);
    const float k = std::max( 0.f, std::min(1.f, p.gain * dom / domS) );
    // target with the brightness of the screen colour: the brightness of the pixel stays
    const float ls = luma(p.screen), lt = luma(p.target);
    const float sc = (lt > 1e-6f) ? ls / lt : 0.f;
    for (int i = 0; i < 3; ++i) {
        out[i] = c[i] + k * (p.target[i] * sc - p.screen[i]);
    }

    return k;
}

// ---------------------------------------------------------------- vector
struct VectorParams
{
    float amount = 1.f;       // 0 = nothing removed, 1 = all the spill
    float gain = 1.f;         // range: higher removes spill from less screen-coloured pixels too
    float threshold = 0.f;    // spill below this (fraction of the screen's strength) is left alone
    float protect = 0.7f;     // 0 = average reference (strong), 1 = limit reference (keeps cyan / yellow / skin)
    float luminance = 1.f;    // 1 = brightness kept, 0 = the spill's brightness is removed too (darker)
    float replace = 0.f;      // amount of the replacement colour put where the spill was
    float edges = 0.f;        // 1 = only on semi-transparent edges (needs an alpha), 0 = everywhere
};

// s: the screen colour at this pixel, rep: replacement colour (or NULL), a: alpha of the pixel
inline float
vectorDespill(const float* c, const float* s, const float* rep, float a, const VectorParams& p, float* out)
{
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    const int sc = (s[2] > s[1]) ? 2 : 1, o = (sc == 2) ? 1 : 2;
    auto ref = [&](const float* x) {
        const float avg = 0.5f * (x[0] + x[o]), lim = std::max(x[0], x[o]);

        return avg + (lim - avg) * p.protect;
    };
    const float domS = s[sc] - ref(s);
    if (domS <= 1e-6f) {
        return 0.f;
    }
    float k = (c[sc] - ref(c)) / domS;
    k = std::max( 0.f, (k - p.threshold) * p.gain );
    k = std::min(k, 1.5f) * p.amount;
    if (p.edges > 0.f) {
        // edges: alpha between 0 and 1 (a solid core keeps its colour)
        const float e = std::max( 0.f, std::min(1.f, 4.f * a * (1.f - a) ) );
        k *= 1.f - p.edges + p.edges * e;
    }
    if (k <= 0.f) {
        return 0.f;
    }
    // remove the screen colour's chroma (and some of its brightness when luminance < 1)
    const float ls = luma(s);
    for (int i = 0; i < 3; ++i) {
        const float chroma = s[i] - ls;
        out[i] -= k * ( chroma + (1.f - p.luminance) * ls );
    }
    if ( rep && (p.replace > 0.f) ) {
        // replacement: its chroma, at the brightness that was taken away
        const float lr = luma(rep);
        for (int i = 0; i < 3; ++i) {
            out[i] += k * p.replace * ( (rep[i] - lr) + (1.f - p.luminance) * lr );
        }
    }

    return k;
}

// screen colour of a shot: the screen type (green / blue) whose channel dominates more pixels, then the
// average of the 5% most screen-coloured pixels. rgba: w * h * 4
inline void
autoScreenColour(const std::vector<float>& rgba, int w, int h, float* out)
{
    long gN = 0, bN = 0;
    const size_t n = (size_t)w * h;
    const size_t step = std::max( (size_t)1, n / 200000 );
    for (size_t i = 0; i < n; i += step) {
        const float* c = &rgba[i * 4];
        if ( (c[1] > c[0]) && (c[1] > c[2]) ) {
            ++gN;
        } else if ( (c[2] > c[0]) && (c[2] > c[1]) ) {
            ++bN;
        }
    }
    const int s = (bN > gN) ? 2 : 1, o = (s == 2) ? 1 : 2;
    std::vector<std::pair<float, size_t> > dom;
    dom.reserve(n / step + 1);
    for (size_t i = 0; i < n; i += step) {
        const float* c = &rgba[i * 4];
        dom.push_back( std::make_pair( c[s] - std::max(c[0], c[o]), i ) );
    }
    out[0] = 0.f; out[1] = (s == 1) ? 1.f : 0.f; out[2] = (s == 2) ? 1.f : 0.f;
    if ( dom.empty() ) {
        return;
    }
    const size_t top = std::max( (size_t)1, dom.size() / 20 );
    std::nth_element( dom.begin(), dom.begin() + (dom.size() - top), dom.end() );
    double acc[3] = { 0., 0., 0. };
    for (size_t j = dom.size() - top; j < dom.size(); ++j) {
        const float* c = &rgba[dom[j].second * 4];
        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2];
    }
    for (int k = 0; k < 3; ++k) {
        out[k] = (float)(acc[k] / top);
    }
}
} // namespace APFXDespill

#endif // APFX_DESPILL_H
