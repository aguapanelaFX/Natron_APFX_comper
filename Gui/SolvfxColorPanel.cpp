/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX, based on Natron <https://natrongithub.github.io/>,
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

#include "SolvfxColorPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#ifdef SOLVFX_HAS_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

#include "Engine/AppManager.h" // TO_DPIX
#include "Gui/SpinBox.h"

#define SOLVFX_COLORPANEL_SETTINGS "SOLVFX/ColorPanel"
#define SOLVFX_PALETTE_SIZE 12

NATRON_NAMESPACE_ENTER

// ---------------------------------------------------------------------------
// color helpers
// ---------------------------------------------------------------------------

static double
srgbEncode(double c)
{
    c = std::max(0., std::min(1., c));

    return (c <= 0.0031308) ? 12.92 * c : 1.055 * std::pow(c, 1. / 2.4) - 0.055;
}

static double
srgbDecode(double c)
{
    return (c <= 0.04045) ? c / 12.92 : std::pow( (c + 0.055) / 1.055, 2.4 );
}

#ifdef SOLVFX_HAS_OCIO
// sRGB (8-bit, like a texture or a brand color) <-> scene linear of the current OCIO config
struct SolvfxHexOCIO
{
    std::string configEnv;
    OCIO::ConstCPUProcessorRcPtr toLinear;
    OCIO::ConstCPUProcessorRcPtr fromLinear;
    bool valid = false;

    bool initialized = false;

    void ensure()
    {
        const char* env = std::getenv("OCIO");
        std::string e = env ? env : "";
        if ( initialized && (e == configEnv) ) {
            return;
        }
        initialized = true;
        configEnv = e;
        valid = false;
        toLinear.reset();
        fromLinear.reset();
        if ( e.empty() ) {
            return;
        }
        try {
            OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromEnv();
            const char* candidates[] = {
                OCIO::ROLE_MATTE_PAINT,
                "Utility - sRGB - Texture",
                "sRGB - Texture",
                "sRGB Encoded Rec.709 (sRGB)",
                "srgb_texture",
                "sRGB",
                "srgb8",
                "sRGB (D65)",
                OCIO::ROLE_COLOR_PICKING,
                0
            };
            const char* src = 0;
            for (int i = 0; candidates[i]; ++i) {
                if ( config->getColorSpace(candidates[i]) ) {
                    src = candidates[i];
                    break;
                }
            }
            if ( !src || !config->getColorSpace(OCIO::ROLE_SCENE_LINEAR) ) {
                return;
            }
            toLinear = config->getProcessor(src, OCIO::ROLE_SCENE_LINEAR)->getDefaultCPUProcessor();
            fromLinear = config->getProcessor(OCIO::ROLE_SCENE_LINEAR, src)->getDefaultCPUProcessor();
            valid = true;
        } catch (const std::exception&) {
            valid = false;
            toLinear.reset();
            fromLinear.reset();
        }
    }
};

static SolvfxHexOCIO&
hexOCIO()
{
    static SolvfxHexOCIO s;

    s.ensure();

    return s;
}
#endif // SOLVFX_HAS_OCIO

// hex (display sRGB) -> scene linear
static bool
hexToLinear(const QString& text, double* r, double* g, double* b)
{
    QString t = text.trimmed();
    if ( t.startsWith( QLatin1Char('#') ) ) {
        t = t.mid(1);
    }
    if (t.size() == 3) { // #RGB shorthand
        QString e;
        for (int i = 0; i < 3; ++i) {
            e += t[i];
            e += t[i];
        }
        t = e;
    }
    if (t.size() != 6) {
        return false;
    }
    bool ok = false;
    unsigned int v = t.toUInt(&ok, 16);
    if (!ok) {
        return false;
    }
    float rgb[3] = { ( (v >> 16) & 0xff ) / 255.f, ( (v >> 8) & 0xff ) / 255.f, (v & 0xff) / 255.f };
#ifdef SOLVFX_HAS_OCIO
    SolvfxHexOCIO& o = hexOCIO();
    if (o.valid) {
        try {
            o.toLinear->applyRGB(rgb);
            *r = rgb[0]; *g = rgb[1]; *b = rgb[2];

            return true;
        } catch (const std::exception&) {
        }
    }
#endif
    *r = srgbDecode(rgb[0]);
    *g = srgbDecode(rgb[1]);
    *b = srgbDecode(rgb[2]);

    return true;
}

// scene linear -> hex (display sRGB, clipped)
static QString
linearToHex(double r, double g, double b)
{
    float rgb[3] = { (float)r, (float)g, (float)b };
    bool done = false;
#ifdef SOLVFX_HAS_OCIO
    SolvfxHexOCIO& o = hexOCIO();
    if (o.valid) {
        try {
            o.fromLinear->applyRGB(rgb);
            done = true;
        } catch (const std::exception&) {
        }
    }
#endif
    if (!done) {
        rgb[0] = srgbEncode(r);
        rgb[1] = srgbEncode(g);
        rgb[2] = srgbEncode(b);
    }
    QString s = QString::fromUtf8("#");
    for (int i = 0; i < 3; ++i) {
        int c = (int)std::floor(std::max(0.f, std::min(1.f, rgb[i])) * 255.f + 0.5f);
        s += QString::fromUtf8("%1").arg(c, 2, 16, QLatin1Char('0')).toUpper();
    }

    return s;
}

// SOLVFX display-mode hex (UI colors: no color management)
static bool
hexToDisplay(const QString& text, double* r, double* g, double* b)
{
    QString t = text.trimmed();
    if ( t.startsWith( QLatin1Char('#') ) ) {
        t = t.mid(1);
    }
    if (t.size() == 3) {
        QString e;
        for (int i = 0; i < 3; ++i) {
            e += t[i];
            e += t[i];
        }
        t = e;
    }
    if (t.size() != 6) {
        return false;
    }
    bool ok = false;
    unsigned int v = t.toUInt(&ok, 16);
    if (!ok) {
        return false;
    }
    *r = ( (v >> 16) & 0xff ) / 255.;
    *g = ( (v >> 8) & 0xff ) / 255.;
    *b = (v & 0xff) / 255.;

    return true;
}

