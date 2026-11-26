/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Crop: the box starts at the input's format when the node is first connected; in the viewer the
// corners, edges and centre can be dragged, and dragging outside the box draws a new one. softness,
// reformat (the box becomes the format, moved to the origin), intersect, black outside.

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cmath>
#include <algorithm>

#include "ofxsInteract.h"
#include "APFXOfxCommon.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Crop"

class APFXCropPlugin
    : public ImageEffect
{
public:
    APFXCropPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _bl = fetchDouble2DParam("boxBottomLeft");
        _tr = fetchDouble2DParam("boxTopRight");
        _soft = fetchDoubleParam("softness");
        _reformat = fetchBooleanParam("reformat");
        _intersect = fetchBooleanParam("intersect");
        _black = fetchBooleanParam("blackOutside");
        _init = fetchBooleanParam("boxInitialised");
    }

    void box(double t, double* x1, double* y1, double* x2, double* y2) const
    {
        double a, b, c, d;
        _bl->getValueAtTime(t, a, b);
        _tr->getValueAtTime(t, c, d);
        *x1 = std::min(a, c);
        *x2 = std::max(a, c);
        *y1 = std::min(b, d);
        *y2 = std::max(b, d);
    }

    void setBox(double x1, double y1, double x2, double y2)
    {
        _bl->setValue( std::min(x1, x2), std::min(y1, y2) );
        _tr->setValue( std::max(x1, x2), std::max(y1, y2) );
    }

    bool reformatOn() const { return _reformat->getValue(); }

