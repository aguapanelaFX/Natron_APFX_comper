/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Vignette: physically based lens falloff + Lumetri-like artistic vignette.
//  Physical (multiplied together):
//   - natural: cos^4 law, angle from focal length and sensor size (cannot be fixed by stopping down)
//   - optical: the lens barrel clips the off-axis pupil: area of two overlapping circles, stronger
//     wide open (lower T-stop)
//   - pixel:   digital sensors add an extra angle falloff (~cos^2)
//   - mechanical: hood / matte box: hard (soft-edged) cut of the corners
//  Artistic: amount (stops), midpoint, roundness, feather.
// Applied as exposure (stops), with colour tint, highlight protection and a slight chroma fringe.

#include <cfloat>
#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXLensOps.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Vignette"

enum TypeEnum { eTypePhysical = 0, eTypeArtistic };

struct Preset
{
    const char* label;
    int type;
    double focal, sensor, tstop, strength;
    bool pixel, mech;
    double mechSize, mechSoft;
    double squeeze;
    double amount, midpoint, roundness, feather; // artistic
    double tint[3];
};

// approximate starting points (not measured lens data)
const Preset kPresets[] = {
    { "Custom", -1, 0, 0, 0, 0, false, false, 0, 0, 0, 0, 0, 0, 0, { 1, 1, 1 } },
    { "Spherical prime T2 (Cooke S4-like 32mm, S35)", eTypePhysical, 32, 24.89, 2.0, 1.0, false, false, 1.1, 0.1, 1, 0, 0, 0, 0, { 1, 1, 1 } },
    { "Fast vintage T1.3 (Super Speed-like 25mm, S35)", eTypePhysical, 25, 24.89, 1.3, 1.2, false, false, 1.1, 0.1, 1, 0, 0, 0, 0, { 1, 0.97, 0.93 } },
    { "Helios 44-2 58mm f2 (full frame)", eTypePhysical, 58, 36.0, 2.0, 1.4, false, false, 1.1, 0.1, 1, 0, 0, 0, 0, { 1, 0.96, 0.9 } },
    { "Canon K35-like 24mm T1.5 (S35)", eTypePhysical, 24, 24.89, 1.5, 1.1, false, false, 1.1, 0.1, 1, 0, 0, 0, 0, { 1, 0.98, 0.95 } },
    { "Anamorphic 2x oval (Panavision C-like 40mm T2.3)", eTypePhysical, 40, 24.89, 2.3, 1.2, false, false, 1.1, 0.1, 2.0, 0, 0, 0, 0, { 1, 1, 1 } },
    { "Super 16 zoom 10mm (dark corners)", eTypePhysical, 10, 12.52, 2.0, 1.0, false, true, 1.02, 0.08, 1, 0, 0, 0, 0, { 1, 1, 1 } },
    { "Smartphone wide (26mm eq.)", eTypePhysical, 4.2, 5.6, 1.8, 1.0, true, false, 1.1, 0.1, 1, 0, 0, 0, 0, { 1, 1, 1 } },
    { "Action cam fisheye", eTypePhysical, 2.7, 6.17, 2.8, 1.4, true, false, 1.1, 0.1, 1, 0, 0, 0, 0, { 1, 1, 1 } },
    { "Old film (warm)", eTypeArtistic, 0, 0, 0, 0, false, false, 1.1, 0.1, 1, -1.2, 0.45, 0.2, 0.7, { 1, 0.85, 0.65 } },
};
const int kNbPresets = (int)( sizeof(kPresets) / sizeof(kPresets[0]) );

class APFXVignettePlugin
    : public ImageEffect
{
public:
    APFXVignettePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true, false);
        _preset = fetchChoiceParam("preset");
        _type = fetchChoiceParam("type");
        _strength = fetchDoubleParam("strength");
        _focal = fetchDoubleParam("focalLength");
        _sensor = fetchDoubleParam("sensorWidth");
        _tstop = fetchDoubleParam("tstop");
        _pixel = fetchBooleanParam("pixel");
        _mech = fetchBooleanParam("mechanical");
        _mechSize = fetchDoubleParam("mechSize");
        _mechSoft = fetchDoubleParam("mechSoft");
        _amount = fetchDoubleParam("amount");
        _midpoint = fetchDoubleParam("midpoint");
        _roundness = fetchDoubleParam("roundness");
        _feather = fetchDoubleParam("feather");
        _center = fetchDouble2DParam("center");
        _squeeze = fetchDoubleParam("squeeze");
        _rotate = fetchDoubleParam("rotate");
        _tint = fetchRGBParam("tint");
        _preserve = fetchDoubleParam("preserveHighlights");
        _fringe = fetchDoubleParam("fringe");
        updateVisibility();
    }

