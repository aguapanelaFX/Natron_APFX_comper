/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include "SolvScanline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <thread>

namespace Solv3D {

static const double kPi = 3.14159265358979323846;

void
TexImage::sample(double px, double py, float* out) const
{
    // px, py: absolute pixel coordinates (pixel centres at +0.5)
    const double x = px - 0.5 - x1, y = py - 0.5 - y1;
    const int ix = (int)std::floor(x), iy = (int)std::floor(y);
    const float fx = (float)(x - ix), fy = (float)(y - iy);
    out[0] = out[1] = out[2] = out[3] = 0.f;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int xx = ix + i, yy = iy + j;
            if ( (xx < 0) || (yy < 0) || (xx >= w) || (yy >= h) ) {
                continue;
            }
            const float wgt = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
            const float* p = &data[( (size_t)yy * w + xx ) * 4];
            for (int c = 0; c < 4; ++c) {
                out[c] += p[c] * wgt;
            }
        }
    }
}

void
TexImage::buildMips()
{
    mips.clear();
    const TexImage* prev = this;
    while ( (prev->w > 1) || (prev->h > 1) ) {
        TexImage n;
        n.w = std::max(1, prev->w / 2);
        n.h = std::max(1, prev->h / 2);
        n.data.assign( (size_t)n.w * n.h * 4, 0.f );
        for (int y = 0; y < n.h; ++y) {
            for (int x = 0; x < n.w; ++x) {
                for (int c = 0; c < 4; ++c) {
                    float acc = 0.f;
                    for (int j = 0; j < 2; ++j) {
                        for (int i = 0; i < 2; ++i) {
                            const int xx = std::min(prev->w - 1, 2 * x + i), yy = std::min(prev->h - 1, 2 * y + j);
                            acc += prev->data[( (size_t)yy * prev->w + xx ) * 4 + c];
                        }
                    }
                    n.data[( (size_t)y * n.w + x ) * 4 + c] = acc * 0.25f;
                }
            }
        }
        mips.push_back(n);
        prev = &mips.back();
        if (mips.size() > 16) {
            break;
        }
    }
}

void
TexImage::sampleLod(double px, double py, double lod, float* out) const
{
    if ( (lod <= 0.) || mips.empty() ) {
        sample(px, py, out);

        return;
    }
    const double L = std::min( lod, (double)mips.size() );
    const int l0 = (int)std::floor(L), l1 = std::min( (int)mips.size(), l0 + 1 );
    const float f = (float)(L - l0);
    auto at = [&](int l, float* o) {
        if (l == 0) {
            sample(px, py, o);

            return;
        }
        const TexImage& m = mips[l - 1];
        // continuous position in this level's pixels (the level starts at 0, 0)
        const double lx = (px - x1) * (double)m.w / w, ly = (py - y1) * (double)m.h / h;
        m.sample(lx, ly, o);
    };
    float a[4], b[4];
    at(l0, a);
    at(l1, b);
    for (int c = 0; c < 4; ++c) {
        out[c] = a[c] + (b[c] - a[c]) * f;
    }
}

namespace {

// separable reconstruction filters, x in pixels from the pixel centre, r = radius
inline double
filterWeight(ScanlineFilter f, double x, double r)
{
    const double ax = std::fabs(x);
    if (ax >= r) {
        return 0.;
    }
    switch (f) {
    case ScanlineFilter::Box:
        return 1.;
    case ScanlineFilter::Triangle:
        return 1. - ax / r;
    case ScanlineFilter::Gaussian: {
        const double sigma = r / 2.5;
        return std::exp( -0.5 * x * x / (sigma * sigma) ) - std::exp( -0.5 * r * r / (sigma * sigma) );
    }
    case ScanlineFilter::Mitchell: {
        const double B = 1. / 3., C = 1. / 3.;
        const double t = 2. * ax / r; // Mitchell-Netravali spans 2 units
        if (t < 1.) {
            return ( (12 - 9 * B - 6 * C) * t * t * t + (-18 + 12 * B + 6 * C) * t * t + (6 - 2 * B) ) / 6.;
        }
        return ( (-B - 6 * C) * t * t * t + (6 * B + 30 * C) * t * t + (-12 * B - 48 * C) * t + (8 * B + 24 * C) ) / 6.;
    }
    case ScanlineFilter::BlackmanHarris:
    default: {
        const double n = 0.5 + 0.5 * x / r; // 0..1 across the window
        const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
        return a0 - a1 * std::cos(2 * kPi * n) + a2 * std::cos(4 * kPi * n) - a3 * std::cos(6 * kPi * n);
    }
    }
}

struct EnvMips
{
    std::vector<std::vector<float> > lv; // rgb per level
    std::vector<int> w, h;

