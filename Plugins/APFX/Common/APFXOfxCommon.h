/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// OFX helpers shared by the APFX whole-frame nodes: host threads, image <-> plane,
// channels / mask / mix output, standard parameter blocks.

#ifndef APFX_OFXCOMMON_H
#define APFX_OFXCOMMON_H

#include <algorithm>
#include <functional>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxNatron.h"

#include "APFXImageOps.h"

#ifndef OVERRIDE
#define OVERRIDE override
#endif
#ifndef FINAL
#define FINAL final
#endif

namespace APFXOfx {

// runs fn(threadIndex, nThreads) on the host threads
class Job
    : public OFX::MultiThread::Processor
{
public:
    explicit Job(const std::function<void(unsigned, unsigned)>& fn) : _fn(fn) {}
    void multiThreadFunction(unsigned int id, unsigned int n) OVERRIDE FINAL { _fn(id, n); }

private:
    std::function<void(unsigned, unsigned)> _fn;
};

// ParallelFor on the host threads
inline APFXOps::ParallelFor
hostParallelFor()
{
    return [](const std::function<void(int, int)>& fn, int n) {
        if (n <= 0) {
            return;
        }
        Job job([&](unsigned id, unsigned nt) {
            const int a = (int)( (long long)n * id / nt ), b = (int)( (long long)n * (id + 1) / nt );
            if (b > a) {
                fn(a, b);
            }
        });
        job.multiThread();
    };
}

// copies an RGBA float image (its bounds) into a plane; returns the bounds
inline OfxRectI
imageToPlane(const OFX::Image* img, APFXOps::Plane& p)
{
    OfxRectI b = img->getBounds();
    const int w = std::max(0, b.x2 - b.x1), h = std::max(0, b.y2 - b.y1);
    p.resize(w, h, 4);
    for (int y = 0; y < h; ++y) {
        const float* s = (const float*)img->getPixelAddress(b.x1, b.y1 + y);
        if (s) {
            std::copy( s, s + (size_t)w * 4, p.px(0, y) );
        }
    }

    return b;
}

// RGBA plane value at absolute pixel (x, y) given its bounds (0 outside)
inline const float*
planeAt(const APFXOps::Plane& p, const OfxRectI& b, int x, int y)
{
    if ( (x < b.x1) || (y < b.y1) || (x >= b.x2) || (y >= b.y2) ) {
        return NULL;
    }

    return p.px(x - b.x1, y - b.y1);
}

struct OutputArgs
{
    bool proc[4] = { true, true, true, true };
    float mix = 1.f;
    const OFX::Image* mask = NULL;
    bool maskInvert = false;
};

// writes the render window: result where the channel is processed, mixed with the
// source by mix * mask; unprocessed channels come from the source
inline void
writeOutput(OFX::ImageEffect& effect, OFX::Image* dst, const OfxRectI& win,
            const APFXOps::Plane& res, const OfxRectI& resB,
            const APFXOps::Plane* src, const OfxRectI& srcB, const OutputArgs& o)
{
    (void)effect;
    APFXOps::ParallelFor pf = hostParallelFor();
    pf([&](int y0, int y1) {
        for (int y = win.y1 + y0; y < win.y1 + y1; ++y) {
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (!d) {
                continue;
            }
            for (int x = win.x1; x < win.x2; ++x, d += 4) {
                const float* r = planeAt(res, resB, x, y);
                const float* s = src ? planeAt(*src, srcB, x, y) : NULL;
                float m = o.mix;
                if (o.mask) {
                    const float* mp = (const float*)o.mask->getPixelAddress(x, y);
                    float mv = mp ? *mp : 0.f;
                    if (o.maskInvert) {
                        mv = 1.f - mv;
                    }
                    m *= std::max( 0.f, std::min(1.f, mv) );
                }
                for (int c = 0; c < 4; ++c) {
                    const float sv = s ? s[c] : 0.f;
                    const float rv = r ? r[c] : 0.f;
                    d[c] = o.proc[c] ? sv + (rv - sv) * m : sv;
                }
            }
        }
    }, win.y2 - win.y1);
}

// ---------------------------------------------------------------- descriptors

inline void
describeCommon(OFX::ImageEffectDescriptor& desc, const char* label, const char* description)
{
    desc.setLabel(label);
    desc.setPluginGrouping("APFX");
    desc.setPluginDescription(description);
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedContext(OFX::eContextGeneral);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(false);   // whole-frame algorithms
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderFullySafe);
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(OFX::ePixelComponentNone); // own red / green / blue / alpha checkboxes
#endif
}

