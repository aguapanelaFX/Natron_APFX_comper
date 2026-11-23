/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX ScreenKey: alpha extraction from green / blue / any colour screens, made for compressed
// and generated footage (see APFXScreenKey.h). The colours pass through untouched.
// Optional temporal stabilisation of the edges uses the APFX motion vectors (shared cache).

#include <cmath>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "ofxsInteract.h"
#include "APFXFlowOfx.h"
#include "APFXScreenKey.h"

using namespace OFX;
using APFXOps::Plane;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.ScreenKey"

enum Output { eOutRGBA = 0, eOutPremult, eOutAlpha, eOutStatus, eOutScreenMatte, eOutPlate, eOutHalo };

// keyed alphas of recent frames (stabilisation reuses the neighbours' keys while playing)
class AlphaCache
{
public:
    static AlphaCache& instance() { static AlphaCache c; return c; }
    std::shared_ptr<const APFXSK::Result> get(const std::string& k)
    {
        std::lock_guard<std::mutex> l(_m);
        std::map<std::string, Entry>::iterator it = _map.find(k);
        if ( it == _map.end() ) {
            return std::shared_ptr<const APFXSK::Result>();
        }
        it->second.use = ++_clock;

        return it->second.r;
    }
    void put(const std::string& k, const std::shared_ptr<const APFXSK::Result>& r)
    {
        std::lock_guard<std::mutex> l(_m);
        Entry e;
        e.r = r;
        e.use = ++_clock;
        _map[k] = e;
        while (_map.size() > 12) {
            std::map<std::string, Entry>::iterator old = _map.begin();
            for (std::map<std::string, Entry>::iterator it = _map.begin(); it != _map.end(); ++it) {
                if (it->second.use < old->second.use) {
                    old = it;
                }
            }
            _map.erase(old);
        }
    }

private:
    struct Entry
    {
        std::shared_ptr<const APFXSK::Result> r;
        uint64_t use = 0;
    };
    std::mutex _m;
    std::map<std::string, Entry> _map;
    uint64_t _clock = 0;
};

class APFXScreenKeyPlugin
    : public ImageEffect
{
public:
    APFXScreenKeyPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _plate = fetchClip("Plate");
        _inside = fetchClip("Inside");
        _outside = fetchClip("Outside");
        _screen = fetchChoiceParam("screen");
        _auto = fetchBooleanParam("autoColour");
        _colour = fetchRGBParam("screenColour");
        _plateMode = fetchChoiceParam("plateMode");
        _plateSize = fetchIntParam("plateSize");
        _plateSmooth = fetchDoubleParam("plateSmooth");
        _chroma = fetchBooleanParam("chromaRebuild");
        _chromaR = fetchIntParam("chromaRadius");
        _denoise = fetchDoubleParam("screenDenoise");
        _denoiseSize = fetchDoubleParam("denoiseSize");
        _gain = fetchDoubleParam("screenGain");
        _balance = fetchDoubleParam("screenBalance");
        _clipB = fetchDoubleParam("clipBlack");
        _clipW = fetchDoubleParam("clipWhite");
        _core = fetchBooleanParam("core");
        _coreGain = fetchDoubleParam("coreGain");
        _coreClipW = fetchDoubleParam("coreClipWhite");
        _coreShrink = fetchIntParam("coreShrink");
        _halo = fetchDoubleParam("haloFlatten");
        _haloW = fetchIntParam("haloWidth");
        _haloD = fetchDoubleParam("haloDetail");
        _ring = fetchChoiceParam("ring");
        _snap = fetchDoubleParam("edgeSnap");
        _snapR = fetchIntParam("snapRadius");
        _grow = fetchDoubleParam("shrinkGrow");
        _soften = fetchDoubleParam("soften");
        _stab = fetchDoubleParam("stabilise");
        _output = fetchChoiceParam("output");
        _areaOn = fetchBooleanParam("areaOn");
        _areaBL = fetchDouble2DParam("areaBottomLeft");
        _areaTR = fetchDouble2DParam("areaTopRight");
        _frames = fetchChoiceParam("analyseFrames");
        _report = fetchStringParam("analysisReport");
        refresh();
    }

    // ---- analysis area (drawn in the viewer, used once by analyse)
    bool drawMode() const { return _drawMode; }
    bool areaVisible(double* x0, double* y0, double* x1, double* y1) const
    {
        if ( !_areaOn->getValue() ) {
            return false;
        }
        _areaBL->getValue(*x0, *y0);
        _areaTR->getValue(*x1, *y1);

        return true;
    }
    void setArea(const OfxPointD& a, const OfxPointD& b)
    {
        _drawMode = false;
        if ( (std::fabs(a.x - b.x) < 2.) || (std::fabs(a.y - b.y) < 2.) ) {
            return;   // a click, not a box
        }
        beginEditBlock("draw area");
        _areaBL->setValue( std::min(a.x, b.x), std::min(a.y, b.y) );
        _areaTR->setValue( std::max(a.x, b.x), std::max(a.y, b.y) );
        _areaOn->setValue(true);
        endEditBlock();
        redrawOverlays();
    }

