/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX PMatte: matte from a world position pass. A sphere, cube or plane is placed in world space
// (centre, rotate, scale); each pixel's position (Position input rgb = xyz, or the Source rgb when
// Position is not connected) is tested against it with a soft falloff. Ctrl+click in the viewer moves
// the centre to the world position under the cursor.

#include <cfloat>
#include <cmath>

#include "APFXOfxCommon.h"
#include "ofxsInteract.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.PMatte"

enum ShapeEnum { eShapeSphere = 0, eShapeCube, eShapePlane };
enum OutputEnum { eOutAlpha = 0, eOutAddAlpha, eOutMultiplyAlpha, eOutMatteRGBA, eOutPreview };

class PMatteInteract;

class APFXPMattePlugin
    : public ImageEffect
{
public:
    APFXPMattePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _pos = fetchClip("Position");
        _std.fetch(*this, true, false);
        _shape = fetchChoiceParam("shape");
        _center = fetchDouble3DParam("center");
        _rotate = fetchDouble3DParam("rotate");
        _scale = fetchDouble3DParam("scale");
        _uniform = fetchDoubleParam("uniformScale");
        _falloff = fetchDoubleParam("falloff");
        _gamma = fetchDoubleParam("falloffGamma");
        _invert = fetchBooleanParam("invert");
        _output = fetchChoiceParam("output");
        _pickPos = fetchDouble2DParam("pickPos");
        _pickTime = fetchDoubleParam("pickTime");
        _pickCount = fetchIntParam("pickCount");
    }

    // called by the interact (Ctrl+click): stored, then handled in changedParam (images can be fetched there)
    void requestPick(double time, const OfxPointD& pos)
    {
        beginEditBlock("pick position");
        _pickTime->setValue(time);
        _pickPos->setValue(pos.x, pos.y);
        _pickCount->setValue( _pickCount->getValue() + 1 );
        endEditBlock();
    }

