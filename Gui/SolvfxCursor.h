/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (a Natron fork) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX cursor: a dark arrow with a thin white outline, used everywhere instead of the
// system arrow and instead of the cross (node graph, curve editor, dope sheet...).
// Resize / text / hand cursors stay the system ones (they are hints).

#ifndef Gui_SolvfxCursor_h
#define Gui_SolvfxCursor_h

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>
#include <QWidget>

namespace SolvfxCursor {

inline QCursor
arrow()
{
    static QCursor* c = 0;
    if (!c) {
        const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.;
        const qreal s = 1.25; // arrow ~23 px tall (logical)
        const int w = (int)( 18 * s ), h = (int)( 26 * s );
        QPixmap pm( (int)(w * dpr), (int)(h * dpr) );
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(s, s);
        p.translate(0.6, 0.6);
        QPainterPath path;
        path.moveTo(1, 1);
        path.lineTo(1, 18.5);
        path.lineTo(5.2, 14.6);
        path.lineTo(8.1, 21.2);
        path.lineTo(10.9, 20);
        path.lineTo(8.1, 13.6);
        path.lineTo(13.8, 13.6);
        path.closeSubpath();
        QPen pen(QColor(255, 255, 255), 0.9);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush( QColor(0x1b, 0x1b, 0x1b) );
        p.drawPath(path);
        p.end();
        c = new QCursor( pm, (int)(1.6 * s), (int)(1.6 * s) );
    }

    return *c;
}

// Gives every window the SOLVFX arrow (children inherit it unless they set their own)
class WindowFilter
    : public QObject
{
public:
    explicit WindowFilter(QObject* parent) : QObject(parent) {}

protected:
    virtual bool eventFilter(QObject* o, QEvent* e) override
    {
        if ( e->type() == QEvent::Show ) {
            QWidget* w = qobject_cast<QWidget*>(o);
            if ( w && w->isWindow() && !w->testAttribute(Qt::WA_SetCursor) ) {
                w->setCursor( arrow() );
            }
        }

        return false;
    }
};

inline void
install()
{
    if (qApp) {
        qApp->installEventFilter( new WindowFilter(qApp) );
    }
}
} // namespace SolvfxCursor

#endif // Gui_SolvfxCursor_h
