/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX ZDefocus: depth based defocus (Nuke ZDefocus-like), layered.
// The blur radius comes from the depth (math: depth / far = 0 / direct) around a focal plane with
// a depth of field. The image is split in N radius layers (soft assignment), each layer is blurred
// with the APFX convex bokeh kernel (disc / blades, O(height) per pixel) and the layers are
// composited back to front (far layers, in focus, then near layers over everything).

#include <cfloat>
#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXDefocusMath.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.ZDefocus"

enum MathEnum { eMathDepth = 0, eMathFarZero, eMathDirect };
enum OutputEnum { eOutResult = 0, eOutFocalPlane, eOutRadius };

class APFXZDefocusPlugin
    : public ImageEffect
{
public:
    APFXZDefocusPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _depth = fetchClip("Depth");
        _std.fetch(*this, true, false);
        _depthChan = fetchChoiceParam("depthChannel");
        _math = fetchChoiceParam("math");
        _focal = fetchDoubleParam("focalPlane");
        _dof = fetchDoubleParam("depthOfField");
        _size = fetchDoubleParam("size");
        _max = fetchDoubleParam("maximum");
        _layers = fetchIntParam("layers");
        _blades = fetchIntParam("blades");
        _rotation = fetchDoubleParam("rotation");
        _roundness = fetchDoubleParam("roundness");
        _aspect = fetchDoubleParam("aspect");
        _output = fetchChoiceParam("output");
    }