static QString
displayToHex(double r, double g, double b)
{
    const double c[3] = { r, g, b };
    QString s = QString::fromUtf8("#");
    for (int i = 0; i < 3; ++i) {
        int v = (int)std::floor(std::max(0., std::min(1., c[i])) * 255. + 0.5);
        s += QString::fromUtf8("%1").arg(v, 2, 16, QLatin1Char('0')).toUpper();
    }

    return s;
}

QColor
SolvfxColorPanel::toDisplay(double r,
                            double g,
                            double b) const
{
    QColor c;

    if (_displayMode) {
        c.setRgbF( std::max(0., std::min(1., r)), std::max(0., std::min(1., g)), std::max(0., std::min(1., b)) );

        return c;
    }
    c.setRgbF( srgbEncode(r), srgbEncode(g), srgbEncode(b) );

    return c;
}

void
SolvfxColorPanel::rgbToHsv(double r,
                           double g,
                           double b,
                           double* h,
                           double* s,
                           double* v)
{
    double mx = std::max( r, std::max(g, b) );
    double mn = std::min( r, std::min(g, b) );
    double d = mx - mn;

    *v = mx;
    *s = (mx > 0.) ? d / mx : 0.;
    if (d <= 0.) {
        *h = 0.;

        return;
    }
    double hh;
    if (mx == r) {
        hh = (g - b) / d;
    } else if (mx == g) {
        hh = 2. + (b - r) / d;
    } else {
        hh = 4. + (r - g) / d;
    }
    hh /= 6.;
    if (hh < 0.) {
        hh += 1.;
    }
    *h = hh;
}

static void
solvfxHsvToRgb(double h,
               double s,
               double v,
               double* r,
               double* g,
               double* b)
{
    if (s <= 0.) {
        *r = *g = *b = v;

        return;
    }
    h = h - std::floor(h);
    double hh = h * 6.;
    int i = (int)std::floor(hh);
    double f = hh - i;
    double p = v * (1. - s);
    double q = v * (1. - s * f);
    double t = v * ( 1. - s * (1. - f) );
    switch (i % 6) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

void
SolvfxColorPanel::hsvToRgb(double h,
                           double s,
                           double v,
                           double* r,
                           double* g,
                           double* b)
{
    solvfxHsvToRgb(h, s, v, r, g, b);
}

// TMI (Nuke-like): I = average, T = blue/red balance, M = magenta/green balance
static void
rgbToTmi(double r, double g, double b, double* t, double* m, double* i)
{
    *i = (r + g + b) / 3.;
    *t = (b - r) / 2.;
    *m = (r + b) / 2. - g;
}

static void
tmiToRgb(double t, double m, double i, double* r, double* g, double* b)
{
    *g = i - (2. / 3.) * m;
    *r = i + m / 3. - t;
    *b = i + m / 3. + t;
}

// ---------------------------------------------------------------------------
// SolvfxGradientSlider
// ---------------------------------------------------------------------------

SolvfxGradientSlider::SolvfxGradientSlider(ScaleEnum scale,
                                           QWidget* parent)
    : QWidget(parent)
    , _scale(scale)
    , _value(0.)
    , _stops()
    , _dragging(false)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
}

QSize
SolvfxGradientSlider::sizeHint() const
{
    return QSize( TO_DPIX(340), TO_DPIY(20) );
}

QSize
SolvfxGradientSlider::minimumSizeHint() const
{
    return QSize( TO_DPIX(160), TO_DPIY(20) );
}

void
SolvfxGradientSlider::setValue(double v)
{
    if (v != _value) {
        _value = v;
        update();
    }
}

void
SolvfxGradientSlider::setGradient(const QGradientStops& stops)
{
    _stops = stops;
    update();
}

// Nuke-like slider scales (square-root spacing, measured on Nuke's panel)
double
SolvfxGradientSlider::posFromValue(ScaleEnum scale,
                                   double v)
{
    switch (scale) {
    case eScaleLinear01:

        return std::max( 0., std::min(1., v) );
    case eScaleUnsigned4:
        // 0 -> 0, 1 -> 0.5, 4 -> 1

        return 0.5 * std::sqrt( std::max( 0., std::min(4., v) ) );
    case eScaleSigned4:
    default: {
        // -4 -> 0, 0 -> 0.5, 1 -> 0.75, 4 -> 1
        double a = 0.25 * std::sqrt( std::min( 4., std::abs(v) ) );

        return 0.5 + (v < 0. ? -a : a);
    }
    }
}

double
SolvfxGradientSlider::valueFromPos(ScaleEnum scale,
                                   double p)
{
    p = std::max( 0., std::min(1., p) );
    switch (scale) {
    case eScaleLinear01:

        return p;
    case eScaleUnsigned4: {
        double a = 2. * p;

        return a * a;
    }
    case eScaleSigned4:
    default: {
        double d = (p - 0.5) * 4.;

        return (d < 0. ? -1. : 1.) * d * d;
    }
    }
}

