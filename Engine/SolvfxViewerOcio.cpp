/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (a Natron fork) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include <Python.h>

#include "SolvfxViewerOcio.h"

#include <cstdlib>
#include <iostream>

#ifdef SOLVFX_HAS_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

struct SolvfxViewerOcio::Imp
{
#ifdef SOLVFX_HAS_OCIO
    OCIO::ConstCPUProcessorRcPtr cpu;
#endif
};

SolvfxViewerOcio::SolvfxViewerOcio()
    : _imp(new Imp)
{
}

SolvfxViewerOcio::~SolvfxViewerOcio()
{
}

std::shared_ptr<SolvfxViewerOcio>
SolvfxViewerOcio::create(const std::string& display,
                         const std::string& view)
{
#ifdef SOLVFX_HAS_OCIO
    if ( display.empty() ) {
        return std::shared_ptr<SolvfxViewerOcio>();
    }
    const char* env = std::getenv("OCIO");
    if ( !env || !env[0] ) {
        return std::shared_ptr<SolvfxViewerOcio>();
    }
    try {
        OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromEnv();
        OCIO::DisplayViewTransformRcPtr dv = OCIO::DisplayViewTransform::Create();
        dv->setSrc(OCIO::ROLE_SCENE_LINEAR);
        dv->setDisplay( display.c_str() );
        dv->setView( view.c_str() );
        OCIO::ConstProcessorRcPtr processor = config->getProcessor(dv);
        std::shared_ptr<SolvfxViewerOcio> ret( new SolvfxViewerOcio() );
        ret->_imp->cpu = processor->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_DEFAULT);

        return ret;
    } catch (const std::exception& e) {
        std::cerr << "[SOLVFX] OCIO viewer (CPU): " << e.what() << std::endl;
    }
#else
    (void)display;
    (void)view;
#endif

    return std::shared_ptr<SolvfxViewerOcio>();
}

void
SolvfxViewerOcio::applyRGBA(float* pixels,
                            long n) const
{
#ifdef SOLVFX_HAS_OCIO
    if ( !_imp->cpu || (n <= 0) ) {
        return;
    }
    OCIO::PackedImageDesc img(pixels, n, 1, 4);
    _imp->cpu->apply(img);
#else
    (void)pixels;
    (void)n;
#endif
}
