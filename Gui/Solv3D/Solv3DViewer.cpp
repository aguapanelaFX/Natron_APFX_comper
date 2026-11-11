/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
#include <Python.h>

#include "Global/GLIncludes.h" // before Qt's OpenGL headers (glad)

#include "Solv3DViewer.h"

#include <algorithm>
#include <cmath>

#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/Solv3D/Solv3DNodes.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

using namespace Solv3D;

static const double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- GL widget
Solv3DGLWidget::Solv3DGLWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
}

void
Solv3DGLWidget::initializeGL()
{
    appPTR->initializeOpenGLFunctionsOnce();
}

void
Solv3DGLWidget::viewMatrices(Mat4* proj, Mat4* view) const
{
    const double aspect = (double)std::max(1, width()) / std::max(1, height());
    const Prim* cp = _lookThrough.empty() ? 0 : _scene.find(_lookThrough);
    if (cp && cp->camera) {
        *proj = cp->camera->projection(aspect);
        *view = _scene.worldMatrix(_lookThrough).inverse();

        return;
    }
    Camera c;
    c.focalLength = 30.;
    c.nearClip = 0.01;
    c.farClip = 100000.;
    *proj = c.projection(aspect);
    // orbit: camera at target + rotation * (0, 0, dist)
    const Mat4 rot = Mat4::rotateXYZ( Vec3(_pitch, _yaw, 0.) );
    const Mat4 rotYX = Mat4::rotateXYZ( Vec3(0, _yaw, 0) ) * Mat4::rotateXYZ( Vec3(_pitch, 0, 0) );
    (void)rot;
    const Mat4 camWorld = Mat4::translate(_target) * rotYX * Mat4::translate( Vec3(0, 0, _dist) );
    *view = camWorld.inverse();
}

void
Solv3DGLWidget::frameAll()
{
    Vec3 lo, hi;
    if ( _scene.bounds(&lo, &hi) ) {
        _target = (lo + hi) * 0.5;
        const Vec3 d = hi - lo;
        _dist = std::max( 0.5, std::sqrt( dot(d, d) ) * 1.4 );
    } else {
        _target = Vec3();
        _dist = 8.;
    }
    _lookThrough.clear();
    update();
}

static void
loadMat(GLenum mode, const Mat4& m)
{
    // our matrices are row-major column-vector: OpenGL wants column-major -> the transpose memory layout
    GLdouble a[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            a[c * 4 + r] = m.m[r][c];
        }
    }
    glMatrixMode(mode);
    glLoadMatrixd(a);
}

