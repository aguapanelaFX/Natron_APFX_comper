/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Halation: film halation. Bright light passes through the emulsion, reflects off the film
// base (no / thin anti-halation layer) and re-exposes the emulsion from behind. The red-sensitive
// layer is the deepest, so the halo is mostly red, a bit of green, almost no blue.
//  1. highlights above threshold (soft knee), as energy (luma)
//  2. halo = core (tight) + tail (wide) gaussians, size in % of width scaled by the film gauge
//     (the same physical spread is bigger relative to a small frame)
//  3. added per channel with the R / G / B response and tint (optionally energy conserving)
//  4. optional neutral lens bloom

#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXLensOps.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Halation"

struct Preset
{
    const char* label;
    double threshold, knee, intensity, size, core, coreSize;
    int gauge;
    double resp[3];
    double tint[3];
    double bloom;
};

const double kGaugeScale[] = { 2.2, 1.0, 0.55 }; // 16mm, 35mm, 65mm

// approximate looks (not measured film data)
const Preset kPresets[] = {
    { "Custom", 0, 0, 0, 0, 0, 0, 1, { 1, 1, 1 }, { 1, 1, 1 }, 0 },
    { "CineStill 800T (no rem-jet, strong red)", 0.6, 0.4, 1.6, 0.8, 0.35, 0.15, 1, { 1.0, 0.18, 0.02 }, { 1.0, 0.35, 0.2 }, 0.0 },
    { "Kodak Vision3 500T (subtle warm)", 1.0, 0.5, 0.45, 0.5, 0.5, 0.15, 1, { 1.0, 0.3, 0.05 }, { 1.0, 0.55, 0.35 }, 0.0 },
    { "Kodak Vision3 250D (daylight, faint)", 1.2, 0.5, 0.3, 0.45, 0.5, 0.15, 1, { 1.0, 0.35, 0.08 }, { 1.0, 0.6, 0.4 }, 0.0 },
    { "16mm (big soft halo)", 0.8, 0.5, 0.7, 0.6, 0.4, 0.2, 0, { 1.0, 0.28, 0.05 }, { 1.0, 0.5, 0.3 }, 0.1 },
    { "Subtle digital (bloom only feel)", 1.5, 0.8, 0.25, 0.35, 0.6, 0.1, 1, { 1.0, 0.6, 0.35 }, { 1.0, 0.85, 0.75 }, 0.15 },
};
const int kNbPresets = (int)( sizeof(kPresets) / sizeof(kPresets[0]) );

class APFXHalationPlugin
    : public ImageEffect
{
public:
    APFXHalationPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true, false);
        _preset = fetchChoiceParam("preset");
        _threshold = fetchDoubleParam("threshold");
        _knee = fetchDoubleParam("knee");
        _intensity = fetchDoubleParam("intensity");
        _size = fetchDoubleParam("size");
        _gauge = fetchChoiceParam("gauge");
        _core = fetchDoubleParam("core");
        _coreSize = fetchDoubleParam("coreSize");
        _resp = fetchRGBParam("response");
        _tint = fetchRGBParam("tint");
        _energy = fetchBooleanParam("energyConserving");
        _bloom = fetchDoubleParam("bloom");
        _bloomSize = fetchDoubleParam("bloomSize");
        _haloOnly = fetchBooleanParam("haloOnly");
    }

