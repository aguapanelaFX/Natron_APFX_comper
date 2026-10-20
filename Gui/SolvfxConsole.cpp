/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (based on Natron) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * SOLVFX is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include "SolvfxConsole.h"

#include <algorithm>
#include <cstdio>

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>

#include "Global/Macros.h"

#ifdef __NATRON_WIN32__
#include <windows.h>
#endif

namespace SolvfxConsole {

namespace {

struct State
{
    bool opened = false;
#ifdef __NATRON_WIN32__
    HANDLE out = INVALID_HANDLE_VALUE;
#endif
    QElapsedTimer clock;   // started on open()

    // playback window (about 1 s) and whole session
    qint64 winStartMs = -1;
    int winFrames = 0;
    qint64 winNs[ePerfCount] = {0, 0, 0, 0};
    int sessFrames = 0;
    qint64 sessStartMs = -1;
    qint64 sessNs[ePerfCount] = {0, 0, 0, 0};
    qint64 lastFrameMs = -1;
    int lastFrame = 0;
    bool haveLastFrame = false;

    // time spent since the last heartbeat (node graph stalls)
    qint64 beatNs[ePerfCount] = {0, 0, 0, 0};
    qint64 lastBeatMs = 0;
    qint64 lastInteractionMs = -100000;
    const char* lastInteraction = "pan";
    qint64 lastStallPrintMs = -100000;
};

State&
st()
{
    static State s;

    return s;
}

double
ms(qint64 ns)
{
    return ns / 1.0e6;
}

QString
fmtMs(double v)
{
    return QString::number(v, 'f', v < 10. ? 1 : 0) + QString::fromUtf8("ms");
}

void
resetWindow(qint64 nowMs)
{
    State& s = st();
    s.winStartMs = nowMs;
    s.winFrames = 0;
    for (int i = 0; i < ePerfCount; ++i) {
        s.winNs[i] = 0;
    }
}

void
heartbeat()
{
    State& s = st();
    const qint64 now = s.clock.elapsed();
    const qint64 gap = now - s.lastBeatMs;
    s.lastBeatMs = now;

    // Node graph: the event loop was blocked while the user was moving around in it
    if ( (gap > 50) && (now - s.lastInteractionMs < 600) && (now - s.lastStallPrintMs > 200) ) {
        s.lastStallPrintMs = now;
        log( QString::fromUtf8("nodegraph slow: %1 %2ms (busy: upload %3  viewer %4  timeline %5  nodegraph %6)")
             .arg( QString::fromUtf8(s.lastInteraction) )
             .arg(gap)
             .arg( fmtMs( ms(s.beatNs[ePerfUpload]) ) )
             .arg( fmtMs( ms(s.beatNs[ePerfViewer]) ) )
             .arg( fmtMs( ms(s.beatNs[ePerfTimeline]) ) )
             .arg( fmtMs( ms(s.beatNs[ePerfNodeGraph]) ) ) );
    }
    for (int i = 0; i < ePerfCount; ++i) {
        s.beatNs[i] = 0;
    }

    // Playback: one line per second while frames come in
    if ( (s.winFrames > 0) && (now - s.winStartMs >= 1000) ) {
        const double secs = (now - s.winStartMs) / 1000.;
        const double n = s.winFrames;
        log( QString::fromUtf8("play %1  fps %2 | upload %3  viewer %4  timeline %5  nodegraph %6")
             .arg(s.lastFrame)
             .arg( QString::number(n / secs, 'f', 1) )
             .arg( fmtMs(ms(s.winNs[ePerfUpload]) / n) )
             .arg( fmtMs(ms(s.winNs[ePerfViewer]) / n) )
             .arg( fmtMs(ms(s.winNs[ePerfTimeline]) / n) )
             .arg( fmtMs(ms(s.winNs[ePerfNodeGraph]) / n) ) );
        resetWindow(now);
    }
    // Playback stopped: summary
    if ( (s.sessFrames > 1) && (s.lastFrameMs >= 0) && (now - s.lastFrameMs > 700) ) {
        const double secs = std::max(0.001, (s.lastFrameMs - s.sessStartMs) / 1000.);
        const double n = s.sessFrames;
        log( QString::fromUtf8("play stopped: %1 frames  avg fps %2 | per frame: upload %3  viewer %4  timeline %5  nodegraph %6")
             .arg(s.sessFrames)
             .arg( QString::number( (n - 1) / secs, 'f', 1 ) )
             .arg( fmtMs(ms(s.sessNs[ePerfUpload]) / n) )
             .arg( fmtMs(ms(s.sessNs[ePerfViewer]) / n) )
             .arg( fmtMs(ms(s.sessNs[ePerfTimeline]) / n) )
             .arg( fmtMs(ms(s.sessNs[ePerfNodeGraph]) / n) ) );
    }
    if ( (s.sessFrames > 0) && (s.lastFrameMs >= 0) && (now - s.lastFrameMs > 700) ) {
        s.sessFrames = 0;
        s.sessStartMs = -1;
        for (int i = 0; i < ePerfCount; ++i) {
            s.sessNs[i] = 0;
        }
        s.winFrames = 0;
        s.lastFrameMs = -1;
    }
}
} // anon namespace

void
raiseUiThreadPriority()
{
#ifdef __NATRON_WIN32__
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

void
open()
{
    State& s = st();
    if (s.opened) {
        return;
    }
#ifdef __NATRON_WIN32__
    if ( !::GetConsoleWindow() ) {
        if ( !::AllocConsole() ) {
            return;
        }
    }
    ::SetConsoleTitleW(L"SOLVFX console");
    // Closing the console would kill SOLVFX: remove its close button
    HWND w = ::GetConsoleWindow();
    if (w) {
        HMENU m = ::GetSystemMenu(w, FALSE);
        if (m) {
            ::DeleteMenu(m, SC_CLOSE, MF_BYCOMMAND);
        }
    }
    // Ctrl+C in the console must not kill SOLVFX either
    ::SetConsoleCtrlHandler(NULL, TRUE);
    s.out = ::CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (s.out == INVALID_HANDLE_VALUE) {
        return;
    }
#endif
    s.opened = true;
    s.clock.start();
    s.lastBeatMs = 0;

    QTimer* beat = new QTimer( QCoreApplication::instance() );
    beat->setTimerType(Qt::PreciseTimer);
    QObject::connect( beat, &QTimer::timeout, [] () { heartbeat(); } );
    beat->start(16);

    log( QString::fromUtf8("console ready (playback and node graph timing)") );
} // open

bool
isOpen()
{
    return st().opened;
}

void
log(const QString& line)
{
    State& s = st();
    if (!s.opened) {
        return;
    }
    const QString text = QString::fromUtf8("[SOLVFX] ") + line + QString::fromUtf8("\r\n");
#ifdef __NATRON_WIN32__
    DWORD written = 0;
    ::WriteConsoleW( s.out, (const wchar_t*)text.utf16(), (DWORD)text.size(), &written, NULL );
#else
    std::fputs(text.toUtf8().constData(), stderr);
#endif
}

void
addTime(PerfKindEnum kind,
        qint64 nsecs)
{
    State& s = st();
    if ( !s.opened || (kind < 0) || (kind >= ePerfCount) ) {
        return;
    }
    s.winNs[kind] += nsecs;
    s.sessNs[kind] += nsecs;
    s.beatNs[kind] += nsecs;
}

void
frameShown(int frame)
{
    State& s = st();
    if (!s.opened) {
        return;
    }
    if ( s.haveLastFrame && (frame == s.lastFrame) ) {
        return; // same frame redrawn (zoom, pan...), not playback
    }
    const qint64 now = s.clock.elapsed();
    s.haveLastFrame = true;
    s.lastFrame = frame;
    s.lastFrameMs = now;
    if (s.sessFrames == 0) {
        s.sessStartMs = now;
        resetWindow(now);
        for (int i = 0; i < ePerfCount; ++i) {
            s.sessNs[i] = 0;
        }
    }
    ++s.sessFrames;
    ++s.winFrames;
}

void
nodeGraphInteraction(const char* what)
{
    State& s = st();
    if (!s.opened) {
        return;
    }
    s.lastInteractionMs = s.clock.elapsed();
    s.lastInteraction = what;
}
} // namespace SolvfxConsole
