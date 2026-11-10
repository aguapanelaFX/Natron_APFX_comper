/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include <Python.h>
#include "SolvfxKeyerRange.h"

#include <algorithm>
#include <cmath>

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QUndoCommand>

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ViewIdx.h"
#include "Gui/GuiDefines.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/KnobGui.h"

NATRON_NAMESPACE_ENTER

static const char* kNames[4] = { "rangeA", "rangeB", "rangeC", "rangeD" };
static const char* kLetters[4] = { "A", "B", "C", "D" };

// One graph drag = one undo step. The value is already set while dragging,
// so the first redo() (called by QUndoStack::push) does nothing.
class SolvfxKeyerRangeUndo
    : public QUndoCommand
{
public:
    SolvfxKeyerRangeUndo(const KnobDoublePtr& k, double oldV, double newV)
        : QUndoCommand( QObject::tr("Keyer range") ), _k(k), _old(oldV), _new(newV) {}

    virtual void undo() OVERRIDE FINAL { set(_old); }
    virtual void redo() OVERRIDE FINAL
    {
        if (_first) {
            _first = false;
            return;
        }
        set(_new);
    }

private:
    void set(double v)
    {
        KnobDoublePtr k = _k.lock();
        if (k) {
            k->setValue(v, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
        }
    }

    KnobDoubleWPtr _k;
    double _old, _new;
    bool _first = true;
};

bool
SolvfxKeyerRange::isKeyerOperationKnob(const KnobIPtr& knob)
{
    if ( !knob || (knob->getName() != "operation") ) {
        return false;
    }
    EffectInstance* fx = dynamic_cast<EffectInstance*>( knob->getHolder() );

    return fx && fx->getNode() && (fx->getNode()->getPluginID() == "com.aguapanelafx.APFX.Keyer");
}

SolvfxKeyerRange::SolvfxKeyerRange(const KnobIPtr& operationKnob,
                                   QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight( TO_DPIY(90) );
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip( tr("Range: alpha is 0 below A, ramps up from A to B, is 1 from B to C, ramps down from C to D.\n"
                   "Drag the handles; the A B C D fields below follow.") );
    EffectInstance* fx = dynamic_cast<EffectInstance*>( operationKnob->getHolder() );
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr k = fx ? std::dynamic_pointer_cast<KnobDouble>( fx->getKnobByName(kNames[i]) ) : KnobDoublePtr();
        _k[i] = k;
        if (k) {
            KnobSignalSlotHandlerPtr h = k->getSignalSlotHandler();
            if (h) {
                QObject::connect( h.get(), SIGNAL(valueChanged(ViewSpec,int,int)), this, SLOT(update()) );
            }
        }
    }
}

QSize
SolvfxKeyerRange::sizeHint() const
{
    return QSize( TO_DPIX(300), TO_DPIY(90) );
}

double
SolvfxKeyerRange::value(int i) const
{
    KnobDoublePtr k = _k[i].lock();

    return k ? k->getValue() : (i == 0 ? 0. : 1.);
}

