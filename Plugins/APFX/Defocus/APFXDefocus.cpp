/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Defocus: fast lens defocus with a flat bokeh (disc or blades), aspect ratio,
// highlight boost, linear / gamma blending and black / extend edges.
// Uses per-row prefix sums so a pixel costs O(defocus size), not O(size^2).

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxNatron.h"

#include "APFXDefocusMath.h"

#ifndef OVERRIDE
#define OVERRIDE override
#endif
#ifndef FINAL
#define FINAL final
#endif

using namespace OFX;

namespace {

#define kPluginName "Defocus"
#define kPluginGrouping "APFX"
#define kPluginDescription \
    "APFX Defocus: fast lens defocus. Every pixel spreads into a flat bokeh shape " \
    "(disc or aperture blades) instead of a soft gaussian, like Nuke's Defocus.\n" \
    "Highlights can be boosted so bright spots bloom into strong bokeh balls.\n" \
    "Works on premultiplied images. For depth based defocus use a ZDefocus."
#define kPluginIdentifier "com.aguapanelafx.APFX.Defocus"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

#define kParamDefocus "defocus"
#define kParamAspect "aspectRatio"
#define kParamScale "scale"
#define kParamQuality "quality"
#define kParamShape "shape"
#define kParamBlades "blades"
#define kParamRotation "rotation"
#define kParamRoundness "roundness"
#define kParamHighThreshold "highlightThreshold"
#define kParamHighBoost "highlightBoost"
#define kParamGammaBias "gammaBias"
#define kParamEdge "edges"
#define kParamMix "mix"
#define kParamMaskInvert "maskInvert"

// runs fn(threadIndex, nThreads) on all host threads
class Job
    : public MultiThread::Processor
{
public:
    explicit Job(const std::function<void(unsigned, unsigned)>& fn) : _fn(fn) {}
    void multiThreadFunction(unsigned int id, unsigned int n) OVERRIDE FINAL { _fn(id, n); }

private:
    std::function<void(unsigned, unsigned)> _fn;
};

inline void
range(unsigned id, unsigned n, int a, int b, int* ra, int* rb)
{
    const int len = b - a;
    *ra = a + (int)( (long long)len * id / n );
    *rb = a + (int)( (long long)len * (id + 1) / n );
}

class APFXDefocusPlugin
    : public ImageEffect
{
public:
    APFXDefocusPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _maskClip = fetchClip("Mask");
        _processR = fetchBooleanParam(kNatronOfxParamProcessR);
        _processG = fetchBooleanParam(kNatronOfxParamProcessG);
        _processB = fetchBooleanParam(kNatronOfxParamProcessB);
        _processA = fetchBooleanParam(kNatronOfxParamProcessA);
        _defocus = fetchDoubleParam(kParamDefocus);
        _aspect = fetchDoubleParam(kParamAspect);
        _scale = fetchDoubleParam(kParamScale);
        _quality = fetchChoiceParam(kParamQuality);
        _shape = fetchChoiceParam(kParamShape);
        _blades = fetchIntParam(kParamBlades);
        _rotation = fetchDoubleParam(kParamRotation);
        _roundness = fetchDoubleParam(kParamRoundness);
        _hiThreshold = fetchDoubleParam(kParamHighThreshold);
        _hiBoost = fetchDoubleParam(kParamHighBoost);
        _gammaBias = fetchDoubleParam(kParamGammaBias);
        _edge = fetchChoiceParam(kParamEdge);
        _mix = fetchDoubleParam(kParamMix);
        _maskInvert = fetchBooleanParam(kParamMaskInvert);
        updateVisibility();
    }

private:
    // kernel in canonical (full resolution) pixels
    APFXDefocus::ShapeParams shapeAt(double t, const OfxPointD& rs) const
    {
        APFXDefocus::ShapeParams sp;
        sp.radius = std::max(0., _defocus->getValueAtTime(t) * _scale->getValueAtTime(t)) * rs.y;
        sp.aspect = std::max(0.01, _aspect->getValueAtTime(t)) * (rs.x / std::max(1e-9, rs.y));
        sp.blades = (_shape->getValueAtTime(t) == 1) ? std::max(3, _blades->getValueAtTime(t)) : 0;
        sp.rotation = _rotation->getValueAtTime(t);
        sp.roundness = _roundness->getValueAtTime(t);
        sp.fast = (_quality->getValueAtTime(t) == 1);

        return sp;
    }