private:
    // the box = the input's format (canonical)
    void matchInput()
    {
        if ( !_src->isConnected() ) {
            return;
        }
        OfxRectI f;
        _src->getFormat(f);
        const double par = _src->getPixelAspectRatio();
        OfxRectD r;
        if ( (f.x2 > f.x1) && (f.y2 > f.y1) ) {
            r.x1 = f.x1 * par;
            r.x2 = f.x2 * par;
            r.y1 = f.y1;
            r.y2 = f.y2;
        } else {
            r = _src->getRegionOfDefinition( timeLineGetTime() );
        }
        if ( (r.x2 <= r.x1) || (r.y2 <= r.y1) ) {
            return;
        }
        beginEditBlock("crop box");
        setBox(r.x1, r.y1, r.x2, r.y2);
        _init->setValue(true);
        endEditBlock();
    }

    void changedClip(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == kOfxImageEffectSimpleSourceClipName) && !_init->getValue() ) {
            matchInput();   // first connection: like a new crop on a selected node
        }
    }

    void changedParam(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if (n == "matchInput") {
            matchInput();
        } else if ( ( (n == "boxBottomLeft") || (n == "boxTopRight") ) && !_init->getValue() ) {
            _init->setValue(true);
        }
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        double x1, y1, x2, y2;
        box(args.time, &x1, &y1, &x2, &y2);
        rod.x1 = x1; rod.y1 = y1; rod.x2 = x2; rod.y2 = y2;
        if ( _intersect->getValueAtTime(args.time) && _src->isConnected() ) {
            const OfxRectD s = _src->getRegionOfDefinition(args.time);
            rod.x1 = std::max(rod.x1, s.x1);
            rod.y1 = std::max(rod.y1, s.y1);
            rod.x2 = std::min(rod.x2, s.x2);
            rod.y2 = std::min(rod.y2, s.y2);
            if ( (rod.x2 <= rod.x1) || (rod.y2 <= rod.y1) ) {
                rod.x2 = rod.x1;
                rod.y2 = rod.y1;
            }
        }
        if ( _reformat->getValueAtTime(args.time) ) {
            rod.x1 -= x1; rod.x2 -= x1;
            rod.y1 -= y1; rod.y2 -= y1;
        }
        if ( _black->getValueAtTime(args.time) ) {
            const double par = _src->isConnected() ? _src->getPixelAspectRatio() : 1.;
            rod.x1 -= par; rod.x2 += par;
            rod.y1 -= 1.; rod.y2 += 1.;
        }

        return true;
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        double x1, y1, x2, y2;
        box(args.time, &x1, &y1, &x2, &y2);
        OfxRectD r = args.regionOfInterest;
        if ( _reformat->getValueAtTime(args.time) ) {
            r.x1 += x1; r.x2 += x1;
            r.y1 += y1; r.y2 += y1;
        }
        r.x1 = std::max(r.x1, x1);
        r.y1 = std::max(r.y1, y1);
        r.x2 = std::min(r.x2, x2);
        r.y2 = std::min(r.y2, y2);
        if ( (r.x2 <= r.x1) || (r.y2 <= r.y1) ) {
            r.x2 = r.x1;
            r.y2 = r.y1;
        }
        rois.setRegionOfInterest(*_src, r);
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        if ( _reformat->getValue() ) {
            double x1, y1, x2, y2;
            box(timeLineGetTime(), &x1, &y1, &x2, &y2);
            const double par = _src->isConnected() ? _src->getPixelAspectRatio() : 1.;
            OfxRectI f = { 0, 0, (int)std::ceil( (x2 - x1) / par - 1e-6 ), (int)std::ceil(y2 - y1 - 1e-6) };
            cp.setOutputFormat(f);
        }
        cp.setOutputPremultiplication( _src->getPreMultiplication() );
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime
#ifdef OFX_EXTENSIONS_NATRON
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        if ( _reformat->getValueAtTime(args.time) || _black->getValueAtTime(args.time) || (_soft->getValueAtTime(args.time) > 0.) ) {
            return false;
        }
        double x1, y1, x2, y2;
        box(args.time, &x1, &y1, &x2, &y2);
        const OfxRectD s = _src->getRegionOfDefinition(args.time);
        if ( (x1 <= s.x1) && (y1 <= s.y1) && (x2 >= s.x2) && (y2 >= s.y2) ) {
            identityClip = _src;
            identityTime = args.time;

            return true;
        }

        return false;
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        std::unique_ptr<Image> dst( _dst->fetchImage(args.time) );
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        std::unique_ptr<const Image> src( _src->isConnected() ? _src->fetchImage(args.time) : NULL );
        const OfxRectI win = args.renderWindow;
        const OfxPointD rs = args.renderScale;
        const int nc = dst->getPixelComponentCount();
        double x1, y1, x2, y2;
        box(args.time, &x1, &y1, &x2, &y2);
        const double par = _src->isConnected() ? _src->getPixelAspectRatio() : 1.;
        const bool ref = _reformat->getValueAtTime(args.time);
        const double soft = std::max( 0., _soft->getValueAtTime(args.time) );
        // box in source pixels (render scale)
        const double bx1 = x1 * rs.x / par, bx2 = x2 * rs.x / par, by1 = y1 * rs.y, by2 = y2 * rs.y;
        const int ox = ref ? (int)std::floor(bx1 + 0.5) : 0, oy = ref ? (int)std::floor(by1 + 0.5) : 0;
        const double sx = soft * rs.x, sy = soft * rs.y;
        auto ramp = [](double d, double s) {
            if (d <= 0.) { return 0.; }
            if ( (s <= 0.) || (d >= s) ) { return 1.; }
            const double t = d / s;

            return t * t * (3. - 2. * t);
        };
        const APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        const int sc = src ? src->getPixelComponentCount() : 0;
        const OfxRectI sb = src ? src->getBounds() : OfxRectI{ 0, 0, 0, 0 };
        pf([&](int r0, int r1) {
            for (int y = win.y1 + r0; y < win.y1 + r1; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                const int syi = y + oy;
                const double cy = syi + 0.5;
                const double wy = std::min( ramp(cy - by1, sy), ramp(by2 - cy, sy) );
                for (int x = win.x1; x < win.x2; ++x, d += nc) {
                    const int sxi = x + ox;
                    const double cx = sxi + 0.5;
                    const double w = std::min( wy, std::min( ramp(cx - bx1, sx), ramp(bx2 - cx, sx) ) );
                    const float* s = ( src && (w > 0.) && (sxi >= sb.x1) && (sxi < sb.x2) && (syi >= sb.y1) && (syi < sb.y2) )
                                     ? (const float*)src->getPixelAddress(sxi, syi) : NULL;
                    for (int k = 0; k < nc; ++k) {
                        d[k] = s ? (float)(s[k < sc ? k : sc - 1] * w) : 0.f;
                    }
                }
            }
        }, win.y2 - win.y1);
    }

    Clip* _dst;
    Clip* _src;
    Double2DParam* _bl;
    Double2DParam* _tr;
    DoubleParam* _soft;
    BooleanParam* _reformat;
    BooleanParam* _intersect;
    BooleanParam* _black;
    BooleanParam* _init;
};

