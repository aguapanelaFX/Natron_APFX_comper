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
class QPainter;
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
    bool texturePending() const { return (bool)_job; } // a preview render is running: repaint when it lands

    // manipulator (Q select, W move, E rotate, R scale; Insert / hold D pivot edit) on the selected node
    struct Xform
    {
        Solv3D::Vec3 translate, rotate, scale = Solv3D::Vec3(1, 1, 1), pivot;
        double uniform = 1.;
        int rotOrder = 0;   // the node's rotation order (0 XYZ .. 5 ZYX)
    };
    void setGizmo(bool on, const Solv3D::Mat4& parentWorld, const Solv3D::Mat4& world, const Solv3D::Vec3& pivot,
                  const std::string& node = std::string());
    void setManipMode(int m);
    int manipMode() const { return _manipMode; }
    void cycleSpace();                       // local -> global -> view
    int space() const { return _space; }
    void frameSelected();
    std::function<bool(Xform* out)> onManipBegin;                      // current knob values
    // 1 translate 2 rotate 3 scale 4 uniform 5 pivot
    std::function<void(int what, const Solv3D::Vec3& v, double uniform)> onManipSet;
    std::function<void(int mode)> onManipModeChanged;
    std::function<void(int space)> onSpaceChanged;
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
    virtual void keyReleaseEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual void focusOutEvent(QFocusEvent* e) OVERRIDE FINAL;
    virtual void enterEvent(QEvent* e) OVERRIDE FINAL;
    virtual bool focusNextPrevChild(bool) OVERRIDE FINAL { return false; } // Tab reaches the viewer

private:
    void viewMatrices(Solv3D::Mat4* proj, Solv3D::Mat4* view) const;
    void drawScene(const Solv3D::Scene& s, const Solv3D::Mat4& V, bool templ);
    unsigned int textureFor(const std::shared_ptr<void>& handle); // GL texture id, 0 = not ready
    void pollTextures();
    std::string pick(const QPoint& p) const;
    void drawCameraIcon(bool sel, double ha, double va, double fl);
    void drawLightIcon(bool sel, const Solv3D::Light& L);
    bool screenPoint(const Solv3D::Vec3& w, double* x, double* y) const; // widget pixels, y down
    void mouseRay(const QPoint& p, Solv3D::Vec3* o, Solv3D::Vec3* d) const;

    // manipulator internals (see the manipulator spec)
    enum Part { ePartNone = -1, ePartX = 0, ePartY, ePartZ, ePartPlaneX, ePartPlaneY, ePartPlaneZ, ePartCenter, ePartFree };
    int tool() const { return _pivotMode ? 4 : _manipMode; } // 4 = pivot
    bool gizmoVisible() const { return _gizmoOn && ( (_manipMode != 0) || _pivotMode ); }
    void gizmoAxes(Solv3D::Vec3 ax[3]) const;                 // unit world axes for the current tool / space
    double gizmoLength() const;                                // L (world units, constant on screen)
    Solv3D::Vec3 eyePos() const;
    int hitPart(const QPoint& p) const;
    void drawGizmo2D(QPainter& p);
    void drawHud(QPainter& p);
    void beginManip(int part, const QPoint& pos);
    void dragManip(const QPoint& pos);
    void endManip();
    void buildSnapData();
    bool snapTarget(const QPoint& mouse, Solv3D::Vec3* target) const;
    std::string snapLabel() const;
    bool held(int key) const { return _held.count(key) != 0; }
    void centrePivot();

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
    int _manipMode = 1, _lastTool = 1;
    int _space = 0;                  // 0 local, 1 global, 2 view
    bool _pivotMode = false, _pivotByD = false;
    double _handleScale = 1.;
    int _activePart[5] = { ePartCenter, ePartCenter, ePartFree, ePartCenter, ePartCenter }; // per tool (4 = pivot)
    std::set<int> _held;             // keys held down
    bool _snapGrid = false;          // grid snap always on (right-click menu)
    bool _gizmoOn = false;
    std::string _gNode;
    Solv3D::Mat4 _gParent, _gWorld;
    Solv3D::Vec3 _gPivot, _gOrigin;
    int _gHot = ePartNone, _gDrag = ePartNone;
    bool _gMiddle = false;           // dragging with the middle button (active part)
    // drag start
    Xform _gStart;
    QPoint _gMouse0;
    Solv3D::Mat4 _gParent0, _gWorld0;
    Solv3D::Vec3 _gC0, _gAx0[3], _gHit0;
    double _gL0 = 1., _gSx0 = 0., _gSy0 = 0., _gSegX = 0., _gSegY = 0.;
    Solv3D::Vec3 _gLiveAx[3];        // rotate: axes following the drag
    bool _gLive = false;
    std::map<int, QPoint> _keyDown;  // view keys: switch the view on release when not used for snapping
    bool _keyUsed = false;
    // snapping data (world, built at drag start)
    std::vector<Solv3D::Vec3> _snapPts, _snapFaces;
    std::vector<std::pair<Solv3D::Vec3, Solv3D::Vec3> > _snapEdges;
    std::vector<double> _snapPtsScr, _snapFacesScr, _snapEdgesScr; // screen x, y pairs

    // texture previews (key: the image node)
    struct Tex
    {
        unsigned int id = 0;
        U64 hash = 0;
        double time = -1e30;
        bool pending = false;
    };
    std::map<const void*, Tex> _tex;

    // GPU mesh cache: triangulated vertex buffers keyed by mesh content (survives scene rebuilds)
    struct GpuMesh
    {
        unsigned int vbo = 0, lineVbo = 0;
        int triVerts = 0, lineVerts = 0;
        bool hasN = false, hasUV = false;
        int lastUse = 0;
    };
    std::map<U64, GpuMesh> _gpu;
    std::map<const Solv3D::Mesh*, U64> _meshKey; // mesh of the current scenes -> content key
    int _frameNo = 0;
    U64 meshKey(const Solv3D::Mesh* m);
    GpuMesh* gpuMesh(const Solv3D::Mesh& m);
    void indexMeshes(const Solv3D::Scene& s, std::map<const Solv3D::Mesh*, U64>* out);
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
    QToolButton* _spaceBtn;
    QTimer* _timer;
    std::vector<std::string> _cams;
    U64 _lastSig = 0;   // scene inputs at the last rebuild: unchanged = no rebuild, no repaint
    int _idleTicks = 0;
};

NATRON_NAMESPACE_EXIT

#endif // SOLVFX_SOLV3DVIEWER_H
