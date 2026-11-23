/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX VectorBlend: motion-aware frame blending (deflicker, degrain, denoise) and motion
// smoothing (de-jitter, wobble removal) from optical flow. See APFXVectorBlend.h.

#include <cmath>
#include <vector>
#include <algorithm>

#include "APFXFlowOfx.h"
#include "APFXVectorBlend.h"

using namespace OFX;
using APFXOps::Plane;
using namespace APFXFlow;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.VectorBlend"

enum Vectors { eVecDirect = 0, eVecAccumulated };
enum MatteUse { eMatteNone = 0, eMatteInside, eMatteOutside };
enum Output { eOutResult = 0, eOutCorrection, eOutVectors };

class APFXVectorBlendPlugin
    : public ImageEffect
{
public:
    APFXVectorBlendPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _matte = fetchClip("Matte");
        _blendOn = fetchBooleanParam("enableBlending");
        _blendR = fetchIntParam("blendRadius");
        _blendOp = fetchChoiceParam("blendOperation");
        _smoothOn = fetchBooleanParam("enableSmoothing");
        _smoothR = fetchIntParam("smoothRadius");
        _smoothOp = fetchChoiceParam("smoothOperation");
        _sigma = fetchDoubleParam("sigma");
        _vectors = fetchChoiceParam("vectors");
        _detail = fetchDoubleParam("vectorDetail");
        _smooth = fetchDoubleParam("smoothness");
        _vblur = fetchDoubleParam("blurVectors");
        _matteUse = fetchChoiceParam("matteUse");
        _filter = fetchChoiceParam("filter");
        _log = fetchBooleanParam("processLog");
        _unpremult = fetchBooleanParam("processUnpremult");
        _keepAlpha = fetchBooleanParam("keepAlpha");
        _clampAlpha = fetchBooleanParam("clampAlpha");
        _output = fetchChoiceParam("output");
        _sublabel = paramExists(kNatronOfxParamStringSublabelName) ? fetchStringParam(kNatronOfxParamStringSublabelName) : NULL;
        _std.fetch(*this, true, false);
        refresh();
    }

