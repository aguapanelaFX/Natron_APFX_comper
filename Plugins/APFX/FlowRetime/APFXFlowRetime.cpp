/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX FlowRetime: motion-estimated retiming.
//  timing: speed (input / output ranges with locks, like APFX Retime) or a source frame curve
//  method: frame (nearest), blend (mix of the two frames), motion (optical flow interpolation,
//          occlusion aware: what is hidden in one frame is taken from the other)
//  matte: optional foreground matte; foreground and background get their own motion and are
//         interpolated separately, then layered (no background dragged by the foreground)
//  motion blur: shutter in output frames, samples along the motion (vectors reused)
//  output: result, vectors (rg forward, ba backward, pixels), occlusion
// Vectors are cached per frame pair (shared with every flow node), so replays are fast.

#include <cmath>
#include <vector>
#include <algorithm>

#include "APFXFlowOfx.h"

using namespace OFX;
using APFXOps::Plane;
using namespace APFXFlow;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.FlowRetime"

enum Timing { eTimingSpeed = 0, eTimingFrame };
enum Method { eMethodFrame = 0, eMethodBlend, eMethodMotion };
enum BeforeAfter { eContinue = 0, eLoop, eBounce, eHold, eBlack };
enum Output { eOutResult = 0, eOutVectors, eOutOcclusion };
enum ShutterOffset { eShCentred = 0, eShStart, eShEnd };

struct Ranges
{
    double inF, inL, outF, outL, speed;
};

class APFXFlowRetimePlugin
    : public ImageEffect
{
public:
    APFXFlowRetimePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _matte = fetchClip("Matte");
        _timing = fetchChoiceParam("timing");
        _inF = fetchIntParam("inputFirst");
        _inL = fetchIntParam("inputLast");
        _outF = fetchIntParam("outputFirst");
        _outL = fetchIntParam("outputLast");
        _inFLock = fetchBooleanParam("inputFirstLock");
        _inLLock = fetchBooleanParam("inputLastLock");
        _outFLock = fetchBooleanParam("outputFirstLock");
        _outLLock = fetchBooleanParam("outputLastLock");
        _speed = fetchDoubleParam("speed");
        _reverse = fetchBooleanParam("reverse");
        _before = fetchChoiceParam("before");
        _after = fetchChoiceParam("after");
        _frame = fetchDoubleParam("frame");
        _method = fetchChoiceParam("method");
        _detail = fetchDoubleParam("vectorDetail");
        _smooth = fetchDoubleParam("smoothness");
        _filter = fetchChoiceParam("filter");
        _shutter = fetchDoubleParam("shutter");
        _samples = fetchIntParam("shutterSamples");
        _shOffset = fetchChoiceParam("shutterOffset");
        _output = fetchChoiceParam("output");
        _sublabel = paramExists(kNatronOfxParamStringSublabelName) ? fetchStringParam(kNatronOfxParamStringSublabelName) : NULL;
        updateVisibility();
    }

