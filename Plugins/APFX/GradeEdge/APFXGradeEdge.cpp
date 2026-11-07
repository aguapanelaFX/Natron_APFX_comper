/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX GradeEdge: port of the GradeEdge_JR gizmo (JoRM, 2022), same knobs and layout.
//  edge matte = blur( dilate( 1 - alpha, Expand Edge ) [x mask] , Blur Size )
//  output     = Grade( HueCorrect(input) ) through the edge matte  (+ BG x edge matte)
//  Preview Edge: the edge matte added in red over the input.
// The dilate is SpinVFX's Erode_Fine idea: fractional sizes, optional round filter.
// The HueCorrect curves use the openfx-misc HueCorrect maths (GPL, as SOLVFX).

#include <cfloat>
#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXLensOps.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.GradeEdge"

enum { kCurveHue = 0, kCurveSat, kCurveLum, kCurveRed, kCurveGreen, kCurveBlue, kCurveRSup, kCurveGSup, kCurveBSup, kCurveSatThrsh, kCurveNb };

// grey dilation (max) of a single-channel plane, radius r (integer), square or round
APFXOps::Plane
dilate(const APFXOps::Plane& in, int r, bool round, const APFXOps::ParallelFor& pf)
{
    if (r <= 0) {
        return in;
    }
    const int w = in.w, h = in.h;
    APFXOps::Plane out(w, h, 1);
    if (!round) {
        // separable running max
        APFXOps::Plane tmp(w, h, 1);
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const float* s = in.px(0, y);
                float* d = tmp.px(0, y);
                for (int x = 0; x < w; ++x) {
                    float m = -FLT_MAX;
                    for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k) {
                        m = std::max(m, s[k]);
                    }
                    d[x] = m;
                }
            }
        }, h);
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* d = out.px(0, y);
                for (int x = 0; x < w; ++x) {
                    float m = -FLT_MAX;
                    for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k) {
                        m = std::max(m, tmp.px(x, k)[0]);
                    }
                    d[x] = m;
                }
            }
        }, h);

        return out;
    }
    // round: for each row offset dy, a horizontal max of half width sqrt(r^2 - dy^2)
    std::vector<int> hw(2 * r + 1);
    for (int dy = -r; dy <= r; ++dy) {
        hw[dy + r] = (int)std::floor( std::sqrt( (double)r * r - (double)dy * dy ) + 0.5 );
    }
    std::vector<int> widths(hw.begin(), hw.end());
    std::sort( widths.begin(), widths.end() );
    widths.erase( std::unique( widths.begin(), widths.end() ), widths.end() );
    // horizontal maxima for every distinct half width
    std::vector<APFXOps::Plane> hmax( widths.size() );
    for (std::size_t i = 0; i < widths.size(); ++i) {
        const int rr = widths[i];
        hmax[i].resize(w, h, 1);
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const float* s = in.px(0, y);
                float* d = hmax[i].px(0, y);
                for (int x = 0; x < w; ++x) {
                    float m = -FLT_MAX;
                    for (int k = std::max(0, x - rr); k <= std::min(w - 1, x + rr); ++k) {
                        m = std::max(m, s[k]);
                    }
                    d[x] = m;
                }
            }
        }, h);
    }
    std::vector<int> idx(2 * r + 1);
    for (int j = 0; j <= 2 * r; ++j) {
        idx[j] = (int)( std::lower_bound( widths.begin(), widths.end(), hw[j] ) - widths.begin() );
    }
    pf([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* d = out.px(0, y);
            for (int x = 0; x < w; ++x) {
                float m = -FLT_MAX;
                for (int dy = -r; dy <= r; ++dy) {
                    const int yy = y + dy;
                    if ( (yy >= 0) && (yy < h) ) {
                        m = std::max(m, hmax[idx[dy + r]].px(x, yy)[0]);
                    }
                }
                d[x] = m;
            }
        }
    }, h);

    return out;
}

class APFXGradeEdgePlugin
    : public ImageEffect
{
public:
    APFXGradeEdgePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _mask = fetchClip("mask");
        _bg = fetchClip("BG");
        _round = fetchBooleanParam("round_filter");
        _expand = fetchDoubleParam("expandEdge");
        _blur = fetchDoubleParam("size");
        _view = fetchBooleanParam("viewEdge");
        _hue = fetchParametricParam("hue");
        _gain = fetchRGBAParam("white");
        _multiply = fetchRGBAParam("multiply");
        _gamma = fetchRGBAParam("gamma");
        _useBG = fetchBooleanParam("useBG");
        _useMask = fetchBooleanParam("useMask");
    }

