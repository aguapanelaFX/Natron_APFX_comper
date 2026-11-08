/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Remove: like Nuke's Remove.
//  keep (default): only the chosen rgba channels go through, every other layer is removed
//  remove: the chosen rgba channels are set to 0, every other layer goes through

#include <cstring>
#include <list>
#include <vector>
#include <string>

#include "APFXOfxCommon.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Remove"

enum { eOpKeep = 0, eOpRemove };
enum { eChRGBA = 0, eChRGB, eChAlpha };

class APFXRemovePlugin
    : public ImageEffect
{
public:
    APFXRemovePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _op = fetchChoiceParam("operation");
        _ch = fetchChoiceParam("channels");
    }

private:
    // channels of rgba selected by the 'channels' menu
    void selected(double t, bool sel[4])
    {
        const int ch = _ch->getValueAtTime(t);
        sel[0] = sel[1] = sel[2] = (ch == eChRGBA) || (ch == eChRGB);
        sel[3] = (ch == eChRGBA) || (ch == eChAlpha);
    }

    OfxStatus getClipComponents(const ClipComponentsArguments& args, ClipComponentsSetter& cs) OVERRIDE FINAL
    {
        cs.addClipPlane(*_dst, kFnOfxImagePlaneColour);
        cs.addClipPlane(*_src, kFnOfxImagePlaneColour);
        if ( (_op->getValueAtTime(args.time) == eOpRemove) && _src->isConnected() ) {
            // every other layer goes through
            std::vector<std::string> present;
            _src->getPlanesPresent(&present);
            for (std::vector<std::string>::iterator it = present.begin(); it != present.end(); ++it) {
                if (*it == kFnOfxImagePlaneColour) {
                    continue;
                }
                cs.addClipPlane(*_dst, *it);
                cs.addClipPlane(*_src, *it);
            }
        }
        cs.setPassThroughClip(_src, args.time, args.view);

        return kOfxStatOK;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
    }

    bool isIdentity(const IsIdentityArguments& /*args*/, Clip*& /*identityClip*/, double& /*identityTime*/
#ifdef OFX_EXTENSIONS_NUKE
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        return false;
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        const double t = args.time;
        bool sel[4];
        selected(t, sel);
        const bool keep = _op->getValueAtTime(t) == eOpKeep;
        const OfxRectI win = args.renderWindow;
        for (std::list<std::string>::const_iterator pl = args.planes.begin(); pl != args.planes.end(); ++pl) {
            Image* dst = _dst->fetchImagePlane( t, pl->c_str() );
            Image* src = _src->isConnected() ? _src->fetchImagePlane( t, pl->c_str() ) : NULL;
            if (!dst) {
                delete src;
                continue;
            }
            const bool colour = (*pl == kFnOfxImagePlaneColour);
            const int dnc = dst->getPixelComponentCount();
            const int snc = src ? src->getPixelComponentCount() : 0;
            for (int y = win.y1; y < win.y2; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                for (int x = win.x1; x < win.x2; ++x, d += dnc) {
                    const float* s = src ? (const float*)src->getPixelAddress(x, y) : NULL;
                    for (int c = 0; c < dnc; ++c) {
                        float v = ( s && (c < snc) ) ? s[c] : 0.f;
                        if (colour) {
                            // rgba channel index (RGB images: 0..2, alpha only: 3)
                            const int ci = (dnc == 1) ? 3 : c;
                            const bool on = sel[ci];
                            if ( (keep && !on) || (!keep && on) ) {
                                v = 0.f;
                            }
                        }
                        d[c] = v;
                    }
                }
            }
            delete src;
            delete dst;
        }
    }

    Clip* _dst;
    Clip* _src;
    ChoiceParam* _op;
    ChoiceParam* _ch;
};

mDeclarePluginFactory(APFXRemoveFactory, {}, {});

void
APFXRemoveFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Remove",
                            "APFX Remove (like Nuke's Remove). Keep (default): only the chosen rgba channels go through and every "
                            "other layer is removed. Remove: the chosen rgba channels are set to 0 and every other layer goes through.");
    desc.setPluginGrouping("Channel");
    desc.setSupportsTiles(true);
#ifdef OFX_EXTENSIONS_NUKE
    desc.setIsMultiPlanar(true);
    desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelBlockAllNonRenderedPlanes);
#endif
}

void
APFXRemoveFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
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
    PageParamDescriptor* page = desc.definePageParam("Remove");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("operation");
        p->setLabel("operation");
        p->appendOption("keep", "Only the chosen channels go through, every other layer is removed.");
        p->appendOption("remove", "The chosen channels are set to 0, every other layer goes through.");
        p->setDefault(eOpKeep);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("channels");
        p->setLabel("channels");
        p->appendOption("rgba");
        p->appendOption("rgb");
        p->appendOption("alpha");
        p->setDefault(eChRGBA);
        p->setAnimates(false);
        page->addChild(*p);
    }
}

ImageEffect*
APFXRemoveFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXRemovePlugin(h);
}
} // anon namespace

static APFXRemoveFactory apfxRemoveFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxRemoveFactory)