void
SolvfxGradientSlider::paintEvent(QPaintEvent* /*e*/)
{
    QPainter p(this);
    QRectF r = QRectF( rect() ).adjusted(0.5, 0.5, -0.5, -0.5);

    // gradient bar
    QLinearGradient grad( r.topLeft(), r.topRight() );
    grad.setStops(_stops);
    p.fillRect(r, grad);
    p.setPen( QColor(20, 20, 20) );
    p.drawRect(r);

    // ticks and labels, drawn on the bar like Nuke
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.68);
    p.setFont(f);
    QFontMetrics fm(f);
    std::vector<std::pair<double, QString> > ticks;
    if (_scale == eScaleLinear01) {
        for (int i = 0; i <= 10; ++i) {
            ticks.push_back( std::make_pair( i / 10., QString::number(i / 10.) ) );
        }
    } else if (_scale == eScaleUnsigned4) {
        const double vals[] = { 0.1, 0.2, 0.4, 0.6, 0.8, 1., 2., 3., 4. };
        for (int i = 0; i < 9; ++i) {
            // 1 is the center: tick without label, like Nuke
            ticks.push_back( std::make_pair( vals[i], vals[i] == 1. ? QString() : QString::number(vals[i]) ) );
        }
    } else {
        const double vals[] = { -4., -3., -2., -1., -0.5, -0.1, 0., 0.1, 0.5, 1., 2., 3., 4. };
        for (int i = 0; i < 13; ++i) {
            ticks.push_back( std::make_pair( vals[i], vals[i] == 0. ? QString() : QString::number(vals[i]) ) );
        }
    }
    const double w = r.width();
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        double x = r.left() + posFromValue(ticks[i].first) * w;
        // tick color: dark on light parts, light on dark parts
        QColor bg = Qt::gray;
        {
            double pos = posFromValue(ticks[i].first);
            for (int k = 0; k + 1 < _stops.size(); ++k) {
                if ( (pos >= _stops[k].first) && (pos <= _stops[k + 1].first) ) {
                    double span = std::max(1e-6, _stops[k + 1].first - _stops[k].first);
                    double tt = (pos - _stops[k].first) / span;
                    QColor c0 = _stops[k].second, c1 = _stops[k + 1].second;
                    bg.setRgbF( c0.redF() + (c1.redF() - c0.redF()) * tt,
                                c0.greenF() + (c1.greenF() - c0.greenF()) * tt,
                                c0.blueF() + (c1.blueF() - c0.blueF()) * tt );
                    break;
                }
            }
        }
        QColor ink = (bg.lightnessF() > 0.55) ? QColor(20, 20, 20) : QColor(225, 225, 225);
        p.setPen(ink);
        p.drawLine( QPointF(x, r.top() + 2), QPointF(x, r.top() + r.height() * 0.38) );
        const QString& label = ticks[i].second;
        if ( label.isEmpty() ) {
            continue;
        }
        int tw = fm.horizontalAdvance(label);
        int tx = (int)std::floor(x - tw / 2. + 0.5);
        tx = std::max( (int)r.left() + 1, std::min(tx, (int)r.right() - tw - 1) );
        p.drawText( tx, (int)r.bottom() - 1, label );
    }

    // value marker (white line with dark outline)
    double mx = r.left() + posFromValue(_value) * w;
    p.setPen( QPen(QColor(10, 10, 10), 4.) );
    p.drawLine( QPointF(mx, r.top()), QPointF(mx, r.bottom()) );
    p.setPen( QPen(QColor(245, 245, 245), 2.) );
    p.drawLine( QPointF(mx, r.top()), QPointF(mx, r.bottom()) );
}

void
SolvfxGradientSlider::setFromMouse(int x)
{
    double p = (x - 0.5) / std::max(1., width() - 1.);
    double v = valueFromPos(p);

    // snap to 0 near the center of signed sliders
    if ( (_scale == eScaleSigned4) && (std::abs(v) < 0.002) ) {
        v = 0.;
    } else if ( (_scale == eScaleUnsigned4) && (std::abs(v - 1.) < 0.01) ) {
        v = 1.; // snap to the center
    }
    _value = v;
    update();
    Q_EMIT valueDragged(v);
}

void
SolvfxGradientSlider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        _dragging = true;
        setFromMouse( e->pos().x() );
    }
    e->accept();
}

void
SolvfxGradientSlider::mouseMoveEvent(QMouseEvent* e)
{
    if (_dragging) {
        setFromMouse( e->pos().x() );
    }
    e->accept();
}

void
SolvfxGradientSlider::mouseReleaseEvent(QMouseEvent* e)
{
    if (_dragging) {
        _dragging = false;
        Q_EMIT dragFinished();
    }
    e->accept();
}

// ---------------------------------------------------------------------------
// SolvfxColorWheel
// ---------------------------------------------------------------------------

SolvfxColorWheel::SolvfxColorWheel(QWidget* parent)
    : QWidget(parent)
    , _h(0.)
    , _s(0.)
    , _image()
{
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setCursor(Qt::CrossCursor);
}

QSize
SolvfxColorWheel::sizeHint() const
{
    return QSize( TO_DPIX(270), TO_DPIY(270) );
}

QRectF
SolvfxColorWheel::wheelRect() const
{
    double side = std::min( width(), height() ) - TO_DPIX(16); // room for the hue arrow
    QRectF r(0, 0, side, side);
    r.moveCenter( QRectF( rect() ).center() );

    return r;
}

void
SolvfxColorWheel::setHueSat(double h,
                            double s)
{
    if ( (h != _h) || (s != _s) ) {
        _h = h;
        _s = s;
        update();
    }
}

void
SolvfxColorWheel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    rebuildImage();
}

