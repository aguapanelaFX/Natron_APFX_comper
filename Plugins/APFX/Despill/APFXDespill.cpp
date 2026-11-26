/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX despill nodes:
//  - DespillPro: the classic despill formulas (average, double average, limit, custom balance), fine tune,
//    luminance restore, spill matte
//  - DespillToColour: the spill is replaced by a target colour of the same brightness
//  - Despill: spill removed along the screen colour (brightness and hues kept), screen colour picked,
//    automatic or from a clean plate (c input), replaced by a colour or the bg input, optionally only on
//    the edges of the alpha

#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXDespill.h"

using namespace OFX;

namespace {

enum { eOutDespill = 0, eOutSpillMatte };

static void
defineOutput(ImageEffectDescriptor& desc, PageParamDescriptor* page)
{
    ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
    p->setLabel("output");
    p->setHint("despilled image, or the spill matte (how much spill was removed, in rgb and alpha).");
    p->appendOption("despill", "", "despill");
    p->appendOption("spill matte", "", "spillmatte");
    p->setDefault(eOutDespill);
    p->setAnimates(false);
    p->setLayoutHint(eLayoutHintNoNewLine, 1);
    page->addChild(*p);
    APFXOfx::defineBool(desc, page, "spillToAlpha", "spill matte in alpha", "Despill output: put the spill matte in the alpha (the colour stays despilled).", false);
}

static RGBParamDescriptor*
defineColour(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint,
             double r, double g, double b)
{
    RGBParamDescriptor* p = desc.defineRGBParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(r, g, b);
    page->addChild(*p);

    return p;
}

// common part of the three nodes: source, output, mask / mix, output choice
class DespillBase
    : public ImageEffect
{
public:
    DespillBase(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true, false);
        _output = fetchChoiceParam("output");
        _toAlpha = fetchBooleanParam("spillToAlpha");
    }

protected:
    // per-pixel despill of 'in' into 'out' (rgb) and the spill amount (spill)
    virtual void process(double t, const APFXOps::Plane& in, const OfxRectI& b, APFXOps::Plane& rgbOut, std::vector<float>& spill) = 0;

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        rod = _src->getRegionOfDefinition(args.time);

        return true;
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE
    {
        rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(args.time) );
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setClipBitDepth(*_dst, eBitDepthFloat);
        cp.setOutputPremultiplication( _src->isConnected() ? _src->getPreMultiplication() : eImagePreMultiplied );
    }

    void render(const RenderArguments& args) OVERRIDE
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
        std::vector<float> spill;
        process(t, in, b, out, spill);
        const int mode = _output->getValueAtTime(t);
        const bool toAlpha = _toAlpha->getValueAtTime(t);
        APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        pf([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * in.w; i < (size_t)y1 * in.w; ++i) {
                float* o = &out.d[i * 4];
                if (mode == eOutSpillMatte) {
                    o[0] = o[1] = o[2] = o[3] = spill[i];
                } else {
                    o[3] = toAlpha ? spill[i] : in.d[i * 4 + 3];
                }
            }
        }, in.h);
        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        APFXOfx::writeOutput(*this, dst, args.renderWindow, out, b, &in, b, o);
        delete mask;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    APFXOfx::StdParams _std;
    ChoiceParam* _output;
    BooleanParam* _toAlpha;
};

