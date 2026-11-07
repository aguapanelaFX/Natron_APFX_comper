/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX ChromaticAberration
//  Lateral (transverse) CA: magnification depends on the wavelength -> radial scale per channel,
//    k1 r^2 + k2 r^4 (red out, blue in for a positive amount).
//  Spectral mode: N wavelengths 400..700nm, Cauchy dispersion n = A + B / lambda^2, each one scaled
//    by its own magnification and weighted into R / G / B (weights normalized) -> smooth rainbow fringes.
//  Longitudinal (axial) CA: each colour focuses at a different distance -> per channel blur
//    (purple fringing).
//  Creative: per channel translate / scale / rotate around the centre.
//  Edge falloff: the effect grows from the centre to the edges.

#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXLensOps.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.ChromaticAberration"

enum ModeEnum { eModeRGB = 0, eModeSpectral };

struct Preset
{
    const char* label;
    int mode;
    double lateral, k2;
    double longi[3];
    int samples;
    double edge;
};

const Preset kPresets[] = {
    { "Custom", 0, 0, 0, { 0, 0, 0 }, 0, 0 },
    { "Subtle modern lens", eModeSpectral, 0.15, 0., { 0., 0., 0. }, 9, 0.2 },
    { "Vintage zoom", eModeSpectral, 0.5, 0.2, { 0.5, 0., 1.0 }, 11, 0.3 },
    { "Cheap wide angle", eModeRGB, 0.9, 0.5, { 0., 0., 0.5 }, 7, 0.4 },
    { "Purple fringing (axial)", eModeRGB, 0.05, 0., { 0.8, 0., 2.0 }, 7, 0. },
    { "Strong prism", eModeSpectral, 2.0, 0.5, { 0., 0., 0. }, 17, 0.1 },
};
const int kNbPresets = (int)( sizeof(kPresets) / sizeof(kPresets[0]) );

// rough wavelength -> RGB sensitivity (gaussians)
inline void
wlWeights(double nm, double* w)
{
    const double r = (nm - 610.) / 45., g = (nm - 545.) / 40., b = (nm - 450.) / 30.;
    w[0] = std::exp(-0.5 * r * r);
    w[1] = std::exp(-0.5 * g * g);
    w[2] = std::exp(-0.5 * b * b);
}

class APFXChromaticAberrationPlugin
    : public ImageEffect
{
public:
    APFXChromaticAberrationPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true, false);
        _preset = fetchChoiceParam("preset");
        _mode = fetchChoiceParam("mode");
        _center = fetchDouble2DParam("center");
        _lateral = fetchDoubleParam("lateral");
        _k2 = fetchDoubleParam("k2");
        _samples = fetchIntParam("samples");
        _longi = fetchRGBParam("longitudinal");
        _edge = fetchDoubleParam("edgeFalloff");
        const char* ch[3] = { "R", "G", "B" };
        for (int c = 0; c < 3; ++c) {
            _tr[c] = fetchDouble2DParam(std::string("translate") + ch[c]);
            _sc[c] = fetchDouble2DParam(std::string("scale") + ch[c]);
            _rot[c] = fetchDoubleParam(std::string("rotate") + ch[c]);
        }
        updateVisibility();
    }