inline OFX::ClipDescriptor*
defineRGBAClip(OFX::ImageEffectDescriptor& desc, const char* name, bool optional)
{
    OFX::ClipDescriptor* c = desc.defineClip(name);
    c->addSupportedComponent(OFX::ePixelComponentRGBA);
    c->setTemporalClipAccess(false);
    c->setSupportsTiles(false);
    c->setOptional(optional);
    c->setIsMask(false);

    return c;
}

inline void
defineMaskClip(OFX::ImageEffectDescriptor& desc)
{
    OFX::ClipDescriptor* m = desc.defineClip("Mask");
    m->addSupportedComponent(OFX::ePixelComponentAlpha);
    m->setTemporalClipAccess(false);
    m->setOptional(true);
    m->setSupportsTiles(false);
    m->setIsMask(true);
}

inline void
defineChannels(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, bool alphaDefault)
{
    const char* names[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
    const char* labels[4] = { "red", "green", "blue", "alpha" };
    for (int i = 0; i < 4; ++i) {
        OFX::BooleanParamDescriptor* p = desc.defineBooleanParam(names[i]);
        p->setLabel(labels[i]);
        p->setHint("Process this channel.");
        p->setDefault(i < 3 ? true : alphaDefault);
        if (i < 3) {
            p->setLayoutHint(OFX::eLayoutHintNoNewLine, 1);
        }
        page->addChild(*p);
    }
}

inline void
defineMaskMix(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page)
{
    {
        OFX::BooleanParamDescriptor* p = desc.defineBooleanParam("maskInvert");
        p->setLabel("Invert Mask");
        p->setHint("When checked, the effect is fully applied where the mask is 0.");
        p->setDefault(false);
        page->addChild(*p);
    }
    {
        OFX::DoubleParamDescriptor* p = desc.defineDoubleParam("mix");
        p->setLabel("Mix");
        p->setHint("Mix between the original (0) and the result (1).");
        p->setDefault(1.);
        p->setRange(0., 1.);
        p->setDisplayRange(0., 1.);
        page->addChild(*p);
    }
}

inline OFX::DoubleParamDescriptor*
defineDouble(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name, const char* label,
             const char* hint, double def, double dmin, double dmax, double rmin = -1e9, double rmax = 1e9)
{
    OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(rmin, rmax);
    p->setDisplayRange(dmin, dmax);
    page->addChild(*p);

    return p;
}

// Nuke-like divider line under a parameter
inline void
divider(OFX::ParamDescriptor* p)
{
    p->setLayoutHint(OFX::eLayoutHintDivider);
}

inline OFX::BooleanParamDescriptor*
defineBool(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name, const char* label,
           const char* hint, bool def)
{
    OFX::BooleanParamDescriptor* p = desc.defineBooleanParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(def);
    page->addChild(*p);

    return p;
}

// fetches the standard output arguments (channels, mask, mix) of an instance
struct StdParams
{
    OFX::BooleanParam* proc[4] = { NULL, NULL, NULL, NULL };
    OFX::BooleanParam* maskInvert = NULL;
    OFX::DoubleParam* mix = NULL;
    OFX::Clip* maskClip = NULL;

    // withChannels: the node has the red / green / blue / alpha checkboxes (defineChannels)
    void fetch(OFX::ImageEffect& fx, bool withMask, bool withChannels = true)
    {
        const char* names[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
        for (int i = 0; withChannels && i < 4; ++i) {
            proc[i] = fx.fetchBooleanParam(names[i]);
        }
        if (withMask) {
            maskInvert = fx.fetchBooleanParam("maskInvert");
            mix = fx.fetchDoubleParam("mix");
            maskClip = fx.fetchClip("Mask");
        }
    }

    // fills o (mask image must be deleted by the caller)
    OFX::Image* get(double t, OutputArgs& o) const
    {
        for (int i = 0; i < 4; ++i) {
            o.proc[i] = proc[i] ? proc[i]->getValueAtTime(t) : true;
        }
        OFX::Image* m = NULL;
        if (mix) {
            o.mix = (float)mix->getValueAtTime(t);
            o.maskInvert = maskInvert->getValueAtTime(t);
            if ( maskClip && maskClip->isConnected() ) {
                m = maskClip->fetchImage(t);
                if ( m && ( (m->getPixelDepth() != OFX::eBitDepthFloat) || (m->getPixelComponents() != OFX::ePixelComponentAlpha) ) ) {
                    delete m;
                    m = NULL;
                }
            }
        }
        o.mask = m;

        return m;
    }
};
} // namespace APFXOfx

#endif // APFX_OFXCOMMON_H
