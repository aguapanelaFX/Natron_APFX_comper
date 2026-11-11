/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Look: one-node look, all maths in one float pass (per pixel, tiles OK).
//  basic:    white balance (temperature / tint), exposure, contrast (around a pivot),
//            highlights / shadows / whites / blacks, saturation
//  creative: faded film, vibrance, shadow tint / highlight tint + balance
//  wheels:   shadows / midtones / highlights colour offsets (lift / gamma / gain style)
//  lock:     save / load look presets (text files) and export the colour part as a .cube 3D LUT

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "APFXOfxCommon.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Look"

struct LookParams
{
    double temp = 0, tint = 0, exposure = 0, contrast = 0, pivot = 0.18;
    double highlights = 0, shadows = 0, whites = 0, blacks = 0, saturation = 1;
    double fade = 0, vibrance = 0;
    double shTint[3] = { 0, 0, 0 }, hiTint[3] = { 0, 0, 0 }, balance = 0;
    double lift[3] = { 0, 0, 0 }, gammaW[3] = { 0, 0, 0 }, gain[3] = { 0, 0, 0 };
};

inline double
luma(const double* c)
{
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

inline double
smooth(double a, double b, double x)
{
    const double t = std::max( 0., std::min(1., (x - a) / (b - a) ) );

    return t * t * (3. - 2. * t);
}

// the whole look on one linear RGB value (scene linear in, scene linear out)
inline void
applyLook(const LookParams& P, double* c)
{
    // white balance: temperature = blue <-> amber, tint = green <-> magenta (gains, luminance kept)
    {
        const double k = P.temp * 0.3, m = P.tint * 0.3;
        double g[3] = { 1. + k, 1. - m * 0.5, 1. - k };
        g[0] *= 1. + m * 0.25;
        g[2] *= 1. + m * 0.25;
        const double norm = 0.2126 * g[0] + 0.7152 * g[1] + 0.0722 * g[2];
        for (int i = 0; i < 3; ++i) {
            c[i] *= g[i] / norm;
        }
    }
    // exposure (stops)
    const double e = std::pow(2., P.exposure);
    for (int i = 0; i < 3; ++i) {
        c[i] *= e;
    }
    // tone controls on luminance in a log-like space (stops around the pivot)
    const double piv = std::max(1e-4, P.pivot);
    const double L = std::max( 1e-6, luma(c) );
    double s = std::log2(L / piv);                    // stops from the pivot
    s *= 1. + P.contrast;                             // contrast
    const double hiW = smooth(0., 3., s), shW = smooth(0., 3., -s);
    s += P.highlights * 1.5 * hiW * (1. - smooth(3., 6., s) );   // highlights (not the extreme whites)
    s += P.shadows * 1.5 * shW * (1. - smooth(3., 6., -s) );      // shadows (not the deepest blacks)
    s += P.whites * 1.5 * smooth(1.5, 5., s);                       // whites (top end)
    s += P.blacks * 1.5 * smooth(1.5, 5., -s);                      // blacks (bottom end)
    const double L2 = piv * std::pow(2., s);
    const double r = L2 / L;
    for (int i = 0; i < 3; ++i) {
        c[i] *= r;
    }
    // colour wheels: lift (shadows), gamma (mids), gain (highlights) as colour offsets
    {
        const double l = std::max( 0., luma(c) );
        const double ws = 1. - smooth(0., 0.4, l), wh = smooth(0.18, 1.5, l), wm = std::max(0., 1. - ws - wh);
        for (int i = 0; i < 3; ++i) {
            c[i] += P.lift[i] * 0.1 * ws;
            c[i] *= 1. + P.gammaW[i] * 0.5 * wm;
            c[i] *= 1. + P.gain[i] * 0.5 * wh;
        }
    }
    // saturation + vibrance (vibrance boosts low-saturation colours more)
    {
        const double l = luma(c);
        const double mx = std::max( c[0], std::max(c[1], c[2]) ), mn = std::min( c[0], std::min(c[1], c[2]) );
        const double sat = mx > 1e-6 ? (mx - mn) / mx : 0.;
        const double v = 1. + P.vibrance * (1. - sat);
        const double k = P.saturation * std::max(0., v);
        for (int i = 0; i < 3; ++i) {
            c[i] = l + (c[i] - l) * k;
        }
    }
    // split toning: shadow / highlight tint, balance moves the split point
    {
        const double l = std::max( 0., luma(c) );
        const double split = 0.18 * std::pow(2., P.balance * 2.);
        const double ws = 1. - smooth(0., split * 2., l), wh = smooth(split * 0.5, split * 6., l);
        for (int i = 0; i < 3; ++i) {
            c[i] += (P.shTint[i] * ws * 0.05) + (P.hiTint[i] * wh * 0.1 * std::max(l, 0.1) );
        }
    }
    // faded film: lifts the blacks and softens the top
    if (P.fade > 0.) {
        const double f = P.fade;
        for (int i = 0; i < 3; ++i) {
            const double v = c[i];
            c[i] = v + f * 0.04 * (1. - smooth(0., 0.25, v) ) - f * 0.15 * v * smooth(0.5, 2., v);
        }
    }
}

class APFXLookPlugin
    : public ImageEffect
{
public:
    APFXLookPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true, false);
        const char* d[] = { "temperature", "tint", "exposure", "contrast", "pivot", "highlights", "shadows", "whites", "blacks",
                            "saturation", "fade", "vibrance", "balance" };
        for (const char* n : d) {
            _d[n] = fetchDoubleParam(n);
        }
        const char* rgb[] = { "shadowTint", "highlightTint", "lift", "gammaWheel", "gain" };
        for (const char* n : rgb) {
            _rgb[n] = fetchRGBParam(n);
        }
        _file = fetchStringParam("lookFile");
        _cubeSize = fetchIntParam("cubeSize");
    }