private:
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
        const double m = _max->getValueAtTime(args.time) + 2.;
        OfxRectD r = args.regionOfInterest;
        r.x1 -= m * std::max(1., _aspect->getValueAtTime(args.time) ); r.x2 += m * std::max(1., _aspect->getValueAtTime(args.time) );
        r.y1 -= m; r.y2 += m;
        rois.setRegionOfInterest(*_src, r);
        if ( _depth->isConnected() ) {
            rois.setRegionOfInterest(*_depth, r);
        }
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_depth, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
    }

    // signed radius (pixels at full res): < 0 near (in front of the focal plane), > 0 far
    inline double radiusOf(double z, int math, double focal, double dof, double size, double mx) const
    {
        double d;
        if (math == eMathDirect) {
            d = z;
        } else {
            const double zz = (math == eMathFarZero) ? ( std::fabs(z) > 1e-9 ? 1. / z : 1e9 ) : z;
            const double f = std::max(1e-6, focal);
            double dz = zz - f;
            const double half = 0.5 * std::max(0., dof);
            dz = (dz > 0.) ? std::max(0., dz - half) : std::min(0., dz + half);
            d = dz / std::max(1e-6, std::fabs(zz) ); // circle of confusion ~ |z - f| / z
        }
        const double r = d * size;

        return std::max( -mx, std::min(mx, r) );
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        const double t = args.time;
        Image* dst = _dst->fetchImage(t);
        Image* src = _src->isConnected() ? _src->fetchImage(t) : NULL;
        Image* dep = _depth->isConnected() ? _depth->fetchImage(t) : NULL;
        if ( !dst || !src || (src->getPixelComponents() != ePixelComponentRGBA) || (src->getPixelDepth() != eBitDepthFloat) ) {
            delete dep;
            delete src;
            delete dst;
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        if ( dep && ( (dep->getPixelComponents() != ePixelComponentRGBA) || (dep->getPixelDepth() != eBitDepthFloat) ) ) {
            delete dep;
            dep = NULL;
        }
        const OfxRectI win = args.renderWindow;
        const double rs = args.renderScale.y;
        const int math = _math->getValueAtTime(t);
        const double focal = _focal->getValueAtTime(t), dof = _dof->getValueAtTime(t);
        const double size = _size->getValueAtTime(t) * rs, mx = std::max(0., _max->getValueAtTime(t) * rs);
        const int dch = _depthChan->getValueAtTime(t);
        const int out = _output->getValueAtTime(t);
        const int nl = std::max( 2, std::min(64, _layers->getValueAtTime(t) ) );

        // working buffer: window grown by the max radius, clipped to the source bounds
        const OfxRectI sb = src->getBounds();
        const int grow = (int)std::ceil( mx * std::max( 1., _aspect->getValueAtTime(t) ) ) + 2;
        const int bx1 = std::max(sb.x1, win.x1 - grow), bx2 = std::min(sb.x2, win.x2 + grow);
        const int by1 = std::max(sb.y1, win.y1 - grow), by2 = std::min(sb.y2, win.y2 + grow);
        const int bw = std::max(0, bx2 - bx1), bh = std::max(0, by2 - by1);
        std::vector<float> rad( (size_t)bw * bh, 0.f );
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                float z = (float)focal;
                if (dep) {
                    const float* p = (const float*)dep->getPixelAddress(bx1 + x, by1 + y);
                    if (p) {
                        z = p[dch];
                    }
                } else {
                    const float* p = (const float*)src->getPixelAddress(bx1 + x, by1 + y);
                    if (p) {
                        z = p[3]; // no Depth input: alpha as depth (direct maths useful for masks)
                    }
                }
                rad[(size_t)y * bw + x] = (float)radiusOf(z, math, focal, dof, size, mx);
            }
        }
        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);

        if (out != eOutResult) {
            // QC: focal plane (red near, green in focus, blue far) or radius / max
            for (int y = win.y1; y < win.y2; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                for (int x = win.x1; d && x < win.x2; ++x, d += 4) {
                    const float* s = (const float*)src->getPixelAddress(x, y);
                    const bool inside = (x >= bx1) && (x < bx2) && (y >= by1) && (y < by2);
                    const float r = inside ? rad[(size_t)(y - by1) * bw + (x - bx1)] : 0.f;
                    const float l = s ? 0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2] : 0.f;
                    if (out == eOutFocalPlane) {
                        const float k = 0.5f;
                        d[0] = l * (1.f - k) + (r < -0.5f ? k : 0.f);
                        d[1] = l * (1.f - k) + (std::fabs(r) <= 0.5f ? k : 0.f);
                        d[2] = l * (1.f - k) + (r > 0.5f ? k : 0.f);
                    } else {
                        d[0] = d[1] = d[2] = mx > 0. ? std::fabs(r) / (float)mx : 0.f;
                    }
                    d[3] = s ? s[3] : 0.f;
                }
            }
            delete mask;
            delete dep;
            delete src;
            delete dst;

            return;
        }

        // layers: radii r_k = mx * k / (nl - 1), near (negative) and far (positive) separately
        APFXDefocus::ShapeParams sp;
        sp.blades = _blades->getValueAtTime(t);
        sp.rotation = _rotation->getValueAtTime(t);
        sp.roundness = _roundness->getValueAtTime(t);
        sp.aspect = _aspect->getValueAtTime(t);
        const int ww = win.x2 - win.x1, wh = win.y2 - win.y1;
        std::vector<float> accum( (size_t)ww * wh * 4, 0.f );
        // composite order: far (largest radius first) ... in focus ... near (smallest first, largest last)
        std::vector<int> order;
        for (int k = nl - 1; k >= 1; --k) {
            order.push_back(k);       // far
        }
        order.push_back(0);           // in focus
        for (int k = 1; k < nl; ++k) {
            order.push_back(-k);      // near
        }
        const double step = mx / std::max(1, nl - 1);
        for (int li : order) {
            const int k = std::abs(li);
            const double rk = step * k;
            // soft membership of each pixel in this layer (triangle in radius)
            APFXDefocus::Buffer B;
            B.allocate(bx1, by1, bw, bh);
            bool any = false;
            for (int y = 0; y < bh; ++y) {
                float* v = B.row(y);
                for (int x = 0; x < bw; ++x, v += 4) {
                    const float r = rad[(size_t)y * bw + x];
                    // hard assignment to the nearest layer (keeps occlusion correct when compositing over)
                    const double a = std::fabs(r);
                    int lk = step > 0. ? (int)std::floor(a / step + 0.5) : 0;
                    lk = std::min(nl - 1, lk);
                    const int sl = (lk == 0) ? 0 : (r > 0.f ? lk : -lk);
                    const float m = (sl == li) ? 1.f : 0.f;
                    if (m > 0.f) {
                        const float* s = (const float*)src->getPixelAddress(bx1 + x, by1 + y);
                        if (s) {
                            for (int c = 0; c < 4; ++c) {
                                v[c] = s[c] * m;
                            }
                            any = true;
                        }
                    }
                }
            }
            if (!any) {
                continue;
            }
            B.prefixRows(0, bh);
            sp.radius = rk;
            const APFXDefocus::Kernel K = APFXDefocus::buildKernel(sp);
            const float inv = (float)(1. / K.area);
            APFXOfx::hostParallelFor()([&](int ya, int yb) {
                std::vector<float> acc( (size_t)ww * 4 );
                for (int y = win.y1 + ya; y < win.y1 + yb; ++y) {
                    APFXDefocus::convolveRow(B, K, false, y, win.x1, win.x2, acc.data() );
                    float* a = &accum[(size_t)(y - win.y1) * ww * 4];
                    for (int x = 0; x < ww; ++x) {
                        const float* l = &acc[(size_t)x * 4];
                        const float la = l[3] * inv;
                        // layer over the accumulated result
                        for (int c = 0; c < 4; ++c) {
                            a[x * 4 + c] = l[c] * inv + a[x * 4 + c] * std::max(0.f, 1.f - la);
                        }
                    }
                }
            }, wh);
        }
        // normalise by the accumulated coverage of an opaque image (keeps solid areas solid)
        for (int y = win.y1; y < win.y2; ++y) {
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (!d) {
                continue;
            }
            const float* a = &accum[(size_t)(y - win.y1) * ww * 4];
            for (int x = win.x1; x < win.x2; ++x, d += 4, a += 4) {
                const float* s = (const float*)src->getPixelAddress(x, y);
                float mv = o.mix;
                if (mask) {
                    const float* mp = (const float*)mask->getPixelAddress(x, y);
                    float v = mp ? *mp : 0.f;
                    if (o.maskInvert) {
                        v = 1.f - v;
                    }
                    mv *= std::max(0.f, std::min(1.f, v) );
                }
                for (int c = 0; c < 4; ++c) {
                    const float sv = s ? s[c] : 0.f;
                    d[c] = sv + (a[c] - sv) * mv;
                }
            }
        }
        delete mask;
        delete dep;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    Clip* _depth;
    APFXOfx::StdParams _std;
    ChoiceParam* _depthChan;
    ChoiceParam* _math;
    DoubleParam* _focal;
    DoubleParam* _dof;
    DoubleParam* _size;
    DoubleParam* _max;
    IntParam* _layers;
    IntParam* _blades;
    DoubleParam* _rotation;
    DoubleParam* _roundness;
    DoubleParam* _aspect;
    ChoiceParam* _output;
};

