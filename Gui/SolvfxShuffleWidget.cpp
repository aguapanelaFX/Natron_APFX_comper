/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (a Natron fork) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include <Python.h>

#include "SolvfxShuffleWidget.h"
#include "Gui/KnobGui.h"
#include <QPointer>
#include <QUndoCommand>

#include <algorithm>
#include <cmath>

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/ViewIdx.h"
#include "Gui/ComboBox.h"

NATRON_NAMESPACE_ENTER

// SOLVFX: one Shuffle wire change (an output channel menu), undoable
class SolvfxShuffleUndo
    : public QUndoCommand
{
public:
    SolvfxShuffleUndo(const KnobChoicePtr& k, int oldEntry, int newEntry, QWidget* w)
        : QUndoCommand( QObject::tr("Shuffle") )
        , _k(k), _old(oldEntry), _new(newEntry), _w(w)
    {
    }

    virtual void undo() OVERRIDE FINAL { apply(_old); }
    virtual void redo() OVERRIDE FINAL { apply(_new); }

private:
    void apply(int v)
    {
        KnobChoicePtr k = _k.lock();
        if (k) {
            k->setValue(v, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
        }
        if (_w) {
            _w->update();
        }
    }

    KnobChoiceWPtr _k;
    int _old, _new;
    QPointer<QWidget> _w;
};

// layout (pixels before DPI scaling)
static const int kMargin = 4;
static const int kLinesW = 64;     // space for the lines between input and output blocks
static const int kMinBlockW = 130;
static const int kMaxBlockW = 210;
static const int kHeaderH = 24;
static const int kRowH = 18;
static const int kBlockPad = 5;
static const int kGap = 6;

#define SHUFFLE_PLUGIN_ID "net.sf.openfx.ShufflePlugin"

static inline double
D(double v)
{
    return TO_DPIX(v);
}

bool
SolvfxShuffleWidget::isShuffleLayerKnob(const KnobIPtr& knob)
{
    if ( !knob || (knob->getName() != "outputLayer") || !std::dynamic_pointer_cast<KnobChoice>(knob) ) {
        return false;
    }
    EffectInstance* fx = dynamic_cast<EffectInstance*>( knob->getHolder() );
    if ( !fx || (fx->getPluginID() != SHUFFLE_PLUGIN_ID) ) {
        return false;
    }
    // the R / G / B / A menus must be there too
    static const char* names[4] = { "outputR", "outputG", "outputB", "outputA" };
    for (int i = 0; i < 4; ++i) {
        if ( !std::dynamic_pointer_cast<KnobChoice>( fx->getKnobByName(names[i]) ) ) {
            return false;
        }
    }

    return true;
}

bool
SolvfxShuffleWidget::isHiddenShuffleKnob(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    const std::string& n = knob->getName();
    if ( (n != "outputR") && (n != "outputG") && (n != "outputB") && (n != "outputA") &&
         (n != "outputComponents") && (n != "setGBAFromR") ) {
        return false;
    }
    EffectInstance* fx = dynamic_cast<EffectInstance*>( knob->getHolder() );
    if ( !fx || (fx->getPluginID() != SHUFFLE_PLUGIN_ID) ) {
        return false;
    }

    return isShuffleLayerKnob( fx->getKnobByName("outputLayer") );
}

SolvfxShuffleWidget::SolvfxShuffleWidget(const KnobChoicePtr& outLayer,
                                         QWidget* outCombo,
                                         QWidget* parent)
    : QWidget(parent)
    , _layerKnob(outLayer)
    , _outCombo(outCombo)
    , _const0(-1)
    , _const1(-1)
    , _dragEntry(-1)
    , _dragging(false)
    , _selectedKnob(-1)
{
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth( (int)D(2 * kMinBlockW + kLinesW + 2 * kMargin) );
    setToolTip( tr("Drag from an input channel to an output channel.\n"
                   "0 / 1 on an output row: black / white.\n"
                   "Click a line and press Delete to remove it (the channel becomes black).") );

    EffectInstance* fx = dynamic_cast<EffectInstance*>( outLayer->getHolder() );
    static const char* names[4] = { "outputR", "outputG", "outputB", "outputA" };
    for (int i = 0; i < 4; ++i) {
        KnobChoicePtr k = fx ? std::dynamic_pointer_cast<KnobChoice>( fx->getKnobByName(names[i]) ) : KnobChoicePtr();
        _outKnobs[i] = k;
        if (k) {
            KnobSignalSlotHandlerPtr h = k->getSignalSlotHandler();
            if (h) {
                QObject::connect( h.get(), SIGNAL(valueChanged(ViewSpec,int,int)), this, SLOT(update()) );
            }
            if (i == 0) {
                QObject::connect( k.get(), &KnobChoice::populated, this, [this]() { rebuild(); } );
            }
        }
    }
    if (fx) {
        KnobIPtr comps = fx->getKnobByName("outputComponents");
        KnobSignalSlotHandlerPtr h = comps ? comps->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (h) {
            QObject::connect( h.get(), SIGNAL(valueChanged(ViewSpec,int,int)), this, SLOT(update()) );
        }
    }
    {
        KnobSignalSlotHandlerPtr h = outLayer->getSignalSlotHandler();
        if (h) {
            QObject::connect( h.get(), SIGNAL(valueChanged(ViewSpec,int,int)), this, SLOT(update()) );
        }
        QObject::connect( outLayer.get(), &KnobChoice::populated, this, [this]() { relayout(); update(); } );
    }

    for (int c = 0; c < 2; ++c) {
        _inCombo[c] = new ComboBox(this);
        _inCombo[c]->setFocusPolicy(Qt::NoFocus);
        QObject::connect( _inCombo[c], static_cast<void (ComboBox::*)(int)>(&ComboBox::currentIndexChanged), this,
                          [this, c](int index) { onInputLayerChosen(c, index); } );
    }
    if (_outCombo) {
        _outCombo->setParent(this);
        _outCombo->show();
    }
    rebuild();
}

SolvfxShuffleWidget::~SolvfxShuffleWidget()
{
}

QString
SolvfxShuffleWidget::displayPlane(const QString& plane)
{
    if ( plane.isEmpty() || (plane == QString::fromUtf8("Color")) ) {
        return QString::fromUtf8("rgba");
    }

    return plane.toLower();
}

QString
SolvfxShuffleWidget::displayChannel(const QString& plane,
                                    const QString& chan) const
{
    if ( plane.isEmpty() || (plane == QString::fromUtf8("Color")) ) {
        const QString c = chan.toUpper();
        if ( c == QString::fromUtf8("R") ) {
            return QString::fromUtf8("red");
        } else if ( c == QString::fromUtf8("G") ) {
            return QString::fromUtf8("green");
        } else if ( c == QString::fromUtf8("B") ) {
            return QString::fromUtf8("blue");
        } else if ( c == QString::fromUtf8("A") ) {
            return QString::fromUtf8("alpha");
        }
    }

    return chan.toLower();
}

QColor
SolvfxShuffleWidget::channelColor(const QString& chan) const
{
    const QString c = chan.toLower();
    if ( (c == QString::fromUtf8("r")) || (c == QString::fromUtf8("red")) || (c == QString::fromUtf8("x")) ) {
        return QColor(0xd3, 0x3a, 0x3a);
    } else if ( (c == QString::fromUtf8("g")) || (c == QString::fromUtf8("green")) || (c == QString::fromUtf8("y")) ) {
        return QColor(0x2f, 0xa8, 0x4f);
    } else if ( (c == QString::fromUtf8("b")) || (c == QString::fromUtf8("blue")) ) {
        return QColor(0x55, 0x77, 0xff);
    } else if ( (c == QString::fromUtf8("a")) || (c == QString::fromUtf8("alpha")) ) {
        return QColor(0xc8, 0xc8, 0xc8);
    }

    return QColor(0x9a, 0x9a, 0x9a);
}

// menu entries look like "B.Color.R", "A.Depth.Z", "0", "1"
void
SolvfxShuffleWidget::rebuild()
{
    KnobChoicePtr k = _outKnobs[0].lock();
    _planes[0].clear();
    _planes[1].clear();
    _const0 = _const1 = -1;
    if (k) {
        const std::vector<ChoiceOption> entries = k->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const QString label = QString::fromUtf8( entries[i].label.c_str() );
            if ( label == QString::fromUtf8("0") ) {
                _const0 = (int)i;
                continue;
            }
            if ( label == QString::fromUtf8("1") ) {
                _const1 = (int)i;
                continue;
            }
            const int first = label.indexOf( QLatin1Char('.') );
            const int last = label.lastIndexOf( QLatin1Char('.') );
            if ( (first < 0) || (last <= first) ) {
                continue;
            }
            const QString clip = label.left(first);
            int c = -1;
            if ( clip == QString::fromUtf8("B") ) {
                c = 0;
            } else if ( clip == QString::fromUtf8("A") ) {
                c = 1;
            }
            if (c < 0) {
                continue;
            }
            const QString plane = label.mid(first + 1, last - first - 1);
            const QString chan = label.mid(last + 1);
            Plane* p = 0;
            for (std::size_t j = 0; j < _planes[c].size(); ++j) {
                if (_planes[c][j].label == plane) {
                    p = &_planes[c][j];
                    break;
                }
            }
            if (!p) {
                Plane np;
                np.label = plane;
                _planes[c].push_back(np);
                p = &_planes[c].back();
            }
            Channel ch;
            ch.name = chan;
            ch.entry = (int)i;
            p->channels.push_back(ch);
        }
    }
    // shown plane: keep the current one if still there, else the one used by a line, else Color
    for (int c = 0; c < 2; ++c) {
        bool keep = false;
        for (std::size_t j = 0; j < _planes[c].size(); ++j) {
            if (_planes[c][j].label == _shownPlane[c]) {
                keep = true;
            }
        }
        if (!keep) {
            _shownPlane[c].clear();
            for (int o = 0; o < 4 && _shownPlane[c].isEmpty(); ++o) {
                int cl, row;
                QString plane;
                if ( sourceOfEntry(currentEntry(o), &cl, &plane, &row) && (cl == c) ) {
                    _shownPlane[c] = plane;
                }
            }
            if ( _shownPlane[c].isEmpty() ) {
                for (std::size_t j = 0; j < _planes[c].size(); ++j) {
                    if ( _planes[c][j].label == QString::fromUtf8("Color") ) {
                        _shownPlane[c] = _planes[c][j].label;
                    }
                }
            }
            if ( _shownPlane[c].isEmpty() && !_planes[c].empty() ) {
                _shownPlane[c] = _planes[c][0].label;
            }
        }
        refreshInputCombo(c);
    }
    relayout();
    update();
} // SolvfxShuffleWidget::rebuild