private:
    Clip* posClip() { return _pos->isConnected() ? _pos : _src; }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) OVERRIDE FINAL
    {
        if (name != "pickCount") {
            return;
        }
        const double t = _pickTime->getValue();
        double px, py;
        _pickPos->getValue(px, py);
        Image* img = posClip()->fetchImage(t);
        if (!img) {
            return;
        }
        const int x = (int)std::floor(px * args.renderScale.x), y = (int)std::floor(py * args.renderScale.y);
        const float* p = (const float*)img->getPixelAddress(x, y);
        if ( p && (img->getPixelDepth() == eBitDepthFloat) && (img->getPixelComponentCount() >= 3) ) {
            _center->setValueAtTime(t, p[0], p[1], p[2]);
        }
        delete img;
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
        cp.setClipComponents(*_pos, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
    }

    // world -> shape local space: inverse of  T(center) * R(x, y, z) * S(scale)
    struct Xf
    {
        double inv[3][3];
        double c[3];
    };

    static Xf makeXf(const double* center, const double* rotDeg, const double* scale)
    {
        const double rx = rotDeg[0] * M_PI / 180., ry = rotDeg[1] * M_PI / 180., rz = rotDeg[2] * M_PI / 180.;
        const double cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry), cz = std::cos(rz), sz = std::sin(rz);
        // R = Rz * Ry * Rx (XYZ order)
        const double R[3][3] = {
            { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx },
            { sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx },
            { -sy, cy * sx, cy * cx }
        };
        Xf x;
        for (int i = 0; i < 3; ++i) {
            x.c[i] = center[i];
            const double s = std::fabs(scale[i]) > 1e-12 ? scale[i] : 1e-12;
            // inverse = S^-1 * R^T
            for (int j = 0; j < 3; ++j) {
                x.inv[i][j] = R[j][i] / s;
            }
        }

        return x;
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
        Image* pos = _pos->isConnected() ? _pos->fetchImage(t) : NULL;
        if ( pos && ( (pos->getPixelDepth() != eBitDepthFloat) || (pos->getPixelComponentCount() < 3) ) ) {
            delete pos;
            pos = NULL;
        }
        const int pstride = pos ? pos->getPixelComponentCount() : 4;
        double center[3], rot[3], scale[3];
        _center->getValueAtTime(t, center[0], center[1], center[2]);
        _rotate->getValueAtTime(t, rot[0], rot[1], rot[2]);
        _scale->getValueAtTime(t, scale[0], scale[1], scale[2]);
        const double u = _uniform->getValueAtTime(t);
        for (int i = 0; i < 3; ++i) {
            scale[i] *= u;
        }
        const Xf X = makeXf(center, rot, scale);
        const int shape = _shape->getValueAtTime(t);
        const double falloff = std::max( 0., std::min(1., _falloff->getValueAtTime(t) ) );
        const double gamma = std::max(0.01, _gamma->getValueAtTime(t) );
        const bool invert = _invert->getValueAtTime(t);
        const int out = _output->getValueAtTime(t);
        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        const OfxRectI win = args.renderWindow;
        APFXOfx::hostParallelFor()([&](int ya, int yb) {
            for (int y = win.y1 + ya; y < win.y1 + yb; ++y) {
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
                    const float* p = pos ? (const float*)pos->getPixelAddress(x, y) : s;
                    double m = 0.;
                    if (p) {
                        const double w[3] = { p[0] - X.c[0], p[1] - X.c[1], p[2] - X.c[2] };
                        double l[3];
                        for (int i = 0; i < 3; ++i) {
                            l[i] = X.inv[i][0] * w[0] + X.inv[i][1] * w[1] + X.inv[i][2] * w[2];
                        }
                        double dist; // 0 at the centre, 1 on the shape surface
                        if (shape == eShapeSphere) {
                            dist = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                        } else if (shape == eShapeCube) {
                            dist = std::max( std::fabs(l[0]), std::max( std::fabs(l[1]), std::fabs(l[2]) ) );
                        } else {
                            // plane through the centre, normal = local +Y: 1 above, falloff over scale.y below/above
                            dist = 1. - l[1];
                        }
                        if (shape == eShapePlane) {
                            m = std::max( 0., std::min(1., (falloff > 1e-9) ? (l[1] / falloff + 0.5) : (l[1] >= 0. ? 1. : 0.) ) );
                        } else {
                            const double inner = 1. - falloff;
                            m = dist <= inner ? 1. : ( dist >= 1. ? 0. : 1. - (dist - inner) / std::max(1e-9, falloff) );
                        }
                        (void)dist;
                        m = std::pow(m, gamma);
                    }
                    if (invert) {
                        m = 1. - m;
                    }
                    float mv = o.mix;
                    if (mask) {
                        const float* mp = (const float*)mask->getPixelAddress(x, y);
                        float v = mp ? *mp : 0.f;
                        if (o.maskInvert) {
                            v = 1.f - v;
                        }
                        mv *= std::max(0.f, std::min(1.f, v) );
                    }
                    const float mf = (float)m;
                    float r[4] = { s[0], s[1], s[2], s[3] };
                    switch (out) {
                    case eOutAlpha: r[3] = mf; break;
                    case eOutAddAlpha: r[3] = std::max(s[3], mf); break;
                    case eOutMultiplyAlpha: r[3] = s[3] * mf; break;
                    case eOutMatteRGBA: r[0] = r[1] = r[2] = r[3] = mf; break;
                    default: // preview: warm tint where the matte is
                        r[0] = s[0] * 0.6f + mf * 0.45f;
                        r[1] = s[1] * 0.6f + mf * 0.3f;
                        r[2] = s[2] * 0.6f;
                        break;
                    }
                    for (int c = 0; c < 4; ++c) {
                        d[c] = s[c] + (r[c] - s[c]) * mv;
                    }
                }
            }
        }, win.y2 - win.y1);
        (void)pstride;
        delete mask;
        delete pos;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    Clip* _pos;
    APFXOfx::StdParams _std;
    ChoiceParam* _shape;
    Double3DParam* _center;
    Double3DParam* _rotate;
    Double3DParam* _scale;
    DoubleParam* _uniform;
    DoubleParam* _falloff;
    DoubleParam* _gamma;
    BooleanParam* _invert;
    ChoiceParam* _output;
    Double2DParam* _pickPos;
    DoubleParam* _pickTime;
    IntParam* _pickCount;
};

