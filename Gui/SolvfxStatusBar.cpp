/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (based on Natron) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * SOLVFX is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include "SolvfxStatusBar.h"

#include <QtCore/QThread>
#include <QtCore/QFile>
#include <QtCore/QTextStream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0A00)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // GetIfTable2, DXGI 1.4
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include <iphlpapi.h>
#include <netioapi.h>
#else
#include <unistd.h>
#include <sys/resource.h>
#endif

#include <string>
#include <vector>

struct SolvfxStatusBar::Private
{
    qint64 lastMs = 0;
    unsigned long long lastCpu = 0;   // process cpu time, 100 ns (win) / us (posix)
    unsigned long long lastIo = 0;    // process disk bytes
    unsigned long long lastNet = 0;   // system network bytes
    bool first = true;
#ifdef _WIN32
    PDH_HQUERY gpuQuery = NULL;
    PDH_HCOUNTER gpuCounter = NULL;
    IDXGIAdapter3* adapter = NULL;
#endif
};

static QString
fmtBytes(double b)
{
    const double gb = b / (1024. * 1024. * 1024.);
    if (gb >= 1.) {
        return QString::number(gb, 'f', 1) + QString::fromUtf8(" GB");
    }

    return QString::number(b / (1024. * 1024.), 'f', 0) + QString::fromUtf8(" MB");
}

SolvfxStatusBar::SolvfxStatusBar(QWidget* parent)
    : QLabel(parent)
    , _p(new Private)
{
    setObjectName( QString::fromUtf8("SolvfxStatusBar") );
    setTextFormat(Qt::RichText);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setStyleSheet( QString::fromUtf8("QLabel#SolvfxStatusBar { color: #9a9a9a; padding: 0px 8px; font-size: 11px; }") );
#ifdef _WIN32
    // GPU 3D engine load of this process (Windows 10 1709+ performance counters)
    if (PdhOpenQueryW(NULL, 0, &_p->gpuQuery) == ERROR_SUCCESS) {
        std::wstring path = L"\\GPU Engine(pid_" + std::to_wstring( GetCurrentProcessId() ) + L"*engtype_3D)\\Utilization Percentage";
        if (PdhAddEnglishCounterW(_p->gpuQuery, path.c_str(), 0, &_p->gpuCounter) != ERROR_SUCCESS) {
            _p->gpuCounter = NULL;
        }
        PdhCollectQueryData(_p->gpuQuery);
    }
    // VRAM of this process on the first adapter
    IDXGIFactory1* factory = NULL;
    if ( SUCCEEDED( CreateDXGIFactory1( __uuidof(IDXGIFactory1), (void**)&factory ) ) && factory ) {
        IDXGIAdapter1* a = NULL;
        if ( (factory->EnumAdapters1(0, &a) == S_OK) && a ) {
            a->QueryInterface( __uuidof(IDXGIAdapter3), (void**)&_p->adapter );
            a->Release();
        }
        factory->Release();
    }
#endif
    _clock.start();
    QObject::connect( &_timer, &QTimer::timeout, this, [this]() { refresh(); } );
    _timer.start(1000);
    refresh();
}

SolvfxStatusBar::~SolvfxStatusBar()
{
#ifdef _WIN32
    if (_p->gpuQuery) {
        PdhCloseQuery(_p->gpuQuery);
    }
    if (_p->adapter) {
        _p->adapter->Release();
    }
#endif
    delete _p;
}