private:
    LookParams params(double t)
    {
        LookParams P;
        P.temp = _d["temperature"]->getValueAtTime(t);
        P.tint = _d["tint"]->getValueAtTime(t);
        P.exposure = _d["exposure"]->getValueAtTime(t);
        P.contrast = _d["contrast"]->getValueAtTime(t);
        P.pivot = _d["pivot"]->getValueAtTime(t);
        P.highlights = _d["highlights"]->getValueAtTime(t);
        P.shadows = _d["shadows"]->getValueAtTime(t);
        P.whites = _d["whites"]->getValueAtTime(t);
        P.blacks = _d["blacks"]->getValueAtTime(t);
        P.saturation = _d["saturation"]->getValueAtTime(t);
        P.fade = _d["fade"]->getValueAtTime(t);
        P.vibrance = _d["vibrance"]->getValueAtTime(t);
        P.balance = _d["balance"]->getValueAtTime(t);
        _rgb["shadowTint"]->getValueAtTime(t, P.shTint[0], P.shTint[1], P.shTint[2]);
        _rgb["highlightTint"]->getValueAtTime(t, P.hiTint[0], P.hiTint[1], P.hiTint[2]);
        _rgb["lift"]->getValueAtTime(t, P.lift[0], P.lift[1], P.lift[2]);
        _rgb["gammaWheel"]->getValueAtTime(t, P.gammaW[0], P.gammaW[1], P.gammaW[2]);
        _rgb["gain"]->getValueAtTime(t, P.gain[0], P.gain[1], P.gain[2]);

        return P;
    }

    std::string path(const char* ext)
    {
        std::string f;
        _file->getValue(f);
        if ( f.empty() ) {
            return f;
        }
        const size_t dot = f.find_last_of('.');
        const size_t slash = f.find_last_of("/\\");
        if ( (dot == std::string::npos) || ( (slash != std::string::npos) && (dot < slash) ) ) {
            return f + ext;
        }

        return f.substr(0, dot) + ext;
    }

    void saveLook()
    {
        const std::string f = path(".look");
        if ( f.empty() ) {
            sendMessage(Message::eMessageError, "", "Set the look file first.");

            return;
        }
        std::ofstream o( f.c_str() );
        for (std::map<std::string, DoubleParam*>::iterator it = _d.begin(); it != _d.end(); ++it) {
            o << it->first << " " << it->second->getValue() << "\n";
        }
        for (std::map<std::string, RGBParam*>::iterator it = _rgb.begin(); it != _rgb.end(); ++it) {
            double r, g, b;
            it->second->getValue(r, g, b);
            o << it->first << " " << r << " " << g << " " << b << "\n";
        }
        sendMessage(Message::eMessageMessage, "", o ? "Look saved: " + f : "Could not write " + f);
    }

    void loadLook()
    {
        const std::string f = path(".look");
        std::ifstream in( f.c_str() );
        if (!in) {
            sendMessage(Message::eMessageError, "", "Could not read " + f);

            return;
        }
        beginEditBlock("load look");
        std::string line;
        while ( std::getline(in, line) ) {
            std::istringstream is(line);
            std::string n;
            is >> n;
            if ( _d.count(n) ) {
                double v;
                if (is >> v) {
                    _d[n]->setValue(v);
                }
            } else if ( _rgb.count(n) ) {
                double r, g, b;
                if (is >> r >> g >> b) {
                    _rgb[n]->setValue(r, g, b);
                }
            }
        }
        endEditBlock();
    }

    // colour part as a 3D LUT on a log2 shaper (-8..+8 stops around 0.18) so it covers scene linear
    void exportCube(double t)
    {
        const std::string f = path(".cube");
        if ( f.empty() ) {
            sendMessage(Message::eMessageError, "", "Set the look file first.");

            return;
        }
        const int n = std::max( 2, std::min(129, _cubeSize->getValue() ) );
        const LookParams P = params(t);
        std::ofstream o( f.c_str() );
        o << "TITLE \"APFX Look\"\n";
        o << "# input: log2(x / 0.18) mapped from [-8, 8] to [0, 1] (apply a matching shaper before this LUT)\n";
        o << "LUT_3D_SIZE " << n << "\n";
        o << "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
        for (int b = 0; b < n; ++b) {
            for (int g = 0; g < n; ++g) {
                for (int r = 0; r < n; ++r) {
                    const int idx[3] = { r, g, b };
                    double c[3];
                    for (int i = 0; i < 3; ++i) {
                        c[i] = 0.18 * std::pow(2., -8. + 16. * idx[i] / (n - 1.) );
                    }
                    applyLook(P, c);
                    for (int i = 0; i < 3; ++i) {
                        const double v = std::log2( std::max(1e-9, c[i]) / 0.18 );
                        o << std::max( 0., std::min(1., (v + 8.) / 16.) ) << (i < 2 ? " " : "\n");
                    }
                }
            }
        }
        sendMessage(Message::eMessageMessage, "", o ? "LUT exported: " + f : "Could not write " + f);
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) OVERRIDE FINAL
    {
        if (name == "saveLook") {
            saveLook();
        } else if (name == "loadLook") {
            loadLook();
        } else if (name == "exportCube") {
            exportCube(args.time);
        } else if (name == "resetLook") {
            beginEditBlock("reset look");
            for (std::map<std::string, DoubleParam*>::iterator it = _d.begin(); it != _d.end(); ++it) {
                it->second->resetToDefault();
            }
            for (std::map<std::string, RGBParam*>::iterator it = _rgb.begin(); it != _rgb.end(); ++it) {
                it->second->resetToDefault();
            }
            endEditBlock();
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
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
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
        const LookParams P = params(t);
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
                    // unpremultiply, grade, premultiply (alpha untouched)
                    const double a = s[3];
                    double c[3];
                    for (int i = 0; i < 3; ++i) {
                        c[i] = a > 1e-6 ? s[i] / a : s[i];
                    }
                    applyLook(P, c);
                    float mv = o.mix;
                    if (mask) {
                        const float* mp = (const float*)mask->getPixelAddress(x, y);
                        float v = mp ? *mp : 0.f;
                        if (o.maskInvert) {
                            v = 1.f - v;
                        }
                        mv *= std::max(0.f, std::min(1.f, v) );
                    }
                    for (int i = 0; i < 3; ++i) {
                        const float r = (float)( a > 1e-6 ? c[i] * a : c[i] );
                        d[i] = s[i] + (r - s[i]) * mv;
                    }
                    d[3] = s[3];
                }
            }
        }, win.y2 - win.y1);
        delete mask;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    APFXOfx::StdParams _std;
    std::map<std::string, DoubleParam*> _d;
    std::map<std::string, RGBParam*> _rgb;
    StringParam* _file;
    IntParam* _cubeSize;
};

