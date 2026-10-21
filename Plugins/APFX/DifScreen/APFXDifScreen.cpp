/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX difference screen keyer:
//  - DifScreenColour builds a clean screen plate (screen colour grown under the foreground)
//  - DifScreenKey keys the shot against that plate with a colour difference (red / blue weights)

#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXKeyOps.h"

using namespace OFX;

namespace {

static void
defineScreenChoice(ImageEffectDescriptor& desc, PageParamDescriptor* page)
{
    ChoiceParamDescriptor* p = desc.defineChoiceParam("screen");
    p->setLabel("screen type");
    p->setHint("Colour of the screen.");
    p->appendOption("green", "", "green");
    p->appendOption("blue", "", "blue");
    p->setDefault(0);
    p->setAnimates(false);
    page->addChild(*p);
}

// ================================================================ DifScreenColour

class APFXDifScreenColourPlugin
    : public ImageEffect
{
public:
    APFXDifScreenColourPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, false);
        _screen = fetchChoiceParam("screen");
        _threshold = fetchDoubleParam("threshold");
        _softness = fetchDoubleParam("softness");
        _darks = fetchRGBParam("darks");
        _lights = fetchRGBParam("lights");
        _size = fetchDoubleParam("size");
        _erode = fetchDoubleParam("erode");
        _patch = fetchDoubleParam("patchBlack");
        _smooth = fetchDoubleParam("smooth");
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
        rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(args.time) );
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setClipBitDepth(*_dst, eBitDepthFloat);
        cp.setOutputPremultiplication(eImageOpaque);
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
        APFXOps::Plane in, out;
        const OfxRectI b = APFXOfx::imageToPlane(src, in);
        const double rs = args.renderScale.x;
        APFXKey::ScreenColourParams p;
        p.screen = _screen->getValueAtTime(t);
        p.threshold = (float)_threshold->getValueAtTime(t);
        p.softness = (float)_softness->getValueAtTime(t);
        double r, g, bl;
        _darks->getValueAtTime(t, r, g, bl);
        p.darks[0] = (float)r; p.darks[1] = (float)g; p.darks[2] = (float)bl;
        _lights->getValueAtTime(t, r, g, bl);
        p.lights[0] = (float)r; p.lights[1] = (float)g; p.lights[2] = (float)bl;
        p.size = (int)std::floor(_size->getValueAtTime(t) * rs + 0.5);
        p.erode = (int)std::floor(_erode->getValueAtTime(t) * rs + 0.5);
        p.patchBlack = (float)_patch->getValueAtTime(t);
        p.smooth = (float)(_smooth->getValueAtTime(t) * rs);
        p.showMask = (_output->getValueAtTime(t) == 1);
        APFXKey::difScreenColour( in, p, out, APFXOfx::hostParallelFor() );
        APFXOfx::OutputArgs o;
        _std.get(t, o);
        APFXOfx::writeOutput(*this, dst, args.renderWindow, out, b, &in, b, o);
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    APFXOfx::StdParams _std;
    ChoiceParam* _screen;
    DoubleParam* _threshold;
    DoubleParam* _softness;
    RGBParam* _darks;
    RGBParam* _lights;
    DoubleParam* _size;
    DoubleParam* _erode;
    DoubleParam* _patch;
    DoubleParam* _smooth;
    ChoiceParam* _output;
};

mDeclarePluginFactory(APFXDifScreenColourFactory, {}, {});

void
APFXDifScreenColourFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "DifScreenColour",
                            "APFX DifScreenColour: builds a clean screen plate from a green / blue screen shot. "
                            "Pixels that are clearly screen are kept, the foreground is grown by 'size' and filled with "
                            "the surrounding screen colour. Feed the result to DifScreenKey's Plate input.");
}

