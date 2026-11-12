/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// Scene graph tree of a 3D file (ReadGeo "Scene Graph" tab): search, type filters, depth, and per prim
// payload / active / visible toggles. The toggles are stored in the knob (one edit per line).

#ifndef SOLVFX_KNOBGUISCENEGRAPH_H
#define SOLVFX_KNOBGUISCENEGRAPH_H

#include <Python.h>

#include "Global/Macros.h"

#include <map>
#include <set>
#include <string>

#include "Engine/EngineFwd.h"
#include "Gui/KnobGui.h"
#include "Gui/GuiFwd.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QToolButton;
class QSpinBox;
class QLabel;
class QTimer;

NATRON_NAMESPACE_ENTER

class KnobGuiSceneGraph
    : public KnobGui
{
public:
    static KnobGui * BuildKnobGui(KnobIPtr knob,
                                  KnobGuiContainerI *container)
    {
        return new KnobGuiSceneGraph(knob, container);
    }

    KnobGuiSceneGraph(KnobIPtr knob, KnobGuiContainerI *container);
    virtual ~KnobGuiSceneGraph() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;
    virtual KnobIPtr getKnob() const OVERRIDE FINAL;
    virtual bool isLabelOnSameColumn() const OVERRIDE FINAL { return true; }

private:
    virtual bool shouldAddStretch() const OVERRIDE FINAL { return false; }
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;
    virtual void setEnabled() OVERRIDE FINAL {}
    virtual void setReadOnly(bool, int) OVERRIDE FINAL {}
    virtual void setDirty(bool) OVERRIDE FINAL {}
    virtual void updateGUI(int) OVERRIDE FINAL;
    virtual void reflectAnimationLevel(int, AnimationLevelEnum) OVERRIDE FINAL {}
    virtual void reflectExpressionState(int, bool) OVERRIDE FINAL {}
    virtual void updateToolTip() OVERRIDE FINAL {}

    void poll();
    void rebuild();
    void applyFilter();
    void writeEdits();
    void resetFlag(char f);

    KnobSceneGraphWPtr _knob;
    QWidget* _w = 0;
    QLineEdit* _search = 0;
    QToolButton* _filter[4];
    QSpinBox* _depth = 0;
    QTreeWidget* _tree = 0;
    QLabel* _message = 0;
    QTimer* _timer = 0;
    int _rev = -1;
    bool _building = false;
    std::set<std::string> _expanded;
    // edits currently stored: flag char -> paths
    std::map<char, std::set<std::string> > _edits;
};

NATRON_NAMESPACE_EXIT

#endif // SOLVFX_KNOBGUISCENEGRAPH_H