mDeclarePluginFactory(APFXLookFactory, {}, {});

void
APFXLookFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Look",
                            "APFX Look: white balance, exposure, contrast, highlights / shadows / whites / blacks, "
                            "saturation, vibrance, faded film, split toning, colour wheels. One fast float pass, works on "
                            "premultiplied images. Save / load looks and export the colour part as a .cube 3D LUT (log2 shaper).");
    desc.setPluginGrouping("Color");
    desc.setSupportsTiles(true);
}

void
APFXLookFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false)->setSupportsTiles(true);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false)->setSupportsTiles(true);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("Look");
    APFXOfx::defineDouble(desc, page, "temperature", "temperature", "Blue (-) to amber (+).", 0., -1., 1., -5., 5.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "tint", "tint", "Green (-) to magenta (+).", 0., -1., 1., -5., 5.) );
    APFXOfx::defineDouble(desc, page, "exposure", "exposure", "Stops.", 0., -4., 4., -20., 20.);
    APFXOfx::defineDouble(desc, page, "contrast", "contrast", "Contrast around the pivot.", 0., -1., 1., -1., 5.);
    APFXOfx::defineDouble(desc, page, "pivot", "pivot", "Contrast pivot (scene linear, 0.18 = mid grey).", 0.18, 0.01, 1., 0.0001, 100.);
    APFXOfx::defineDouble(desc, page, "highlights", "highlights", "Brighten (+) / recover (-) the highlights.", 0., -1., 1., -5., 5.);
    APFXOfx::defineDouble(desc, page, "shadows", "shadows", "Open (+) / deepen (-) the shadows.", 0., -1., 1., -5., 5.);
    APFXOfx::defineDouble(desc, page, "whites", "whites", "Top end.", 0., -1., 1., -5., 5.);
    APFXOfx::defineDouble(desc, page, "blacks", "blacks", "Bottom end.", 0., -1., 1., -5., 5.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "saturation", "saturation", "Saturation.", 1., 0., 2., 0., 10.) );
    APFXOfx::defineDouble(desc, page, "fade", "faded film", "Lifted blacks and softer top.", 0., 0., 1., 0., 5.);
    APFXOfx::defineDouble(desc, page, "vibrance", "vibrance", "Saturates the less saturated colours more.", 0., -1., 1., -1., 5.);
    {
        RGBParamDescriptor* p = desc.defineRGBParam("shadowTint");
        p->setLabel("shadow tint");
        p->setDefault(0., 0., 0.);
        p->setDisplayRange(-1., -1., -1., 1., 1., 1.);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("highlightTint");
        p->setLabel("highlight tint");
        p->setDefault(0., 0., 0.);
        p->setDisplayRange(-1., -1., -1., 1., 1., 1.);
        page->addChild(*p);
    }
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "balance", "tint balance", "Moves the split between shadow and highlight tint.", 0., -1., 1., -2., 2.) );
    {
        RGBParamDescriptor* p = desc.defineRGBParam("lift");
        p->setLabel("shadows wheel");
        p->setDefault(0., 0., 0.);
        p->setDisplayRange(-1., -1., -1., 1., 1., 1.);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("gammaWheel");
        p->setLabel("midtones wheel");
        p->setDefault(0., 0., 0.);
        p->setDisplayRange(-1., -1., -1., 1., 1., 1.);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("gain");
        p->setLabel("highlights wheel");
        p->setDefault(0., 0., 0.);
        p->setDisplayRange(-1., -1., -1., 1., 1., 1.);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        StringParamDescriptor* p = desc.defineStringParam("lookFile");
        p->setLabel("look file");
        p->setHint("File used by save / load (.look) and export (.cube, same name).");
        p->setStringType(eStringTypeFilePath);
        p->setFilePathExists(false);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    const char* btn[][3] = { { "saveLook", "save look", "Saves all settings to the .look file." },
                             { "loadLook", "load look", "Loads the .look file." },
                             { "exportCube", "export .cube", "Exports the colour part as a 3D LUT (log2 shaper -8..+8 stops around 0.18)." },
                             { "resetLook", "reset", "All settings back to default." } };
    for (int i = 0; i < 4; ++i) {
        PushButtonParamDescriptor* p = desc.definePushButtonParam(btn[i][0]);
        p->setLabel(btn[i][1]);
        p->setHint(btn[i][2]);
        if (i < 3) {
            p->setLayoutHint(eLayoutHintNoNewLine, 1);
        }
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("cubeSize");
        p->setLabel("cube size");
        p->setDefault(33);
        p->setRange(2, 129);
        p->setDisplayRange(17, 65);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXLookFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXLookPlugin(h);
}
} // anon namespace

static APFXLookFactory apfxLookFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxLookFactory)
