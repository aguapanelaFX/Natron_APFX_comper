/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX USD bridge (see solvusd.h). Built against OpenUSD (monolithic, no imaging / python).

#define SOLVUSD_BUILD
#include "solvusd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <pxr/pxr.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdUtils/dependencies.h>
#include <pxr/base/plug/registry.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// USD's file formats are plugins: register the "usd" folder next to this library (the bridge ships
// usd_ms + its plugInfo files in its own folder)
void
registerPlugins()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    std::string dir;
#ifdef _WIN32
    HMODULE h = 0;
    if ( GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&registerPlugins, &h) ) {
        char buf[MAX_PATH * 2] = { 0 };
        GetModuleFileNameA(h, buf, (DWORD)sizeof(buf) );
        dir = buf;
    }
#else
    Dl_info info;
    if ( dladdr( (void*)&registerPlugins, &info ) && info.dli_fname ) {
        dir = info.dli_fname;
    }
#endif
    const size_t slash = dir.find_last_of("/\\");
    if (slash != std::string::npos) {
        dir = dir.substr(0, slash);
        PlugRegistry::GetInstance().RegisterPlugins(dir + "/usd/");
    }
}

void
setErr(char* err, int len, const std::string& s)
{
    if (err && (len > 0)) {
        std::snprintf(err, (size_t)len, "%s", s.c_str());
    }
}

// GfMatrix4d (row vectors, p' = p * m) -> row-major column-vector (p' = M * p): transpose
void
toLocal(const GfMatrix4d& m, double* out)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = m[c][r];
        }
    }
}

GfMatrix4d
fromLocal(const double* in)
{
    GfMatrix4d m;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[c][r] = in[r * 4 + c];
        }
    }

    return m;
}

template <class T>
T
getOr(const UsdAttribute& a, UsdTimeCode t, T def)
{
    T v = def;
    if (a) {
        a.Get(&v, t);
    }

    return v;
}

struct Stage
{
    UsdStageRefPtr stage;
    std::vector<SolvUsdPrim> prims;
    std::deque<std::string> strings;
    std::deque<std::vector<float> > fbufs;
    std::deque<std::vector<int> > ibufs;

    const char* keep(const std::string& s)
    {
        strings.push_back(s);

        return strings.back().c_str();
    }

    void clear()
    {
        prims.clear();
        strings.clear();
        fbufs.clear();
        ibufs.clear();
    }
};

// per face-vertex values from a primvar of any interpolation
template <class V, int N>
bool
faceVarying(const VtArray<V>& vals, const TfToken& interp, const VtIntArray& indices, const VtIntArray& counts,
            std::vector<float>* out)
{
    out->clear();
    const size_t nfv = indices.size();
    out->reserve(nfv * N);
    auto push = [&](const V& v) {
        for (int k = 0; k < N; ++k) {
            out->push_back( (float)v[k] );
        }
    };
    if ( (interp == UsdGeomTokens->faceVarying) && (vals.size() == nfv) ) {
        for (size_t i = 0; i < nfv; ++i) {
            push(vals[i]);
        }
    } else if ( (interp == UsdGeomTokens->vertex) || (interp == UsdGeomTokens->varying) ) {
        for (size_t i = 0; i < nfv; ++i) {
            const int vi = indices[i];
            if ( (vi < 0) || ( (size_t)vi >= vals.size() ) ) {
                return false;
            }
            push(vals[vi]);
        }
    } else if (interp == UsdGeomTokens->uniform) {
        size_t base = 0;
        for (size_t f = 0; f < counts.size(); ++f) {
            for (int k = 0; k < counts[f]; ++k) {
                if ( f >= vals.size() ) {
                    return false;
                }
                push(vals[f]);
            }
            base += counts[f];
        }
    } else if ( (interp == UsdGeomTokens->constant) && !vals.empty() ) {
        for (size_t i = 0; i < nfv; ++i) {
            push(vals[0]);
        }
    } else {
        return false;
    }

    return out->size() == nfv * N;
}