private:
    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(args.time) );
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
        if ( _bg->isConnected() ) {
            cp.setClipComponents(*_bg, ePixelComponentRGBA);
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
        APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        APFXOps::Plane in;
        const OfxRectI sb = APFXOfx::imageToPlane(src, in);
        const int W = in.w, H = in.h;
        const double rs = args.renderScale.x;

        // edge matte: inverted alpha, dilated (fractional size: mix of floor / ceil)
        APFXOps::Plane inv(W, H, 1);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                inv.px(x, y)[0] = 1.f - in.px(x, y)[3];
            }
        }
        const double ex = std::max(0., _expand->getValueAtTime(t) * rs);
        const bool round = _round->getValueAtTime(t);
        const int r0 = (int)std::floor(ex);
        const float fr = (float)(ex - r0);
        APFXOps::Plane edge = dilate(inv, r0, round, pf);
        if (fr > 1e-3f) {
            APFXOps::Plane e1 = dilate(inv, r0 + 1, round, pf);
            for (std::size_t i = 0; i < edge.d.size(); ++i) {
                edge.d[i] += (e1.d[i] - edge.d[i]) * fr;
            }
        }
        // mask input
        Image* mask = ( _useMask->getValueAtTime(t) && _mask->isConnected() ) ? _mask->fetchImage(t) : NULL;
        if (mask) {
            const int mnc = mask->getPixelComponentCount();
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    const float* m = (const float*)mask->getPixelAddress(sb.x1 + x, sb.y1 + y);
                    edge.px(x, y)[0] *= m ? m[mnc - 1] : 0.f;
                }
            }
            delete mask;
        }
        // blur (Nuke Blur size ~ 2.5 sigma)
        const float sigma = (float)( _blur->getValueAtTime(t) * rs / 2.5 );
        if (sigma > 0.3f) {
            edge = APFXLens::bigBlur(edge, sigma, pf);
        }

        // hue correct LUTs (0..6, periodic)
        const int nb = 256;
        std::vector<float> lut[kCurveNb];
        for (int c = 0; c < kCurveNb; ++c) {
            lut[c].resize(nb + 1);
            for (int i = 0; i <= nb; ++i) {
                lut[c][i] = (float)std::max( 0., _hue->getValue(c, t, 6. * i / nb) );
            }
        }
        auto interp = [&](int c, float h) {
            const float x = APFXLens::clampf(h / 6.f, 0.f, 1.f) * nb;
            const int i = std::min(nb - 1, (int)x);
            const float a = x - i;

            return lut[c][i] * (1.f - a) + lut[c][i + 1] * a;
        };
        double gn[4], mu[4], ga[4];
        _gain->getValueAtTime(t, gn[0], gn[1], gn[2], gn[3]);
        _multiply->getValueAtTime(t, mu[0], mu[1], mu[2], mu[3]);
        _gamma->getValueAtTime(t, ga[0], ga[1], ga[2], ga[3]);

        Image* bg = ( _useBG->getValueAtTime(t) && _bg->isConnected() ) ? _bg->fetchImage(t) : NULL;
        const int bgnc = bg ? bg->getPixelComponentCount() : 0;
        const bool view = _view->getValueAtTime(t);
        const OfxRectI win = args.renderWindow;

        pf([&](int y0, int y1) {
            for (int y = win.y1 + y0; y < win.y1 + y1; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                for (int x = win.x1; x < win.x2; ++x, d += 4) {
                    const float* s = APFXOfx::planeAt(in, sb, x, y);
                    if (!s) {
                        d[0] = d[1] = d[2] = d[3] = 0.f;
                        continue;
                    }
                    const float e = APFXLens::clampf(edge.px(x - sb.x1, y - sb.y1)[0], 0.f, 1.f);
                    if (view) {
                        // Preview Edge: edge in red added over the input
                        d[0] = s[0] + e;
                        d[1] = s[1];
                        d[2] = s[2] + e * 0.001937f;
                        d[3] = s[3];
                        continue;
                    }
                    float r = s[0], g = s[1], b = s[2];
                    if (e > 0.f) {
                        // HueCorrect
                        float h0, sat, v;
                        APFXLens::rgbToHsv(r, g, b, &h0, &sat, &v);
                        float h = h0 * 6.f + 1.f;
                        if (h > 6.f) {
                            h -= 6.f;
                        }
                        const float hue = interp(kCurveHue, h), csat = interp(kCurveSat, h), lum = interp(kCurveLum, h);
                        const float red = interp(kCurveRed, h), green = interp(kCurveGreen, h), blue = interp(kCurveBlue, h);
                        const float rsup = interp(kCurveRSup, h), gsup = interp(kCurveGSup, h), bsup = interp(kCurveBSup, h);
                        const float thr = interp(kCurveSatThrsh, h);
                        float hr = r, hg = g, hb = b;
                        if (hue != 1.f) {
                            float h1 = h0 + (hue - 1.f) / 2.f;
                            h1 -= std::floor(h1);
                            APFXLens::hsvToRgb(h1, sat, v, &hr, &hg, &hb);
                        }
                        if (rsup != 1.f) { const float m = std::min(hg, hb); if (hr > m) { hr = m + rsup * (hr - m); } }
                        if (gsup != 1.f) { const float m = std::min(hr, hb); if (hg > m) { hg = m + gsup * (hg - m); } }
                        if (bsup != 1.f) { const float m = std::min(hr, hg); if (hb > m) { hb = m + bsup * (hb - m); } }
                        if (sat > thr) {
                            hr *= (thr + (sat - thr) * red * lum) / sat;
                            hg *= (thr + (sat - thr) * green * lum) / sat;
                            hb *= (thr + (sat - thr) * blue * lum) / sat;
                        } else if (thr == 0.f) {
                            hr *= red * lum; hg *= green * lum; hb *= blue * lum;
                        }
                        if (csat != 1.f) {
                            const float l = 0.2126f * hr + 0.7152f * hg + 0.0722f * hb;
                            hr = (1.f - csat) * l + csat * hr;
                            hg = (1.f - csat) * l + csat * hg;
                            hb = (1.f - csat) * l + csat * hb;
                        }
                        r += (hr - r) * e; g += (hg - g) * e; b += (hb - b) * e;
                        // Grade (gain, multiply, gamma) through the edge
                        float c3[3] = { r, g, b };
                        for (int c = 0; c < 3; ++c) {
                            float o = c3[c] * (float)(gn[c] * mu[c]);
                            if ( (ga[c] > 0.) && (ga[c] != 1.) && (o > 0.f) ) {
                                o = std::pow(o, (float)(1. / ga[c]));
                            }
                            c3[c] += (o - c3[c]) * e;
                        }
                        r = c3[0]; g = c3[1]; b = c3[2];
                        // Use Background: BG through the edge, added
                        if (bg) {
                            const float* bp = (const float*)bg->getPixelAddress(x, y);
                            if (bp && bgnc >= 3) {
                                r += bp[0] * e; g += bp[1] * e; b += bp[2] * e;
                            }
                        }
                    }
                    d[0] = r; d[1] = g; d[2] = b; d[3] = s[3];
                }
            }
        }, win.y2 - win.y1);
        delete bg;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    Clip* _mask;
    Clip* _bg;
    BooleanParam* _round;
    DoubleParam* _expand;
    DoubleParam* _blur;
    BooleanParam* _view;
    ParametricParam* _hue;
    RGBAParam* _gain;
    RGBAParam* _multiply;
    RGBAParam* _gamma;
    BooleanParam* _useBG;
    BooleanParam* _useMask;
};

