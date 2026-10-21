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

#include "ScaleSliderQWidget.h"

#include <vector>
#include <cmath> // for std::pow()
#include <algorithm>
#include <cassert>
#include <stdexcept>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QPaintEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
#include <QMouseEvent>
#include <QPainter>
#include <QApplication>
#include <QStyleOption>

#include "Engine/Settings.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"

#include "Gui/GuiMacros.h"
#include "Gui/ticks.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/ZoomContext.h"
#include "Gui/Gui.h"

#define TICK_HEIGHT 7
#define SLIDER_WIDTH 4
#define SLIDER_HEIGHT 15

NATRON_NAMESPACE_ENTER

struct ScaleSliderQWidgetPrivate
{
    Gui* gui;
    ZoomContext zoomCtx;
    QPointF oldClick;
    double minimum, maximum;
    ScaleTypeEnum type;
    double value;
    bool dragging;
    QFont font;
    QColor sliderColor;
    QColor customSliderColor;
    bool initialized;
    bool mustInitializeSliderPosition;
    bool readOnly;
    bool ctrlDown;
    bool shiftDown;
    double currentZoom;
    ScaleSliderQWidget::DataTypeEnum dataType;
    bool altered;
    bool useLineColor;
    bool useSliderColor;
    QColor lineColor;
    bool allowDraftModeSetting;
    std::vector<std::pair<double, QString> > customTicks; // SOLVFX
    bool usePivot; // SOLVFX: default value drawn in the middle of the slider
    double pivot;

    // value <-> slider axis (0..1 with the pivot at 0.5 when usePivot)
    double toAxis(double v) const
    {
        if (!usePivot) {
            return v;
        }
        if (v <= pivot) {
            return (pivot > minimum) ? 0.5 * (v - minimum) / (pivot - minimum) : 0.5;
        }
        return (maximum > pivot) ? 0.5 + 0.5 * (v - pivot) / (maximum - pivot) : 0.5;
    }

    double fromAxis(double s) const
    {
        if (!usePivot) {
            return s;
        }
        if (s <= 0.5) {
            return minimum + (s / 0.5) * (pivot - minimum);
        }
        return pivot + ( (s - 0.5) / 0.5 ) * (maximum - pivot);
    }

    ScaleSliderQWidgetPrivate(QWidget* parent,
                              double min,
                              double max,
                              double initialPos,
                              bool allowDraftModeSetting,
                              Gui* gui,
                              ScaleSliderQWidget::DataTypeEnum dataType,
                              ScaleTypeEnum type)
        : gui(gui)
        , zoomCtx( (max-min) * 1e-8, (max-min) * 1e8 )
        , oldClick()
        , minimum(min)
        , maximum(max)
        , type(type)
        , value(initialPos)
        , dragging(false)
        , font( parent->font() )
        , sliderColor(85, 116, 114)
        , initialized(false)
        , mustInitializeSliderPosition(true)
        , readOnly(false)
        , ctrlDown(false)
        , shiftDown(false)
        , currentZoom(1.)
        , dataType(dataType)
        , altered(false)
        , useLineColor(false)
        , useSliderColor(false)
        , lineColor(Qt::black)
        , allowDraftModeSetting(allowDraftModeSetting)
        , usePivot(false)
        , pivot(0.)
    {
        // SOLVFX: smaller tick numbers (Nuke-like, the field shows the real value)
        font.setPointSizeF( std::max( 6., font.pointSizeF() * NATRON_FONT_SIZE_10 / NATRON_FONT_SIZE_13 * 0.85 ) );
        assert( std::isfinite(minimum) && std::isfinite(maximum) && std::isfinite(initialPos) );
    }
};

ScaleSliderQWidget::ScaleSliderQWidget(double min,
                                       double max,
                                       double initialPos,
                                       bool allowDraftModeSetting,
                                       DataTypeEnum dataType,
                                       Gui* gui,
                                       ScaleTypeEnum type,
                                       QWidget* parent)
    : QWidget(parent)
    , _imp( new ScaleSliderQWidgetPrivate(parent, min, max, initialPos, allowDraftModeSetting, gui, dataType, type) )
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    QSize sizeh = sizeHint();
    if ( (sizeh.width() > 0) && (sizeh.height() > 0) ) {
        _imp->zoomCtx.setScreenSize( sizeh.width(), sizeh.height() );
    }
    setFocusPolicy(Qt::ClickFocus);
}

QSize
ScaleSliderQWidget::sizeHint() const
{
    return QWidget::sizeHint();
}

