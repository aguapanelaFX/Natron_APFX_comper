/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Keyer: like Nuke's Keyer. An operation turns the pixel into one value, the range A B C D maps it to alpha:
//   0 below A, ramp A->B, 1 between B and C, ramp C->D, 0 above D (A=0 B=1 C=1 D=1: plain luminance key).
// rgb goes through, alpha is replaced by the key.

#include <algorithm>
#include <cmath>
#include <cfloat>

#include "APFXOfxCommon.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Keyer"

enum OpEnum { eOpLum = 0, eOpRed, eOpGreen, eOpBlue, eOpSat, eOpMin, eOpMax, eOpGreenscreen, eOpBluescreen };

class APFXKeyerPlugin
    : public ImageEffect
{
public:
    APFXKeyerPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true, false);
        _op = fetchChoiceParam("operation");
        _a = fetchDoubleParam("rangeA");
        _b = fetchDoubleParam("rangeB");
        _c = fetchDoubleParam("rangeC");
        _d = fetchDoubleParam("rangeD");
    }

private:
    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setOutputPremultiplication(eImageUnPreMultiplied);
    }

    static float value(int op, const float* s)
    {
        const float r = s[0], g = s[1], b = s[2];
        switch (op) {
        case eOpRed: return r;
        case eOpGreen: return g;
        case eOpBlue: return b;
        case eOpSat: {
            const float mx = std::max( r, std::max(g, b) ), mn = std::min( r, std::min(g, b) );

            return mx > 0.f ? (mx - mn) / mx : 0.f;
        }
        case eOpMin: return std::min( r, std::min(g, b) );
        case eOpMax: return std::max( r, std::max(g, b) );
        case eOpGreenscreen: return g - std::max(r, b);
        case eOpBluescreen: return b - std::max(r, g);
        default: return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }
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
        const int op = _op->getValueAtTime(t);
        const float A = (float)_a->getValueAtTime(t), B = (float)_b->getValueAtTime(t);
        const float C = (float)_c->getValueAtTime(t), D = (float)_d->getValueAtTime(t);
        APFXOps::Plane res(in.w, in.h, 4);
        APFXOfx::hostParallelFor()([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < in.w; ++x) {
                    const float* s = in.px(x, y);
                    float* r = res.px(x, y);
                    const float v = value(op, s);
                    float k;
                    if (v < A) {
                        k = 0.f;
                    } else if (v < B) {
                        k = (B > A) ? (v - A) / (B - A) : 1.f;
                    } else if (v <= C) {
                        k = 1.f;
                    } else if (v < D) {
                        k = (D > C) ? (D - v) / (D - C) : 0.f;
                    } else {
                        k = (D <= C) ? 1.f : 0.f; // D == C == B: everything above B is keyed
                    }
                    r[0] = s[0];
                    r[1] = s[1];
                    r[2] = s[2];
                    r[3] = k;
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
    ChoiceParam* _op;
    DoubleParam* _a;
    DoubleParam* _b;
    DoubleParam* _c;
    DoubleParam* _d;
};

mDeclarePluginFactory(APFXKeyerFactory, {}, {});

void
APFXKeyerFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Keyer",
                            "APFX Keyer: the operation turns each pixel into a value, the range A B C D maps it "
                            "to alpha (0 below A, ramp A to B, 1 from B to C, ramp C to D, 0 above D). RGB goes through.");
    desc.setPluginGrouping("Keyer");
    desc.setSupportsTiles(false);
}

static void
rangeParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, double def, bool last)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabel(label);
    p->setHint("Range: 0 below A, ramp A to B, 1 from B to C, ramp C to D, 0 above D.");
    p->setDefault(def);
    p->setRange(-DBL_MAX, DBL_MAX);
    p->setDisplayRange(0., 1.);
    p->setIncrement(0.01);
    if (!last) {
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
    }
    page->addChild(*p);
}

void
APFXKeyerFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("Keyer");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("operation");
        p->setLabel("operation");
        p->appendOption("luminance key");
        p->appendOption("red keyer");
        p->appendOption("green keyer");
        p->appendOption("blue keyer");
        p->appendOption("saturation key");
        p->appendOption("min keyer");
        p->appendOption("max keyer");
        p->appendOption("greenscreen");
        p->appendOption("bluescreen");
        p->setDefault(eOpLum);
        page->addChild(*p);
    }
    rangeParam(desc, page, "rangeA", "range  A", 0., false);
    rangeParam(desc, page, "rangeB", "B", 1., false);
    rangeParam(desc, page, "rangeC", "C", 1., false);
    rangeParam(desc, page, "rangeD", "D", 1., true);
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXKeyerFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXKeyerPlugin(h);
}
} // anon namespace

static APFXKeyerFactory apfxKeyerFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxKeyerFactory)