// viewer: drag corners / edges / centre; drag outside the box draws a new box
class CropInteract
    : public OverlayInteract
{
public:
    CropInteract(OfxInteractHandle h, ImageEffect* fx)
        : OverlayInteract(h), _fx( dynamic_cast<APFXCropPlugin*>(fx) ) {}

    enum Part { eNone = 0, eBL, eBR, eTR, eTL, eL, eR, eB, eT, eCentre, eNew };

    virtual bool draw(const DrawArgs& args) OVERRIDE FINAL
    {
        if ( !_fx || _fx->reformatOn() ) {
            return false;
        }
        double x1, y1, x2, y2;
        _fx->box(args.time, &x1, &y1, &x2, &y2);
        const double px = args.pixelScale.x, py = args.pixelScale.y;
        const double hs = 4.;
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glDisable(GL_LINE_STIPPLE);
        for (int pass = 0; pass < 2; ++pass) {
            const double o = pass == 0 ? 1. : 0.;
            auto col = [&](int part) {
                if (pass == 0) {
                    glColor3f(0.f, 0.f, 0.f);
                } else if ( (part != eNone) && ( (part == _hover) || (part == _drag) ) ) {
                    glColor3f(1.f, 0.824f, 0.341f);
                } else {
                    glColor3f(0.9f, 0.9f, 0.9f);
                }
            };
            glLineWidth(1.5f);
            col(eNone);
            glBegin(GL_LINE_LOOP);
            glVertex2d(x1 + o * px, y1 - o * py);
            glVertex2d(x2 + o * px, y1 - o * py);
            glVertex2d(x2 + o * px, y2 - o * py);
            glVertex2d(x1 + o * px, y2 - o * py);
            glEnd();
            // handles: corners, edge middles
            const double hx[8] = { x1, x2, x2, x1, x1, x2, 0.5 * (x1 + x2), 0.5 * (x1 + x2) };
            const double hy[8] = { y1, y1, y2, y2, 0.5 * (y1 + y2), 0.5 * (y1 + y2), y1, y2 };
            glPointSize(7.f);
            glBegin(GL_POINTS);
            for (int i = 0; i < 8; ++i) {
                col(eBL + i);
                glVertex2d(hx[i] + o * px, hy[i] - o * py);
            }
            glEnd();
            // centre cross
            col(eCentre);
            const double cx = 0.5 * (x1 + x2), cy = 0.5 * (y1 + y2);
            glBegin(GL_LINES);
            glVertex2d(cx - hs * 2 * px + o * px, cy - o * py);
            glVertex2d(cx + hs * 2 * px + o * px, cy - o * py);
            glVertex2d(cx + o * px, cy - hs * 2 * py - o * py);
            glVertex2d(cx + o * px, cy + hs * 2 * py - o * py);
            glEnd();
        }
        glPopAttrib();

        return true;
    }

    int hit(double t, const OfxPointD& p, const OfxPointD& ps) const
    {
        double x1, y1, x2, y2;
        _fx->box(t, &x1, &y1, &x2, &y2);
        const double rx = 8. * ps.x, ry = 8. * ps.y;
        const double hx[8] = { x1, x2, x2, x1, x1, x2, 0.5 * (x1 + x2), 0.5 * (x1 + x2) };
        const double hy[8] = { y1, y1, y2, y2, 0.5 * (y1 + y2), 0.5 * (y1 + y2), y1, y2 };
        for (int i = 0; i < 8; ++i) {
            if ( (std::fabs(p.x - hx[i]) < rx) && (std::fabs(p.y - hy[i]) < ry) ) {
                return eBL + i;
            }
        }
        const double cx = 0.5 * (x1 + x2), cy = 0.5 * (y1 + y2);
        if ( (std::fabs(p.x - cx) < 2 * rx) && (std::fabs(p.y - cy) < 2 * ry) ) {
            return eCentre;
        }
        // edges anywhere along their length
        const bool inY = (p.y > y1 - ry) && (p.y < y2 + ry), inX = (p.x > x1 - rx) && (p.x < x2 + rx);
        if ( inY && (std::fabs(p.x - x1) < rx) ) { return eL; }
        if ( inY && (std::fabs(p.x - x2) < rx) ) { return eR; }
        if ( inX && (std::fabs(p.y - y1) < ry) ) { return eB; }
        if ( inX && (std::fabs(p.y - y2) < ry) ) { return eT; }
        if ( (p.x < x1) || (p.x > x2) || (p.y < y1) || (p.y > y2) ) {
            return eNew;    // outside: draw a new box
        }

        return eNone;
    }

    virtual bool penMotion(const PenArgs& args) OVERRIDE FINAL
    {
        if ( !_fx || _fx->reformatOn() ) {
            return false;
        }
        if (_drag == eNone) {
            const int h = hit(args.time, args.penPosition, args.pixelScale);
            const int hv = (h == eNew) ? eNone : h;
            if (hv != _hover) {
                _hover = hv;
                requestRedraw();
            }

            return false;
        }
        const OfxPointD& p = args.penPosition;
        double x1 = _b[0], y1 = _b[1], x2 = _b[2], y2 = _b[3];
        const double dx = p.x - _start.x, dy = p.y - _start.y;
        switch (_drag) {
        case eBL: x1 += dx; y1 += dy; break;
        case eBR: x2 += dx; y1 += dy; break;
        case eTR: x2 += dx; y2 += dy; break;
        case eTL: x1 += dx; y2 += dy; break;
        case eL: x1 += dx; break;
        case eR: x2 += dx; break;
        case eB: y1 += dy; break;
        case eT: y2 += dy; break;
        case eCentre: x1 += dx; x2 += dx; y1 += dy; y2 += dy; break;
        case eNew: x1 = _start.x; y1 = _start.y; x2 = p.x; y2 = p.y; break;
        default: break;
        }
        // whole pixels
        _fx->setBox( std::floor(x1 + 0.5), std::floor(y1 + 0.5), std::floor(x2 + 0.5), std::floor(y2 + 0.5) );

        return true;
    }

    virtual bool penDown(const PenArgs& args) OVERRIDE FINAL
    {
        if ( !_fx || _fx->reformatOn() ) {
            return false;
        }
        const int h = hit(args.time, args.penPosition, args.pixelScale);
        if (h == eNone) {
            return false;
        }
        _drag = h;
        _start = args.penPosition;
        _fx->box(args.time, &_b[0], &_b[1], &_b[2], &_b[3]);
        _fx->beginEditBlock("crop box");   // one undo step per drag

        return true;
    }

    virtual bool penUp(const PenArgs&) OVERRIDE FINAL
    {
        if (_drag == eNone) {
            return false;
        }
        _drag = eNone;
        _fx->endEditBlock();
        requestRedraw();

        return true;
    }

private:
    APFXCropPlugin* _fx;
    int _hover = eNone, _drag = eNone;
    OfxPointD _start = { 0., 0. };
    double _b[4] = { 0., 0., 0., 0. };
};

