/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX 3D Viewer: a dockable tab that draws the 3D scene of the selected 3D node (or of the
// ScanlineRender selected). Maya navigation: Alt+LMB tumble, Alt+MMB track, Alt+RMB dolly, wheel zoom,
// F frames the scene. Look through any camera of the scene.

#ifndef SOLVFX_SOLV3DVIEWER_H
#define SOLVFX_SOLV3DVIEWER_H

#include "Global/Macros.h"

#include <memory>

#include <QOpenGLWidget>
#include <QWidget>

#include "Engine/EngineFwd.h"
#include "Engine/Solv3D/SolvScene.h"
#include "Gui/GuiFwd.h"
#include "Gui/PanelWidget.h"

class QComboBox;
class QLabel;
class QToolButton;
class QTimer;

NATRON_NAMESPACE_ENTER

class Solv3DGLWidget
    : public QOpenGLWidget
{
public:
    explicit Solv3DGLWidget(QWidget* parent);

    void setScene(const Solv3D::Scene& s) { _scene = s; update(); }
    void setLookThrough(const std::string& camPath) { _lookThrough = camPath; update(); }
    void setShading(int mode) { _shading = mode; update(); }
    void frameAll();
    const Solv3D::Scene& scene() const { return _scene; }

protected:
    virtual void initializeGL() OVERRIDE FINAL;
    virtual void paintGL() OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;

private:
    void viewMatrices(Solv3D::Mat4* proj, Solv3D::Mat4* view) const;

    Solv3D::Scene _scene;
    std::string _lookThrough;
    int _shading = 0; // 0 shaded + wire, 1 shaded, 2 wireframe
    // orbit camera
    double _yaw = 30., _pitch = -20., _dist = 8.;
    Solv3D::Vec3 _target;
    int _dragMode = 0; // 1 tumble, 2 track, 3 dolly
    QPoint _last;
};

class Solv3DViewerPanel
    : public QWidget, public PanelWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit Solv3DViewerPanel(Gui* gui);
    virtual ~Solv3DViewerPanel();

public Q_SLOTS:
    void refresh();
    void onLookThroughChanged(int);
    void onShadingChanged(int);
    void onFrameClicked();

protected:
    virtual void enterEvent(QEvent* e) OVERRIDE FINAL;
    virtual void leaveEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;

private:
    EffectInstancePtr currentNode();

    Solv3DGLWidget* _gl;
    QLabel* _nodeLabel;
    QToolButton* _lock;
    QComboBox* _view;
    QComboBox* _shading;
    QTimer* _timer;
    EffectInstanceWPtr _node;
    std::vector<std::string> _cams;
};

NATRON_NAMESPACE_EXIT

#endif // SOLVFX_SOLV3DVIEWER_H
