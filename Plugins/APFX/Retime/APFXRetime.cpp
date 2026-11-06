/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Retime (Nuke-like): maps an input frame range to an output range or by a speed,
// with reverse, before/after (hold, loop, bounce, black) and filtering (none, blend, box
// with shutter). Integer source frames are passed through (no copy).

#include <cmath>
#include <vector>
#include <algorithm>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

#ifndef OVERRIDE
#define OVERRIDE override
#endif
#ifndef FINAL
#define FINAL final
#endif

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Retime"

enum BeforeAfter { eHold = 0, eLoop, eBounce, eBlack };
enum Filter { eFilterNone = 0, eFilterBlend, eFilterBox };

struct Sample
{
    double frame;
    double w;
};

class APFXRetimePlugin
    : public ImageEffect
{
public:
    APFXRetimePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _inEnable = fetchBooleanParam("inputRangeEnable");
        _inFirst = fetchDoubleParam("inputFirst");
        _inLast = fetchDoubleParam("inputLast");
        _outEnable = fetchBooleanParam("outputRangeEnable");
        _outFirst = fetchDoubleParam("outputFirst");
        _outLast = fetchDoubleParam("outputLast");
        _reverse = fetchBooleanParam("reverse");
        _speed = fetchDoubleParam("speed");
        _before = fetchChoiceParam("before");
        _after = fetchChoiceParam("after");
        _filter = fetchChoiceParam("filter");
        _shutter = fetchDoubleParam("shutter");
        _rangeSet = fetchBooleanParam("rangeSet");
        update();
    }