mDeclarePluginFactory(APFXZDefocusFactory, {}, {});

void
APFXZDefocusFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "ZDefocus",
                            "APFX ZDefocus: depth based defocus. Connect the depth pass to Depth (choose its channel) and set the "
                            "focal plane (use output 'focal plane' to see near red / in focus green / far blue). Maths: depth = "
                            "distance, far = 0 = 1/z depth, direct = the depth value is the blur size. The image is defocused in "
                            "layers with the APFX bokeh (disc or blades) and composited back to front. Premultiplied input.");
    desc.setPluginGrouping("Filter");
}

void
APFXZDefocusFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, "Depth", true);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("ZDefocus");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("depthChannel");
        p->setLabel("depth channel");
        p->appendOption("red");
        p->appendOption("green");
        p->appendOption("blue");
        p->appendOption("alpha");
        p->setDefault(0);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("math");
        p->setLabel("math");
        p->appendOption("depth", "The depth value is the distance from the camera.");
        p->appendOption("far = 0", "The depth value is 1/z (far = 0), as most renderers' normalised depth.");
        p->appendOption("direct", "The depth value is the blur size (times size).");
        p->setDefault(eMathDepth);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "focalPlane", "focal plane", "Distance in focus (same units as the depth).", 1., 0., 100., -1e9, 1e9);
    APFXOfx::defineDouble(desc, page, "depthOfField", "depth of field", "Range around the focal plane that stays sharp.", 0., 0., 10., 0., 1e9);
    APFXOfx::defineDouble(desc, page, "size", "size", "Blur size scale (pixels).", 20., 0., 100., 0., 10000.);
    APFXOfx::defineDouble(desc, page, "maximum", "maximum", "Largest blur radius (pixels).", 30., 0., 100., 0., 1000.);
    {
        IntParamDescriptor* p = desc.defineIntParam("layers");
        p->setLabel("layers");
        p->setHint("Number of blur layers (more = smoother transitions, slower).");
        p->setDefault(16);
        p->setRange(2, 64);
        p->setDisplayRange(2, 32);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("blades");
        p->setLabel("blades");
        p->setHint("0 = disc, else aperture blades.");
        p->setDefault(0);
        p->setRange(0, 16);
        p->setDisplayRange(0, 12);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "rotation", "rotation", "Blade rotation (degrees).", 0., -180., 180.);
    APFXOfx::defineDouble(desc, page, "roundness", "roundness", "0 = straight blades, 1 = disc.", 0., 0., 1., 0., 1.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "aspect", "aspect", "Bokeh width / height (anamorphic < 1).", 1., 0.3, 2., 0.05, 20.) );
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
        p->setLabel("output");
        p->appendOption("result");
        p->appendOption("focal plane", "Near red, in focus green, far blue over the image.");
        p->appendOption("radius", "Blur radius / maximum.");
        p->setDefault(eOutResult);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXZDefocusFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXZDefocusPlugin(h);
}
} // anon namespace

static APFXZDefocusFactory apfxZDefocusFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxZDefocusFactory)
