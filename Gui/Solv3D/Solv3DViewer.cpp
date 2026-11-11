/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
#include <Python.h>

#include "Global/GLIncludes.h" // before Qt's OpenGL headers (glad)

#include "Solv3DViewer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

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
#include "Engine/ViewerInstance.h"
#include "Engine/Solv3D/Solv3DNodes.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

using namespace Solv3D;

// background preview render of a texture node (Node::makePreviewImage, 8-bit display colours)
struct Solv3DTexJob
{
    NodePtr node;
    const void* key = 0;
    double time = 0.;
    U64 hash = 0;
    int w = 512, h = 512;
    std::vector<unsigned int> buf;
    bool ok = false;
    std::atomic<bool> done { false };
};

static const float kSelCol[3] = { 0.80f, 0.70f, 0.32f }; // dim yellow

// ---------------------------------------------------------------- GL widget
Solv3DGLWidget::Solv3DGLWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
}

Solv3DGLWidget::~Solv3DGLWidget()
{
    if ( context() ) {
        makeCurrent();
        for (std::map<const void*, Tex>::iterator it = _tex.begin(); it != _tex.end(); ++it) {
            if (it->second.id) {
                GLuint id = it->second.id;
                glDeleteTextures(1, &id);
            }
        }
        doneCurrent();
    }
}

void
Solv3DGLWidget::initializeGL()
{
    appPTR->initializeOpenGLFunctionsOnce();
}

void
Solv3DGLWidget::setScenes(const Scene& main, const Scene& templ, const std::set<std::string>& selectedNodes)
{
    _scene = main;
    _templ = templ;
    _selected = selectedNodes;
    if ( !_lookThrough.empty() && !_scene.find(_lookThrough) && !_templ.find(_lookThrough) ) {
        _lookThrough.clear();
    }
    update();
}

void
Solv3DGLWidget::setLookThrough(const std::string& camPath)
{
    _lookThrough = camPath;
    update();
}

void
Solv3DGLWidget::setPreset(int p)
{
    _preset = std::max(0, std::min(6, p) );
    switch (_preset) {
    case 1: _yaw = 0.; _pitch = -90.; break;   // top
    case 2: _yaw = 0.; _pitch = 90.; break;    // bottom
    case 3: _yaw = 0.; _pitch = 0.; break;     // front
    case 4: _yaw = 180.; _pitch = 0.; break;   // back
    case 5: _yaw = 90.; _pitch = 0.; break;    // right
    case 6: _yaw = -90.; _pitch = 0.; break;   // left
    default: break;                            // perspective keeps the current angles
    }
    if (_preset != 0) {
        _lookThrough.clear();
    }
    update();
}

void
Solv3DGLWidget::viewMatrices(Mat4* proj, Mat4* view) const
{
    const double aspect = (double)std::max(1, width()) / std::max(1, height());
    const Scene* src = _scene.find(_lookThrough) ? &_scene : &_templ;
    const Prim* cp = _lookThrough.empty() ? 0 : src->find(_lookThrough);
    if (cp && cp->camera) {
        *proj = cp->camera->projection(aspect);
        *view = src->worldMatrix(_lookThrough).inverse();

        return;
    }
    Camera c;
    const Mat4 rotYX = Mat4::rotateXYZ( Vec3(0, _yaw, 0) ) * Mat4::rotateXYZ( Vec3(_pitch, 0, 0) );
    double camDist = _dist;
    if (_preset == 0) {
        c.focalLength = 30.;
        c.nearClip = std::max(0.001, _dist * 0.002);
        c.farClip = 100000.;
    } else {
        // orthographic side views: the visible height follows the zoom
        const double h = _dist * 0.8;
        c.projectionMode = 1;
        c.horizontalAperture = h * aspect * 10.; // tenths of a unit
        camDist = 5000.;
        c.nearClip = 0.1;
        c.farClip = 20000.;
    }
    *proj = c.projection(aspect);
    const Mat4 camWorld = Mat4::translate(_target) * rotYX * Mat4::translate( Vec3(0, 0, camDist) );
    *view = camWorld.inverse();
}

