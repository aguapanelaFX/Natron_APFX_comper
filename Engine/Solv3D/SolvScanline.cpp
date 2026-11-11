/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include "SolvScanline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
    const double fw = s.formatX2 - s.formatX1, fh = s.formatY2 - s.formatY1;
    const double aspect = fw * s.pixelAspect / std::max(1e-9, fh);
    const Mat4 view = camWorld.inverse();
    const Mat4 viewProj = cam.projection(aspect) * view;
    const Vec3 camPos = camWorld.point( Vec3(0, 0, 0) );

    // triangles (world space attributes + clip, near-plane clipped)
    std::vector<Tri> tris;
    for (const auto& kv : scene.prims()) {
        const Prim& p = kv.second;
        if ( (p.type != PrimType::Mesh) || !p.mesh || !p.visible ) {
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
                toClip(viewProj, v[k].P, c);
                v[k].cx = c[0]; v[k].cy = c[1]; v[k].cz = c[2]; v[k].cw = c[3];
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
                        if ( z < depth[k] ) {
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
                        if (t.mesh->texture && t.hasUV) {
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
                        // projections (Project3D): later ones go over earlier ones
                        for (const Projection& pr : t.mesh->projections) {
                            const TexImage* tex = textures(pr.texture);
                            if (!tex) {
                                continue;
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