class PMatteInteract
    : public OverlayInteract
{
public:
    PMatteInteract(OfxInteractHandle h, ImageEffect* fx)
        : OverlayInteract(h), _fx( dynamic_cast<APFXPMattePlugin*>(fx) ), _ctrl(false) {}

    virtual bool penDown(const PenArgs& args) OVERRIDE FINAL
    {
        if (!_ctrl || !_fx) {
            return false;
        }
        _fx->requestPick(args.time, args.penPosition);

        return true;
    }

    virtual bool keyDown(const KeyArgs& args) OVERRIDE FINAL
    {
        if ( (args.keySymbol == kOfxKey_Control_L) || (args.keySymbol == kOfxKey_Control_R) ) {
            _ctrl = true;
        }

        return false;
    }

    virtual bool keyUp(const KeyArgs& args) OVERRIDE FINAL
    {
        if ( (args.keySymbol == kOfxKey_Control_L) || (args.keySymbol == kOfxKey_Control_R) ) {
            _ctrl = false;
        }

        return false;
    }

    virtual void loseFocus(const FocusArgs& /*args*/) OVERRIDE FINAL { _ctrl = false; }

private:
    APFXPMattePlugin* _fx;
    bool _ctrl;
};

class PMatteOverlayDescriptor
    : public DefaultEffectOverlayDescriptor<PMatteOverlayDescriptor, PMatteInteract>
{
};

mDeclarePluginFactory(APFXPMatteFactory, {}, {});

void
APFXPMatteFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "PMatte",
                            "APFX PMatte: matte from a world position pass. Connect the position pass (rgb = world xyz) to "
                            "Position (or feed it as the Source). Place a sphere, cube or plane in world space with centre, "
                            "rotate and scale; falloff softens the edge. Ctrl+click in the viewer moves the centre to the world "
                            "position under the cursor. Output to alpha (replace, add or multiply), as a grey matte, or as a "
                            "preview tint.");
    desc.setPluginGrouping("Keyer");
    desc.setSupportsTiles(true);
    desc.setOverlayInteractDescriptor(new PMatteOverlayDescriptor);
}

void
APFXPMatteFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false)->setSupportsTiles(true);
    APFXOfx::defineRGBAClip(desc, "Position", true)->setSupportsTiles(true);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false)->setSupportsTiles(true);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("PMatte");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("shape");
        p->setLabel("shape");
        p->appendOption("sphere");
        p->appendOption("cube");
        p->appendOption("plane", "Everything above a plane through the centre (local +Y up).");
        p->setDefault(eShapeSphere);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        Double3DParamDescriptor* p = desc.defineDouble3DParam("center");
        p->setLabel("center");
        p->setHint("World position of the shape (Ctrl+click in the viewer to pick it).");
        p->setDefault(0., 0., 0.);
        p->setDisplayRange(-10., -10., -10., 10., 10., 10.);
        page->addChild(*p);
    }
    {
        Double3DParamDescriptor* p = desc.defineDouble3DParam("rotate");
        p->setLabel("rotate");
        p->setHint("Rotation in degrees (X, Y, Z).");
        p->setDefault(0., 0., 0.);
        p->setDisplayRange(-180., -180., -180., 180., 180., 180.);
        page->addChild(*p);
    }
    {
        Double3DParamDescriptor* p = desc.defineDouble3DParam("scale");
        p->setLabel("scale");
        p->setDefault(1., 1., 1.);
        p->setDisplayRange(0., 0., 0., 10., 10., 10.);
        page->addChild(*p);
    }
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "uniformScale", "uniform scale", "Size of the shape in world units.", 1., 0., 10., 0., 1e9) );
    APFXOfx::defineDouble(desc, page, "falloff", "falloff", "Soft edge: part of the size that fades out (plane: fade distance).", 0.3, 0., 1., 0., 1e6);
    APFXOfx::defineDouble(desc, page, "falloffGamma", "falloff gamma", "Shapes the falloff curve.", 1., 0.2, 4., 0.01, 100.);
    APFXOfx::defineBool(desc, page, "invert", "invert", "Matte outside the shape.", false);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
        p->setLabel("output");
        p->appendOption("alpha", "The matte replaces the alpha.");
        p->appendOption("add to alpha", "Max of the alpha and the matte.");
        p->appendOption("multiply alpha", "Alpha times the matte.");
        p->appendOption("matte (rgba)", "The matte in all channels.");
        p->appendOption("preview", "Tints the image where the matte is.");
        p->setDefault(eOutAlpha);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    // hidden pick data (interact -> changedParam)
    {
        Double2DParamDescriptor* p = desc.defineDouble2DParam("pickPos");
        p->setIsSecretAndDisabled(true);
        p->setEvaluateOnChange(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("pickTime");
        p->setIsSecretAndDisabled(true);
        p->setEvaluateOnChange(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("pickCount");
        p->setIsSecretAndDisabled(true);
        p->setEvaluateOnChange(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXPMatteFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXPMattePlugin(h);
}
} // anon namespace

static APFXPMatteFactory apfxPMatteFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxPMatteFactory)
