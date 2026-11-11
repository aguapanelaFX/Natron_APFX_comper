/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Toe: like Nuke's Toe. Lifts the blacks with a smooth curve that only affects values below
// 'range' and leaves everything above untouched (no change in the mids / highs):
//   x < range: out = x + lift * (1 - x / range)^2      (out(0) = lift, slope 1 and value x at x = range)
//   x >= range: out = x

#include <cmath>

#include "APFXOfxCommon.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Toe"

class APFXToePlugin
    : public ImageEffect
{
public:
    APFXToePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true, true);
        _lift = fetchDoubleParam("lift");
        _range = fetchDoubleParam("range");
    }

private:
    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& /*identityTime*/
#ifdef OFX_EXTENSIONS_NUKE
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        if (_lift->getValueAtTime(args.time) == 0.) {
            identityClip = _src;

            return true;
        }

        return false;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
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
        const float lift = (float)_lift->getValueAtTime(t);
        const float range = (float)std::max(1e-6, _range->getValueAtTime(t));
        APFXOps::Plane res(in.w, in.h, 4);
        APFXOfx::hostParallelFor()([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < in.w; ++x) {
                    const float* s = in.px(x, y);
                    float* r = res.px(x, y);
                    for (int c = 0; c < 4; ++c) {
                        const float v = s[c];
                        if ( (c < 4) && (v < range) ) {
                            const float k = 1.f - std::max(0.f, v) / range;
                            r[c] = v + lift * k * k;
                        } else {
                            r[c] = v;
                        }
                    }
                }
            }
        }, in.h);
        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        APFXOfx::writeOutput(*this, dst, args.renderWindow, res, sb, &in, sb, o);
        delete mask;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    APFXOfx::StdParams _std;
    DoubleParam* _lift;
    DoubleParam* _range;
};

mDeclarePluginFactory(APFXToeFactory, {}, {});

void
APFXToeFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Toe",
                            "APFX Toe: lifts the blacks with a smooth curve that only affects values below Range; "
                            "values above Range are untouched.");
    desc.setPluginGrouping("Color");
}

void
APFXToeFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("Toe");
    APFXOfx::defineChannels(desc, page, false);
    APFXOfx::defineDouble(desc, page, "lift", "lift", "Value the blacks are lifted to (output at input 0).", 0., -0.5, 0.5, -10., 10.);
    APFXOfx::defineDouble(desc, page, "range", "range", "Values below this are affected (smooth roll-off, values above are untouched).", 0.2, 0.001, 1., 0.000001, 100.);
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXToeFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXToePlugin(h);
}
} // anon namespace

static APFXToeFactory apfxToeFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxToeFactory)