mDeclarePluginFactory(APFXGradeEdgeFactory, {}, {});

void
APFXGradeEdgeFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "GradeEdge",
                            "APFX GradeEdge (port of GradeEdge_JR by JoRM): grades the inner edge of the input alpha. The edge band is "
                            "the inverted alpha grown inward by Expand Edge (Round Filter = round growth) and blurred by Blur Size; "
                            "HueCorrect and gain / multiply / gamma are applied through it. Use Background adds the BG input through "
                            "the edge; Use Mask limits the edge to the mask input; Preview Edge shows the edge in red.");
    desc.setPluginGrouping("Color");
    desc.setSupportsTiles(false);
}

static RGBAParamDescriptor*
gradeParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, double def, double dmax)
{
    RGBAParamDescriptor* p = desc.defineRGBAParam(name);
    p->setLabel(label);
    p->setHint(label);
    p->setDefault(def, def, def, def);
    p->setRange(-DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX);
    p->setDisplayRange(0, 0, 0, 0, dmax, dmax, dmax, dmax);
    page->addChild(*p);

    return p;
}

static void
textLine(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* text)
{
    StringParamDescriptor* p = desc.defineStringParam(name);
    p->setLabel(label);
    p->setStringType(eStringTypeLabel);
    p->setDefault(text);
    p->setIsPersistent(false);
    p->setEvaluateOnChange(false);
    page->addChild(*p);
}