void
SolvfxStatusBar::refresh()
{
    const qint64 now = _clock.elapsed();
    const double dt = std::max(0.001, (now - _p->lastMs) / 1000.);
    const int nCores = std::max(1, QThread::idealThreadCount());
    QStringList parts;
    const QString sep = QString::fromUtf8("&nbsp;&nbsp;");
    auto item = [](const QString& k, const QString& v) {
        return QString::fromUtf8("%1: <span style='color:#c8c8c8'>%2</span>").arg(k, v);
    };

#ifdef _WIN32
    // memory
    PROCESS_MEMORY_COUNTERS_EX pmc;
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if ( GetProcessMemoryInfo( GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc) ) && GlobalMemoryStatusEx(&ms) ) {
        const double used = (double)pmc.WorkingSetSize;
        parts << item( tr("Memory"), QString::fromUtf8("%1 (%2%)").arg( fmtBytes(used) ).arg(100. * used / (double)ms.ullTotalPhys, 0, 'f', 1) );
    }
    // cpu (process, all cores = 100%)
    FILETIME c, e, k, u;
    unsigned long long cpu = 0;
    if ( GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ) {
        cpu = ( ( (unsigned long long)k.dwHighDateTime << 32 ) | k.dwLowDateTime ) + ( ( (unsigned long long)u.dwHighDateTime << 32 ) | u.dwLowDateTime );
    }
    if (!_p->first) {
        const double pct = 100. * ( (cpu - _p->lastCpu) / 1e7 ) / (dt * nCores);
        parts << item( tr("CPU"), QString::number(std::min(100., pct), 'f', 1) + QString::fromUtf8("%") );
    } else {
        parts << item( tr("CPU"), QString::fromUtf8("-") );
    }
    _p->lastCpu = cpu;
    // gpu
    {
        QString g;
        if (_p->gpuQuery && _p->gpuCounter && PdhCollectQueryData(_p->gpuQuery) == ERROR_SUCCESS) {
            DWORD size = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(_p->gpuCounter, PDH_FMT_DOUBLE, &size, &count, NULL) == (PDH_STATUS)PDH_MORE_DATA && size > 0) {
                std::vector<unsigned char> buf(size);
                PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                if (PdhGetFormattedCounterArrayW(_p->gpuCounter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                    double sum = 0.;
                    for (DWORD i = 0; i < count; ++i) {
                        if (items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                            sum += items[i].FmtValue.doubleValue;
                        }
                    }
                    g = QString::number(std::min(100., sum), 'f', 0) + QString::fromUtf8("%");
                }
            } else {
                g = QString::fromUtf8("0%");
            }
        }
        if (_p->adapter) {
            DXGI_QUERY_VIDEO_MEMORY_INFO vm;
            if ( SUCCEEDED( _p->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm) ) ) {
                if ( !g.isEmpty() ) {
                    g += QString::fromUtf8(" · ");
                }
                g += tr("VRAM ") + fmtBytes( (double)vm.CurrentUsage );
            }
        }
        if ( !g.isEmpty() ) {
            parts << item(tr("GPU"), g);
        }
    }
    // disk (this process)
    IO_COUNTERS io;
    unsigned long long ioBytes = 0;
    if ( GetProcessIoCounters(GetCurrentProcess(), &io) ) {
        ioBytes = io.ReadTransferCount + io.WriteTransferCount;
    }
    // network (all adapters)
    unsigned long long net = 0;
    MIB_IF_TABLE2* table = NULL;
    if ( (GetIfTable2(&table) == NO_ERROR) && table ) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& r = table->Table[i];
            if (r.InterfaceAndOperStatusFlags.HardwareInterface && r.OperStatus == IfOperStatusUp) {
                net += r.InOctets + r.OutOctets;
            }
        }
        FreeMibTable(table);
    }
#else
    // posix fallback: memory (rss) and cpu of this process
    long rssPages = 0;
    {
        QFile f( QString::fromUtf8("/proc/self/statm") );
        if ( f.open(QIODevice::ReadOnly) ) {
            QTextStream ts(&f);
            long sizePages = 0;
            ts >> sizePages >> rssPages;
        }
    }
    parts << item( tr("Memory"), fmtBytes( (double)rssPages * sysconf(_SC_PAGESIZE) ) );
    struct rusage ru;
    unsigned long long cpu = 0;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        cpu = (unsigned long long)ru.ru_utime.tv_sec * 1000000ULL + ru.ru_utime.tv_usec + (unsigned long long)ru.ru_stime.tv_sec * 1000000ULL + ru.ru_stime.tv_usec;
    }
    if (!_p->first) {
        parts << item( tr("CPU"), QString::number(std::min(100., 100. * ( (cpu - _p->lastCpu) / 1e6 ) / (dt * nCores) ), 'f', 1) + QString::fromUtf8("%") );
    }
    _p->lastCpu = cpu;
    unsigned long long ioBytes = 0, net = 0;
#endif
    if (!_p->first) {
        parts << item( tr("Disk"), QString::number( (ioBytes - _p->lastIo) / dt / (1024. * 1024.), 'f', 1 ) + QString::fromUtf8(" MB/s") );
        parts << item( tr("Network"), QString::number( (net - _p->lastNet) / dt / (1024. * 1024.), 'f', 1 ) + QString::fromUtf8(" MB/s") );
    }
    _p->lastIo = ioBytes;
    _p->lastNet = net;
    _p->lastMs = now;
    _p->first = false;
    setText( parts.join(sep) );
}