private:
    void refresh()
    {
        const bool custom = _screen->getValue() == APFXSK::eScreenCustom;
        _auto->setEnabled(!custom);
        _colour->setEnabled( custom || !_auto->getValue() );
        _plateSize->setEnabled(_plateMode->getValue() == APFXSK::ePlateAuto);
        _plateSmooth->setEnabled(_plateMode->getValue() == APFXSK::ePlateAuto);
        _chromaR->setEnabled( _chroma->getValue() );
        const bool core = _core->getValue();
        _coreGain->setEnabled(core);
        _coreClipW->setEnabled(core);
        _coreShrink->setEnabled(core);
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& n) OVERRIDE FINAL
    {
        if (n == "drawArea") {
            _drawMode = true;      // the next drag in the viewer draws the area
            _areaOn->setValue(false);
            redrawOverlays();

            return;
        }
        if (n == "analyse") {
            runAnalysis(args);

            return;
        }
        if ( (n == "screen") && (args.reason == eChangeUserEdit) ) {
            const int s = _screen->getValue();
            if (s == APFXSK::eScreenGreen) {
                _colour->setValue(0.1, 0.6, 0.15);
            } else if (s == APFXSK::eScreenBlue) {
                _colour->setValue(0.08, 0.2, 0.7);
            } else {
                _auto->setValue(false);
            }
        }
        if ( (n == "screenColour") && (args.reason == eChangeUserEdit) && (_screen->getValue() != APFXSK::eScreenCustom) ) {
            _auto->setValue(false);   // a picked colour is used as is
        }
        refresh();
    }

    APFXSK::Params params(double t) const
    {
        APFXSK::Params p;
        p.screen = _screen->getValueAtTime(t);
        p.autoColour = _auto->getValueAtTime(t);
        double r, g, b;
        _colour->getValueAtTime(t, r, g, b);
        p.colour[0] = (float)r;
        p.colour[1] = (float)g;
        p.colour[2] = (float)b;
        p.plateMode = _plateMode->getValueAtTime(t);
        if ( (p.plateMode == APFXSK::ePlateInput) && !_plate->isConnected() ) {
            p.plateMode = APFXSK::ePlateAuto;
        }
        p.plateSize = _plateSize->getValueAtTime(t);
        p.plateSmooth = (float)_plateSmooth->getValueAtTime(t);
        p.chromaRebuild = _chroma->getValueAtTime(t);
        p.chromaRadius = _chromaR->getValueAtTime(t);
        p.denoise = (float)_denoise->getValueAtTime(t);
        p.denoiseSize = (float)_denoiseSize->getValueAtTime(t);
        p.gain = (float)_gain->getValueAtTime(t);
        p.balance = (float)_balance->getValueAtTime(t);
        p.clipBlack = (float)_clipB->getValueAtTime(t);
        p.clipWhite = (float)_clipW->getValueAtTime(t);
        p.core = _core->getValueAtTime(t);
        p.coreGain = (float)_coreGain->getValueAtTime(t);
        p.coreClipWhite = (float)_coreClipW->getValueAtTime(t);
        p.coreShrink = _coreShrink->getValueAtTime(t);
        p.halo = (float)_halo->getValueAtTime(t);
        p.haloWidth = _haloW->getValueAtTime(t);
        p.haloDetail = (float)_haloD->getValueAtTime(t);
        p.ring = _ring->getValueAtTime(t);
        p.snap = (float)_snap->getValueAtTime(t);
        p.snapRadius = _snapR->getValueAtTime(t);
        p.grow = (float)_grow->getValueAtTime(t);
        p.soften = (float)_soften->getValueAtTime(t);

        return p;
    }

    static std::string keyOf(const APFXSK::Params& p, double frame, uint64_t srcHash, uint64_t plateHash, const OfxRectI& win)
    {
        std::ostringstream s;
        s << frame << '|' << srcHash << '|' << plateHash << '|' << win.x1 << ',' << win.y1 << ',' << win.x2 << ',' << win.y2 << '|'
          << p.screen << p.autoColour << ',' << p.colour[0] << ',' << p.colour[1] << ',' << p.colour[2] << '|' << p.plateMode << ','
          << p.plateSize << ',' << p.plateSmooth << '|' << p.chromaRebuild << p.chromaRadius << ',' << p.denoise << ',' << p.denoiseSize
          << '|' << p.gain << ',' << p.balance << ',' << p.clipBlack << ',' << p.clipWhite << '|' << p.core << p.coreGain << ','
          << p.coreClipWhite << ',' << p.coreShrink << '|' << p.halo << ',' << p.haloWidth << ',' << p.haloDetail << ',' << p.ring
          << '|' << p.snap << ',' << p.snapRadius << ',' << p.grow << ',' << p.soften;

        return s.str();
    }

    // key of one frame (cached)
    std::shared_ptr<const APFXSK::Result> keyFrame(double f, APFXFlowOfx::FrameStore& fs, APFXFlowOfx::FrameStore& ps,
                                                     const APFXOps::ParallelFor& pf)
    {
        const APFXSK::Params p = params(f);
        const uint64_t sh = fs.frame(f).hash;
        const Plane* plate = NULL;
        uint64_t ph = 0;
        if (p.plateMode == APFXSK::ePlateInput) {
            plate = &ps.plane(f);
            ph = ps.frame(f).hash;
        }
        const std::string k = keyOf(p, f, sh, ph, fs.win);
        std::shared_ptr<const APFXSK::Result> r = AlphaCache::instance().get(k);
        if (r) {
            return r;
        }
        std::shared_ptr<APFXSK::Result> nr = std::make_shared<APFXSK::Result>();
        APFXSK::screenKey(fs.plane(f), plate, p, *nr, pf);
        AlphaCache::instance().put(k, nr);

        return nr;
    }

    // full source frame as a plane (+ its pixel bounds)
    bool fetchFull(double t, Plane& p, OfxRectI& b)
    {
        std::unique_ptr<Image> img( _src->fetchImage(t) );
        if (!img) {
            return false;
        }
        b = img->getBounds();
        img.reset();

        return APFXFlowOfx::fetchPlane(_src, t, b, 4, p);
    }

    void runAnalysis(const InstanceChangedArgs& args)
    {
        if ( !_src->isConnected() ) {
            _report->setValue("connect a source first");

            return;
        }
        std::vector<double> times;
        if (_frames->getValue() == 1) {
            const OfxRangeD r = _src->getFrameRange();
            for (int i = 0; i < 5; ++i) {
                times.push_back( std::floor(r.min + (r.max - r.min) * (i + 0.5) / 5. + 0.5) );
            }
        } else {
            times.push_back(args.time);
        }
        const APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        const APFXSK::Params cur = params(args.time);
        std::vector<APFXSK::Analysis> res;
        double ax0, ay0, ax1, ay1;
        const bool useArea = areaVisible(&ax0, &ay0, &ax1, &ay1);
        const double par = _src->getPixelAspectRatio();
        for (size_t k = 0; k < times.size(); ++k) {
            Plane p;
            OfxRectI b;
            if ( !fetchFull(times[k], p, b) || p.d.empty() ) {
                continue;
            }
            int area[4];
            if (useArea) {
                // canonical -> pixels of this image (render scale of the action)
                const double sx = args.renderScale.x / par, sy = args.renderScale.y;
                area[0] = (int)std::floor(ax0 * sx) - b.x1;
                area[1] = (int)std::floor(ay0 * sy) - b.y1;
                area[2] = (int)std::ceil(ax1 * sx) - b.x1;
                area[3] = (int)std::ceil(ay1 * sy) - b.y1;
            }
            APFXSK::Analysis an;
            APFXSK::analyse(p, useArea ? area : NULL, cur, an, pf);
            if (an.found) {
                res.push_back(an);
            }
        }
        _areaOn->setValue(false);   // the area is used once
        redrawOverlays();
        if ( res.empty() ) {
            _report->setValue("no screen colour found (try another frame or area)");

            return;
        }
        // combine the frames: medians for colours / noise, the safest for the rest
        APFXSK::Analysis a = res[0];
        if (res.size() > 1) {
            for (int c = 0; c < 3; ++c) {
                std::vector<float> v;
                for (size_t i = 0; i < res.size(); ++i) { v.push_back(res[i].colour[c]); }
                a.colour[c] = APFXSK::percentile(v, 0.5f);
                a.p.colour[c] = a.colour[c];
            }
            std::vector<float> nz;
            int haloVotes = 0;
            for (size_t i = 0; i < res.size(); ++i) {
                const APFXSK::Analysis& r = res[i];
                a.unevenness = std::max(a.unevenness, r.unevenness);
                a.subsampled = a.subsampled || r.subsampled;
                a.heavy = a.heavy || r.heavy;
                a.eightBit = a.eightBit || r.eightBit;
                nz.push_back(r.noise);
                a.p.denoise = std::max(a.p.denoise, r.p.denoise);
                a.p.denoiseSize = std::max(a.p.denoiseSize, r.p.denoiseSize);
                a.clipBlack = std::max(a.clipBlack, r.clipBlack);
                a.clipWhite = std::min(a.clipWhite, r.clipWhite);
                haloVotes += r.halo;
                a.haloWidth = std::max(a.haloWidth, r.haloWidth);
            }
            a.noise = APFXSK::percentile(nz, 0.5f);
            a.p.plateMode = (a.unevenness < 0.03f) ? APFXSK::ePlateFlat : APFXSK::ePlateAuto;
            a.p.plateSmooth = a.unevenness > 0.25f ? 2.f : 4.f;
            a.p.chromaRebuild = a.subsampled;
            a.p.chromaRadius = a.heavy ? 2 : 1;
            a.p.clipBlack = a.clipBlack;
            a.p.clipWhite = std::max(a.clipBlack + 0.2f, a.clipWhite);
            a.halo = (2 * std::abs(haloVotes) >= (int)res.size()) ? (haloVotes > 0 ? 1 : -1) : 0;
            a.p.halo = a.halo ? 1.f : 0.f;
            a.p.haloWidth = std::max(1, a.haloWidth);
            a.p.ring = a.halo >= 0 ? APFXSK::eRingBright : APFXSK::eRingDark;
        }
        // apply (one undo step)
        beginEditBlock("analyse");
        _screen->setValue(a.p.screen);
        _auto->setValue(false);
        _colour->setValue(a.p.colour[0], a.p.colour[1], a.p.colour[2]);
        _plateMode->setValue(a.p.plateMode);
        _plateSmooth->setValue(a.p.plateSmooth);
        _chroma->setValue(a.p.chromaRebuild);
        _chromaR->setValue(a.p.chromaRadius);
        _denoise->setValue(a.p.denoise);
        _denoiseSize->setValue(a.p.denoiseSize);
        _clipB->setValue(a.p.clipBlack);
        _clipW->setValue(a.p.clipWhite);
        _halo->setValue(a.p.halo);
        if (a.halo) {
            _haloW->setValue(a.p.haloWidth);
            _haloD->setValue(a.p.haloDetail);
            _ring->setValue(a.p.ring);
        }
        endEditBlock();
        // report
        const char* scr[3] = { "green", "blue", "custom" };
        char buf[512];
        snprintf(buf, sizeof(buf), "%s screen (%.3f %.3f %.3f) | %s%s | %s%s | noise %s | %s | clips %.3f / %.3f%s",
                 scr[std::max( 0, std::min(2, a.p.screen) )], a.colour[0], a.colour[1], a.colour[2],
                 a.unevenness < 0.03f ? "even" : (a.unevenness < 0.15f ? "slightly uneven" : "uneven"),
                 "",
                 a.subsampled ? (a.heavy ? "soft colour (heavy compression)" : "blocky colour (compressed)") : "full colour",
                 a.eightBit ? ", 8-bit" : "",
                 a.noise < 0.01f ? "low" : (a.noise < 0.03f ? "medium" : "high"),
                 a.halo > 0 ? "bright ring" : (a.halo < 0 ? "dark ring" : "no ring"),
                 a.p.clipBlack, a.p.clipWhite,
                 res.size() > 1 ? " (5 frames)" : "");
        std::string rep = buf;
        if (a.halo) {
            char w[64];
            snprintf(w, sizeof(w), " ~%d px", a.p.haloWidth);
            rep.insert(rep.find(a.halo > 0 ? "bright ring" : "dark ring") + (a.halo > 0 ? 11 : 9), w);
        }
        _report->setValue(rep);
    }

    void getFramesNeeded(const FramesNeededArguments& args, FramesNeededSetter& frames) OVERRIDE FINAL
    {
        OfxRangeD r = { args.time, args.time };
        if (_stab->getValueAtTime(args.time) > 0.) {
            const OfxRangeD c = _src->getFrameRange();
            r.min = std::max(c.min, args.time - 1.);
            r.max = std::min(c.max, args.time + 1.);
        }
        frames.setFramesNeeded(*_src, r);
        if ( _plate->isConnected() ) {
            frames.setFramesNeeded(*_plate, r);
        }
        OfxRangeD one = { args.time, args.time };
        if ( _inside->isConnected() ) {
            frames.setFramesNeeded(*_inside, one);
        }
        if ( _outside->isConnected() ) {
            frames.setFramesNeeded(*_outside, one);
        }
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        const OfxRectD rod = _src->getRegionOfDefinition(args.time);
        rois.setRegionOfInterest(*_src, rod);
        if ( _plate->isConnected() ) {
            rois.setRegionOfInterest(*_plate, rod);
        }
        if ( _inside->isConnected() ) {
            rois.setRegionOfInterest(*_inside, args.regionOfInterest);
        }
        if ( _outside->isConnected() ) {
            rois.setRegionOfInterest(*_outside, args.regionOfInterest);
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
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        const int o = _output->getValue();
        cp.setOutputPremultiplication( (o == eOutPremult) ? eImagePreMultiplied : eImageUnPreMultiplied );
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        std::unique_ptr<Image> dst( _dst->fetchImage(args.time) );
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        const double t = args.time;
        const OfxRectI win = args.renderWindow;
        const int W = win.x2 - win.x1, H = win.y2 - win.y1;
        const APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        APFXFlowOfx::FrameStore fs, ps;
        fs.clip = _src;
        fs.win = win;
        ps.clip = _plate;
        ps.win = win;
        std::shared_ptr<const APFXSK::Result> key = keyFrame(t, fs, ps, pf);
        Plane alpha = key->alpha;

        // ---- temporal stabilisation of the edges (motion compensated neighbours)
        const double stab = _stab->getValueAtTime(t);
        if ( (stab > 0.) && !abort() ) {
            const OfxRangeD c = _src->getFrameRange();
            APFXFlow::Params fp;
            fp.detail = 0.5f;
            fp.smoothness = 0.5f;
            Plane acc = alpha, wsum(W, H, 1, 1.f);
            for (int d = -1; d <= 1; d += 2) {
                const double f = t + d;
                if ( (f < c.min) || (f > c.max) ) {
                    continue;
                }
                std::shared_ptr<const APFXSK::Result> nk = keyFrame(f, fs, ps, pf);
                APFXFlow::PairPtr fl = APFXFlow::pairFlows(t, f, fs.frame(t), fs.frame(f), fp, 0, pf);
                const APFXFlow::Field& V = fl->f01;
                const Plane& na = nk->alpha;
                pf([&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y) {
                        for (int x = 0; x < W; ++x) {
                            const size_t i = (size_t)y * W + x;
                            const float a0 = alpha.d[i];
                            float vu, vv;
                            V.atImage( (float)x, (float)y, &vu, &vv );
                            float an;
                            APFXFlow::samplePlane(na, x + vu, y + vv, 0, &an);
                            if ( (std::max(a0, an) < 0.002f) || (std::min(a0, an) > 0.998f) ) {
                                continue;   // both clear screen or both solid
                            }
                            // neighbours that disagree a lot are another object (occlusion): ignored
                            const float dd = (an - a0) / 0.2f;
                            const float wk = (float)stab * std::exp(-dd * dd);
                            acc.d[i] += wk * an;
                            wsum.d[i] += wk;
                        }
                    }
                }, H);
            }
            for (size_t i = 0; i < alpha.d.size(); ++i) {
                alpha.d[i] = acc.d[i] / wsum.d[i];
            }
        }
        // ---- inside / outside masks
        if ( _inside->isConnected() || _outside->isConnected() ) {
            Plane in, out;
            const bool hasIn = APFXFlowOfx::fetchPlane(_inside, t, win, 1, in);
            const bool hasOut = APFXFlowOfx::fetchPlane(_outside, t, win, 1, out);
            for (size_t i = 0; i < alpha.d.size(); ++i) {
                float a = alpha.d[i];
                if (hasIn) { a = std::max( a, APFXSK::clamp01(in.d[i]) ); }
                if (hasOut) { a = std::min( a, 1.f - APFXSK::clamp01(out.d[i]) ); }
                alpha.d[i] = a;
            }
        }
        // ---- output
        const Plane& src = fs.plane(t);
        const int output = _output->getValueAtTime(t);
        const int nc = dst->getPixelComponentCount();
        pf([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, win.y1 + y);
                if (!d) {
                    continue;
                }
                for (int x = 0; x < W; ++x, d += nc) {
                    const size_t i = (size_t)y * W + x;
                    const float* s = src.px(x, y);
                    const float a = alpha.d[i];
                    float o[4] = { s[0], s[1], s[2], a };
                    switch (output) {
                    case eOutPremult: o[0] *= a; o[1] *= a; o[2] *= a; break;
                    case eOutAlpha: o[0] = o[1] = o[2] = a; break;
                    case eOutStatus: {
                        const float v = (a <= 0.002f) ? 0.f : (a >= 0.998f ? 1.f : 0.5f);
                        o[0] = o[1] = o[2] = v;
                        if (key->removed.d.size() == alpha.d.size() && key->removed.d[i] > 0.01f) {
                            o[0] = 1.f; o[1] *= 0.5f; o[2] *= 0.5f;   // halo flattened: red
                        }
                        o[3] = 1.f;
                        break;
                    }
                    case eOutScreenMatte: o[0] = o[1] = o[2] = o[3] = key->raw.d[i]; break;
                    case eOutPlate: {
                        const float* pl = &key->plate.d[i * 3];
                        o[0] = pl[0]; o[1] = pl[1]; o[2] = pl[2]; o[3] = 1.f;
                        break;
                    }
                    case eOutHalo: {
                        const float r = key->removed.d.empty() ? 0.f : key->removed.d[i];
                        o[0] = a + r; o[1] = a; o[2] = a; o[3] = 1.f;
                        break;
                    }
                    default: break;
                    }
                    for (int k = 0; k < nc; ++k) {
                        d[k] = (nc == 1) ? o[3] : o[k];
                    }
                }
            }
        }, H);
    }

    Clip* _dst;
    Clip* _src;
    Clip* _plate;
    Clip* _inside;
    Clip* _outside;
    ChoiceParam* _screen;
    BooleanParam* _auto;
    RGBParam* _colour;
    ChoiceParam* _plateMode;
    IntParam* _plateSize;
    DoubleParam* _plateSmooth;
    BooleanParam* _chroma;
    IntParam* _chromaR;
    DoubleParam* _denoise;
    DoubleParam* _denoiseSize;
    DoubleParam* _gain;
    DoubleParam* _balance;
    DoubleParam* _clipB;
    DoubleParam* _clipW;
    BooleanParam* _core;
    DoubleParam* _coreGain;
    DoubleParam* _coreClipW;
    IntParam* _coreShrink;
    DoubleParam* _halo;
    IntParam* _haloW;
    DoubleParam* _haloD;
    ChoiceParam* _ring;
    DoubleParam* _snap;
    IntParam* _snapR;
    DoubleParam* _grow;
    DoubleParam* _soften;
    DoubleParam* _stab;
    ChoiceParam* _output;
    BooleanParam* _areaOn;
    Double2DParam* _areaBL;
    Double2DParam* _areaTR;
    ChoiceParam* _frames;
    StringParam* _report;
    bool _drawMode = false;
};