void
SolvfxColorWheel::rebuildImage()
{
    QRectF wr = wheelRect();
    int d = std::max(2, (int)wr.width() );
    _image = QImage(d, d, QImage::Format_ARGB32_Premultiplied);
    _image.fill(Qt::transparent);
    double rad = d / 2.;
    double ring = rad * 0.10; // darker outer ring like Nuke
    for (int y = 0; y < d; ++y) {
        QRgb* line = (QRgb*)_image.scanLine(y);
        for (int x = 0; x < d; ++x) {
            double dx = x + 0.5 - rad;
            double dy = rad - (y + 0.5);
            double dist = std::sqrt(dx * dx + dy * dy);
            if (dist > rad) {
                continue;
            }
            double hue = std::atan2(dy, dx) / (2. * M_PI);
            if (hue < 0.) {
                hue += 1.;
            }
            double sat = std::min(1., dist / (rad - ring) );
            double val = (dist > rad - ring) ? 0.8 : 1.;
            double r, g, b;
            solvfxHsvToRgb(hue, sat, val, &r, &g, &b);
            // antialias the border
            double alpha = std::min(1., rad - dist);
            int R = (int)(r * 255 * alpha + 0.5), G = (int)(g * 255 * alpha + 0.5), B = (int)(b * 255 * alpha + 0.5);
            line[x] = qRgba( R, G, B, (int)(alpha * 255 + 0.5) );
        }
    }
    update();
}

void
SolvfxColorWheel::paintEvent(QPaintEvent* /*e*/)
{
    if ( _image.isNull() ) {
        rebuildImage();
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRectF wr = wheelRect();
    p.drawImage(wr.topLeft(), _image);

    double rad = wr.width() / 2.;
    double ring = rad * 0.10;
    QPointF c = wr.center();
    double ang = _h * 2. * M_PI;
    double sr = std::min(1., std::max(0., _s) ) * (rad - ring);
    QPointF m( c.x() + std::cos(ang) * sr, c.y() - std::sin(ang) * sr );

    // current hue/saturation marker
    p.setPen( QPen(Qt::black, 1.5) );
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(m, TO_DPIX(4), TO_DPIX(4) );
    p.setPen( QPen(Qt::white, 1.) );
    p.drawEllipse(m, TO_DPIX(5.5), TO_DPIX(5.5) );

    // hue arrow on the rim
    double a = TO_DPIX(8);
    QPointF tip( c.x() + std::cos(ang) * (rad - 1), c.y() - std::sin(ang) * (rad - 1) );
    QPointF base( c.x() + std::cos(ang) * (rad + a), c.y() - std::sin(ang) * (rad + a) );
    QPointF perp( -std::sin(ang) * a * 0.6, -std::cos(ang) * a * 0.6 );
    QPainterPath tri;
    tri.moveTo(tip);
    tri.lineTo(base + perp);
    tri.lineTo(base - perp);
    tri.closeSubpath();
    p.setPen( QPen(Qt::black, 1.) );
    p.setBrush(Qt::white);
    p.drawPath(tri);
}

void
SolvfxColorWheel::setFromMouse(const QPoint& pt)
{
    QRectF wr = wheelRect();
    double rad = wr.width() / 2.;
    double ring = rad * 0.10;
    QPointF c = wr.center();
    double dx = pt.x() - c.x();
    double dy = c.y() - pt.y();
    double dist = std::sqrt(dx * dx + dy * dy);
    double h = std::atan2(dy, dx) / (2. * M_PI);

    if (h < 0.) {
        h += 1.;
    }
    double s = std::min(1., dist / std::max(1., rad - ring) );
    _h = h;
    _s = s;
    update();
    Q_EMIT hueSatDragged(h, s);
}

void
SolvfxColorWheel::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        setFromMouse( e->pos() );
    }
    e->accept();
}

void
SolvfxColorWheel::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton) {
        setFromMouse( e->pos() );
    }
    e->accept();
}

// ---------------------------------------------------------------------------
// SolvfxColorPanel
// ---------------------------------------------------------------------------

static SolvfxGradientSlider::ScaleEnum
scaleForRow(int row)
{
    switch (row) {
    case 0: // T
    case 1: // M
        return SolvfxGradientSlider::eScaleSigned4;
    case 3: // H
        return SolvfxGradientSlider::eScaleLinear01;
    default: // I S V R G B A
        return SolvfxGradientSlider::eScaleUnsigned4;
    }
}

static QToolButton*
makeToggle(const QString& text,
           QWidget* parent,
           bool checked)
{
    QToolButton* b = new QToolButton(parent);

    b->setText(text);
    b->setCheckable(true);
    b->setChecked(checked);
    b->setFocusPolicy(Qt::NoFocus);
    b->setAutoRaise(false);
    b->setMinimumHeight( TO_DPIY(20) );

    return b;
}

static void
setSwatchColor(QToolButton* b,
               const QColor& c)
{
    b->setStyleSheet( QString::fromUtf8("QToolButton { background-color: %1; border: 1px solid #111; }").arg( c.name() ) );
}