// ================================================================ DespillPro
class DespillProPlugin
    : public DespillBase
{
public:
    DespillProPlugin(OfxImageEffectHandle h)
        : DespillBase(h)
    {
        _screen = fetchChoiceParam("screen");
        _algo = fetchChoiceParam("algorithm");
        _balance = fetchDoubleParam("balance");
        _gain = fetchDoubleParam("spillGain");
        _threshold = fetchDoubleParam("threshold");
        _restore = fetchDoubleParam("restore");
        _tint = fetchRGBParam("restoreTint");
        update();
    }

private:
    void update()
    {
        _balance->setEnabled(_algo->getValue() == APFXDespill::eCustom);
        _tint->setEnabled(_restore->getValue() != 0.);
    }

    void changedParam(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == "algorithm") || (n == "restore") ) {
            update();
        }
    }

    void process(double t, const APFXOps::Plane& in, const OfxRectI&, APFXOps::Plane& out, std::vector<float>& spill) OVERRIDE FINAL
    {
        APFXDespill::ClassicParams p;
        p.screen = _screen->getValueAtTime(t);
        p.algorithm = _algo->getValueAtTime(t);
        p.balance = (float)_balance->getValueAtTime(t);
        p.gain = (float)_gain->getValueAtTime(t);
        p.threshold = (float)_threshold->getValueAtTime(t);
        p.restore = (float)_restore->getValueAtTime(t);
        double r, g, b;
        _tint->getValueAtTime(t, r, g, b);
        p.tint[0] = (float)r; p.tint[1] = (float)g; p.tint[2] = (float)b;
        out.resize(in.w, in.h, 4);
        spill.assign( (size_t)in.w * in.h, 0.f );
        APFXOfx::hostParallelFor()([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * in.w; i < (size_t)y1 * in.w; ++i) {
                spill[i] = APFXDespill::classic(&in.d[i * 4], p, &out.d[i * 4]);
            }
        }, in.h);
    }

    ChoiceParam* _screen;
    ChoiceParam* _algo;
    DoubleParam* _balance;
    DoubleParam* _gain;
    DoubleParam* _threshold;
    DoubleParam* _restore;
    RGBParam* _tint;
};

mDeclarePluginFactory(DespillProFactory, {}, {});

void
DespillProFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "DespillPro",
                            "APFX DespillPro: the classic despill formulas in one node. The screen channel is limited to a "
                            "reference made of the other two: average, double red average, double blue (green) average, red "
                            "limit, blue (green) limit, or a custom red balance. Fine tune: spill gain and threshold. "
                            "Luminance restore gives the removed brightness back as grey (keeps skin and grain). "
                            "Output the spill matte to see or reuse what was removed.");
    desc.setPluginGrouping("Keyer");
}

void
DespillProFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("DespillPro");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("screen");
        p->setLabel("screen type");
        p->setHint("Colour of the screen (spill).");
        p->appendOption("green", "", "green");
        p->appendOption("blue", "", "blue");
        p->setDefault(0);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("algorithm");
        p->setLabel("algorithm");
        p->setHint("Reference the screen channel is limited to. average: strong, neutral. double red / double other: "
                   "the reference leans to red (warmer) or to the other channel. limit: gentlest on one side, can look "
                   "darker. custom: set the red balance.");
        p->appendOption("average", "", "average");
        p->appendOption("double red average", "", "doublered");
        p->appendOption("double blue/green average", "", "doubleother");
        p->appendOption("red limit", "", "redlimit");
        p->appendOption("blue/green limit", "", "otherlimit");
        p->appendOption("custom", "", "custom");
        p->setDefault(0);
        page->addChild(*p);
    }
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "balance", "red balance", "custom: weight of red in the reference (the other channel gets the rest).", 0.5, 0., 1., -1., 2.) );
    APFXOfx::defineDouble(desc, page, "spillGain", "spill gain", "Fine tune: how much of the spill is removed (above 1: more).", 1., 0., 2., 0., 10.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "threshold", "threshold", "Fine tune: spill below this is left alone.", 0., 0., 0.2, -1., 1.) );
    APFXOfx::defineDouble(desc, page, "restore", "luminance restore", "Gives the brightness removed by the despill back as grey (0 = off).", 0., 0., 1., 0., 2.);
    APFXOfx::divider( defineColour(desc, page, "restoreTint", "restore tint", "Colour of the restored light (e.g. the background's colour).", 1., 1., 1.) );
    defineOutput(desc, page);
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
DespillProFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new DespillProPlugin(h);
}