void
Solv3DGLWidget::frameAll()
{
    Vec3 lo, hi;
    Scene all = _scene;
    all.merge(_templ);
    if ( all.bounds(&lo, &hi) ) {
        _target = (lo + hi) * 0.5;
        const Vec3 d = hi - lo;
        _dist = std::max( 0.5, std::sqrt( dot(d, d) ) * 1.4 );
    } else {
        _target = Vec3();
        _dist = 8.;
    }
    if (!_locked) {
        _lookThrough.clear();
    }
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
Solv3DGLWidget::pollTextures()
{
    if ( !_job || !_job->done.load() ) {
        return;
    }
    std::shared_ptr<Solv3DTexJob> j = _job;
    _job.reset();
    Tex& t = _tex[j->key];
    t.pending = false;
    t.hash = j->hash;
    t.time = j->time;
    if ( !j->ok || (j->w <= 0) || (j->h <= 0) ) {
        return;
    }
    // preview rows are top to bottom: flip so that v = 0 is the bottom of the image
    std::vector<unsigned int> flipped( (size_t)j->w * j->h );
    for (int y = 0; y < j->h; ++y) {
        std::copy( j->buf.begin() + (size_t)(j->h - 1 - y) * j->w, j->buf.begin() + (size_t)(j->h - y) * j->w, flipped.begin() + (size_t)y * j->w );
    }
    if (!t.id) {
        GLuint id = 0;
        glGenTextures(1, &id);
        t.id = id;
    }
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, j->w, j->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, flipped.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

unsigned int
Solv3DGLWidget::textureFor(const std::shared_ptr<void>& handle)
{
    Solv3DTexRef* ref = static_cast<Solv3DTexRef*>( handle.get() );
    EffectInstancePtr e = ref ? ref->effect.lock() : EffectInstancePtr();
    NodePtr n = e ? e->getNode() : NodePtr();
    if (!n) {
        return 0;
    }
    const void* key = e.get();
    Tex& t = _tex[key];
    const U64 hash = n->getHashValue();
    if ( ( (t.hash != hash) || (t.time != _time) ) && !t.pending && !_job ) {
        // one preview render at a time, in the background
        std::shared_ptr<Solv3DTexJob> j = std::make_shared<Solv3DTexJob>();
        j->node = n;
        j->key = key;
        j->time = _time;
        j->hash = hash;
        j->buf.assign( (size_t)j->w * j->h, 0u );
        t.pending = true;
        _job = j;
        std::thread( [j]() {
            int w = j->w, h = j->h;
            try {
                j->ok = j->node->makePreviewImage( (SequenceTime)j->time, &w, &h, j->buf.data() );
            } catch (...) {
                j->ok = false;
            }
            j->w = w;
            j->h = h;
            j->done = true;
        } ).detach();
    }

    return t.id;
}

// is any parent (or the prim) hidden
static bool
primHidden(const Scene& s, const std::string& path)
{
    for (std::string p = path; !p.empty(); p = parentPath(p)) {
        const Prim* pp = s.find(p);
        if (pp && !pp->visible) {
            return true;
        }
    }

    return false;
}

void
Solv3DGLWidget::drawScene(const Scene& s, const Mat4& V, bool templ)
{
    for (const auto& kv : s.prims()) {
        const Prim& p = kv.second;
        if ( primHidden(s, kv.first) ) {
            continue;
        }
        const int mode = (_displayOverride >= 0 && p.display != 0) ? _displayOverride : p.display;
        if (mode == 0) {
            continue;
        }
        const bool sel = templ || ( !p.node.empty() && _selected.count(p.node) );
        const Mat4 W = s.worldMatrix(kv.first);
        loadMat(GL_MODELVIEW, V * W);
        if ( (p.type == PrimType::Mesh) && p.mesh ) {
            const Mesh& m = *p.mesh;
            const bool hasN = m.normals.size() == m.faceVertexIndices.size();
            const bool hasUV = m.st.size() == m.faceVertexIndices.size();
            const float bc[3] = { (float)m.material.baseColor.x, (float)m.material.baseColor.y, (float)m.material.baseColor.z };
            const float alpha = templ ? 0.35f : 1.f;
            if ( (mode >= 2) ) {
                // texture: the mesh's own (needs uv) or the last projection (uv from the projector, per vertex)
                unsigned int tex = 0;
                const Projection* proj = 0;
                if (mode >= 4) {
                    if (m.texture && hasUV) {
                        tex = textureFor(m.texture);
                    }
                    if ( !tex && !m.projections.empty() ) {
                        proj = &m.projections.back();
                        tex = textureFor(proj->texture);
                        if (!tex) {
                            proj = 0;
                        }
                    }
                }
                const bool lit = m.material.emission < 0.5;
                if (lit) {
                    glEnable(GL_LIGHTING);
                    glEnable(GL_LIGHT0);
                }
                if (tex) {
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                    glColor4f(bc[0], bc[1], bc[2], alpha);
                } else {
                    glColor4f(bc[0] * 0.6f, bc[1] * 0.6f, bc[2] * 0.6f, alpha);
                }
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(1.f, 1.f);
                if (templ) {
                    glDepthMask(GL_FALSE);
                }
                size_t base = 0;
                for (int n : m.faceVertexCounts) {
                    glBegin(GL_POLYGON);
                    for (int k = 0; k < n; ++k) {
                        if (hasN) {
                            const Vec3& nn = m.normals[base + k];
                            glNormal3d(nn.x, nn.y, nn.z);
                        }
                        const Vec3& v = m.points[m.faceVertexIndices[base + k]];
                        if (tex && proj) {
                            const Vec3 wp = W.point(v);
                            double c[4];
                            for (int i = 0; i < 4; ++i) {
                                c[i] = proj->viewProj.m[i][0] * wp.x + proj->viewProj.m[i][1] * wp.y + proj->viewProj.m[i][2] * wp.z + proj->viewProj.m[i][3];
                            }
                            const double iw = std::fabs(c[3]) > 1e-9 ? 1. / c[3] : 0.;
                            glTexCoord2d(c[0] * iw * 0.5 + 0.5, c[1] * iw * 0.5 + 0.5);
                        } else if (tex) {
                            const Vec2& uv = m.st[base + k];
                            glTexCoord2d(uv.x, uv.y);
                        }
                        glVertex3d(v.x, v.y, v.z);
                    }
                    glEnd();
                    base += n;
                }
                if (templ) {
                    glDepthMask(GL_TRUE);
                }
                glDisable(GL_POLYGON_OFFSET_FILL);
                if (tex) {
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glDisable(GL_TEXTURE_2D);
                }
                glDisable(GL_LIGHTING);
            }
            const bool lines = (mode == 1) || (mode == 3) || (mode == 5) || sel;
            if (lines) {
                if (sel) {
                    glLineWidth(1.5f);
                    glColor4f(kSelCol[0], kSelCol[1], kSelCol[2], 0.9f);
                } else if (mode == 1) {
                    glLineWidth(1.f);
                    glColor4f(0.78f, 0.78f, 0.78f, 1.f);
                } else {
                    glLineWidth(1.f);
                    glColor4f(0.08f, 0.08f, 0.08f, 0.55f);
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
            const double ha = p.camera->horizontalAperture, va = p.camera->verticalAperture, fl = std::max(1e-6, p.camera->focalLength);
            const double sz = 0.6, w = sz * ha / fl * 0.5, h = sz * va / fl * 0.5;
            glLineWidth(sel ? 2.f : 1.5f);
            if (sel) {
                glColor4f(kSelCol[0], kSelCol[1], kSelCol[2], 1.f);
            } else {
                glColor4f(0.85f, 0.62f, 0.95f, 1.f);
            }
            glBegin(GL_LINES);
            const double c[4][2] = { { -w, -h }, { w, -h }, { w, h }, { -w, h } };
            for (int i = 0; i < 4; ++i) {
                glVertex3d(0, 0, 0); glVertex3d(c[i][0], c[i][1], -sz);
                glVertex3d(c[i][0], c[i][1], -sz); glVertex3d(c[(i + 1) % 4][0], c[(i + 1) % 4][1], -sz);
            }
            // up marker
            glVertex3d(-w * 0.4, h * 1.1, -sz); glVertex3d(0, h * 1.5, -sz);
            glVertex3d(0, h * 1.5, -sz); glVertex3d(w * 0.4, h * 1.1, -sz);
            glEnd();
        }
    }
}

void
Solv3DGLWidget::paintGL()
{
    pollTextures();
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

    if (_grid) {
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
    }

    // headlight
    const GLfloat lpos[4] = { 0.f, 0.f, 1.f, 0.f };
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glPopMatrix();
    const GLfloat amb[4] = { 0.3f, 0.3f, 0.3f, 1.f }, dif[4] = { 0.8f, 0.8f, 0.8f, 1.f };
    glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    drawScene(_scene, V, false);
    drawScene(_templ, V, true);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
}

std::string
Solv3DGLWidget::pick(const QPoint& pt) const
{
    Mat4 P, V;
    viewMatrices(&P, &V);
    const double W = std::max(1, width()), H = std::max(1, height());
    const double px = pt.x(), py = H - pt.y();
    std::string best;
    double bestZ = std::numeric_limits<double>::max();
    const Scene* scenes[2] = { &_scene, &_templ };
    for (int si = 0; si < 2; ++si) {
        const Scene& s = *scenes[si];
        for (const auto& kv : s.prims()) {
            const Prim& p = kv.second;
            if ( !p.selectable || p.node.empty() || (p.display == 0) || primHidden(s, kv.first) ) {
                continue;
            }
            const Mat4 M = P * V * s.worldMatrix(kv.first);
            auto toScreen = [&](const Vec3& v, double* sx, double* sy, double* sz) -> bool {
                double c[4];
                for (int i = 0; i < 4; ++i) {
                    c[i] = M.m[i][0] * v.x + M.m[i][1] * v.y + M.m[i][2] * v.z + M.m[i][3];
                }
                if (c[3] <= 1e-9) {
                    return false;
                }
                *sx = (c[0] / c[3] * 0.5 + 0.5) * W;
                *sy = (c[1] / c[3] * 0.5 + 0.5) * H;
                *sz = c[2] / c[3];

                return true;
            };
            if ( (p.type == PrimType::Camera) && p.camera ) {
                double sx, sy, sz;
                if ( toScreen(Vec3(), &sx, &sy, &sz) && (std::hypot(sx - px, sy - py) < 12.) && (sz < bestZ) ) {
                    bestZ = sz;
                    best = p.node;
                }
                continue;
            }
            if ( (p.type != PrimType::Mesh) || !p.mesh ) {
                continue;
            }
            std::vector<std::array<int, 3> > tris, fv;
            p.mesh->triangulate(&tris, &fv);
            for (const std::array<int, 3>& t : tris) {
                double x[3], y[3], z[3];
                bool ok = true;
                for (int k = 0; k < 3 && ok; ++k) {
                    ok = toScreen(p.mesh->points[t[k]], &x[k], &y[k], &z[k]);
                }
                if (!ok) {
                    continue;
                }
                const double area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
                if (std::fabs(area) < 1e-12) {
                    continue;
                }
                const double w0 = ( (x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py) ) / area;
                const double w1 = ( (x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py) ) / area;
                const double w2 = 1. - w0 - w1;
                if ( (w0 < 0) || (w1 < 0) || (w2 < 0) ) {
                    continue;
                }
                const double zz = w0 * z[0] + w1 * z[1] + w2 * z[2];
                if ( (zz >= -1.) && (zz < bestZ) ) {
                    bestZ = zz;
                    best = p.node;
                }
            }
        }
    }

    return best;
}

void
Solv3DGLWidget::mousePressEvent(QMouseEvent* e)
{
    _last = e->pos();
    _pressPos = e->pos();
    _moved = false;
    _dragMode = 0;
    const bool alt = e->modifiers() & Qt::AltModifier;
    if (alt && (e->button() == Qt::LeftButton)) {
        _dragMode = 1;
    } else if ( (alt && (e->button() == Qt::MiddleButton)) || (e->button() == Qt::MiddleButton) ) {
        _dragMode = 2;
    } else if (alt && (e->button() == Qt::RightButton)) {
        _dragMode = 3;
    }
    if ( _dragMode && _locked && !_lookThrough.empty() ) {
        _dragMode = 0; // locked to the camera
    }
    setFocus();
}

void
Solv3DGLWidget::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint d = e->pos() - _last;
    _last = e->pos();
    if ( (e->pos() - _pressPos).manhattanLength() > 3 ) {
        _moved = true;
    }
    if (!_dragMode) {
        return;
    }
    if ( !_lookThrough.empty() ) {
        // navigating leaves the camera view (like Maya), starting from where the camera was
        _lookThrough.clear();
        if (onLeaveCamera) {
            onLeaveCamera();
        }
    }
    switch (_dragMode) {
    case 1:
        _yaw -= d.x() * 0.4;
        _pitch = std::max( -90., std::min(90., _pitch - d.y() * 0.4) );
        if (_preset != 0) {
            _preset = 0;
            if (onPresetChanged) {
                onPresetChanged(0);
            }
        }
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
Solv3DGLWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if ( !_dragMode && !_moved && (e->button() == Qt::LeftButton) && onPick ) {
        onPick( pick( e->pos() ), e->modifiers() & Qt::ShiftModifier );
    }
    _dragMode = 0;
}

void
Solv3DGLWidget::wheelEvent(QWheelEvent* e)
{
    if ( _locked && !_lookThrough.empty() ) {
        return;
    }
    _dist = std::max( 0.05, _dist * std::pow(0.999, e->angleDelta().y()) );
    if ( !_lookThrough.empty() ) {
        _lookThrough.clear();
        if (onLeaveCamera) {
            onLeaveCamera();
        }
    }
    update();
}

void
Solv3DGLWidget::keyPressEvent(QKeyEvent* e)
{
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    int preset = -1;
    switch ( e->key() ) {
    case Qt::Key_F:
    case Qt::Key_A:
        frameAll();

        return;
    case Qt::Key_X: preset = shift ? 6 : 5; break;
    case Qt::Key_C: preset = shift ? 2 : 1; break;
    case Qt::Key_Z: preset = shift ? 4 : 3; break;
    case Qt::Key_V: preset = 0; break;
    case Qt::Key_H:
        if (onLookThroughSelected) {
            onLookThroughSelected();
        }

        return;
    default:
        QOpenGLWidget::keyPressEvent(e);

        return;
    }
    setPreset(preset);
    if (onPresetChanged) {
        onPresetChanged(preset);
    }
}

// ---------------------------------------------------------------- view (bar + GL)
Solv3DView::Solv3DView(ViewerTab* viewer, Gui* gui)
    : QWidget(viewer)
    , _viewer(viewer)
    , _gui(gui)
{
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    QWidget* bar = new QWidget(this);
    QHBoxLayout* h = new QHBoxLayout(bar);
    h->setContentsMargins(4, 2, 4, 2);
    h->setSpacing(4);
    _view = new QComboBox(bar);
    _view->addItem( tr("default") );
    _view->setToolTip( tr("Look through a camera of the scene (H looks through the selected camera).") );
    _view->setMinimumContentsLength(14);
    _view->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    h->addWidget(_view);
    _lock = new QToolButton(bar);
    _lock->setText( tr("lock") );
    _lock->setCheckable(true);
    _lock->setToolTip( tr("Lock the view to the camera: navigation does not leave it.") );
    h->addWidget(_lock);
    h->addStretch(1);
    _display = new QComboBox(bar);
    const char* modes[7] = { "as set", "wireframe", "solid", "solid+lines", "textured", "textured+lines", 0 };
    for (int i = 0; modes[i]; ++i) {
        _display->addItem( QString::fromUtf8(modes[i]) );
    }
    _display->setToolTip( tr("How geometry is drawn. 'as set' uses the display setting of each node.") );
    h->addWidget(_display);
    _gridBtn = new QToolButton(bar);
    _gridBtn->setText( tr("grid") );
    _gridBtn->setCheckable(true);
    _gridBtn->setChecked(true);
    _gridBtn->setToolTip( tr("Ground grid and world axes.") );
    h->addWidget(_gridBtn);
    QToolButton* frame = new QToolButton(bar);
    frame->setText( tr("frame") );
    frame->setToolTip( tr("Frame everything (F).") );
    h->addWidget(frame);
    v->addWidget(bar);
    _gl = new Solv3DGLWidget(this);
    v->addWidget(_gl, 1);
    QObject::connect( _view, SIGNAL(currentIndexChanged(int)), this, SLOT(onLookThroughChanged(int)) );
    QObject::connect( _display, SIGNAL(currentIndexChanged(int)), this, SLOT(onDisplayChanged(int)) );
    QObject::connect( _gridBtn, SIGNAL(toggled(bool)), this, SLOT(onGridToggled(bool)) );
    QObject::connect( _lock, SIGNAL(toggled(bool)), this, SLOT(onLockToggled(bool)) );
    QObject::connect( frame, SIGNAL(clicked()), this, SLOT(onFrameClicked()) );
    _gl->onPick = [this](const std::string& n, bool add) { pickNode(n, add); };
    _gl->onPresetChanged = [this](int p) {
        if (onPresetChanged) {
            onPresetChanged(p);
        }
    };
    _gl->onLookThroughSelected = [this]() { lookThroughSelected(); };
    _gl->onLeaveCamera = [this]() {
        _view->blockSignals(true);
        _view->setCurrentIndex(0);
        _view->blockSignals(false);
    };
    _timer = new QTimer(this);
    _timer->setInterval(120);
    QObject::connect( _timer, SIGNAL(timeout()), this, SLOT(refresh()) );
    _timer->start();
}

Solv3DView::~Solv3DView()
{
}

void
Solv3DView::setPreset(int p)
{
    _gl->setPreset(p);
    _gl->setFocus();
}

// first 3D node upstream of e (through the main input of 2D nodes)
static Solv3DNode*
upstream3D(EffectInstancePtr e)
{
    for (int guard = 0; e && guard < 64; ++guard) {
        if ( Solv3DNode* n = Solv3DNode::as3D(e) ) {
            return n;
        }
        e = e->getInput(0);
    }

    return 0;
}

static bool
sceneHasNode(const Scene& s, const std::string& name)
{
    for (const auto& kv : s.prims()) {
        if (kv.second.node == name) {
            return true;
        }
    }

    return false;
}

void
Solv3DView::refresh()
{
    if ( !isVisible() || !_gui || !_gui->getApp() ) {
        return;
    }
    const double t = _gui->getApp()->getTimeLine()->currentFrame();
    _gl->setTime(t);
    Scene main, templ;
    ViewerInstance* vi = _viewer ? _viewer->getInternalNode() : 0;
    if (vi) {
        int a = -1, b = -1;
        vi->getActiveInputs(a, b);
        EffectInstancePtr in = (a >= 0) ? vi->getInput(a) : EffectInstancePtr();
        if ( Solv3DNode* n = upstream3D(in) ) {
            n->buildScene(t, &main);
        }
    }
    std::set<std::string> sel;
    const NodesGuiList& selection = _gui->getSelectedNodes();
    for (NodesGuiList::const_iterator it = selection.begin(); it != selection.end(); ++it) {
        NodePtr node = (*it) ? (*it)->getNode() : NodePtr();
        EffectInstancePtr e = node ? node->getEffectInstance() : EffectInstancePtr();
        Solv3DNode* n = e ? Solv3DNode::as3D(e) : 0;
        if (!n) {
            continue;
        }
        const std::string name = node->getFullyQualifiedName();
        sel.insert(name);
        if ( !sceneHasNode(main, name) ) {
            Scene s;
            n->buildScene(t, &s);
            // its own prims get the highlight
            for (auto& kv : s.prims()) {
                if ( kv.second.node.empty() ) {
                    kv.second.node = name;
                }
            }
            templ.merge(s);
        }
    }
    for (const auto& kv : templ.prims()) {
        sel.insert(kv.second.node);
    }
    _gl->setScenes(main, templ, sel);
    // camera list
    std::vector<std::string> cams = main.pathsOfType(PrimType::Camera);
    std::vector<std::string> tc = templ.pathsOfType(PrimType::Camera);
    cams.insert( cams.end(), tc.begin(), tc.end() );
    if (cams != _cams) {
        _cams = cams;
        const QString cur = _view->currentText();
        _view->blockSignals(true);
        _view->clear();
        _view->addItem( tr("default") );
        for (const std::string& c : cams) {
            _view->addItem( QString::fromUtf8( c.c_str() ) );
        }
        const int idx = _view->findText(cur);
        _view->setCurrentIndex(idx >= 0 ? idx : 0);
        _view->blockSignals(false);
    }
}

void
Solv3DView::lookThroughSelected()
{
    const NodesGuiList& selection = _gui->getSelectedNodes();
    for (NodesGuiList::const_reverse_iterator it = selection.rbegin(); it != selection.rend(); ++it) {
        NodePtr node = (*it) ? (*it)->getNode() : NodePtr();
        if ( !node || (node->getPluginID() != PLUGINID_SOLV3D_CAMERA) ) {
            continue;
        }
        const std::string name = node->getFullyQualifiedName();
        for (size_t i = 0; i < _cams.size(); ++i) {
            // the camera prim of a Camera node is "/<name>" (possibly under a parent)
            const std::string& c = _cams[i];
            const std::string leaf = leafName(c);
            if ( leaf == makeValidName( node->getScriptName_mt_safe() ) ) {
                _view->setCurrentIndex( (int)i + 1 );

                return;
            }
        }
        (void)name;
    }
}

void
Solv3DView::pickNode(const std::string& name, bool add)
{
    if ( name.empty() || !_gui || !_gui->getApp() ) {
        return;
    }
    NodePtr node = _gui->getApp()->getNodeByFullySpecifiedName(name);
    NodeGuiPtr ng = node ? std::dynamic_pointer_cast<NodeGui>( node->getNodeGui() ) : NodeGuiPtr();
    NodeGraph* graph = ng ? ng->getDagGui() : 0;
    if (graph) {
        graph->selectNode(ng, add);
    }
}

void
Solv3DView::onLookThroughChanged(int i)
{
    _gl->setLookThrough( i <= 0 ? std::string() : _view->itemText(i).toStdString() );
}

void
Solv3DView::onDisplayChanged(int i)
{
    _gl->setDisplayOverride(i - 1);
}

void
Solv3DView::onGridToggled(bool on)
{
    _gl->setGrid(on);
}

void
Solv3DView::onLockToggled(bool on)
{
    _gl->setLocked(on);
}

void
Solv3DView::onFrameClicked()
{
    _gl->frameAll();
    if ( !_lock->isChecked() ) {
        _view->blockSignals(true);
        _view->setCurrentIndex(0);
        _view->blockSignals(false);
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_Solv3DViewer.cpp"