void
APFXDifScreenColourFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    PageParamDescriptor* page = desc.definePageParam("DifScreenColour");
    APFXOfx::defineChannels(desc, page, true);
    defineScreenChoice(desc, page);
    APFXOfx::defineDouble(desc, page, "size", "size", "How hard the foreground is removed: grows it by this many pixels before the screen is patched.", 10., 0., 100., 0., 2000.);
    {
        RGBParamDescriptor* p = desc.defineRGBParam("darks");
        p->setLabel("darks");
        p->setHint("Colour treated as black: separates dark foreground from the screen. Lower the screen channel (green / blue) to keep more dark screen.");
        p->setDefault(0., 0., 0.);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("lights");
        p->setLabel("lights");
        p->setHint("Colour treated as white: with darks, separates the foreground from the screen (e.g. raise red to remove light red traces).");
        p->setDefault(1., 1., 1.);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "erode", "erode", "Extra erosion for traces of foreground edge colour left in the plate (pixels).", 0., 0., 5., 0., 500.);
    APFXOfx::defineDouble(desc, page, "patchBlack", "patch black",
                          "Patches the black (removed foreground) with the surrounding screen colours. Every step reaches further, "
                          "like stacking more colour nodes: 0 = foreground stays black, 5 = everything patched (full clean plate).", 5., 0., 5., 0., 5.);
    {
        GroupParamDescriptor* g = desc.defineGroupParam("advancedGroup");
        g->setLabel("Advanced");
        g->setOpen(false);
        page->addChild(*g);
        DoubleParamDescriptor* th = APFXOfx::defineDouble(desc, page, "threshold", "threshold", "How much the screen channel must exceed the others (after darks / lights) to count as screen.", 0., 0., 0.5, -1., 10.);
        th->setParent(*g);
        DoubleParamDescriptor* so = APFXOfx::defineDouble(desc, page, "softness", "softness", "Ramp above the threshold (soft screen detection).", 0.05, 0.001, 0.5, 0.0001, 10.);
        so->setParent(*g);
        DoubleParamDescriptor* sm = APFXOfx::defineDouble(desc, page, "smooth", "smooth", "Blur of the final plate (pixels).", 0., 0., 20., 0., 1000.);
        sm->setParent(*g);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
        p->setLabel("output");
        p->setHint("clean plate, or the screen mask used to build it (to check the settings).");
        p->appendOption("clean plate", "", "plate");
        p->appendOption("screen mask", "", "mask");
        p->setDefault(0);
        p->setAnimates(false);
        page->addChild(*p);
    }
}

ImageEffect*
APFXDifScreenColourFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXDifScreenColourPlugin(h);
}

// ================================================================ DifScreenKey

class APFXDifScreenKeyPlugin
    : public ImageEffect
{
public:
    APFXDifScreenKeyPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _plate = fetchClip("Plate");
        _bg = fetchClip("Bg");
        _std.fetch(*this, true);
        _screen = fetchChoiceParam("screen");
        _screenColour = fetchRGBParam("colour");
        _redWeight = fetchDoubleParam("redWeight");
        _otherWeight = fetchDoubleParam("otherWeight");
        _lumMatch = fetchBooleanParam("luminanceMatch");
        _screenRange = fetchDoubleParam("screenRange");
        _lumLevel = fetchDoubleParam("luminanceLevel");
        _lumEnable = fetchBooleanParam("luminanceLevelEnable");
        _autolevels = fetchBooleanParam("autolevels");
        _yellow = fetchBooleanParam("yellow");
        _cyan = fetchBooleanParam("cyan");
        _magenta = fetchBooleanParam("magenta");
        _blackClip = fetchDoubleParam("blackClip");
        _whiteClip = fetchDoubleParam("whiteClip");
        _subtract = fetchBooleanParam("screenSubtraction");
        _bgLum = fetchBooleanParam("useBkgLuminance");
        _bgChroma = fetchBooleanParam("useBkgChroma");
        _premult = fetchBooleanParam("premultiply");
        update();
    }