void
SolvfxShuffleWidget::refreshInputCombo(int clip)
{
    ComboBox* cb = _inCombo[clip];
    cb->blockSignals(true);
    cb->clear();
    if ( _planes[clip].empty() ) {
        cb->addItem( QString::fromUtf8("none") );
        cb->setEnabled(false);
    } else {
        cb->setEnabled(true);
        int cur = 0;
        for (std::size_t j = 0; j < _planes[clip].size(); ++j) {
            cb->addItem( displayPlane(_planes[clip][j].label) );
            if (_planes[clip][j].label == _shownPlane[clip]) {
                cur = (int)j;
            }
        }
        cb->setCurrentIndex_no_emit(cur);
    }
    cb->blockSignals(false);
}

void
SolvfxShuffleWidget::onInputLayerChosen(int clip,
                                        int index)
{
    if ( (index < 0) || ( index >= (int)_planes[clip].size() ) ) {
        return;
    }
    _shownPlane[clip] = _planes[clip][index].label;
    relayout();
    update();
}

const SolvfxShuffleWidget::Plane*
SolvfxShuffleWidget::shownPlane(int clip) const
{
    for (std::size_t j = 0; j < _planes[clip].size(); ++j) {
        if (_planes[clip][j].label == _shownPlane[clip]) {
            return &_planes[clip][j];
        }
    }

    return 0;
}

