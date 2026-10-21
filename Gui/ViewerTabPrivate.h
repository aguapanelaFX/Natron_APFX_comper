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

#ifndef Gui_ViewerTabPrivate_h
#define Gui_ViewerTabPrivate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <set>
#include <vector>
#include <cmath>
#include <algorithm>
#include <QString>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QMutex>
#include <QTimer>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/Enums.h"

#include "Engine/ViewIdx.h"
#include "Engine/EngineFwd.h"

#include "Gui/ComboBox.h"
#include "Gui/GuiFwd.h"
#include "Gui/NodeViewerContext.h"


#define NATRON_TRANSFORM_AFFECTS_OVERLAYS

#include "Gui/SolvfxOcioViewer.h"

NATRON_NAMESPACE_ENTER

// SOLVFX: Nuke-like viewer exposure/gamma helpers
// Gamma slider: positions 0..2, value 0..1 on the left half and 1..4 on the right half
inline double
solvfxGammaFromSliderPos(double pos)
{
    return (pos <= 1.) ? std::max(0., pos) : 1. + (pos - 1.) * 3.;
}

inline double
solvfxGammaToSliderPos(double gamma)
{
    return (gamma <= 1.) ? std::max(0., gamma) : 1. + (gamma - 1.) / 3.;
}

// f-number shown on the exposure button: f/8 at gain 1, one stop up = f/5.6 ...
inline QString
solvfxFStopLabel(double stops)
{
    static const char* names[] = { "1", "1.4", "2", "2.8", "4", "5.6", "8", "11", "16", "22", "32", "45", "64", "90", "128" };
    const int idx = 6 - (int)std::floor(stops + 0.5); // index of f/8 is 6
    if ( (std::abs( stops - std::floor(stops + 0.5) ) < 1e-6) && (idx >= 0) && (idx < 15) ) {
        return QString::fromUtf8("f/") + QString::fromUtf8(names[idx]);
    }
    double f = 8. * std::pow(2., -stops / 2.);
    return QString::fromUtf8("f/") + QString::number(f, 'g', 2);
}

struct ViewerTabPrivate
{
    struct InputName
    {
        QString name;
        NodeWPtr input;
    };

    typedef std::map<int, InputName> InputNamesMap;

    ViewerTab* publicInterface;

    /*OpenGL viewer*/
    ViewerGL* viewer;
    QWidget* viewerContainer;
    QHBoxLayout* viewerLayout;
    QWidget* viewerSubContainer;
    QVBoxLayout* viewerSubContainerLayout;
    QVBoxLayout* mainLayout;

    /*Viewer Settings*/
    QWidget* firstSettingsRow, *secondSettingsRow;
    QHBoxLayout* firstRowLayout, *secondRowLayout;

    /*1st row*/
    //ComboBox* viewerLayers;
    ComboBox* layerChoice;
    ComboBox* alphaChannelChoice;
    mutable QMutex currentLayerMutex;
    QString currentLayerChoice, currentAlphaLayerChoice;
    ChannelsComboBox* viewerChannels;
    bool viewerChannelsAutoswitchedToAlpha;
    ComboBox* zoomCombobox;
    Button* syncViewerButton;
    Button* centerViewerButton;
    Button* clipToProjectFormatButton;
    Button* fullFrameProcessingButton;
    Button* enableViewerRoI;
    Button* refreshButton;
    Button* pauseButton;
    QIcon iconRefreshOff, iconRefreshOn;
    Button* activateRenderScale;
    bool renderScaleActive;
    ComboBox* renderScaleCombo;
    Label* firstInputLabel;
    ComboBox* firstInputImage;
    Label* compositingOperatorLabel;
    ComboBox* compositingOperator;
    Label* secondInputLabel;
    ComboBox* secondInputImage;

    /*2nd row*/
    Button* toggleGainButton;
    SpinBox* gainBox;
    ScaleSliderQWidget* gainSlider;
    double lastFstopValue;
    Button* autoContrast;
    SpinBox* gammaBox;
    double lastGammaValue;
    Button* toggleGammaButton;
    ScaleSliderQWidget* gammaSlider;
    // SOLVFX: viewer saturation (preview only) and f-stop step buttons
    Button* toggleSaturationButton = nullptr;
    SpinBox* saturationBox = nullptr;
    ScaleSliderQWidget* saturationSlider = nullptr;
    double lastSaturationValue = 1.;
    Button* gainDownButton = nullptr;
    Button* gainUpButton = nullptr;
    ComboBox* viewerColorSpace;
    // SOLVFX: OCIO display/views listed in viewerColorSpace (empty = legacy LUT list)
    std::vector<SolvfxOcioViewerShader::DisplayView> ocioViews;
    Button* checkerboardButton;
    Button* pickerButton;
    ComboBox* viewsComboBox;
    ViewIdx currentViewIndex;
    QMutex currentViewMutex;
    /*Info*/
    InfoViewerWidget* infoWidget[2];