SolvfxColorPanel::SolvfxColorPanel(bool withAlpha,
                                   QWidget* parent)
    : QWidget(parent)
    , _withAlpha(withAlpha)
    , _displayMode(false)
    , _r(1.), _g(1.), _b(1.), _a(1.)
    , _origR(1.), _origG(1.), _origB(1.), _origA(1.)
    , _lastHue(0.)
    , _currentSwatch(0)
    , _originalSwatch(0)
    , _hexEdit(0)
    , _tmiButton(0)
    , _hsvButton(0)
    , _rgbButton(0)
    , _wheelButton(0)
    , _paletteButton(0)
    , _dynButton(0)
    , _wheel(0)
    , _paletteRow(0)
    , _paletteSwatches()
    , _refreshing(false)
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );
    settings.beginGroup( QString::fromUtf8(SOLVFX_COLORPANEL_SETTINGS) );

    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins( TO_DPIX(6), TO_DPIY(6), TO_DPIX(6), TO_DPIY(6) );
    mainLayout->setSpacing( TO_DPIX(10) );

    QWidget* left = new QWidget(this);
    QVBoxLayout* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing( TO_DPIY(4) );

    // --- top bar: swatches, hex, TMI/HSV/RGB, wheel/palette/Dyn
    QWidget* top = new QWidget(left);
    QHBoxLayout* topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing( TO_DPIX(3) );

    _currentSwatch = new QToolButton(top);
    _currentSwatch->setFixedSize( TO_DPIX(24), TO_DPIY(20) );
    _currentSwatch->setFocusPolicy(Qt::NoFocus);
    _currentSwatch->setToolTip( tr("Current color") );
    _originalSwatch = new QToolButton(top);
    _originalSwatch->setFixedSize( TO_DPIX(24), TO_DPIY(20) );
    _originalSwatch->setFocusPolicy(Qt::NoFocus);
    _originalSwatch->setToolTip( tr("Color when the panel was opened. Click to go back to it.") );
    QObject::connect( _originalSwatch, SIGNAL(clicked()), this, SLOT(onOriginalSwatchClicked()) );
    topLayout->addWidget(_currentSwatch);
    topLayout->addWidget(_originalSwatch);
    topLayout->addSpacing( TO_DPIX(6) );

    _hexEdit = new QLineEdit(top);
    _hexEdit->setFixedWidth( TO_DPIX(84) );
    _hexEdit->setToolTip( tr("Hex color (sRGB, like a brand or web color). It is converted to the working "
                             "linear colorspace with your OCIO config. HDR values above 1 are clipped here.") );
    QObject::connect( _hexEdit, SIGNAL(editingFinished()), this, SLOT(onHexEdited()) );
    topLayout->addWidget(_hexEdit);
    topLayout->addStretch();

    _tmiButton = makeToggle(QString::fromUtf8("TMI"), top, settings.value(QString::fromUtf8("tmi"), true).toBool() );
    _hsvButton = makeToggle(QString::fromUtf8("HSV"), top, settings.value(QString::fromUtf8("hsv"), true).toBool() );
    _rgbButton = makeToggle(QString::fromUtf8("RGB"), top, settings.value(QString::fromUtf8("rgb"), true).toBool() );
    _tmiButton->setToolTip( tr("Show the Temperature / Magenta / Intensity sliders") );
    _hsvButton->setToolTip( tr("Show the Hue / Saturation / Value sliders") );
    _rgbButton->setToolTip( tr("Show the Red / Green / Blue sliders") );
    QObject::connect( _tmiButton, SIGNAL(toggled(bool)), this, SLOT(onToggleGroups()) );
    QObject::connect( _hsvButton, SIGNAL(toggled(bool)), this, SLOT(onToggleGroups()) );
    QObject::connect( _rgbButton, SIGNAL(toggled(bool)), this, SLOT(onToggleGroups()) );
    topLayout->addWidget(_tmiButton);
    topLayout->addWidget(_hsvButton);
    topLayout->addWidget(_rgbButton);
    topLayout->addStretch();

    _wheelButton = makeToggle(QString::fromUtf8("●"), top, settings.value(QString::fromUtf8("wheel"), true).toBool() );
    _wheelButton->setToolTip( tr("Show the color wheel") );
    _paletteButton = makeToggle(QString::fromUtf8("▦"), top, settings.value(QString::fromUtf8("palette"), false).toBool() );
    _paletteButton->setToolTip( tr("Show the color swatches. Click a swatch to use it, right-click to store the current color in it.") );
    _dynButton = makeToggle(QString::fromUtf8("Dyn"), top, settings.value(QString::fromUtf8("dyn"), false).toBool() );
    _dynButton->setToolTip( tr("Dynamic slider backgrounds: show what each slider would do to the current color") );
    QObject::connect( _wheelButton, SIGNAL(toggled(bool)), this, SLOT(onWheelButtonToggled(bool)) );
    QObject::connect( _paletteButton, SIGNAL(toggled(bool)), this, SLOT(onPaletteButtonToggled(bool)) );
    QObject::connect( _dynButton, SIGNAL(toggled(bool)), this, SLOT(onDynToggled(bool)) );
    topLayout->addWidget(_wheelButton);
    topLayout->addWidget(_paletteButton);
    topLayout->addWidget(_dynButton);

    leftLayout->addWidget(top);

    // --- rows T M I H S V R G B A
    QWidget* rowsWidget = new QWidget(left);
    QGridLayout* grid = new QGridLayout(rowsWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing( TO_DPIX(4) );
    grid->setVerticalSpacing( TO_DPIY(3) );
    const char* names[eRowCount] = { "T", "M", "I", "H", "S", "V", "R", "G", "B", "A" };
    const char* tips[eRowCount] = {
        "Temperature: orange (negative) to blue (positive)",
        "Magenta: green (negative) to magenta (positive)",
        "Intensity: average of red, green and blue",
        "Hue",
        "Saturation",
        "Value: the largest of red, green and blue",
        "Red", "Green", "Blue", "Alpha"
    };
    for (int i = 0; i < eRowCount; ++i) {
        Row& row = _rows[i];
        row.label = new QToolButton(rowsWidget);
        row.label->setText( QString::fromUtf8(names[i]) );
        row.label->setToolTip( tr(tips[i]) );
        row.label->setFocusPolicy(Qt::NoFocus);
        row.label->setPopupMode(QToolButton::InstantPopup);
        row.label->setFixedSize( TO_DPIX(30), TO_DPIY(20) );
        {
            QMenu* menu = new QMenu(row.label);
            QAction* zero = menu->addAction( tr("Set to 0") );
            zero->setData( QVariant(i * 10 + 0) );
            QAction* one = menu->addAction( tr("Set to 1") );
            one->setData( QVariant(i * 10 + 1) );
            QObject::connect( zero, SIGNAL(triggered()), this, SLOT(onRowMenuAction()) );
            QObject::connect( one, SIGNAL(triggered()), this, SLOT(onRowMenuAction()) );
            row.label->setMenu(menu);
        }
        row.spin = new SpinBox(rowsWidget, SpinBox::eSpinBoxTypeDouble);
        row.spin->setFixedWidth( TO_DPIX(60) );
        row.spin->setIncrement(0.01);
        row.spin->setProperty("solvfxRow", i);
        QObject::connect( row.spin, SIGNAL(valueChanged(double)), this, SLOT(onRowSpinChanged(double)) );
        row.slider = new SolvfxGradientSlider(scaleForRow(i), rowsWidget);
        row.slider->setProperty("solvfxRow", i);
        row.slider->setToolTip( tr(tips[i]) );
        QObject::connect( row.slider, SIGNAL(valueDragged(double)), this, SLOT(onRowSliderDragged(double)) );
        grid->addWidget(row.label, i, 0);
        grid->addWidget(row.spin, i, 1);
        grid->addWidget(row.slider, i, 2);
    }
    grid->setColumnStretch(2, 1);
    leftLayout->addWidget(rowsWidget);

    // --- swatch palette (hidden by default)
    _paletteRow = new QWidget(left);
    QHBoxLayout* palLayout = new QHBoxLayout(_paletteRow);
    palLayout->setContentsMargins(0, 0, 0, 0);
    palLayout->setSpacing( TO_DPIX(3) );
    for (int i = 0; i < SOLVFX_PALETTE_SIZE; ++i) {
        QToolButton* sw = new QToolButton(_paletteRow);
        sw->setFixedSize( TO_DPIX(22), TO_DPIY(18) );
        sw->setFocusPolicy(Qt::NoFocus);
        sw->setProperty("solvfxSwatch", i);
        sw->setContextMenuPolicy(Qt::CustomContextMenu);
        QString key = QString::fromUtf8("swatch%1").arg(i);
        QStringList v = settings.value(key).toStringList();
        if (v.size() == 3) {
            sw->setProperty( "r", v[0].toDouble() );
            sw->setProperty( "g", v[1].toDouble() );
            sw->setProperty( "b", v[2].toDouble() );
        } else {
            double grey = i / (double)(SOLVFX_PALETTE_SIZE - 1);
            grey = srgbDecode(grey);
            sw->setProperty("r", grey);
            sw->setProperty("g", grey);
            sw->setProperty("b", grey);
        }
        setSwatchColor( sw, toDisplay( sw->property("r").toDouble(), sw->property("g").toDouble(), sw->property("b").toDouble() ) );
        QObject::connect( sw, SIGNAL(clicked()), this, SLOT(onPaletteSwatchClicked()) );
        QObject::connect( sw, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(onPaletteSwatchStore()) );
        palLayout->addWidget(sw);
        _paletteSwatches.push_back(sw);
    }
    palLayout->addStretch();
    leftLayout->addWidget(_paletteRow);
    leftLayout->addStretch();

    mainLayout->addWidget(left, 1);

    // --- wheel
    _wheel = new SolvfxColorWheel(this);
    _wheel->setFixedSize( TO_DPIX(270), TO_DPIY(270) );
    QObject::connect( _wheel, SIGNAL(hueSatDragged(double,double)), this, SLOT(onWheelDragged(double,double)) );
    mainLayout->addWidget(_wheel, 0, Qt::AlignTop);

    settings.endGroup();

    if (!_withAlpha) {
        _rows[eRowA].label->hide();
        _rows[eRowA].spin->hide();
        _rows[eRowA].slider->hide();
    }
    _wheel->setVisible( _wheelButton->isChecked() );
    _paletteRow->setVisible( _paletteButton->isChecked() );
    refreshVisibility();
    refreshAll(-1);
}

