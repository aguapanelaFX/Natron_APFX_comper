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
#include <cstring>
#include <thread>
#include <vector>

#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QFocusEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Curve.h"
#include "Engine/Settings.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoCommand.h"
#include "Engine/NodeGuiI.h"
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
    setMouseTracking(true); // gizmo hover
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
        for (std::map<U64, GpuMesh>::iterator it = _gpu.begin(); it != _gpu.end(); ++it) {
            GLuint b[2] = { it->second.vbo, it->second.lineVbo };
            glDeleteBuffers(2, b);
        }
        doneCurrent();
    }
}

void
Solv3DGLWidget::initializeGL()
{
    appPTR->initializeOpenGLFunctionsOnce();
}

static U64
hashBytes(U64 h, const void* data, size_t n)
{
    // FNV-1a over 8 byte words (tail bytes at the end)
    const unsigned char* p = static_cast<const unsigned char*>(data);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        U64 w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ULL;
    }
    for (; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ULL;
    }

    return h;
}

U64
Solv3DGLWidget::meshKey(const Mesh* m)
{
    std::map<const Mesh*, U64>::const_iterator it = _meshKey.find(m);
    if ( it != _meshKey.end() ) {
        return it->second;
    }
    U64 h = 1469598103934665603ULL;
    h = hashBytes( h, m->points.data(), m->points.size() * sizeof(Vec3) );
    h = hashBytes( h, m->faceVertexCounts.data(), m->faceVertexCounts.size() * sizeof(int) );
    h = hashBytes( h, m->faceVertexIndices.data(), m->faceVertexIndices.size() * sizeof(int) );
    h = hashBytes( h, m->normals.data(), m->normals.size() * sizeof(Vec3) );
    h = hashBytes( h, m->st.data(), m->st.size() * sizeof(Vec2) );
    _meshKey[m] = h;

    return h;
}

void
Solv3DGLWidget::indexMeshes(const Scene& s, std::map<const Mesh*, U64>* out)
{
    for (const auto& kv : s.prims()) {
        const Mesh* m = kv.second.mesh.get();
        if (!m) {
            continue;
        }
        std::map<const Mesh*, U64>::const_iterator it = _meshKey.find(m);
        (*out)[m] = ( it != _meshKey.end() ) ? it->second : meshKey(m);
    }
}

Solv3DGLWidget::GpuMesh*
Solv3DGLWidget::gpuMesh(const Mesh& m)
{
    const U64 key = meshKey(&m);
    GpuMesh& g = _gpu[key];
    g.lastUse = _frameNo;
    if (g.vbo) {
        return &g;
    }
    // interleaved: pos 3, normal 3, uv 2 (fan triangulation of each face)
    g.hasN = m.normals.size() == m.faceVertexIndices.size();
    g.hasUV = m.st.size() == m.faceVertexIndices.size();
    std::vector<float> tri, lin;
    tri.reserve(m.faceVertexIndices.size() * 8 * 2);
    lin.reserve(m.faceVertexIndices.size() * 6);
    size_t base = 0;
    auto put = [&](size_t c) {
        const Vec3& v = m.points[m.faceVertexIndices[c]];
        tri.push_back( (float)v.x ); tri.push_back( (float)v.y ); tri.push_back( (float)v.z );
        const Vec3 n = g.hasN ? m.normals[c] : Vec3(0, 1, 0);
        tri.push_back( (float)n.x ); tri.push_back( (float)n.y ); tri.push_back( (float)n.z );
        const Vec2 uv = g.hasUV ? m.st[c] : Vec2{0, 0};
        tri.push_back( (float)uv.x ); tri.push_back( (float)uv.y );
    };
    for (int n : m.faceVertexCounts) {
        if ( base + n > m.faceVertexIndices.size() ) {
            break;
        }
        for (int k = 1; k + 1 < n; ++k) {
            put(base); put(base + k); put(base + k + 1);
        }
        for (int k = 0; k < n; ++k) {
            const Vec3& a = m.points[m.faceVertexIndices[base + k]];
            const Vec3& b = m.points[m.faceVertexIndices[base + (k + 1) % n]];
            lin.push_back( (float)a.x ); lin.push_back( (float)a.y ); lin.push_back( (float)a.z );
            lin.push_back( (float)b.x ); lin.push_back( (float)b.y ); lin.push_back( (float)b.z );
        }
        base += n;
    }
    GLuint b[2];
    glGenBuffers(2, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData( GL_ARRAY_BUFFER, tri.size() * sizeof(float), tri.data(), GL_STATIC_DRAW );
    glBindBuffer(GL_ARRAY_BUFFER, b[1]);
    glBufferData( GL_ARRAY_BUFFER, lin.size() * sizeof(float), lin.data(), GL_STATIC_DRAW );
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g.vbo = b[0];
    g.lineVbo = b[1];
    g.triVerts = (int)(tri.size() / 8);
    g.lineVerts = (int)(lin.size() / 3);

    return &g;
}

void
Solv3DGLWidget::setScenes(const Scene& main, const Scene& templ, const std::set<std::string>& selectedNodes)
{
    // content keys of the new meshes (the old scenes are still alive, so pointers are not reused yet)
    std::map<const Mesh*, U64> keys;
    indexMeshes(main, &keys);
    indexMeshes(templ, &keys);
    _meshKey.swap(keys);
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
Solv3DGLWidget::frameSelected()
{
    Scene sel;
    const Scene* scenes[2] = { &_scene, &_templ };
    Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
    bool any = false;
    for (int si = 0; si < 2; ++si) {
        for (const auto& kv : scenes[si]->prims()) {
            const Prim& p = kv.second;
            if ( p.node.empty() || !_selected.count(p.node) ) {
                continue;
            }
            const Mat4 W = scenes[si]->worldMatrix(kv.first);
            if (p.mesh) {
                for (const Vec3& q : p.mesh->points) {
                    const Vec3 w = W.point(q);
                    lo = Vec3( std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z) );
                    hi = Vec3( std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z) );
                    any = true;
                }
            } else {
                const Vec3 w = W.point( Vec3() );
                lo = Vec3( std::min(lo.x, w.x - 1), std::min(lo.y, w.y - 1), std::min(lo.z, w.z - 1) );
                hi = Vec3( std::max(hi.x, w.x + 1), std::max(hi.y, w.y + 1), std::max(hi.z, w.z + 1) );
                any = true;
            }
        }
    }
    if (!any) {
        frameAll();

        return;
    }
    _target = (lo + hi) * 0.5;
    const Vec3 d = hi - lo;
    _dist = std::max( 0.5, std::sqrt( dot(d, d) ) * 1.4 );
    if (!_locked) {
        _lookThrough.clear();
    }
    update();
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
    if ( !e && ref && ref->image && (ref->image->w > 0) ) {
        // texture file of the geometry: upload once (a small display copy, at most 1024 px)
        const TexImage& im = *ref->image;
        Tex& t = _tex[im.data.data()];
        if (!t.id) {
            const int step = std::max( 1, std::max(im.w, im.h) / 1024 );
            const int w = std::max(1, im.w / step), h = std::max(1, im.h / step);
            std::vector<unsigned char> px( (size_t)w * h * 4 );
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float* sp = &im.data[( (size_t)(y * step) * im.w + (size_t)(x * step) ) * 4];
                    unsigned char* d = &px[( (size_t)y * w + x ) * 4];
                    for (int c = 0; c < 4; ++c) {
                        float v = sp[c];
                        if (c < 3) {
                            v = (sp[3] > 1e-6f) ? v / sp[3] : 0.f;               // unpremultiply
                            v = std::pow(std::max(0.f, v), 1.f / 2.2f);          // display
                        }
                        d[c] = (unsigned char)std::max( 0.f, std::min(255.f, v * 255.f + 0.5f) );
                    }
                }
            }
            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            t.id = id;
        }

        return t.id;
    }
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
                GpuMesh* g = proj ? 0 : gpuMesh(m);
                if (g) {
                    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
                    glEnableClientState(GL_VERTEX_ARRAY);
                    glVertexPointer( 3, GL_FLOAT, 8 * sizeof(float), (const void*)0 );
                    if (g->hasN) {
                        glEnableClientState(GL_NORMAL_ARRAY);
                        glNormalPointer( GL_FLOAT, 8 * sizeof(float), (const void*)(3 * sizeof(float)) );
                    }
                    if (tex) {
                        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                        glTexCoordPointer( 2, GL_FLOAT, 8 * sizeof(float), (const void*)(6 * sizeof(float)) );
                    }
                    glDrawArrays(GL_TRIANGLES, 0, g->triVerts);
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                    glDisableClientState(GL_NORMAL_ARRAY);
                    glDisableClientState(GL_VERTEX_ARRAY);
                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                }
                size_t base = 0;
                for (int n : (g ? std::vector<int>() : m.faceVertexCounts)) {
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
                GpuMesh* g = gpuMesh(m);
                glBindBuffer(GL_ARRAY_BUFFER, g->lineVbo);
                glEnableClientState(GL_VERTEX_ARRAY);
                glVertexPointer( 3, GL_FLOAT, 0, (const void*)0 );
                glDrawArrays(GL_LINES, 0, g->lineVerts);
                glDisableClientState(GL_VERTEX_ARRAY);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
        } else if ( (p.type == PrimType::Camera) && p.camera && (kv.first != _lookThrough) ) {
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            double is = std::max(0.01, p.camera->iconScale);
            if (!p.camera->iconWorldScale) {
                // same size on screen
                Mat4 PP, VV;
                viewMatrices(&PP, &VV);
                const Vec3 cv = VV.point( W.point( Vec3() ) );
                const double depth = (PP.m[3][3] == 1.) ? 1. / std::max(1e-9, PP.m[1][1]) * 3. : std::max(1e-3, -cv.z) / std::max(1e-9, PP.m[1][1]);
                is *= depth * 0.06;
            }
            glScaled(is, is, is);
            drawCameraIcon(sel, p.camera->horizontalAperture, p.camera->verticalAperture, p.camera->focalLength);
            glPopMatrix();
        } else if ( (p.type == PrimType::Light) && p.light ) {
            drawLightIcon(sel, *p.light);
        } else if ( (p.type == PrimType::Xform) && (p.axisSize > 0.) ) {
            // locator (Axis node): three axes, + ends a little longer
            const double a = p.axisSize;
            glLineWidth(sel ? 2.5f : 1.5f);
            glBegin(GL_LINES);
            const float k = sel ? 1.f : 0.85f;
            glColor3f(0.93f * k, 0.33f * k, 0.31f * k); glVertex3d(-a * 0.5, 0, 0); glVertex3d(a, 0, 0);
            glColor3f(0.4f * k, 0.85f * k, 0.42f * k); glVertex3d(0, -a * 0.5, 0); glVertex3d(0, a, 0);
            glColor3f(0.36f * k, 0.55f * k, 1.f * k); glVertex3d(0, 0, -a * 0.5); glVertex3d(0, 0, a);
            glEnd();
            if (sel) {
                glColor4f(kSelCol[0], kSelCol[1], kSelCol[2], 1.f);
                glPointSize(6.f);
                glBegin(GL_POINTS);
                glVertex3d(0, 0, 0);
                glEnd();
            }
        }
    }
}

