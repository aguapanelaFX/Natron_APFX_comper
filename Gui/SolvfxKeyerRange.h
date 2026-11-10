/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#ifndef Gui_SolvfxKeyerRange_h
#define Gui_SolvfxKeyerRange_h

// SOLVFX: Nuke-like range graph of the APFX Keyer: the A B C D trapezoid with 4 draggable handles,
// kept in sync with the rangeA..rangeD parameters.

#include <Python.h>

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class SolvfxKeyerRange
    : public QWidget
{
public:
    // knob = the Keyer's "operation" choice (the graph lives under it)
    static bool isKeyerOperationKnob(const KnobIPtr& knob);

    SolvfxKeyerRange(const KnobIPtr& operationKnob, QWidget* parent);

    virtual QSize sizeHint() const OVERRIDE FINAL;

private:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;

    double value(int i) const;
    void setValue(int i, double v);
    void viewRange(double* lo, double* hi) const;
    double toX(double v, double lo, double hi) const;
    double fromX(double x, double lo, double hi) const;

    KnobDoubleWPtr _k[4];
    int _drag = -1;
    double _dragLo = 0., _dragHi = 1.;
    double _dragStart = 0.; // value of the dragged handle on press (undo)
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SolvfxKeyerRange_h