    void extents(double t, double* ex, double* ey) const
    {
        const double r = std::max(0., _defocus->getValueAtTime(t) * _scale->getValueAtTime(t));
        *ey = std::ceil(r) + 1.;
        *ex = std::ceil( r * std::max(0.01, _aspect->getValueAtTime(t)) ) + 2.;
    }

    void updateVisibility()
    {
        const bool blades = _shape->getValue() == 1;
        _blades->setEnabled(blades);
        _rotation->setEnabled(blades);
        _roundness->setEnabled(blades);
        _hiThreshold->setEnabled( _hiBoost->getValue() > 0. );
    }

    void changedParam(const InstanceChangedArgs& /*args*/, const std::string& name) OVERRIDE FINAL
    {
        if ( (name == kParamShape) || (name == kParamHighBoost) ) {
            updateVisibility();
        }
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime
#ifdef OFX_EXTENSIONS_NUKE
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        const double t = args.time;
        const bool any = _processR->getValueAtTime(t) || _processG->getValueAtTime(t) ||
                         _processB->getValueAtTime(t) || _processA->getValueAtTime(t);
        const bool noBlur = (_defocus->getValueAtTime(t) * _scale->getValueAtTime(t) <= 0.) && (_hiBoost->getValueAtTime(t) <= 0.);
        if ( !any || noBlur || (_mix->getValueAtTime(t) <= 0.) ) {
            identityClip = _srcClip;
            identityTime = t;

            return true;
        }

        return false;
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        if ( !_srcClip || !_srcClip->isConnected() ) {
            return false;
        }
        rod = _srcClip->getRegionOfDefinition(args.time);
        if (_edge->getValueAtTime(args.time) == 1) {
            return true;   // extend: the frame stays the same size (no black border)
        }
        double ex, ey;
        extents(args.time, &ex, &ey);
        rod.x1 -= ex; rod.x2 += ex;
        rod.y1 -= ey; rod.y2 += ey;

        return true;
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        double ex, ey;
        extents(args.time, &ex, &ey);
        OfxRectD r = args.regionOfInterest;
        r.x1 -= ex; r.x2 += ex;
        r.y1 -= ey; r.y2 += ey;
        rois.setRegionOfInterest(*_srcClip, r);
        if ( _maskClip && _maskClip->isConnected() ) {
            rois.setRegionOfInterest(*_maskClip, args.regionOfInterest);
        }
    }

    void getClipPreferences(ClipPreferencesSetter& clipPreferences) OVERRIDE FINAL
    {
        clipPreferences.setClipComponents(*_srcClip, ePixelComponentRGBA);
        clipPreferences.setClipComponents(*_dstClip, ePixelComponentRGBA);
        clipPreferences.setClipBitDepth(*_dstClip, eBitDepthFloat);
    }

    void render(const RenderArguments& args) OVERRIDE FINAL;

    Clip* _dstClip;
    Clip* _srcClip;
    Clip* _maskClip;
    BooleanParam* _processR;
    BooleanParam* _processG;
    BooleanParam* _processB;
    BooleanParam* _processA;
    DoubleParam* _defocus;
    DoubleParam* _aspect;
    DoubleParam* _scale;
    ChoiceParam* _quality;
    ChoiceParam* _shape;
    IntParam* _blades;
    DoubleParam* _rotation;
    DoubleParam* _roundness;
    DoubleParam* _hiThreshold;
    DoubleParam* _hiBoost;
    DoubleParam* _gammaBias;
    ChoiceParam* _edge;
    DoubleParam* _mix;
    BooleanParam* _maskInvert;
};