// output layer label "Color.RGBA" -> channels "RGBA", plane "Color"
QString
SolvfxShuffleWidget::parseOutputChannels(QString* planeLabel) const
{
    KnobChoicePtr k = _layerKnob.lock();
    QString label = k ? QString::fromUtf8( k->getActiveEntry().label.c_str() ) : QString();
    const int dot = label.lastIndexOf( QLatin1Char('.') );
    if (dot < 0) {
        *planeLabel = QString::fromUtf8("Color");

        return QString::fromUtf8("RGBA");
    }
    *planeLabel = label.left(dot);

    return label.mid(dot + 1);
}

QString
SolvfxShuffleWidget::outputPlane() const
{
    QString plane;
    parseOutputChannels(&plane);

    return plane;
}

void
SolvfxShuffleWidget::outputRows(std::vector<std::pair<QString, int> >* rows) const
{
    rows->clear();
    QString plane;
    QString chans = parseOutputChannels(&plane);
    if ( plane.isEmpty() || (plane == QString::fromUtf8("Color")) ) {
        // the colour plane is rgba unless Output Components says otherwise
        KnobChoicePtr k = _layerKnob.lock();
        EffectInstance* fx = k ? dynamic_cast<EffectInstance*>( k->getHolder() ) : 0;
        KnobChoicePtr comps = fx ? std::dynamic_pointer_cast<KnobChoice>( fx->getKnobByName("outputComponents") ) : KnobChoicePtr();
        const QString c = comps ? QString::fromUtf8( comps->getActiveEntry().id.c_str() ).toLower() : QString::fromUtf8("rgba");
        if ( c == QString::fromUtf8("alpha") ) {
            rows->push_back( std::make_pair(QString::fromUtf8("A"), 3) );

            return;
        }
        chans = ( c == QString::fromUtf8("rgb") ) ? QString::fromUtf8("RGB") : QString::fromUtf8("RGBA");
    }
    const int n = std::min( 4, std::max( 1, (int)chans.size() ) );
    for (int i = 0; i < n; ++i) {
        const QString ch = ( i < chans.size() ) ? QString( chans.at(i) ) : QString::number(i);
        rows->push_back( std::make_pair(ch, i) );
    }
}

