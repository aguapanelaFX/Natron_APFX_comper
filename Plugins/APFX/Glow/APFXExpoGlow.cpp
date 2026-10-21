/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX ExpoGlow: exponential glow = stacked glows of doubling size whose strength
// falls off exponentially, built on an image pyramid (fast at any size).

#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXKeyOps.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.ExpoGlow"

class APFXExpoGlowPlugin
    : public ImageEffect
{
public:
    APFXExpoGlowPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true);
        _falloff = fetchDoubleParam("falloff");
        _intensity = fetchDoubleParam("intensity");
        _size = fetchDoubleParam("size");
        _addSource = fetchBooleanParam("addSource");
        _crop = fetchBooleanParam("cropToFormat");
    }

private:
    OfxRectD format()
    {
        const OfxPointD off = getProjectOffset(), sz = getProjectSize();
        OfxRectD r = { off.x, off.y, off.x + sz.x, off.y + sz.y };

        return r;
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        if ( _crop->getValueAtTime(args.time) ) {
            rod = format();

            return true;
        }
        rod = _src->getRegionOfDefinition(args.time);
        const double s = _size->getValueAtTime(args.time);
        rod.x1 -= s; rod.y1 -= s; rod.x2 += s; rod.y2 += s;

        return true;
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(args.time) );
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setClipBitDepth(*_dst, eBitDepthFloat);
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        const double t = args.time;
        Image* dst = _dst->fetchImage(t);
        Image* src = _src->isConnected() ? _src->fetchImage(t) : NULL;
        if ( !dst || !src || (src->getPixelComponents() != ePixelComponentRGBA) || (src->getPixelDepth() != eBitDepthFloat) ) {
            delete src;
            delete dst;
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        APFXOps::Plane in;
        const OfxRectI sb = APFXOfx::imageToPlane(src, in);
        // work area: source bounds, grown to cover the render window (the glow spills out)
        const OfxRectI win = args.renderWindow;
        OfxRectI b = { std::min(sb.x1, win.x1), std::min(sb.y1, win.y1), std::max(sb.x2, win.x2), std::max(sb.y2, win.y2) };
        APFXOps::Plane work(b.x2 - b.x1, b.y2 - b.y1, 4);
        for (int y = sb.y1; y < sb.y2; ++y) {
            std::copy( in.px(0, y - sb.y1), in.px(0, y - sb.y1) + (size_t)in.w * 4, work.px(sb.x1 - b.x1, y - b.y1) );
        }
        APFXKey::GlowParams p;
        p.falloff = (float)_falloff->getValueAtTime(t);
        p.intensity = (float)_intensity->getValueAtTime(t);
        p.size = (float)(_size->getValueAtTime(t) * args.renderScale.x);
        p.addSource = _addSource->getValueAtTime(t);
        APFXOps::Plane out;
        APFXKey::expoGlow( work, p, out, APFXOfx::hostParallelFor() );

        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        APFXOfx::writeOutput(*this, dst, win, out, b, &in, sb, o);
        delete mask;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    APFXOfx::StdParams _std;
    DoubleParam* _falloff;
    DoubleParam* _intensity;
    DoubleParam* _size;
    BooleanParam* _addSource;
    BooleanParam* _crop;
};

mDeclarePluginFactory(APFXExpoGlowFactory, {}, {});

void
APFXExpoGlowFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "ExpoGlow",
                            "APFX ExpoGlow: exponential glow. Stacks glows of doubling size whose strength falls off "
                            "exponentially: a hot core with a wide natural falloff. Glows the whole input: key / grade "
                            "what should glow before it.");
}

void
APFXExpoGlowFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("ExpoGlow");
    APFXOfx::defineChannels(desc, page, false);
    APFXOfx::defineDouble(desc, page, "falloff", "falloff", "Strength ratio between successive glow sizes: low = tight glow, high = huge haze.", 0.75, 0., 1., 0., 1.);
    APFXOfx::defineDouble(desc, page, "intensity", "intensity", "Overall glow strength.", 1., 0., 4., 0., 1000.);
    APFXOfx::defineDouble(desc, page, "size", "size", "How far the glow reaches, in pixels.", 200., 2., 2000., 1., 100000.);
    APFXOfx::defineBool(desc, page, "addSource", "add source", "Add the glow over the source image (off: glow only).", false);
    APFXOfx::defineBool(desc, page, "cropToFormat", "crop to format", "Keep the output inside the project format (off: the glow can spill past the edges).", true);
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXExpoGlowFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXExpoGlowPlugin(h);
}
} // anon namespace

static APFXExpoGlowFactory apfxExpoGlowFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxExpoGlowFactory)