void
APFXGradeEdgeFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false)->setLabel("Input");
    {
        ClipDescriptor* m = desc.defineClip("mask");
        m->addSupportedComponent(ePixelComponentAlpha);
        m->addSupportedComponent(ePixelComponentRGBA);
        m->setOptional(true);
        m->setSupportsTiles(false);
        m->setIsMask(true);
    }
    APFXOfx::defineRGBAClip(desc, "BG", true);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);

    PageParamDescriptor* page = desc.definePageParam("User");
    {
        // divider at the top (gizmo: empty text knob)
        StringParamDescriptor* p = desc.defineStringParam("topDivider");
        p->setLabel("");
        p->setStringType(eStringTypeLabel);
        p->setDefault("");
        p->setIsPersistent(false);
        p->setLayoutHint(eLayoutHintDivider);
        page->addChild(*p);
    }
    textLine(desc, page, "edgeText", "Edge Alpha Controls", "");
    APFXOfx::defineBool(desc, page, "round_filter", "Round Filter",
                        "Grow the edge with a round filter instead of a square one.", false);
    APFXOfx::defineDouble(desc, page, "expandEdge", "Expand Edge", "Expands edge inwards from input alpha", 0., 0., 10., 0., 1000.);
    APFXOfx::defineDouble(desc, page, "size", "Blur Size", "Blurs alpha edge", 0., 0., 100., 0., 10000.);
    APFXOfx::defineBool(desc, page, "viewEdge", "Preview Edge ", "Check this on to preview edge against input image", false);
    textLine(desc, page, "colorCtrl", "Edge Color Controls", "");
    {
        ParametricParamDescriptor* p = desc.defineParametricParam("hue");
        p->setLabel("");
        p->setHint("HueCorrect curves (applied through the edge).");
        p->setPeriodic(true);
        p->setDimension(kCurveNb);
        p->setDimensionLabel("hue", kCurveHue);
        p->setDimensionLabel("sat", kCurveSat);
        p->setDimensionLabel("lum", kCurveLum);
        p->setDimensionLabel("red", kCurveRed);
        p->setDimensionLabel("green", kCurveGreen);
        p->setDimensionLabel("blue", kCurveBlue);
        p->setDimensionLabel("r_sup", kCurveRSup);
        p->setDimensionLabel("g_sup", kCurveGSup);
        p->setDimensionLabel("b_sup", kCurveBSup);
        p->setDimensionLabel("sat_thrsh", kCurveSatThrsh);
        const OfxRGBColourD red = { 1., 0.3, 0.3 }, green = { 0.2, 0.8, 0.3 }, blue = { 0.35, 0.45, 1. }, grey = { 0.75, 0.75, 0.75 };
        const OfxRGBColourD cols[kCurveNb] = { grey, grey, grey, red, green, blue, red, green, blue, grey };
        for (int c = 0; c < kCurveNb; ++c) {
            p->setUIColour(c, cols[c]);
        }
        p->setRange(0., 6.);
        for (int c = 0; c < kCurveNb; ++c) {
            p->setDimensionRange(c == kCurveHue ? -DBL_MAX : 0., c == kCurveSatThrsh ? 1. : DBL_MAX, c);
            p->setDimensionDisplayRange(0., 2., c);
            for (int pt = 0; pt <= 5; ++pt) {
                p->addControlPoint(c, 0., pt, c == kCurveSatThrsh ? 0. : 1., false);
            }
        }
        p->setLayoutHint(eLayoutHintDivider);
        page->addChild(*p);
    }
    gradeParam(desc, page, "white", "gain", 1., 4.);
    gradeParam(desc, page, "multiply", "multiply", 1., 4.);
    gradeParam(desc, page, "gamma", "gamma", 1., 5.)->setLayoutHint(eLayoutHintDivider);
    APFXOfx::defineBool(desc, page, "useBG", "Use Background",
                        "Check this box to use background input, adds background color on top of graded edge", false)->setLayoutHint(eLayoutHintDivider);
    APFXOfx::defineBool(desc, page, "useMask", "Use Mask", "Check this box on to use mask input", false)->setLayoutHint(eLayoutHintDivider);
    textLine(desc, page, "Credit", "credit", "JoRM * Version_001 - 2022");
}

ImageEffect*
APFXGradeEdgeFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXGradeEdgePlugin(h);
}
} // anon namespace

static APFXGradeEdgeFactory apfxGradeEdgeFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxGradeEdgeFactory)