private:
    // ------------------------------------------------------------ timing
    Ranges compute() const
    {
        Ranges r;
        OfxRangeD clip = { 1., 1. };
        if ( _src->isConnected() ) {
            clip = _src->getFrameRange();
        }
        r.inF = _inFLock->getValue() ? _inF->getValue() : std::floor(clip.min + 0.5);
        r.inL = _inLLock->getValue() ? _inL->getValue() : std::floor(clip.max + 0.5);
        if (r.inL < r.inF) {
            std::swap(r.inF, r.inL);
        }
        const double inLen = r.inL - r.inF;
        const bool fL = _outFLock->getValue(), lL = _outLLock->getValue();
        if (fL && lL) {
            r.outF = _outF->getValue();
            r.outL = _outL->getValue();
            if (r.outL < r.outF) {
                std::swap(r.outF, r.outL);
            }
            r.speed = (r.outL > r.outF) ? inLen / (r.outL - r.outF) : 1.;
        } else {
            r.speed = std::max(1e-4, _speed->getValue());
            const double outLen = inLen / r.speed;
            if (lL) {
                r.outL = _outL->getValue();
                r.outF = r.outL - outLen;
            } else {
                r.outF = fL ? _outF->getValue() : r.inF;
                r.outL = r.outF + outLen;
            }
        }
        r.speed = std::max(1e-4, r.speed);

        return r;
    }

    void refreshFields()
    {
        const Ranges r = compute();
        if ( !_inFLock->getValue() && (_inF->getValue() != (int)r.inF) ) {
            _inF->setValue( (int)r.inF );
        }
        if ( !_inLLock->getValue() && (_inL->getValue() != (int)r.inL) ) {
            _inL->setValue( (int)r.inL );
        }
        const int oF = (int)std::floor(r.outF + 0.5), oL = (int)std::floor(r.outL + 0.5);
        if ( !_outFLock->getValue() && (_outF->getValue() != oF) ) {
            _outF->setValue(oF);
        }
        if ( !_outLLock->getValue() && (_outL->getValue() != oL) ) {
            _outL->setValue(oL);
        }
        if ( _outFLock->getValue() && _outLLock->getValue() && (std::fabs(_speed->getValue() - r.speed) > 1e-9) ) {
            _speed->setValue(r.speed);
        }
        if (_sublabel) {
            const char* m[3] = { "frame", "blend", "motion" };
            char buf[128];
            if (_timing->getValue() == eTimingFrame) {
                snprintf(buf, sizeof(buf), "frame curve (%s)", m[std::max( 0, std::min(2, _method->getValue() ) )]);
            } else {
                snprintf(buf, sizeof(buf), "%d-%d > %d-%d (%s)", (int)r.inF, (int)r.inL, oF, oL, m[std::max( 0, std::min(2, _method->getValue() ) )]);
            }
            std::string cur;
            _sublabel->getValue(cur);
            if (cur != buf) {
                _sublabel->setValue(buf);
            }
        }
        updateVisibility();
    }

    void updateVisibility()
    {
        const bool speed = _timing->getValue() == eTimingSpeed;
        _inF->setIsSecret(!speed);
        _inL->setIsSecret(!speed);
        _outF->setIsSecret(!speed);
        _outL->setIsSecret(!speed);
        _inFLock->setIsSecret(!speed);
        _inLLock->setIsSecret(!speed);
        _outFLock->setIsSecret(!speed);
        _outLLock->setIsSecret(!speed);
        _speed->setIsSecret(!speed);
        _reverse->setIsSecret(!speed);
        _frame->setIsSecret(speed);
        _speed->setEnabled( !( _outFLock->getValue() && _outLLock->getValue() ) );
        const bool motion = _method->getValue() == eMethodMotion;
        _detail->setEnabled(motion);
        _smooth->setEnabled(motion);
        _samples->setEnabled(_shutter->getValue() > 0.);
        _shOffset->setEnabled(_shutter->getValue() > 0.);
    }

    // source time for output time t (fractional); false = black
    bool sourceTime(double t, double* s) const
    {
        OfxRangeD clip = { 1., 1. };
        if ( _src->isConnected() ) {
            clip = _src->getFrameRange();
        }
        if (_timing->getValue() == eTimingFrame) {
            *s = std::max( clip.min, std::min( clip.max, _frame->getValueAtTime(t) ) );

            return true;
        }
        const Ranges r = compute();
        const double len = r.outL - r.outF;
        double u = t - r.outF;
        bool clampToInput = true;
        if ( (t < r.outF) || (t > r.outL) ) {
            const int mode = (t < r.outF) ? _before->getValue() : _after->getValue();
            if (mode == eBlack) {
                return false;
            }
            if (mode == eContinue) {
                clampToInput = false;
            } else if ( (mode == eHold) || (len <= 0.) ) {
                u = (t < r.outF) ? 0. : len;
            } else if (mode == eLoop) {
                const double p = len + 1.;
                u = std::fmod(u, p);
                if (u < 0) {
                    u += p;
                }
                u = std::min(u, len);
            } else {
                const double p = 2. * len;
                u = std::fmod(u, p);
                if (u < 0) {
                    u += p;
                }
                if (u > len) {
                    u = p - u;
                }
            }
        }
        double s0 = _reverse->getValue() ? r.inL - u * r.speed : r.inF + u * r.speed;
        if (clampToInput) {
            s0 = std::max( r.inF, std::min(r.inL, s0) );
        }
        *s = std::max( clip.min, std::min(clip.max, s0) );

        return true;
    }

    // source times of the shutter samples at output time t
    bool sampleTimes(double t, std::vector<double>& out) const
    {
        out.clear();
        const double sh = std::max( 0., _shutter->getValueAtTime(t) );
        const int n = (sh > 0.) ? std::max( 1, std::min( 64, _samples->getValueAtTime(t) ) ) : 1;
        double a = 0.;
        const int off = _shOffset->getValueAtTime(t);
        if (off == eShCentred) {
            a = -0.5 * sh;
        } else if (off == eShEnd) {
            a = -sh;
        }
        for (int i = 0; i < n; ++i) {
            const double ot = (n == 1 && sh <= 0.) ? t : t + a + sh * (i + 0.5) / n;
            double s;
            if ( !sourceTime(ot, &s) ) {
                return false;
            }
            out.push_back(s);
        }

        return true;
    }

    // frames a source time reads
    void framesOf(double s, int method, double* f0, double* f1, double* a) const
    {
        OfxRangeD clip = _src->getFrameRange();
        *f0 = std::floor(s + 1e-6);
        *a = s - *f0;
        if (method == eMethodFrame) {
            *f0 = std::floor(s + 0.5);
            *a = 0.;
        }
        if (*a < 1e-3) {
            *a = 0.;
        } else if (*a > 0.999) {
            *f0 += 1.;
            *a = 0.;
        }
        *f1 = std::min(clip.max, *f0 + 1.);
        if (*f1 <= *f0) {
            *a = 0.;
        }
    }

    Params flowParams(double t) const
    {
        Params p;
        p.detail = (float)_detail->getValueAtTime(t);
        p.smoothness = (float)_smooth->getValueAtTime(t);

        return p;
    }

    // ------------------------------------------------------------ actions
    void changedParam(const InstanceChangedArgs& args, const std::string& n) OVERRIDE FINAL
    {
        if (args.reason == eChangeUserEdit) {
            if (n == "inputFirst") {
                _inFLock->setValue(true);
            } else if (n == "inputLast") {
                _inLLock->setValue(true);
            } else if (n == "outputFirst") {
                _outFLock->setValue(true);
            } else if (n == "outputLast") {
                _outLLock->setValue(true);
            } else if ( (n == "speed") && _outFLock->getValue() && _outLLock->getValue() ) {
                _outLLock->setValue(false);
            } else if ( (n == "timing") && (_timing->getValue() == eTimingFrame) && (_frame->getNumKeys() == 0) ) {
                // start the curve from the current speed mapping
                const Ranges r = compute();
                _frame->setValueAtTime(r.outF, _reverse->getValue() ? r.inL : r.inF);
                _frame->setValueAtTime(r.outL, _reverse->getValue() ? r.inF : r.inL);
            }
        }
        refreshFields();
    }

    void changedClip(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if (n == kOfxImageEffectSimpleSourceClipName) {
            refreshFields();
        }
    }

    bool getTimeDomain(OfxRangeD& range) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        if (_timing->getValue() == eTimingFrame) {
            range = _src->getFrameRange();

            return true;
        }
        const Ranges r = compute();
        range.min = std::floor(r.outF + 0.5);
        range.max = std::floor(r.outL + 0.5);

        return true;
    }

    void getFramesNeeded(const FramesNeededArguments& args, FramesNeededSetter& frames) OVERRIDE FINAL
    {
        std::vector<double> ts;
        const int method = _method->getValueAtTime(args.time);
        if ( !sampleTimes(args.time, ts) || ts.empty() ) {
            OfxRangeD r = { args.time, args.time };
            frames.setFramesNeeded(*_src, r);

            return;
        }
        double lo = 1e30, hi = -1e30;
        for (size_t i = 0; i < ts.size(); ++i) {
            double f0, f1, a;
            framesOf(ts[i], method, &f0, &f1, &a);
            lo = std::min(lo, f0);
            hi = std::max(hi, a > 0. ? f1 : f0);
        }
        OfxRangeD r = { lo, hi };
        frames.setFramesNeeded(*_src, r);
        if ( _matte->isConnected() ) {
            frames.setFramesNeeded(*_matte, r);
        }
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        // whole frames: motion can come from anywhere
        double s;
        const double st = sourceTime(args.time, &s) ? std::floor(s + 0.5) : args.time;
        rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(st) );
        if ( _matte->isConnected() ) {
            rois.setRegionOfInterest( *_matte, _src->getRegionOfDefinition(st) );
        }
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        double s;
        if ( !_src->isConnected() || !sourceTime(args.time, &s) ) {
            return false;
        }
        rod = _src->getRegionOfDefinition( std::floor(s) );
        const OfxRectD r2 = _src->getRegionOfDefinition( std::floor(s) + 1. );
        rod.x1 = std::min(rod.x1, r2.x1);
        rod.y1 = std::min(rod.y1, r2.y1);
        rod.x2 = std::max(rod.x2, r2.x2);
        rod.y2 = std::max(rod.y2, r2.y2);

        return true;
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime
#ifdef OFX_EXTENSIONS_NATRON
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        if (_output->getValueAtTime(args.time) != eOutResult) {
            return false;
        }
        std::vector<double> ts;
        if ( !sampleTimes(args.time, ts) || (ts.size() != 1) ) {
            return false;
        }
        double f0, f1, a;
        framesOf(ts[0], _method->getValueAtTime(args.time), &f0, &f1, &a);
        if (a == 0.) {
            identityClip = _src;
            identityTime = f0;

            return true;
        }

        return false;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setOutputFrameVarying(true);
        cp.setOutputPremultiplication( _src->getPreMultiplication() );
    }

    // ------------------------------------------------------------ render
    // one source time into 'out' (rgba over the window)
    void renderSample(double s, double t, APFXFlowOfx::FrameStore& fs, APFXFlowOfx::FrameStore* ms, Plane& out,
                      Plane* vec, Plane* occ, const APFXOps::ParallelFor& pf)
    {
        const int method = _method->getValueAtTime(t);
        double f0, f1, a;
        framesOf(s, method, &f0, &f1, &a);
        const Plane& A = fs.plane(f0);
        const bool needVec = (vec != NULL) || (occ != NULL);
        if ( (a == 0.) && !needVec ) {
            out = A;

            return;
        }
        const Plane& B = fs.plane(f1);
        if ( (method == eMethodBlend) && !needVec ) {
            out.resize(A.w, A.h, 4);
            for (size_t i = 0; i < out.d.size(); ++i) {
                out.d[i] = (float)( A.d[i] * (1. - a) + B.d[i] * a );
            }

            return;
        }
        const Params prm = flowParams(t);
        const int filter = _filter->getValueAtTime(t);
        if ( !ms || (a == 0.) ) {
            FrameData& da = fs.frame(f0);
            FrameData& db = fs.frame(f1);
            PairPtr fl = pairFlows(f0, f1, da, db, prm, 0, pf);
            if (vec) {
                vec->resize(A.w, A.h, 4);
                pf([&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y) {
                        for (int x = 0; x < A.w; ++x) {
                            float* d = vec->px(x, y);
                            fl->f01.atImage( (float)x, (float)y, &d[0], &d[1] );
                            fl->f10.atImage( (float)x, (float)y, &d[2], &d[3] );
                        }
                    }
                }, A.h);
            }
            if (a == 0.) {
                out = A;

                return;
            }
            InterpDebug dbg;
            interpolate(A, B, *fl, (float)a, filter, out, pf, occ ? &dbg : NULL);
            if (occ) {
                occ->resize(A.w, A.h, 4);
                for (size_t i = 0; i < (size_t)A.w * A.h; ++i) {
                    occ->d[i * 4] = dbg.occlusion.d[i * 2];
                    occ->d[i * 4 + 1] = dbg.occlusion.d[i * 2 + 1];
                    occ->d[i * 4 + 2] = 0.f;
                    occ->d[i * 4 + 3] = 1.f;
                }
            }

            return;
        }
        // matte: foreground and background layers, each with its own motion
        const Plane& mA = ms->plane(f0);
        const Plane& mB = ms->plane(f1);
        FrameData bgA = fs.frame(f0), bgB = fs.frame(f1), fgA = fs.frame(f0), fgB = fs.frame(f1);
        Plane m1A(A.w, A.h, 1), m1B(A.w, A.h, 1);
        for (size_t i = 0; i < (size_t)A.w * A.h; ++i) {
            m1A.d[i] = mA.d[i * 4 + 3];
            m1B.d[i] = mB.d[i * 4 + 3];
        }
        APFXFlowOfx::weightsOf(m1A, false, bgA);
        APFXFlowOfx::weightsOf(m1B, false, bgB);
        APFXFlowOfx::weightsOf(m1A, true, fgA);
        APFXFlowOfx::weightsOf(m1B, true, fgB);
        PairPtr flBg = pairFlows(f0, f1, bgA, bgB, prm, 1, pf);
        PairPtr flFg = pairFlows(f0, f1, fgA, fgB, prm, 2, pf);
        // foreground: colour x matte and the matte (5 channels); background: colour
        Plane fA(A.w, A.h, 5), fB(A.w, A.h, 5);
        for (size_t i = 0; i < (size_t)A.w * A.h; ++i) {
            for (int c = 0; c < 4; ++c) {
                fA.d[i * 5 + c] = A.d[i * 4 + c] * m1A.d[i];
                fB.d[i * 5 + c] = B.d[i * 4 + c] * m1B.d[i];
            }
            fA.d[i * 5 + 4] = m1A.d[i];
            fB.d[i * 5 + 4] = m1B.d[i];
        }
        Plane fg, bg;
        interpolate(fA, fB, *flFg, (float)a, filter, fg, pf);
        interpolate(A, B, *flBg, (float)a, filter, bg, pf, NULL);
        out.resize(A.w, A.h, 4);
        for (size_t i = 0; i < (size_t)A.w * A.h; ++i) {
            const float m = std::max( 0.f, std::min(1.f, fg.d[i * 5 + 4]) );
            for (int c = 0; c < 4; ++c) {
                out.d[i * 4 + c] = fg.d[i * 5 + c] + bg.d[i * 4 + c] * (1.f - m);
            }
        }
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        std::unique_ptr<Image> dst( _dst->fetchImage(args.time) );
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        const OfxRectI win = args.renderWindow;
        const int W = win.x2 - win.x1, H = win.y2 - win.y1;
        const int nc = dst->getPixelComponentCount();
        const APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        std::vector<double> ts;
        Plane acc(W, H, 4, 0.f);
        const int output = _output->getValueAtTime(args.time);
        if ( sampleTimes(args.time, ts) && !ts.empty() ) {
            APFXFlowOfx::FrameStore fs, mst;
            fs.clip = _src;
            fs.win = win;
            mst.clip = _matte;
            mst.win = win;
            const bool useMatte = _matte->isConnected() && (_method->getValueAtTime(args.time) == eMethodMotion);
            if (output != eOutResult) {
                // diagnostics at the centre sample
                Plane o, v, oc;
                renderSample( ts[ts.size() / 2], args.time, fs, NULL, o, output == eOutVectors ? &v : NULL,
                              output == eOutOcclusion ? &oc : NULL, pf );
                acc = (output == eOutVectors) ? v : oc;
                if ( acc.d.empty() ) {
                    acc.resize(W, H, 4, 0.f);
                }
            } else {
                const float k = 1.f / ts.size();
                for (size_t i = 0; i < ts.size() && !abort(); ++i) {
                    Plane o;
                    renderSample(ts[i], args.time, fs, useMatte ? &mst : NULL, o, NULL, NULL, pf);
                    if (o.d.size() == acc.d.size()) {
                        for (size_t j = 0; j < acc.d.size(); ++j) {
                            acc.d[j] += o.d[j] * k;
                        }
                    }
                }
            }
        }
        for (int y = win.y1; y < win.y2; ++y) {
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (!d) {
                continue;
            }
            const float* s = acc.px(0, y - win.y1);
            for (int x = 0; x < W; ++x, d += nc, s += 4) {
                if (nc == 4) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                } else if (nc == 3) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                } else {
                    d[0] = s[3];
                }
            }
        }
    }

    Clip* _dst;
    Clip* _src;
    Clip* _matte;
    ChoiceParam* _timing;
    IntParam* _inF;
    IntParam* _inL;
    IntParam* _outF;
    IntParam* _outL;
    BooleanParam* _inFLock;
    BooleanParam* _inLLock;
    BooleanParam* _outFLock;
    BooleanParam* _outLLock;
    DoubleParam* _speed;
    BooleanParam* _reverse;
    ChoiceParam* _before;
    ChoiceParam* _after;
    DoubleParam* _frame;
    ChoiceParam* _method;
    DoubleParam* _detail;
    DoubleParam* _smooth;
    ChoiceParam* _filter;
    DoubleParam* _shutter;
    IntParam* _samples;
    ChoiceParam* _shOffset;
    ChoiceParam* _output;
    StringParam* _sublabel;
};