QSize
ScaleSliderQWidget::minimumSizeHint() const
{
    return QSize( TO_DPIX(150), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
}

ScaleSliderQWidget::~ScaleSliderQWidget()
{
}

ScaleTypeEnum
ScaleSliderQWidget::type() const
{
    return _imp->type;
}

double
ScaleSliderQWidget::minimum() const
{
    return _imp->minimum;
}

double
ScaleSliderQWidget::maximum() const
{
    return _imp->maximum;
}

double
ScaleSliderQWidget::getPosition() const
{
    return _imp->value;
}

bool
ScaleSliderQWidget::isReadOnly() const
{
    return _imp->readOnly;
}

void
ScaleSliderQWidget::mousePressEvent(QMouseEvent* e)
{
    if (!_imp->readOnly) {
        // Ctrl+click cannot be used to reset to default, because Ctrl is already used for fine-tuning
        // (fine-tuning is not possible in Nuke)!
        // same goes for Shift+click.
        // we use Alt instead (Nuke uses Ctrl).
        // @see ScaleSliderQWidget::keyPressEvent()
        //
        if ( modifierHasAlt(e) ) {
            Q_EMIT resetToDefaultRequested();
        } else {
            QPoint newClick =  e->pos();
            _imp->oldClick = newClick;
            QPointF newClick_opengl = _imp->zoomCtx.toZoomCoordinates( newClick.x(), newClick.y() );
            const double av = _imp->fromAxis( newClick_opengl.x() );
            double v = _imp->dataType == eDataTypeInt ? std::floor(av + 0.5) : av;
            seekInternal(v);
        }
    }
    QWidget::mousePressEvent(e);
}

void
ScaleSliderQWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!_imp->readOnly) {
        QPoint newClick =  e->pos();
        QPointF newClick_opengl = _imp->zoomCtx.toZoomCoordinates( newClick.x(), newClick.y() );
        const double av = _imp->fromAxis( newClick_opengl.x() );
        double v = _imp->dataType == eDataTypeInt ? std::floor(av + 0.5) : av;
        if (_imp->gui && _imp->allowDraftModeSetting) {
            _imp->gui->setDraftRenderEnabled(true);
        }
        seekInternal(v);
    }
}

void
ScaleSliderQWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (!_imp->readOnly) {
        bool hasMoved = true;
        if (_imp->gui && _imp->allowDraftModeSetting) {
            hasMoved = _imp->gui->isDraftRenderEnabled();
            _imp->gui->setDraftRenderEnabled(false);
        }
        Q_EMIT editingFinished(hasMoved);
    }
    QWidget::mouseReleaseEvent(e);
}

void
ScaleSliderQWidget::focusInEvent(QFocusEvent* e)
{
    Qt::KeyboardModifiers mod = qApp->keyboardModifiers();

    if ( mod.testFlag(Qt::ControlModifier) ) {
        _imp->ctrlDown = true;
    }
    if ( mod.testFlag(Qt::ShiftModifier) ) {
        _imp->shiftDown = true;
    }
    zoomRange();
    QWidget::focusInEvent(e);
}

void
ScaleSliderQWidget::focusOutEvent(QFocusEvent* e)
{
    _imp->ctrlDown = false;
    _imp->shiftDown = false;
    zoomRange();
    QWidget::focusOutEvent(e);
}

void
ScaleSliderQWidget::zoomRange()
{
    if (_imp->ctrlDown) {
        double scale = _imp->shiftDown ? 100. : 10.;
        assert(_imp->currentZoom != 0);
        double effectiveZoom = scale / _imp->currentZoom;
        _imp->currentZoom = scale;
        _imp->zoomCtx.zoomx(_imp->toAxis(_imp->value), 0, effectiveZoom);
    } else {
        _imp->zoomCtx.zoomx(_imp->toAxis(_imp->value), 0, 1. / _imp->currentZoom);
        _imp->currentZoom = 1.;
        if (_imp->minimum < _imp->maximum) {
            centerOnAxisRange();
        }
    }
    update();
}

void
ScaleSliderQWidget::enterEvent(QtCompat::QEnterEvent* e)
{
    if ( Gui::isFocusStealingPossible() ) {
        setFocus();
    }
    QWidget::enterEvent(e);
}

void
ScaleSliderQWidget::leaveEvent(QEvent* e)
{
    if ( hasFocus() ) {
        clearFocus();
    }
    QWidget::leaveEvent(e);
}

