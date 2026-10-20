/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (based on Natron) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * SOLVFX is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef SOLVFX_GUI_SOLVFXCONSOLE_H
#define SOLVFX_GUI_SOLVFXCONSOLE_H

// SOLVFX debug console: a console window next to the app that only shows
// "[SOLVFX] ..." lines (playback timing, node graph stalls).

#include <QtCore/QElapsedTimer>
#include <QtCore/QString>

namespace SolvfxConsole {

enum PerfKindEnum
{
    ePerfUpload = 0,   // viewer texture upload (RAM -> GPU)
    ePerfViewer,       // viewer paint
    ePerfTimeline,     // timeline paint
    ePerfNodeGraph,    // node graph paint
    ePerfCount
};

// SOLVFX: the UI thread runs one step above normal priority so the node graph, viewer
// and timeline stay responsive while all cores render (render threads stay normal).
// Call once from the GUI thread.
void raiseUiThreadPriority();

// Opens the console window (Windows) and starts the monitor. Call once from the GUI thread.
void open();

bool isOpen();

// Prints "[SOLVFX] <line>"
void log(const QString& line);

// Time spent on the GUI thread
void addTime(PerfKindEnum kind, qint64 nsecs);

// A new image reached the viewer for this frame
void frameShown(int frame);

// The user is panning / zooming / dragging in the node graph
void nodeGraphInteraction(const char* what);

class ScopedTimer
{
public:
    explicit ScopedTimer(PerfKindEnum kind)
        : _kind(kind)
        , _on( isOpen() )
    {
        if (_on) {
            _t.start();
        }
    }

    ~ScopedTimer()
    {
        if (_on) {
            addTime( _kind, _t.nsecsElapsed() );
        }
    }

private:
    PerfKindEnum _kind;
    bool _on;
    QElapsedTimer _t;
};
} // namespace SolvfxConsole

#endif // SOLVFX_GUI_SOLVFXCONSOLE_H
