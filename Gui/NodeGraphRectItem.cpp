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

#include "NodeGraphRectItem.h"

#include <algorithm>
#include <QPainter>
#include <QLinearGradient>

NATRON_NAMESPACE_ENTER

NodeGraphRectItem::NodeGraphRectItem(QGraphicsItem *parent,
                                     int cornerRadiusPx)
: QGraphicsRectItem(parent)
, _cornerRadiusPx(cornerRadiusPx)
, _solvfxLook(false)
, _pill(false)
, _selectedLook(false)
{
}

QColor
NodeGraphRectItem::solvfxTopColor(const QColor& bottom)
{
    // bottom -> top pairs of the SOLVFX category colors
    static const unsigned int pairs[][2] = {
        { 0xa8a8a8, 0xcfcfcf }, // image I/O
        { 0xd9bd33, 0xf5dc58 }, // write
        { 0x7792c6, 0x9cb5e6 }, // color
        { 0xd4844a, 0xf2ad74 }, // filter
        { 0x4c62bc, 0x6c80d8 }, // merge
        { 0xad80c8, 0xd3a9ea }, // transform
        { 0x62b862, 0x8bd98b }, // draw
        { 0x1cbf3a, 0x3fe35c }, // keyer
        { 0x98406d, 0xac5683 }, // channel
        { 0xa89c7f, 0xc9bea3 }, // time
        { 0x537f35, 0x6f9c4e }, // 3D
        { 0x969696, 0xbcbcbc }, // other
    };
    const unsigned int rgb = bottom.rgb() & 0xffffff;
    for (std::size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
        if (pairs[i][0] == rgb) {
            return QColor( (int)( (pairs[i][1] >> 16) & 0xff ), (int)( (pairs[i][1] >> 8) & 0xff ), (int)(pairs[i][1] & 0xff) );
        }
    }

    return bottom.lighter(118);
}

void
NodeGraphRectItem::paint(QPainter *painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/)
{
    const QRectF r = rect();
    double radius = _pill ? r.height() / 2. : (double)_cornerRadiusPx;
    radius = std::min( radius, std::min( r.width(), r.height() ) / 2. );

    if ( !_solvfxLook || (brush().style() != Qt::SolidPattern) ) {
        painter->setPen( pen() );
        painter->setBrush( brush() );
        painter->drawRoundedRect(r, radius, radius);

        return;
    }

    // SOLVFX node look: vertical gradient, lighter top
    painter->setRenderHint(QPainter::Antialiasing, true);
    QColor bottom = brush().color();
    QColor top = solvfxTopColor(bottom);
    if (_selectedLook) {
        top = top.lighter(112);
        bottom = bottom.lighter(112);
    }
    QLinearGradient grad( r.topLeft(), r.bottomLeft() );
    grad.setColorAt(0., top);
    grad.setColorAt(1., bottom);
    painter->setPen( pen() );
    painter->setBrush(grad);
    const double hw = pen().widthF() / 2.;
    painter->drawRoundedRect(r.adjusted(hw, hw, -hw, -hw), radius, radius);
}

NATRON_NAMESPACE_EXIT