void
SolvfxColorPanel::getColor(float* r,
                           float* g,
                           float* b,
                           float* a)
{
    *r = (float)_r;
    *g = (float)_g;
    *b = (float)_b;
    *a = (float)_a;
}

void
SolvfxColorPanel::setColor(float r,
                           float g,
                           float b,
                           float a)
{
    _r = r; _g = g; _b = b; _a = a;
    refreshAll(-1);
}

double
SolvfxColorPanel::channelValue(int row) const
{
    double t, m, i, h, s, v;

    rgbToTmi(_r, _g, _b, &t, &m, &i);
    rgbToHsv(_r, _g, _b, &h, &s, &v);
    if (s <= 0.) {
        h = _lastHue;
    }
    switch (row) {
    case eRowT: return t;
    case eRowM: return m;
    case eRowI: return i;
    case eRowH: return h;
    case eRowS: return s;
    case eRowV: return v;
    case eRowR: return _r;
    case eRowG: return _g;
    case eRowB: return _b;
    case eRowA: return _a;
    default: return 0.;
    }
}

void
SolvfxColorPanel::setChannel(int row,
                             double val,
                             bool doAnnounce)
{
    double t, m, i, h, s, v;

    rgbToTmi(_r, _g, _b, &t, &m, &i);
    rgbToHsv(_r, _g, _b, &h, &s, &v);
    if (s <= 0.) {
        h = _lastHue;
    }
    switch (row) {
    case eRowT: tmiToRgb(val, m, i, &_r, &_g, &_b); break;
    case eRowM: tmiToRgb(t, val, i, &_r, &_g, &_b); break;
    case eRowI: tmiToRgb(t, m, val, &_r, &_g, &_b); break;
    case eRowH: _lastHue = val - std::floor(val); hsvToRgb(_lastHue, s, v, &_r, &_g, &_b); break;
    case eRowS: hsvToRgb(h, val, v, &_r, &_g, &_b); break;
    case eRowV: hsvToRgb(h, s, val, &_r, &_g, &_b); break;
    case eRowR: _r = val; break;
    case eRowG: _g = val; break;
    case eRowB: _b = val; break;
    case eRowA: _a = val; break;
    default: break;
    }
    refreshAll(row);
    if (doAnnounce) {
        announce();
    }
}