void
SolvfxKeyerRange::setValue(int i,
                           double v)
{
    KnobDoublePtr k = _k[i].lock();
    if (k) {
        k->setValue(v, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
    }
}

void
SolvfxKeyerRange::viewRange(double* lo,
                            double* hi) const
{
    double mn = 0., mx = 1.;
    for (int i = 0; i < 4; ++i) {
        mn = std::min( mn, value(i) );
        mx = std::max( mx, value(i) );
    }
    const double pad = (mx - mn) * 0.08;
    *lo = mn - pad;
    *hi = mx + pad;
}

double
SolvfxKeyerRange::toX(double v, double lo, double hi) const
{
    const double m = TO_DPIX(10);

    return m + (v - lo) / std::max(1e-9, hi - lo) * (width() - 2 * m);
}

double
SolvfxKeyerRange::fromX(double x, double lo, double hi) const
{
    const double m = TO_DPIX(10);

    return lo + (x - m) / std::max(1., width() - 2 * m) * (hi - lo);
}

void
SolvfxKeyerRange::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect( rect(), QColor(0x26, 0x26, 0x26) );
    double lo, hi;
    if (_drag >= 0) {
        lo = _dragLo; hi = _dragHi;
    } else {
        viewRange(&lo, &hi);
    }
    const double top = TO_DPIY(10), bottom = height() - TO_DPIY(18);
    // grid: 0 and 1 marks
    p.setPen( QPen(QColor(0x3c, 0x3c, 0x3c), 1) );
    p.drawLine( QPointF(0, top), QPointF(width(), top) );
    p.drawLine( QPointF(0, bottom), QPointF(width(), bottom) );
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    for (int g = 0; g <= 1; ++g) {
        const double x = toX(g, lo, hi);
        p.setPen( QPen(QColor(0x44, 0x44, 0x44), 1, Qt::DashLine) );
        p.drawLine( QPointF(x, top), QPointF(x, bottom) );
        p.setPen( QColor(0x80, 0x80, 0x80) );
        p.drawText( QPointF( x + 3, height() - TO_DPIY(4) ), QString::number(g) );
    }
    // trapezoid
    double v[4];
    for (int i = 0; i < 4; ++i) {
        v[i] = value(i);
    }
    QPainterPath path;
    path.moveTo(0, bottom);
    path.lineTo(toX(v[0], lo, hi), bottom);
    path.lineTo(toX(v[1], lo, hi), top);
    path.lineTo(toX(v[2], lo, hi), top);
    if (v[3] > v[2]) {
        path.lineTo(toX(v[3], lo, hi), bottom);
        path.lineTo(width(), bottom);
    } else {
        path.lineTo(width(), top);
    }
    QPainterPath fill = path;
    fill.lineTo(width(), bottom);
    fill.closeSubpath();
    p.fillPath( fill, QColor(200, 200, 200, 30) );
    p.setPen( QPen(QColor(0xd0, 0xd0, 0xd0), 1.6) );
    p.drawPath(path);
    // handles
    for (int i = 0; i < 4; ++i) {
        const double x = toX(v[i], lo, hi);
        const double y = (i == 0 || i == 3) ? bottom : top;
        const bool hot = (i == _drag);
        p.setPen( QPen(QColor(0x10, 0x10, 0x10), 1) );
        p.setBrush( hot ? QColor(0xff, 0xa5, 0x2e) : QColor(0xe0, 0xe0, 0xe0) );
        p.drawRect( QRectF(x - 4, y - 4, 8, 8) );
        p.setPen( QColor(0xb0, 0xb0, 0xb0) );
        p.drawText( QPointF( x - 3, (i == 0 || i == 3) ? bottom - 7 : top + 16 ), QString::fromUtf8(kLetters[i]) );
    }
}

void
SolvfxKeyerRange::mousePressEvent(QMouseEvent* e)
{
    double lo, hi;
    viewRange(&lo, &hi);
    const double top = TO_DPIY(10), bottom = height() - TO_DPIY(18);
    int best = -1;
    double bestD = TO_DPIX(12);
    for (int i = 0; i < 4; ++i) {
        const double x = toX(value(i), lo, hi);
        const double y = (i == 0 || i == 3) ? bottom : top;
        // handles on top (B, C) and bottom (A, D): prefer the row the click is near
        const double d = std::abs(e->pos().x() - x) + 0.5 * std::abs(e->pos().y() - y) / std::max(1., (bottom - top)) * TO_DPIX(12);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    _drag = best;
    if (best >= 0) {
        _dragStart = value(best);
    }
    _dragLo = lo;
    _dragHi = hi; // keep the scale fixed while dragging
    update();
}

void
SolvfxKeyerRange::mouseMoveEvent(QMouseEvent* e)
{
    if (_drag < 0) {
        return;
    }
    double v = fromX(e->pos().x(), _dragLo, _dragHi);
    // keep A <= B <= C <= D
    if (_drag > 0) {
        v = std::max( v, value(_drag - 1) );
    }
    if (_drag < 3) {
        v = std::min( v, value(_drag + 1) );
    }
    if ( !(e->modifiers() & Qt::ControlModifier) ) {
        v = std::round(v * 1000.) / 1000.;
    }
    setValue(_drag, v);
    update();
}

void
SolvfxKeyerRange::mouseReleaseEvent(QMouseEvent*)
{
    if (_drag >= 0) {
        KnobDoublePtr k = _k[_drag].lock();
        const double v = value(_drag);
        if ( k && (v != _dragStart) ) {
            KnobGuiPtr g = std::dynamic_pointer_cast<KnobGui>( k->getKnobGuiPointer() );
            if (g) {
                g->pushUndoCommand( new SolvfxKeyerRangeUndo(k, _dragStart, v) );
            }
        }
    }
    _drag = -1;
    update();
}

NATRON_NAMESPACE_EXIT