// camera icon (Maya / Houdini style): body + film reels + lens hood, a short frustum and the up marker.
// Drawn in camera space (looking down -Z), about 1.5 units long.
void
Solv3DGLWidget::drawCameraIcon(bool sel, double ha, double va, double fl)
{
    const float col[3] = { sel ? kSelCol[0] : 0.62f, sel ? kSelCol[1] : 0.66f, sel ? kSelCol[2] : 0.95f };
    glLineWidth(sel ? 2.f : 1.5f);
    glColor4f(col[0], col[1], col[2], 1.f);
    auto box = [](double x0, double x1, double y0, double y1, double z0, double z1) {
        const double v[8][3] = { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
                                 { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } };
        const int e[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
                               { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        glBegin(GL_LINES);
        for (int i = 0; i < 12; ++i) {
            glVertex3dv(v[e[i][0]]);
            glVertex3dv(v[e[i][1]]);
        }
        glEnd();
    };
    // body (behind the lens: +Z)
    box(-0.25, 0.25, -0.35, 0.3, 0.15, 1.2);
    // film reels on top
    for (int r = 0; r < 2; ++r) {
        const double zc = (r == 0) ? 0.45 : 0.95, yc = 0.3 + 0.24, rad = 0.24;
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 24; ++i) {
            const double a = i * 2. * 3.14159265358979323846 / 24.;
            glVertex3d(0., yc + rad * std::sin(a), zc + rad * std::cos(a) );
        }
        glEnd();
    }
    // lens hood: square tapering forward (-Z)
    {
        const double s0 = 0.14, s1 = 0.26, z0 = 0.15, z1 = -0.35;
        const double q[2][4][3] = { { { -s0, -s0, z0 }, { s0, -s0, z0 }, { s0, s0, z0 }, { -s0, s0, z0 } },
                                    { { -s1, -s1, z1 }, { s1, -s1, z1 }, { s1, s1, z1 }, { -s1, s1, z1 } } };
        glBegin(GL_LINES);
        for (int i = 0; i < 4; ++i) {
            glVertex3dv(q[0][i]); glVertex3dv(q[0][(i + 1) % 4]);
            glVertex3dv(q[1][i]); glVertex3dv(q[1][(i + 1) % 4]);
            glVertex3dv(q[0][i]); glVertex3dv(q[1][i]);
        }
        glEnd();
    }
    // short frustum (field of view) + up triangle
    const double f = std::max(1e-6, fl), d = 2.;
    const double w = d * ha / f * 0.5, h = d * va / f * 0.5;
    glLineWidth(1.f);
    glColor4f(col[0], col[1], col[2], 0.45f);
    glBegin(GL_LINES);
    const double c[4][2] = { { -w, -h }, { w, -h }, { w, h }, { -w, h } };
    for (int i = 0; i < 4; ++i) {
        glVertex3d(0, 0, -0.35); glVertex3d(c[i][0], c[i][1], -0.35 - d);
        glVertex3d(c[i][0], c[i][1], -0.35 - d); glVertex3d(c[(i + 1) % 4][0], c[(i + 1) % 4][1], -0.35 - d);
    }
    glEnd();
    glBegin(GL_TRIANGLES);
    glVertex3d(-w * 0.35, h * 1.08, -0.35 - d); glVertex3d(0, h * 1.4, -0.35 - d); glVertex3d(w * 0.35, h * 1.08, -0.35 - d);
    glEnd();
}

// light icons (in the light's space, pointing down -Z): point = star, spot = cone, distant = parallel arrows,
// ambient = ring
void
Solv3DGLWidget::drawLightIcon(bool sel, const Light& L)
{
    const float lc[3] = { (float)std::min(1., L.color.x * 0.5 + 0.5), (float)std::min(1., L.color.y * 0.5 + 0.5), (float)std::min(1., L.color.z * 0.3 + 0.4) };
    if (sel) {
        glColor4f(kSelCol[0], kSelCol[1], kSelCol[2], 1.f);
    } else {
        glColor4f(lc[0], lc[1], lc[2], 1.f);
    }
    glLineWidth(sel ? 2.f : 1.5f);
    const double pi2 = 2. * 3.14159265358979323846;
    auto circle = [&](double r, double z, int axis) {
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 32; ++i) {
            const double a = i * pi2 / 32., c = r * std::cos(a), s2 = r * std::sin(a);
            if (axis == 2) {
                glVertex3d(c, s2, z);
            } else if (axis == 1) {
                glVertex3d(c, z, s2);
            } else {
                glVertex3d(z, c, s2);
            }
        }
        glEnd();
    };
    if (L.kind == "point") {
        circle(0.35, 0., 0); circle(0.35, 0., 1); circle(0.35, 0., 2);
        glBegin(GL_LINES);
        const double d[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (int i = 0; i < 6; ++i) {
            glVertex3d(d[i][0] * 0.5, d[i][1] * 0.5, d[i][2] * 0.5);
            glVertex3d(d[i][0] * 0.9, d[i][1] * 0.9, d[i][2] * 0.9);
        }
        glEnd();
    } else if (L.kind == "spot") {
        const double len = 2.5, r = len * std::tan( std::min(80., L.coneAngle * 0.5) * 3.14159265358979323846 / 180. );
        circle(0.2, 0., 2);
        circle(r, -len, 2);
        glBegin(GL_LINES);
        for (int i = 0; i < 4; ++i) {
            const double a = i * pi2 / 4.;
            glVertex3d(0, 0, 0);
            glVertex3d(r * std::cos(a), r * std::sin(a), -len);
        }
        glVertex3d(0, 0, 0); glVertex3d(0, 0, -len * 1.2);
        glEnd();
    } else if (L.kind == "distant") {
        glBegin(GL_LINES);
        for (int i = 0; i < 5; ++i) {
            const double x = (i % 3 - 1) * 0.4, y = (i / 3 - 0.5) * 0.4 * (i < 3 ? 1 : -1);
            glVertex3d(x, y, 0.8); glVertex3d(x, y, -1.2);
            glVertex3d(x, y, -1.2); glVertex3d(x + 0.12, y, -0.95);
            glVertex3d(x, y, -1.2); glVertex3d(x - 0.12, y, -0.95);
        }
        glEnd();
        circle(0.3, 0.8, 2);
    } else {
        circle(0.5, 0., 1);
        circle(0.3, 0., 1);
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
        // ground grid (XZ) like Maya: 12 units each side, a line every unit, stronger every 5, axes darkest
        glDisable(GL_LIGHTING);
        glLineWidth(1.f);
        glBegin(GL_LINES);
        for (int i = -12; i <= 12; ++i) {
            const float a = (i == 0) ? 0.08f : ( (i % 5 == 0) ? 0.30f : 0.38f );
            glColor4f(a, a, a, 1.f);
            glVertex3d(i, 0, -12); glVertex3d(i, 0, 12);
            glVertex3d(-12, 0, i); glVertex3d(12, 0, i);
        }
        glEnd();
        // world axes
        glLineWidth(2.f);
        glBegin(GL_LINES);
        glColor3f(0.93f, 0.33f, 0.31f); glVertex3d(0, 0, 0); glVertex3d(2, 0, 0);
        glColor3f(0.4f, 0.85f, 0.42f); glVertex3d(0, 0, 0); glVertex3d(0, 2, 0);
        glColor3f(0.36f, 0.55f, 1.f); glVertex3d(0, 0, 0); glVertex3d(0, 0, 2);
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

    ++_frameNo;
    drawScene(_scene, V, false);
    drawScene(_templ, V, true);
    // free buffers of meshes not drawn for a while
    if (_gpu.size() > 64) {
        for (std::map<U64, GpuMesh>::iterator it = _gpu.begin(); it != _gpu.end(); ) {
            if (_frameNo - it->second.lastUse > 120) {
                GLuint b[2] = { it->second.vbo, it->second.lineVbo };
                glDeleteBuffers(2, b);
                it = _gpu.erase(it);
            } else {
                ++it;
            }
        }
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_BLEND);

    // handles and HUD in 2D over the frame
    QPainter painter(this);
    drawGizmo2D(painter);
    drawHud(painter);
    painter.end();
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
            if ( ( (p.type == PrimType::Camera) && p.camera ) || (p.type == PrimType::Light) || (p.axisSize > 0.) ) {
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


// ---------------------------------------------------------------- manipulator
// (behaviour: see the manipulator spec. Handles are drawn in 2D over the 3D frame.)

static const QColor kAxQ[3] = { QColor(0xff, 0x3a, 0x3a), QColor(0x2f, 0xdc, 0x4f), QColor(0x55, 0x77, 0xff) };
static const QColor kActiveQ(0xff, 0xd2, 0x57);
static const QColor kOutlineQ(0x14, 0x14, 0x14);
static const double kDeg = 180. / 3.14159265358979323846;

// ray / plane (point P0, normal n) intersection
static Vec3
rayPlane(const Vec3& ro, const Vec3& rd, const Vec3& P0, const Vec3& n)
{
    const double den = dot(rd, n);
    if (std::fabs(den) < 1e-12) {
        return P0;
    }

    return ro + rd * ( dot(P0 - ro, n) / den );
}

static double
segDist(double px, double py, double ax, double ay, double bx, double by, double* tOut = 0)
{
    const double ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey;
    double u = l2 > 1e-12 ? ( (px - ax) * ex + (py - ay) * ey ) / l2 : 0.;
    u = std::max( 0., std::min(1., u) );
    if (tOut) {
        *tOut = u;
    }

    return std::hypot(ax + u * ex - px, ay + u * ey - py);
}

// rotation applied in the order of the letters (XYZ = X first), as the nodes do it
static const char* kViewRotOrders[6] = { "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX" };

static Mat4
eulerToMatrix(const Vec3& deg, int order)
{
    const char* o = kViewRotOrders[std::max( 0, std::min(5, order) )];
    Mat4 r;
    for (int i = 0; i < 3; ++i) {
        Vec3 v;
        switch (o[i]) {
        case 'X': v.x = deg.x; break;
        case 'Y': v.y = deg.y; break;
        default: v.z = deg.z; break;
        }
        r = Mat4::rotateXYZ(v) * r;
    }

    return r;
}

// inverse of eulerToMatrix (R = Rc * Rb * Ra, a applied first); keeps the angles close to 'near'
static Vec3
matrixToEuler(const Mat4& R, int order, const Vec3& near)
{
    const char* o = kViewRotOrders[std::max( 0, std::min(5, order) )];
    const int a = o[0] - 'X', b = o[1] - 'X', c = o[2] - 'X';
    // even permutations of (0 1 2) keep the signs
    const double s = ( (a == 0 && b == 1) || (a == 1 && b == 2) || (a == 2 && b == 0) ) ? 1. : -1.;
    const double sb = std::max( -1., std::min(1., -s * R.m[c][a]) );
    double ang[3];
    ang[b] = std::asin(sb);
    if (std::fabs(sb) < 0.9999999) {
        ang[a] = std::atan2(s * R.m[c][b], R.m[c][c]);
        ang[c] = std::atan2(s * R.m[b][a], R.m[a][a]);
    } else {
        // gimbal lock: put everything on the first angle
        ang[c] = 0.;
        ang[a] = std::atan2(-s * R.m[b][c], R.m[b][b]);
    }
    Vec3 e(ang[0] * kDeg, ang[1] * kDeg, ang[2] * kDeg);
    // continuity with the previous values (no jumps of 360)
    auto wrap = [](double v, double ref) {
        while (v - ref > 180.) { v -= 360.; }
        while (v - ref < -180.) { v += 360.; }

        return v;
    };
    e.x = wrap(e.x, near.x); e.y = wrap(e.y, near.y); e.z = wrap(e.z, near.z);

    return e;
}

static Mat4
axisAngle(const Vec3& axis, double deg)
{
    const Vec3 k = normalize(axis);
    const double t = deg / kDeg, c = std::cos(t), s = std::sin(t), C = 1. - c;
    Mat4 m;
    m.m[0][0] = c + k.x * k.x * C;       m.m[0][1] = k.x * k.y * C - k.z * s; m.m[0][2] = k.x * k.z * C + k.y * s;
    m.m[1][0] = k.y * k.x * C + k.z * s; m.m[1][1] = c + k.y * k.y * C;       m.m[1][2] = k.y * k.z * C - k.x * s;
    m.m[2][0] = k.z * k.x * C - k.y * s; m.m[2][1] = k.z * k.y * C + k.x * s; m.m[2][2] = c + k.z * k.z * C;

    return m;
}

static Mat4
linearPart(const Mat4& m)
{
    Mat4 r = m;
    r.m[0][3] = r.m[1][3] = r.m[2][3] = 0.;

    return r;
}

void
Solv3DGLWidget::setGizmo(bool on, const Mat4& parentWorld, const Mat4& world, const Vec3& pivot, const std::string& node)
{
    if ( (_gDrag != ePartNone) && (node != _gNode) ) {
        return; // the dragged node keeps its handle
    }
    _gizmoOn = on;
    _gNode = node;
    _gParent = parentWorld;
    _gWorld = world;
    _gPivot = pivot;
    _gOrigin = world.point(pivot);
}

void
Solv3DGLWidget::setManipMode(int m)
{
    if ( (m == _manipMode) && (m != 0) ) {
        // pressing the active tool again resets its active part
        const int defaults[4] = { ePartCenter, ePartCenter, ePartFree, ePartCenter };
        _activePart[m] = defaults[m];
    }
    if ( (_manipMode != 0) && (m != _manipMode) ) {
        _lastTool = _manipMode;
    }
    _manipMode = m;
    update();
}

void
Solv3DGLWidget::cycleSpace()
{
    _space = (_space + 1) % 3;
    if (onSpaceChanged) {
        onSpaceChanged(_space);
    }
    update();
}

bool
Solv3DGLWidget::screenPoint(const Vec3& w, double* x, double* y) const
{
    Mat4 P, V;
    viewMatrices(&P, &V);
    const Mat4 M = P * V;
    double c[4];
    for (int i = 0; i < 4; ++i) {
        c[i] = M.m[i][0] * w.x + M.m[i][1] * w.y + M.m[i][2] * w.z + M.m[i][3];
    }
    if (c[3] <= 1e-9) {
        return false;
    }
    *x = (c[0] / c[3] * 0.5 + 0.5) * width();
    *y = (1. - (c[1] / c[3] * 0.5 + 0.5) ) * height();

    return true;
}

void
Solv3DGLWidget::mouseRay(const QPoint& p, Vec3* o, Vec3* d) const
{
    Mat4 P, V;
    viewMatrices(&P, &V);
    const Mat4 inv = (P * V).inverse();
    const double nx = 2. * p.x() / std::max(1, width()) - 1., ny = 1. - 2. * p.y() / std::max(1, height());
    auto un = [&](double z) {
        double c[4];
        const double in[4] = { nx, ny, z, 1. };
        for (int i = 0; i < 4; ++i) {
            c[i] = inv.m[i][0] * in[0] + inv.m[i][1] * in[1] + inv.m[i][2] * in[2] + inv.m[i][3] * in[3];
        }

        return Vec3(c[0] / c[3], c[1] / c[3], c[2] / c[3]);
    };
    const Vec3 a = un(-1.), b = un(1.);
    *o = a;
    *d = normalize(b - a);
}

Vec3
Solv3DGLWidget::eyePos() const
{
    Mat4 P, V;
    viewMatrices(&P, &V);

    return V.inverse().point( Vec3() );
}

void
Solv3DGLWidget::gizmoAxes(Vec3 ax[3]) const
{
    if (_gLive) {
        for (int i = 0; i < 3; ++i) {
            ax[i] = _gLiveAx[i];
        }

        return;
    }
    const int t = tool();
    const int sp = (t == 3) ? 0 : _space; // scale: always the object's own axes
    if (sp == 1) {
        ax[0] = Vec3(1, 0, 0); ax[1] = Vec3(0, 1, 0); ax[2] = Vec3(0, 0, 1);

        return;
    }
    if (sp == 2) {
        Mat4 P, V;
        viewMatrices(&P, &V);
        for (int i = 0; i < 3; ++i) {
            ax[i] = normalize( Vec3(V.m[i][0], V.m[i][1], V.m[i][2]) );
        }

        return;
    }
    for (int i = 0; i < 3; ++i) {
        Vec3 e;
        (i == 0 ? e.x : (i == 1 ? e.y : e.z) ) = 1.;
        const Vec3 v = _gWorld.vector(e);
        const double l = std::sqrt( dot(v, v) );
        ax[i] = l > 1e-12 ? v * (1. / l) : e;
    }
}

double
Solv3DGLWidget::gizmoLength() const
{
    Mat4 P, V;
    viewMatrices(&P, &V);
    const double inv = 1. / std::max(1e-9, P.m[1][1]); // tan(fov / 2)
    if (P.m[3][3] == 1.) { // orthographic: half height of the view = 1 / P11
        return inv * 0.28 * _handleScale;
    }
    const Vec3 d = eyePos() - _gOrigin;

    return std::sqrt( dot(d, d) ) * inv * 0.28 * _handleScale;
}

static bool
insidePoly(double px, double py, const double* x, const double* y, int n)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ( ( (y[i] > py) != (y[j] > py) ) && ( px < (x[j] - x[i]) * (py - y[i]) / (y[j] - y[i]) + x[i] ) ) {
            in = !in;
        }
    }

    return in;
}

int
Solv3DGLWidget::hitPart(const QPoint& pt) const
{
    if ( !gizmoVisible() ) {
        return ePartNone;
    }
    const double px = pt.x(), py = pt.y();
    double cx, cy;
    if ( !screenPoint(_gOrigin, &cx, &cy) ) {
        return ePartNone;
    }
    Vec3 ax[3];
    gizmoAxes(ax);
    const double L = gizmoLength();
    const int t = tool();
    if (t == 2) {
        // rings: nearest segment within 9 px with an end on the front half
        const Vec3 eye = eyePos();
        int best = ePartNone;
        double bd = 9.;
        for (int k = 0; k < 3; ++k) {
            const Vec3& u = ax[(k + 1) % 3];
            const Vec3& v = ax[(k + 2) % 3];
            double x0 = 0, y0 = 0;
            bool f0 = false, have = false;
            for (int i = 0; i <= 64; ++i) {
                const double a = i * 2. * 3.14159265358979323846 / 64.;
                const Vec3 w = _gOrigin + (u * std::cos(a) + v * std::sin(a) ) * L;
                const bool front = dot(w - _gOrigin, eye - _gOrigin) >= 0.;
                double x, y;
                if ( !screenPoint(w, &x, &y) ) {
                    have = false;
                    continue;
                }
                if ( have && (front || f0) ) {
                    const double d = segDist(px, py, x0, y0, x, y);
                    if (d < bd) {
                        bd = d;
                        best = k;
                    }
                }
                x0 = x; y0 = y; f0 = front; have = true;
            }
        }
        if (best != ePartNone) {
            return best;
        }
        double ex, ey;
        if ( screenPoint(_gOrigin + ax[0] * L, &ex, &ey) && (std::hypot(px - cx, py - cy) < 0.85 * std::hypot(ex - cx, ey - cy) ) ) {
            return ePartFree;
        }

        return ePartNone;
    }
    // move / scale / pivot
    if (std::fabs(px - cx) + std::fabs(py - cy) < 12.) {
        return ePartCenter;
    }
    if ( (t == 1) || (t == 3) ) {
        for (int k = 0; k < 3; ++k) {
            const Vec3& a = ax[(k + 1) % 3];
            const Vec3& b = ax[(k + 2) % 3];
            const double s1 = 0.36 * L, s2 = 0.56 * L;
            const Vec3 q[4] = { _gOrigin + a * s1 + b * s1, _gOrigin + a * s2 + b * s1, _gOrigin + a * s2 + b * s2, _gOrigin + a * s1 + b * s2 };
            double xs[4], ys[4];
            bool ok = true;
            for (int i = 0; i < 4 && ok; ++i) {
                ok = screenPoint(q[i], &xs[i], &ys[i]);
            }
            if ( ok && insidePoly(px, py, xs, ys, 4) ) {
                return ePartPlaneX + k;
            }
        }
    }
    int best = ePartNone;
    double bd = 9.;
    for (int k = 0; k < 3; ++k) {
        double ex, ey;
        if ( !screenPoint(_gOrigin + ax[k] * L, &ex, &ey) ) {
            continue;
        }
        const double d = segDist(px, py, cx, cy, ex, ey);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }

    return best;
}

void
Solv3DGLWidget::drawGizmo2D(QPainter& p)
{
    if ( !gizmoVisible() ) {
        return;
    }
    double cx, cy;
    if ( !screenPoint(_gOrigin, &cx, &cy) ) {
        return;
    }
    Vec3 ax[3];
    gizmoAxes(ax);
    const double L = gizmoLength();
    const int t = tool();
    const int lit = (_gDrag != ePartNone) ? _gDrag : ( (_gHot != ePartNone) ? _gHot : _activePart[t] );
    const QPointF c(cx, cy);
    auto scr = [&](const Vec3& w, QPointF* o) -> bool {
        double x, y;
        if ( !screenPoint(w, &x, &y) ) {
            return false;
        }
        *o = QPointF(x, y);

        return true;
    };
    p.setRenderHint(QPainter::Antialiasing, true);
    if (t == 2) {
        const Vec3 eye = eyePos();
        // ghost axes of the drag start
        if (_gDrag != ePartNone) {
            QPen gp(QColor(200, 200, 200, 70), 1., Qt::DashLine);
            p.setPen(gp);
            for (int k = 0; k < 3; ++k) {
                QPointF e;
                if ( scr(_gC0 + _gAx0[k] * _gL0, &e) ) {
                    p.drawLine(c, e);
                }
            }
        }
        // trackball
        QPointF e0;
        double rad = L;
        if ( scr(_gOrigin + ax[0] * L, &e0) ) {
            rad = std::hypot(e0.x() - cx, e0.y() - cy);
        }
        p.setPen(Qt::NoPen);
        p.setBrush( QColor(160, 160, 160, lit == ePartFree ? 38 : 14) );
        p.drawEllipse(c, rad, rad);
        p.setBrush(Qt::NoBrush);
        for (int k = 0; k < 3; ++k) {
            const Vec3& u = ax[(k + 1) % 3];
            const Vec3& v = ax[(k + 2) % 3];
            const QColor col = (lit == k) ? kActiveQ : kAxQ[k];
            QPointF prev;
            bool have = false;
            for (int i = 0; i <= 64; ++i) {
                const double a = i * 2. * 3.14159265358979323846 / 64.;
                const Vec3 w = _gOrigin + (u * std::cos(a) + v * std::sin(a) ) * L;
                QPointF q;
                if ( !scr(w, &q) ) {
                    have = false;
                    continue;
                }
                if (have) {
                    const Vec3 wm = _gOrigin + ( u * std::cos(a - 3.14159265358979323846 / 64.) + v * std::sin(a - 3.14159265358979323846 / 64.) ) * L;
                    const bool front = dot(wm - _gOrigin, eye - _gOrigin) >= 0.;
                    QColor cc = col;
                    if (!front) {
                        cc.setAlpha(60);
                    }
                    p.setPen( QPen(cc, front ? 2.2 : 1.2) );
                    p.drawLine(prev, q);
                }
                prev = q;
                have = true;
            }
        }
        p.setPen(Qt::NoPen);
        p.setBrush( (lit == ePartFree) && (_gDrag == ePartFree) ? kActiveQ : QColor(0xc8, 0xc8, 0xc8) );
        p.drawEllipse(c, 5., 5.);

        return;
    }
    if (t == 4) {
        // pivot: dashed axes and a crosshair
        for (int k = 0; k < 3; ++k) {
            QPointF e;
            if ( scr(_gOrigin + ax[k] * L, &e) ) {
                p.setPen( QPen(lit == k ? kActiveQ : kAxQ[k], 2., Qt::DashLine) );
                p.drawLine(c, e);
            }
        }
        const QColor cc = (lit == ePartCenter) ? kActiveQ : QColor(220, 220, 220);
        p.setPen( QPen(cc, 1.5) );
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 9., 9.);
        p.drawLine( QPointF(cx - 14., cy), QPointF(cx + 14., cy) );
        p.drawLine( QPointF(cx, cy - 14.), QPointF(cx, cy + 14.) );

        return;
    }
    // move / scale: plane squares, axes with arrow heads (move) or squares (scale), centre box
    for (int k = 0; k < 3; ++k) {
        const Vec3& a = ax[(k + 1) % 3];
        const Vec3& b = ax[(k + 2) % 3];
        const double s1 = 0.36 * L, s2 = 0.56 * L;
        QPointF q[4];
        if ( scr(_gOrigin + a * s1 + b * s1, &q[0]) && scr(_gOrigin + a * s2 + b * s1, &q[1]) &&
             scr(_gOrigin + a * s2 + b * s2, &q[2]) && scr(_gOrigin + a * s1 + b * s2, &q[3]) ) {
            const bool on = (lit == ePartPlaneX + k);
            QColor f = on ? kActiveQ : kAxQ[k];
            f.setAlpha(on ? 110 : 55);
            p.setBrush(f);
            p.setPen( QPen(on ? kActiveQ : kAxQ[k], 1.) );
            p.drawPolygon(q, 4);
        }
    }
    for (int k = 0; k < 3; ++k) {
        QPointF e;
        if ( !scr(_gOrigin + ax[k] * L, &e) ) {
            continue;
        }
        const QColor col = (lit == k) ? kActiveQ : kAxQ[k];
        p.setPen( QPen(col, 2.2) );
        p.drawLine(c, e);
        const double dx = e.x() - cx, dy = e.y() - cy, len = std::hypot(dx, dy);
        const double ux = len > 1e-6 ? dx / len : 1., uy = len > 1e-6 ? dy / len : 0.;
        p.setPen( QPen(kOutlineQ, 1.) );
        p.setBrush(col);
        if (t == 1) {
            const QPointF tip(e.x() + ux * 12., e.y() + uy * 12.);
            const QPointF tri[3] = { tip, QPointF(e.x() - uy * 5., e.y() + ux * 5.), QPointF(e.x() + uy * 5., e.y() - ux * 5.) };
            p.drawPolygon(tri, 3);
        } else {
            p.drawRect( QRectF(e.x() - 5., e.y() - 5., 10., 10.) );
        }
    }
    p.setPen( QPen(kOutlineQ, 1.) );
    p.setBrush( (lit == ePartCenter) ? QColor(0xff, 0xf2, 0xb0) : kActiveQ );
    p.drawRect( QRectF(cx - 5., cy - 5., 10., 10.) );
}

std::string
Solv3DGLWidget::snapLabel() const
{
    std::vector<std::string> s;
    if ( held(Qt::Key_X) || _snapGrid ) { s.push_back("grid"); }
    if ( held(Qt::Key_C) ) { s.push_back("edges"); }
    if ( held(Qt::Key_V) ) { s.push_back( held(Qt::Key_Shift) ? "face centres" : "points" ); }
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        r += (i ? ", " : "") + s[i];
    }

    return r;
}