void
SolvfxColorPanel::refreshAll(int skipRow)
{
    _refreshing = true;
    double h, s, v;
    rgbToHsv(_r, _g, _b, &h, &s, &v);
    if (s > 0.) {
        _lastHue = h;
    }
    (void)skipRow;
    for (int i = 0; i < eRowCount; ++i) {
        double val = channelValue(i);
        _rows[i].spin->setValue(val);
        _rows[i].slider->setValue(val);
    }
    _wheel->setHueSat( (s > 0.) ? h : _lastHue, s );
    setSwatchColor( _currentSwatch, toDisplay(_r, _g, _b) );
    setSwatchColor( _originalSwatch, toDisplay(_origR, _origG, _origB) );
    if ( !_hexEdit->hasFocus() ) {
        _hexEdit->setText( _displayMode ? displayToHex(_r, _g, _b) : linearToHex(_r, _g, _b) );
    }
    refreshGradients();
    _refreshing = false;
}

void
SolvfxColorPanel::refreshGradients()
{
    const bool dyn = _dynButton->isChecked();

    for (int row = 0; row < eRowCount; ++row) {
        QGradientStops stops;
        if (dyn && row != eRowA) {
            // sample what the slider would do to the current color
            const int N = 24;
            SolvfxGradientSlider::ScaleEnum scale = scaleForRow(row);
            double sr = _r, sg = _g, sb = _b, sa = _a, lh = _lastHue;
            for (int k = 0; k <= N; ++k) {
                double p = k / (double)N;
                double v = SolvfxGradientSlider::valueFromPos(scale, p);
                // evaluate on a copy of the state
                double t, m, i, h, s, vv, r = sr, g = sg, b = sb;
                rgbToTmi(sr, sg, sb, &t, &m, &i);
                rgbToHsv(sr, sg, sb, &h, &s, &vv);
                if (s <= 0.) {
                    h = lh;
                }
                switch (row) {
                case eRowT: tmiToRgb(v, m, i, &r, &g, &b); break;
                case eRowM: tmiToRgb(t, v, i, &r, &g, &b); break;
                case eRowI: tmiToRgb(t, m, v, &r, &g, &b); break;
                case eRowH: hsvToRgb(v, std::max(s, 0.5), std::max(vv, 0.5), &r, &g, &b); break;
                case eRowS: hsvToRgb(h, v, vv, &r, &g, &b); break;
                case eRowV: hsvToRgb(h, s, v, &r, &g, &b); break;
                case eRowR: r = v; break;
                case eRowG: g = v; break;
                case eRowB: b = v; break;
                default: break;
                }
                stops.push_back( qMakePair( p, toDisplay(r, g, b) ) );
            }
            (void)sa;
        } else {
            const QColor black(0, 0, 0), white(235, 235, 235), grey(185, 185, 185);
            switch (row) {
            case eRowT:
                stops << qMakePair(0., QColor(240, 160, 40) ) << qMakePair(0.5, grey) << qMakePair(1., QColor(40, 170, 245) );
                break;
            case eRowM:
                stops << qMakePair(0., QColor(120, 225, 100) ) << qMakePair(0.5, grey) << qMakePair(1., QColor(225, 40, 225) );
                break;
            case eRowH:
                for (int k = 0; k <= 12; ++k) {
                    double r, g, b;
                    hsvToRgb(k / 12., 1., 1., &r, &g, &b);
                    QColor c;
                    c.setRgbF(r, g, b);
                    stops << qMakePair(k / 12., c);
                }
                break;
            case eRowS:
                stops << qMakePair(0., white) << qMakePair(0.5, QColor(240, 40, 40) ) << qMakePair(1., QColor(250, 0, 0) );
                break;
            case eRowR:
                stops << qMakePair(0., black) << qMakePair(0.5, QColor(240, 0, 0) ) << qMakePair(1., QColor(255, 0, 0) );
                break;
            case eRowG:
                stops << qMakePair(0., black) << qMakePair(0.5, QColor(0, 230, 0) ) << qMakePair(1., QColor(0, 255, 0) );
                break;
            case eRowB:
                stops << qMakePair(0., black) << qMakePair(0.5, QColor(0, 0, 240) ) << qMakePair(1., QColor(0, 0, 255) );
                break;
            default: // I, V, A: black to white at 1 (center), white above
                stops << qMakePair(0., black) << qMakePair(0.5, white) << qMakePair(1., white);
                break;
            }
        }
        _rows[row].slider->setGradient(stops);
    }
}

void
SolvfxColorPanel::refreshVisibility()
{
    const bool tmi = _tmiButton->isChecked();
    const bool hsv = _hsvButton->isChecked();
    const bool rgb = _rgbButton->isChecked();

    for (int i = 0; i < eRowCount; ++i) {
        bool vis;
        if (i <= eRowI) {
            vis = tmi;
        } else if (i <= eRowV) {
            vis = hsv;
        } else if (i <= eRowB) {
            vis = rgb;
        } else {
            vis = _withAlpha;
        }
        _rows[i].label->setVisible(vis);
        _rows[i].spin->setVisible(vis);
        _rows[i].slider->setVisible(vis);
    }
    adjustSize();
}

void
SolvfxColorPanel::announce()
{
    Q_EMIT colorChanged( (float)_r, (float)_g, (float)_b, (float)_a );
}