void
APFXDefocusPlugin::render(const RenderArguments& args)
{
    const double t = args.time;
    Image* dst = _dstClip->fetchImage(t);
    if (!dst) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    if ( (dst->getPixelDepth() != eBitDepthFloat) || (dst->getPixelComponents() != ePixelComponentRGBA) ) {
        delete dst;
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }
    Image* src = ( _srcClip && _srcClip->isConnected() ) ? _srcClip->fetchImage(t) : NULL;
    if ( src && ( (src->getPixelDepth() != eBitDepthFloat) || (src->getPixelComponents() != ePixelComponentRGBA) ) ) {
        delete src;
        src = NULL;
    }
    Image* mask = ( _maskClip && _maskClip->isConnected() ) ? _maskClip->fetchImage(t) : NULL;
    if ( mask && ( (mask->getPixelDepth() != eBitDepthFloat) || (mask->getPixelComponents() != ePixelComponentAlpha) ) ) {
        delete mask;
        mask = NULL;
    }

    const OfxRectI win = args.renderWindow;
    const bool proc[4] = { _processR->getValueAtTime(t), _processG->getValueAtTime(t), _processB->getValueAtTime(t), _processA->getValueAtTime(t) };
    const float mix = (float)_mix->getValueAtTime(t);
    const bool maskInvert = _maskInvert->getValueAtTime(t);
    const bool extend = (_edge->getValueAtTime(t) == 1);
    const float hiTh = (float)_hiThreshold->getValueAtTime(t);
    const float hiBoost = (float)std::max(0., _hiBoost->getValueAtTime(t));
    const float gammaBias = (float)std::max(0.05, _gammaBias->getValueAtTime(t));
    const APFXDefocus::Kernel K = APFXDefocus::buildKernel( shapeAt(t, args.renderScale) );

    // input buffer: render window grown by the kernel, clipped to the source bounds
    APFXDefocus::Buffer B;
    OfxRectI sb = { 0, 0, 0, 0 };
    if (src) {
        // only real source pixels: the image bounds can carry black padding beyond the
        // source RoD, which "extend" would otherwise repeat as a black border
        sb = src->getBounds();
        const OfxRectD rd = _srcClip->getRegionOfDefinition(t);
        const double par = _srcClip->getPixelAspectRatio();
        const double sx = args.renderScale.x / (par > 0. ? par : 1.), sy = args.renderScale.y;
        const int rx1 = (int)std::floor(rd.x1 * sx + 1e-6), rx2 = (int)std::ceil(rd.x2 * sx - 1e-6);
        const int ry1 = (int)std::floor(rd.y1 * sy + 1e-6), ry2 = (int)std::ceil(rd.y2 * sy - 1e-6);
        sb.x1 = std::max(sb.x1, rx1); sb.x2 = std::min(sb.x2, rx2);
        sb.y1 = std::max(sb.y1, ry1); sb.y2 = std::min(sb.y2, ry2);
    }
    const int bx1 = std::max(sb.x1, win.x1 - K.rx), bx2 = std::min(sb.x2, win.x2 + K.rx);
    const int by1 = std::max(sb.y1, win.y1 - K.ry), by2 = std::min(sb.y2, win.y2 + K.ry);
    if ( src && (bx2 > bx1) && (by2 > by1) ) {
        B.allocate(bx1, by1, bx2 - bx1, by2 - by1);
        // 1) copy + highlight boost + gamma bias, 2) prefix sums (parallel by rows)
        Job prep([&](unsigned id, unsigned n) {
            int r0, r1;
            range(id, n, 0, B.h, &r0, &r1);
            for (int r = r0; r < r1; ++r) {
                const float* s = (const float*)src->getPixelAddress(B.x0, B.y0 + r);
                float* v = B.row(r);
                for (int x = 0; x < B.w; ++x, s += 4, v += 4) {
                    float c[3] = { s[0], s[1], s[2] };
                    if (hiBoost > 0.f) {
                        const float L = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                        if (L > hiTh) {
                            const float k = 1.f + hiBoost * (L - hiTh) / std::max(L, 1e-6f);
                            c[0] *= k; c[1] *= k; c[2] *= k;
                        }
                    }
                    if (gammaBias != 1.f) {
                        for (int i = 0; i < 3; ++i) {
                            c[i] = (c[i] > 0.f) ? std::pow(c[i], gammaBias) : c[i];
                        }
                    }
                    v[0] = c[0]; v[1] = c[1]; v[2] = c[2]; v[3] = s[3];
                }
            }
            B.prefixRows(r0, r1);
        });
        prep.multiThread();
    }

    // 3) convolution + inverse gamma + mask / mix, parallel by output rows
    const float inv = (float)(1.0 / K.area);
    const float invGamma = 1.f / gammaBias;
    Job conv([&](unsigned id, unsigned n) {
        int y0, y1;
        range(id, n, win.y1, win.y2, &y0, &y1);
        std::vector<float> acc( (size_t)(win.x2 - win.x1) * 4 );
        for (int y = y0; y < y1; ++y) {
            if ( abort() ) {
                break;
            }
            APFXDefocus::convolveRow(B, K, extend, y, win.x1, win.x2, acc.data());
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (!d) {
                continue;
            }
            for (int x = win.x1; x < win.x2; ++x, d += 4) {
                const float* a = &acc[(size_t)(x - win.x1) * 4];
                const float* s = src ? (const float*)src->getPixelAddress(x, y) : NULL;
                float m = mix;
                if (mask) {
                    const float* mp = (const float*)mask->getPixelAddress(x, y);
                    float mv = mp ? *mp : 0.f;
                    if (maskInvert) {
                        mv = 1.f - mv;
                    }
                    m *= std::max(0.f, std::min(1.f, mv));
                }
                for (int c = 0; c < 4; ++c) {
                    float b = a[c] * inv;
                    if ( (c < 3) && (gammaBias != 1.f) && (b > 0.f) ) {
                        b = std::pow(b, invGamma);
                    }
                    const float o = s ? s[c] : 0.f;
                    d[c] = proc[c] ? (o + (b - o) * m) : o;
                }
            }
        }
    });
    conv.multiThread();

    delete mask;
    delete src;
    delete dst;
} // render