int
SolvfxShuffleWidget::currentEntry(int knob) const
{
    KnobChoicePtr k = _outKnobs[knob].lock();

    return k ? k->getValue() : -1;
}

void
SolvfxShuffleWidget::setEntry(int knob,
                              int entry)
{
    KnobChoicePtr k = _outKnobs[knob].lock();
    if ( !k || (entry < 0) ) {
        return;
    }
    const int old = k->getValue();
    if (old == entry) {
        return;
    }
    if (_undoHost) {
        _undoHost->pushUndoCommand( new SolvfxShuffleUndo(k, old, entry, this) ); // redo() applies it
    } else {
        k->setValue(entry, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    }
    update();
}

bool
SolvfxShuffleWidget::sourceOfEntry(int entry,
                                   int* clip,
                                   QString* plane,
                                   int* row) const
{
    if (entry < 0) {
        return false;
    }
    if (entry == _const0) {
        *clip = 2; *row = 0; plane->clear();

        return true;
    }
    if (entry == _const1) {
        *clip = 2; *row = 1; plane->clear();

        return true;
    }
    for (int c = 0; c < 2; ++c) {
        for (std::size_t j = 0; j < _planes[c].size(); ++j) {
            const Plane& p = _planes[c][j];
            for (std::size_t r = 0; r < p.channels.size(); ++r) {
                if (p.channels[r].entry == entry) {
                    *clip = c;
                    *plane = p.label;
                    *row = (int)r;

                    return true;
                }
            }
        }
    }

    return false;
}

// where a line from this entry starts: its row dot, or the block header if its layer is not shown
QPointF
SolvfxShuffleWidget::sourceDot(int entry) const
{
    for (std::size_t i = 0; i < _sources.size(); ++i) {
        if (_sources[i].entry == entry) {
            return _sources[i].dot;
        }
    }
    int clip, row;
    QString plane;
    if ( sourceOfEntry(entry, &clip, &plane, &row) && (clip < 2) ) {
        // its layer is not the one shown: start at the block header
        const QRectF& b = _blockRect[clip];

        return QPointF( b.right(), b.top() + D(kHeaderH) / 2. );
    }

    return QPointF(-1, -1); // constants (0 / 1) or nothing: no line
}

// Nuke-like: input block (B, then A under it) on the left, output block on the right,
// rows facing each other so the lines are short
void
SolvfxShuffleWidget::relayout()
{
    _sources.clear();
    _targets.clear();
    const double m = D(kMargin), hh = D(kHeaderH), rh = D(kRowH), pad = D(kBlockPad), gap = D(kGap), lw = D(kLinesW);
    const double bw = std::max( D(kMinBlockW), std::min( D(kMaxBlockW), (width() - 2 * m - lw) / 2. ) );
    const double lx = m, rx = m + bw + lw;

    double y = m;
    for (int c = 0; c < 2; ++c) {
        const Plane* p = shownPlane(c);
        const int n = p ? (int)p->channels.size() : 4;
        const double h = hh + n * rh + pad;
        _blockRect[c] = QRectF(lx, y, bw, h);
        for (int r = 0; r < n; ++r) {
            Source s;
            s.clip = c;
            s.row = r;
            s.entry = p ? p->channels[r].entry : -1;
            s.dot = QPointF( lx + bw, y + hh + r * rh + rh / 2. );
            s.hit = QRectF(lx, y + hh + r * rh, bw + D(8), rh);
            _sources.push_back(s);
        }
        _inCombo[c]->setGeometry( (int)(lx + D(22)), (int)(y + D(3)), (int)(bw - D(28)), (int)(hh - D(6)) );
        y += h + gap;
    }
    const double leftBottom = y - gap + m;

    // output block, aligned with the B block
    std::vector<std::pair<QString, int> > rows;
    outputRows(&rows);
    const QString plane = outputPlane();
    const double oh = hh + rows.size() * rh + pad;
    _blockRect[2] = QRectF(rx, m, bw, oh);
    const double bs = D(13); // 0 / 1 button size
    for (std::size_t r = 0; r < rows.size(); ++r) {
        Target t;
        t.knob = rows[r].second;
        t.label = displayPlane(plane) + QLatin1Char('.') + displayChannel(plane, rows[r].first);
        const double cy = m + hh + r * rh + rh / 2.;
        t.dot = QPointF(rx, cy);
        t.hit = QRectF( rx - D(8), cy - rh / 2., bw + D(8), rh );
        t.zero = QRectF( rx + D(10), cy - bs / 2., bs, bs );
        t.one = QRectF( rx + D(10) + bs + D(3), cy - bs / 2., bs, bs );
        _targets.push_back(t);
    }
    if (_outCombo) {
        _outCombo->setGeometry( (int)(rx + D(6)), (int)(m + D(3)), (int)(bw - D(12)), (int)(hh - D(6)) );
    }
    const int hNeeded = (int)std::ceil( std::max(leftBottom, m + oh + m) );
    if ( height() != hNeeded ) {
        setFixedHeight(hNeeded);
    }
} // SolvfxShuffleWidget::relayout

void
SolvfxShuffleWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    relayout();
}