private:
    // ------------------------------------------------------------ mapping
    void inputRange(double* a, double* b) const
    {
        if ( _inEnable->getValue() ) {
            *a = _inFirst->getValue();
            *b = _inLast->getValue();
        } else {
            OfxRangeD r = _src->getFrameRange();
            *a = r.min;
            *b = r.max;
        }
        if (*b < *a) {
            std::swap(*a, *b);
        }
    }

    // output range and source frames per output frame
    void outputRange(double inA, double inB, double* oA, double* oB, double* spd) const
    {
        if ( _outEnable->getValue() ) {
            *oA = _outFirst->getValue();
            *oB = _outLast->getValue();
            if (*oB < *oA) {
                std::swap(*oA, *oB);
            }
            *spd = (*oB > *oA) ? (inB - inA) / (*oB - *oA) : 0.;
        } else {
            double s = _speed->getValue();
            if (std::fabs(s) < 1e-6) {
                s = 1e-6;
            }
            *spd = std::fabs(s);
            *oA = inA;
            *oB = inA + (inB - inA) / *spd;
        }
    }

    // source frame for output time t; false = black
    bool sourceFrame(double t, double* srcT, double* spd) const
    {
        double inA, inB, oA, oB;
        inputRange(&inA, &inB);
        outputRange(inA, inB, &oA, &oB, spd);
        double u = t - oA;               // output frames since start
        const double len = oB - oA;
        if ( (t < oA) || (t > oB) ) {
            const int mode = (t < oA) ? _before->getValue() : _after->getValue();
            if (mode == eBlack) {
                return false;
            }
            if ( (mode == eHold) || (len <= 0.) ) {
                u = (t < oA) ? 0. : len;
            } else if (mode == eLoop) {
                const double p = len + 1.;
                u = std::fmod(u, p);
                if (u < 0) {
                    u += p;
                }
                u = std::min(u, len);
            } else { // bounce
                const double p = 2. * len;
                u = std::fmod(u, p);
                if (u < 0) {
                    u += p;
                }
                if (u > len) {
                    u = p - u;
                }
            }
        }
        double s = inA + u * *spd;
        if ( _reverse->getValue() ) {
            s = inB - u * *spd;
        }
        *srcT = std::max( inA, std::min(inB, s) );

        return true;
    }

    // source frames and weights needed for output time t
    bool samples(double t, std::vector<Sample>& out) const
    {
        out.clear();
        double s, spd;
        if ( !sourceFrame(t, &s, &spd) ) {
            return false;
        }
        const int f = _filter->getValue();
        if (f == eFilterNone) {
            out.push_back( { std::floor(s + 0.5), 1. } );
        } else if (f == eFilterBlend) {
            const double a = std::floor(s), fr = s - a;
            if (fr < 1e-4) {
                out.push_back( { a, 1. } );
            } else if (fr > 1. - 1e-4) {
                out.push_back( { a + 1., 1. } );
            } else {
                out.push_back( { a, 1. - fr } );
                out.push_back( { a + 1., fr } );
            }
        } else {
            // box: average the source frames covered by the shutter (in source frames)
            const double half = 0.5 * std::max(0., _shutter->getValue()) * spd;
            const double a = s - half, b = s + half;
            if (b - a < 1e-4) {
                out.push_back( { std::floor(s + 0.5), 1. } );
            } else {
                double total = 0.;
                for (double fr = std::floor(a + 0.5); fr <= std::floor(b + 0.5); fr += 1.) {
                    // overlap of [fr-0.5, fr+0.5] with [a, b]
                    const double w = std::min(b, fr + 0.5) - std::max(a, fr - 0.5);
                    if (w > 0) {
                        out.push_back( { fr, w } );
                        total += w;
                    }
                    if (out.size() > 64) {
                        break;
                    }
                }
                for (size_t i = 0; i < out.size(); ++i) {
                    out[i].w /= total;
                }
            }
        }

        return true;
    }

    // ------------------------------------------------------------ actions
    void update()
    {
        const bool inOn = _inEnable->getValue(), outOn = _outEnable->getValue();
        _inFirst->setEnabled(inOn);
        _inLast->setEnabled(inOn);
        _outFirst->setEnabled(outOn);
        _outLast->setEnabled(outOn);
        _speed->setEnabled(!outOn);
        _shutter->setEnabled(_filter->getValue() == eFilterBox);
    }

    void fillRangeFromSource()
    {
        if ( _rangeSet->getValue() || !_src->isConnected() ) {
            return;
        }
        OfxRangeD r = _src->getFrameRange();
        _inFirst->setValue(r.min);
        _inLast->setValue(r.max);
        _outFirst->setValue(r.min);
        _outLast->setValue(r.max);
        _rangeSet->setValue(true);
    }

    void changedParam(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == "inputRangeEnable") || (n == "outputRangeEnable") || (n == "filter") ) {
            update();
        }
    }

    void changedClip(const InstanceChangedArgs& args, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == kOfxImageEffectSimpleSourceClipName) && (args.reason == eChangeUserEdit) ) {
            fillRangeFromSource();
        }
    }

    bool getTimeDomain(OfxRangeD& range) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        double inA, inB, oA, oB, spd;
        inputRange(&inA, &inB);
        outputRange(inA, inB, &oA, &oB, &spd);
        range.min = std::floor(oA + 0.5);
        range.max = std::floor(oB + 0.5);

        return true;
    }

    void getFramesNeeded(const FramesNeededArguments& args, FramesNeededSetter& frames) OVERRIDE FINAL
    {
        std::vector<Sample> s;
        if ( !samples(args.time, s) || s.empty() ) {
            OfxRangeD r = { args.time, args.time };
            frames.setFramesNeeded(*_src, r);

            return;
        }
        for (size_t i = 0; i < s.size(); ++i) {
            OfxRangeD r = { s[i].frame, s[i].frame };
            frames.setFramesNeeded(*_src, r);
        }
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        std::vector<Sample> s;
        if ( !_src->isConnected() || !samples(args.time, s) || s.empty() ) {
            return false;
        }
        rod = _src->getRegionOfDefinition(s[0].frame);
        for (size_t i = 1; i < s.size(); ++i) {
            OfxRectD r = _src->getRegionOfDefinition(s[i].frame);
            rod.x1 = std::min(rod.x1, r.x1);
            rod.y1 = std::min(rod.y1, r.y1);
            rod.x2 = std::max(rod.x2, r.x2);
            rod.y2 = std::max(rod.y2, r.y2);
        }

        return true;
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime
#ifdef OFX_EXTENSIONS_NUKE
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        std::vector<Sample> s;
        if ( !samples(args.time, s) ) {
            return false; // black
        }
        if (s.size() == 1) {
            identityClip = _src;
            identityTime = s[0].frame;

            return true;
        }

        return false;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setOutputFrameVarying(true);
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        Image* dst = _dst->fetchImage(args.time);
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        const int nc = dst->getPixelComponentCount();
        const OfxRectI& win = args.renderWindow;
        const int w = win.x2 - win.x1;
        std::vector<Sample> s;
        const bool ok = samples(args.time, s);

        // clear
        for (int y = win.y1; y < win.y2; ++y) {
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (d) {
                std::fill(d, d + (size_t)w * nc, 0.f);
            }
        }
        if (ok) {
            for (size_t i = 0; i < s.size(); ++i) {
                if ( abort() ) {
                    break;
                }
                Image* src = _src->fetchImage(s[i].frame);
                if (!src) {
                    continue;
                }
                const int snc = src->getPixelComponentCount();
                const float wt = (float)s[i].w;
                if ( (src->getPixelDepth() == eBitDepthFloat) && (snc == nc) ) {
                    for (int y = win.y1; y < win.y2; ++y) {
                        float* d = (float*)dst->getPixelAddress(win.x1, y);
                        if (!d) {
                            continue;
                        }
                        for (int x = win.x1; x < win.x2; ++x, d += nc) {
                            const float* p = (const float*)src->getPixelAddress(x, y);
                            if (p) {
                                for (int c = 0; c < nc; ++c) {
                                    d[c] += p[c] * wt;
                                }
                            }
                        }
                    }
                }
                delete src;
            }
        }
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    BooleanParam* _inEnable;
    DoubleParam* _inFirst;
    DoubleParam* _inLast;
    BooleanParam* _outEnable;
    DoubleParam* _outFirst;
    DoubleParam* _outLast;
    BooleanParam* _reverse;
    DoubleParam* _speed;
    ChoiceParam* _before;
    ChoiceParam* _after;
    ChoiceParam* _filter;
    DoubleParam* _shutter;
    BooleanParam* _rangeSet;
};