void
ScaleSliderQWidget::keyPressEvent(QKeyEvent* e)
{
    bool accepted = true;

    if (e->key() == Qt::Key_Control) {
        _imp->ctrlDown = true;
        zoomRange();
        accepted = false;
    } else if (e->key() == Qt::Key_Shift) {
        _imp->shiftDown = true;
        zoomRange();
        accepted = false;
    } else {
        accepted = false;
    }
    if (!accepted) {
        QWidget::keyPressEvent(e);
    }
}

double
ScaleSliderQWidget::increment()
{
    return ( _imp->zoomCtx.right() - _imp->zoomCtx.left() ) / width();
}

void
ScaleSliderQWidget::setAltered(bool b)
{
    _imp->altered = b;
    update();
}

bool
ScaleSliderQWidget::getAltered() const
{
    return _imp->altered;
}

void
ScaleSliderQWidget::keyReleaseEvent(QKeyEvent* e)
{
    bool accepted = true;

    if (e->key() == Qt::Key_Control) {
        _imp->ctrlDown = false;
        zoomRange();
        accepted = false;
    } else if (e->key() == Qt::Key_Shift) {
        _imp->shiftDown = false;
        zoomRange();
        accepted = false;
    } else {
        accepted = false;
    }
    if (!accepted) {
        QWidget::keyReleaseEvent(e);
    }
}

void
ScaleSliderQWidget::seekScalePosition(double v)
{
    assert( std::isfinite(v) );
    if (v < _imp->minimum) {
        v = _imp->minimum;
    }
    if (v > _imp->maximum) {
        v = _imp->maximum;
    }


    if ( (v == _imp->value) && _imp->initialized ) {
        return;
    }
    _imp->value = v;
    if ( _imp->initialized && isVisible() ) {
        update();
    }
}

void
ScaleSliderQWidget::seekInternal(double v)
{
    assert( std::isfinite(v) );
    if (v < _imp->minimum) {
        v = _imp->minimum;
    }
    if (v > _imp->maximum) {
        v = _imp->maximum;
    }
    if (v == _imp->value) {
        return;
    }
    _imp->value = v;
    if (_imp->initialized) {
        update();
    }
    Q_EMIT positionChanged(v);
}

void
ScaleSliderQWidget::setMinimumAndMaximum(double min,
                                         double max)
{
    assert(std::isfinite(min) && std::isfinite(max) && min < max);
    _imp->minimum = min;
    _imp->maximum = max;
    if ( _imp->usePivot && ( (_imp->pivot <= min) || (_imp->pivot >= max) ) ) {
        _imp->usePivot = false; // SOLVFX: pivot no longer inside the range
    }
    if (_imp->minimum < _imp->maximum) {
        centerOnAxisRange();
    }
}

void
ScaleSliderQWidget::centerOnAxisRange()
{
    if (_imp->usePivot) {
        centerOn(0., 1.);
    } else if (_imp->minimum < _imp->maximum) {
        centerOn(_imp->minimum, _imp->maximum);
    }
}

void
ScaleSliderQWidget::setDefaultPivot(double pivot)
{
    // SOLVFX: draw the default value in the middle of the slider (piecewise linear)
    const bool use = (pivot > _imp->minimum) && (pivot < _imp->maximum);
    _imp->usePivot = use;
    _imp->pivot = pivot;
    if (!_imp->mustInitializeSliderPosition) {
        centerOnAxisRange();
    }
    update();
}

void
ScaleSliderQWidget::centerOn(double left,
                             double right)
{
    assert(std::isfinite(left) && std::isfinite(right) && left < right);
    if ( (_imp->zoomCtx.screenHeight() == 0) || (_imp->zoomCtx.screenWidth() == 0) || (left >= right) ) {
        return;
    }
    double w = right - left;
    _imp->zoomCtx.fill( left - w * 0.05, right + w * 0.05, _imp->zoomCtx.bottom(), _imp->zoomCtx.top() );

    update();
}

void
ScaleSliderQWidget::resizeEvent(QResizeEvent* e)
{
    if ( (e->size().width() > 0) && (e->size().height() > 0) ) {
        _imp->zoomCtx.setScreenSize( e->size().width(), e->size().height() );
    }
    if ( !_imp->mustInitializeSliderPosition && (_imp->minimum < _imp->maximum) ) {
        centerOnAxisRange();
    }
    QWidget::resizeEvent(e);
}