private:
    void updateVisibility()
    {
        const bool phys = _type->getValue() == eTypePhysical;
        _focal->setIsSecretAndDisabled(!phys);
        _sensor->setIsSecretAndDisabled(!phys);
        _tstop->setIsSecretAndDisabled(!phys);
        _pixel->setIsSecretAndDisabled(!phys);
        _mech->setIsSecretAndDisabled(!phys);
        _mechSize->setIsSecretAndDisabled(!phys);
        _mechSoft->setIsSecretAndDisabled(!phys);
        _strength->setIsSecretAndDisabled(!phys);
        _amount->setIsSecretAndDisabled(phys);
        _midpoint->setIsSecretAndDisabled(phys);
        _roundness->setIsSecretAndDisabled(phys);
        _feather->setIsSecretAndDisabled(phys);
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) OVERRIDE FINAL
    {
        if ( (name == "preset") && (args.reason == eChangeUserEdit) ) {
            const int i = _preset->getValue();
            if ( (i > 0) && (i < kNbPresets) ) {
                const Preset& p = kPresets[i];
                beginEditBlock("preset");
                _type->setValue(p.type);
                if (p.type == eTypePhysical) {
                    _focal->setValue(p.focal);
                    _sensor->setValue(p.sensor);
                    _tstop->setValue(p.tstop);
                    _strength->setValue(p.strength);
                    _pixel->setValue(p.pixel);
                    _mech->setValue(p.mech);
                    _mechSize->setValue(p.mechSize);
                    _mechSoft->setValue(p.mechSoft);
                } else {
                    _amount->setValue(p.amount);
                    _midpoint->setValue(p.midpoint);
                    _roundness->setValue(p.roundness);
                    _feather->setValue(p.feather);
                }
                _squeeze->setValue(p.squeeze);
                _tint->setValue(p.tint[0], p.tint[1], p.tint[2]);
                endEditBlock();
            }
            updateVisibility();
        } else if (name == "type") {
            updateVisibility();
        }
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        rod = _src->getRegionOfDefinition(args.time);

        return true;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
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

    // light kept (0..1) at a normalized position (x, y relative to the centre, in units of
    // the format half diagonal)
    struct Model
    {
        int type;
        double strength, halfDiagMM, focal, opt, mechSize, mechSoft, aspect;
        bool pixel, mech;
        double amount, midpoint, roundness, feather;

        double physical(double r, double ux, double uy) const
        {
            // natural: cos^4 of the field angle
            const double rmm = r * halfDiagMM;
            const double c = focal / std::sqrt(focal * focal + rmm * rmm);
            double f = c * c * c * c;
            if (pixel) {
                f *= c * c;
            }
            // optical: overlap of the entrance pupil (radius 1) with the rear aperture shifted
            // proportionally to the field position (equal circles)
            const double d = std::min(2., r * opt);
            const double overlap = ( 2. * std::acos(d / 2.) - (d / 2.) * std::sqrt( std::max(0., 4. - d * d) ) ) / M_PI;
            f *= overlap;
            if (mech) {
                // hood: superellipse following the format shape, soft edge
                const double ax = std::abs(ux) / aspect, ay = std::abs(uy);
                const double e = std::pow( std::pow(ax, 6.) + std::pow(ay, 6.), 1. / 6. ) * std::sqrt(1. + aspect * aspect) / std::sqrt(1. + 1. / (aspect * aspect)) ;
                f *= 1. - APFXLens::smoothstep( (float)(mechSize - mechSoft), (float)mechSize, (float)(e * 0.72) );
            }
            f = std::max(1e-6, f);

            return std::pow(f, strength);
        }

        // artistic: returns stops (<0 darkens)
        double artistic(double ux, double uy) const
        {
            // roundness -1 = format shaped (square-ish), 1 = circle
            const double p = 2. + (1. - roundness) * 3.;
            const double ax = std::abs(ux), ay = std::abs(uy);
            const double e = std::pow( std::pow(ax, p) + std::pow(ay, p), 1. / p );
            const double inner = std::max(0., midpoint * (1. - feather));
            const double outer = std::max(inner + 1e-3, midpoint + feather * (1.2 - midpoint));

            return amount * APFXLens::smoothstep( (float)inner, (float)outer, (float)e );
        }
    };

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
        const OfxRectD f = format();
        const double fw = f.x2 - f.x1, fh = f.y2 - f.y1;
        double cx, cy;
        _center->getValueAtTime(t, cx, cy);
        Model m;
        m.type = _type->getValueAtTime(t);
        m.strength = _strength->getValueAtTime(t);
        const double sensorW = std::max(0.1, _sensor->getValueAtTime(t));
        const double sensorH = sensorW * fh / std::max(1., fw);
        m.halfDiagMM = 0.5 * std::sqrt(sensorW * sensorW + sensorH * sensorH);
        m.focal = std::max(0.1, _focal->getValueAtTime(t));
        m.opt = 1.2 * 1.4 / std::max(0.5, _tstop->getValueAtTime(t)); // T1.4 -> shift 1.2 at the corner
        m.pixel = _pixel->getValueAtTime(t);
        m.mech = _mech->getValueAtTime(t);
        m.mechSize = _mechSize->getValueAtTime(t);
        m.mechSoft = std::max(1e-3, _mechSoft->getValueAtTime(t));
        m.aspect = fw / std::max(1., fh);
        m.amount = _amount->getValueAtTime(t);
        m.midpoint = _midpoint->getValueAtTime(t);
        m.roundness = _roundness->getValueAtTime(t);
        m.feather = _feather->getValueAtTime(t);
        const double squeeze = std::max(0.1, _squeeze->getValueAtTime(t));
        const double rot = _rotate->getValueAtTime(t) * M_PI / 180.;
        const double cr = std::cos(rot), sr = std::sin(rot);
        double tint[3];
        _tint->getValueAtTime(t, tint[0], tint[1], tint[2]);
        const float preserve = (float)_preserve->getValueAtTime(t);
        const float fringe = (float)_fringe->getValueAtTime(t);
        const double halfDiag = 0.5 * std::sqrt(fw * fw + fh * fh);
        const double rsx = args.renderScale.x, rsy = args.renderScale.y;

        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        const OfxRectI win = args.renderWindow;
        APFXOfx::hostParallelFor()([&](int y0, int y1) {
            for (int y = win.y1 + y0; y < win.y1 + y1; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                for (int x = win.x1; x < win.x2; ++x, d += 4) {
                    const float* s = (const float*)src->getPixelAddress(x, y);
                    if (!s) {
                        d[0] = d[1] = d[2] = d[3] = 0.f;
                        continue;
                    }
                    // position relative to the centre (canonical), rotated, squeezed
                    const double px = (x + 0.5) / rsx - cx, py = (y + 0.5) / rsy - cy;
                    double qx = cr * px + sr * py, qy = -sr * px + cr * py;
                    qx /= squeeze;
                    float res[4];
                    for (int c = 0; c < 3; ++c) {
                        // chroma fringe: red falls off a bit later, blue a bit earlier
                        const double k = 1. + (c == 0 ? -1. : (c == 2 ? 1. : 0.)) * fringe * 0.03;
                        double ux, uy, r;
                        double stops;
                        if (m.type == eTypePhysical) {
                            ux = qx * k / halfDiag; uy = qy * k / halfDiag;
                            r = std::sqrt(ux * ux + uy * uy);
                            stops = std::log2( m.physical(r, ux * halfDiag / (fw * 0.5) , uy * halfDiag / (fh * 0.5)) );
                        } else {
                            ux = qx * k / (fw * 0.5); uy = qy * k / (fh * 0.5);
                            stops = m.artistic(ux, uy);
                        }
                        float g = (float)std::pow(2., stops);
                        // tint: the darkened part takes the tint colour
                        const float dark = APFXLens::clampf(1.f - g, 0.f, 1.f);
                        g *= 1.f + ( (float)tint[c] - 1.f ) * dark;
                        // protect highlights
                        if ( (preserve > 0.f) && (g < 1.f) ) {
                            const float l = APFXLens::luma709(s);
                            g += (1.f - g) * preserve * APFXLens::smoothstep(1.f, 4.f, l);
                        }
                        res[c] = s[c] * g;
                    }
                    res[3] = s[3];
                    float mv = o.mix;
                    if (mask) {
                        const float* mp = (const float*)mask->getPixelAddress(x, y);
                        float v = mp ? *mp : 0.f;
                        if (o.maskInvert) {
                            v = 1.f - v;
                        }
                        mv *= APFXLens::clampf(v, 0.f, 1.f);
                    }
                    for (int c = 0; c < 4; ++c) {
                        d[c] = s[c] + (res[c] - s[c]) * mv;
                    }
                }
            }
        }, win.y2 - win.y1);
        delete mask;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    APFXOfx::StdParams _std;
    ChoiceParam* _preset;
    ChoiceParam* _type;
    DoubleParam* _strength;
    DoubleParam* _focal;
    DoubleParam* _sensor;
    DoubleParam* _tstop;
    BooleanParam* _pixel;
    BooleanParam* _mech;
    DoubleParam* _mechSize;
    DoubleParam* _mechSoft;
    DoubleParam* _amount;
    DoubleParam* _midpoint;
    DoubleParam* _roundness;
    DoubleParam* _feather;
    Double2DParam* _center;
    DoubleParam* _squeeze;
    DoubleParam* _rotate;
    RGBParam* _tint;
    DoubleParam* _preserve;
    DoubleParam* _fringe;
};