void
Solv3DGLWidget::drawHud(QPainter& p)
{
    p.setRenderHint(QPainter::TextAntialiasing, true);
    QFont f = p.font();
    f.setPointSizeF(9.);
    p.setFont(f);
    const char* tools[5] = { "select", "move", "rotate", "scale", "pivot" };
    const char* spaces[3] = { "local", "global", "view" };
    std::string top = tools[tool()];
    if ( (tool() != 0) && (tool() != 3) ) {
        top += std::string(" \xC2\xB7 ") + spaces[_space];
    } else if (tool() == 3) {
        top += " \xC2\xB7 local";
    }
    p.setPen( QColor(200, 200, 200, 200) );
    p.drawText( QPointF(8., 16.), QString::fromUtf8( top.c_str() ) );
    const std::string sn = snapLabel();
    if ( !sn.empty() ) {
        p.setPen(kActiveQ);
        p.drawText( QPointF(8., height() - 8.), QString::fromUtf8( ("snap: " + sn).c_str() ) );
    }
}

void
Solv3DGLWidget::buildSnapData()
{
    _snapPts.clear(); _snapFaces.clear(); _snapEdges.clear();
    _snapPtsScr.clear(); _snapFacesScr.clear(); _snapEdgesScr.clear();
    const Scene* scenes[2] = { &_scene, &_templ };
    size_t budget = 400000;
    for (int si = 0; si < 2; ++si) {
        for (const auto& kv : scenes[si]->prims()) {
            const Prim& pr = kv.second;
            if ( !pr.mesh || (pr.display == 0) || (pr.node == _gNode) || primHidden(*scenes[si], kv.first) ) {
                continue;
            }
            const Mesh& m = *pr.mesh;
            if (m.points.size() > budget) {
                continue;
            }
            budget -= m.points.size();
            const Mat4 W = scenes[si]->worldMatrix(kv.first);
            std::vector<Vec3> wp( m.points.size() );
            for (size_t i = 0; i < m.points.size(); ++i) {
                wp[i] = W.point(m.points[i]);
                _snapPts.push_back(wp[i]);
            }
            size_t base = 0;
            for (int n : m.faceVertexCounts) {
                if ( base + n > m.faceVertexIndices.size() ) {
                    break;
                }
                Vec3 cs;
                for (int k = 0; k < n; ++k) {
                    const int ia = m.faceVertexIndices[base + k], ib = m.faceVertexIndices[base + (k + 1) % n];
                    cs = cs + wp[ia];
                    if (ia < ib) { // shared edges once
                        _snapEdges.push_back( std::make_pair(wp[ia], wp[ib]) );
                    }
                }
                if (n > 0) {
                    _snapFaces.push_back( cs * (1. / n) );
                }
                base += n;
            }
        }
    }
    // screen positions (the camera does not move during a drag)
    auto proj = [&](const Vec3& w, std::vector<double>* out) {
        double x, y;
        if ( !screenPoint(w, &x, &y) ) {
            x = y = -1e9;
        }
        out->push_back(x);
        out->push_back(y);
    };
    for (const Vec3& w : _snapPts) { proj(w, &_snapPtsScr); }
    for (const Vec3& w : _snapFaces) { proj(w, &_snapFacesScr); }
    for (const auto& e : _snapEdges) { proj(e.first, &_snapEdgesScr); proj(e.second, &_snapEdgesScr); }
}