static QPainterPath
wirePath(const QPointF& a,
         const QPointF& b)
{
    QPainterPath path(a);
    const double dx = std::max( 8., (b.x() - a.x()) * 0.45 );
    path.cubicTo( QPointF(a.x() + dx, a.y()), QPointF(b.x() - dx, b.y()), b );

    return path;
}

void
SolvfxShuffleWidget::paintEvent(QPaintEvent* /*e*/)
{
    std::vector<std::pair<QString, int> > rows;
    outputRows(&rows);
    if ( rows.size() != _targets.size() ) {
        relayout(); // the output layer changed
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f = font();
    f.setPointSizeF( std::max( 6., f.pointSizeF() * 0.92 ) );
    p.setFont(f);
    const double hh = D(kHeaderH), rh = D(kRowH), dotR = D(4);
    const QColor blockBg(0x2a, 0x2a, 0x2a), border(0x14, 0x14, 0x14), sep(0x3c, 0x3c, 0x3c), text(0xc8, 0xc8, 0xc8), tag(0xe2, 0xc7, 0x9a);

    std::vector<int> used;
    for (std::size_t r = 0; r < _targets.size(); ++r) {
        used.push_back( currentEntry(_targets[r].knob) );
    }
    const auto isUsed = [&](int entry) { return std::find(used.begin(), used.end(), entry) != used.end(); };

    // blocks
    for (int b = 0; b < 3; ++b) {
        const QRectF& r = _blockRect[b];
        p.setPen( QPen(border, 1) );
        p.setBrush(blockBg);
        p.drawRoundedRect(r, D(2), D(2));
        p.setPen( QPen(sep, 1) );
        p.drawLine( QPointF(r.left() + 1, r.top() + hh), QPointF(r.right() - 1, r.top() + hh) );
        if (b < 2) {
            QFont bf = f;
            bf.setBold(true);
            p.setFont(bf);
            p.setPen(tag);
            p.drawText( QRectF(r.left() + D(6), r.top(), D(14), hh), Qt::AlignVCenter | Qt::AlignLeft, (b == 0) ? QString::fromUtf8("B") : QString::fromUtf8("A") );
            p.setFont(f);
        }
    }

    // input rows: "rgba.red" right-aligned, dot on the right edge
    for (std::size_t i = 0; i < _sources.size(); ++i) {
        const Source& s = _sources[i];
        const Plane* pl = shownPlane(s.clip);
        const QString chan = pl ? pl->channels[s.row].name : QString::fromUtf8("RGBA").mid(s.row, 1);
        const QString planeName = pl ? pl->label : QString();
        const QString name = displayChannel(planeName, chan);
        const bool on = (s.entry >= 0) && isUsed(s.entry);
        p.setOpacity( (s.entry < 0) ? 0.3 : (on ? 1. : 0.55) );
        p.setPen(text);
        p.drawText( QRectF(s.hit.left() + D(6), s.dot.y() - rh / 2., s.dot.x() - s.hit.left() - D(16), rh),
                    Qt::AlignVCenter | Qt::AlignRight, displayPlane(planeName) + QLatin1Char('.') + name );
        p.setPen( QPen(QColor(0x11, 0x11, 0x11), 1) );
        p.setBrush( on ? channelColor(name) : QColor(0x55, 0x55, 0x55) );
        p.drawEllipse(s.dot, dotR, dotR);
    }
    p.setOpacity(1.);

    // output rows: dot on the left edge, 0 / 1 buttons, "rgba.red"
    for (std::size_t r = 0; r < _targets.size(); ++r) {
        const Target& t = _targets[r];
        const int entry = currentEntry(t.knob);
        const bool isZero = (entry == _const0) || (entry < 0);
        const bool isOne = (entry == _const1);
        const QString chanName = t.label.mid( t.label.indexOf( QLatin1Char('.') ) + 1 );
        p.setPen( QPen(QColor(0x11, 0x11, 0x11), 1) );
        p.setBrush( (isZero || isOne) ? QColor(0x55, 0x55, 0x55) : channelColor(chanName) );
        p.drawEllipse(t.dot, dotR, dotR);
        // 0 = black, 1 = white; lit when that constant is used
        p.setPen( QPen(isZero ? QColor(0xe0, 0x74, 0x3a) : QColor(0x60, 0x60, 0x60), 1) );
        p.setBrush( QColor(0x10, 0x10, 0x10) );
        p.drawEllipse( t.zero.center(), t.zero.width() / 2. - 0.5, t.zero.height() / 2. - 0.5 );
        p.setPen( QPen(isOne ? QColor(0xe0, 0x74, 0x3a) : QColor(0x60, 0x60, 0x60), 1) );
        p.setBrush( isOne ? QColor(0xf0, 0xf0, 0xf0) : QColor(0x9a, 0x9a, 0x9a) );
        p.drawEllipse( t.one.center(), t.one.width() / 2. - 0.5, t.one.height() / 2. - 0.5 );
        p.setPen(text);
        p.drawText( QRectF(t.one.right() + D(6), t.dot.y() - rh / 2., _blockRect[2].right() - t.one.right() - D(8), rh),
                    Qt::AlignVCenter | Qt::AlignLeft, t.label );
    }

    // lines
    for (std::size_t r = 0; r < _targets.size(); ++r) {
        const Target& t = _targets[r];
        const int entry = currentEntry(t.knob);
        const QPointF from = sourceDot(entry);
        if (from.x() < 0) {
            continue;
        }
        int clip, row;
        QString plane;
        QColor col(0x9a, 0x9a, 0x9a);
        if ( sourceOfEntry(entry, &clip, &plane, &row) && (clip < 2) ) {
            for (std::size_t j = 0; j < _planes[clip].size(); ++j) {
                if (_planes[clip][j].label == plane) {
                    col = channelColor( displayChannel(plane, _planes[clip][j].channels[row].name) );
                }
            }
        }
        const bool sel = (t.knob == _selectedKnob);
        if (sel) {
            col = QColor(0xe0, 0x74, 0x3a);
        }
        QPen pen(col, sel ? D(2.4) : D(1.8));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath( wirePath(from, t.dot) );
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawEllipse(from, dotR - D(1), dotR - D(1));
        p.drawEllipse(t.dot, dotR - D(1), dotR - D(1));
    }

    if (_dragging) {
        QPen pen( QColor(0xe8, 0xe8, 0xe8), D(1.6), Qt::DashLine );
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath( wirePath(_dragFrom, _dragTo) );
    }
} // SolvfxShuffleWidget::paintEvent

int
SolvfxShuffleWidget::wireAt(const QPointF& pos) const
{
    QPainterPathStroker stroker;
    stroker.setWidth( D(8) );
    for (std::size_t r = 0; r < _targets.size(); ++r) {
        const QPointF from = sourceDot( currentEntry(_targets[r].knob) );
        if (from.x() < 0) {
            continue;
        }
        if ( stroker.createStroke( wirePath(from, _targets[r].dot) ).contains(pos) ) {
            return _targets[r].knob;
        }
    }

    return -1;
}

void
SolvfxShuffleWidget::mousePressEvent(QMouseEvent* e)
{
    if ( e->button() != Qt::LeftButton ) {
        QWidget::mousePressEvent(e);

        return;
    }
    const QPointF pos = e->pos();
    for (std::size_t r = 0; r < _targets.size(); ++r) {
        if ( _targets[r].zero.adjusted( -2, -2, 2, 2 ).contains(pos) ) {
            setEntry(_targets[r].knob, _const0);

            return;
        }
        if ( _targets[r].one.adjusted( -2, -2, 2, 2 ).contains(pos) ) {
            setEntry(_targets[r].knob, _const1);

            return;
        }
    }
    for (std::size_t i = 0; i < _sources.size(); ++i) {
        if ( _sources[i].hit.contains(pos) && (_sources[i].entry >= 0) ) {
            _dragEntry = _sources[i].entry;
            _dragging = true;
            _dragFrom = _sources[i].dot;
            _dragTo = pos;
            _selectedKnob = -1;
            update();

            return;
        }
    }
    _selectedKnob = wireAt(pos);
    setFocus();
    update();
}

void
SolvfxShuffleWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (_dragging) {
        _dragTo = e->pos();
        update();
    }
}

void
SolvfxShuffleWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (!_dragging) {
        return;
    }
    _dragging = false;
    const QPointF pos = e->pos();
    for (std::size_t r = 0; r < _targets.size(); ++r) {
        if ( _targets[r].hit.contains(pos) ) {
            setEntry(_targets[r].knob, _dragEntry);
            break;
        }
    }
    _dragEntry = -1;
    update();
}

void
SolvfxShuffleWidget::keyPressEvent(QKeyEvent* e)
{
    if ( (_selectedKnob >= 0) && ( (e->key() == Qt::Key_Delete) || (e->key() == Qt::Key_Backspace) ) ) {
        setEntry(_selectedKnob, _const0);
        _selectedKnob = -1;
        e->accept();

        return;
    }
    QWidget::keyPressEvent(e);
}

NATRON_NAMESPACE_EXIT