private:
    void updateVisibility()
    {
        _samples->setIsSecretAndDisabled(_mode->getValue() != eModeSpectral);
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) OVERRIDE FINAL
    {
        if ( (name == "preset") && (args.reason == eChangeUserEdit) ) {
            const int i = _preset->getValue();
            if ( (i > 0) && (i < kNbPresets) ) {
                const Preset& p = kPresets[i];
                beginEditBlock("preset");
                _mode->setValue(p.mode);
                _lateral->setValue(p.lateral);
                _k2->setValue(p.k2);
                _longi->setValue(p.longi[0], p.longi[1], p.longi[2]);
                _samples->setValue(p.samples);
                _edge->setValue(p.edge);
                endEditBlock();
            }
            updateVisibility();
        } else if (name == "mode") {
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

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(args.time) );
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

    // per channel creative 2x2 + translate (pixel space, around the centre)
    struct Xf
    {
        double a, b, c, d, tx, ty;
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
        APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        APFXOps::Plane in;
        const OfxRectI sb = APFXOfx::imageToPlane(src, in);
        const int W = in.w, H = in.h;
        const double rs = args.renderScale.x;

        // longitudinal: per channel blur (pixels at full res)
        double lg[3];
        _longi->getValueAtTime(t, lg[0], lg[1], lg[2]);
        APFXOps::Plane ch[3];
        for (int c = 0; c < 3; ++c) {
            ch[c] = APFXOps::Plane(W, H, 1);
            APFXOps::Plane& p = ch[c];
            pf([&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    for (int x = 0; x < W; ++x) {
                        p.px(x, y)[0] = in.px(x, y)[c];
                    }
                }
            }, H);
            if (lg[c] > 0.) {
                ch[c] = APFXLens::bigBlur(ch[c], (float)(lg[c] * rs), pf);
            }
        }

        const OfxRectD f = format();
        double cx, cy;
        _center->getValueAtTime(t, cx, cy);
        // centre in plane pixels
        const double pcx = cx * rs - sb.x1, pcy = cy * rs - sb.y1;
        const double fw = (f.x2 - f.x1) * rs, fh = (f.y2 - f.y1) * rs;
        const double halfDiag = std::max(1., 0.5 * std::sqrt(fw * fw + fh * fh));
        const int mode = _mode->getValueAtTime(t);
        const double lat = _lateral->getValueAtTime(t) / 100.; // % magnification difference at the corner
        const double k2 = _k2->getValueAtTime(t);
        const double edge = APFXLens::clampf( (float)_edge->getValueAtTime(t), 0.f, 0.99f );
        Xf xf[3];
        for (int c = 0; c < 3; ++c) {
            double tx, ty, sx, sy;
            _tr[c]->getValueAtTime(t, tx, ty);
            _sc[c]->getValueAtTime(t, sx, sy);
            sx = std::abs(sx) < 1e-4 ? 1e-4 : sx;
            sy = std::abs(sy) < 1e-4 ? 1e-4 : sy;
            const double r = _rot[c]->getValueAtTime(t) * M_PI / 180.;
            // inverse mapping (output -> source): R(-r) * S^-1, translation removed first
            const double co = std::cos(-r), si = std::sin(-r);
            xf[c].a = co / sx; xf[c].b = -si / sx;
            xf[c].c = si / sy; xf[c].d = co / sy;
            xf[c].tx = tx * rs; xf[c].ty = ty * rs;
        }

        // spectral samples: per sample scale factor + normalized RGB weights
        std::vector<double> sScale, sW[3];
        if (mode == eModeSpectral) {
            const int n = std::max( 3, std::min(64, _samples->getValueAtTime(t)) );
            // Cauchy (BK7-like): n = 1.5046 + 4200 / lambda^2 (nm)
            const double nR = 1.5046 + 4200. / (700. * 700.), nB = 1.5046 + 4200. / (400. * 400.), nG = 1.5046 + 4200. / (550. * 550.);
            double tot[3] = { 0, 0, 0 };
            for (int i = 0; i < n; ++i) {
                const double nm = 400. + 300. * i / (n - 1.);
                const double nn = 1.5046 + 4200. / (nm * nm);
                // red (low n) scaled out, blue in: -1 at 400nm .. +1 at 700nm-ish
                sScale.push_back( -(nn - nG) / ( (nB - nR) * 0.5 ) );
                double w[3];
                wlWeights(nm, w);
                for (int c = 0; c < 3; ++c) {
                    sW[c].push_back(w[c]);
                    tot[c] += w[c];
                }
            }
            for (int c = 0; c < 3; ++c) {
                for (size_t i = 0; i < sW[c].size(); ++i) {
                    sW[c][i] /= std::max(1e-9, tot[c]);
                }
            }
        }

        APFXOps::Plane res(W, H, 4);
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < W; ++x) {
                    const double dx = x + 0.5 - pcx, dy = y + 0.5 - pcy;
                    const double r = std::sqrt(dx * dx + dy * dy) / halfDiag;
                    // edge falloff: 0 at the centre ring, 1 at the corner
                    const double e = edge > 0. ? APFXLens::smoothstep( (float)edge, 1.f, (float)r ) : 1.;
                    const double radial = (r * r + k2 * r * r * r * r) / (1. + k2);
                    float* o = res.px(x, y);
                    for (int c = 0; c < 3; ++c) {
                        // creative transform (inverse)
                        const double ux = dx - xf[c].tx, uy = dy - xf[c].ty;
                        const double qx = xf[c].a * ux + xf[c].b * uy, qy = xf[c].c * ux + xf[c].d * uy;
                        // blend the creative displacement with the edge falloff
                        const double bx = dx + (qx - dx) * e, by = dy + (qy - dy) * e;
                        if (mode == eModeSpectral) {
                            double acc = 0.;
                            for (size_t i = 0; i < sScale.size(); ++i) {
                                const double m = 1. / ( 1. + lat * sScale[i] * radial * e );
                                acc += sW[c][i] * APFXLens::sampleBilinear(ch[c], (float)(pcx + bx * m), (float)(pcy + by * m), 0);
                            }
                            o[c] = (float)acc;
                        } else {
                            const double sgn = c == 0 ? 1. : (c == 2 ? -1. : 0.);
                            const double m = 1. / ( 1. + lat * sgn * radial * e );
                            o[c] = APFXLens::sampleBilinear(ch[c], (float)(pcx + bx * m), (float)(pcy + by * m), 0);
                        }
                    }
                    o[3] = in.px(x, y)[3];
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
    ChoiceParam* _mode;
    Double2DParam* _center;
    DoubleParam* _lateral;
    DoubleParam* _k2;
    IntParam* _samples;
    RGBParam* _longi;
    DoubleParam* _edge;
    Double2DParam* _tr[3];
    Double2DParam* _sc[3];
    DoubleParam* _rot[3];
};

