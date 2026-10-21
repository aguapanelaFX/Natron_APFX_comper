/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Copy: Nuke-like Copy. Output = B, with up to 4 channels copied from A
// (default: A alpha -> alpha). Mask and mix.

#include <algorithm>
#include <string>

#include "APFXOfxCommon.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Copy"
#define kRows 4

class APFXCopyPlugin
    : public ImageEffect
{
public:
    APFXCopyPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _a = fetchClip("A");
        _b = fetchClip("B");
        _mask = fetchClip("Mask");
        _mix = fetchDoubleParam("mix");
        _maskInvert = fetchBooleanParam("maskInvert");
        for (int i = 0; i < kRows; ++i) {
            _from[i] = fetchChoiceParam( std::string("from") + char('1' + i) );
            _to[i] = fetchChoiceParam( std::string("to") + char('1' + i) );
        }
    }

private:
    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        if ( !_b->isConnected() ) {
            return false;
        }
        rod = _b->getRegionOfDefinition(args.time);

        return true;
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime
#ifdef OFX_EXTENSIONS_NUKE
                    , int&, std::string&
#endif
                    ) OVERRIDE FINAL
    {
        bool any = false;
        for (int i = 0; i < kRows; ++i) {
            any |= (_from[i]->getValueAtTime(args.time) > 0) && (_to[i]->getValueAtTime(args.time) > 0);
        }
        if ( !any || !_a->isConnected() || (_mix->getValueAtTime(args.time) <= 0.) ) {
            identityClip = _b;
            identityTime = args.time;

            return true;
        }

        return false;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_a, ePixelComponentRGBA);
        cp.setClipComponents(*_b, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setClipBitDepth(*_dst, eBitDepthFloat);
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        const double t = args.time;
        Image* dst = _dst->fetchImage(t);
        Image* b = _b->isConnected() ? _b->fetchImage(t) : NULL;
        Image* a = _a->isConnected() ? _a->fetchImage(t) : NULL;
        Image* m = _mask->isConnected() ? _mask->fetchImage(t) : NULL;
        if ( !dst || (dst->getPixelComponents() != ePixelComponentRGBA) || (dst->getPixelDepth() != eBitDepthFloat) ) {
            delete a; delete b; delete m; delete dst;
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        int from[kRows], to[kRows];
        for (int i = 0; i < kRows; ++i) {
            from[i] = _from[i]->getValueAtTime(t) - 1; // -1 = none, 0..3 = r g b a
            to[i] = _to[i]->getValueAtTime(t) - 1;
        }
        const float mix = (float)_mix->getValueAtTime(t);
        const bool inv = _maskInvert->getValueAtTime(t);
        const OfxRectI win = args.renderWindow;
        APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        pf([&](int y0, int y1) {
            for (int y = win.y1 + y0; y < win.y1 + y1; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                for (int x = win.x1; x < win.x2; ++x, d += 4) {
                    const float* pb = b ? (const float*)b->getPixelAddress(x, y) : NULL;
                    const float* pa = a ? (const float*)a->getPixelAddress(x, y) : NULL;
                    float out[4] = { pb ? pb[0] : 0.f, pb ? pb[1] : 0.f, pb ? pb[2] : 0.f, pb ? pb[3] : 0.f };
                    float res[4] = { out[0], out[1], out[2], out[3] };
                    for (int i = 0; i < kRows; ++i) {
                        if ( (from[i] >= 0) && (to[i] >= 0) ) {
                            res[to[i]] = pa ? pa[from[i]] : 0.f;
                        }
                    }
                    float k = mix;
                    if (m) {
                        const float* mp = (const float*)m->getPixelAddress(x, y);
                        float mv = mp ? *mp : 0.f;
                        if (inv) {
                            mv = 1.f - mv;
                        }
                        k *= std::max( 0.f, std::min(1.f, mv) );
                    }
                    for (int c = 0; c < 4; ++c) {
                        d[c] = out[c] + (res[c] - out[c]) * k;
                    }
                }
            }
        }, win.y2 - win.y1);
        delete a; delete b; delete m; delete dst;
    }

    Clip* _dst;
    Clip* _a;
    Clip* _b;
    Clip* _mask;
    DoubleParam* _mix;
    BooleanParam* _maskInvert;
    ChoiceParam* _from[kRows];
    ChoiceParam* _to[kRows];
};

mDeclarePluginFactory(APFXCopyFactory, {}, {});

void
APFXCopyFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("Copy");
    desc.setPluginGrouping("APFX");
    desc.setPluginDescription("APFX Copy: output is B, with up to 4 channels copied from A (default: A alpha into B alpha), like Nuke's Copy.");
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
}

void
APFXCopyFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    // B first: it is the main input (the one that passes through)
    ClipDescriptor* cb = desc.defineClip("B");
    cb->addSupportedComponent(ePixelComponentRGBA);
    cb->setSupportsTiles(true);
    ClipDescriptor* ca = desc.defineClip("A");
    ca->addSupportedComponent(ePixelComponentRGBA);
    ca->setSupportsTiles(true);
    ca->setOptional(true);
    ClipDescriptor* co = desc.defineClip(kOfxImageEffectOutputClipName);
    co->addSupportedComponent(ePixelComponentRGBA);
    co->setSupportsTiles(true);
    ClipDescriptor* cm = desc.defineClip("Mask");
    cm->addSupportedComponent(ePixelComponentAlpha);
    cm->setOptional(true);
    cm->setSupportsTiles(true);
    cm->setIsMask(true);

    PageParamDescriptor* page = desc.definePageParam("Copy");
    const char* chans[5] = { "none", "rgba.red", "rgba.green", "rgba.blue", "rgba.alpha" };
    const char* ids[5] = { "none", "r", "g", "b", "a" };
    for (int i = 0; i < kRows; ++i) {
        ChoiceParamDescriptor* f = desc.defineChoiceParam( std::string("from") + char('1' + i) );
        f->setLabel(i == 0 ? "copy channel" : "");
        f->setHint("Channel of A to copy.");
        ChoiceParamDescriptor* t = desc.defineChoiceParam( std::string("to") + char('1' + i) );
        t->setLabel("to");
        t->setHint("Channel of B it replaces.");
        for (int k = 0; k < 5; ++k) {
            f->appendOption(chans[k], "", ids[k]);
            t->appendOption(chans[k], "", ids[k]);
        }
        f->setDefault(i == 0 ? 4 : 0);
        t->setDefault(i == 0 ? 4 : 0);
        f->setAnimates(false);
        t->setAnimates(false);
        f->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*f);
        page->addChild(*t);
    }
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXCopyFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXCopyPlugin(h);
}
} // anon namespace

static APFXCopyFactory apfxCopyFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxCopyFactory)
