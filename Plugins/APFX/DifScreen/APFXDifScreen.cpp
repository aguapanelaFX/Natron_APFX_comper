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
        _darks = fetchDoubleParam("darks");
        _lights = fetchDoubleParam("lights");
        _size = fetchDoubleParam("size");
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
        p.darks = (float)_darks->getValueAtTime(t);
        p.lights = (float)_lights->getValueAtTime(t);
        p.size = (int)std::floor(_size->getValueAtTime(t) * rs + 0.5);
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
    DoubleParam* _darks;
    DoubleParam* _lights;
    DoubleParam* _size;
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
    APFXOfx::defineDouble(desc, page, "threshold", "threshold", "How much the screen channel must exceed the others for a pixel to count as screen.", 0.05, 0., 0.5, -1., 10.);
    APFXOfx::defineDouble(desc, page, "softness", "softness", "Ramp above the threshold (soft screen detection).", 0.1, 0.001, 0.5, 0.0001, 10.);
    APFXOfx::defineDouble(desc, page, "darks", "darks", "Pixels whose screen channel is darker than this are not screen.", 0., 0., 0.5, -10., 10.);
    APFXOfx::defineDouble(desc, page, "lights", "lights", "Pixels whose screen channel is brighter than this are not screen.", 2., 0.5, 4., -10., 1000.);
    APFXOfx::defineDouble(desc, page, "size", "size", "Grows the foreground before filling (pixels): removes edge contamination from the plate.", 10., 0., 50., 0., 1000.);
    APFXOfx::defineDouble(desc, page, "smooth", "smooth", "Blur of the final plate (pixels).", 2., 0., 20., 0., 1000.);
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
        _std.fetch(*this, true);
        _screen = fetchChoiceParam("screen");
        _screenColour = fetchRGBParam("screenColour");
        _redWeight = fetchDoubleParam("redWeight");
        _otherWeight = fetchDoubleParam("otherWeight");
        _blackClip = fetchDoubleParam("blackClip");
        _whiteClip = fetchDoubleParam("whiteClip");
        _subtract = fetchBooleanParam("screenSubtraction");
        _premult = fetchBooleanParam("premultiply");
        update();
    }

private:
    void update()
    {
        _otherWeight->setLabel( _screen->getValue() == APFXKey::eScreenBlue ? "green weight" : "blue weight" );
    }

    void changedParam(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if (n == "screen") {
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
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_plate, ePixelComponentRGBA);
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
        // plate: the Plate input, or a flat screen colour
        plate.resize(in.w, in.h, 4);
        Image* pimg = _plate->isConnected() ? _plate->fetchImage(t) : NULL;
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
        APFXKey::ScreenKeyParams p;
        p.screen = _screen->getValueAtTime(t);
        p.redWeight = (float)_redWeight->getValueAtTime(t);
        p.otherWeight = (float)_otherWeight->getValueAtTime(t);
        p.blackClip = (float)_blackClip->getValueAtTime(t);
        p.whiteClip = (float)_whiteClip->getValueAtTime(t);
        p.screenSubtraction = _subtract->getValueAtTime(t);
        p.premultiply = _premult->getValueAtTime(t);
        APFXKey::difScreenKey( in, plate, p, out, APFXOfx::hostParallelFor() );
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
    APFXOfx::StdParams _std;
    ChoiceParam* _screen;
    RGBParam* _screenColour;
    DoubleParam* _redWeight;
    DoubleParam* _otherWeight;
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
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("DifScreenKey");
    APFXOfx::defineChannels(desc, page, true);
    defineScreenChoice(desc, page);
    {
        RGBParamDescriptor* p = desc.defineRGBParam("screenColour");
        p->setLabel("screen colour");
        p->setHint("Used when the Plate input is not connected.");
        p->setDefault(0.1, 0.55, 0.15);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "redWeight", "red weight", "How much the red channel counts against the screen channel.", 0.5, 0., 1., -10., 10.);
    APFXOfx::defineDouble(desc, page, "otherWeight", "blue weight", "How much the third channel counts against the screen channel (blue on green screens, green on blue screens).", 0.5, 0., 1., -10., 10.);
    APFXOfx::defineDouble(desc, page, "blackClip", "black clip", "Alpha below this becomes 0.", 0., 0., 1., -1., 2.);
    APFXOfx::defineDouble(desc, page, "whiteClip", "white clip", "Alpha above this becomes 1.", 1., 0., 1., -1., 2.);
    APFXOfx::defineBool(desc, page, "screenSubtraction", "screen subtraction", "Remove the screen colour from the foreground (despill).", true);
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
