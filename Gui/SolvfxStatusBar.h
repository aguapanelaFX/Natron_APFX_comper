/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (based on Natron) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * SOLVFX is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef SOLVFX_GUI_SOLVFXSTATUSBAR_H
#define SOLVFX_GUI_SOLVFXSTATUSBAR_H

// SOLVFX: Nuke-like info bar at the bottom of the main window:
// Memory (process / % of RAM) · CPU (process) · GPU (3D load, VRAM) · Disk · Network,
// refreshed once per second.

#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QLabel>

class SolvfxStatusBar
    : public QLabel
{
public:
    explicit SolvfxStatusBar(QWidget* parent = 0);
    virtual ~SolvfxStatusBar();

private:
    void refresh();

    QTimer _timer;
    QElapsedTimer _clock;
    struct Private;
    Private* _p;
};

#endif // SOLVFX_GUI_SOLVFXSTATUSBAR_H