void
ScaleSliderQWidget::paintEvent(QPaintEvent* /*e*/)
{
    if (_imp->mustInitializeSliderPosition) {
        if (_imp->minimum < _imp->maximum) {
            centerOnAxisRange();
        }
        _imp->mustInitializeSliderPosition = false;
        seekScalePosition(_imp->value);
        _imp->initialized = true;
    }

    ///fill the background with the appropriate style color
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    p.setOpacity(1);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    double txtR, txtG, txtB;
    if (_imp->altered) {
        appPTR->getCurrentSettings()->getAltTextColor(&txtR, &txtG, &txtB);
    } else {
        appPTR->getCurrentSettings()->getTextColor(&txtR, &txtG, &txtB);
    }

    QColor textColor;
    textColor.setRgbF( Image::clamp<qreal>(txtR, 0., 1.),
                       Image::clamp<qreal>(txtG, 0., 1.),
                       Image::clamp<qreal>(txtB, 0., 1.) );

    QColor scaleColor;
    scaleColor.setRgbF(textColor.redF() / 2., textColor.greenF() / 2., textColor.blueF() / 2.);

    QFontMetrics fontM(_imp->font, 0);
    // SOLVFX: tick numbers in a dark grey, barely above the panel (Nuke-like)
    const QColor solvfxNumberColor = _imp->altered ? textColor.darker(170) : QColor(0x6b, 0x6b, 0x6b);

    if (!_imp->useLineColor) {
        p.setPen( QColor(0x7a, 0x7a, 0x7a) ); // SOLVFX: thin gray track
    } else {
        p.setPen(_imp->lineColor);
    }

    QPointF btmLeft = _imp->zoomCtx.toZoomCoordinates(0, height() - 1);
    QPointF topRight = _imp->zoomCtx.toZoomCoordinates(width() - 1, 0);

    if ( btmLeft.x() == topRight.x() ) {
        return;
    }

    /*drawing X axis*/
    double lineYpos = height() - 1 - fontM.height()  - TO_DPIY(TICK_HEIGHT) / 2;
    p.drawLine(0, lineYpos, width() - 1, lineYpos);

    double tickBottom = _imp->zoomCtx.toZoomCoordinates( 0, height() - 1 - fontM.height() ).y();
    double tickTop = _imp->zoomCtx.toZoomCoordinates( 0, height() - 1 - fontM.height()  - TO_DPIY(TICK_HEIGHT) ).y();
    const double smallestTickSizePixel = std::min(60., width() / 4.); // tick size (in pixels) for alpha = 0.
    const double largestTickSizePixel = 120.; // tick size (in pixels) for alpha = 1.
    const double rangePixel =  width();
    const double range_min = btmLeft.x();
    const double range_max =  topRight.x();
    const double range = range_max - range_min;
    double smallTickSize;
    bool half_tick;
    ticks_size(range_min, range_max, rangePixel, smallestTickSizePixel, &smallTickSize, &half_tick);
    if ( (_imp->dataType == eDataTypeInt) && (smallTickSize < 1.) ) {
        smallTickSize = 1.;
        half_tick = false;
    }
    int m1, m2;
    const int ticks_max = 1000;
    double offset;
    ticks_bounds(range_min, range_max, smallTickSize, half_tick, ticks_max, &offset, &m1, &m2);
    std::vector<int> ticks;
    ticks_fill(half_tick, ticks_max, m1, m2, &ticks);
    const double smallestTickSize = range * smallestTickSizePixel / rangePixel;
    const double largestTickSize = range * largestTickSizePixel / rangePixel;
    const double minTickSizeTextPixel = fontM.horizontalAdvance( QLatin1String("00") ); // AXIS-SPECIFIC
    const double minTickSizeText = range * minTickSizeTextPixel / rangePixel;
    std::vector<std::pair<double, QString> > pivotTicks;
    if ( _imp->customTicks.empty() && _imp->usePivot ) {
        // SOLVFX: ticks at min, halfway, default, halfway, max
        const double vals[5] = { _imp->minimum, (_imp->minimum + _imp->pivot) / 2., _imp->pivot,
                                 (_imp->pivot + _imp->maximum) / 2., _imp->maximum };
        for (int i = 0; i < 5; ++i) {
            pivotTicks.push_back( std::make_pair( vals[i], QString::number(vals[i], 'g', 3) ) );
        }
    }
    const std::vector<std::pair<double, QString> >& fixedTicks = _imp->customTicks.empty() ? pivotTicks : _imp->customTicks;
    if ( !fixedTicks.empty() ) {
        // SOLVFX: fixed ticks with custom labels
        QColor c = _imp->readOnly || !isEnabled() ? Qt::black : textColor;
        QPen pen(c);
        const QColor numberColor = _imp->readOnly || !isEnabled() ? QColor(0x3a, 0x3a, 0x3a) : solvfxNumberColor;
        pen.setWidthF(1.);
        p.setFont(_imp->font);
        for (std::size_t i = 0; i < fixedTicks.size(); ++i) {
            const double value = _imp->toAxis(fixedTicks[i].first);
            if ( (value < range_min) || (value > range_max) ) {
                continue;
            }
            p.setPen(pen);
            QPointF tickBottomPos = _imp->zoomCtx.toWidgetCoordinates(value, tickBottom);
            QPointF tickTopPos = _imp->zoomCtx.toWidgetCoordinates(value, tickTop);
            p.drawLine(tickBottomPos, tickTopPos);
            const QString& s = fixedTicks[i].second;
            if ( s.isEmpty() ) {
                continue;
            }
            QPointF textPos = _imp->zoomCtx.toWidgetCoordinates( value, btmLeft.y() );
            int textWidth = fontM.horizontalAdvance(s);
            int textX = std::floor(textPos.x() + 0.5);
            int textY = std::floor(textPos.y() + 0.5);
            textX = std::min(std::max(0, textX - textWidth / 2), width() - textWidth);
            p.setPen(numberColor);
            p.drawText(textX, textY, s);
        }
    }
    for (int i = m1; fixedTicks.empty() && i <= m2; ++i) {
        double value = i * smallTickSize + offset;
        const double tickSize = ticks[i - m1] * smallTickSize;
        const double alpha = ticks_alpha(smallestTickSize, largestTickSize, tickSize);
        QColor color(textColor);
        color.setAlphaF(alpha);
        QPen pen(color);
        pen.setWidthF(1.9);
        p.setPen(pen);

        // for Int slider, because smallTickSize is at least 1, isFloating can never be true
        bool isFloating = std::abs(std::floor(0.5 + value) - value) != 0.;
        assert( !(_imp->dataType == eDataTypeInt && isFloating) );
        bool renderFloating = _imp->dataType == eDataTypeDouble || !isFloating;

        if (renderFloating) {
            QPointF tickBottomPos = _imp->zoomCtx.toWidgetCoordinates(value, tickBottom);
            QPointF tickTopPos = _imp->zoomCtx.toWidgetCoordinates(value, tickTop);

            p.drawLine(tickBottomPos, tickTopPos);
        }

        if ( renderFloating && (tickSize > minTickSizeText) ) {
            const int tickSizePixel = rangePixel * tickSize / range;
            const QString s = QString::number(value);
            const int sSizePixel =  fontM.horizontalAdvance(s);
            if (tickSizePixel > sSizePixel) {
                const int sSizeFullPixel = sSizePixel + minTickSizeTextPixel;
                double alphaText = 1.0; //alpha;
                if (tickSizePixel < sSizeFullPixel) {
                    // when the text size is between sSizePixel and sSizeFullPixel,
                    // draw it with a lower alpha
                    alphaText *= (tickSizePixel - sSizePixel) / (double)minTickSizeTextPixel;
                }
                //alphaText = std::min(alphaText, alpha); // don't draw more opaque than ticks
                QColor c = _imp->readOnly || !isEnabled() ? QColor(0x3a, 0x3a, 0x3a) : solvfxNumberColor;
                c.setAlphaF(alphaText);
                p.setFont(_imp->font);
                p.setPen(c);

                QPointF textPos = _imp->zoomCtx.toWidgetCoordinates( value, btmLeft.y() );

                int textWidth = fontM.horizontalAdvance(s);
                int textX = std::floor(textPos.x() + 0.5);
                int textY = std::floor(textPos.y() + 0.5);
                // center the text, and make sure it fits
                textX = std::min(std::max(0, textX - textWidth / 2), width() - textWidth);
                //p.drawText(textPos, s);
                p.drawText(textX, textY, s);
                //p.drawText(textPos.x()-1000, textPos.y(), 2*1000, 1000, Qt::AlignHCenter|Qt::AlignTop, s);
                //p.drawText(textPos.x(), textPos.y(), 2*20, 20, Qt::TextDontClip|Qt::AlignLeft|Qt::AlignTop, s);
            }
        }
    }
    double positionValue = _imp->zoomCtx.toWidgetCoordinates(_imp->toAxis(_imp->value), 0).x();

    SettingsPtr settings = appPTR->getCurrentSettings();
    if (!_imp->useSliderColor) {
        _imp->sliderColor = QColor(0xbd, 0xbd, 0xbd); // SOLVFX handle
    } else {
        _imp->sliderColor = _imp->customSliderColor;
    }

    double selectionColorRGB[3];
    settings->getSelectionColor(&selectionColorRGB[0], &selectionColorRGB[1], &selectionColorRGB[2]);
    QColor selectionColor;
    selectionColor.setRgbF(selectionColorRGB[0], selectionColorRGB[1], selectionColorRGB[2]);

#ifdef SLIDER_RECTANGLE
    QPointF sliderBottomLeft(positionValue - TO_DPIX(SLIDER_WIDTH) / 2, height() - 1 - fontM.height() / 2);
    QPointF sliderTopRight( positionValue + TO_DPIX(SLIDER_WIDTH) / 2, height() - 1 - fontM.height() / 2 - TO_DPIY(SLIDER_HEIGHT) );

    /*draw the slider*/
    p.setPen(_imp->sliderColor);
    p.fillRect(sliderBottomLeft.x(), sliderBottomLeft.y(), sliderTopRight.x() - sliderBottomLeft.x(), sliderTopRight.y() - sliderBottomLeft.y(), _imp->sliderColor);

    /*draw a black rect around the slider for contrast or orange when focused*/
    if ( !hasFocus() ) {
        p.setPen(Qt::black);
    } else {
        QPen pen = p.pen();
        pen.setColor(selectionColor)
        QVector<qreal> dashStyle;
        qreal space = 2;
        dashStyle << 1 << space;
        pen.setDashPattern(dashStyle);
        p.setOpacity(0.8);
        p.setPen(pen);
    }

    p.drawLine( sliderBottomLeft.x(), sliderBottomLeft.y(), sliderBottomLeft.x(), sliderTopRight.y() );
    p.drawLine( sliderBottomLeft.x(), sliderTopRight.y(), sliderTopRight.x(), sliderTopRight.y() );
    p.drawLine( sliderTopRight.x(), sliderTopRight.y(), sliderTopRight.x(), sliderBottomLeft.y() );
    p.drawLine( sliderTopRight.x(), sliderBottomLeft.y(), sliderBottomLeft.x(), sliderBottomLeft.y() );
#else
    QPainter::RenderHints rh = p.renderHints();
    p.setRenderHints(QPainter::Antialiasing);
   /*draw the slider*/
    if (!_imp->readOnly) {
        p.setBrush(QBrush(_imp->sliderColor));
    } else {
        QColor disabledColor(100,100,100, 200);
        p.setBrush(QBrush(disabledColor));
    }
    /*draw a black rect around the slider for contrast or orange when focused*/
    if ( !hasFocus() || _imp->readOnly ) {
        p.setPen(Qt::black);
    } else {
        QPen pen = p.pen();
        pen.setColor(selectionColor); // alpha could be set too, if required
        //QVector<qreal> dashStyle;
        //qreal space = 2;
        //dashStyle << 1 << space;
        //pen.setDashPattern(dashStyle);
        //p.setOpacity(0.8); // also sets opacity of the brush/fill!
        p.setPen(pen);
    }
    // SOLVFX: small 6x12 handle with 1px corners, centered on the track
    {
        const double hw = TO_DPIX(6) / 2.;
        const double hh = TO_DPIY(12) / 2.;
        if ( !hasFocus() || _imp->readOnly ) {
            p.setPen( QColor(0x14, 0x14, 0x14) );
        }
        p.drawRoundedRect( QRectF(positionValue - hw, lineYpos - hh, hw * 2., hh * 2.), 1., 1. );
    }
    p.setRenderHints(rh);
#endif
} // paintEvent

void
ScaleSliderQWidget::setReadOnly(bool ro)
{
    _imp->readOnly = ro;
    update();
}

void
ScaleSliderQWidget::setUseLineColor(bool use,
                                    const QColor& color)
{
    _imp->useLineColor = use;
    _imp->lineColor = color;
    update();
}

void
ScaleSliderQWidget::setUseSliderColor(bool use,
                                      const QColor &color)
{
    _imp->useSliderColor = use;
    _imp->customSliderColor = color;
    update();
}

void
ScaleSliderQWidget::setCustomTicks(const std::vector<std::pair<double, QString> >& ticks)
{
    _imp->customTicks = ticks;
    update();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ScaleSliderQWidget.cpp"
