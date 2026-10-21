/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX EdgeExtend: takes the edge from the incoming alpha and pushes the edge pixels
// outward along the edge normals (UV / vector edge extend: nearest-core lookup through an
// exact distance transform, O(pixels)), then puts the source over it.

#include <cmath>

#include "APFXOfxCommon.h"
#include "APFXKeyOps.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.EdgeExtend"

class APFXEdgeExtendPlugin
    : public ImageEffect
{
public:
    APFXEdgeExtendPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _std.fetch(*this, true);
        _intensity = fetchDoubleParam("intensity");
        _erodeIncoming = fetchBooleanParam("erodeIncomingAlpha");
        _width = fetchDoubleParam("width");
        _edgeBlur = fetchDoubleParam("edgeBlur");
        _output = fetchChoiceParam("output");
        _pullInward = fetchBooleanParam("pullInward");
        _pullInwardBlur = fetchDoubleParam("pullInwardBlur");
        _erodeAlpha = fetchBooleanParam("erodeAlpha");
        update();
    }

private:
    void update()
    {
        _width->setEnabled( _erodeIncoming->getValue() );
        _pullInwardBlur->setEnabled( _pullInward->getValue() );
    }

    void changedParam(const InstanceChangedArgs&, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == "erodeIncomingAlpha") || (n == "pullInward") ) {
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
        // the fill can come from anywhere in the frame
        rois.setRegionOfInterest( *_src, _src->getRegionOfDefinition(args.time) );
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setClipBitDepth(*_dst, eBitDepthFloat);
        // "edge extended" is unpremultiplied colour
        cp.setOutputPremultiplication( _output->getValue() == APFXKey::eEdgeOutputExtended ? eImageUnPreMultiplied : eImagePreMultiplied );
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
        APFXKey::EdgeExtendParams p;
        p.intensity = (float)_intensity->getValueAtTime(t);
        p.erodeIncoming = _erodeIncoming->getValueAtTime(t);
        p.width = (int)std::floor(_width->getValueAtTime(t) * rs + 0.5);
        p.edgeBlur = (float)(_edgeBlur->getValueAtTime(t) * rs);
        p.output = _output->getValueAtTime(t);
        p.pullInward = _pullInward->getValueAtTime(t);
        p.pullInwardBlur = (float)(_pullInwardBlur->getValueAtTime(t) * rs);
        p.erodeAlpha = _erodeAlpha->getValueAtTime(t);
        APFXKey::edgeExtend( in, p, out, APFXOfx::hostParallelFor() );

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
    DoubleParam* _intensity;
    BooleanParam* _erodeIncoming;
    DoubleParam* _width;
    DoubleParam* _edgeBlur;
    ChoiceParam* _output;
    BooleanParam* _pullInward;
    DoubleParam* _pullInwardBlur;
    BooleanParam* _erodeAlpha;
};

mDeclarePluginFactory(APFXEdgeExtendFactory, {}, {});

void
APFXEdgeExtendFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "EdgeExtend",
                            "APFX EdgeExtend: takes the edge from the incoming (eroded) alpha and pushes those edge "
                            "pixels outward, perpendicular to the edge, like a vector blur. The source goes over the "
                            "extension, so soft edges (hair, motion blur) get clean edge colours behind them.\n"
                            "Output 'edge extended' is unpremultiplied: premultiply it by the alpha afterwards.");
}

void
APFXEdgeExtendFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("EdgeExtend");
    APFXOfx::defineChannels(desc, page, true);
    APFXOfx::defineDouble(desc, page, "intensity", "intensity", "How much of the extension is applied.", 1., 0., 1., 0., 1.);
    APFXOfx::defineBool(desc, page, "erodeIncomingAlpha", "erode incoming alpha", "Erode the alpha before taking the edge, so contaminated edge pixels are not pushed out.", true);
    APFXOfx::defineDouble(desc, page, "width", "width", "Erode width in pixels.", 2., 0., 20., 0., 500.);
    APFXOfx::defineDouble(desc, page, "edgeBlur", "edge blur", "Smooths the pushed edge colours (along the edge and the streaks), in pixels.", 2., 0., 10., 0., 500.);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
        p->setLabel("output");
        p->setHint("edge extended: the source over its pushed edge colours (unpremultiplied) + alpha. edges only: just the extension where it shows. area affected: where the extension shows.");
        p->appendOption("edge extended", "", "extended");
        p->appendOption("edges only", "", "edges");
        p->appendOption("area affected", "", "area");
        p->setDefault(0);
        p->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*p);
        page->addChild(*p);
    }
    APFXOfx::defineBool(desc, page, "pullInward", "pull inward", "Also replace the eroded band inside the alpha with the clean core colour.", false);
    APFXOfx::defineDouble(desc, page, "pullInwardBlur", "pull inward edge blur", "Softness of the inward band, in pixels.", 2., 0., 10., 0., 500.);
    APFXOfx::defineBool(desc, page, "erodeAlpha", "erode alpha", "Output the eroded alpha instead of the original.", false);
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXEdgeExtendFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXEdgeExtendPlugin(h);
}
} // anon namespace

static APFXEdgeExtendFactory apfxEdgeExtendFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxEdgeExtendFactory)