// viewer overlay: draws the analysis area (after "draw area": drag a box, nothing renders)
class ScreenKeyInteract
    : public OverlayInteract
{
public:
    ScreenKeyInteract(OfxInteractHandle h, ImageEffect* fx)
        : OverlayInteract(h), _fx( dynamic_cast<APFXScreenKeyPlugin*>(fx) ) {}

    virtual bool draw(const DrawArgs& args) OVERRIDE FINAL
    {
        if (!_fx) {
            return false;
        }
        double x0, y0, x1, y1;
        if (_dragging) {
            x0 = std::min(_a.x, _b.x); x1 = std::max(_a.x, _b.x);
            y0 = std::min(_a.y, _b.y); y1 = std::max(_a.y, _b.y);
        } else if ( !_fx->areaVisible(&x0, &y0, &x1, &y1) ) {
            return false;
        }
        const double px = args.pixelScale.x, py = args.pixelScale.y;
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glEnable(GL_LINE_STIPPLE);
        glLineWidth(1.5f);
        for (int pass = 0; pass < 2; ++pass) {
            // dark shadow, then the yellow selection colour
            const double o = pass == 0 ? 1. : 0.;
            if (pass == 0) {
                glColor3f(0.f, 0.f, 0.f);
                glLineStipple(1, 0xFFFF);
            } else {
                glColor3f(1.f, 0.824f, 0.341f);
                glLineStipple(2, 0xF0F0);
            }
            glBegin(GL_LINE_LOOP);
            glVertex2d(x0 + o * px, y0 - o * py);
            glVertex2d(x1 + o * px, y0 - o * py);
            glVertex2d(x1 + o * px, y1 - o * py);
            glVertex2d(x0 + o * px, y1 - o * py);
            glEnd();
        }
        glPopAttrib();

        return true;
    }

    virtual bool penDown(const PenArgs& args) OVERRIDE FINAL
    {
        if ( !_fx || !_fx->drawMode() ) {
            return false;
        }
        _dragging = true;
        _a = _b = args.penPosition;
        requestRedraw();

        return true;
    }

    virtual bool penMotion(const PenArgs& args) OVERRIDE FINAL
    {
        if (!_dragging) {
            return false;
        }
        _b = args.penPosition;
        requestRedraw();   // overlay only: nothing renders while drawing

        return true;
    }

    virtual bool penUp(const PenArgs& args) OVERRIDE FINAL
    {
        if (!_dragging) {
            return false;
        }
        _dragging = false;
        _b = args.penPosition;
        _fx->setArea(_a, _b);
        requestRedraw();

        return true;
    }

private:
    APFXScreenKeyPlugin* _fx;
    bool _dragging = false;
    OfxPointD _a = { 0., 0. }, _b = { 0., 0. };
};