    void build(const TexImage& e)
    {
        int W = e.w, H = e.h;
        std::vector<float> base( (size_t)W * H * 3 );
        for (int i = 0; i < W * H; ++i) {
            const float a = e.data[(size_t)i * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                base[(size_t)i * 3 + c] = e.data[(size_t)i * 4 + c] / (a > 1e-6f ? 1.f : 1.f); // environments are opaque
            }
        }
        lv.push_back(base); w.push_back(W); h.push_back(H);
        while ( (W > 4) && (H > 2) ) {
            const int nw = std::max(1, W / 2), nh = std::max(1, H / 2);
            std::vector<float> n( (size_t)nw * nh * 3, 0.f );
            const std::vector<float>& p = lv.back();
            for (int y = 0; y < nh; ++y) {
                for (int x = 0; x < nw; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        float s = 0.f;
                        for (int j = 0; j < 2; ++j) {
                            for (int i = 0; i < 2; ++i) {
                                const int xx = std::min(W - 1, 2 * x + i), yy = std::min(H - 1, 2 * y + j);
                                s += p[( (size_t)yy * W + xx ) * 3 + c];
                            }
                        }
                        n[( (size_t)y * nw + x ) * 3 + c] = s * 0.25f;
                    }
                }
            }
            lv.push_back(n); w.push_back(nw); h.push_back(nh);
            W = nw; H = nh;
        }
    }

    void fetch(int l, double u, double v, float* out) const
    {
        const int W = w[l], H = h[l];
        double x = u * W - 0.5, y = v * H - 0.5;
        const int ix = (int)std::floor(x), iy = (int)std::floor(y);
        const float fx = (float)(x - ix), fy = (float)(y - iy);
        out[0] = out[1] = out[2] = 0.f;
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                int xx = ( (ix + i) % W + W ) % W;                 // wraps around horizontally
                int yy = std::max( 0, std::min(H - 1, iy + j) );   // clamps at the poles
                const float wgt = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
                const float* p = &lv[l][( (size_t)yy * W + xx ) * 3];
                for (int c = 0; c < 3; ++c) {
                    out[c] += p[c] * wgt;
                }
            }
        }
    }

    // direction (Y up) -> lat-long, roughness 0..1 -> blurrier level
    void sample(const Vec3& d, double roughness, float* out) const
    {
        const double u = 0.5 + std::atan2(d.x, -d.z) / (2. * kPi);
        const double v = 0.5 + std::asin( std::max( -1., std::min(1., d.y) ) ) / kPi;
        const double L = std::max( 0., std::min(1., roughness) ) * (double)( lv.size() - 1 );
        const int l0 = (int)std::floor(L), l1 = std::min( (int)lv.size() - 1, l0 + 1 );
        const float f = (float)(L - l0);
        float a[3], b[3];
        fetch(l0, u, v, a);
        fetch(l1, u, v, b);
        for (int c = 0; c < 3; ++c) {
            out[c] = a[c] + (b[c] - a[c]) * f;
        }
    }
};

struct Tri
{
    // per vertex: world position, world normal, uv; screen x, y, 1/w, z/w
    Vec3 P[3], N[3];
    Vec2 T[3];
    double sx[3], sy[3], iw[3], zw[3];
    const Mesh* mesh;
    bool hasUV;
    bool textured = true; // render mode solid: no texture / projections
    double lodArea = 0.; // uv area / screen area (texture minification), 0 = unknown
};

struct ClipVert
{
    double cx, cy, cz, cw; // clip space
    Vec3 P, N;
    Vec2 T;
};

ClipVert
lerpV(const ClipVert& a, const ClipVert& b, double t)
{
    ClipVert r;
    r.cx = a.cx + (b.cx - a.cx) * t; r.cy = a.cy + (b.cy - a.cy) * t; r.cz = a.cz + (b.cz - a.cz) * t; r.cw = a.cw + (b.cw - a.cw) * t;
    r.P = a.P + (b.P - a.P) * t; r.N = a.N + (b.N - a.N) * t;
    r.T.x = a.T.x + (b.T.x - a.T.x) * t; r.T.y = a.T.y + (b.T.y - a.T.y) * t;

    return r;
}

