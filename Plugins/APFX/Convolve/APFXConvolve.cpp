/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Convolve: convolves the Source with any image (Filter input) through FFT, like Nuke's
// Convolve: image bokeh, streaks, custom glows. The filter is centred on its format centre,
// optionally normalised (energy kept), per channel (rgb), by luminance or by alpha.

#include <cfloat>
#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXFFT.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Convolve"

enum FilterChanEnum { eFilterRGB = 0, eFilterLuminance, eFilterAlpha };

class APFXConvolvePlugin
    : public ImageEffect
{
public:
    APFXConvolvePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _flt = fetchClip("Filter");
        _std.fetch(*this, true, true);
        _chan = fetchChoiceParam("filterChannel");
        _normalize = fetchBooleanParam("normalize");
        _gain = fetchDoubleParam("gain");
        _grow = fetchBooleanParam("growBBox");
    }

private:
    bool kernelSize(double t, const OfxPointD& rs, int* kw, int* kh)
    {
        if ( !_flt->isConnected() ) {
            return false;
        }
        const OfxRectD r = _flt->getRegionOfDefinition(t);
        *kw = (int)std::ceil( (r.x2 - r.x1) * rs.x );
        *kh = (int)std::ceil( (r.y2 - r.y1) * rs.y );

        return (*kw > 0) && (*kh > 0);
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        rod = _src->getRegionOfDefinition(args.time);
        if ( _grow->getValueAtTime(args.time) && _flt->isConnected() ) {
            const OfxRectD f = _flt->getRegionOfDefinition(args.time);
            const double hw = 0.5 * (f.x2 - f.x1), hh = 0.5 * (f.y2 - f.y1);
            rod.x1 -= hw; rod.x2 += hw; rod.y1 -= hh; rod.y2 += hh;
        }

        return true;
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        // the whole source and the whole filter (FFT works on full images)
        if ( _src->isConnected() ) {
            rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(args.time) );
        }
        if ( _flt->isConnected() ) {
            rois.setRegionOfInterest( *_flt, _flt->getRegionOfDefinition(args.time) );
        }
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_flt, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        const double t = args.time;
        Image* dst = _dst->fetchImage(t);
        Image* src = _src->isConnected() ? _src->fetchImage(t) : NULL;
        Image* flt = _flt->isConnected() ? _flt->fetchImage(t) : NULL;
        if ( !dst || !src || (src->getPixelComponents() != ePixelComponentRGBA) || (src->getPixelDepth() != eBitDepthFloat) ) {
            delete flt;
            delete src;
            delete dst;
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        if ( flt && ( (flt->getPixelComponents() != ePixelComponentRGBA) || (flt->getPixelDepth() != eBitDepthFloat) ) ) {
            delete flt;
            flt = NULL;
        }
        APFXOps::Plane sp;
        const OfxRectI sb = APFXOfx::imageToPlane(src, sp);
        APFXOps::Plane res = sp;
        const OfxRectI win = args.renderWindow;
        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        if (flt) {
            APFXOps::Plane fp;
            const OfxRectI fb = APFXOfx::imageToPlane(flt, fp);
            const int kw = fb.x2 - fb.x1, kh = fb.y2 - fb.y1;
            // the kernel centre = centre of the filter's region of definition
            const OfxRectD fr = _flt->getRegionOfDefinition(t);
            const int kcx = (int)std::floor( (fr.x1 + fr.x2) * 0.5 * args.renderScale.x ) - fb.x1;
            const int kcy = (int)std::floor( (fr.y1 + fr.y2) * 0.5 * args.renderScale.y ) - fb.y1;
            // output buffer covers the render window; input = source; FFT size covers both + kernel
            const int ox1 = std::min(win.x1, sb.x1), oy1 = std::min(win.y1, sb.y1);
            const int ox2 = std::max(win.x2, sb.x2), oy2 = std::max(win.y2, sb.y2);
            const int W = APFXFFT::nextPow2(ox2 - ox1 + kw), H = APFXFFT::nextPow2(oy2 - oy1 + kh);
            const int chanMode = _chan->getValueAtTime(t);
            const bool norm = _normalize->getValueAtTime(t);
            const float gain = (float)_gain->getValueAtTime(t);
            res.resize(ox2 - ox1, oy2 - oy1, 4);
            APFXFFT::PF pf = APFXOfx::hostParallelFor();
            std::vector<APFXFFT::cf> A( (size_t)W * H ), K( (size_t)W * H );
            for (int c = 0; c < 4; ++c) {
                if ( !o.proc[c] ) {
                    continue;
                }
                // kernel channel
                std::fill( K.begin(), K.end(), APFXFFT::cf(0.f, 0.f) );
                double ksum = 0.;
                for (int y = 0; y < kh; ++y) {
                    for (int x = 0; x < kw; ++x) {
                        const float* p = fp.px(x, y);
                        float v;
                        if (chanMode == eFilterLuminance) {
                            v = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                        } else if (chanMode == eFilterAlpha) {
                            v = p[3];
                        } else {
                            v = (c < 3) ? p[c] : p[3];
                        }
                        ksum += v;
                        // wrap so the kernel centre lands on (0, 0)
                        const int kx = ( (x - kcx) % W + W ) % W, ky = ( (y - kcy) % H + H ) % H;
                        K[(size_t)ky * W + kx] = APFXFFT::cf(v, 0.f);
                    }
                }
                const float kscale = gain * (float)( norm && std::fabs(ksum) > 1e-12 ? 1. / ksum : 1. );
                std::fill( A.begin(), A.end(), APFXFFT::cf(0.f, 0.f) );
                for (int y = sb.y1; y < sb.y2; ++y) {
                    for (int x = sb.x1; x < sb.x2; ++x) {
                        A[(size_t)(y - oy1) * W + (x - ox1)] = APFXFFT::cf(sp.px(x - sb.x1, y - sb.y1)[c], 0.f);
                    }
                }
                APFXFFT::fft2d(A, W, H, false, pf);
                APFXFFT::fft2d(K, W, H, false, pf);
                const float inv = kscale / ( (float)W * (float)H );
                pf([&](int a, int b) {
                    for (int i = a; i < b; ++i) {
                        A[i] *= K[i] * inv;
                    }
                }, W * H);
                APFXFFT::fft2d(A, W, H, true, pf);
                for (int y = 0; y < res.h; ++y) {
                    for (int x = 0; x < res.w; ++x) {
                        res.px(x, y)[c] = A[(size_t)y * W + x].real();
                    }
                }
            }
            // unprocessed channels: copy the source
            for (int y = 0; y < res.h; ++y) {
                for (int x = 0; x < res.w; ++x) {
                    const float* s = APFXOfx::planeAt(sp, sb, ox1 + x, oy1 + y);
                    for (int c = 0; c < 4; ++c) {
                        if ( !o.proc[c] ) {
                            res.px(x, y)[c] = s ? s[c] : 0.f;
                        }
                    }
                }
            }
            const OfxRectI rb = { ox1, oy1, ox2, oy2 };
            APFXOfx::writeOutput(*this, dst, win, res, rb, &sp, sb, o);
        } else {
            APFXOfx::writeOutput(*this, dst, win, sp, sb, &sp, sb, o);
        }
        delete mask;
        delete flt;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    Clip* _flt;
    APFXOfx::StdParams _std;
    ChoiceParam* _chan;
    BooleanParam* _normalize;
    DoubleParam* _gain;
    BooleanParam* _grow;
};

