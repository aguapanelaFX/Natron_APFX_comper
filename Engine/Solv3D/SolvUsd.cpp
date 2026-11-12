/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include "SolvUsd.h"

#include <cmath>
#include <cstring>
#include <mutex>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLibrary>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef near
#undef far
#endif

#include "../../tools/usdbridge/solvusd.h"

namespace Solv3D {

namespace {
typedef int (*FnVersion)(void);
typedef void* (*FnOpen)(const char*, char*, int);
typedef void* (*FnOpen2)(const char*, const char*, char*, int);
typedef int (*FnTree)(void*, char*, int);
typedef int (*FnDiag)(void*, char*, int);
typedef void (*FnInfo)(void*, double*, double*, double*, double*, int*);
typedef int (*FnEval)(void*, double);
typedef const SolvUsdPrim* (*FnPrim)(void*, int);
typedef void (*FnClose)(void*);
typedef void* (*FnWriteBegin)(const char*, double, double, double, double, char*, int);
typedef int (*FnWriteFrame)(void*, double, const SolvUsdPrim*, int);
typedef int (*FnWriteEnd)(void*, char*, int);

struct Bridge
{
    std::mutex m;               // USD calls are serialised (one stage cache)
    bool tried = false;
    bool ok = false;
    std::string why;
    QLibrary lib;
    FnVersion version = 0;
    FnOpen open = 0;
    FnOpen2 open2 = 0;
    FnTree tree = 0;
    FnInfo info = 0;
    FnDiag diag = 0;
    FnEval eval = 0;
    FnPrim prim = 0;
    FnClose close = 0;
    FnWriteBegin wBegin = 0;
    FnWriteFrame wFrame = 0;
    FnWriteEnd wEnd = 0;
    // last opened stage
    std::string stageFile;
    void* stage = 0;

    bool load()
    {
        if (tried) {
            return ok;
        }
        tried = true;
        QStringList dirs;
        if (QCoreApplication::instance()) {
            // the bridge ships in its own folder (solvusd + usd_ms + tbb + MSVC runtime + usd plugins)
            dirs << QCoreApplication::applicationDirPath() + QString::fromUtf8("/usdbridge");
            dirs << QCoreApplication::applicationDirPath();
        }
        for (const QString& d : dirs) {
#ifdef _WIN32
            // plain LoadLibrary does not search the bridge folder for usd_ms/tbb/CRT:
            // preload with the altered search path so its dependencies resolve next to it
            const QString full = QDir::toNativeSeparators( d + QString::fromUtf8("/solvusd.dll") );
            if ( QFile::exists(full) ) {
                HMODULE h = LoadLibraryExW( (LPCWSTR)full.utf16(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH );
                if (!h) {
                    why = "USD bridge failed to load (Windows error " + std::to_string( (unsigned long)GetLastError() ) + ")";
                }
            }
#endif
            lib.setFileName( d + QString::fromUtf8("/solvusd") );
            if ( lib.load() ) {
                break;
            }
        }
        if ( !lib.isLoaded() ) {
            lib.setFileName( QString::fromUtf8("solvusd") );
            lib.load();
        }
        if ( !lib.isLoaded() ) {
            if ( why.empty() ) {
                why = "USD bridge not installed (solvusd library missing): " + lib.errorString().toStdString();
            }

            return false;
        }
        version = (FnVersion)lib.resolve("solvusd_version");
        open = (FnOpen)lib.resolve("solvusd_open");
        open2 = (FnOpen2)lib.resolve("solvusd_open2");
        tree = (FnTree)lib.resolve("solvusd_tree");
        info = (FnInfo)lib.resolve("solvusd_info");
        diag = (FnDiag)lib.resolve("solvusd_diagnostics");
        eval = (FnEval)lib.resolve("solvusd_eval");
        prim = (FnPrim)lib.resolve("solvusd_prim");
        close = (FnClose)lib.resolve("solvusd_close");
        wBegin = (FnWriteBegin)lib.resolve("solvusd_write_begin");
        wFrame = (FnWriteFrame)lib.resolve("solvusd_write_frame");
        wEnd = (FnWriteEnd)lib.resolve("solvusd_write_end");
        if ( !version || !open || !open2 || !tree || !info || !eval || !prim || !close || !wBegin || !wFrame || !wEnd || (version() != SOLVUSD_VERSION) ) {
            why = "USD bridge version mismatch (app expects " + std::to_string(SOLVUSD_VERSION) + ")";

            return false;
        }
        ok = true;
        why.clear();

        return true;
    }

    std::string stageEdits;

    // edits null: any open stage of that file will do (time range, units, diagnostics)
    void* stageFor(const std::string& file, std::string* err, const std::string* editsPtr = nullptr)
    {
        if ( stage && (stageFile == file) && ( !editsPtr || (stageEdits == *editsPtr) ) ) {
            return stage;
        }
        const std::string edits = editsPtr ? *editsPtr : std::string();
        if (stage) {
            close(stage);
            stage = 0;
        }
        char e[1024] = { 0 };
        stage = open2(file.c_str(), edits.c_str(), e, (int)sizeof(e) );
        if (!stage) {
            *err = e[0] ? e : ("cannot open " + file);
            stageFile.clear();

            return 0;
        }
        stageFile = file;
        stageEdits = edits;

        return stage;
    }
};

Bridge&
bridge()
{
    static Bridge b;

    return b;
}

Mat4
fromLocal(const double* l)
{
    Mat4 m;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m.m[r][c] = l[r * 4 + c];
        }
    }

