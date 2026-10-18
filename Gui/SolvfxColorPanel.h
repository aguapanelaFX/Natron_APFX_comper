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

#ifndef Gui_SolvfxColorPanel_h
#define Gui_SolvfxColorPanel_h

// SOLVFX: Nuke-style color panel (TMI / HSV / RGB rows, gradient sliders,
// color wheel, swatches and an sRGB hex field converted through OCIO).

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <utility>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QWidget>
#include <QGradientStops>
#include <QImage>
#include <QString>
#include <QColor>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

class QToolButton;
class QLineEdit;
class QGridLayout;
class QMouseEvent;
class QKeyEvent;
class QPaintEvent;

NATRON_NAMESPACE_ENTER

/// A horizontal slider drawn as a color gradient with Nuke-style ticks.
class SolvfxGradientSlider
    : public QWidget
{
    Q_OBJECT

public:

    enum ScaleEnum
    {
        eScaleSigned4 = 0,  // -4..4, 0 centered, square-root spacing (Nuke T/M)
        eScaleUnsigned4,    // 0..4, 1 centered, square-root spacing (Nuke I/S/V/R/G/B/A)
        eScaleLinear01      // 0..1 linear, 0.5 centered (Nuke H)
    };

    SolvfxGradientSlider(ScaleEnum scale, QWidget* parent = 0);

    void setValue(double v);
    double value() const { return _value; }

    // stops in slider position space (0 = left, 1 = right)
    void setGradient(const QGradientStops& stops);

    virtual QSize sizeHint() const OVERRIDE FINAL;
    virtual QSize minimumSizeHint() const OVERRIDE FINAL;

Q_SIGNALS:

    void valueDragged(double v);
    void dragFinished();

private:

    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;

public:
    static double posFromValue(ScaleEnum scale, double v);
    static double valueFromPos(ScaleEnum scale, double p);

private:
    double posFromValue(double v) const { return posFromValue(_scale, v); }
    double valueFromPos(double p) const { return valueFromPos(_scale, p); }
    void setFromMouse(int x);

    ScaleEnum _scale;
    double _value;
    QGradientStops _stops;
    bool _dragging;
};

/// Hue/saturation wheel: hue around, saturation from the center.
class SolvfxColorWheel
    : public QWidget
{
    Q_OBJECT

public:

    SolvfxColorWheel(QWidget* parent = 0);

    void setHueSat(double h, double s);

    virtual QSize sizeHint() const OVERRIDE FINAL;
    virtual bool hasHeightForWidth() const OVERRIDE FINAL { return true; }
    virtual int heightForWidth(int w) const OVERRIDE FINAL { return w; }

Q_SIGNALS:

    void hueSatDragged(double h, double s);

private:

    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void resizeEvent(QResizeEvent* e) OVERRIDE FINAL;

    void rebuildImage();
    void setFromMouse(const QPoint& p);
    QRectF wheelRect() const;

    double _h, _s;
    QImage _image;
};

/// Floating window holding a color panel (title "Node.knob", Esc closes)
class SolvfxColorWindow
    : public QWidget
{
public:
    explicit SolvfxColorWindow(QWidget* parent = 0);

private:
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
};

/// The full panel. Same signals/slots as Natron's ColorSelectorWidget so it
/// can be used as a drop-in replacement in KnobGuiColor.
class SolvfxColorPanel
    : public QWidget
{
    Q_OBJECT

public:

    explicit SolvfxColorPanel(bool withAlpha, QWidget* parent = 0);

    void getColor(float* r, float* g, float* b, float* a);

Q_SIGNALS:

    void colorChanged(float r, float g, float b, float a);
    void updateColor();

public Q_SLOTS:

    void setColor(float r, float g, float b, float a);

private Q_SLOTS:

    void onRowSpinChanged(double v);
    void onRowSliderDragged(double v);
    void onWheelDragged(double h, double s);
    void onHexEdited();
    void onToggleGroups();
    void onDynToggled(bool on);
    void onWheelButtonToggled(bool on);
    void onPaletteButtonToggled(bool on);
    void onOriginalSwatchClicked();
    void onPaletteSwatchClicked();
    void onPaletteSwatchStore();
    void onRowMenuAction();

private:

    enum RowEnum
    {
        eRowT = 0, eRowM, eRowI, eRowH, eRowS, eRowV, eRowR, eRowG, eRowB, eRowA, eRowCount
    };

    struct Row
    {
        QToolButton* label;
        SpinBox* spin;
        SolvfxGradientSlider* slider;
    };

    // set the color from the given row/channel value and refresh everything
    void setChannel(int row, double v, bool announce);
    double channelValue(int row) const;
    void refreshAll(int skipRow);
    void refreshGradients();
    void refreshVisibility();
    void announce();
    void saveSettings() const;

    static QColor toDisplay(double r, double g, double b);
    static void rgbToHsv(double r, double g, double b, double* h, double* s, double* v);
    static void hsvToRgb(double h, double s, double v, double* r, double* g, double* b);

    bool _withAlpha;
    double _r, _g, _b, _a;
    double _origR, _origG, _origB, _origA;
    double _lastHue; // keep the hue when saturation goes to 0

    Row _rows[eRowCount];
    QToolButton* _currentSwatch;
    QToolButton* _originalSwatch;
    QLineEdit* _hexEdit;
    QToolButton* _tmiButton;
    QToolButton* _hsvButton;
    QToolButton* _rgbButton;
    QToolButton* _wheelButton;
    QToolButton* _paletteButton;
    QToolButton* _dynButton;
    SolvfxColorWheel* _wheel;
    QWidget* _paletteRow;
    std::vector<QToolButton*> _paletteSwatches;
    bool _refreshing;

    // workaround for QToolButton+QWidgetAction (see ColorSelectorWidget)
    virtual bool event(QEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SolvfxColorPanel_h