private:
    int radius(double t) const
    {
        const int b = _blendOn->getValueAtTime(t) ? _blendR->getValueAtTime(t) : 0;
        const int s = _smoothOn->getValueAtTime(t) ? _smoothR->getValueAtTime(t) : 0;

        return std::max( 0, std::min( 5, std::max(b, s) ) );
    }

    void refresh()
    {
        const bool b = _blendOn->getValue(), s = _smoothOn->getValue();
        _blendR->setEnabled(b);
        _blendOp->setEnabled(b);
        _smoothR->setEnabled(s);
        _smoothOp->setEnabled(s);
        const int op = _smoothOp->getValue();
        _sigma->setEnabled( s && ( (op == APFXVB::eSmoothGaussian) || (op == APFXVB::eSmoothLaplacian) ) );
        if (_sublabel) {
            const int r = radius(0.);
            char buf[64];
            snprintf(buf, sizeof(buf), "%d frames", 2 * r + 1);
            std::string cur;
            _sublabel->getValue(cur);
            if (cur != buf) {
                _sublabel->setValue(buf);
            }
        }
    }

    void changedParam(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == "enableBlending") || (n == "enableSmoothing") || (n == "blendRadius") || (n == "smoothRadius") || (n == "smoothOperation") ) {
            refresh();
        }
    }

    void frameRange(double t, double* lo, double* hi) const
    {
        const int R = radius(t);
        OfxRangeD c = _src->getFrameRange();
        *lo = std::max(c.min, t - R);
        *hi = std::min(c.max, t + R);
    }

    void getFramesNeeded(const FramesNeededArguments& args, FramesNeededSetter& frames) OVERRIDE FINAL
    {
        const double t = std::floor(args.time + 0.5);
        double lo, hi;
        frameRange(t, &lo, &hi);
        OfxRangeD r = { lo, hi };
        frames.setFramesNeeded(*_src, r);
        if ( _matte->isConnected() ) {
            frames.setFramesNeeded(*_matte, r);
        }
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        const OfxRectD rod = _src->getRegionOfDefinition(args.time);
        rois.setRegionOfInterest(*_src, rod);
        if ( _matte->isConnected() ) {
            rois.setRegionOfInterest(*_matte, rod);
        }
        if ( _std.maskClip && _std.maskClip->isConnected() ) {
            rois.setRegionOfInterest(*_std.maskClip, args.regionOfInterest);
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

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime
#ifdef OFX_EXTENSIONS_NATRON
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        if ( (radius(args.time) == 0) || (_std.mix && _std.mix->getValueAtTime(args.time) <= 0.) ) {
            identityClip = _src;
            identityTime = args.time;

            return true;
        }

        return false;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setOutputPremultiplication( _src->getPreMultiplication() );
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        std::unique_ptr<Image> dst( _dst->fetchImage(args.time) );
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        const double t = std::floor(args.time + 0.5);
        const OfxRectI win = args.renderWindow;
        const APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        const int R = radius(t);
        double lo, hi;
        frameRange(t, &lo, &hi);

        APFXFlowOfx::FrameStore fs, ms;
        fs.clip = _src;
        fs.win = win;
        ms.clip = _matte;
        ms.win = win;
        const int matteUse = _matte->isConnected() ? _matteUse->getValueAtTime(t) : eMatteNone;
        Params prm;
        prm.detail = (float)_detail->getValueAtTime(t);
        prm.smoothness = (float)_smooth->getValueAtTime(t);
        auto frameData = [&](double f) -> FrameData& {
            FrameData& d = fs.frame(f);
            if ( (matteUse != eMatteNone) && d.weights.d.empty() ) {
                const Plane& m = ms.plane(f);
                Plane m1(m.w, m.h, 1);
                for (size_t i = 0; i < (size_t)m.w * m.h; ++i) {
                    m1.d[i] = m.d[i * 4 + 3];
                }
                APFXFlowOfx::weightsOf(m1, matteUse == eMatteInside, d);
            }

            return d;
        };
        const int layer = (matteUse == eMatteNone) ? 0 : (matteUse == eMatteInside ? 2 : 1);

        // vectors from this frame to its neighbours
        std::vector<Field> fields(2 * R + 1);
        std::vector<const Field*> V(2 * R + 1, (const Field*)NULL);
        std::vector<const Plane*> frames(2 * R + 1, (const Plane*)NULL);
        frames[R] = &fs.plane(t);
        const bool accumulated = _vectors->getValueAtTime(t) == eVecAccumulated;
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int k = 1; k <= R && !abort(); ++k) {
                const double f = t + dir * k;
                if ( (f < lo) || (f > hi) ) {
                    break;
                }
                const int idx = R + dir * k;
                if (accumulated && (k > 1) && V[idx - dir]) {
                    const double fp = f - dir;
                    PairPtr step = pairFlows(fp, f, frameData(fp), frameData(f), prm, layer, pf);
                    fields[idx] = chain(*V[idx - dir], step->f01, pf);
                } else {
                    PairPtr p = pairFlows(t, f, frameData(t), frameData(f), prm, layer, pf);
                    fields[idx] = p->f01;
                }
                V[idx] = &fields[idx];
                frames[idx] = &fs.plane(f);
            }
        }
        const double vb = _vblur->getValueAtTime(t);
        if (vb > 0.) {
            for (int i = 0; i < 2 * R + 1; ++i) {
                if (V[i]) {
                    blurField(fields[i], (float)(vb / fields[i].scale), pf);
                }
            }
        }

        APFXVB::Settings st;
        st.blendRadius = _blendOn->getValueAtTime(t) ? _blendR->getValueAtTime(t) : 0;
        st.blendOp = _blendOp->getValueAtTime(t);
        st.smoothRadius = _smoothOn->getValueAtTime(t) ? _smoothR->getValueAtTime(t) : 0;
        st.smoothOp = _smoothOp->getValueAtTime(t);
        st.sigma = (float)_sigma->getValueAtTime(t);
        st.filter = _filter->getValueAtTime(t);
        st.logSpace = _log->getValueAtTime(t);
        st.unpremult = _unpremult->getValueAtTime(t);
        st.keepAlpha = _keepAlpha->getValueAtTime(t);
        st.clampAlpha = _clampAlpha->getValueAtTime(t);

        Plane res, corr;
        const int output = _output->getValueAtTime(t);
        if (R == 0) {
            res = *frames[R];
        } else {
            APFXVB::vectorBlend(frames, V, R, st, res, output == eOutCorrection ? &corr : NULL, pf);
        }
        if (output == eOutCorrection) {
            res = corr;
        } else if (output == eOutVectors) {
            res.resize(frames[R]->w, frames[R]->h, 4);
            const Field* fb = (R > 0) ? V[R - 1] : NULL;
            const Field* ff = (R > 0) ? V[R + 1] : NULL;
            pf([&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    for (int x = 0; x < res.w; ++x) {
                        float* d = res.px(x, y);
                        d[0] = d[1] = d[2] = d[3] = 0.f;
                        if (ff) { ff->atImage( (float)x, (float)y, &d[0], &d[1] ); }
                        if (fb) { fb->atImage( (float)x, (float)y, &d[2], &d[3] ); }
                    }
                }
            }, res.h);
        }
        APFXOfx::OutputArgs o;
        std::unique_ptr<Image> mask( _std.get(args.time, o) );
        if (output != eOutResult) {
            o.mix = 1.f;
            o.mask = NULL;
        }
        if ( res.d.empty() ) {
            res.resize(win.x2 - win.x1, win.y2 - win.y1, 4);
        }
        // 4-channel output only (RGB / alpha clips are converted by the host)
        if (dst->getPixelComponentCount() == 4) {
            APFXOfx::writeOutput(*this, dst.get(), win, res, win, frames[R], win, o);
        } else {
            const int nc = dst->getPixelComponentCount();
            for (int y = win.y1; y < win.y2; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                const float* s = res.px(0, y - win.y1);
                for (int x = win.x1; x < win.x2; ++x, d += nc, s += 4) {
                    for (int c = 0; c < nc; ++c) {
                        d[c] = (nc == 1) ? s[3] : s[c];
                    }
                }
            }
        }
    }

    Clip* _dst;
    Clip* _src;
    Clip* _matte;
    BooleanParam* _blendOn;
    IntParam* _blendR;
    ChoiceParam* _blendOp;
    BooleanParam* _smoothOn;
    IntParam* _smoothR;
    ChoiceParam* _smoothOp;
    DoubleParam* _sigma;
    ChoiceParam* _vectors;
    DoubleParam* _detail;
    DoubleParam* _smooth;
    DoubleParam* _vblur;
    ChoiceParam* _matteUse;
    ChoiceParam* _filter;
    BooleanParam* _log;
    BooleanParam* _unpremult;
    BooleanParam* _keepAlpha;
    BooleanParam* _clampAlpha;
    ChoiceParam* _output;
    StringParam* _sublabel;
    APFXOfx::StdParams _std;
};