class ScreenKeyOverlayDescriptor
    : public DefaultEffectOverlayDescriptor<ScreenKeyOverlayDescriptor, ScreenKeyInteract>
{
};

mDeclarePluginFactory(APFXScreenKeyFactory, {}, {});

void
APFXScreenKeyFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "ScreenKey",
                            "APFX ScreenKey: alpha extraction from a green, blue or any colour screen, made for compressed "
                            "and generated footage. Matte only: the colours pass through untouched (despill and edge "
                            "colour are separate passes). Input repair (colour rebuilt at full resolution from the luma, "
                            "screen noise smoothing) is used for measuring only. The matte is the colour difference against "
                            "a clean plate (auto, plate input or flat) for uneven screens. Halo flatten removes the lighter "
                            "(or darker) glow ring generated images leave around the subject, keeping real detail. Edge "
                            "snap, shrink / grow, soften, inside / outside masks, and optional temporal stabilisation of "
                            "the edges along the motion.");
    desc.setPluginGrouping("Keyer");
    desc.setTemporalClipAccess(true);
    desc.setOverlayInteractDescriptor(new ScreenKeyOverlayDescriptor);
}

static IntParamDescriptor*
intParam(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint,
         int def, int lo, int hi, int dlo, int dhi)
{
    IntParamDescriptor* p = desc.defineIntParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(dlo, dhi);
    page->addChild(*p);

    return p;
}