    return m;
}

void
toLocal(const Mat4& m, double* l)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            l[r * 4 + c] = m.m[r][c];
        }
    }
}
} // anon

bool
usdAvailable(std::string* why)
{
    Bridge& b = bridge();
    std::lock_guard<std::mutex> l(b.m);
    const bool ok = b.load();
    if (!ok && why) {
        *why = b.why;
    }

    return ok;
}

bool
usdTimeRange(const std::string& file, double* start, double* end, double* fps)
{
    Bridge& b = bridge();
    std::lock_guard<std::mutex> l(b.m);
    if ( !b.load() ) {
        return false;
    }
    std::string err;
    void* st = b.stageFor(file, &err);
    if (!st) {
        return false;
    }
    double mpu = 0.01;
    int zUp = 0;
    b.info(st, start, end, fps, &mpu, &zUp);

    return true;
}

bool
usdSceneTree(const std::string& file, const std::string& edits, std::string* out, std::string* error)
{
    Bridge& b = bridge();
    std::lock_guard<std::mutex> l(b.m);
    if ( !b.load() ) {
        *error = b.why;

        return false;
    }
    void* st = b.stageFor(file, error, &edits);
    if (!st) {
        return false;
    }
    const int n = b.tree(st, 0, 0);
    std::vector<char> buf( (size_t)std::max(1, n) + 1, 0 );
    b.tree( st, buf.data(), (int)buf.size() );
    *out = buf.data();

    return true;
}

std::string
usdDiagnostics(const std::string& file)
{
    Bridge& b = bridge();
    std::lock_guard<std::mutex> l(b.m);
    if ( !b.load() || !b.diag ) {
        return std::string();
    }
    std::string err;
    void* st = b.stageFor(file, &err);
    if (!st) {
        return err;
    }
    char buf[2048] = { 0 };
    b.diag(st, buf, (int)sizeof(buf) );

    return std::string(buf);
}

bool
usdStageUnits(const std::string& file, double* metersPerUnit, bool* zUp, std::string* error)
{
    Bridge& b = bridge();
    std::lock_guard<std::mutex> l(b.m);
    if ( !b.load() ) {
        *error = b.why;

        return false;
    }
    void* st = b.stageFor(file, error);
    if (!st) {
        return false;
    }
    double s0 = 0, s1 = 0, fps = 24, mpu = 0.01;
    int z = 0;
    b.info(st, &s0, &s1, &fps, &mpu, &z);
    *metersPerUnit = mpu;
    *zUp = z != 0;

    return true;
}