mDeclarePluginFactory(APFXFlowRetimeFactory, {}, {});

void
APFXFlowRetimeFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "FlowRetime",
                            "APFX FlowRetime: retimes with motion estimation. New frames are built from the two "
                            "nearest frames pushed along their motion; what is hidden in one frame is taken from the "
                            "other. timing: speed (input / output ranges with locks) or a source frame curve. "
                            "method: frame, blend or motion. Connect a foreground matte to give the foreground and the "
                            "background their own motion. shutter adds motion blur from the same vectors. Vectors are "
                            "cached per frame pair, so replays and neighbouring frames are fast; lower 'vector detail' "
                            "for speed on big plates.");
    desc.setPluginGrouping("Time");
    desc.setTemporalClipAccess(true);
}

static void
lockParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint)
{
    BooleanParamDescriptor* p = desc.defineBooleanParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(false);
    p->setAnimates(false);
    p->setLayoutHint(eLayoutHintNoNewLine, 4);
    page->addChild(*p);
}

static IntParamDescriptor*
frameParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* hint, int def)
{
    IntParamDescriptor* p = desc.defineIntParam(name);
    p->setLabel("");
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(-10000000, 10000000);
    p->setDisplayRange(1, 200);
    p->setAnimates(false);
    page->addChild(*p);

    return p;
}