private:
    void update()
    {
        const bool lm = _lumMatch->getValue();
        _screenRange->setEnabled(lm);
        _lumEnable->setEnabled(lm);
        _lumLevel->setEnabled( lm && _lumEnable->getValue() );
        const bool al = _autolevels->getValue();
        _yellow->setEnabled(al);
        _cyan->setEnabled(al);
        _magenta->setEnabled(al);
        _screenColour->setEnabled( _screen->getValue() == 2 );
    }

    void changedParam(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == "screen") || (n == "luminanceMatch") || (n == "luminanceLevelEnable") || (n == "autolevels") ) {
            update();
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

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(args.time) );
        if ( _plate->isConnected() ) {
            rois.setRegionOfInterest( *_plate, _src->getRegionOfDefinition(args.time) );
        }
        if ( _bg->isConnected() ) {
            rois.setRegionOfInterest( *_bg, _src->getRegionOfDefinition(args.time) );
        }
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_plate, ePixelComponentRGBA);
        cp.setClipComponents(*_bg, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setClipBitDepth(*_dst, eBitDepthFloat);
        cp.setOutputPremultiplication( _premult->getValue() ? eImagePreMultiplied : eImageUnPreMultiplied );
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
        APFXOps::Plane in, plate, out;
        const OfxRectI b = APFXOfx::imageToPlane(src, in);
        // plate: the Plate input (C-green / C-blue), or the flat colour (pick / no Plate)
        const int screenType = _screen->getValueAtTime(t);
        plate.resize(in.w, in.h, 4);
        Image* pimg = ( (screenType != 2) && _plate->isConnected() ) ? _plate->fetchImage(t) : NULL;
        if ( pimg && ( (pimg->getPixelComponents() != ePixelComponentRGBA) || (pimg->getPixelDepth() != eBitDepthFloat) ) ) {
            delete pimg;
            pimg = NULL;
        }
        double sr, sg, sbv;
        _screenColour->getValueAtTime(t, sr, sg, sbv);
        for (int y = 0; y < in.h; ++y) {
            for (int x = 0; x < in.w; ++x) {
                float* o = plate.px(x, y);
                const float* s = pimg ? (const float*)pimg->getPixelAddress(b.x1 + x, b.y1 + y) : NULL;
                if (s) {
                    o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
                } else {
                    o[0] = (float)sr; o[1] = (float)sg; o[2] = (float)sbv;
                }
                o[3] = 1.f;
            }
        }
        delete pimg;
        // optional background
        APFXOps::Plane bgPlane;
        bool haveBg = false;
        if ( _bg->isConnected() && ( _bgLum->getValueAtTime(t) || _bgChroma->getValueAtTime(t) ) ) {
            Image* bimg = _bg->fetchImage(t);
            if ( bimg && (bimg->getPixelComponents() == ePixelComponentRGBA) && (bimg->getPixelDepth() == eBitDepthFloat) ) {
                bgPlane.resize(in.w, in.h, 4);
                for (int y = 0; y < in.h; ++y) {
                    for (int x = 0; x < in.w; ++x) {
                        const float* s = (const float*)bimg->getPixelAddress(b.x1 + x, b.y1 + y);
                        float* o = bgPlane.px(x, y);
                        for (int k = 0; k < 4; ++k) {
                            o[k] = s ? s[k] : 0.f;
                        }
                    }
                }
                haveBg = true;
            }
            delete bimg;
        }
        APFXKey::ScreenKeyParams p;
        // pick: the screen is whichever of green / blue dominates the picked colour
        p.screen = (screenType == 2) ? ( (sbv > sg) ? APFXKey::eScreenBlue : APFXKey::eScreenGreen ) : screenType;
        p.lumMatch = _lumMatch->getValueAtTime(t);
        p.screenRange = (float)_screenRange->getValueAtTime(t);
        p.lumLevelEnable = _lumEnable->getValueAtTime(t);
        p.lumLevel = (float)_lumLevel->getValueAtTime(t);
        p.autolevels = _autolevels->getValueAtTime(t);
        p.protectYellow = _yellow->getValueAtTime(t);
        p.protectCyan = _cyan->getValueAtTime(t);
        p.protectMagenta = _magenta->getValueAtTime(t);
        p.useBgLuminance = _bgLum->getValueAtTime(t);
        p.useBgChroma = _bgChroma->getValueAtTime(t);
        p.redWeight = (float)_redWeight->getValueAtTime(t);
        p.otherWeight = (float)_otherWeight->getValueAtTime(t);
        p.blackClip = (float)_blackClip->getValueAtTime(t);
        p.whiteClip = (float)_whiteClip->getValueAtTime(t);
        p.screenSubtraction = _subtract->getValueAtTime(t);
        p.premultiply = _premult->getValueAtTime(t);
        APFXKey::difScreenKey( in, plate, haveBg ? &bgPlane : NULL, p, out, APFXOfx::hostParallelFor() );
        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        APFXOfx::writeOutput(*this, dst, args.renderWindow, out, b, &in, b, o);
        delete mask;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    Clip* _plate;
    Clip* _bg;
    APFXOfx::StdParams _std;
    ChoiceParam* _screen;
    RGBParam* _screenColour;
    DoubleParam* _redWeight;
    DoubleParam* _otherWeight;
    BooleanParam* _lumMatch;
    DoubleParam* _screenRange;
    DoubleParam* _lumLevel;
    BooleanParam* _lumEnable;
    BooleanParam* _autolevels;
    BooleanParam* _yellow;
    BooleanParam* _cyan;
    BooleanParam* _magenta;
    BooleanParam* _bgLum;
    BooleanParam* _bgChroma;
    DoubleParam* _blackClip;
    DoubleParam* _whiteClip;
    BooleanParam* _subtract;
    BooleanParam* _premult;
};