bool
Solv3DGLWidget::snapTarget(const QPoint& mouse, Vec3* target) const
{
    const double mx = mouse.x(), my = mouse.y();
    auto nearest = [&](const std::vector<Vec3>& pts, const std::vector<double>& scr) -> int {
        int best = -1;
        double bd = 25.;
        for (size_t i = 0; i < pts.size(); ++i) {
            const double d = std::hypot(scr[i * 2] - mx, scr[i * 2 + 1] - my);
            if (d < bd) {
                bd = d;
                best = (int)i;
            }
        }

        return best;
    };
    if ( held(Qt::Key_V) && held(Qt::Key_Shift) ) {
        const int i = nearest(_snapFaces, _snapFacesScr);
        if (i >= 0) {
            *target = _snapFaces[i];

            return true;
        }
    } else if ( held(Qt::Key_V) ) {
        const int i = nearest(_snapPts, _snapPtsScr);
        if (i >= 0) {
            *target = _snapPts[i];

            return true;
        }
    }
    if ( held(Qt::Key_C) ) {
        int best = -1;
        double bd = 25., bt = 0.;
        for (size_t i = 0; i < _snapEdges.size(); ++i) {
            double tt;
            const double d = segDist(mx, my, _snapEdgesScr[i * 4], _snapEdgesScr[i * 4 + 1], _snapEdgesScr[i * 4 + 2], _snapEdgesScr[i * 4 + 3], &tt);
            if (d < bd) {
                bd = d;
                best = (int)i;
                bt = tt;
            }
        }
        if (best >= 0) {
            const auto& e = _snapEdges[best];
            *target = e.first + (e.second - e.first) * bt;

            return true;
        }
    }
    if ( held(Qt::Key_X) || _snapGrid ) {
        *target = Vec3( std::round(target->x), std::round(target->y), std::round(target->z) );

        return true;
    }

    return false;
}