static ChoiceParamDescriptor*
beforeAfter(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint)
{
    ChoiceParamDescriptor* p = desc.defineChoiceParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->appendOption("continue", "Keep retiming past the range at the same speed.");
    p->appendOption("loop", "Repeat the range.");
    p->appendOption("bounce", "Play the range back and forth.");
    p->appendOption("hold", "Hold the end frame.");
    p->appendOption("black", "Output black.");
    p->setDefault(eHold);
    p->setAnimates(false);
    page->addChild(*p);

    return p;
}

void
APFXFlowRetimeFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
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
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectOutputClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setSupportsTiles(false);
    }
    PageParamDescriptor* page = desc.definePageParam("FlowRetime");
#ifdef OFX_EXTENSIONS_NATRON
    {
        StringParamDescriptor* p = desc.defineStringParam(kNatronOfxParamStringSublabelName);
        p->setIsSecretAndDisabled(true);
        p->setEvaluateOnChange(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
#endif
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("timing");
        p->setLabel("timing");
        p->setHint("speed: input / output ranges and a speed. frame: a curve giving the source frame at each output frame "
                   "(starts from the current speed mapping; animate it for speed ramps).");
        p->appendOption("speed");
        p->appendOption("frame");
        p->setDefault(eTimingSpeed);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    lockParam(desc, page, "inputFirstLock", "input range", "Lock input.first (otherwise it follows the input clip).");
    frameParam(desc, page, "inputFirst", "First input frame used. Typing a value locks it.", 1)->setLayoutHint(eLayoutHintNoNewLine, 16);
    lockParam(desc, page, "inputLastLock", "", "Lock input.last (otherwise it follows the input clip).");
    frameParam(desc, page, "inputLast", "Last input frame used. Typing a value locks it.", 100);
    lockParam(desc, page, "outputFirstLock", "output range", "Lock output.first (otherwise it equals input.first).");
    frameParam(desc, page, "outputFirst", "First output frame. Typing a value locks it.", 1)->setLayoutHint(eLayoutHintNoNewLine, 16);
    lockParam(desc, page, "outputLastLock", "", "Lock output.last (both ends locked: the speed is computed so the ranges match).");
    frameParam(desc, page, "outputLast", "Last output frame. Typing a value locks it.", 100);
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("speed");
        p->setLabel("speed");
        p->setHint("0.5 = half speed (twice as long, new frames built in between), 2 = double speed.");
        p->setDefault(0.5);
        p->setRange(0.0001, 1000.);
        p->setDisplayRange(0.1, 4.);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("reverse");
        p->setLabel("reverse");
        p->setHint("Play the input range backwards.");
        p->setDefault(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("frame");
        p->setLabel("frame");
        p->setHint("Source frame shown at this output frame (fractional values are built from the motion). Animate it.");
        p->setDefault(1.);
        p->setRange(-1e7, 1e7);
        p->setDisplayRange(1., 200.);
        p->setAnimates(true);
        page->addChild(*p);
    }
    beforeAfter(desc, page, "before", "before", "What to output before output.first.")->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::divider( beforeAfter(desc, page, "after", "after", "What to output after output.last.") );
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("method");
        p->setLabel("method");
        p->setHint("frame: nearest frame. blend: mix of the two nearest frames. motion: new frames built along the motion.");
        p->appendOption("frame");
        p->appendOption("blend");
        p->appendOption("motion");
        p->setDefault(eMethodMotion);
        p->setAnimates(false);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "vectorDetail", "vector detail",
                          "Resolution of the motion vectors: 1 = full resolution (finest, slowest), 0.5 = half (default), "
                          "0.25 = quarter (fast, big plates).", 0.5, 0.1, 1., 0.05, 1.);
    APFXOfx::defineDouble(desc, page, "smoothness", "smoothness",
                          "0 = vectors follow every detail, 1 = smooth vectors (fewer tears on flat or noisy areas).", 0.5, 0., 1., 0., 1.);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("filter");
        p->setLabel("filter");
        p->setHint("Image filter when pushing pixels along the motion. bicubic: sharp, never overshoots. bilinear: softer, faster.");
        p->appendOption("bilinear");
        p->appendOption("bicubic");
        p->setDefault(1);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "shutter", "shutter",
                          "Motion blur: shutter length in output frames (0 = off, 0.5 = 180 degrees).", 0., 0., 2., 0., 10.);
    {
        IntParamDescriptor* p = desc.defineIntParam("shutterSamples");
        p->setLabel("shutter samples");
        p->setHint("Frames built across the shutter and averaged (vectors are reused, only the images are rebuilt).");
        p->setDefault(5);
        p->setRange(1, 64);
        p->setDisplayRange(1, 16);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("shutterOffset");
        p->setLabel("offset");
        p->setHint("Shutter around the frame (centred), from the frame on (start) or up to the frame (end).");
        p->appendOption("centred");
        p->appendOption("start");
        p->appendOption("end");
        p->setDefault(eShCentred);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
        p->setLabel("output");
        p->setHint("result: the retimed image. vectors: motion to the next frame (red / green) and to the previous frame "
                   "(blue / alpha), in pixels. occlusion: red = hidden in the earlier frame, green = hidden in the later one.");
        p->appendOption("result");
        p->appendOption("vectors");
        p->appendOption("occlusion");
        p->setDefault(eOutResult);
        p->setAnimates(false);
        page->addChild(*p);
    }
}

ImageEffect*
APFXFlowRetimeFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXFlowRetimePlugin(h);
}
} // anon namespace

static APFXFlowRetimeFactory apfxFlowRetimeFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxFlowRetimeFactory)