mDeclarePluginFactory(APFXVignetteFactory, {}, {});

void
APFXVignetteFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Vignette",
                            "APFX Vignette. Physical: natural falloff (cos^4 law from focal length + sensor size), optical vignetting "
                            "(the lens barrel clips the off-axis pupil, stronger wide open), pixel falloff (digital sensors) and "
                            "mechanical (hood / matte box corners). Artistic: Lumetri-like amount, midpoint, roundness, feather. "
                            "Applied as exposure, with colour tint, highlight protection and chroma fringe. Presets are approximate "
                            "starting points.");
    desc.setPluginGrouping("Filter");
    desc.setSupportsTiles(true);
}

void
APFXVignetteFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false)->setSupportsTiles(true);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false)->setSupportsTiles(true);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("Vignette");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("preset");
        p->setLabel("preset");
        p->setHint("Lens / camera starting points (approximate).");
        for (int i = 0; i < kNbPresets; ++i) {
            p->appendOption(kPresets[i].label);
        }
        p->setDefault(1);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("type");
        p->setLabel("type");
        p->appendOption("physical", "Natural cos^4 + optical + pixel + mechanical vignetting.");
        p->appendOption("artistic", "Lumetri-like amount / midpoint / roundness / feather.");
        p->setDefault(eTypePhysical);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "strength", "strength", "Scales the physical falloff (1 = physical).", 1.0, 0., 3., 0., 100.);
    APFXOfx::defineDouble(desc, page, "focalLength", "focal length (mm)", "Lens focal length.", 32., 8., 135., 0.1, 2000.);
    APFXOfx::defineDouble(desc, page, "sensorWidth", "sensor width (mm)", "Sensor / film gate width (S35 24.89, FF 36, S16 12.52).", 24.89, 5., 54., 0.1, 100.);
    APFXOfx::defineDouble(desc, page, "tstop", "T-stop", "Aperture: wide open (low) clips the off-axis pupil more (optical vignetting).", 2.0, 1., 16., 0.5, 64.);
    APFXOfx::defineBool(desc, page, "pixel", "pixel vignetting", "Extra falloff of digital sensors (light hitting the pixels at an angle).", false);
    APFXOfx::defineBool(desc, page, "mechanical", "mechanical", "Hood / matte box cutting the corners.", false);
    APFXOfx::defineDouble(desc, page, "mechSize", "hood size", "Where the hood starts cutting (1 = the corners).", 1.1, 0.5, 1.5, 0., 10.);
    APFXOfx::defineDouble(desc, page, "mechSoft", "hood softness", "Softness of the hood edge.", 0.1, 0.01, 0.5, 0.001, 10.);
    APFXOfx::defineDouble(desc, page, "amount", "amount", "Artistic vignette strength in stops (negative darkens, positive lightens).", -1., -4., 4., -100., 100.);
    APFXOfx::defineDouble(desc, page, "midpoint", "midpoint", "Where the vignette starts (0 centre, 1 edges).", 0.5, 0., 1., 0., 2.);
    APFXOfx::defineDouble(desc, page, "roundness", "roundness", "-1 follows the frame shape, 1 = round.", 0., -1., 1., -1., 1.);
    APFXOfx::defineDouble(desc, page, "feather", "feather", "Softness of the vignette.", 0.5, 0., 1., 0., 1.);
    {
        Double2DParamDescriptor* p = desc.defineDouble2DParam("center");
        p->setLabel("center");
        p->setHint("Optical centre.");
        p->setDoubleType(eDoubleTypeXYAbsolute);
        p->setDefaultCoordinateSystem(eCoordinatesNormalised);
        p->setDefault(0.5, 0.5);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "squeeze", "squeeze", "Anamorphic oval: > 1 stretches the vignette horizontally.", 1., 0.5, 2.5, 0.1, 10.);
    APFXOfx::defineDouble(desc, page, "rotate", "rotate", "Rotation of the vignette shape (degrees).", 0., -180., 180.);
    {
        RGBParamDescriptor* p = desc.defineRGBParam("tint");
        p->setLabel("tint");
        p->setHint("Colour of the darkened area.");
        p->setDefault(1., 1., 1.);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "preserveHighlights", "preserve highlights", "Keep bright highlights (> 1) from being darkened.", 0., 0., 1., 0., 1.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "fringe", "chroma fringe", "Slight colour fringe in the falloff (red later, blue earlier).", 0., 0., 1., 0., 10.) );
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXVignetteFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXVignettePlugin(h);
}
} // anon namespace

static APFXVignetteFactory apfxVignetteFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxVignetteFactory)