void
SolvfxColorPanel::saveSettings() const
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );

    settings.beginGroup( QString::fromUtf8(SOLVFX_COLORPANEL_SETTINGS) );
    settings.setValue( QString::fromUtf8("tmi"), _tmiButton->isChecked() );
    settings.setValue( QString::fromUtf8("hsv"), _hsvButton->isChecked() );
    settings.setValue( QString::fromUtf8("rgb"), _rgbButton->isChecked() );
    settings.setValue( QString::fromUtf8("wheel"), _wheelButton->isChecked() );
    settings.setValue( QString::fromUtf8("palette"), _paletteButton->isChecked() );
    settings.setValue( QString::fromUtf8("dyn"), _dynButton->isChecked() );
    for (std::size_t i = 0; i < _paletteSwatches.size(); ++i) {
        QStringList v;
        v << QString::number( _paletteSwatches[i]->property("r").toDouble() )
          << QString::number( _paletteSwatches[i]->property("g").toDouble() )
          << QString::number( _paletteSwatches[i]->property("b").toDouble() );
        settings.setValue(QString::fromUtf8("swatch%1").arg(i), v);
    }
    settings.endGroup();
}

void
SolvfxColorPanel::onRowSpinChanged(double v)
{
    if (_refreshing) {
        return;
    }
    int row = sender() ? sender()->property("solvfxRow").toInt() : -1;
    if ( (row < 0) || (row >= eRowCount) ) {
        return;
    }
    setChannel(row, v, true);
}

void
SolvfxColorPanel::onRowSliderDragged(double v)
{
    if (_refreshing) {
        return;
    }
    int row = sender() ? sender()->property("solvfxRow").toInt() : -1;
    if ( (row < 0) || (row >= eRowCount) ) {
        return;
    }
    setChannel(row, v, true);
}

void
SolvfxColorPanel::onWheelDragged(double h,
                                 double s)
{
    double hh, ss, v;

    rgbToHsv(_r, _g, _b, &hh, &ss, &v);
    if (v <= 0.) {
        v = 1.; // picking on the wheel from black gives a visible color
    }
    _lastHue = h;
    hsvToRgb(h, s, v, &_r, &_g, &_b);
    refreshAll(-1);
    announce();
}

void
SolvfxColorPanel::onHexEdited()
{
    double r, g, b;

    const bool hexOk = _displayMode ? hexToDisplay(_hexEdit->text(), &r, &g, &b) : hexToLinear(_hexEdit->text(), &r, &g, &b);
    if (!hexOk) {
        _hexEdit->setText( _displayMode ? displayToHex(_r, _g, _b) : linearToHex(_r, _g, _b) ); // invalid: show the current color again
        return;
    }
    if ( (std::abs(r - _r) < 1e-7) && (std::abs(g - _g) < 1e-7) && (std::abs(b - _b) < 1e-7) ) {
        return; // unchanged (editingFinished is also emitted when the field loses focus)
    }
    _r = r; _g = g; _b = b;
    _hexEdit->clearFocus();
    refreshAll(-1);
    announce();
}

void
SolvfxColorPanel::onToggleGroups()
{
    refreshVisibility();
    saveSettings();
}

void
SolvfxColorPanel::onDynToggled(bool /*on*/)
{
    refreshGradients();
    saveSettings();
}

void
SolvfxColorPanel::onWheelButtonToggled(bool on)
{
    _wheel->setVisible(on);
    adjustSize();
    saveSettings();
}

void
SolvfxColorPanel::onPaletteButtonToggled(bool on)
{
    _paletteRow->setVisible(on);
    adjustSize();
    saveSettings();
}

void
SolvfxColorPanel::onOriginalSwatchClicked()
{
    _r = _origR; _g = _origG; _b = _origB; _a = _origA;
    refreshAll(-1);
    announce();
}

void
SolvfxColorPanel::onPaletteSwatchClicked()
{
    QToolButton* sw = qobject_cast<QToolButton*>( sender() );

    if (!sw) {
        return;
    }
    _r = sw->property("r").toDouble();
    _g = sw->property("g").toDouble();
    _b = sw->property("b").toDouble();
    refreshAll(-1);
    announce();
}

void
SolvfxColorPanel::onPaletteSwatchStore()
{
    // right-click: store the current color in this swatch
    QToolButton* sw = qobject_cast<QToolButton*>( sender() );

    if (!sw) {
        return;
    }
    sw->setProperty("r", _r);
    sw->setProperty("g", _g);
    sw->setProperty("b", _b);
    setSwatchColor( sw, toDisplay(_r, _g, _b) );
    saveSettings();
}

void
SolvfxColorPanel::onRowMenuAction()
{
    QAction* a = qobject_cast<QAction*>( sender() );

    if (!a) {
        return;
    }
    int code = a->data().toInt();
    setChannel(code / 10, (code % 10) ? 1. : 0., true);
}

SolvfxColorWindow::SolvfxColorWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint)
{
    QVBoxLayout* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
}

void
SolvfxColorPanel::setDisplayMode(bool display)
{
    _displayMode = display;
    refreshAll(-1);
}

SolvfxColorDialog::SolvfxColorDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint)
{
    QVBoxLayout* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
}

void
SolvfxColorDialog::closeEvent(QCloseEvent* e)
{
    // closing the window keeps the color (Esc cancels, see QDialog::reject)
    e->ignore();
    accept();
}

void
SolvfxColorWindow::keyPressEvent(QKeyEvent* e)
{
    if ( (e->key() == Qt::Key_Escape) && (e->modifiers() == Qt::NoModifier) ) {
        e->accept();
        close();

        return;
    }
    QWidget::keyPressEvent(e);
}

bool
SolvfxColorPanel::event(QEvent* e)
{
    if (e->type() == QEvent::Show) {
        Q_EMIT updateColor();
        // remember the color the panel was opened with
        _origR = _r; _origG = _g; _origB = _b; _origA = _a;
        refreshAll(-1);
    }

    return QWidget::event(e);
}

void
SolvfxColorPanel::mousePressEvent(QMouseEvent* e)
{
    e->accept(); // https://bugreports.qt.io/browse/QTBUG-47406
}

void
SolvfxColorPanel::mouseReleaseEvent(QMouseEvent* e)
{
    e->accept();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SolvfxColorPanel.cpp"