bool
readUsd(const std::string& file, double timeCode, const std::string& root, Scene* scene, std::string* error, bool keepUnits,
        const std::string& edits)
{
    Bridge& b = bridge();
    std::lock_guard<std::mutex> l(b.m);
    if ( !b.load() ) {
        *error = b.why;

        return false;
    }
    void* st = b.stageFor(file, error, &edits);
    if (!st) {
        return false;
    }
    double s0 = 0, s1 = 0, fps = 24, mpu = 0.01;
    int zUp = 0;
    b.info(st, &s0, &s1, &fps, &mpu, &zUp);
    // to SOLVFX scale (cm) and Y up, applied on the top level prims
    const double k = keepUnits ? 1. : ( (mpu > 0.) ? mpu / 0.01 : 1. );
    Mat4 unit = Mat4::scale( Vec3(k, k, k) );
    if (zUp) {
        unit = Mat4::rotateXYZ( Vec3(-90, 0, 0) ) * unit;
    }
    const bool convert = !keepUnits && ( (std::fabs(k - 1.) > 1e-9) || zUp );
    const int n = b.eval(st, timeCode);
    for (int i = 0; i < n; ++i) {
        const SolvUsdPrim* p = b.prim(st, i);
        if (!p || !p->path) {
            continue;
        }
        const std::string path = root + p->path;
        PrimType ty = PrimType::Xform;
        if (p->type == SOLVUSD_MESH) {
            ty = PrimType::Mesh;
        } else if (p->type == SOLVUSD_CAMERA) {
            ty = PrimType::Camera;
        } else if (p->type == SOLVUSD_LIGHT) {
            ty = PrimType::Light;
        }
        Prim& pr = scene->define(path, ty);
        pr.type = ty;
        pr.visible = p->visible != 0;
        if (p->kind && *p->kind) {
            pr.attributes["kind"] = p->kind;
        }
        if (p->purpose && *p->purpose) {
            pr.attributes["purpose"] = p->purpose;
        }
        if (p->typeName && *p->typeName) {
            pr.attributes["typeName"] = p->typeName;
        }
        pr.local = fromLocal(p->local);
        const bool top = std::strchr(p->path + 1, '/') == 0;
        if (top && convert) {
            pr.local = unit * pr.local;
        }
        if ( (p->type == SOLVUSD_MESH) && (p->nPoints > 0) ) {
            std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
            m->points.resize(p->nPoints);
            for (int q = 0; q < p->nPoints; ++q) {
                m->points[q] = Vec3(p->points[q * 3], p->points[q * 3 + 1], p->points[q * 3 + 2]);
            }
            m->faceVertexCounts.assign(p->faceCounts, p->faceCounts + p->nFaces);
            m->faceVertexIndices.assign(p->indices, p->indices + p->nIndices);
            if (p->normals) {
                m->normals.resize(p->nIndices);
                for (int q = 0; q < p->nIndices; ++q) {
                    m->normals[q] = Vec3(p->normals[q * 3], p->normals[q * 3 + 1], p->normals[q * 3 + 2]);
                }
            } else {
                m->computeFlatNormals();
            }
            if (p->uvs) {
                m->st.resize(p->nIndices);
                for (int q = 0; q < p->nIndices; ++q) {
                    m->st[q] = Vec2{ p->uvs[q * 2], p->uvs[q * 2 + 1] };
                }
            }
            m->displayColor = Vec3(p->color[0], p->color[1], p->color[2]);
            m->material.baseColor = m->displayColor;
            m->material.metalness = p->metallic;
            m->material.roughness = p->roughness;
            m->material.ior = p->ior;
            m->material.specular = p->metallic > 0.f ? 0. : 0.5 * (1. - p->roughness);
            m->material.emission = 0.;
            m->material.opacity = p->opacity;
            if (p->diffuseTexture && *p->diffuseTexture) {
                m->textureFile = p->diffuseTexture;
            }
            pr.mesh = m;
        } else if (p->type == SOLVUSD_CAMERA) {
            std::shared_ptr<Camera> c = std::make_shared<Camera>();
            c->focalLength = p->focalLength;
            c->horizontalAperture = p->hAperture;
            c->verticalAperture = p->vAperture;
            c->horizontalApertureOffset = p->hOffset;
            c->verticalApertureOffset = p->vOffset;
            c->nearClip = p->nearClip * k;
            c->farClip = p->farClip * k;
            c->projectionMode = p->orthographic ? 1 : 0;
            if (p->focusDistance > 0.) {
                c->focusDistance = p->focusDistance * k;
            }
            if (p->fStop > 0.) {
                c->fStop = p->fStop;
            }
            pr.camera = c;
        } else if (p->type == SOLVUSD_LIGHT) {
            std::shared_ptr<Light> L = std::make_shared<Light>();
            L->kind = (p->lightType == SOLVUSD_SPOT) ? "spot" : (p->lightType == SOLVUSD_DISTANT) ? "distant" : (p->lightType == SOLVUSD_DOME) ? "dome" : "point";
            L->color = Vec3(p->lightColor[0], p->lightColor[1], p->lightColor[2]);
            L->intensity = p->intensity * std::pow(2., (double)p->exposure);
            L->falloff = (p->lightType == SOLVUSD_POINT || p->lightType == SOLVUSD_SPOT) ? 2 : 0;
            L->coneAngle = p->coneAngle * 2.; // USD: half angle
            L->conePenumbra = p->coneAngle * 2. * p->coneSoftness;
            L->castShadows = false;           // shadows are switched on in SOLVFX (off by default)
            L->shadowSoftness = (p->lightType == SOLVUSD_DISTANT) ? p->angle : p->radius * k;
            pr.light = L;
        }
    }

    return true;
}