void
readMaterial(const UsdPrim& prim, UsdTimeCode t, Stage* s, SolvUsdPrim* p)
{
    p->color[0] = p->color[1] = p->color[2] = 0.18f;
    p->opacity = 1.f;
    p->metallic = 0.f;
    p->roughness = 0.5f;
    p->ior = 1.5f;
    p->specular = 0.5f;
    p->emissive[0] = p->emissive[1] = p->emissive[2] = 0.f;
    p->diffuseTexture = "";
    // display colour first (overridden by a bound UsdPreviewSurface)
    UsdGeomGprim gp(prim);
    if (gp) {
        VtVec3fArray dc;
        if ( gp.GetDisplayColorPrimvar().ComputeFlattened(&dc, t) && !dc.empty() ) {
            p->color[0] = dc[0][0]; p->color[1] = dc[0][1]; p->color[2] = dc[0][2];
        }
    }
    UsdShadeMaterial mat = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
    if (!mat) {
        return;
    }
    UsdShadeShader sh = mat.ComputeSurfaceSource();
    if (!sh) {
        return;
    }
    TfToken id;
    sh.GetShaderId(&id);
    if ( id != TfToken("UsdPreviewSurface") ) {
        return;
    }
    auto inputValue3 = [&](const char* name, float* out3) {
        UsdShadeInput in = sh.GetInput( TfToken(name) );
        if (!in) {
            return;
        }
        UsdShadeAttributeVector attrs = in.GetValueProducingAttributes();
        for (const UsdAttribute& a : attrs) {
            GfVec3f v;
            if ( a.Get(&v, t) ) {
                out3[0] = v[0]; out3[1] = v[1]; out3[2] = v[2];

                return;
            }
        }
    };
    auto inputValue1 = [&](const char* name, float* out) {
        UsdShadeInput in = sh.GetInput( TfToken(name) );
        if (!in) {
            return;
        }
        UsdShadeAttributeVector attrs = in.GetValueProducingAttributes();
        for (const UsdAttribute& a : attrs) {
            float v;
            if ( a.Get(&v, t) ) {
                *out = v;

                return;
            }
        }
    };
    inputValue3("diffuseColor", p->color);
    inputValue3("emissiveColor", p->emissive);
    inputValue1("opacity", &p->opacity);
    inputValue1("metallic", &p->metallic);
    inputValue1("roughness", &p->roughness);
    inputValue1("ior", &p->ior);
    // diffuse texture: the UsdUVTexture connected to diffuseColor
    UsdShadeInput dIn = sh.GetInput( TfToken("diffuseColor") );
    if (dIn) {
        UsdShadeConnectableAPI src;
        TfToken srcName;
        UsdShadeAttributeType srcType;
        if ( dIn.GetConnectedSource(&src, &srcName, &srcType) ) {
            UsdShadeShader tex(src.GetPrim());
            UsdShadeInput fileIn = tex ? tex.GetInput( TfToken("file") ) : UsdShadeInput();
            SdfAssetPath ap;
            if ( fileIn && fileIn.Get(&ap, t) ) {
                const std::string r = ap.GetResolvedPath().empty() ? ap.GetAssetPath() : ap.GetResolvedPath();
                p->diffuseTexture = s->keep(r);
                p->color[0] = p->color[1] = p->color[2] = 1.f; // the texture carries the colour
            }
        }
    }
}
} // anon

