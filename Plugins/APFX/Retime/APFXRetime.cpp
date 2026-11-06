/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Retime, Nuke-like:
//  input.first / input.last / output.first / output.last are integer frames, each with a lock.
//  Unlocked fields always show live values: input = the input clip range, output.first =
//  input.first, output.last from the speed. Typing a value locks that field.
//  speed = (input.last - input.first) / (output.last - output.first) when both output ends
//  are locked, otherwise the unlocked output end follows the speed.
//  reverse, before/after (continue, loop, bounce, hold, black) outside output.first..last,
//  filter none (fractional frame) / nearest / box (frames averaged over the shutter).

#include <cmath>
#include <vector>
#include <algorithm>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxNatron.h"

#ifndef OVERRIDE
#define OVERRIDE override
#endif
#ifndef FINAL
#define FINAL final
#endif

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Retime"

enum BeforeAfter { eContinue = 0, eLoop, eBounce, eHold, eBlack };
enum Filter { eFilterNone = 0, eFilterNearest, eFilterBox };

struct Sample
{
    double frame;
    double w;
};

struct Ranges
{
    double inF, inL, outF, outL, speed;
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
        _inF = fetchIntParam("inputFirst");
        _inL = fetchIntParam("inputLast");
        _outF = fetchIntParam("outputFirst");
        _outL = fetchIntParam("outputLast");
        _inFLock = fetchBooleanParam("inputFirstLock");
        _inLLock = fetchBooleanParam("inputLastLock");
        _outFLock = fetchBooleanParam("outputFirstLock");
        _outLLock = fetchBooleanParam("outputLastLock");
        _reverse = fetchBooleanParam("reverse");
        _speed = fetchDoubleParam("speed");
        _before = fetchChoiceParam("before");
        _after = fetchChoiceParam("after");
        _filter = fetchChoiceParam("filter");
        _shutter = fetchDoubleParam("shutter");
        _shutter->setEnabled(_filter->getValue() == eFilterBox);
    }