class CropOverlayDescriptor
    : public DefaultEffectOverlayDescriptor<CropOverlayDescriptor, CropInteract>
{
};

mDeclarePluginFactory(APFXCropFactory, {}, {});

void
APFXCropFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Crop",
                            "APFX Crop: crops to a box. A new crop starts at the size of its input. In the viewer drag the "
                            "corners, edges or centre, or drag outside the box to draw a new one. softness fades the edges, "
                            "reformat makes the box the new format (moved to the origin), intersect keeps only what the "
                            "input covers, black outside adds a black pixel border.");
    desc.setPluginGrouping("Transform");
    desc.setSupportsTiles(true);
    desc.setSupportsMultipleClipPARs(false);
    desc.setOverlayInteractDescriptor(new CropOverlayDescriptor);
}

void
APFXCropFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setSupportsTiles(true);
    }
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectOutputClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setSupportsTiles(true);
    }
    PageParamDescriptor* page = desc.definePageParam("Crop");
    const char* names[2] = { "boxBottomLeft", "boxTopRight" };
    const char* labels[2] = { "box x / y", "r / t" };
    for (int i = 0; i < 2; ++i) {
        Double2DParamDescriptor* p = desc.defineDouble2DParam(names[i]);
        p->setLabel(labels[i]);
        p->setHint(i == 0 ? "Left and bottom of the box." : "Right and top of the box.");
        p->setDoubleType(eDoubleTypeXYAbsolute);
        p->setDefaultCoordinateSystem(eCoordinatesCanonical);
        p->setDefault(i == 0 ? 0. : 1920., i == 0 ? 0. : 1080.);
        p->setIncrement(1.);
        p->setDigits(0);
        if (i == 0) {
            p->setLayoutHint(eLayoutHintNoNewLine, 16);
        }
        page->addChild(*p);
        desc.addClipPreferencesSlaveParam(*p);   // reformat: the box is the output format
    }
    {
        PushButtonParamDescriptor* p = desc.definePushButtonParam("matchInput");
        p->setLabel("match input");
        p->setHint("Set the box to the input's format.");
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "softness", "softness", "Fades the edges inwards (pixels).", 0., 0., 100., 0., 10000.);
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "reformat", "reformat",
                                                        "The box becomes the output format (moved to the origin).", false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        desc.addClipPreferencesSlaveParam(*p);
    }
    APFXOfx::defineBool(desc, page, "intersect", "intersect", "Only what the input covers inside the box.", false)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::defineBool(desc, page, "blackOutside", "black outside",
                        "Adds a black pixel border so edge pixels do not repeat in later transforms.", true);
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("boxInitialised");
        p->setDefault(false);
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
}

ImageEffect*
APFXCropFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXCropPlugin(h);
}
} // anon namespace

static APFXCropFactory apfxCropFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxCropFactory)
