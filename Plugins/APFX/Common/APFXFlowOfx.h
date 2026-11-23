/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// OFX side of the APFX optical flow nodes: frames fetched into planes over one window,
// their gray images / weights for the flow, and a small per-render frame store.

#ifndef APFX_FLOWOFX_H
#define APFX_FLOWOFX_H

#include <map>
#include <memory>

#include "APFXOfxCommon.h"
#include "APFXFlow.h"

namespace APFXFlowOfx {

using APFXFlow::FrameData;
using APFXOps::Plane;

// copies a float image (RGBA, RGB or alpha) into a plane of nc channels covering win (0 outside the image)
inline bool
fetchPlane(OFX::Clip* clip, double t, const OfxRectI& win, int nc, Plane& out)
{
    out.resize( std::max(0, win.x2 - win.x1), std::max(0, win.y2 - win.y1), nc, 0.f );
    if ( !clip || !clip->isConnected() ) {
        return false;
    }
    std::unique_ptr<OFX::Image> img( clip->fetchImage(t) );
    if ( !img || (img->getPixelDepth() != OFX::eBitDepthFloat) ) {
        return false;
    }
    const int sc = img->getPixelComponentCount();
    const OfxRectI b = img->getBounds();
    const int x0 = std::max(win.x1, b.x1), x1 = std::min(win.x2, b.x2);
    for (int y = std::max(win.y1, b.y1); y < std::min(win.y2, b.y2); ++y) {
        const float* s = (const float*)img->getPixelAddress(x0, y);
        if (!s) {
            continue;
        }
        float* d = out.px(x0 - win.x1, y - win.y1);
        for (int x = x0; x < x1; ++x, s += sc, d += nc) {
            if (nc == 1) {
                d[0] = s[sc - 1];                       // alpha (or the single channel)
            } else if (sc == 1) {
                d[0] = d[1] = d[2] = 0.f;
                d[nc - 1] = s[0];
            } else {
                for (int c = 0; c < nc; ++c) {
                    d[c] = (c < sc) ? s[c] : 1.f;       // RGB input: alpha 1
                }
            }
        }
    }

    return true;
}

inline void
grayOf(const Plane& p, FrameData& fd)
{
    fd.gray.resize(p.w, p.h);
    const size_t n = (size_t)p.w * p.h;
    for (size_t i = 0; i < n; ++i) {
        const float* c = &p.d[i * p.c];
        fd.gray.d[i] = (p.c >= 3) ? APFXFlow::flowLuma(c[0], c[1], c[2]) : APFXFlow::flowLuma(c[0], c[0], c[0]);
    }
    fd.hash = APFXFlow::hashGray(fd.gray);
}

// weights of a frame from a matte plane (1 channel): inside = matte, otherwise 1 - matte
inline void
weightsOf(const Plane& m, bool inside, FrameData& fd)
{
    fd.weights.resize(m.w, m.h);
    for (size_t i = 0; i < m.d.size(); ++i) {
        const float v = std::max( 0.f, std::min(1.f, m.d[i]) );
        fd.weights.d[i] = inside ? v : 1.f - v;
    }
    fd.whash = APFXFlow::hashGray(fd.weights) + (inside ? 7u : 3u);
    fd.wpyr.reset();
}

// frames of one render, fetched once
struct FrameStore
{
    OFX::Clip* clip = NULL;
    OfxRectI win = { 0, 0, 0, 0 };
    std::map<long long, std::shared_ptr<Plane> > planes;
    std::map<long long, std::shared_ptr<FrameData> > data;

    static long long key(double t) { return (long long)std::llround(t * 1000.); }

    const Plane& plane(double t)
    {
        std::shared_ptr<Plane>& p = planes[key(t)];
        if (!p) {
            p = std::make_shared<Plane>();
            fetchPlane(clip, t, win, 4, *p);
        }

        return *p;
    }
    FrameData& frame(double t)
    {
        std::shared_ptr<FrameData>& f = data[key(t)];
        if (!f) {
            f = std::make_shared<FrameData>();
            grayOf(plane(t), *f);
        }

        return *f;
    }
};
} // namespace APFXFlowOfx

#endif // APFX_FLOWOFX_H
