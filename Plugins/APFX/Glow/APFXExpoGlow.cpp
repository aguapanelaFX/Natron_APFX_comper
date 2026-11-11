/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX ExpoGlow: Stuart Lashley's Shake "Exponential Glow" (as in the Nuke gizmo port by brad-f),
// same knobs and maths, one fast node:
//  1. luminance key (alpha = luma clamped 0..1) + premult  -> glows the highlights (rgb * luma)
//  2. grade x intensity / 10
//  3. 9 blurs whose size doubles: size_i = width / 3 * (Size * 2^i) / 100
//  4. each blur weighted by (1 / falloff)^i, all added (+ the source when Add Source)
//  5. crop to format
// The blurs are done on an image pyramid (fast whatever their size).

#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXLensOps.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.ExpoGlow"
#define kBlurs 9

class APFXExpoGlowPlugin
    : public ImageEffect
{
public:
    APFXExpoGlowPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _falloff = fetchDoubleParam("falloff");
        _intensity = fetchDoubleParam("intensity");
        _size = fetchDoubleParam("size");
        _addSource = fetchBooleanParam("addSource");
        _crop = fetchBooleanParam("cropToFormat");
    }

private:
    OfxRectD format()
    {
#ifdef OFX_EXTENSIONS_NATRON
        if ( _src->isConnected() ) {
            OfxRectI f = { 0, 0, 0, 0 };
            _src->getFormat(f);
            if ( (f.x2 > f.x1) && (f.y2 > f.y1) ) {
                OfxRectD r = { (double)f.x1, (double)f.y1, (double)f.x2, (double)f.y2 };

                return r;
            }
        }
#endif
        const OfxPointD off = getProjectOffset(), sz = getProjectSize();
        OfxRectD r = { off.x, off.y, off.x + sz.x, off.y + sz.y };

        return r;
    }

    // blur size (Nuke Blur "size", pixels) of level i
    double blurSize(double time, int i)
    {
        const OfxRectD f = format();

        return (f.x2 - f.x1) / 3. * ( _size->getValueAtTime(time) * std::pow(2., i) ) / 100.;
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        const OfxRectD f = format();
        if ( _crop->getValueAtTime(args.time) ) {
            rod = f;

            return true;
        }
        rod = _src->getRegionOfDefinition(args.time);
        // the glow spills out: at most one format size around the source
        const double s = std::min( blurSize(args.time, kBlurs - 1), (f.x2 - f.x1) );
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
        APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        APFXOps::Plane in;
        const OfxRectI sb = APFXOfx::imageToPlane(src, in);
        const OfxRectI win = args.renderWindow;
        // work area = source bounds grown to the render window (the glow spills out)
        const OfxRectI b = { std::min(sb.x1, win.x1), std::min(sb.y1, win.y1), std::max(sb.x2, win.x2), std::max(sb.y2, win.y2) };
        const int W = b.x2 - b.x1, H = b.y2 - b.y1;

        // 1-2. luminance key, premult, keep rgb, grade x intensity*10/100
        const float gain = (float)( _intensity->getValueAtTime(t) * 10. / 100. );
        APFXOps::Plane key(W, H, 3);
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int sy = b.y1 + y - sb.y1;
                if ( (sy < 0) || (sy >= in.h) ) {
                    continue;
                }
                for (int x = 0; x < W; ++x) {
                    const int sx = b.x1 + x - sb.x1;
                    if ( (sx < 0) || (sx >= in.w) ) {
                        continue;
                    }
                    const float* s = in.px(sx, sy);
                    const float a = APFXLens::clampf(APFXLens::luma709(s), 0.f, 1.f);
                    float* k = key.px(x, y);
                    k[0] = s[0] * a * gain;
                    k[1] = s[1] * a * gain;
                    k[2] = s[2] * a * gain;
                }
            }
        }, H);

        // 3-4. 9 doubling blurs weighted by (1 / falloff)^i
        const double falloff = std::max(1e-3, _falloff->getValueAtTime(t));
        APFXOps::Plane glow(W, H, 3);
        for (int i = 0; i < kBlurs; ++i) {
            if ( abort() ) {
                break;
            }
            // Nuke Blur size -> gaussian sigma (size ~ 2.5 sigma), at the render scale
            const float sigma = (float)( blurSize(t, i) * args.renderScale.x / 2.5 );
            const float w = (float)std::pow(1. / falloff, (double)i);
            APFXOps::Plane bl = APFXLens::bigBlur(key, sigma, pf);
            pf([&](int y0, int y1) {
                for (size_t j = (size_t)y0 * W * 3; j < (size_t)y1 * W * 3; ++j) {
                    glow.d[j] += bl.d[j] * w;
                }
            }, H);
        }

        // 5. + source, crop (the RoD does the crop to format)
        const bool addSource = _addSource->getValueAtTime(t);
        pf([&](int y0, int y1) {
            for (int y = win.y1 + y0; y < win.y1 + y1; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                for (int x = win.x1; x < win.x2; ++x, d += 4) {
                    const float* g = glow.px(x - b.x1, y - b.y1);
                    const float* s = APFXOfx::planeAt(in, sb, x, y);
                    for (int c = 0; c < 3; ++c) {
                        d[c] = g[c] + ( (addSource && s) ? s[c] : 0.f );
                    }
                    d[3] = (addSource && s) ? s[3] : 0.f;
                }
            }
        }, win.y2 - win.y1);
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
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
                            "APFX ExpoGlow: exponential glow based on Stuart Lashley's Exponential Glow (port by brad-f). Glows the highs of the "
                            "input with a natural exponential falloff: 9 blurs of doubling size (Size = % of width / 3), each weighted "
                            "by 1/Falloff more than the previous one.");
    desc.setPluginGrouping("Filter");
}

void
APFXExpoGlowFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    PageParamDescriptor* page = desc.definePageParam("Exponential Glow");
    APFXOfx::defineDouble(desc, page, "falloff", "Falloff", "Weight ratio between successive (doubling) blurs: each bigger blur counts 1/falloff more.", 0.81, 0., 2., 0.001, 100.);
    APFXOfx::defineDouble(desc, page, "intensity", "Intensity", "Glow strength.", 3.85, 0., 10., 0., 1000.);
    APFXOfx::defineDouble(desc, page, "size", "Size", "Size of the first blur, in % of width / 3 (the next ones double).", 4.1, 0., 5., 0., 100.);
    APFXOfx::defineBool(desc, page, "addSource", "Add Source", "Add the glow over the source image (off: glow only).", true);
    APFXOfx::defineBool(desc, page, "cropToFormat", "Crop to format", "Keep the output inside the format.", true);
}

ImageEffect*
APFXExpoGlowFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXExpoGlowPlugin(h);
}
} // anon namespace

static APFXExpoGlowFactory apfxExpoGlowFactory(kPluginIdentifier, 2, 0);
mRegisterPluginFactoryInstance(apfxExpoGlowFactory)