void
Solv3DGLWidget::beginManip(int part, const QPoint& pos)
{
    if ( (part == ePartNone) || !onManipBegin || !onManipBegin(&_gStart) ) {
        return;
    }
    const int t = tool();
    _gDrag = part;
    _activePart[t] = part;
    _gMouse0 = pos;
    _gParent0 = _gParent;
    _gWorld0 = _gWorld;
    _gC0 = _gOrigin;
    gizmoAxes(_gAx0);
    _gL0 = gizmoLength();
    screenPoint(_gC0, &_gSx0, &_gSy0);
    _gSegX = _gSegY = 0.;
    if (part <= ePartZ) {
        double ex, ey;
        if ( screenPoint(_gC0 + _gAx0[part] * _gL0, &ex, &ey) ) {
            _gSegX = ex - _gSx0;
            _gSegY = ey - _gSy0;
        }
    }
    if ( (part >= ePartPlaneX) && (part <= ePartPlaneZ) ) {
        Vec3 ro, rd;
        mouseRay(pos, &ro, &rd);
        _gHit0 = rayPlane(ro, rd, _gC0, _gAx0[part - ePartPlaneX]);
    }
    if ( (t == 1) || (t == 4) ) {
        buildSnapData();
    }
    _keyUsed = true;
    update();
}