// clip-space point -> clip space (row-major Mat4, column vector)
inline void
toClip(const Mat4& m, const Vec3& p, double* c)
{
    for (int i = 0; i < 4; ++i) {
        c[i] = m.m[i][0] * p.x + m.m[i][1] * p.y + m.m[i][2] * p.z + m.m[i][3];
    }
}
// depth map seen from a projector (Project3D self occlusion): NDC depth per texel
struct OcclusionMap
{
    int W = 0, H = 0;
    Mat4 viewProj;
    std::vector<float> z;

    void build(const Mat4& vp, bool orthographic, const std::vector<std::array<Vec3, 3> >& world, int w, int h)
    {
        W = w; H = h; viewProj = vp; ortho = orthographic;
        z.assign( (size_t)W * H, std::numeric_limits<float>::max() );
        for (const std::array<Vec3, 3>& t : world) {
            double c[3][4];
            bool behind = false;
            for (int k = 0; k < 3; ++k) {
                toClip(vp, t[k], c[k]);
                if (c[k][3] <= 1e-6) {
                    behind = true;
                }
            }
            if (behind) {
                continue; // projector near plane: ignore (rare for occluders)
            }
            double sx[3], sy[3], sz[3];
            for (int k = 0; k < 3; ++k) {
                sx[k] = (c[k][0] / c[k][3] * 0.5 + 0.5) * W;
                sy[k] = (c[k][1] / c[k][3] * 0.5 + 0.5) * H;
                sz[k] = linearDepth(c[k]);
            }
            const double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::fabs(area) < 1e-12) {
                continue;
            }
            const int xa = std::max( 0, (int)std::floor( std::min( sx[0], std::min(sx[1], sx[2]) ) ) );
            const int xb = std::min( W - 1, (int)std::ceil( std::max( sx[0], std::max(sx[1], sx[2]) ) ) );
            const int ya = std::max( 0, (int)std::floor( std::min( sy[0], std::min(sy[1], sy[2]) ) ) );
            const int yb = std::min( H - 1, (int)std::ceil( std::max( sy[0], std::max(sy[1], sy[2]) ) ) );
            const double ia = 1. / area;
            for (int y = ya; y <= yb; ++y) {
                for (int x = xa; x <= xb; ++x) {
                    const double px = x + 0.5, py = y + 0.5;
                    const double w0 = ( (sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py) ) * ia;
                    const double w1 = ( (sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py) ) * ia;
                    const double w2 = 1. - w0 - w1;
                    if ( (w0 < 0) || (w1 < 0) || (w2 < 0) ) {
                        continue;
                    }
                    // linear depth is not linear in screen space (perspective): interpolate 1/z
                    const float zz = (float)( ortho ? (w0 * sz[0] + w1 * sz[1] + w2 * sz[2])
                                                    : 1. / (w0 / sz[0] + w1 / sz[1] + w2 / sz[2]) );
                    float& d = z[(size_t)y * W + x];
                    if (zz < d) {
                        d = zz;
                    }
                }
            }
        }
    }

    // true when the point is behind something else seen from the projector
    bool occluded(const Vec3& P) const
    {
        double c[4];
        toClip(viewProj, P, c);
        if (c[3] <= 1e-9) {
            return false;
        }
        const double nx = c[0] / c[3], ny = c[1] / c[3], lz = linearDepth(c);
        const int x = (int)std::floor( (nx * 0.5 + 0.5) * W ), y = (int)std::floor( (ny * 0.5 + 0.5) * H );
        if ( (x < 0) || (y < 0) || (x >= W) || (y >= H) ) {
            return false;
        }
        const float best = z[(size_t)y * W + x];
        return lz > best * 1.01 + 1e-4; // 1% relative bias: no self shadowing on curved surfaces
    }

    bool ortho = false;
    // distance along the projector axis: w for perspective, z for orthographic (both increase away from it)
    double linearDepth(const double* c) const
    {
        return ortho ? (c[2] + 1.) * 0.5 + 1e-6 : std::max(1e-9, c[3]);
    }
};
} // anon