    /*TimeLine buttons*/
    QWidget* playerButtonsContainer;
    QHBoxLayout* playerLayout;
    SpinBox* currentFrameBox;
    Button* firstFrame_Button;
    Button* previousKeyFrame_Button;
    Button* play_Backward_Button;
    Button* previousFrame_Button;
    Button* nextFrame_Button;
    Button* play_Forward_Button;
    Button* nextKeyFrame_Button;
    Button* lastFrame_Button;
    Button* previousIncrement_Button;
    SpinBox* incrementSpinBox;
    Button* nextIncrement_Button;
    Button* playbackMode_Button;
    Button* playBackInputButton;
    SpinBox* playBackInputSpinbox;
    Button* playBackOutputButton;
    SpinBox* playBackOutputSpinbox;
    mutable QMutex playbackModeMutex;
    PlaybackModeEnum playbackMode;
    Button* tripleSyncButton;
    QCheckBox* canEditFpsBox;
    ClickableLabel* canEditFpsLabel;
    mutable QMutex fpsLockedMutex;
    bool fpsLocked;
    SpinBox* fpsBox;
    double userFps;
    ComboBox* timeFormat;
    Button* turboButton;
    QTimer mustSetUpPlaybackButtonsTimer;

    /*frame seeker*/
    TimeLineGui* timeLineGui;
    // SOLVFX: timeline range dropdown (Global / Input / In-Out) and its row
    ComboBox* timelineRangeCombo = nullptr;
    QWidget* timelineRow = nullptr;
    int timelineRangeMode = 0; // 0 = Global, 1 = Input, 2 = In/Out
    bool inOutPointsSet = false;
    int inPoint = 0;  // SOLVFX: playback in point (I), the orange marker
    int outPoint = 0; // SOLVFX: playback out point (O)
    bool solvfxApplyingRange = false; // true while the dropdown sets the bounds
    QTimer* solvfxEmptyPlayTimer = nullptr; // SOLVFX: plays the timeline when the viewer has no input
    int solvfxEmptyPlayDir = 1;
    int solvfxPrevRangeMode = 0;      // SOLVFX: mode to go back to from Visible
    bool solvfxVisibleSet = false;
    int solvfxVisibleFirst = 0, solvfxVisibleLast = 0;
    Button* solvfxClearInOutButton = nullptr; // SOLVFX: clears the in / out points

    // This is all nodes that have a viewer context
    typedef std::map<NodeGuiWPtr, NodeViewerContextPtr, std::owner_less<NodeGuiWPtr>> NodeViewerContextsMap;
    NodeViewerContextsMap nodesContext;

    /*
       Tells for a given plug-in ID, which nodes is currently activated in the viewer interface
     */
    struct PluginViewerContext
    {
        std::string pluginID;
        NodeGuiWPtr currentNode;
        NodeViewerContextPtr currentContext;
    };


    // This is the current active context for each plug-in
    // We don't use a map because we need to retain the insertion order for each plug-in
    std::list<PluginViewerContext> currentNodeContext;
    InputNamesMap inputNamesMap;
    mutable QMutex compOperatorMutex;
    ViewerCompositingOperatorEnum compOperator;
    ViewerCompositingOperatorEnum compOperatorPrevious;
    ViewerInstance* viewerNode; // < pointer to the internal node
    mutable QMutex visibleToolbarsMutex; //< protects the 4 bool below
    bool infobarVisible;
    bool playerVisible;
    bool timelineVisible;
    bool leftToolbarVisible;
    bool rightToolbarVisible;
    bool topToolbarVisible;
    bool isFileDialogViewer;
    mutable QMutex checkerboardMutex;
    bool checkerboardEnabled;
    mutable QMutex fpsMutex;
    double fps;

    bool hasPenDown;
    bool hasCaughtPenMotionWhileDragging;

    ViewerTabPrivate(ViewerTab* publicInterface,
                     ViewerInstance* node);

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
    // return the tronsform to apply to the overlay as a 3x3 homography in canonical coordinates
    bool getOverlayTransform(double time,
                             ViewIdx view,
                             const NodePtr& target,
                             EffectInstance* currentNode,
                             Transform::Matrix3x3* transform) const;

    bool getTimeTransform(double time,
                          ViewIdx view,
                          const NodePtr& target,
                          EffectInstance* currentNode,
                          double *newTime) const;

#endif

    void getComponentsAvailabel(std::set<ImagePlaneDesc>* comps) const;

    std::list<PluginViewerContext>::iterator findActiveNodeContextForPlugin(const std::string& pluginID);

    // Returns true if this node has a viewer context but it is not active
    bool hasInactiveNodeViewerContext(const NodePtr& node);
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ViewerTabPrivate_h
