/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX STMapGradient: identity UV map for STMap. red = U, green = V, measured at pixel centres
// over the format (the Source format when connected, else the comp format); blue 0, alpha 1.
// Feeding it straight into STMap reproduces the source exactly.

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

#define kPluginIdentifier "com.aguapanelafx.APFX.STMapGradient"

class APFXSTMapGradientPlugin
    : public ImageEffect
{
public:
    APFXSTMapGradientPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = getContext() == eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
        _blue = fetchDoubleParam("blue");
    }

private:
    // format the map covers (pixels)
    OfxRectD format(double time)
    {
        if ( _src && _src->isConnected() ) {
#ifdef OFX_EXTENSIONS_NATRON
            OfxRectI f = { 0, 0, 0, 0 };
            _src->getFormat(f);
            if ( (f.x2 > f.x1) && (f.y2 > f.y1) ) {
                OfxRectD r = { (double)f.x1, (double)f.y1, (double)f.x2, (double)f.y2 };

                return r;
            }
#endif

            return _src->getRegionOfDefinition(time);
        }
        const OfxPointD off = getProjectOffset(), sz = getProjectSize();
        OfxRectD r = { off.x, off.y, off.x + sz.x, off.y + sz.y };

        return r;
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        rod = format(args.time);

        return true;
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& /*args*/, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        if (_src) {
            OfxRectD empty = { 0., 0., 0., 0. };
            rois.setRegionOfInterest(*_src, empty); // the source is only read for its format
        }
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setClipBitDepth(*_dst, eBitDepthFloat);
        cp.setOutputFrameVarying(false);
#ifdef OFX_EXTENSIONS_NATRON
        const OfxRectD f = format(0.);
        OfxRectI fi = { (int)f.x1, (int)f.y1, (int)f.x2, (int)f.y2 };
        cp.setOutputFormat(fi);
#endif
    }

    bool isIdentity(const IsIdentityArguments& /*args*/, Clip*& /*identityClip*/, double& /*identityTime*/
#ifdef OFX_EXTENSIONS_NUKE
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        return false;
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        Image* dst = _dst->fetchImage(args.time);
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        const OfxRectD f = format(args.time);
        // render scale: the map is computed in full-resolution pixel coordinates
        const double sx = args.renderScale.x, sy = args.renderScale.y;
        const double w = std::max(1., f.x2 - f.x1), h = std::max(1., f.y2 - f.y1);
        const float blue = (float)_blue->getValueAtTime(args.time);
        const OfxRectI& win = args.renderWindow;
        for (int y = win.y1; y < win.y2; ++y) {
            if ( abort() ) {
                break;
            }
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (!d) {
                continue;
            }
            // pixel centre in full-res pixels
            const float v = (float)( ( (y + 0.5) / sy - f.y1 ) / h );
            for (int x = win.x1; x < win.x2; ++x, d += 4) {
                d[0] = (float)( ( (x + 0.5) / sx - f.x1 ) / w );
                d[1] = v;
                d[2] = blue;
                d[3] = 1.f;
            }
        }
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    DoubleParam* _blue;
};

mDeclarePluginFactory(APFXSTMapGradientFactory, {}, {});

void
APFXSTMapGradientFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("STMapGradient");
    desc.setPluginGrouping("Transform");
    desc.setPluginDescription("APFX STMapGradient: identity UV map for STMap. red = U, green = V (0..1 across the format, "
                              "measured at pixel centres), blue = constant, alpha = 1. Uses the Source format when connected, "
                              "else the comp format. Distort it (Transform, LensDistortion, GridWarp...) and feed it to STMap "
                              "to apply the same distortion to any image.");
    desc.addSupportedContext(eContextGenerator);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
}

void
APFXSTMapGradientFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context)
{
    if (context == eContextGeneral) {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setOptional(true);
        c->setSupportsTiles(true);
        c->setIsMask(false);
    }
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectOutputClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->setSupportsTiles(true);
    }
    PageParamDescriptor* page = desc.definePageParam("STMapGradient");
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("blue");
        p->setLabel("blue");
        p->setHint("Value written to the blue channel (STMap ignores it).");
        p->setDefault(0.);
        p->setRange(-1e9, 1e9);
        p->setDisplayRange(0., 1.);
        page->addChild(*p);
    }
}

ImageEffect*
APFXSTMapGradientFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXSTMapGradientPlugin(h);
}
} // anon namespace

static APFXSTMapGradientFactory apfxSTMapGradientFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxSTMapGradientFactory)