void
scanlineRender(const Scene& scene,
               const std::string& cameraPath,
               const ScanlineSettings& s,
               const TextureResolver& textures,
               const TexImage* environment,
               std::vector<float>* out)
{
    const int W = s.width, H = s.height;
    out->assign( (size_t)std::max(0, W) * std::max(0, H) * 4, 0.f );
    if ( (W <= 0) || (H <= 0) ) {
        return;
    }
    // camera
    Camera cam;
    Mat4 camWorld = Mat4::translate( Vec3(0, 0, 5) );
    if ( const Prim* cp = scene.find(cameraPath) ) {
        if (cp->camera) {
            cam = *cp->camera;
            camWorld = scene.worldMatrix(cameraPath);
        }
    }
    if (s.projectionMode == 1) {
        cam.projectionMode = 0;
    } else if (s.projectionMode == 2) {
        cam.projectionMode = 1;
    }
    const double fw = s.formatX2 - s.formatX1, fh = s.formatY2 - s.formatY1;
    const double aspect = fw * s.pixelAspect / std::max(1e-9, fh);
    const Mat4 view = camWorld.inverse();
    const Mat4 viewProj = cam.projection(aspect) * view;
    const Vec3 camPos = camWorld.point( Vec3(0, 0, 0) );

    // triangles (world space attributes + clip, near-plane clipped)
    std::vector<Tri> tris;
    std::vector<std::array<Vec3, 3> > occluders; // world triangles (projector self occlusion)
    bool needOccluders = false;
    for (const auto& kv : scene.prims()) {
        if (kv.second.mesh) {
            for (const Projection& pr : kv.second.mesh->projections) {
                needOccluders = needOccluders || pr.occlusion;
            }
        }
    }
    const double farC = std::max(1e-6, cam.farClip);
    for (const auto& kv : scene.prims()) {
        const Prim& p = kv.second;
        if ( (p.type != PrimType::Mesh) || !p.mesh || !p.visible || (p.render == 0) ) {
            continue;
        }
        // hidden parent hides children
        bool hidden = false;
        for (std::string par = parentPath(kv.first); !par.empty(); par = parentPath(par)) {
            const Prim* pp = scene.find(par);
            if (pp && !pp->visible) {
                hidden = true;
                break;
            }
        }
        if (hidden) {
            continue;
        }
        const Mesh& m = *p.mesh;
        const Mat4 world = scene.worldMatrix(kv.first);
        const Mat4 nrmM = world.inverse().transposed();
        std::vector<std::array<int, 3> > t3, fv;
        m.triangulate(&t3, &fv);
        const bool hasN = m.normals.size() == m.faceVertexIndices.size();
        const bool hasUV = m.st.size() == m.faceVertexIndices.size();
        for (size_t t = 0; t < t3.size(); ++t) {
            ClipVert v[3];
            for (int k = 0; k < 3; ++k) {
                v[k].P = world.point(m.points[t3[t][k]]);
                v[k].N = hasN ? normalize( nrmM.vector(m.normals[fv[t][k]]) ) : Vec3();
                if (hasUV) {
                    v[k].T = m.st[fv[t][k]];
                }
                double c[4];
                if (s.projectionMode == 3) {
                    // uv unwrap: the 0..1 uv square fills the format
                    c[0] = v[k].T.x * 2. - 1.; c[1] = v[k].T.y * 2. - 1.; c[2] = 0.; c[3] = 1.;
                } else if (s.projectionMode == 4) {
                    // spherical: lat-long of the direction from the camera (camera space, -Z forward)
                    const Vec3 d = view.vector( v[k].P - camPos );
                    const double len = std::sqrt( dot(d, d) );
                    const Vec3 n = len > 1e-12 ? d * (1. / len) : Vec3(0, 0, -1);
                    const double u = 0.5 + std::atan2(n.x, -n.z) / (2. * kPi);
                    const double vv = 0.5 + std::asin( std::max( -1., std::min(1., n.y) ) ) / kPi;
                    c[0] = u * 2. - 1.; c[1] = vv * 2. - 1.; c[2] = std::min(1., len / farC) * 2. - 1.; c[3] = 1.;
                } else {
                    toClip(viewProj, v[k].P, c);
                }
                v[k].cx = c[0]; v[k].cy = c[1]; v[k].cz = c[2]; v[k].cw = c[3];
            }
            if (needOccluders) {
                occluders.push_back( { { v[0].P, v[1].P, v[2].P } } );
            }
            if (s.projectionMode == 3 && !hasUV) {
                continue;
            }
            if (s.projectionMode == 4) {
                // triangles across the lat-long seam would stretch over the whole image: skip them
                const double u0 = v[0].cx, u1 = v[1].cx, u2 = v[2].cx;
                if ( std::max( u0, std::max(u1, u2) ) - std::min( u0, std::min(u1, u2) ) > 1. ) {
                    continue;
                }
            }
            if (!hasN) {
                const Vec3 n = normalize( cross(v[1].P - v[0].P, v[2].P - v[0].P) );
                v[0].N = v[1].N = v[2].N = n;
            }
            // clip against the near plane (w > eps)
            const double eps = 1e-5;
            std::vector<ClipVert> poly(v, v + 3), outp;
            for (size_t i = 0; i < poly.size(); ++i) {
                const ClipVert& a = poly[i];
                const ClipVert& b = poly[(i + 1) % poly.size()];
                const double da = a.cw - eps, db = b.cw - eps;
                if (da >= 0) {
                    outp.push_back(a);
                }
                if ( (da >= 0) != (db >= 0) ) {
                    outp.push_back( lerpV( a, b, da / (da - db) ) );
                }
            }
            for (size_t i = 1; i + 1 < outp.size(); ++i) {
                const ClipVert* q[3] = { &outp[0], &outp[i], &outp[i + 1] };
                Tri tr;
                tr.mesh = &m;
                tr.hasUV = hasUV;
                tr.textured = p.render != 1;
                for (int k = 0; k < 3; ++k) {
                    const double iw = 1. / q[k]->cw;
                    // NDC -> format pixels -> output buffer pixels
                    tr.sx[k] = s.formatX1 + (q[k]->cx * iw * 0.5 + 0.5) * fw - s.originX;
                    tr.sy[k] = s.formatY1 + (q[k]->cy * iw * 0.5 + 0.5) * fh - s.originY;
                    tr.iw[k] = iw;
                    tr.zw[k] = q[k]->cz * iw;
                    tr.P[k] = q[k]->P;
                    tr.N[k] = q[k]->N;
                    tr.T[k] = q[k]->T;
                }
                {
                    const double sa = std::fabs( (tr.sx[1] - tr.sx[0]) * (tr.sy[2] - tr.sy[0]) - (tr.sx[2] - tr.sx[0]) * (tr.sy[1] - tr.sy[0]) );
                    const double ua = std::fabs( (tr.T[1].x - tr.T[0].x) * (tr.T[2].y - tr.T[0].y) - (tr.T[2].x - tr.T[0].x) * (tr.T[1].y - tr.T[0].y) );
                    tr.lodArea = sa > 1e-12 ? ua / sa : 0.;
                }
                tris.push_back(tr);
            }
        }
    }

    // projector depth maps (one per occluding projection)
    // (Project3D gives every mesh a copy of the same projection: one map per distinct projector)
    std::map<const Projection*, std::shared_ptr<OcclusionMap> > occMaps;
    if (needOccluders) {
        std::vector<std::shared_ptr<OcclusionMap> > built;
        for (const auto& kv : scene.prims()) {
            if (!kv.second.mesh) {
                continue;
            }
            for (const Projection& pr : kv.second.mesh->projections) {
                if (!pr.occlusion) {
                    continue;
                }
                std::shared_ptr<OcclusionMap> found;
                for (const std::shared_ptr<OcclusionMap>& b : built) {
                    bool same = true;
                    for (int i = 0; i < 4 && same; ++i) {
                        for (int j = 0; j < 4 && same; ++j) {
                            same = b->viewProj.m[i][j] == pr.viewProj.m[i][j];
                        }
                    }
                    if (same) {
                        found = b;
                        break;
                    }
                }
                if (!found) {
                    found = std::make_shared<OcclusionMap>();
                    found->build(pr.viewProj, pr.orthographic, occluders, 1024, 1024);
                    built.push_back(found);
                }
                occMaps[&pr] = found;
            }
        }
    }

    EnvMips env;
    if ( environment && (environment->w > 0) && (environment->h > 0) ) {
        env.build(*environment);
    }

    int grid = (int)std::lround( std::sqrt( (double)std::max(1, s.samples) ) );
    grid = std::max(1, std::min(8, grid) );
    const int nS = grid * grid;
    const int nThreads = s.threads > 0 ? s.threads : std::max(1u, std::thread::hardware_concurrency() );

    const double fr = std::max( 0.5, s.filterWidth * 0.5 );   // filter radius (pixels)
    const int fri = (int)std::ceil(fr);
    std::vector<float> wsum( (size_t)W * H, 0.f );
    std::vector<float> sampleCol( (size_t)W * H * 4 );
    std::vector<int> triId( (size_t)W * H );
    std::vector<float> depth( (size_t)W * H );
    std::vector<float> bary( (size_t)W * H * 2 );

    auto runRows = [&](const std::function<void(int, int)>& fn) {
        std::vector<std::thread> pool;
        const int n = std::min(nThreads, H);
        for (int t = 0; t < n; ++t) {
            const int a = (int)( (long long)H * t / n ), b = (int)( (long long)H * (t + 1) / n );
            pool.emplace_back(fn, a, b);
        }
        for (std::thread& th : pool) {
            th.join();
        }
    };

    for (int si = 0; si < nS; ++si) {
        const double ox = ( (si % grid) + 0.5 ) / grid, oy = ( (si / grid) + 0.5 ) / grid;
        std::fill(triId.begin(), triId.end(), -1);
        std::fill( depth.begin(), depth.end(), std::numeric_limits<float>::max() );
        // raster pass
        runRows([&](int y0, int y1) {
            for (size_t ti = 0; ti < tris.size(); ++ti) {
                const Tri& t = tris[ti];
                const double minY = std::min( t.sy[0], std::min(t.sy[1], t.sy[2]) ), maxY = std::max( t.sy[0], std::max(t.sy[1], t.sy[2]) );
                const int ya = std::max( y0, (int)std::floor(minY - oy) ), yb = std::min( y1 - 1, (int)std::ceil(maxY - oy) );
                if (ya > yb) {
                    continue;
                }
                const double minX = std::min( t.sx[0], std::min(t.sx[1], t.sx[2]) ), maxX = std::max( t.sx[0], std::max(t.sx[1], t.sx[2]) );
                const int xa = std::max( 0, (int)std::floor(minX - ox) ), xb = std::min( W - 1, (int)std::ceil(maxX - ox) );
                const double area = (t.sx[1] - t.sx[0]) * (t.sy[2] - t.sy[0]) - (t.sx[2] - t.sx[0]) * (t.sy[1] - t.sy[0]);
                if (std::fabs(area) < 1e-12) {
                    continue;
                }
                const double ia = 1. / area;
                for (int y = ya; y <= yb; ++y) {
                    const double py = y + oy;
                    for (int x = xa; x <= xb; ++x) {
                        const double px = x + ox;
                        const double w0 = ( (t.sx[1] - px) * (t.sy[2] - py) - (t.sx[2] - px) * (t.sy[1] - py) ) * ia;
                        const double w1 = ( (t.sx[2] - px) * (t.sy[0] - py) - (t.sx[0] - px) * (t.sy[2] - py) ) * ia;
                        const double w2 = 1. - w0 - w1;
                        if ( (w0 < 0) || (w1 < 0) || (w2 < 0) ) {
                            continue;
                        }
                        const double z = w0 * t.zw[0] + w1 * t.zw[1] + w2 * t.zw[2];
                        if ( (z < -1.) || (z > 1.) ) {
                            continue;
                        }
                        const size_t k = (size_t)y * W + x;
                        if ( !s.zBuffer || (z < depth[k]) ) {
                            depth[k] = (float)z;
                            triId[k] = (int)ti;
                            // perspective-correct barycentrics
                            const double q0 = w0 * t.iw[0], q1 = w1 * t.iw[1], q2 = w2 * t.iw[2], qs = q0 + q1 + q2;
                            bary[k * 2] = (float)(q0 / qs);
                            bary[k * 2 + 1] = (float)(q1 / qs);
                        }
                    }
                }
            }
        });
        // shading pass (one shaded sample per pixel for this sample position)
        std::fill(sampleCol.begin(), sampleCol.end(), 0.f);
        runRows([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < W; ++x) {
                    const size_t k = (size_t)y * W + x;
                    const int ti = triId[k];
                    if (ti < 0) {
                        continue;
                    }
                    const Tri& t = tris[ti];
                    const double b0 = bary[k * 2], b1 = bary[k * 2 + 1], b2 = 1. - b0 - b1;
                    const Vec3 P = t.P[0] * b0 + t.P[1] * b1 + t.P[2] * b2;
                    Vec3 N = normalize(t.N[0] * b0 + t.N[1] * b1 + t.N[2] * b2);
                    Vec2 uv;
                    uv.x = t.T[0].x * b0 + t.T[1].x * b1 + t.T[2].x * b2;
                    uv.y = t.T[0].y * b0 + t.T[1].y * b1 + t.T[2].y * b2;
                    const Vec3 V = normalize(camPos - P);   // towards the camera
                    if (dot(N, V) < 0.) {
                        N = N * -1.;                         // two-sided
                    }
                    float c[4] = { 0, 0, 0, 0 };
                    switch (s.output) {
                    case ScanlineOutput::Depth: {
                        const float d = (float)std::sqrt( dot(P - camPos, P - camPos) );
                        c[0] = c[1] = c[2] = d; c[3] = 1.f;
                        break;
                    }
                    case ScanlineOutput::Position:
                        c[0] = (float)P.x; c[1] = (float)P.y; c[2] = (float)P.z; c[3] = 1.f;
                        break;
                    case ScanlineOutput::Normal:
                        c[0] = (float)N.x; c[1] = (float)N.y; c[2] = (float)N.z; c[3] = 1.f;
                        break;
                    case ScanlineOutput::UV:
                        c[0] = (float)uv.x; c[1] = (float)uv.y; c[2] = 0.f; c[3] = 1.f;
                        break;
                    default: {
                        const Material& mt = t.mesh->material;
                        float base[4] = { 1.f, 1.f, 1.f, 1.f };
                        bool textured = false;
                        if (t.textured && t.mesh->texture && t.hasUV) {
                            const TexImage* tex = textures(t.mesh->texture);
                            if (tex) {
                                const double tw = tex->fx2 - tex->fx1, thh = tex->fy2 - tex->fy1;
                                double lod = 0.;
                                if ( s.textureFiltering && (t.lodArea > 0.) ) {
                                    lod = 0.5 * std::log2( std::max(1e-12, t.lodArea * tw * thh) );
                                }
                                tex->sampleLod( tex->fx1 + uv.x * tw, tex->fy1 + uv.y * thh, lod, base );
                                textured = true;
                            }
                        }
                        // projections (Project3D): later ones go over earlier ones; a projected surface
                        // without its own texture starts transparent (only the projections show)
                        if ( !textured && t.textured && !t.mesh->projections.empty() ) {
                            base[0] = base[1] = base[2] = base[3] = 0.f;
                            textured = true;
                        }
                        for (const Projection& pr : t.mesh->projections) {
                            if (!t.textured) {
                                break;
                            }
                            const TexImage* tex = textures(pr.texture);
                            if (!tex) {
                                continue;
                            }
                            if (pr.projectOn != 0) {
                                // front / back as seen from the projector (geometric normal, not the flipped one)
                                const Vec3 gN = normalize(t.N[0] * b0 + t.N[1] * b1 + t.N[2] * b2);
                                const Vec3 toProj = pr.orthographic ? pr.forward * -1. : pr.origin - P;
                                const bool front = dot(gN, toProj) >= 0.;
                                if ( front != (pr.projectOn == 1) ) {
                                    continue;
                                }
                            }
                            if (pr.occlusion) {
                                std::map<const Projection*, std::shared_ptr<OcclusionMap> >::const_iterator om = occMaps.find(&pr);
                                if ( (om != occMaps.end()) && om->second->occluded(P) ) {
                                    continue;
                                }
                            }
                            double cc[4];
                            toClip(pr.viewProj, P, cc);
                            if (cc[3] <= 1e-9) {
                                continue;
                            }
                            const double nx = cc[0] / cc[3], ny = cc[1] / cc[3];
                            if ( pr.crop && ( (nx < -1) || (nx > 1) || (ny < -1) || (ny > 1) ) ) {
                                continue;
                            }
                            float pc[4];
                            tex->sample( tex->fx1 + (nx * 0.5 + 0.5) * (tex->fx2 - tex->fx1), tex->fy1 + (ny * 0.5 + 0.5) * (tex->fy2 - tex->fy1), pc );
                            if (!textured) {
                                base[0] = base[1] = base[2] = base[3] = 0.f;
                                textured = true;
                            }
                            for (int ch = 0; ch < 4; ++ch) {
                                base[ch] = pc[ch] + base[ch] * (1.f - pc[3]);
                            }
                        }
                        if (!s.transparency && textured) {
                            // opaque: un-premultiply the texture, alpha 1
                            const float ta = base[3];
                            for (int ch = 0; ch < 3; ++ch) {
                                base[ch] = ta > 1e-6f ? base[ch] / ta : 0.f;
                            }
                            base[3] = 1.f;
                        }
                        const float a = textured ? base[3] : 1.f;
                        const float bc[3] = { (float)mt.baseColor.x, (float)mt.baseColor.y, (float)mt.baseColor.z };
                        // headlight lambert for the lit part
                        const float lam = (float)std::max( s.ambient, dot(N, V) );
                        const float lit = (float)( mt.emission + (1. - mt.emission) * lam );
                        for (int ch = 0; ch < 3; ++ch) {
                            c[ch] = base[ch] * bc[ch] * lit;
                        }
                        c[3] = a;
                        // environment reflection (split-sum style, Schlick Fresnel)
                        const double reflW = std::max(mt.specular, mt.metalness);
                        if ( (reflW > 0.) && !env.lv.empty() ) {
                            const double cosT = std::max( 0., dot(N, V) );
                            const Vec3 R = normalize(N * (2. * cosT) - V);
                            float e[3];
                            env.sample(R, mt.roughness, e);
                            const double f0d = std::pow( (mt.ior - 1.) / (mt.ior + 1.), 2. );
                            const double sch = std::pow(1. - cosT, 5.);
                            const double gloss = (1. - mt.roughness) * (1. - mt.roughness);
                            for (int ch = 0; ch < 3; ++ch) {
                                // metals tint the reflection with the base colour (premultiplied: divide by alpha)
                                const double albedo = a > 1e-6f ? base[ch] * bc[ch] / a : 0.;
                                const double f0 = f0d + (albedo - f0d) * mt.metalness;
                                const double F = f0 + (std::max(f0, gloss) - f0) * sch;
                                const double refl = e[ch] * F * reflW;
                                c[ch] = (float)( c[ch] * (1. - F * mt.metalness) + refl * a );
                            }
                        }
                        break;
                    }
                    }
                    float* o = &sampleCol[k * 4];
                    for (int ch = 0; ch < 4; ++ch) {
                        o[ch] = c[ch];
                    }
                }
            }
        });
        // filter pass (gather): every pixel takes the weighted samples around it
        runRows([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < W; ++x) {
                    float acc[4] = { 0, 0, 0, 0 };
                    float ws = 0.f;
                    for (int j = -fri; j <= fri; ++j) {
                        const int yy = y + j;
                        if ( (yy < 0) || (yy >= H) ) {
                            continue;
                        }
                        const double wy = filterWeight(s.filter, (yy + oy) - (y + 0.5), fr);
                        if (wy == 0.) {
                            continue;
                        }
                        for (int i = -fri; i <= fri; ++i) {
                            const int xx = x + i;
                            if ( (xx < 0) || (xx >= W) ) {
                                continue;
                            }
                            const double w = wy * filterWeight(s.filter, (xx + ox) - (x + 0.5), fr);
                            if (w == 0.) {
                                continue;
                            }
                            const float* sc = &sampleCol[( (size_t)yy * W + xx ) * 4];
                            for (int ch = 0; ch < 4; ++ch) {
                                acc[ch] += (float)w * sc[ch];
                            }
                            ws += (float)w;
                        }
                    }
                    const size_t k = (size_t)y * W + x;
                    float* o = &(*out)[k * 4];
                    for (int ch = 0; ch < 4; ++ch) {
                        o[ch] += acc[ch];
                    }
                    wsum[k] += ws;
                }
            }
        });
    }
    for (size_t k = 0; k < wsum.size(); ++k) {
        const float w = wsum[k];
        float* o = &(*out)[k * 4];
        for (int ch = 0; ch < 4; ++ch) {
            o[ch] = w != 0.f ? o[ch] / w : 0.f;
        }
    }
}
} // namespace Solv3D
