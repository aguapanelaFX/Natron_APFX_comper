/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (a Natron fork) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX: Nuke-like "noodle" panel for the Shuffle node (net.sf.openfx.ShufflePlugin).
// Input layers (B, A) on the left, the output layer on the right; drag lines from an
// input channel (or 0 / 1) to an output channel. The lines drive the Shuffle's own
// outputR / outputG / outputB / outputA menus (hidden), so rendering is unchanged.

#ifndef Gui_SolvfxShuffleWidget_h
#define Gui_SolvfxShuffleWidget_h

#include <Python.h>

#include "Global/Macros.h"

#include <vector>
#include <map>

#include <QWidget>
#include <QString>
#include <QPointF>
#include <QRectF>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"


NATRON_NAMESPACE_ENTER

class SolvfxShuffleWidget
    : public QWidget
{
public:
    // outLayer: the Shuffle's "outputLayer" knob; outCombo: its combo box (re-parented here)
    SolvfxShuffleWidget(const KnobChoicePtr& outLayer,
                        QWidget* outCombo,
                        QWidget* parent);

    virtual ~SolvfxShuffleWidget();

    // true for the knob whose GUI is replaced by this panel
    static bool isShuffleLayerKnob(const KnobIPtr& knob);
    // true for the Shuffle knobs the panel replaces (always hidden in the GUI)
    static bool isHiddenShuffleKnob(const KnobIPtr& knob);

    // edits go through this knob gui's undo stack (Ctrl+Z / Ctrl+Shift+Z)
    void setUndoHost(KnobGui* gui) { _undoHost = gui; }

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual void resizeEvent(QResizeEvent* e) OVERRIDE FINAL;

private:
    KnobGui* _undoHost = 0;

    struct Channel
    {
        QString name;   // channel letter as in the menu ("R", "Z", ...)
        int entry;      // index in the outputR.. menus
    };

    struct Plane
    {
        QString label;  // plane label as in the menu ("Color", "Depth", ...)
        std::vector<Channel> channels;
    };

    // a left-side source (input channel row) with its dot position
    struct Source
    {
        int clip;       // 0 = B, 1 = A
        int row;        // channel row
        int entry;      // menu index, -1 if none
        QPointF dot;
        QRectF hit;
    };

    struct Target
    {
        int knob;       // 0..3 = outputR / G / B / A
        QString label;  // "rgba.red"
        QPointF dot;
        QRectF hit;
        QRectF zero, one; // the 0 / 1 buttons of the row
    };

    void rebuild();
    void relayout();
    void refreshInputCombo(int clip);
    void onInputLayerChosen(int clip, int index);
    const Plane* shownPlane(int clip) const;
    QString parseOutputChannels(QString* planeLabel) const;
    void outputRows(std::vector<std::pair<QString, int> >* rows) const; // (channel name, knob index)
    QString outputPlane() const;
    int currentEntry(int knob) const;
    void setEntry(int knob, int entry);
    bool sourceOfEntry(int entry, int* clip, QString* plane, int* row) const;
    QPointF sourceDot(int entry) const;
    QColor channelColor(const QString& chan) const;
    QString displayChannel(const QString& plane, const QString& chan) const;
    static QString displayPlane(const QString& plane);
    int wireAt(const QPointF& p) const;

    KnobChoiceWPtr _layerKnob;
    KnobChoiceWPtr _outKnobs[4];
    QWidget* _outCombo;
    ComboBox* _inCombo[2];

    std::vector<Plane> _planes[2];            // available planes of B, A
    QString _shownPlane[2];                   // plane shown in each input block
    int _const0, _const1;                     // menu indices of "0" / "1"

    // geometry
    QRectF _blockRect[3];                     // B, A, out
    std::vector<Source> _sources;
    std::vector<Target> _targets;

    // interaction
    int _dragEntry;                           // entry being dragged, -1 if none
    bool _dragging;
    QPointF _dragFrom, _dragTo;
    int _selectedKnob;                        // selected line (by output knob), -1 if none
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SolvfxShuffleWidget_h