private:
    // ------------------------------------------------------------ ranges
    Ranges compute() const
    {
        Ranges r;
        OfxRangeD clip = { 1., 1. };
        if ( _src->isConnected() ) {
            clip = _src->getFrameRange();
        }
        r.inF = _inFLock->getValue() ? _inF->getValue() : std::floor(clip.min + 0.5);
        r.inL = _inLLock->getValue() ? _inL->getValue() : std::floor(clip.max + 0.5);
        if (r.inL < r.inF) {
            std::swap(r.inF, r.inL);
        }
        const double inLen = r.inL - r.inF;
        const bool fL = _outFLock->getValue(), lL = _outLLock->getValue();
        if (fL && lL) {
            r.outF = _outF->getValue();
            r.outL = _outL->getValue();
            if (r.outL < r.outF) {
                std::swap(r.outF, r.outL);
            }
            r.speed = (r.outL > r.outF) ? inLen / (r.outL - r.outF) : 1.;
        } else {
            r.speed = std::max(1e-4, _speed->getValue());
            const double outLen = inLen / r.speed;
            if (lL) {
                r.outL = _outL->getValue();
                r.outF = r.outL - outLen;
            } else {
                r.outF = fL ? _outF->getValue() : r.inF;
                r.outL = r.outF + outLen;
            }
        }
        if (r.speed <= 0.) {
            r.speed = 1e-4;
        }

        return r;
    }

    // writes the live values into the unlocked fields (and speed when both outputs are locked)
    void refreshFields()
    {
        const Ranges r = compute();
        if ( !_inFLock->getValue() && (_inF->getValue() != (int)r.inF) ) {
            _inF->setValue( (int)r.inF );
        }
        if ( !_inLLock->getValue() && (_inL->getValue() != (int)r.inL) ) {
            _inL->setValue( (int)r.inL );
        }
        const int oF = (int)std::floor(r.outF + 0.5), oL = (int)std::floor(r.outL + 0.5);
        if ( !_outFLock->getValue() && (_outF->getValue() != oF) ) {
            _outF->setValue(oF);
        }
        if ( !_outLLock->getValue() && (_outL->getValue() != oL) ) {
            _outL->setValue(oL);
        }
        if ( _outFLock->getValue() && _outLLock->getValue() && (std::fabs(_speed->getValue() - r.speed) > 1e-9) ) {
            _speed->setValue(r.speed);
        }
        _speed->setEnabled( !( _outFLock->getValue() && _outLLock->getValue() ) );
        _shutter->setEnabled(_filter->getValue() == eFilterBox);
    }

    // source frame for output time t; false = black
    bool sourceFrame(double t, double* srcT, double* spd) const
    {
        const Ranges r = compute();
        *spd = r.speed;
        const double len = r.outL - r.outF;
        double u = t - r.outF;
        bool clampToInput = true;
        if ( (t < r.outF) || (t > r.outL) ) {
            const int mode = (t < r.outF) ? _before->getValue() : _after->getValue();
            if (mode == eBlack) {
                return false;
            }
            if (mode == eContinue) {
                clampToInput = false; // keep going at the same speed past the range
            } else if ( (mode == eHold) || (len <= 0.) ) {
                u = (t < r.outF) ? 0. : len;
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
        double s = _reverse->getValue() ? r.inL - u * r.speed : r.inF + u * r.speed;
        if (clampToInput) {
            s = std::max( r.inF, std::min(r.inL, s) );
        }
        *srcT = s;

        return true;
    }

    // source frames and weights for output time t
    bool samples(double t, std::vector<Sample>& out) const
    {
        out.clear();
        double s, spd;
        if ( !sourceFrame(t, &s, &spd) ) {
            return false;
        }
        const int f = _filter->getValue();
        if (f == eFilterNone) {
            out.push_back( { s, 1. } );
        } else if (f == eFilterNearest) {
            out.push_back( { std::floor(s + 0.5), 1. } );
        } else {
            // box: average the source frames covered by the shutter (output frames * speed)
            const double half = 0.5 * std::max(0., _shutter->getValue()) * spd;
            const double a = s - half, b = s + half;
            if (b - a < 1e-4) {
                out.push_back( { std::floor(s + 0.5), 1. } );
            } else {
                double total = 0.;
                for (double fr = std::floor(a + 0.5); fr <= std::floor(b + 0.5) && out.size() < 64; fr += 1.) {
                    const double w = std::min(b, fr + 0.5) - std::max(a, fr - 0.5);
                    if (w > 1e-6) {
                        out.push_back( { fr, w } );
                        total += w;
                    }
                }
                if ( out.empty() ) {
                    out.push_back( { std::floor(s + 0.5), 1. } );
                } else {
                    for (size_t i = 0; i < out.size(); ++i) {
                        out[i].w /= total;
                    }
                }
            }
        }

        return true;
    }

    // ------------------------------------------------------------ actions
    void changedParam(const InstanceChangedArgs& args, const std::string& n) OVERRIDE FINAL
    {
        if (args.reason != eChangeUserEdit) {
            return;
        }
        // Nuke: typing a value locks that field
        if (n == "inputFirst") {
            _inFLock->setValue(true);
        } else if (n == "inputLast") {
            _inLLock->setValue(true);
        } else if (n == "outputFirst") {
            _outFLock->setValue(true);
        } else if (n == "outputLast") {
            _outLLock->setValue(true);
        } else if ( (n == "speed") && _outFLock->getValue() && _outLLock->getValue() ) {
            _outLLock->setValue(false); // the speed drives the output end again
        }
        refreshFields();
    }

    void changedClip(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if (n == kOfxImageEffectSimpleSourceClipName) {
            refreshFields();
        }
    }

    bool getTimeDomain(OfxRangeD& range) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        const Ranges r = compute();
        range.min = std::floor(r.outF + 0.5);
        range.max = std::floor(r.outL + 0.5);

        return true;
    }

    void getFramesNeeded(const FramesNeededArguments& args, FramesNeededSetter& frames) OVERRIDE FINAL
    {
        std::vector<Sample> s;
        if ( !samples(args.time, s) ) {
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
            const OfxRectD r = _src->getRegionOfDefinition(s[i].frame);
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
        for (int y = win.y1; y < win.y2; ++y) {
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (d) {
                std::fill(d, d + (size_t)w * nc, 0.f);
            }
        }
        for (size_t i = 0; ok && i < s.size() && !abort(); ++i) {
            Image* src = _src->fetchImage(s[i].frame);
            if (!src) {
                continue;
            }
            const float wt = (float)s[i].w;
            if ( (src->getPixelDepth() == eBitDepthFloat) && (src->getPixelComponentCount() == nc) ) {
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
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    IntParam* _inF;
    IntParam* _inL;
    IntParam* _outF;
    IntParam* _outL;
    BooleanParam* _inFLock;
    BooleanParam* _inLLock;
    BooleanParam* _outFLock;
    BooleanParam* _outLLock;
    BooleanParam* _reverse;
    DoubleParam* _speed;
    ChoiceParam* _before;
    ChoiceParam* _after;
    ChoiceParam* _filter;
    DoubleParam* _shutter;
};

mDeclarePluginFactory(APFXRetimeFactory, {}, {});

void
APFXRetimeFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("Retime");
    desc.setPluginGrouping("Time");
    desc.setPluginDescription("APFX Retime (Nuke-like): maps the input frame range to the output frame range, or "
                              "retimes by a speed. Each range field has a lock; unlocked fields show live values "
                              "(typing a value locks it). Reverse, before/after (continue, loop, bounce, hold, black) "
                              "and filter: none (fractional frames), nearest, box (averages frames over the shutter).");
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
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone); // no channels row (like Nuke's Retime)
#endif
}

static void
lockParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint)
{
    BooleanParamDescriptor* p = desc.defineBooleanParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(false);
    p->setAnimates(false);
    p->setLayoutHint(eLayoutHintNoNewLine, 1);
    page->addChild(*p);
}

static IntParamDescriptor*
frameParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint, int def)
{
    IntParamDescriptor* p = desc.defineIntParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(-10000000, 10000000);
    p->setDisplayRange(1, 200);
    p->setAnimates(false);
    page->addChild(*p);

    return p;
}