// ================================================================ DespillToColour
class DespillToColourPlugin
    : public DespillBase
{
public:
    DespillToColourPlugin(OfxImageEffectHandle h)
        : DespillBase(h)
    {
        _screen = fetchRGBParam("screenColour");
        _target = fetchRGBParam("targetColour");
        _balance = fetchDoubleParam("balance");
        _gain = fetchDoubleParam("gain");
    }

private:
    void process(double t, const APFXOps::Plane& in, const OfxRectI&, APFXOps::Plane& out, std::vector<float>& spill) OVERRIDE FINAL
    {
        APFXDespill::ToColourParams p;
        double r, g, b;
        _screen->getValueAtTime(t, r, g, b);
        p.screen[0] = (float)r; p.screen[1] = (float)g; p.screen[2] = (float)b;
        _target->getValueAtTime(t, r, g, b);
        p.target[0] = (float)r; p.target[1] = (float)g; p.target[2] = (float)b;
        p.balance = (float)_balance->getValueAtTime(t);
        p.gain = (float)_gain->getValueAtTime(t);
        out.resize(in.w, in.h, 4);
        spill.assign( (size_t)in.w * in.h, 0.f );
        APFXOfx::hostParallelFor()([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * in.w; i < (size_t)y1 * in.w; ++i) {
                spill[i] = APFXDespill::toColour(&in.d[i * 4], p, &out.d[i * 4]);
            }
        }, in.h);
    }

    RGBParam* _screen;
    RGBParam* _target;
    DoubleParam* _balance;
    DoubleParam* _gain;
};

mDeclarePluginFactory(DespillToColourFactory, {}, {});

void
DespillToColourFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "DespillToColour",
                            "APFX DespillToColour: removes the screen colour from the edges and puts a target colour of the same "
                            "brightness in its place (e.g. the colour of the new background, or the object's own colour). Pick "
                            "the screen colour and the target colour. balance: how much reddish tones are affected. gain: how "
                            "wide a range of colours counts as spill. Works best on one area with one screen colour.");
    desc.setPluginGrouping("Keyer");
}

void
DespillToColourFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("DespillToColour");
    defineColour(desc, page, "screenColour", "screen colour", "Colour of the screen (pick it on clean screen).", 0.1, 0.6, 0.15);
    APFXOfx::divider( defineColour(desc, page, "targetColour", "target colour", "Colour the spill turns into (brightness is kept).", 0.5, 0.5, 0.5) );
    APFXOfx::defineDouble(desc, page, "balance", "balance", "Weight of red against the other channel: lower it so reddish tones (skin) are touched less.", 0.5, 0., 1., -1., 2.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "gain", "gain", "Range of colours treated as spill: higher catches more, below 1 stays close to the screen colour.", 1., 0., 3., 0., 20.) );
    defineOutput(desc, page);
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
DespillToColourFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new DespillToColourPlugin(h);
}

// ================================================================ Despill (vector)
enum { eScreenAuto = 0, eScreenPick, eScreenPlate };
enum { eReplaceNone = 0, eReplaceColour, eReplaceBg };