extern "C" {

SOLVUSD_API int
solvusd_version(void)
{
    return SOLVUSD_VERSION;
}

SOLVUSD_API void*
solvusd_open(const char* file, char* err, int errLen)
{
    try {
        registerPlugins();
        UsdStageRefPtr st = UsdStage::Open( std::string(file) );
        if (!st) {
            setErr(err, errLen, std::string("cannot open ") + file);

            return 0;
        }
        Stage* s = new Stage;
        s->stage = st;

        return s;
    } catch (const std::exception& e) {
        setErr(err, errLen, e.what() );

        return 0;
    }
}

SOLVUSD_API void
solvusd_info(void* h, double* start, double* end, double* fps, double* metersPerUnit, int* zUp)
{
    Stage* s = static_cast<Stage*>(h);
    if (!s) {
        return;
    }
    if (start) { *start = s->stage->GetStartTimeCode(); }
    if (end) { *end = s->stage->GetEndTimeCode(); }
    if (fps) { *fps = s->stage->GetTimeCodesPerSecond(); }
    if (metersPerUnit) { *metersPerUnit = UsdGeomGetStageMetersPerUnit(s->stage); }
    if (zUp) { *zUp = UsdGeomGetStageUpAxis(s->stage) == UsdGeomTokens->z ? 1 : 0; }
}

SOLVUSD_API int
solvusd_eval(void* h, double timeCode)
{
    Stage* s = static_cast<Stage*>(h);
    if (!s) {
        return 0;
    }
    s->clear();
    const UsdTimeCode t(timeCode);
    for ( UsdPrim prim : UsdPrimRange::Stage( s->stage, UsdTraverseInstanceProxies() ) ) {
        if ( prim.GetPath() == SdfPath::AbsoluteRootPath() ) {
            continue;
        }
        const bool imageable = prim.IsA<UsdGeomImageable>();
        if (!imageable) {
            continue; // materials, shaders, subsets ...
        }
        SolvUsdPrim p;
        std::memset( &p, 0, sizeof(p) );
        p.path = s->keep( prim.GetPath().GetString() );
        p.type = SOLVUSD_XFORM;
        p.visible = UsdGeomImageable(prim).ComputeVisibility(t) != UsdGeomTokens->invisible;
        GfMatrix4d m(1.);
        UsdGeomXformable xf(prim);
        if (xf) {
            bool reset = false;
            xf.GetLocalTransformation(&m, &reset, t);
        }
        toLocal(m, p.local);
        p.diffuseTexture = "";

        if ( prim.IsA<UsdGeomMesh>() ) {
            UsdGeomMesh mesh(prim);
            VtVec3fArray pts;
            VtIntArray counts, idx;
            mesh.GetPointsAttr().Get(&pts, t);
            mesh.GetFaceVertexCountsAttr().Get(&counts, t);
            mesh.GetFaceVertexIndicesAttr().Get(&idx, t);
            if ( pts.empty() || counts.empty() || idx.empty() ) {
                s->prims.push_back(p);
                continue;
            }
            const bool leftHanded = getOr<TfToken>(mesh.GetOrientationAttr(), t, UsdGeomTokens->rightHanded) == UsdGeomTokens->leftHanded;
            s->fbufs.push_back( std::vector<float>() );
            std::vector<float>& P = s->fbufs.back();
            P.reserve(pts.size() * 3);
            for (const GfVec3f& v : pts) {
                P.push_back(v[0]); P.push_back(v[1]); P.push_back(v[2]);
            }
            s->ibufs.push_back( std::vector<int>( counts.begin(), counts.end() ) );
            std::vector<int>& C = s->ibufs.back();
            s->ibufs.push_back( std::vector<int>( idx.begin(), idx.end() ) );
            std::vector<int>& I = s->ibufs.back();
            // normals
            std::vector<float> N;
            {
                VtVec3fArray nv;
                TfToken interp;
                UsdGeomPrimvar np = UsdGeomPrimvarsAPI(prim).GetPrimvar( TfToken("normals") );
                if ( np && np.ComputeFlattened(&nv, t) ) {
                    interp = np.GetInterpolation();
                } else if ( mesh.GetNormalsAttr().Get(&nv, t) ) {
                    interp = mesh.GetNormalsInterpolation();
                }
                if ( !nv.empty() && !faceVarying<GfVec3f, 3>(nv, interp, idx, counts, &N) ) {
                    N.clear();
                }
            }
            // uv: primvars:st (or st0 / UVMap / uv)
            std::vector<float> UV;
            {
                const char* names[4] = { "st", "st0", "UVMap", "uv" };
                for (int i = 0; i < 4 && UV.empty(); ++i) {
                    UsdGeomPrimvar up = UsdGeomPrimvarsAPI(prim).GetPrimvar( TfToken(names[i]) );
                    VtVec2fArray uv;
                    if ( up && up.ComputeFlattened(&uv, t) && !uv.empty() ) {
                        if ( !faceVarying<GfVec2f, 2>(uv, up.GetInterpolation(), idx, counts, &UV) ) {
                            UV.clear();
                        }
                    }
                }
            }
            if (leftHanded) {
                // reverse every face (indices + per face-vertex data)
                size_t base = 0;
                for (int c : C) {
                    std::reverse(I.begin() + base, I.begin() + base + c);
                    for (int k = 0; k < c / 2; ++k) {
                        const size_t a = base + k, b = base + c - 1 - k;
                        if ( !N.empty() ) {
                            for (int q = 0; q < 3; ++q) {
                                std::swap(N[a * 3 + q], N[b * 3 + q]);
                            }
                        }
                        if ( !UV.empty() ) {
                            for (int q = 0; q < 2; ++q) {
                                std::swap(UV[a * 2 + q], UV[b * 2 + q]);
                            }
                        }
                    }
                    base += c;
                }
            }
            p.type = SOLVUSD_MESH;
            p.nPoints = (int)pts.size();
            p.points = P.data();
            p.nFaces = (int)C.size();
            p.faceCounts = C.data();
            p.nIndices = (int)I.size();
            p.indices = I.data();
            if ( !N.empty() ) {
                s->fbufs.push_back(N);
                p.normals = s->fbufs.back().data();
            }
            if ( !UV.empty() ) {
                s->fbufs.push_back(UV);
                p.uvs = s->fbufs.back().data();
            }
            readMaterial(prim, t, s, &p);
        } else if ( prim.IsA<UsdGeomCamera>() ) {
            UsdGeomCamera cam(prim);
            p.type = SOLVUSD_CAMERA;
            p.focalLength = getOr<float>(cam.GetFocalLengthAttr(), t, 50.f);
            p.hAperture = getOr<float>(cam.GetHorizontalApertureAttr(), t, 20.955f);
            p.vAperture = getOr<float>(cam.GetVerticalApertureAttr(), t, 15.2908f);
            p.hOffset = getOr<float>(cam.GetHorizontalApertureOffsetAttr(), t, 0.f);
            p.vOffset = getOr<float>(cam.GetVerticalApertureOffsetAttr(), t, 0.f);
            const GfVec2f cr = getOr<GfVec2f>( cam.GetClippingRangeAttr(), t, GfVec2f(1.f, 1000000.f) );
            p.nearClip = cr[0];
            p.farClip = cr[1];
            p.focusDistance = getOr<float>(cam.GetFocusDistanceAttr(), t, 0.f);
            p.fStop = getOr<float>(cam.GetFStopAttr(), t, 0.f);
            p.orthographic = getOr<TfToken>(cam.GetProjectionAttr(), t, UsdGeomTokens->perspective) == UsdGeomTokens->orthographic;
        } else if ( prim.HasAPI<UsdLuxLightAPI>() ) {
            UsdLuxLightAPI L(prim);
            p.type = SOLVUSD_LIGHT;
            const GfVec3f c = getOr<GfVec3f>( L.GetColorAttr(), t, GfVec3f(1.f) );
            p.lightColor[0] = c[0]; p.lightColor[1] = c[1]; p.lightColor[2] = c[2];
            p.intensity = getOr<float>(L.GetIntensityAttr(), t, 1.f);
            p.exposure = getOr<float>(L.GetExposureAttr(), t, 0.f);
            p.lightType = SOLVUSD_POINT;
            if ( prim.IsA<UsdLuxDistantLight>() ) {
                p.lightType = SOLVUSD_DISTANT;
                p.angle = getOr<float>(UsdLuxDistantLight(prim).GetAngleAttr(), t, 0.53f);
            } else if ( prim.IsA<UsdLuxDomeLight>() ) {
                p.lightType = SOLVUSD_DOME;
            } else if ( prim.IsA<UsdLuxSphereLight>() ) {
                p.radius = getOr<float>(UsdLuxSphereLight(prim).GetRadiusAttr(), t, 0.5f);
            } else if ( prim.IsA<UsdLuxDiskLight>() ) {
                p.radius = getOr<float>(UsdLuxDiskLight(prim).GetRadiusAttr(), t, 0.5f);
            } else if ( prim.IsA<UsdLuxRectLight>() ) {
                p.radius = 0.5f * getOr<float>(UsdLuxRectLight(prim).GetWidthAttr(), t, 1.f);
            }
            if ( prim.HasAPI<UsdLuxShapingAPI>() ) {
                UsdLuxShapingAPI sp(prim);
                const float cone = getOr<float>(sp.GetShapingConeAngleAttr(), t, 90.f);
                if (cone < 89.9f) {
                    p.lightType = SOLVUSD_SPOT;
                    p.coneAngle = cone;
                    p.coneSoftness = getOr<float>(sp.GetShapingConeSoftnessAttr(), t, 0.f);
                }
            }
            p.shadows = prim.HasAPI<UsdLuxShadowAPI>() ? (getOr<bool>(UsdLuxShadowAPI(prim).GetShadowEnableAttr(), t, true) ? 1 : 0) : 1;
        }
        s->prims.push_back(p);
    }

    return (int)s->prims.size();
}

SOLVUSD_API const SolvUsdPrim*
solvusd_prim(void* h, int i)
{
    Stage* s = static_cast<Stage*>(h);
    if ( !s || (i < 0) || ( i >= (int)s->prims.size() ) ) {
        return 0;
    }

    return &s->prims[i];
}

SOLVUSD_API void
solvusd_close(void* h)
{
    delete static_cast<Stage*>(h);
}

// ------------------------------------------------------------------------------------------- writing
namespace {
struct Writer
{
    UsdStageRefPtr stage;
    std::string file, writePath;
    bool usdz = false;
    bool animated = false;
    std::set<std::string> defined;
    std::map<std::string, std::string> looks;
};

std::string
lower(std::string s)
{
    std::transform( s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); } );

    return s;
}
} // anon