void
Solv3DGLWidget::paintGL()
{
    glClearColor(0.196f, 0.196f, 0.196f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport( 0, 0, (GLsizei)(width() * devicePixelRatio()), (GLsizei)(height() * devicePixelRatio()) );
    Mat4 P, V;
    viewMatrices(&P, &V);
    loadMat(GL_PROJECTION, P);
    loadMat(GL_MODELVIEW, V);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // grid on the ground plane (XZ)
    glDisable(GL_LIGHTING);
    glLineWidth(1.f);
    glBegin(GL_LINES);
    for (int i = -10; i <= 10; ++i) {
        const float a = (i == 0) ? 0.55f : 0.32f;
        glColor4f(a, a, a, 1.f);
        glVertex3d(i, 0, -10); glVertex3d(i, 0, 10);
        glVertex3d(-10, 0, i); glVertex3d(10, 0, i);
    }
    glEnd();
    // world axes
    glLineWidth(2.f);
    glBegin(GL_LINES);
    glColor3f(0.93f, 0.33f, 0.31f); glVertex3d(0, 0, 0); glVertex3d(1, 0, 0);
    glColor3f(0.4f, 0.85f, 0.42f); glVertex3d(0, 0, 0); glVertex3d(0, 1, 0);
    glColor3f(0.36f, 0.55f, 1.f); glVertex3d(0, 0, 0); glVertex3d(0, 0, 1);
    glEnd();

    // headlight
    const GLfloat lpos[4] = { 0.f, 0.f, 1.f, 0.f };
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glPopMatrix();
    const GLfloat amb[4] = { 0.25f, 0.25f, 0.25f, 1.f }, dif[4] = { 0.8f, 0.8f, 0.8f, 1.f };
    glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    for (const auto& kv : _scene.prims()) {
        const Prim& p = kv.second;
        if (!p.visible) {
            continue;
        }
        const Mat4 W = _scene.worldMatrix(kv.first);
        loadMat(GL_MODELVIEW, V * W);
        if ( (p.type == PrimType::Mesh) && p.mesh ) {
            const Mesh& m = *p.mesh;
            const bool hasN = m.normals.size() == m.faceVertexIndices.size();
            const float bc[3] = { (float)m.material.baseColor.x, (float)m.material.baseColor.y, (float)m.material.baseColor.z };
            const bool textured = (bool)m.texture || !m.projections.empty();
            if (_shading != 2) {
                glEnable(GL_LIGHTING);
                glEnable(GL_LIGHT0);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(1.f, 1.f);
                // textured surfaces show warm grey (textures are rendered by ScanlineRender)
                glColor4f(bc[0] * (textured ? 0.75f : 0.6f), bc[1] * (textured ? 0.7f : 0.6f), bc[2] * (textured ? 0.62f : 0.6f), 1.f);
                size_t base = 0;
                for (int n : m.faceVertexCounts) {
                    glBegin(GL_POLYGON);
                    for (int k = 0; k < n; ++k) {
                        if (hasN) {
                            const Vec3& nn = m.normals[base + k];
                            glNormal3d(nn.x, nn.y, nn.z);
                        }
                        const Vec3& v = m.points[m.faceVertexIndices[base + k]];
                        glVertex3d(v.x, v.y, v.z);
                    }
                    glEnd();
                    base += n;
                }
                glDisable(GL_POLYGON_OFFSET_FILL);
                glDisable(GL_LIGHTING);
            }
            if (_shading != 1) {
                glLineWidth(1.f);
                glColor4f(0.08f, 0.08f, 0.08f, _shading == 2 ? 1.f : 0.55f);
                if (_shading == 2) {
                    glColor4f(0.8f, 0.8f, 0.8f, 1.f);
                }
                size_t base = 0;
                for (int n : m.faceVertexCounts) {
                    glBegin(GL_LINE_LOOP);
                    for (int k = 0; k < n; ++k) {
                        const Vec3& v = m.points[m.faceVertexIndices[base + k]];
                        glVertex3d(v.x, v.y, v.z);
                    }
                    glEnd();
                    base += n;
                }
            }
        } else if ( (p.type == PrimType::Camera) && p.camera && (kv.first != _lookThrough) ) {
            // frustum icon
            const double ha = p.camera->horizontalAperture, va = p.camera->verticalAperture, fl = p.camera->focalLength;
            const double s = 0.6, w = s * ha / fl * 0.5, h = s * va / fl * 0.5;
            glLineWidth(1.5f);
            glColor3f(0.85f, 0.62f, 0.95f);
            glBegin(GL_LINES);
            const double c[4][2] = { { -w, -h }, { w, -h }, { w, h }, { -w, h } };
            for (int i = 0; i < 4; ++i) {
                glVertex3d(0, 0, 0); glVertex3d(c[i][0], c[i][1], -s);
                glVertex3d(c[i][0], c[i][1], -s); glVertex3d(c[(i + 1) % 4][0], c[(i + 1) % 4][1], -s);
            }
            // up marker
            glVertex3d(-w * 0.4, h * 1.1, -s); glVertex3d(0, h * 1.5, -s);
            glVertex3d(0, h * 1.5, -s); glVertex3d(w * 0.4, h * 1.1, -s);
            glEnd();
        }
    }
    glDisable(GL_DEPTH_TEST);
}

void
Solv3DGLWidget::mousePressEvent(QMouseEvent* e)
{
    _last = e->pos();
    _dragMode = 0;
    const bool alt = e->modifiers() & Qt::AltModifier;
    if (alt && (e->button() == Qt::LeftButton)) {
        _dragMode = 1;
    } else if ( (alt && (e->button() == Qt::MiddleButton)) || (e->button() == Qt::MiddleButton) ) {
        _dragMode = 2;
    } else if (alt && (e->button() == Qt::RightButton)) {
        _dragMode = 3;
    }
    if (_dragMode && !_lookThrough.empty()) {
        _lookThrough.clear(); // navigating leaves the camera view (like Maya)
    }
    setFocus();
}

void
Solv3DGLWidget::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint d = e->pos() - _last;
    _last = e->pos();
    switch (_dragMode) {
    case 1:
        _yaw -= d.x() * 0.4;
        _pitch = std::max( -89., std::min(89., _pitch - d.y() * 0.4) );
        break;
    case 2: {
        const double k = _dist * 0.0015;
        const Mat4 r = Mat4::rotateXYZ( Vec3(0, _yaw, 0) ) * Mat4::rotateXYZ( Vec3(_pitch, 0, 0) );
        _target = _target + r.vector( Vec3(-d.x() * k, d.y() * k, 0) );
        break;
    }
    case 3:
        _dist = std::max( 0.05, _dist * std::pow(1.006, d.x() - d.y()) );
        break;
    default:
        return;
    }
    update();
}

void
Solv3DGLWidget::mouseReleaseEvent(QMouseEvent*)
{
    _dragMode = 0;
}

void
Solv3DGLWidget::wheelEvent(QWheelEvent* e)
{
    _dist = std::max( 0.05, _dist * std::pow(0.999, e->angleDelta().y()) );
    _lookThrough.clear();
    update();
}