mDeclarePluginFactory(APFXConvolveFactory, {}, {});

void
APFXConvolveFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Convolve",
                            "APFX Convolve: convolves the Source with the Filter image (FFT), like Nuke's Convolve. Use any image "
                            "as the kernel: image bokeh, streaks, custom glows. The filter is centred on its format centre; "
                            "normalize keeps the brightness. Works on premultiplied images.");
    desc.setPluginGrouping("Filter");
}

void
APFXConvolveFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, "Filter", true);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("Convolve");
    APFXOfx::defineChannels(desc, page, true);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("filterChannel");
        p->setLabel("filter channel");
        p->appendOption("rgb", "Each channel uses the matching filter channel (coloured bokeh).");
        p->appendOption("luminance", "All channels use the filter luminance.");
        p->appendOption("alpha", "All channels use the filter alpha.");
        p->setDefault(eFilterRGB);
        page->addChild(*p);
    }
    APFXOfx::defineBool(desc, page, "normalize", "normalize", "Divide the filter by its sum so the image keeps its brightness.", true);
    APFXOfx::defineDouble(desc, page, "gain", "gain", "Multiplies the result.", 1., 0., 4., 0., 1000.);
    APFXOfx::divider( APFXOfx::defineBool(desc, page, "growBBox", "grow bbox", "Grow the bounding box by half the filter size.", false) );
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXConvolveFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXConvolvePlugin(h);
}
} // anon namespace

static APFXConvolveFactory apfxConvolveFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxConvolveFactory)