mDeclarePluginFactory(APFXDifScreenKeyFactory, {}, {});

void
APFXDifScreenKeyFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "DifScreenKey",
                            "APFX DifScreenKey: difference screen keyer. The alpha comes from how much the screen colour "
                            "of the shot differs from the clean Plate (DifScreenColour output), using a colour difference "
                            "with red / blue weights. Without a Plate, the flat screen colour is used. "
                            "Screen subtraction removes the screen colour from the foreground (despill).");
}

void
APFXDifScreenKeyFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, "Plate", true);
    APFXOfx::defineRGBAClip(desc, "Bg", true);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("DifScreenKey");
    APFXOfx::defineChannels(desc, page, true);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("screen");
        p->setLabel("screen type");
        p->setHint("C-green / C-blue: key against the Plate input (DifScreenColour). pick: key against the colour below.");
        p->appendOption("C-green", "", "green");
        p->appendOption("C-blue", "", "blue");
        p->appendOption("pick", "", "pick");
        p->setDefault(0);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("colour");
        p->setLabel("colour");
        p->setHint("Screen colour for 'pick' (also used when the Plate input is not connected).");
        p->setDefault(0., 0., 1.);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "redWeight", "red weight", "Weight of the red channel: lower it to fix the edges of red foreground.", 0.5, 0., 1., -10., 10.);
    APFXOfx::defineDouble(desc, page, "otherWeight", "blue/green weight", "Weight of blue on a green screen, green on a blue screen.", 0.5, 0., 1., -10., 10.);
    APFXOfx::defineBool(desc, page, "luminanceMatch", "luminance match", "Adds a luminance factor: captures foreground brighter than the screen (bright transparent areas).", false);
    APFXOfx::defineDouble(desc, page, "screenRange", "screen range", "Lower it until the background stops changing: less screen noise, harder edges.", 1., 0., 1., 0.01, 10.);
    APFXOfx::defineDouble(desc, page, "luminanceLevel", "luminance level", "Strength of the luminance match (when enabled).", 0., 0., 1., 0., 10.);
    APFXOfx::defineBool(desc, page, "luminanceLevelEnable", "enable", "Use the luminance level control.", false);
    APFXOfx::defineBool(desc, page, "autolevels", "autolevels", "Harder key on saturated foreground colours only (the weights affect the whole image).", false);
    APFXOfx::defineBool(desc, page, "yellow", "yellow", "Protect saturated yellow from autolevels.", false);
    APFXOfx::defineBool(desc, page, "cyan", "cyan", "Protect saturated cyan from autolevels.", false);
    APFXOfx::defineBool(desc, page, "magenta", "magenta", "Protect saturated magenta from autolevels.", false);
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "screenSubtraction", "screen subtraction", "On: removes the screen colour from the foreground (despill). Off: the original foreground is multiplied by the matte.", true);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
    }
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "useBkgLuminance", "use bkg luminance", "The Bg input affects the brightness of the edges (helps with fringing).", false);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
    }
    APFXOfx::defineBool(desc, page, "useBkgChroma", "use bkg chroma", "The Bg input affects the colour of the edges.", false);
    APFXOfx::defineDouble(desc, page, "blackClip", "black clip", "Alpha below this becomes 0 (cleans screen noise).", 0., 0., 1., -1., 2.);
    APFXOfx::defineDouble(desc, page, "whiteClip", "white clip", "Alpha above this becomes 1.", 1., 0., 1., -1., 2.);
    {
        BooleanParamDescriptor* p = APFXOfx::defineBool(desc, page, "premultiply", "premultiply", "Output a premultiplied foreground.", true);
        desc.addClipPreferencesSlaveParam(*p);
    }
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXDifScreenKeyFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXDifScreenKeyPlugin(h);
}
} // anon namespace

static APFXDifScreenColourFactory apfxDifScreenColourFactory("com.aguapanelafx.APFX.DifScreenColour", 1, 0);
mRegisterPluginFactoryInstance(apfxDifScreenColourFactory)
static APFXDifScreenKeyFactory apfxDifScreenKeyFactory("com.aguapanelafx.APFX.DifScreenKey", 1, 0);
mRegisterPluginFactoryInstance(apfxDifScreenKeyFactory)