void
Solv3DGLWidget::keyPressEvent(QKeyEvent* e)
{
    if ( (e->key() == Qt::Key_F) || (e->key() == Qt::Key_A) ) {
        frameAll();

        return;
    }
    QOpenGLWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------- panel
Solv3DViewerPanel::Solv3DViewerPanel(Gui* gui)
    : QWidget(gui)
    , PanelWidget(this, gui)
{
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    QWidget* bar = new QWidget(this);
    QHBoxLayout* h = new QHBoxLayout(bar);
    h->setContentsMargins(6, 3, 6, 3);
    h->setSpacing(6);
    _nodeLabel = new QLabel(tr("select a 3D node"), bar);
    h->addWidget(_nodeLabel);
    _lock = new QToolButton(bar);
    _lock->setText( tr("lock") );
    _lock->setCheckable(true);
    _lock->setToolTip( tr("Keep showing this node when the selection changes.") );
    h->addWidget(_lock);
    h->addStretch(1);
    _view = new QComboBox(bar);
    _view->addItem( tr("perspective") );
    _view->setToolTip( tr("Look through a camera of the scene.") );
    h->addWidget(_view);
    _shading = new QComboBox(bar);
    _shading->addItem( tr("shaded + wire") );
    _shading->addItem( tr("shaded") );
    _shading->addItem( tr("wireframe") );
    h->addWidget(_shading);
    QToolButton* frame = new QToolButton(bar);
    frame->setText( tr("frame (F)") );
    h->addWidget(frame);
    v->addWidget(bar);
    _gl = new Solv3DGLWidget(this);
    v->addWidget(_gl, 1);
    QObject::connect( _view, SIGNAL(currentIndexChanged(int)), this, SLOT(onLookThroughChanged(int)) );
    QObject::connect( _shading, SIGNAL(currentIndexChanged(int)), this, SLOT(onShadingChanged(int)) );
    QObject::connect( frame, SIGNAL(clicked()), this, SLOT(onFrameClicked()) );
    _timer = new QTimer(this);
    _timer->setInterval(120);
    QObject::connect( _timer, SIGNAL(timeout()), this, SLOT(refresh()) );
    _timer->start();
}

Solv3DViewerPanel::~Solv3DViewerPanel()
{
}

EffectInstancePtr
Solv3DViewerPanel::currentNode()
{
    if ( _lock->isChecked() ) {
        return _node.lock();
    }
    Gui* gui = getGui();
    if (!gui) {
        return EffectInstancePtr();
    }
    const NodesGuiList& sel = gui->getSelectedNodes();
    for (NodesGuiList::const_reverse_iterator it = sel.rbegin(); it != sel.rend(); ++it) {
        NodePtr n = (*it) ? (*it)->getNode() : NodePtr();
        EffectInstancePtr e = n ? n->getEffectInstance() : EffectInstancePtr();
        if ( e && Solv3DNode::as3D(e) ) {
            _node = e;

            return e;
        }
    }

    return _node.lock(); // keep the last one
}

void
Solv3DViewerPanel::refresh()
{
    if ( !isVisible() || !getGui() || !getGui()->getApp() ) {
        return;
    }
    EffectInstancePtr e = currentNode();
    Solv3DNode* n = e ? Solv3DNode::as3D(e) : 0;
    Scene s;
    if (n) {
        const double t = getGui()->getApp()->getTimeLine()->currentFrame();
        n->buildScene(t, &s);
        _nodeLabel->setText( QString::fromUtf8( e->getNode()->getLabel_mt_safe().c_str() ) );
    }
    _gl->setScene(s);
    // camera list
    std::vector<std::string> cams = s.pathsOfType(PrimType::Camera);
    if (cams != _cams) {
        _cams = cams;
        const QString cur = _view->currentText();
        _view->blockSignals(true);
        _view->clear();
        _view->addItem( tr("perspective") );
        for (const std::string& c : cams) {
            _view->addItem( QString::fromUtf8( c.c_str() ) );
        }
        const int idx = _view->findText(cur);
        _view->setCurrentIndex(idx >= 0 ? idx : 0);
        _view->blockSignals(false);
    }
}

void
Solv3DViewerPanel::onLookThroughChanged(int i)
{
    _gl->setLookThrough( i <= 0 ? std::string() : _view->itemText(i).toStdString() );
}

void
Solv3DViewerPanel::onShadingChanged(int i)
{
    _gl->setShading(i);
}

void
Solv3DViewerPanel::onFrameClicked()
{
    _gl->frameAll();
    _view->setCurrentIndex(0);
}

void
Solv3DViewerPanel::enterEvent(QEvent* e)
{
    enterEventBase();
    QWidget::enterEvent(e);
}

void
Solv3DViewerPanel::leaveEvent(QEvent* e)
{
    leaveEventBase();
    QWidget::leaveEvent(e);
}

void
Solv3DViewerPanel::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_F) {
        onFrameClicked();

        return;
    }
    handleUnCaughtKeyPressEvent(e);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_Solv3DViewer.cpp"