class DespillVectorPlugin
    : public DespillBase
{
public:
    DespillVectorPlugin(OfxImageEffectHandle h)
        : DespillBase(h)
    {
        _plate = fetchClip("Plate");
        _bg = fetchClip("Bg");
        _screenMode = fetchChoiceParam("screenMode");
        _screen = fetchRGBParam("screenColour");
        _amount = fetchDoubleParam("amount");
        _gain = fetchDoubleParam("range");
        _threshold = fetchDoubleParam("threshold");
        _protect = fetchDoubleParam("protect");
        _lum = fetchDoubleParam("luminance");
        _replaceMode = fetchChoiceParam("replace");
        _replaceColour = fetchRGBParam("replaceColour");
        _replaceAmount = fetchDoubleParam("replaceAmount");
        _bgBlur = fetchDoubleParam("bgBlur");
        _edges = fetchDoubleParam("edges");
        update();
    }

private:
    void update()
    {
        _screen->setEnabled(_screenMode->getValue() == eScreenPick);
        const int rm = _replaceMode->getValue();
        _replaceColour->setEnabled(rm == eReplaceColour);
        _replaceAmount->setEnabled(rm != eReplaceNone);
        _bgBlur->setEnabled(rm == eReplaceBg);
    }

    void changedParam(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == "screenMode") || (n == "replace") ) {
            update();
        }
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        const OfxRectD r = _src->getRegionOfDefinition(args.time);
        rois.setRegionOfInterest(*_src, r);
        if ( _plate->isConnected() ) {
            rois.setRegionOfInterest(*_plate, r);
        }
        if ( _bg->isConnected() ) {
            rois.setRegionOfInterest(*_bg, r);
        }
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        DespillBase::getClipPreferences(cp);
        cp.setClipComponents(*_plate, ePixelComponentRGBA);
        cp.setClipComponents(*_bg, ePixelComponentRGBA);
    }

    // an optional input copied to the bounds of the source (false when missing)
    bool fetchAligned(Clip* c, double t, const OfxRectI& b, int w, int h, APFXOps::Plane& p)
    {
        if ( !c->isConnected() ) {
            return false;
        }
        Image* img = c->fetchImage(t);
        if ( !img || (img->getPixelComponents() != ePixelComponentRGBA) || (img->getPixelDepth() != eBitDepthFloat) ) {
            delete img;

            return false;
        }
        p.resize(w, h, 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* s = (const float*)img->getPixelAddress(b.x1 + x, b.y1 + y);
                float* o = p.px(x, y);
                for (int k = 0; k < 4; ++k) {
                    o[k] = s ? s[k] : 0.f;
                }
            }
        }
        delete img;

        return true;
    }

    void process(double t, const APFXOps::Plane& in, const OfxRectI& b, APFXOps::Plane& out, std::vector<float>& spill) OVERRIDE FINAL
    {
        APFXDespill::VectorParams p;
        p.amount = (float)_amount->getValueAtTime(t);
        p.gain = (float)_gain->getValueAtTime(t);
        p.threshold = (float)_threshold->getValueAtTime(t);
        p.protect = (float)_protect->getValueAtTime(t);
        p.luminance = (float)_lum->getValueAtTime(t);
        p.edges = (float)_edges->getValueAtTime(t);
        const int w = in.w, h = in.h;
        const int mode = _screenMode->getValueAtTime(t);
        // screen colour: flat (picked / automatic) or per pixel (clean plate on the c input)
        float flat[3];
        APFXOps::Plane plate;
        const bool usePlate = (mode == eScreenPlate) && fetchAligned(_plate, t, b, w, h, plate);
        if (mode == eScreenPick) {
            double r, g, bl;
            _screen->getValueAtTime(t, r, g, bl);
            flat[0] = (float)r; flat[1] = (float)g; flat[2] = (float)bl;
        } else {
            APFXDespill::autoScreenColour(in.d, w, h, flat);
        }
        // replacement colour: flat, or the (blurred) bg input
        const int rm = _replaceMode->getValueAtTime(t);
        float rep[3] = { 0.f, 0.f, 0.f };
        APFXOps::Plane bg;
        bool useBg = false;
        if (rm == eReplaceColour) {
            double r, g, bl;
            _replaceColour->getValueAtTime(t, r, g, bl);
            rep[0] = (float)r; rep[1] = (float)g; rep[2] = (float)bl;
            p.replace = (float)_replaceAmount->getValueAtTime(t);
        } else if (rm == eReplaceBg) {
            useBg = fetchAligned(_bg, t, b, w, h, bg);
            if (useBg) {
                p.replace = (float)_replaceAmount->getValueAtTime(t);
                APFXOps::blur( bg, (float)_bgBlur->getValueAtTime(t), APFXOfx::hostParallelFor() );
            }
        }
        out.resize(w, h, 4);
        spill.assign( (size_t)w * h, 0.f );
        APFXOfx::hostParallelFor()([&](int y0, int y1) {
            for (size_t i = (size_t)y0 * w; i < (size_t)y1 * w; ++i) {
                const float* c = &in.d[i * 4];
                const float* s = usePlate ? &plate.d[i * 4] : flat;
                const float* r = useBg ? &bg.d[i * 4] : ( (rm == eReplaceColour) ? rep : NULL );
                spill[i] = APFXDespill::vectorDespill(c, s, r, c[3], p, &out.d[i * 4]);
            }
        }, h);
    }

    Clip* _plate;
    Clip* _bg;
    ChoiceParam* _screenMode;
    RGBParam* _screen;
    DoubleParam* _amount;
    DoubleParam* _gain;
    DoubleParam* _threshold;
    DoubleParam* _protect;
    DoubleParam* _lum;
    ChoiceParam* _replaceMode;
    RGBParam* _replaceColour;
    DoubleParam* _replaceAmount;
    DoubleParam* _bgBlur;
    DoubleParam* _edges;
};

