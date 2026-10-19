/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AnimatedCheckBox.h"

#include <stdexcept>

#include <QStyle>
#include <QPainter>
#include <algorithm>
#include <QPainterPath>
#include <QStyleOption>
#include "Gui/GuiMacros.h"
// clang-format off
CLANG_DIAG_OFF(deprecated-register) //'register' storage class specifier is deprecated
GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QMouseEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
CLANG_DIAG_ON(deprecated-register)
// clang-format on

#include "Gui/GuiApplicationManager.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER
AnimatedCheckBox::AnimatedCheckBox(QWidget *parent)
    : QFrame(parent), animation(0), readOnly(false), dirty(false), altered(false), checked(false)
{
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy( QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed, QSizePolicy::Label) );
    setMouseTracking(true);
    setFrameShape(QFrame::Box);
}

void
AnimatedCheckBox::setAnimation(int i)
{
    animation = i;
    update();
}

void
AnimatedCheckBox::setReadOnly(bool readOnly)
{
    this->readOnly = readOnly;
    update();
}

void
AnimatedCheckBox::setDirty(bool b)
{
    dirty = b;
    update();
}

QSize
AnimatedCheckBox::minimumSizeHint() const
{
    return QSize( TO_DPIX(15), TO_DPIY(15) );
}

QSize
AnimatedCheckBox::sizeHint() const
{
    return QSize( TO_DPIX(15), TO_DPIY(15) );
}

void
AnimatedCheckBox::setChecked(bool c)
{
    checked = c;
    Q_EMIT toggled(checked);
    update();
}

void
AnimatedCheckBox::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Space) {
        checked = !checked;
        Q_EMIT toggled(checked);
        Q_EMIT clicked(checked);
        update();
        e->accept();
    } else {
        QFrame::keyPressEvent(e);
    }
}

void
AnimatedCheckBox::mousePressEvent(QMouseEvent* e)
{
    if ( buttonDownIsLeft(e) ) {
        if (readOnly) {
            return;
        }
        checked = !checked;
        update();
        Q_EMIT clicked(checked);
        Q_EMIT toggled(checked);
    } else {
        QFrame::mousePressEvent(e);
    }
}

void
AnimatedCheckBox::getBackgroundColor(double *r,
                                     double *g,
                                     double *b) const
{
    appPTR->getCurrentSettings()->getRaisedColor(r, g, b);
}

void
AnimatedCheckBox::paintEvent(QPaintEvent* e)
{
    // SOLVFX: rounded square checkbox (3px corners). Channel checkboxes (custom
    // color) are tinted by channel; checked = filled with a dark check mark.
    QFrame::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const double side = std::min( width(), height() ) - 2.;
    QRectF box( (width() - side) / 2., (height() - side) / 2., side, side );

    double tr = 0., tg = 0., tb = 0.;
    getBackgroundColor(&tr, &tg, &tb);
    QColor raised;
    {
        double rr, rg, rb;
        appPTR->getCurrentSettings()->getRaisedColor(&rr, &rg, &rb);
        raised.setRgbF(rr, rg, rb);
    }
    QColor tint;
    tint.setRgbF(tr, tg, tb);
    const bool hasTint = (tint != raised);
    QColor onColor = hasTint ? tint : QColor(0xe0, 0x74, 0x3a);

    // field states: animated / keyframe / expression / linked
    QColor fill(0x26, 0x26, 0x26);
    QColor border = hasTint ? tint : QColor(0x14, 0x14, 0x14);
    if (animation == 1) {
        fill = QColor(0x6b, 0x4a, 0x2a);
        onColor = QColor(0xe0, 0x74, 0x3a);
    } else if (animation == 2) {
        fill = QColor(0xe0, 0x74, 0x3a);
        onColor = QColor(0xe0, 0x74, 0x3a);
    } else if (animation == 3) {
        fill = QColor(0x24, 0x52, 0x4a);
        onColor = QColor(0x5f, 0xd3, 0xbc);
    }
    if (readOnly) {
        onColor = onColor.darker(160);
        border = border.darker(160);
    }
    if (checked) {
        fill = onColor;
    }
    if (dirty) {
        fill = Qt::black;
    }

    QPen pen(border);
    pen.setWidthF( hasTint ? 1.5 : 1. );
    p.setPen(pen);
    p.setBrush(fill);
    p.drawRoundedRect(box.adjusted(0.75, 0.75, -0.75, -0.75), 3., 3.);

    if (checked) {
        QPen check( QColor(0x14, 0x14, 0x14) );
        check.setWidthF( std::max(1.5, side / 8.) );
        check.setCapStyle(Qt::RoundCap);
        check.setJoinStyle(Qt::RoundJoin);
        p.setPen(check);
        QPainterPath path;
        path.moveTo( box.left() + side * 0.22, box.top() + side * 0.53 );
        path.lineTo( box.left() + side * 0.42, box.top() + side * 0.72 );
        path.lineTo( box.left() + side * 0.78, box.top() + side * 0.30 );
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    if ( hasFocus() ) {
        QPen focus( QColor(0xe0, 0x74, 0x3a) );
        focus.setWidthF(1.);
        p.setPen(focus);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box.adjusted(-1., -1., 1., 1.), 4., 4.);
    }
} // AnimatedCheckBox::paintEvent

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_AnimatedCheckBox.cpp"