mDeclarePluginFactory(APFXRetimeFactory, {}, {});

void
APFXRetimeFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("Retime");
    desc.setPluginGrouping("Time");
    desc.setPluginDescription("APFX Retime (Nuke-like): maps an input frame range to an output frame range, "
                              "or retimes by a speed. Reverse, before/after (hold, loop, bounce, black) and "
                              "filtering: none (nearest frame), blend (mix of the two nearest frames) or box "
                              "(averages the frames covered by the shutter).");
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(true);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
}

static ChoiceParamDescriptor*
beforeAfter(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label)
{
    ChoiceParamDescriptor* p = desc.defineChoiceParam(name);
    p->setLabel(label);
    p->setHint("What to output outside the output range.");
    p->appendOption("hold");
    p->appendOption("loop");
    p->appendOption("bounce");
    p->appendOption("black");
    p->setDefault(eHold);
    p->setAnimates(false);
    page->addChild(*p);

    return p;
}

static DoubleParamDescriptor*
frameParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, double def)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabel(label);
    p->setHint("Frame.");
    p->setDefault(def);
    p->setRange(-1e9, 1e9);
    p->setDisplayRange(1., 200.);
    p->setDoubleType(eDoubleTypePlain);
    p->setIncrement(1.);
    p->setDigits(0);
    page->addChild(*p);

    return p;
}

void
APFXRetimeFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setTemporalClipAccess(true);
        c->setSupportsTiles(true);
        c->setIsMask(false);
    }
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectOutputClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setSupportsTiles(true);
    }
    PageParamDescriptor* page = desc.definePageParam("Retime");
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("inputRangeEnable");
        p->setLabel("input range");
        p->setHint("Use this input range instead of the input's whole frame range.");
        p->setDefault(false);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*p);
    }
    frameParam(desc, page, "inputFirst", "", 1.)->setLayoutHint(eLayoutHintNoNewLine, 1);
    frameParam(desc, page, "inputLast", "", 100.);
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("outputRangeEnable");
        p->setLabel("output range");
        p->setHint("Stretch the input range to this output range (sets the speed).");
        p->setDefault(false);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*p);
    }
    frameParam(desc, page, "outputFirst", "", 1.)->setLayoutHint(eLayoutHintNoNewLine, 1);
    frameParam(desc, page, "outputLast", "", 100.);
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("reverse");
        p->setLabel("reverse");
        p->setHint("Play the input range backwards.");
        p->setDefault(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("speed");
        p->setLabel("speed");
        p->setHint("Playback speed: 0.5 = half speed (twice as long), 2 = double speed.");
        p->setDefault(1.);
        p->setRange(0.0001, 1000.);
        p->setDisplayRange(0.1, 4.);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintDivider);
        page->addChild(*p);
    }
    beforeAfter(desc, page, "before", "before");
    ChoiceParamDescriptor* after = beforeAfter(desc, page, "after", "after");
    after->setLayoutHint(eLayoutHintDivider);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("filter");
        p->setLabel("filter");
        p->setHint("none: nearest frame. blend: mix of the two nearest frames. box: average of the frames covered by the shutter.");
        p->appendOption("none");
        p->appendOption("blend");
        p->appendOption("box");
        p->setDefault(eFilterBox);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("shutter");
        p->setLabel("shutter");
        p->setHint("Box filter width, in output frames.");
        p->setDefault(1.);
        p->setRange(0., 100.);
        p->setDisplayRange(0., 4.);
        page->addChild(*p);
    }
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("rangeSet");
        p->setLabel("rangeSet");
        p->setDefault(false);
        p->setIsSecret(true);
        p->setAnimates(false);
        page->addChild(*p);
    }
}

ImageEffect*
APFXRetimeFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXRetimePlugin(h);
}
} // anon namespace

static APFXRetimeFactory apfxRetimeFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxRetimeFactory)