mDeclarePluginFactory(APFXDefocusPluginFactory, {}, {});

void
APFXDefocusPluginFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);
    desc.addSupportedContext(eContextFilter);
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
    desc.setChannelSelector(ePixelComponentNone); // own red / green / blue / alpha checkboxes
#endif
}

void
APFXDefocusPluginFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum /*context*/)
{
    ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(true);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(true);

    ClipDescriptor* maskClip = desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setTemporalClipAccess(false);
    maskClip->setOptional(true);
    maskClip->setSupportsTiles(true);
    maskClip->setIsMask(true);

    PageParamDescriptor* page = desc.definePageParam("Defocus");

    const char* chNames[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
    const char* chLabels[4] = { "red", "green", "blue", "alpha" };
    for (int i = 0; i < 4; ++i) {
        BooleanParamDescriptor* p = desc.defineBooleanParam(chNames[i]);
        p->setLabel(chLabels[i]);
        p->setHint("Defocus this channel.");
        p->setDefault(true);
        if (i < 3) {
            p->setLayoutHint(eLayoutHintNoNewLine, 1);
        }
        page->addChild(*p);
    }

    {
        DoubleParamDescriptor* p = desc.defineDoubleParam(kParamDefocus);
        p->setLabel("defocus");
        p->setHint("Radius of the bokeh in pixels.");
        p->setDefault(1.);
        p->setRange(0., 1000.);
        p->setDisplayRange(0., 100.);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam(kParamAspect);
        p->setLabel("aspect ratio");
        p->setHint("Width / height of the bokeh: below 1 = tall ovals, above 1 = wide ovals (anamorphic look).");
        p->setDefault(1.);
        p->setRange(0.01, 100.);
        p->setDisplayRange(0.25, 4.);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam(kParamScale);
        p->setLabel("scale");
        p->setHint("Multiplies the defocus size.");
        p->setDefault(1.);
        p->setRange(0., 100.);
        p->setDisplayRange(0., 4.);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam(kParamQuality);
        p->setLabel("quality");
        p->setHint("full: exact bokeh. fast: skips every other bokeh row, about 2x faster, nearly the same look.");
        p->appendOption("full", "", "full");
        p->appendOption("fast", "", "fast");
        p->setDefault(0);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        GroupParamDescriptor* g = desc.defineGroupParam("bokehGroup");
        g->setLabel("Bokeh");
        g->setOpen(true);
        page->addChild(*g);
        ChoiceParamDescriptor* sh = desc.defineChoiceParam(kParamShape);
        sh->setLabel("shape");
        sh->setHint("Aperture shape.");
        sh->appendOption("disc", "Round bokeh.", "disc");
        sh->appendOption("blades", "Polygon bokeh from aperture blades.", "blades");
        sh->setDefault(0);
        sh->setAnimates(false);
        sh->setParent(*g);
        page->addChild(*sh);
        IntParamDescriptor* bl = desc.defineIntParam(kParamBlades);
        bl->setLabel("blades");
        bl->setHint("Number of aperture blades.");
        bl->setDefault(6);
        bl->setRange(3, 16);
        bl->setDisplayRange(3, 12);
        bl->setParent(*g);
        page->addChild(*bl);
        DoubleParamDescriptor* ro = desc.defineDoubleParam(kParamRotation);
        ro->setLabel("rotation");
        ro->setHint("Rotation of the blades in degrees.");
        ro->setDefault(0.);
        ro->setDisplayRange(-180., 180.);
        ro->setParent(*g);
        page->addChild(*ro);
        DoubleParamDescriptor* rd = desc.defineDoubleParam(kParamRoundness);
        rd->setLabel("roundness");
        rd->setHint("0 = straight blades, 1 = round.");
        rd->setDefault(0.);
        rd->setRange(0., 1.);
        rd->setDisplayRange(0., 1.);
        rd->setParent(*g);
        page->addChild(*rd);
    }
    {
        GroupParamDescriptor* g = desc.defineGroupParam("highlightGroup");
        g->setLabel("Highlights");
        g->setOpen(true);
        page->addChild(*g);
        DoubleParamDescriptor* th = desc.defineDoubleParam(kParamHighThreshold);
        th->setLabel("threshold");
        th->setHint("Pixels brighter than this (luminance) are boosted before the defocus.");
        th->setDefault(1.);
        th->setDisplayRange(0., 4.);
        th->setParent(*g);
        page->addChild(*th);
        DoubleParamDescriptor* bo = desc.defineDoubleParam(kParamHighBoost);
        bo->setLabel("boost");
        bo->setHint("How much the highlights are boosted: 0 = off. Bright spots bloom into strong bokeh balls.");
        bo->setDefault(0.);
        bo->setRange(0., 1000.);
        bo->setDisplayRange(0., 10.);
        bo->setParent(*g);
        page->addChild(*bo);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam(kParamGammaBias);
        p->setLabel("gamma bias");
        p->setHint("1 = defocus in linear light (physically right). Other values blend with a gamma, to match older comps.");
        p->setDefault(1.);
        p->setRange(0.05, 10.);
        p->setDisplayRange(0.2, 3.);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam(kParamEdge);
        p->setLabel("edges");
        p->setHint("Outside the image: black, or repeat the edge pixels (no dark borders).");
        p->appendOption("black", "", "black");
        p->appendOption("extend", "", "extend");
        p->setDefault(0);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam(kParamMaskInvert);
        p->setLabel("Invert Mask");
        p->setHint("When checked, the effect is fully applied where the mask is 0.");
        p->setDefault(false);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam(kParamMix);
        p->setLabel("Mix");
        p->setHint("Mix between the original (0) and the defocus (1).");
        p->setDefault(1.);
        p->setRange(0., 1.);
        p->setDisplayRange(0., 1.);
        page->addChild(*p);
    }
} // describeInContext

ImageEffect*
APFXDefocusPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new APFXDefocusPlugin(handle);
}
} // anon namespace

static APFXDefocusPluginFactory apfxDefocusFactory(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(apfxDefocusFactory)
