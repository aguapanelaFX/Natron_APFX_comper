/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (a Natron fork) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX: engine-side playback counters, printed by the debug console (Gui/SolvfxConsole)

#ifndef Engine_SolvfxPerf_h
#define Engine_SolvfxPerf_h

#include <atomic>

namespace SolvfxPerf {

enum CounterEnum
{
    eTilesCached = 0,   // viewer tiles found in the playback cache
    eTilesRendered,     // viewer tiles that had to be rendered
    eRenderNs,          // time spent rendering viewer frames (tree + conversion), all threads
    eRenderedFrames,    // viewer frames that needed a render
    eCount
};

inline std::atomic<long long>&
counter(int i)
{
    static std::atomic<long long> c[eCount];

    return c[i];
}

inline void
add(int i, long long v)
{
    counter(i).fetch_add(v, std::memory_order_relaxed);
}

// returns and resets
inline long long
take(int i)
{
    return counter(i).exchange(0, std::memory_order_relaxed);
}
} // namespace SolvfxPerf

#endif // Engine_SolvfxPerf_h