SOLVUSD_API void*
solvusd_write_begin(const char* file, double start, double end, double fps, double metersPerUnit, char* err, int errLen)
{
    try {
        registerPlugins();
        Writer* w = new Writer;
        w->file = file;
        const std::string lf = lower(w->file);
        w->usdz = (lf.size() > 5) && (lf.compare(lf.size() - 5, 5, ".usdz") == 0);
        w->writePath = w->usdz ? w->file.substr(0, w->file.size() - 5) + "_tmp.usdc" : w->file;
        w->stage = UsdStage::CreateNew(w->writePath);
        if (!w->stage) {
            setErr(err, errLen, "cannot create " + w->writePath);
            delete w;

            return 0;
        }
        UsdGeomSetStageUpAxis(w->stage, UsdGeomTokens->y);
        UsdGeomSetStageMetersPerUnit(w->stage, metersPerUnit);
        w->animated = end > start;
        if (w->animated) {
            w->stage->SetStartTimeCode(start);
            w->stage->SetEndTimeCode(end);
        }
        w->stage->SetTimeCodesPerSecond(fps);
        w->stage->SetFramesPerSecond(fps);

        return w;
    } catch (const std::exception& e) {
        setErr(err, errLen, e.what() );

        return 0;
    }
}

SOLVUSD_API int
solvusd_write_frame(void* h, double timeCode, const SolvUsdPrim* prims, int n)
{
    Writer* w = static_cast<Writer*>(h);
    if (!w) {
        return 0;
    }
    const UsdTimeCode t = w->animated ? UsdTimeCode(timeCode) : UsdTimeCode::Default();
    for (int i = 0; i < n; ++i) {
        const SolvUsdPrim& p = prims[i];
        const SdfPath path(p.path);
        const bool first = w->defined.insert(p.path).second;
        UsdPrim prim;
        switch (p.type) {
        case SOLVUSD_MESH: prim = UsdGeomMesh::Define(w->stage, path).GetPrim(); break;
        case SOLVUSD_CAMERA: prim = UsdGeomCamera::Define(w->stage, path).GetPrim(); break;
        case SOLVUSD_LIGHT:
            if (p.lightType == SOLVUSD_DISTANT) {
                prim = UsdLuxDistantLight::Define(w->stage, path).GetPrim();
            } else if (p.lightType == SOLVUSD_DOME) {
                prim = UsdLuxDomeLight::Define(w->stage, path).GetPrim();
            } else {
                prim = UsdLuxSphereLight::Define(w->stage, path).GetPrim();
            }
            break;
        default: prim = UsdGeomXform::Define(w->stage, path).GetPrim(); break;
        }
        if (!prim) {
            continue;
        }
        // transform: one matrix op
        UsdGeomXformable xf(prim);
        if (xf) {
            UsdGeomXformOp op;
            if (first) {
                op = xf.MakeMatrixXform();
            } else {
                op = UsdGeomXformOp( prim.GetAttribute( TfToken("xformOp:transform") ) );
            }
            if (op) {
                op.Set(fromLocal(p.local), t);
            }
            UsdGeomImageable(prim).GetVisibilityAttr().Set(p.visible ? UsdGeomTokens->inherited : UsdGeomTokens->invisible, t);
        }
        if (p.type == SOLVUSD_MESH) {
            UsdGeomMesh mesh(prim);
            VtVec3fArray pts(p.nPoints);
            for (int k = 0; k < p.nPoints; ++k) {
                pts[k] = GfVec3f(p.points[k * 3], p.points[k * 3 + 1], p.points[k * 3 + 2]);
            }
            mesh.GetPointsAttr().Set(pts, t);
            if (first) {
                mesh.GetFaceVertexCountsAttr().Set( VtIntArray(p.faceCounts, p.faceCounts + p.nFaces) );
                mesh.GetFaceVertexIndicesAttr().Set( VtIntArray(p.indices, p.indices + p.nIndices) );
                mesh.GetSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
                if (p.uvs) {
                    VtVec2fArray uv(p.nIndices);
                    for (int k = 0; k < p.nIndices; ++k) {
                        uv[k] = GfVec2f(p.uvs[k * 2], p.uvs[k * 2 + 1]);
                    }
                    UsdGeomPrimvar st = UsdGeomPrimvarsAPI(prim).CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->faceVarying);
                    st.Set(uv);
                }
                // material: UsdPreviewSurface with the values
                const std::string lookPath = "/Looks/" + path.GetName() + "_" + std::to_string( w->looks.size() );
                UsdShadeMaterial mat = UsdShadeMaterial::Define( w->stage, SdfPath(lookPath) );
                UsdShadeShader sh = UsdShadeShader::Define( w->stage, SdfPath(lookPath + "/surface") );
                sh.CreateIdAttr( VtValue( TfToken("UsdPreviewSurface") ) );
                sh.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set( GfVec3f(p.color[0], p.color[1], p.color[2]) );
                sh.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f).Set( GfVec3f(p.emissive[0], p.emissive[1], p.emissive[2]) );
                sh.CreateInput(TfToken("metallic"), SdfValueTypeNames->Float).Set(p.metallic);
                sh.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(p.roughness);
                sh.CreateInput(TfToken("ior"), SdfValueTypeNames->Float).Set(p.ior);
                sh.CreateInput(TfToken("opacity"), SdfValueTypeNames->Float).Set(p.opacity);
                mat.CreateSurfaceOutput().ConnectToSource( sh.ConnectableAPI(), TfToken("surface") );
                UsdShadeMaterialBindingAPI::Apply(prim).Bind(mat);
                w->looks[p.path] = lookPath;
            }
            if (p.normals) {
                VtVec3fArray nv(p.nIndices);
                for (int k = 0; k < p.nIndices; ++k) {
                    nv[k] = GfVec3f(p.normals[k * 3], p.normals[k * 3 + 1], p.normals[k * 3 + 2]);
                }
                mesh.GetNormalsAttr().Set(nv, t);
                if (first) {
                    mesh.SetNormalsInterpolation(UsdGeomTokens->faceVarying);
                }
            }
        } else if (p.type == SOLVUSD_CAMERA) {
            UsdGeomCamera cam(prim);
            cam.GetFocalLengthAttr().Set( (float)p.focalLength, t );
            cam.GetHorizontalApertureAttr().Set( (float)p.hAperture, t );
            cam.GetVerticalApertureAttr().Set( (float)p.vAperture, t );
            cam.GetHorizontalApertureOffsetAttr().Set( (float)p.hOffset, t );
            cam.GetVerticalApertureOffsetAttr().Set( (float)p.vOffset, t );
            cam.GetClippingRangeAttr().Set( GfVec2f( (float)p.nearClip, (float)p.farClip ), t );
            if (p.focusDistance > 0.) {
                cam.GetFocusDistanceAttr().Set( (float)p.focusDistance, t );
            }
            if (p.fStop > 0.) {
                cam.GetFStopAttr().Set( (float)p.fStop, t );
            }
            if (first) {
                cam.GetProjectionAttr().Set(p.orthographic ? UsdGeomTokens->orthographic : UsdGeomTokens->perspective);
            }
        } else if (p.type == SOLVUSD_LIGHT) {
            UsdLuxLightAPI L(prim);
            L.CreateColorAttr().Set( GfVec3f(p.lightColor[0], p.lightColor[1], p.lightColor[2]), t );
            L.CreateIntensityAttr().Set(p.intensity, t);
            if (p.lightType == SOLVUSD_DISTANT) {
                UsdLuxDistantLight(prim).CreateAngleAttr().Set(p.angle, t);
            } else if (p.lightType != SOLVUSD_DOME) {
                UsdLuxSphereLight(prim).CreateRadiusAttr().Set(p.radius, t);
                if (p.lightType == SOLVUSD_SPOT) {
                    UsdLuxShapingAPI sp = UsdLuxShapingAPI::Apply(prim);
                    sp.CreateShapingConeAngleAttr().Set(p.coneAngle, t);
                    sp.CreateShapingConeSoftnessAttr().Set(p.coneSoftness, t);
                }
            }
            if (first) {
                UsdLuxShadowAPI::Apply(prim).CreateShadowEnableAttr().Set( (bool)p.shadows );
            }
        }
    }

    return 1;
}

SOLVUSD_API int
solvusd_write_end(void* h, char* err, int errLen)
{
    Writer* w = static_cast<Writer*>(h);
    if (!w) {
        return 0;
    }
    bool ok = true;
    try {
        // default prim: the first root
        for ( const UsdPrim& c : w->stage->GetPseudoRoot().GetChildren() ) {
            if ( c.GetName() != TfToken("Looks") ) {
                w->stage->SetDefaultPrim(c);
                break;
            }
        }
        ok = w->stage->GetRootLayer()->Save();
        if (ok && w->usdz) {
            ok = UsdUtilsCreateNewUsdzPackage(SdfAssetPath(w->writePath), w->file);
            std::remove( w->writePath.c_str() );
        }
        if (!ok) {
            setErr(err, errLen, "cannot write " + w->file);
        }
    } catch (const std::exception& e) {
        setErr(err, errLen, e.what() );
        ok = false;
    }
    delete w;

    return ok ? 1 : 0;
}

} // extern "C"