void
APFXScreenKeyFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->setTemporalClipAccess(true);
        c->setSupportsTiles(false);
    }
    {
        ClipDescriptor* c = desc.defineClip("Plate");
        c->setLabel("plate");
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->setTemporalClipAccess(true);
        c->setSupportsTiles(false);
        c->setOptional(true);
    }
    const char* mn[2] = { "Inside", "Outside" };
    for (int i = 0; i < 2; ++i) {
        ClipDescriptor* c = desc.defineClip(mn[i]);
        c->setLabel(i == 0 ? "inside" : "outside");
        c->addSupportedComponent(ePixelComponentAlpha);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->setSupportsTiles(false);
        c->setOptional(true);
        c->setIsMask(true);
    }
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectOutputClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->setSupportsTiles(false);
    }
    PageParamDescriptor* page = desc.definePageParam("ScreenKey");
    // ---- analysis (optional starting setup)
    {
        PushButtonParamDescriptor* p = desc.definePushButtonParam("drawArea");
        p->setLabel("draw area");
        p->setHint("Optional: then drag a box in the viewer over the part to analyse (screen and subject edge). "
                   "Nothing renders while drawing. Without an area the whole frame is analysed.");
        p->setLayoutHint(eLayoutHintNoNewLine, 8);
        page->addChild(*p);
    }
    {
        PushButtonParamDescriptor* p = desc.definePushButtonParam("analyse");
        p->setLabel("analyse");
        p->setHint("Measures the screen colour, unevenness, compression, noise, halo ring and clips, and sets a starting "
                   "setup (one undo step). The area is used once and then hidden.");
        p->setLayoutHint(eLayoutHintNoNewLine, 8);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("analyseFrames");
        p->setLabel("");
        p->setHint("current frame, or 5 frames across the range (settings that hold for the whole shot).");
        p->appendOption("current frame");
        p->appendOption("5 frames");
        p->setDefault(0);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        StringParamDescriptor* p = desc.defineStringParam("analysisReport");
        p->setLabel("");
        p->setStringType(eStringTypeLabel);
        p->setDefault("");
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("areaOn");
        p->setDefault(false);
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    const char* an[2] = { "areaBottomLeft", "areaTopRight" };
    for (int i = 0; i < 2; ++i) {
        Double2DParamDescriptor* p = desc.defineDouble2DParam(an[i]);
        p->setDefault(0., 0.);
        p->setDoubleType(eDoubleTypeXYAbsolute);
        p->setDefaultCoordinateSystem(eCoordinatesCanonical);
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    // ---- screen
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("screen");
        p->setLabel("screen");
        p->setHint("green / blue: screen colour measured automatically each frame (or picked). custom: any colour, picked.");
        p->appendOption("green");
        p->appendOption("blue");
        p->appendOption("custom");
        p->setDefault(APFXSK::eScreenGreen);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    APFXOfx::defineBool(desc, page, "autoColour", "auto",
                        "Measure the screen colour on every frame (green / blue). Off: the screen colour below is used "
                        "(picking a colour turns this off).", true);
    {
        RGBParamDescriptor* p = desc.defineRGBParam("screenColour");
        p->setLabel("screen colour");
        p->setHint("Screen colour: pick it on a clean area of the screen in the viewer.");
        p->setDefault(0.1, 0.6, 0.15);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("plateMode");
        p->setLabel("clean plate");
        p->setHint("auto: the screen without the subject, rebuilt from the shot (uneven screens). plate: the plate input. "
                   "flat: the screen colour everywhere.");
        p->appendOption("auto");
        p->appendOption("plate");
        p->appendOption("flat");
        p->setDefault(APFXSK::ePlateAuto);
        p->setAnimates(false);
        page->addChild(*p);
    }
    intParam(desc, page, "plateSize", "plate size",
             "Auto plate: the subject is grown by this many pixels before the screen around it is filled in.", 10, 0, 200, 0, 50)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "plateSmooth", "smooth", "Blur of the auto plate (pixels).",
                                            4., 0., 20., 0., 200.) );
    // ---- input repair
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "chromaRebuild", "chroma rebuild",
                                                         "Compressed footage stores colour at half resolution (blocky colour "
                                                         "edges). Rebuilds it at full resolution from the luma before measuring.",
                                                         true);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
    }
    intParam(desc, page, "chromaRadius", "radius", "Rebuild radius: 1 for 4:2:2 / sharp sources, 2 for heavy 4:2:0.", 1, 1, 8, 1, 4);
    APFXOfx::defineDouble(desc, page, "screenDenoise", "screen denoise",
                          "Smooths noise and compression blocks on the screen only (edges and subject untouched).", 0.5, 0., 1., 0., 1.)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "denoiseSize", "size", "Screen denoise size (pixels).", 2., 0.5, 8., 0., 50.) );
    // ---- matte
    APFXOfx::defineDouble(desc, page, "screenGain", "screen gain",
                          "Higher: more of the screen removed (cleaner background, thinner edges).", 1.05, 0.5, 2., 0., 10.);
    APFXOfx::defineDouble(desc, page, "screenBalance", "screen balance",
                          "Tilts the colour difference between the two other colour directions (for green: towards red or "
                          "blue). 0 = neutral.", 0., -1., 1., -1., 1.);
    APFXOfx::defineDouble(desc, page, "clipBlack", "clip black", "Alpha below this becomes 0.", 0., 0., 1., 0., 1.)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "clipWhite", "clip white", "Alpha above this becomes 1.", 1., 0., 1., 0., 1.) );
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "core", "core matte",
                                                         "Adds a harder, shrunk matte inside the subject (solid core, soft edges "
                                                         "stay from the edge matte).", false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
    }
    APFXOfx::defineDouble(desc, page, "coreGain", "gain", "Screen gain of the core matte.", 1.5, 1., 4., 0., 10.)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::defineDouble(desc, page, "coreClipWhite", "clip white", "Clip white of the core matte.", 0.7, 0.1, 1., 0.01, 1.)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::divider( intParam(desc, page, "coreShrink", "shrink", "Core matte shrink (pixels).", 2, 0, 100, 0, 20) );
    // ---- halo
    APFXOfx::defineDouble(desc, page, "haloFlatten", "halo flatten",
                          "Removes the lighter (or darker) glow ring around the subject that generated images leave on the "
                          "screen: a real edge is a mix of subject and screen, the ring is lighter than that mix. Hair and "
                          "real soft edges stay.", 0., 0., 1., 0., 1.);
    intParam(desc, page, "haloWidth", "width", "How far outside the solid subject the ring reaches (pixels).", 8, 1, 200, 1, 30)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::defineDouble(desc, page, "haloDetail", "detail", "Smallest ring structure size (pixels): noise below this is not a ring.",
                          2., 0., 8., 0., 50.)->setLayoutHint(eLayoutHintNoNewLine, 16);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("ring");
        p->setLabel("ring");
        p->setHint("brighter: light rings (most common). darker: dark rings. both.");
        p->appendOption("brighter");
        p->appendOption("darker");
        p->appendOption("both");
        p->setDefault(APFXSK::eRingBright);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    // ---- refine
    APFXOfx::defineDouble(desc, page, "edgeSnap", "edge snap",
                          "Soft edges follow the image detail (guided by the luma): cleaner compressed edges. Solid and "
                          "clear areas are not touched.", 0., 0., 1., 0., 1.)->setLayoutHint(eLayoutHintNoNewLine, 16);
    intParam(desc, page, "snapRadius", "radius", "Edge snap radius (pixels).", 2, 1, 16, 1, 8);
    APFXOfx::defineDouble(desc, page, "shrinkGrow", "shrink / grow", "Negative shrinks, positive grows the matte (pixels, fractional).",
                          0., -5., 5., -100., 100.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "soften", "soften", "Blur of the final matte (pixels).", 0., 0., 5., 0., 100.) );
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "stabilise", "stabilise",
                                            "Video: averages the soft edges with the previous and next frames along the motion "
                                            "(less buzzing edges). Costs two more keys per frame (cached while playing).",
                                            0., 0., 1., 0., 1.) );
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
        p->setLabel("output");
        p->setHint("rgba: the source colours with the new alpha (unpremultiplied). premultiplied. alpha: the matte in all "
                   "channels. status: black = clear screen, white = solid, grey = in between, red = halo flattened. "
                   "screen matte: before halo / refine. clean plate. halo: removed ring in red over the matte.");
        p->appendOption("rgba");
        p->appendOption("premultiplied");
        p->appendOption("alpha");
        p->appendOption("status");
        p->appendOption("screen matte");
        p->appendOption("clean plate");
        p->appendOption("halo");
        p->setDefault(eOutRGBA);
        page->addChild(*p);
    }
}

ImageEffect*
APFXScreenKeyFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXScreenKeyPlugin(h);
}
} // anon namespace

static APFXScreenKeyFactory apfxScreenKeyFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxScreenKeyFactory)
