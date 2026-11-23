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
#include <QPolygonF>

#include <algorithm>

NATRON_NAMESPACE_ENTER

NodeGraphRectItem::NodeGraphRectItem(QGraphicsItem *parent,
                                     int cornerRadiusPx)
: QGraphicsRectItem(parent)
, _cornerRadiusPx(cornerRadiusPx)
, _solvfxLook(false)
, _shape(eSolvfxShapeRect)
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
        { 0x7f9bcf, 0xa9c2ee }, // color (light blue)
        { 0x7792c6, 0xa9c2ee }, // color (old default)
        { 0xd4844a, 0xf2ad74 }, // filter
        { 0x4c62bc, 0x6c80d8 }, // merge
        { 0xad80c8, 0xd3a9ea }, // transform
        { 0x62b862, 0x8bd98b }, // draw
        { 0x1cbf3a, 0x3fe35c }, // keyer
        { 0x98406d, 0xc06a97 }, // channel (magenta)
        { 0x8f8d55, 0xb6b47d }, // time (olive)
        { 0xa89c7f, 0xc9bea3 }, // time (old default)
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

QPainterPath
NodeGraphRectItem::solvfxPath(const QRectF& r) const
{
    QPainterPath p;
    const double h = r.height();
    switch (_shape) {
    case eSolvfxShapeHexagon: {
        const double k = std::min( h * 0.42, r.width() / 4. );
        QPolygonF poly;
        poly << QPointF(r.left() + k, r.top()) << QPointF(r.right() - k, r.top()) << QPointF( r.right(), r.center().y() )
             << QPointF(r.right() - k, r.bottom()) << QPointF(r.left() + k, r.bottom()) << QPointF( r.left(), r.center().y() );
        p.addPolygon(poly);
        p.closeSubpath();
        break;
    }
    case eSolvfxShapeCapsule:
        p.addRoundedRect( r, h / 2., h / 2. );
        break;
    case eSolvfxShapeCircle:
        p.addEllipse(r);
        break;
    case eSolvfxShapePointRight: {
        const double k = std::min( h * 0.45, r.width() / 4. );
        const double rad = std::min( (double)_cornerRadiusPx, h / 2. );
        p.moveTo(r.left() + rad, r.top());
        p.lineTo(r.right() - k, r.top());
        p.lineTo( r.right(), r.center().y() );
        p.lineTo(r.right() - k, r.bottom());
        p.lineTo(r.left() + rad, r.bottom());
        p.quadTo( r.bottomLeft(), QPointF(r.left(), r.bottom() - rad) );
        p.lineTo(r.left(), r.top() + rad);
        p.quadTo( r.topLeft(), QPointF(r.left() + rad, r.top()) );
        p.closeSubpath();
        break;
    }
    default: {
        const double radius = std::min( (double)_cornerRadiusPx, std::min( r.width(), r.height() ) / 2. );
        p.addRoundedRect(r, radius, radius);
        break;
    }
    }

    return p;
}

void
NodeGraphRectItem::paint(QPainter *painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/)
{
    const QRectF r = rect();
    double radius = std::min( (double)_cornerRadiusPx, std::min( r.width(), r.height() ) / 2. );

    if ( !_solvfxLook || (brush().style() != Qt::SolidPattern) ) {
        painter->setPen( pen() );
        painter->setBrush( brush() );
        painter->drawRoundedRect(r, radius, radius);

        return;
    }

    // SOLVFX node look: vertical gradient, lighter top, shape by category
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
    painter->drawPath( solvfxPath( r.adjusted(hw, hw, -hw, -hw) ) );
    if (_selectedLook) {
        // SOLVFX: thin dark ring inside the yellow outline so it reads on light nodes
        QPen ring( QColor(0x14, 0x14, 0x14) );
        ring.setWidthF(1.);
        painter->setPen(ring);
        painter->setBrush(Qt::NoBrush);
        const double in = pen().widthF() + 0.5;
        painter->drawPath( solvfxPath( r.adjusted(in, in, -in, -in) ) );
    }
}

NATRON_NAMESPACE_EXIT