mDeclarePluginFactory(APFXChromaticAberrationFactory, {}, {});

void
APFXChromaticAberrationFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "ChromaticAberration",
                            "APFX ChromaticAberration. Lateral CA (radial magnification per wavelength, k1 r^2 + k2 r^4), spectral mode "
                            "(N wavelengths, Cauchy dispersion, smooth rainbow fringes), longitudinal CA (per channel defocus, purple "
                            "fringing), creative per channel translate / scale / rotate and an edge falloff.");
    desc.setPluginGrouping("Filter");
}

void
APFXChromaticAberrationFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("ChromaticAberration");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("preset");
        p->setLabel("preset");
        for (int i = 0; i < kNbPresets; ++i) {
            p->appendOption(kPresets[i].label);
        }
        p->setDefault(1);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("mode");
        p->setLabel("mode");
        p->appendOption("RGB", "Three channels shifted (sharp colour edges).");
        p->appendOption("spectral", "N wavelengths with Cauchy dispersion (smooth rainbow fringes).");
        p->setDefault(eModeSpectral);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        Double2DParamDescriptor* p = desc.defineDouble2DParam("center");
        p->setLabel("center");
        p->setHint("Optical centre.");
        p->setDoubleType(eDoubleTypeXYAbsolute);
        p->setDefaultCoordinateSystem(eCoordinatesNormalised);
        p->setDefault(0.5, 0.5);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "lateral", "lateral", "Lateral CA: % magnification difference red / blue at the corner (negative swaps).", 0.15, -3., 3., -100., 100.);
    APFXOfx::defineDouble(desc, page, "k2", "k2 (r^4)", "Higher order: the fringes grow faster toward the corners.", 0., -1., 2., -10., 10.);
    {
        IntParamDescriptor* p = desc.defineIntParam("samples");
        p->setLabel("samples");
        p->setHint("Spectral samples (wavelengths).");
        p->setDefault(9);
        p->setRange(3, 64);
        p->setDisplayRange(3, 32);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("longitudinal");
        p->setLabel("longitudinal blur");
        p->setHint("Axial CA: per channel defocus in pixels (e.g. blue / red for purple fringing).");
        p->setDefault(0., 0., 0.);
        p->setRange(0., 0., 0., 1000., 1000., 1000.);
        p->setDisplayRange(0., 0., 0., 5., 5., 5.);
        page->addChild(*p);
    }
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "edgeFalloff", "edge falloff", "0 = everywhere; higher keeps the centre clean (radius where the effect starts).", 0.2, 0., 0.95, 0., 0.99) );
    const char* ch[3] = { "R", "G", "B" };
    const char* nm[3] = { "red", "green", "blue" };
    for (int c = 0; c < 3; ++c) {
        {
            Double2DParamDescriptor* p = desc.defineDouble2DParam(std::string("translate") + ch[c]);
            p->setLabel(std::string(nm[c]) + " translate");
            p->setDoubleType(eDoubleTypeXY);
            p->setDefault(0., 0.);
            p->setDisplayRange(-20., -20., 20., 20.);
            page->addChild(*p);
        }
        {
            Double2DParamDescriptor* p = desc.defineDouble2DParam(std::string("scale") + ch[c]);
            p->setLabel(std::string(nm[c]) + " scale");
            p->setDefault(1., 1.);
            p->setDisplayRange(0.95, 0.95, 1.05, 1.05);
            page->addChild(*p);
        }
        {
            DoubleParamDescriptor* p = desc.defineDoubleParam(std::string("rotate") + ch[c]);
            p->setLabel(std::string(nm[c]) + " rotate");
            p->setDefault(0.);
            p->setDisplayRange(-2., 2.);
            if (c == 2) {
                p->setLayoutHint(eLayoutHintDivider);
            }
            page->addChild(*p);
        }
    }
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXChromaticAberrationFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXChromaticAberrationPlugin(h);
}
} // anon namespace

static APFXChromaticAberrationFactory apfxChromaticAberrationFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxChromaticAberrationFactory)