mDeclarePluginFactory(DespillVectorFactory, {}, {});

void
DespillVectorFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Despill",
                            "APFX Despill: removes the spill along the screen colour itself, so the brightness and the hues "
                            "stay right (skin, yellows, cyans, greys come back neutral instead of shifting). Screen colour: "
                            "automatic, picked, or per pixel from a clean plate on the c input (uneven screens). protect "
                            "keeps colours close to the screen colour from being changed. The spill can be replaced by a "
                            "colour or by the (blurred) bg input, and limited to the semi-transparent edges of the alpha.");
    desc.setPluginGrouping("Keyer");
}

void
DespillVectorFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false)->setLabels("fg", "fg", "fg");
    APFXOfx::defineRGBAClip(desc, "Plate", true)->setLabels("c", "c", "c");
    APFXOfx::defineRGBAClip(desc, "Bg", true)->setLabels("bg", "bg", "bg");
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("Despill");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("screenMode");
        p->setLabel("screen colour");
        p->setHint("auto: measured on the image. pick: the colour below. clean plate: per pixel from the c input "
                   "(DifScreenColour output), for uneven screens.");
        p->appendOption("auto", "", "auto");
        p->appendOption("pick", "", "pick");
        p->appendOption("clean plate (c)", "", "plate");
        p->setDefault(eScreenAuto);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*p);
    }
    APFXOfx::divider( defineColour(desc, page, "screenColour", "", "Screen colour (pick: on clean screen).", 0.1, 0.6, 0.15) );
    APFXOfx::defineDouble(desc, page, "amount", "amount", "How much of the spill is removed.", 1., 0., 1., 0., 2.);
    APFXOfx::defineDouble(desc, page, "range", "range", "Higher: also removes spill from pixels that are less screen-coloured.", 1., 0., 3., 0., 20.);
    APFXOfx::defineDouble(desc, page, "threshold", "threshold", "Spill weaker than this (fraction of the screen's strength) is left alone.", 0., 0., 0.5, -1., 1.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "protect", "protect", "Keeps colours near the screen colour (cyan / yellow on green, skin) from changing. 0 = strongest despill.", 0.7, 0., 1., 0., 1.) );
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "luminance", "luminance", "1 = brightness kept, 0 = the spill's light is removed too (darker edges).", 1., 0., 1., 0., 1.) );
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("replace");
        p->setLabel("replace with");
        p->setHint("What goes where the spill was: nothing (neutral), a colour, or the colours of the bg input (blurred).");
        p->appendOption("nothing", "", "none");
        p->appendOption("colour", "", "colour");
        p->appendOption("bg input", "", "bg");
        p->setDefault(eReplaceNone);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*p);
    }
    defineColour(desc, page, "replaceColour", "", "Replacement colour.", 0.5, 0.5, 0.5);
    APFXOfx::defineDouble(desc, page, "replaceAmount", "replace amount", "Strength of the replacement.", 1., 0., 1., 0., 2.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "bgBlur", "bg blur", "Blur of the bg input before its colours are used (pixels).", 20., 0., 100., 0., 1000.) );
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "edges", "edges only", "1 = only the semi-transparent edges of the alpha are despilled (a solid core keeps its colour).", 0., 0., 1., 0., 1.) );
    defineOutput(desc, page);
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
DespillVectorFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new DespillVectorPlugin(h);
}
} // anon namespace

static DespillProFactory apfxDespillProFactory("com.aguapanelafx.APFX.DespillPro", 1, 0);
mRegisterPluginFactoryInstance(apfxDespillProFactory)
static DespillToColourFactory apfxDespillToColourFactory("com.aguapanelafx.APFX.DespillToColour", 1, 0);
mRegisterPluginFactoryInstance(apfxDespillToColourFactory)
static DespillVectorFactory apfxDespillVectorFactory("com.aguapanelafx.APFX.Despill", 1, 0);
mRegisterPluginFactoryInstance(apfxDespillVectorFactory)