void
Solv3DGLWidget::dragManip(const QPoint& pos)
{
    if ( (_gDrag == ePartNone) || !onManipSet ) {
        return;
    }
    const int t = tool();
    const double dx = pos.x() - _gMouse0.x(), dy = pos.y() - _gMouse0.y();
    const Mat4 S = linearPart(_gParent0), S3inv = S.inverse();
    const double segLen = std::hypot(_gSegX, _gSegY);
    const double px = segLen > 1e-6 ? (dx * _gSegX + dy * _gSegY) / segLen : 0.;
    Mat4 P, V;
    viewMatrices(&P, &V);
    if ( (t == 1) || (t == 4) ) {
        Vec3 delta;
        if (_gDrag <= ePartZ) {
            delta = segLen > 1e-6 ? _gAx0[_gDrag] * (px / segLen * _gL0) : Vec3();
        } else if (_gDrag <= ePartPlaneZ) {
            Vec3 ro, rd;
            mouseRay(pos, &ro, &rd);
            delta = rayPlane(ro, rd, _gC0, _gAx0[_gDrag - ePartPlaneX]) - _gHit0;
        } else {
            // view plane: world units per pixel at the centre's depth
            double k = 2. / ( std::max(1e-9, P.m[1][1]) * std::max(1, height()) );
            if (P.m[3][3] != 1.) {
                const Vec3 d = eyePos() - _gC0;
                k *= std::sqrt( dot(d, d) );
            }
            const Vec3 right = normalize( Vec3(V.m[0][0], V.m[0][1], V.m[0][2]) ), up = normalize( Vec3(V.m[1][0], V.m[1][1], V.m[1][2]) );
            delta = (right * dx - up * dy) * k;
        }
        Vec3 target = _gC0 + delta;
        snapTarget(pos, &target);
        if (t == 1) {
            onManipSet(1, _gStart.translate + S3inv.vector(target - _gC0), 0.);
        } else {
            // pivot: the object stays in place
            const Vec3 p2 = _gWorld0.inverse().point(target);
            const Mat4 RS = linearPart( _gParent0.inverse() * _gWorld0 );
            const Vec3 p0 = _gStart.pivot;
            const Vec3 t2 = _gStart.translate + (p0 - p2) + RS.vector(p2 - p0);
            onManipSet(5, p2, 0.);
            onManipSet(1, t2, 0.);
        }
    } else if (t == 2) {
        const Mat4 R0 = eulerToMatrix(_gStart.rotate, _gStart.rotOrder);
        Mat4 Rn;
        if (_gDrag <= ePartZ) {
            const double a0 = std::atan2(_gMouse0.y() - _gSy0, _gMouse0.x() - _gSx0);
            const double a1 = std::atan2(pos.y() - _gSy0, pos.x() - _gSx0);
            double deg = -(a1 - a0) * kDeg; // screen y points down
            while (deg > 180.) { deg -= 360.; }
            while (deg < -180.) { deg += 360.; }
            const Vec3 axView = V.vector(_gAx0[_gDrag]);
            if (axView.z < 0.) {
                deg = -deg;                  // the axis points away from the camera
            }
            if ( held(Qt::Key_J) ) {
                deg = std::round(deg / 15.) * 15.;
            }
            Rn = axisAngle(S3inv.vector(_gAx0[_gDrag]), deg) * R0;
        } else {
            const Vec3 right = normalize( Vec3(V.m[0][0], V.m[0][1], V.m[0][2]) ), up = normalize( Vec3(V.m[1][0], V.m[1][1], V.m[1][2]) );
            double ay = dx * 0.5, ax = dy * 0.5;
            if ( held(Qt::Key_J) ) {
                ay = std::round(ay / 15.) * 15.;
                ax = std::round(ax / 15.) * 15.;
            }
            Rn = axisAngle(S3inv.vector(right), ax) * axisAngle(S3inv.vector(up), ay) * R0;
        }
        const Vec3 e = matrixToEuler(Rn, _gStart.rotOrder, _gStart.rotate);
        onManipSet(2, e, 0.);
        // the handle turns with the object (local space)
        if (_space == 0) {
            const Mat4 W = S * eulerToMatrix(e, _gStart.rotOrder);
            for (int i = 0; i < 3; ++i) {
                Vec3 u;
                (i == 0 ? u.x : (i == 1 ? u.y : u.z) ) = 1.;
                _gLiveAx[i] = normalize( W.vector(u) );
            }
            _gLive = true;
        }
    } else if (t == 3) {
        if (_gDrag <= ePartZ) {
            Vec3 s = _gStart.scale;
            const double f = std::max( 0.001, 1. + (segLen > 1e-6 ? px / segLen : 0.) );
            (_gDrag == 0 ? s.x : (_gDrag == 1 ? s.y : s.z) ) = std::max(0.001, (_gDrag == 0 ? s.x : (_gDrag == 1 ? s.y : s.z) ) * f);
            onManipSet(3, s, 0.);
        } else {
            const double f = std::max(0.01, 1. + (dx - dy) / 150.);
            if (_gDrag == ePartCenter) {
                onManipSet(4, Vec3(), _gStart.uniform * f);
            } else {
                const int k = _gDrag - ePartPlaneX;
                Vec3 s = _gStart.scale;
                if (k != 0) { s.x *= f; }
                if (k != 1) { s.y *= f; }
                if (k != 2) { s.z *= f; }
                onManipSet(3, s, 0.);
            }
        }
    }
    update();
}

void
Solv3DGLWidget::endManip()
{
    if (_gDrag == ePartNone) {
        return;
    }
    _gDrag = ePartNone;
    _gMiddle = false;
    _gLive = false;
    if (onManipEnd) {
        onManipEnd();
    }
    update();
}

void
Solv3DGLWidget::centrePivot()
{
    if ( !_gizmoOn || _gNode.empty() || !onManipBegin || !onManipSet || !onManipBegin(&_gStart) ) {
        return;
    }
    // bounding box of the node's geometry in its own space
    const Mat4 inv = _gWorld.inverse();
    Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
    bool any = false;
    const Scene* scenes[2] = { &_scene, &_templ };
    for (int si = 0; si < 2; ++si) {
        for (const auto& kv : scenes[si]->prims()) {
            if ( !kv.second.mesh || (kv.second.node != _gNode) ) {
                continue;
            }
            const Mat4 M = inv * scenes[si]->worldMatrix(kv.first);
            for (const Vec3& q : kv.second.mesh->points) {
                const Vec3 l = M.point(q);
                lo = Vec3( std::min(lo.x, l.x), std::min(lo.y, l.y), std::min(lo.z, l.z) );
                hi = Vec3( std::max(hi.x, l.x), std::max(hi.y, l.y), std::max(hi.z, l.z) );
                any = true;
            }
        }
    }
    if (!any) {
        return;
    }
    const Vec3 p2 = (lo + hi) * 0.5;
    const Mat4 RS = linearPart( _gParent.inverse() * _gWorld );
    const Vec3 p0 = _gStart.pivot;
    onManipSet(5, p2, 0.);
    onManipSet(1, _gStart.translate + (p0 - p2) + RS.vector(p2 - p0), 0.);
    if (onManipEnd) {
        onManipEnd();
    }
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
    } else if (alt && (e->button() == Qt::MiddleButton)) {
        _dragMode = 2;
    } else if (alt && (e->button() == Qt::RightButton)) {
        _dragMode = 3;
    } else if (e->button() == Qt::MiddleButton) {
        // middle-drag drives the active part of the handle (else tracks the view)
        if ( gizmoVisible() ) {
            _gMiddle = true;
            beginManip(_activePart[tool()], e->pos());
            _moved = true;
        } else {
            _dragMode = 2;
        }
    } else if (e->button() == Qt::RightButton) {
        QMenu m(this);
        QAction* g = m.addAction( tr("snap to grid") );
        g->setCheckable(true);
        g->setChecked(_snapGrid);
        QAction* sp = m.addAction( tr("handle space: %1").arg( QString::fromUtf8(_space == 0 ? "local" : _space == 1 ? "global" : "view") ) );
        QAction* chosen = m.exec( e->globalPos() );
        if (chosen == g) {
            _snapGrid = !_snapGrid;
        } else if (chosen == sp) {
            cycleSpace();
        }
        update();

        return;
    }
    if ( _dragMode && _locked && !_lookThrough.empty() ) {
        _dragMode = 0; // locked to the camera
    }
    if ( !_dragMode && (e->button() == Qt::LeftButton) ) {
        const int h = hitPart( e->pos() );
        if (h != ePartNone) {
            beginManip(h, e->pos());
            _moved = true; // no pick on release
        }
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
    if (_gDrag != ePartNone) {
        dragManip( e->pos() );

        return;
    }
    if (!_dragMode) {
        const int h = hitPart( e->pos() );
        if (h != _gHot) {
            _gHot = h;
            update();
        }

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
    case 3: {
        // right / up = closer (Maya); Settings > Viewer can invert it
        const bool inv = appPTR->getCurrentSettings()->solvfx3DInvertDolly();
        const double amount = (d.x() - d.y()) * (inv ? 1. : -1.);
        _dist = std::max( 0.05, _dist * std::pow(1.006, amount) );
        break;
    }
    default:
        return;
    }
    update();
}