static ChoiceParamDescriptor*
beforeAfter(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint)
{
    ChoiceParamDescriptor* p = desc.defineChoiceParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->appendOption("continue", "Keep retiming past the range at the same speed.");
    p->appendOption("loop", "Repeat the range.");
    p->appendOption("bounce", "Play the range back and forth.");
    p->appendOption("hold", "Hold the end frame.");
    p->appendOption("black", "Output black.");
    p->setDefault(eHold);
    p->setAnimates(false);
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

    // input range  [x] first  [x] last
    lockParam(desc, page, "inputFirstLock", "input range", "Lock input.first (otherwise it follows the input clip).");
    frameParam(desc, page, "inputFirst", "", "First input frame used. Typing a value locks it.", 1)->setLayoutHint(eLayoutHintNoNewLine, 1);
    lockParam(desc, page, "inputLastLock", "", "Lock input.last (otherwise it follows the input clip).");
    frameParam(desc, page, "inputLast", "", "Last input frame used. Typing a value locks it.", 100);
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("reverse");
        p->setLabel("reverse");
        p->setHint("Play the input range backwards.");
        p->setDefault(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
    // output range [x] first  [x] last
    lockParam(desc, page, "outputFirstLock", "output range", "Lock output.first (otherwise it equals input.first).");
    frameParam(desc, page, "outputFirst", "", "First output frame. Typing a value locks it; the last frame follows the speed unless locked.", 1)->setLayoutHint(eLayoutHintNoNewLine, 1);
    lockParam(desc, page, "outputLastLock", "", "Lock output.last (both ends locked: the speed is computed so the ranges match).");
    frameParam(desc, page, "outputLast", "", "Last output frame. Typing a value locks it.", 100);
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("speed");
        p->setLabel("speed");
        p->setHint("Playback speed: 0.5 = half speed (twice as long), 2 = double speed. Computed when both output ends are locked.");
        p->setDefault(1.);
        p->setRange(0.0001, 1000.);
        p->setDisplayRange(0.1, 4.);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintDivider);
        page->addChild(*p);
    }
    beforeAfter(desc, page, "before", "before", "What to output before output.first.");
    beforeAfter(desc, page, "after", "after", "What to output after output.last.")->setLayoutHint(eLayoutHintDivider);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("filter");
        p->setLabel("filter");
        p->setHint("none: asks the input for the fractional frame. nearest: rounds to the nearest frame. box: averages the frames covered by the shutter.");
        p->appendOption("none");
        p->appendOption("nearest");
        p->appendOption("box");
        p->setDefault(eFilterBox);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("shutter");
        p->setLabel("shutter");
        p->setHint("Box filter width in output frames: lower values blend less.");
        p->setDefault(1.);
        p->setRange(0., 100.);
        p->setDisplayRange(0., 4.);
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