mDeclarePluginFactory(APFXVectorBlendFactory, {}, {});

void
APFXVectorBlendFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "VectorBlend",
                            "APFX VectorBlend: blends neighbouring frames along their motion. Frame blending (average, "
                            "median, min, max, plus) removes flicker, grain and noise without smearing moving things. "
                            "Frame smoothing moves every pixel onto a smooth path through time (average, laplacian, "
                            "gaussian, or quadratic that keeps real acceleration): removes jitter and wobble. "
                            "vectors: direct (each neighbour compared with this frame, small motion) or accumulated "
                            "(chained frame to frame, large motion, fastest: shared between frames). A matte limits where "
                            "motion is measured. Vectors are cached and shared with the other flow nodes.");
    desc.setPluginGrouping("Time");
    desc.setTemporalClipAccess(true);
}

static void
radiusParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, int def, const char* hint)
{
    IntParamDescriptor* p = desc.defineIntParam(name);
    p->setLabel("temporal radius");
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(1, 5);
    p->setDisplayRange(1, 5);
    p->setAnimates(false);
    p->setLayoutHint(eLayoutHintNoNewLine, 16);
    page->addChild(*p);
}

void
APFXVectorBlendFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setTemporalClipAccess(true);
        c->setSupportsTiles(false);
        c->setIsMask(false);
    }
    {
        ClipDescriptor* c = desc.defineClip("Matte");
        c->setLabel("matte");
        c->addSupportedComponent(ePixelComponentAlpha);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->setTemporalClipAccess(true);
        c->setSupportsTiles(false);
        c->setOptional(true);
        c->setIsMask(false);
    }
    APFXOfx::defineMaskClip(desc);
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectOutputClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setSupportsTiles(false);
    }
    PageParamDescriptor* page = desc.definePageParam("VectorBlend");