bool
writeUsd(const std::string& file, const std::vector<std::pair<double, const Scene*> >& frames, double fps, std::string* error)
{
    Bridge& b = bridge();
    std::lock_guard<std::mutex> l(b.m);
    if ( !b.load() ) {
        *error = b.why;

        return false;
    }
    if ( frames.empty() ) {
        *error = "nothing to write";

        return false;
    }
    char e[1024] = { 0 };
    void* w = b.wBegin(file.c_str(), frames.front().first, frames.back().first, fps, 0.01, e, (int)sizeof(e) );
    if (!w) {
        *error = e;

        return false;
    }
    for (const std::pair<double, const Scene*>& fr : frames) {
        std::vector<SolvUsdPrim> prims;
        std::vector<std::vector<float> > fb;
        std::vector<std::vector<int> > ib;
        fb.reserve(fr.second->prims().size() * 3);
        ib.reserve(fr.second->prims().size() * 2);
        for (const auto& kv : fr.second->prims()) {
            const Prim& p = kv.second;
            SolvUsdPrim u;
            std::memset( &u, 0, sizeof(u) );
            u.path = kv.first.c_str();
            u.visible = p.visible ? 1 : 0;
            toLocal(p.local, u.local);
            u.diffuseTexture = "";
            u.type = SOLVUSD_XFORM;
            auto attr = [&p](const char* k) -> const char* {
                std::map<std::string, std::string>::const_iterator it = p.attributes.find(k);

                return it == p.attributes.end() ? "" : it->second.c_str();
            };
            u.kind = attr("kind");
            u.purpose = attr("purpose");
            u.typeName = attr("typeName");
            if ( (p.type == PrimType::Mesh) && p.mesh ) {
                const Mesh& m = *p.mesh;
                u.type = SOLVUSD_MESH;
                fb.push_back( std::vector<float>() );
                for (const Vec3& v : m.points) {
                    fb.back().push_back( (float)v.x ); fb.back().push_back( (float)v.y ); fb.back().push_back( (float)v.z );
                }
                u.nPoints = (int)m.points.size();
                u.points = fb.back().data();
                ib.push_back(m.faceVertexCounts);
                u.nFaces = (int)m.faceVertexCounts.size();
                u.faceCounts = ib.back().data();
                ib.push_back(m.faceVertexIndices);
                u.nIndices = (int)m.faceVertexIndices.size();
                u.indices = ib.back().data();
                if ( m.normals.size() == m.faceVertexIndices.size() ) {
                    fb.push_back( std::vector<float>() );
                    for (const Vec3& v : m.normals) {
                        fb.back().push_back( (float)v.x ); fb.back().push_back( (float)v.y ); fb.back().push_back( (float)v.z );
                    }
                    u.normals = fb.back().data();
                }
                if ( m.st.size() == m.faceVertexIndices.size() ) {
                    fb.push_back( std::vector<float>() );
                    for (const Vec2& v : m.st) {
                        fb.back().push_back( (float)v.x ); fb.back().push_back( (float)v.y );
                    }
                    u.uvs = fb.back().data();
                }
                u.color[0] = (float)m.material.baseColor.x; u.color[1] = (float)m.material.baseColor.y; u.color[2] = (float)m.material.baseColor.z;
                u.opacity = (float)m.material.opacity;
                u.metallic = (float)m.material.metalness;
                u.roughness = (float)m.material.roughness;
                u.ior = (float)m.material.ior;
                u.specular = (float)m.material.specular;
                const float em = (float)m.material.emission;
                u.emissive[0] = u.color[0] * em; u.emissive[1] = u.color[1] * em; u.emissive[2] = u.color[2] * em;
            } else if ( (p.type == PrimType::Camera) && p.camera ) {
                const Camera& c = *p.camera;
                u.type = SOLVUSD_CAMERA;
                u.focalLength = c.focalLength;
                u.hAperture = c.horizontalAperture;
                u.vAperture = c.verticalAperture;
                u.hOffset = c.horizontalApertureOffset;
                u.vOffset = c.verticalApertureOffset;
                u.nearClip = c.nearClip;
                u.farClip = c.farClip;
                u.focusDistance = c.focusDistance;
                u.fStop = c.fStop;
                u.orthographic = c.projectionMode == 1;
            } else if ( (p.type == PrimType::Light) && p.light ) {
                const Light& L = *p.light;
                u.type = SOLVUSD_LIGHT;
                u.lightType = (L.kind == "spot") ? SOLVUSD_SPOT : (L.kind == "distant") ? SOLVUSD_DISTANT : (L.kind == "ambient" || L.kind == "dome") ? SOLVUSD_DOME : SOLVUSD_POINT;
                u.lightColor[0] = (float)L.color.x; u.lightColor[1] = (float)L.color.y; u.lightColor[2] = (float)L.color.z;
                u.intensity = (float)L.intensity;
                u.radius = (float)L.shadowSoftness;
                u.angle = (float)L.shadowSoftness;
                u.coneAngle = (float)(L.coneAngle * 0.5);
                u.coneSoftness = (float)( L.coneAngle > 0. ? L.conePenumbra / L.coneAngle : 0. );
                u.shadows = L.castShadows ? 1 : 0;
            }
            prims.push_back(u);
        }
        b.wFrame(w, fr.first, prims.data(), (int)prims.size() );
    }
    if ( !b.wEnd(w, e, (int)sizeof(e) ) ) {
        *error = e;

        return false;
    }

    return true;
}

} // namespace Solv3D