void
Solv3DGLWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (_gDrag != ePartNone) {
        endManip();
        _dragMode = 0;

        return;
    }
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
Solv3DGLWidget::enterEvent(QEvent* e)
{
    // hotkeys go to the pane under the cursor
    if ( window() && window()->isActiveWindow() ) {
        setFocus(Qt::MouseFocusReason);
    }
    QOpenGLWidget::enterEvent(e);
}

void
Solv3DGLWidget::focusOutEvent(QFocusEvent* e)
{
    _held.clear();
    _keyDown.clear();
    if (_pivotByD) {
        _pivotMode = false;
        _pivotByD = false;
    }
    update();
    QOpenGLWidget::focusOutEvent(e);
}

void
Solv3DGLWidget::keyPressEvent(QKeyEvent* e)
{
    const int key = e->key();
    if ( e->isAutoRepeat() ) {
        if ( (key == Qt::Key_Plus) || (key == Qt::Key_Equal) || (key == Qt::Key_Minus) ) {
            // size keys repeat
        } else {
            return;
        }
    }
    _held.insert(key);
    const Qt::KeyboardModifiers mods = e->modifiers();
    const bool shift = mods & Qt::ShiftModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    switch (key) {
    case Qt::Key_F:
        frameSelected();

        return;
    case Qt::Key_A:
        frameAll();

        return;
    case Qt::Key_X:
    case Qt::Key_C:
    case Qt::Key_Z:
    case Qt::Key_V:
        // views switch on release (when the key was not held for snapping during a drag)
        _keyDown[key] = QPoint(shift ? 1 : 0, 0);
        _keyUsed = (_gDrag != ePartNone);
        update();

        return;
    case Qt::Key_J:
    case Qt::Key_Shift:
        update();

        return;
    case Qt::Key_H:
        if (onLookThroughSelected) {
            onLookThroughSelected();
        }

        return;
    case Qt::Key_P:
        if (ctrl && shift) {
            centrePivot();
        }

        return;
    case Qt::Key_Insert:
        _pivotMode = !_pivotMode;
        _pivotByD = false;
        update();

        return;
    case Qt::Key_D:
        if (!_pivotMode) {
            _pivotMode = true;
            _pivotByD = true;
            update();
        }

        return;
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde:
        cycleSpace();

        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        _handleScale = std::min(4., _handleScale * 1.2);
        update();

        return;
    case Qt::Key_Minus:
        _handleScale = std::max(0.25, _handleScale / 1.2);
        update();

        return;
    case Qt::Key_Y:
    case Qt::Key_Q:
    case Qt::Key_W:
    case Qt::Key_E:
    case Qt::Key_R: {
        const int m = (key == Qt::Key_Y) ? _lastTool : (key == Qt::Key_Q) ? 0 : (key == Qt::Key_W) ? 1 : (key == Qt::Key_E) ? 2 : 3;
        setManipMode(m);
        if (onManipModeChanged) {
            onManipModeChanged(m);
        }

        return;
    }
    default:
        QOpenGLWidget::keyPressEvent(e);

        return;
    }
}

void
Solv3DGLWidget::keyReleaseEvent(QKeyEvent* e)
{
    if ( e->isAutoRepeat() ) {
        return;
    }
    const int key = e->key();
    _held.erase(key);
    if ( (key == Qt::Key_D) && _pivotByD ) {
        _pivotMode = false;
        _pivotByD = false;
    }
    std::map<int, QPoint>::iterator it = _keyDown.find(key);
    if ( it != _keyDown.end() ) {
        const bool shift = it->second.x() != 0;
        _keyDown.erase(it);
        if (!_keyUsed) {
            int preset = -1;
            switch (key) {
            case Qt::Key_X: preset = shift ? 6 : 5; break;
            case Qt::Key_C: preset = shift ? 2 : 1; break;
            case Qt::Key_Z: preset = shift ? 4 : 3; break;
            case Qt::Key_V: preset = shift ? -1 : 0; break;
            default: break;
            }
            if (preset >= 0) {
                setPreset(preset);
                if (onPresetChanged) {
                    onPresetChanged(preset);
                }
            }
        }
        _keyUsed = false;
    }
    update();
}

