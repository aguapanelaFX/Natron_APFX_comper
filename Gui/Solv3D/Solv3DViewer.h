/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX 3D view: lives inside a Viewer (2D / 3D switch in the viewer toolbar, Tab toggles).
// Shows the 3D scene upstream of the viewer input; the selected 3D nodes are highlighted (dim yellow edges),
// selected nodes that are not connected show as a template (shaded, semi transparent, dim yellow edges).
// Maya navigation: Alt+LMB tumble, Alt+MMB track, Alt+RMB dolly, wheel zoom. Keys: F frame, X / Shift+X
// right / left, C / Shift+C top / bottom, Z / Shift+Z front / back, V perspective, H look through the
// selected camera. Click picks geometry (selects its node), Shift+click adds.
// Textures are drawn from small preview renders (made in the background, at most 512 px).

#ifndef SOLVFX_SOLV3DVIEWER_H
#define SOLVFX_SOLV3DVIEWER_H

#include "Global/Macros.h"
#include "Global/GlobalDefines.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <QOpenGLWidget>
#include <QWidget>

#include "Engine/EngineFwd.h"
#include "Engine/Solv3D/SolvScene.h"
#include "Gui/GuiFwd.h"

class QComboBox;
class QToolButton;
class QTimer;

NATRON_NAMESPACE_ENTER

struct Solv3DTexJob;

class Solv3DGLWidget
    : public QOpenGLWidget
{
public:
    explicit Solv3DGLWidget(QWidget* parent);
    virtual ~Solv3DGLWidget();

    // main scene (upstream of the viewer) and template scene (selected, not connected)
    void setScenes(const Solv3D::Scene& main, const Solv3D::Scene& templ, const std::set<std::string>& selectedNodes);
    void setLookThrough(const std::string& camPath);
    const std::string& lookThrough() const { return _lookThrough; }
    void setLocked(bool l) { _locked = l; }
    void setDisplayOverride(int mode) { _displayOverride = mode; update(); } // -1 = as set on the nodes
    void setGrid(bool g) { _grid = g; update(); }
    // 0 perspective, 1 top, 2 bottom, 3 front, 4 back, 5 right, 6 left
    void setPreset(int p);
    int preset() const { return _preset; }
    void frameAll();
    void setTime(double t) { _time = t; }

    // manipulator (W move, E rotate, R scale, Q none) on the selected node
    struct Xform
    {
        Solv3D::Vec3 translate, rotate, scale = Solv3D::Vec3(1, 1, 1), pivot;
        double uniform = 1.;
    };
    void setGizmo(bool on, const Solv3D::Mat4& parentWorld, const Solv3D::Mat4& world, const Solv3D::Vec3& pivot);
    void setManipMode(int m) { _manipMode = m; update(); }
    int manipMode() const { return _manipMode; }
    std::function<bool(Xform* out)> onManipBegin;                      // current knob values
    std::function<void(int what, const Solv3D::Vec3& v, double uniform)> onManipSet; // 1 translate 2 rotate 3 scale 4 uniform
    std::function<void(int mode)> onManipModeChanged;
    std::function<void()> onManipEnd;                                    // drag finished (undo step)

    std::function<void(const std::string& node, bool add)> onPick;     // click on geometry
    std::function<void(int preset)> onPresetChanged;                     // keys changed the view
    std::function<void()> onLookThroughSelected;                         // H
    std::function<void()> onLeaveCamera;                                 // navigation left the camera view

protected:
    virtual void initializeGL() OVERRIDE FINAL;
    virtual void paintGL() OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual bool focusNextPrevChild(bool) OVERRIDE FINAL { return false; } // Tab reaches the viewer

private:
    void viewMatrices(Solv3D::Mat4* proj, Solv3D::Mat4* view) const;
    void drawScene(const Solv3D::Scene& s, const Solv3D::Mat4& V, bool templ);
    unsigned int textureFor(const std::shared_ptr<void>& handle); // GL texture id, 0 = not ready
    void pollTextures();
    std::string pick(const QPoint& p) const;
    void drawCameraIcon(bool sel, double ha, double va, double fl);
    void drawLightIcon(bool sel, const Solv3D::Light& L);
    void drawGizmo(const Solv3D::Mat4& P, const Solv3D::Mat4& V);
    // gizmo handle under the mouse: 1..3 axes, 4 centre, 0 none
    int gizmoHandle(const QPoint& p) const;
    bool screenPoint(const Solv3D::Vec3& w, double* x, double* y) const; // widget pixels, y down
    void mouseRay(const QPoint& p, Solv3D::Vec3* o, Solv3D::Vec3* d) const;
    Solv3D::Vec3 gizmoAxis(int a) const;
    double gizmoSize() const;
    Solv3D::Vec3 viewForward() const;

    Solv3D::Scene _scene, _templ;
    std::set<std::string> _selected;
    std::string _lookThrough;
    bool _locked = false;
    int _displayOverride = -1;
    bool _grid = true;
    int _preset = 0;
    double _time = 0.;
    // orbit camera (Maya default persp view: 1 unit = 1 cm)
    double _yaw = 45., _pitch = -27.9, _dist = 45.;
    Solv3D::Vec3 _target;
    int _dragMode = 0; // 1 tumble, 2 track, 3 dolly
    QPoint _last, _pressPos;
    bool _moved = false;

    // manipulator
    int _manipMode = 1;
    bool _gizmoOn = false;
    Solv3D::Mat4 _gParent, _gWorld;
    Solv3D::Vec3 _gPivot, _gOrigin;
    int _gHot = 0, _gDrag = 0;
    Xform _gStart;
    Solv3D::Vec3 _gAxisStart;   // axis-parameter / plane point at press
    double _gT0 = 0., _gAng0 = 0.;

    // texture previews (key: the image node)
    struct Tex
    {
        unsigned int id = 0;
        U64 hash = 0;
        double time = -1e30;
        bool pending = false;
    };
    std::map<const void*, Tex> _tex;
    std::shared_ptr<Solv3DTexJob> _job;
};

class Solv3DView
    : public QWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    Solv3DView(ViewerTab* viewer, Gui* gui);
    virtual ~Solv3DView();

    void setPreset(int p);
    std::function<void(int preset)> onPresetChanged;

public Q_SLOTS:
    void refresh();
    void onLookThroughChanged(int);
    void onDisplayChanged(int);
    void onGridToggled(bool);
    void onLockToggled(bool);
    void onFrameClicked();
    void onManipButton();

private:
    void lookThroughSelected();
    void pickNode(const std::string& name, bool add);
    NodePtr gizmoNode() const;
    std::string _gizmoNode;
    Solv3DGLWidget::Xform _manipStart;
    int _manipWhat = 0;

    ViewerTab* _viewer;
    Gui* _gui;
    Solv3DGLWidget* _gl;
    QComboBox* _view;
    QComboBox* _display;
    QToolButton* _lock;
    QToolButton* _gridBtn;
    QToolButton* _manipBtn[4];
    QTimer* _timer;
    std::vector<std::string> _cams;
};

NATRON_NAMESPACE_EXIT

#endif // SOLVFX_SOLV3DVIEWER_H