private:
    void changedParam(const InstanceChangedArgs& args, const std::string& name) OVERRIDE FINAL
    {
        if ( (name == "preset") && (args.reason == eChangeUserEdit) ) {
            const int i = _preset->getValue();
            if ( (i > 0) && (i < kNbPresets) ) {
                const Preset& p = kPresets[i];
                beginEditBlock("preset");
                _threshold->setValue(p.threshold);
                _knee->setValue(p.knee);
                _intensity->setValue(p.intensity);
                _size->setValue(p.size);
                _core->setValue(p.core);
                _coreSize->setValue(p.coreSize);
                _gauge->setValue(p.gauge);
                _resp->setValue(p.resp[0], p.resp[1], p.resp[2]);
                _tint->setValue(p.tint[0], p.tint[1], p.tint[2]);
                _bloom->setValue(p.bloom);
                endEditBlock();
            }
        }
    }

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

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        rod = _src->getRegionOfDefinition(args.time);

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
        const int W = in.w, H = in.h;

        const float thr = (float)_threshold->getValueAtTime(t);
        const float knee = (float)std::max(1e-4, _knee->getValueAtTime(t));
        // 1. highlight energy with a soft (quadratic) knee
        APFXOps::Plane hl(W, H, 1);
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < W; ++x) {
                    const float l = std::max( 0.f, APFXLens::luma709( in.px(x, y) ) );
                    float e;
                    if (l <= thr - knee) {
                        e = 0.f;
                    } else if (l < thr + knee) {
                        const float q = l - thr + knee;
                        e = q * q / (4.f * knee);
                    } else {
                        e = l - thr;
                    }
                    hl.px(x, y)[0] = e;
                }
            }
        }, H);

        // 2. core + tail
        const OfxRectD f = format();
        const double fw = f.x2 - f.x1;
        const int gauge = std::max( 0, std::min(2, _gauge->getValueAtTime(t)) );
        const double px = fw * args.renderScale.x / 100. * kGaugeScale[gauge];
        const float tail = (float)( _size->getValueAtTime(t) * px );
        const float coreS = (float)( _coreSize->getValueAtTime(t) * px );
        const float coreW = (float)APFXLens::clampf( (float)_core->getValueAtTime(t), 0.f, 1.f );
        APFXOps::Plane a = APFXLens::bigBlur(hl, coreS, pf);
        APFXOps::Plane b = APFXLens::bigBlur(hl, tail, pf);
        const float bloomA = (float)_bloom->getValueAtTime(t);
        APFXOps::Plane bl;
        if (bloomA > 0.f) {
            bl = APFXLens::bigBlur( hl, (float)( _bloomSize->getValueAtTime(t) * px ), pf );
        }

        // 3. per channel response
        double resp[3], tint[3];
        _resp->getValueAtTime(t, resp[0], resp[1], resp[2]);
        _tint->getValueAtTime(t, tint[0], tint[1], tint[2]);
        float k[3];
        for (int c = 0; c < 3; ++c) {
            k[c] = (float)( resp[c] * tint[c] * _intensity->getValueAtTime(t) );
        }
        const bool energy = _energy->getValueAtTime(t);
        const bool haloOnly = _haloOnly->getValueAtTime(t);
        APFXOps::Plane res(W, H, 4);
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < W; ++x) {
                    const float* s = in.px(x, y);
                    float* r = res.px(x, y);
                    const float h0 = hl.px(x, y)[0];
                    const float halo = a.px(x, y)[0] * coreW + b.px(x, y)[0] * (1.f - coreW);
                    // energy conserving: the halo light is taken from where it came from
                    const float add = energy ? halo - h0 : halo;
                    const float bloom = bloomA > 0.f ? ( energy ? bl.px(x, y)[0] - h0 : bl.px(x, y)[0] ) * bloomA : 0.f;
                    for (int c = 0; c < 3; ++c) {
                        const float v = add * k[c] + bloom;
                        r[c] = haloOnly ? std::max(0.f, halo * k[c] + (bloomA > 0.f ? bl.px(x, y)[0] * bloomA : 0.f)) : std::max(0.f, s[c] + v);
                    }
                    r[3] = s[3];
                }
            }
        }, H);

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
    ChoiceParam* _preset;
    DoubleParam* _threshold;
    DoubleParam* _knee;
    DoubleParam* _intensity;
    DoubleParam* _size;
    ChoiceParam* _gauge;
    DoubleParam* _core;
    DoubleParam* _coreSize;
    RGBParam* _resp;
    RGBParam* _tint;
    BooleanParam* _energy;
    DoubleParam* _bloom;
    DoubleParam* _bloomSize;
    BooleanParam* _haloOnly;
};

mDeclarePluginFactory(APFXHalationFactory, {}, {});

void
APFXHalationFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Halation",
                            "APFX Halation: film halation. Highlights reflect off the film base and re-expose the emulsion from behind; "
                            "the red layer is the deepest so the halo is mostly red. Core + tail halo, size in % of width scaled by the "
                            "film gauge, R / G / B response, tint, energy conserving option and optional lens bloom. Presets are "
                            "approximate looks.");
    desc.setPluginGrouping("Filter");
}

void
APFXHalationFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("Halation");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("preset");
        p->setLabel("preset");
        p->setHint("Film stock starting points (approximate).");
        for (int i = 0; i < kNbPresets; ++i) {
            p->appendOption(kPresets[i].label);
        }
        p->setDefault(2);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "threshold", "threshold", "Luminance above which light halates.", 1.0, 0., 4., 0., 1000.);
    APFXOfx::defineDouble(desc, page, "knee", "knee", "Softness of the threshold.", 0.5, 0., 2., 0., 100.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "intensity", "intensity", "Halo strength.", 0.45, 0., 3., 0., 1000.) );
    APFXOfx::defineDouble(desc, page, "size", "size", "Tail size, in % of the image width (scaled by the gauge).", 0.5, 0., 3., 0., 100.);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("gauge");
        p->setLabel("film gauge");
        p->setHint("The same physical spread is bigger relative to a small frame.");
        p->appendOption("16mm");
        p->appendOption("35mm");
        p->appendOption("65mm");
        p->setDefault(1);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "core", "core", "Amount of the tight core (0 = tail only).", 0.5, 0., 1., 0., 1.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "coreSize", "core size", "Core size, in % of the image width.", 0.15, 0., 1., 0., 100.) );
    {
        RGBParamDescriptor* p = desc.defineRGBParam("response");
        p->setLabel("R / G / B response");
        p->setHint("How much each layer is re-exposed (red is the deepest layer).");
        p->setDefault(1., 0.3, 0.05);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("tint");
        p->setLabel("tint");
        p->setHint("Colour of the halo.");
        p->setDefault(1., 0.55, 0.35);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineBool(desc, page, "energyConserving", "energy conserving", "The halo light is taken from the highlights (keeps the overall exposure).", false);
    APFXOfx::defineDouble(desc, page, "bloom", "lens bloom", "Neutral lens bloom amount.", 0., 0., 1., 0., 100.);
    APFXOfx::defineDouble(desc, page, "bloomSize", "bloom size", "Bloom size, in % of the image width.", 2., 0., 10., 0., 100.);
    APFXOfx::divider( APFXOfx::defineBool(desc, page, "haloOnly", "halo only", "Output the halo alone (to check or comp it yourself).", false) );
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXHalationFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXHalationPlugin(h);
}
} // anon namespace

static APFXHalationFactory apfxHalationFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxHalationFactory)