#ifdef OFX_EXTENSIONS_NATRON
    {
        StringParamDescriptor* p = desc.defineStringParam(kNatronOfxParamStringSublabelName);
        p->setIsSecretAndDisabled(true);
        p->setEvaluateOnChange(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
#endif
    // frame blending
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "enableBlending", "frame blending",
                                                         "Blend the neighbouring frames, pushed onto this frame along their motion.", true);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
    }
    radiusParam(desc, page, "blendRadius", 2, "Frames on each side: 2 blends 5 frames (-2 .. +2).");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("blendOperation");
        p->setLabel("operation");
        p->setHint("average: flicker, grain, noise. median: also removes dust, sparkles and ghosts. min / max: darkest / "
                   "brightest of the frames. plus: sum.");
        p->appendOption("average");
        p->appendOption("median");
        p->appendOption("min");
        p->appendOption("max");
        p->appendOption("plus");
        p->setDefault(APFXVB::eBlendAverage);
        p->setAnimates(false);
        page->addChild(*p);
    }
    // frame smoothing
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "enableSmoothing", "frame smoothing",
                                                         "Move every pixel onto a smooth path through time (removes jitter / wobble).", false);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
    }
    radiusParam(desc, page, "smoothRadius", 3, "Frames on each side used to fit the smooth path.");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("smoothOperation");
        p->setLabel("operation");
        p->setHint("average: spreads spikes over the neighbouring frames. laplacian: removes one-frame spikes. gaussian: "
                   "softly spreads spikes (sigma). quadratic: removes high-frequency jitter but follows real acceleration "
                   "(needs radius 2 or more).");
        p->appendOption("average");
        p->appendOption("laplacian");
        p->appendOption("gaussian");
        p->appendOption("quadratic");
        p->setDefault(APFXVB::eSmoothGaussian);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "sigma", "sigma", "Width in frames of the gaussian / laplacian weights.",
                                            2.5, 0.25, 4., 0.25, 10.) );
    // vectors
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("vectors");
        p->setLabel("vectors");
        p->setHint("direct: each neighbour compared with this frame (best for little motion). accumulated: frame to frame "
                   "vectors chained (large motion; fastest, the vectors are shared between frames).");
        p->appendOption("direct");
        p->appendOption("accumulated");
        p->setDefault(eVecAccumulated);
        p->setAnimates(false);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "vectorDetail", "vector detail",
                          "Resolution of the motion vectors: 1 = full (finest, slowest), 0.5 = half (default), 0.25 = quarter (fast).",
                          0.5, 0.1, 1., 0.05, 1.);
    APFXOfx::defineDouble(desc, page, "smoothness", "smoothness",
                          "0 = vectors follow every detail, 1 = smooth vectors (fewer tears on flat or noisy areas).", 0.5, 0., 1., 0., 1.);
    APFXOfx::defineDouble(desc, page, "blurVectors", "blur vectors", "Blur the vectors (pixels): calmer motion, softer edges.",
                          0., 0., 20., 0., 200.);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("matteUse");
        p->setLabel("matte");
        p->setHint("With a matte connected: measure the motion only inside the matte (the object) or only outside it (the "
                   "background); the rest follows its surroundings.");
        p->appendOption("none");
        p->appendOption("inside");
        p->appendOption("outside");
        p->setDefault(eMatteNone);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    // processing
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("filter");
        p->setLabel("filter");
        p->setHint("bicubic: sharp, never overshoots. bilinear: softer, faster.");
        p->appendOption("bilinear");
        p->appendOption("bicubic");
        p->setDefault(1);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    APFXOfx::defineBool(desc, page, "processLog", "process in log",
                        "Blend the colours in log space: flicker (brightness changes) averages correctly, highlights do not "
                        "dominate.", true);
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "processUnpremult", "process rgb unpremultiplied",
                                                         "For premultiplied input: colours are unpremultiplied and extended past "
                                                         "the edges before warping (no dark fringes).", false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
    }
    APFXOfx::defineBool(desc, page, "keepAlpha", "keep original alpha", "Output alpha = the alpha of this frame.", false)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::divider( APFXOfx::defineBool(desc, page, "clampAlpha", "clamp alpha", "Clamp the output alpha to 0..1.", false) );
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
        p->setLabel("output");
        p->setHint("result. correction: smoothing offset per pixel (red / green, pixels). vectors: motion to the next frame "
                   "(red / green) and to the previous frame (blue / alpha), pixels.");
        p->appendOption("result");
        p->appendOption("correction");
        p->appendOption("vectors");
        p->setDefault(eOutResult);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXVectorBlendFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXVectorBlendPlugin(h);
}
} // anon namespace

static APFXVectorBlendFactory apfxVectorBlendFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxVectorBlendFactory)