// one undo step per manipulator drag (all the parameters the drag changed)
class Solv3DManipUndo
    : public UndoCommand
{
public:
    struct Entry
    {
        KnobDoubleWPtr k;
        std::vector<double> before, after;
    };

    Solv3DManipUndo(const std::vector<Entry>& e, const std::string& what)
        : UndoCommand(what)
        , _e(e)
        , _first(true)
    {}

    virtual void redo() OVERRIDE FINAL
    {
        if (_first) { // the drag already set the values
            _first = false;

            return;
        }
        for (const Entry& e : _e) {
            set(e, e.after);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        for (const Entry& e : _e) {
            set(e, e.before);
        }
    }

private:
    static void set(const Entry& e, const std::vector<double>& v)
    {
        KnobDoublePtr k = e.k.lock();
        if (!k) {
            return;
        }
        k->beginChanges();
        for (size_t d = 0; d < v.size(); ++d) {
            KeyFrame kf;
            k->setValue(v[d], ViewSpec::all(), (int)d, eValueChangedReasonUserEdited, &kf);
        }
        k->endChanges();
    }

    std::vector<Entry> _e;
    bool _first;
};

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
    h->addSpacing(8);
    {
        const char* names[4] = { "select", "move", "rotate", "scale" };
        const char* tips[4] = { "Select only (Q).",
                                "Move (W): arrows along an axis, squares in a plane, the centre box in the view plane. "
                                "Hold X grid / C edge / V point / Shift+V face centre to snap. Insert or hold D: edit the pivot.",
                                "Rotate (E): drag a ring, inside the ball for free rotation. Hold J for 15 degree steps.",
                                "Scale (R): handles along the object's axes, squares for two axes, the centre for uniform scale. "
                                "Y: last tool. Middle-drag drives the last used part. +/-: handle size." };
        for (int i = 0; i < 4; ++i) {
            _manipBtn[i] = new QToolButton(bar);
            _manipBtn[i]->setText( QString::fromUtf8(names[i]) );
            _manipBtn[i]->setToolTip( QString::fromUtf8(tips[i]) );
            _manipBtn[i]->setCheckable(true);
            _manipBtn[i]->setChecked(i == 1);
            _manipBtn[i]->setProperty("solvManip", i);
            QObject::connect( _manipBtn[i], SIGNAL(clicked()), this, SLOT(onManipButton()) );
            h->addWidget(_manipBtn[i]);
        }
        _spaceBtn = new QToolButton(bar);
        _spaceBtn->setText( QString::fromUtf8("L") );
        _spaceBtn->setToolTip( tr("Handle space: L local, G global, V view (` cycles). Scale always uses local.") );
        h->addWidget(_spaceBtn);
    }
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
    QObject::connect( _spaceBtn, &QToolButton::clicked, this, [this]() { _gl->cycleSpace(); } );
    _gl->onSpaceChanged = [this](int sp) {
        _spaceBtn->setText( QString::fromUtf8(sp == 0 ? "L" : sp == 1 ? "G" : "V") );
    };
    _gl->onManipModeChanged = [this](int m) {
        for (int i = 0; i < 4; ++i) {
            _manipBtn[i]->setChecked(i == m);
        }
    };
    _gl->onManipBegin = [this](Solv3DGLWidget::Xform* x) -> bool {
        NodePtr n = gizmoNode();
        if (!n) {
            return false;
        }
        auto get3 = [&](const char* name, Vec3* v) {
            KnobDoublePtr k = std::dynamic_pointer_cast<KnobDouble>( n->getKnobByName(name) );
            if (k) {
                *v = Vec3( k->getValue(0), k->getValue(1), k->getValue(2) );
            }
        };
        get3("translate", &x->translate);
        get3("rotate", &x->rotate);
        get3("scaling", &x->scale);
        get3("pivot", &x->pivot);
        KnobDoublePtr u = std::dynamic_pointer_cast<KnobDouble>( n->getKnobByName("uniform_scale") );
        x->uniform = u ? u->getValue() : 1.;
        KnobChoicePtr ro = std::dynamic_pointer_cast<KnobChoice>( n->getKnobByName("rot_order") );
        x->rotOrder = ro ? ro->getValue() : 0;
        _manipStart = *x;
        _manipWhat = 0;

        return true;
    };
    _gl->onManipEnd = [this]() {
        NodePtr n = gizmoNode();
        if ( !n || (_manipWhat == 0) ) {
            return;
        }
        NodeGuiIPtr ng = n->getNodeGui();
        if (!ng) {
            return;
        }
        std::vector<Solv3DManipUndo::Entry> entries;
        const char* names[5] = { "translate", "rotate", "scaling", "uniform_scale", "pivot" };
        const Vec3 starts[5] = { _manipStart.translate, _manipStart.rotate, _manipStart.scale, Vec3(), _manipStart.pivot };
        std::string label;
        for (int i = 0; i < 5; ++i) {
            if ( !( _manipWhat & (1 << i) ) ) {
                continue;
            }
            KnobDoublePtr k = std::dynamic_pointer_cast<KnobDouble>( n->getKnobByName(names[i]) );
            if (!k) {
                continue;
            }
            Solv3DManipUndo::Entry e;
            e.k = k;
            if (i == 3) {
                e.before.push_back(_manipStart.uniform);
                e.after.push_back( k->getValue() );
            } else {
                e.before.push_back(starts[i].x); e.before.push_back(starts[i].y); e.before.push_back(starts[i].z);
                for (int d = 0; d < 3; ++d) {
                    e.after.push_back( k->getValue(d) );
                }
            }
            entries.push_back(e);
            label = label.empty() ? std::string(names[i]) : label + " + " + names[i];
        }
        if ( !entries.empty() ) {
            ng->pushUndoCommand( UndoCommandPtr( new Solv3DManipUndo(entries, "move " + label) ) );
        }
        _manipWhat = 0;
    };
    _gl->onManipSet = [this](int what, const Vec3& v, double uniform) {
        NodePtr n = gizmoNode();
        if (!n) {
            return;
        }
        const char* name = (what == 1) ? "translate" : (what == 2) ? "rotate" : (what == 3) ? "scaling" : (what == 4) ? "uniform_scale" : "pivot";
        KnobDoublePtr k = std::dynamic_pointer_cast<KnobDouble>( n->getKnobByName(name) );
        if (!k) {
            return;
        }
        _manipWhat |= 1 << (what - 1);
        // user edit: keys the value when the parameter is animated (auto key)
        const double vals[3] = { v.x, v.y, v.z };
        const int dims = (what == 4) ? 1 : 3;
        k->beginChanges();
        for (int d = 0; d < dims; ++d) {
            KeyFrame kf;
            k->setValue( (what == 4) ? uniform : vals[d], ViewSpec::all(), d, eValueChangedReasonUserEdited, &kf );
        }
        k->endChanges();
    };
    _gl->onLeaveCamera = [this]() {
        _view->blockSignals(true);
        _view->setCurrentIndex(0);
        _view->blockSignals(false);
    };
    _timer = new QTimer(this);
    _timer->setInterval(40); // cheap check: the scene is rebuilt only when its inputs changed
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
    const NodesGuiList& selection = _gui->getSelectedNodes();
    ViewerInstance* vi = _viewer ? _viewer->getInternalNode() : 0;
    Solv3DNode* up = 0;
    if (vi) {
        int a = -1, b = -1;
        vi->getActiveInputs(a, b);
        EffectInstancePtr in = (a >= 0) ? vi->getInput(a) : EffectInstancePtr();
        up = upstream3D(in);
    }
    {
        // signature of everything the scene depends on (node hashes change with any upstream parameter / link)
        U64 sig = 1469598103934665603ULL;
        auto mix = [&sig](U64 v) { sig = (sig ^ v) * 1099511628211ULL; };
        U64 tb;
        std::memcpy(&tb, &t, 8);
        mix(tb);
        mix( (U64)(uintptr_t)up );
        if (up) {
            mix( up->getHash() );
        }
        for (NodesGuiList::const_iterator it = selection.begin(); it != selection.end(); ++it) {
            NodePtr node = (*it) ? (*it)->getNode() : NodePtr();
            EffectInstancePtr e = node ? node->getEffectInstance() : EffectInstancePtr();
            if ( e && Solv3DNode::as3D(e) ) {
                mix( (U64)(uintptr_t)e.get() );
                mix( e->getHash() );
            }
        }
        // safety net: rebuild at least every 2 s (textures / files changed on disk)
        if ( (sig == _lastSig) && (++_idleTicks < 50) ) {
            if ( _gl->texturePending() ) {
                _gl->update();
            }

            return;
        }
        _lastSig = sig;
        _idleTicks = 0;
    }
    _gl->setTime(t);
    Scene main, templ;
    if (up) {
        up->buildScene(t, &main);
    }
    std::set<std::string> sel;
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

    // manipulator on the last selected node that has a transform (its own prim in the scene)
    _gizmoNode.clear();
    bool gizmo = false;
    for (NodesGuiList::const_reverse_iterator it = selection.rbegin(); it != selection.rend() && !gizmo; ++it) {
        NodePtr node = (*it) ? (*it)->getNode() : NodePtr();
        EffectInstancePtr e = node ? node->getEffectInstance() : EffectInstancePtr();
        if ( !e || !Solv3DNode::as3D(e) || !node->getKnobByName("translate") ) {
            continue;
        }
        const std::string name = node->getFullyQualifiedName();
        const std::string own = Solv3DNode::as3D(e)->primPath();
        const Scene* scenes[2] = { &main, &templ };
        for (int si = 0; si < 2 && !gizmo; ++si) {
            for (const auto& kv : scenes[si]->prims()) {
                // the node's own prim (its path ends like the node's prim path: it can sit under a parent)
                const bool ownPrim = (kv.first == own) ||
                                     ( (kv.first.size() > own.size()) && (kv.first.compare(kv.first.size() - own.size(), own.size(), own) == 0) );
                if ( (kv.second.node != name) || !ownPrim || !kv.second.editable ) {
                    continue;
                }
                const std::string parent = parentPath(kv.first);
                const Mat4 pw = parent.empty() ? Mat4() : scenes[si]->worldMatrix(parent);
                Vec3 pivot;
                KnobDoublePtr pk = std::dynamic_pointer_cast<KnobDouble>( node->getKnobByName("pivot") );
                if (pk) {
                    pivot = Vec3( pk->getValue(0), pk->getValue(1), pk->getValue(2) );
                }
                _gl->setGizmo( true, pw, scenes[si]->worldMatrix(kv.first), pivot, name );
                _gizmoNode = name;
                gizmo = true;
                break;
            }
        }
    }
    if (!gizmo) {
        _gl->setGizmo( false, Mat4(), Mat4(), Vec3() );
    }
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
        EffectInstancePtr ce = node->getEffectInstance();
        const std::string own = ( ce && Solv3DNode::as3D(ce) ) ? Solv3DNode::as3D(ce)->primPath() : std::string();
        for (size_t i = 0; i < _cams.size(); ++i) {
            // the camera prim of a Camera node is its prim path (possibly under a parent)
            const std::string& c = _cams[i];
            if ( !own.empty() && (c.size() >= own.size()) && (c.compare(c.size() - own.size(), own.size(), own) == 0) ) {
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
Solv3DView::onManipButton()
{
    QToolButton* b = qobject_cast<QToolButton*>( sender() );
    const int m = b ? b->property("solvManip").toInt() : 1;
    for (int i = 0; i < 4; ++i) {
        _manipBtn[i]->setChecked(i == m);
    }
    _gl->setManipMode(m);
    _gl->setFocus();
}

NodePtr
Solv3DView::gizmoNode() const
{
    if ( _gizmoNode.empty() || !_gui || !_gui->getApp() ) {
        return NodePtr();
    }

    return _gui->getApp()->getNodeByFullySpecifiedName(_gizmoNode);
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
