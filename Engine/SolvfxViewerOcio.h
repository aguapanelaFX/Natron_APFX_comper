/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (a Natron fork) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX: the viewer's OCIO display/view transform on the CPU, used by the 8-bit
// viewer (Nuke-like "byte" buffer): the cached viewer frames are display-ready
// 8-bit, 4x smaller than float, so playback caches and uploads much faster.

#ifndef Engine_SolvfxViewerOcio_h
#define Engine_SolvfxViewerOcio_h

#include <memory>
#include <string>

class SolvfxViewerOcio
{
public:
    // null if the transform cannot be built (no OCIO, bad display/view)
    static std::shared_ptr<SolvfxViewerOcio> create(const std::string& display, const std::string& view);

    // n RGBA float pixels in place (scene linear -> display); thread-safe
    void applyRGBA(float* pixels, long n) const;

    ~SolvfxViewerOcio();

private:
    SolvfxViewerOcio();
    struct Imp;
    std::unique_ptr<Imp> _imp;
};

#endif // Engine_SolvfxViewerOcio_h
