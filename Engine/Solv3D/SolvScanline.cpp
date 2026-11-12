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
    unsigned char edges = 0; // face edges among the triangle edges: bit0 = 0-1, bit1 = 1-2, bit2 = 2-0
    int lineMode = 0;        // 0 none, 1 wireframe only, 2 lines over the surface
    bool receiveShadow = true;
};

// distance (pixels) from (px, py) to the nearest face edge of the triangle, large if none
double
edgeDistance(const Tri& t, double px, double py)
{
    double best = 1e30;
    for (int e = 0; e < 3; ++e) {
        if ( !( t.edges & (1 << e) ) ) {
            continue;
        }
        const int a = e, b = (e + 1) % 3;
        const double ex = t.sx[b] - t.sx[a], ey = t.sy[b] - t.sy[a];
        const double l2 = ex * ex + ey * ey;
        double u = l2 > 1e-12 ? ( (px - t.sx[a]) * ex + (py - t.sy[a]) * ey ) / l2 : 0.;
        u = std::max( 0., std::min(1., u) );
        const double dx = t.sx[a] + u * ex - px, dy = t.sy[a] + u * ey - py;
        best = std::min( best, std::sqrt(dx * dx + dy * dy) );
    }

    return best;
}

static const double kLineHalfWidth = 0.6; // pixels

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

// ---------------------------------------------------------------- lights and shadows
// shadow map seen from a light: linear depth per texel (perspective: distance along the light axis,
// orthographic: distance from the front plane)
struct ShadowView
{
    Mat4 vp;
    bool ortho = false;
    double n = 0.1, f = 1e5;
    int R = 0;
    double k = 1.;          // texels per scene unit (orthographic) or per unit at distance 1 (perspective)
    std::vector<float> z;

    double lin(const double* c) const
    {
        if (ortho) {
            return ( (c[2] / c[3]) * 0.5 + 0.5 ) * (f - n) + n;
        }

        return c[3];
    }

    void build(const std::vector<std::array<Vec3, 3> >& world)
    {
        z.assign( (size_t)R * R, std::numeric_limits<float>::max() );
        for (const std::array<Vec3, 3>& t : world) {
            double c[3][4];
            bool behind = false;
            for (int q = 0; q < 3; ++q) {
                toClip(vp, t[q], c[q]);
                if (c[q][3] <= 1e-6) {
                    behind = true;
                }
            }
            if (behind) {
                continue;
            }
            double sx[3], sy[3], sz[3];
            for (int q = 0; q < 3; ++q) {
                sx[q] = (c[q][0] / c[q][3] * 0.5 + 0.5) * R;
                sy[q] = (c[q][1] / c[q][3] * 0.5 + 0.5) * R;
                sz[q] = lin(c[q]);
            }
            const double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::fabs(area) < 1e-12) {
                continue;
            }
            const int xa = std::max( 0, (int)std::floor( std::min( sx[0], std::min(sx[1], sx[2]) ) ) );
            const int xb = std::min( R - 1, (int)std::ceil( std::max( sx[0], std::max(sx[1], sx[2]) ) ) );
            const int ya = std::max( 0, (int)std::floor( std::min( sy[0], std::min(sy[1], sy[2]) ) ) );
            const int yb = std::min( R - 1, (int)std::ceil( std::max( sy[0], std::max(sy[1], sy[2]) ) ) );
            if ( (xa > xb) || (ya > yb) ) {
                continue;
            }
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
                    const float zz = (float)( ortho ? (w0 * sz[0] + w1 * sz[1] + w2 * sz[2])
                                                    : 1. / (w0 / sz[0] + w1 / sz[1] + w2 / sz[2]) );
                    float& d = z[(size_t)y * R + x];
                    if (zz < d) {
                        d = zz;
                    }
                }
            }
        }
    }

    // where P lands: texel position and linear depth. false when outside the map
    bool locate(const Vec3& P, double* u, double* v, double* d) const
    {
        double c[4];
        toClip(vp, P, c);
        if (c[3] <= 1e-9) {
            return false;
        }
        const double nx = c[0] / c[3], ny = c[1] / c[3];
        if ( (nx < -1.) || (nx > 1.) || (ny < -1.) || (ny > 1.) ) {
            return false;
        }
        *u = (nx * 0.5 + 0.5) * R;
        *v = (ny * 0.5 + 0.5) * R;
        *d = lin(c);

        return true;
    }

    float at(double u, double v) const
    {
        const int x = std::max( 0, std::min(R - 1, (int)u) ), y = std::max( 0, std::min(R - 1, (int)v) );

        return z[(size_t)y * R + x];
    }
};

// Vogel disk (golden angle spiral): even coverage for any sample count, rotated per pixel
static inline void
vogel(int i, int n, double rot, double* x, double* y)
{
    const double r = std::sqrt( (i + 0.5) / n );
    const double a = i * 2.39996322972865332 + rot;
    *x = r * std::cos(a);
    *y = r * std::sin(a);
}

static inline double
hash01(unsigned int a, unsigned int b, unsigned int c)
{
    unsigned int h = a * 73856093u ^ b * 19349663u ^ c * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;

    return (h & 0xffffff) / 16777216.;
}

struct LightEval
{
    int kind = 0;           // 0 point, 1 spot, 2 distant, 3 ambient
    Vec3 col;               // colour * intensity
    Vec3 pos, dir;          // dir: where the light points
    int falloff = 0;
    double cosInner = 1., cosOuter = 0.;
    bool shadows = false;
    double size = 0.;       // point / spot: radius (units); distant: tan(half angle)
    int samples = 16;
    double bias = 0.1;
    std::vector<ShadowView> views; // spot / distant: 1, point: 6 faces

    // 1 = lit, 0 = in shadow. PCSS: blocker search, then a filter as wide as the penumbra.
    double visibility(const Vec3& P, double NdotL, double rot) const
    {
        if ( !shadows || views.empty() ) {
            return 1.;
        }
        const ShadowView* sv = &views[0];
        if (views.size() == 6) {
            const Vec3 d = P - pos;
            const double ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
            const int face = (ax >= ay && ax >= az) ? (d.x > 0 ? 0 : 1) : (ay >= az ? (d.y > 0 ? 2 : 3) : (d.z > 0 ? 4 : 5) );
            sv = &views[face];
        }
        double u, v, dR;
        if ( !sv->locate(P, &u, &v, &dR) ) {
            return 1.;
        }
        const double tanT = std::sqrt( std::max(0., 1. - NdotL * NdotL) ) / std::max(0.05, NdotL);
        const double b = std::max(bias, dR * 0.0015) * ( 1. + std::min(4., tanT) );
        const double ref = dR - b;
        // texels for a world size at the receiver
        const double kR = sv->ortho ? sv->k : sv->k / std::max(1e-9, dR);
        // blocker search region
        double rs;
        if (sv->ortho) {
            rs = sv->k * size * std::max(0., dR - sv->n);
        } else {
            rs = kR * size * 4.;
        }
        rs = std::max( 1.5, std::min(48., rs) );
        // receiver slope: a tilted surface changes depth across the filter -> bias grows with the radius
        const double slope = std::min(4., tanT) / std::max(1e-9, kR);
        double sumB = 0.;
        int nB = 0;
        const int ns = 16;
        for (int i = 0; i < ns; ++i) {
            double ox, oy;
            vogel(i, ns, rot, &ox, &oy);
            const float zb = sv->at(u + ox * rs, v + oy * rs);
            if ( zb < ref - slope * rs * std::sqrt(ox * ox + oy * oy) ) {
                sumB += zb;
                ++nB;
            }
        }
        if (nB == 0) {
            return 1.;
        }
        const double dB = sumB / nB;
        double pen;
        if (sv->ortho) {
            pen = sv->k * (dR - dB) * size;
        } else {
            pen = kR * size * (dR - dB) / std::max(1e-6, dB);
        }
        const double rf = std::max( 1., std::min(48., pen) );
        int lit = 0;
        const int n = std::max(4, samples);
        for (int i = 0; i < n; ++i) {
            double ox, oy;
            vogel(i, n, rot + 1.3, &ox, &oy);
            if ( sv->at(u + ox * rf, v + oy * rf) >= ref - slope * rf * std::sqrt(ox * ox + oy * oy) ) {
                ++lit;
            }
        }

        return (double)lit / n;
    }
};

static Mat4
orthoMatrix(double l, double r, double b, double t, double n, double f)
{
    Mat4 m;
    m.m[0][0] = 2. / (r - l); m.m[0][3] = -(r + l) / (r - l);
    m.m[1][1] = 2. / (t - b); m.m[1][3] = -(t + b) / (t - b);
    m.m[2][2] = -2. / (f - n); m.m[2][3] = -(f + n) / (f - n);

    return m;
}

static Mat4
perspMatrix(double fovDeg, double n, double f)
{
    Mat4 m;
    const double c = 1. / std::tan(fovDeg * kPi / 360.);
    m.m[0][0] = c; m.m[1][1] = c;
    m.m[2][2] = -(f + n) / (f - n); m.m[2][3] = -2. * f * n / (f - n);
    m.m[3][2] = -1.; m.m[3][3] = 0.;

    return m;
}

// per motion-blur step: camera, triangles, projector occlusion maps, lights + shadow maps
struct Step
{
    Mat4 view, viewProj;
    Vec3 camPos;
    std::vector<Tri> tris;
    std::map<const Projection*, std::shared_ptr<OcclusionMap> > occMaps;
    std::vector<LightEval> lights;
    Vec3 ambientLight;
    bool anyLight = false;
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
    std::vector<const Scene*> one(1, &scene);
    scanlineRenderSteps( one, cameraPath, s, textures, environment, out, ScanlineAOV() );
}

void
scanlineRenderSteps(const std::vector<const Scene*>& scenes,
                    const std::string& cameraPath,
                    const ScanlineSettings& s,
                    const TextureResolver& textures,
                    const TexImage* environment,
                    std::vector<float>* out,
                    const ScanlineAOV& aov)
{
    const int W = s.width, H = s.height;
    out->assign( (size_t)std::max(0, W) * std::max(0, H) * 4, 0.f );
    if (aov.depth) {
        aov.depth->assign( (size_t)std::max(0, W) * std::max(0, H), 0.f );
    }
    if (aov.shadow) {
        aov.shadow->assign( (size_t)std::max(0, W) * std::max(0, H), 0.f );
    }
    const size_t npx = (size_t)std::max(0, W) * std::max(0, H);
    if (aov.position) { aov.position->assign(npx * 3, 0.f); }
    if (aov.normal) { aov.normal->assign(npx * 3, 0.f); }
    if (aov.uv) { aov.uv->assign(npx * 2, 0.f); }
    if (aov.motion) { aov.motion->assign(npx * 2, 0.f); }
    if ( (W <= 0) || (H <= 0) || scenes.empty() ) {
        return;
    }
    const double fw = s.formatX2 - s.formatX1, fh = s.formatY2 - s.formatY1;
    const double aspect = fw * s.pixelAspect / std::max(1e-9, fh);

    auto buildStep = [&](const Scene& scene, Step& stp) {
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
    const Mat4 view = camWorld.inverse();
    const Mat4 viewProj = cam.projection(aspect) * view;
    const Vec3 camPos = camWorld.point( Vec3(0, 0, 0) );

    // triangles (world space attributes + clip, near-plane clipped)
    std::vector<Tri> tris;
    std::vector<std::array<Vec3, 3> > occluders; // world triangles (projector self occlusion)
    std::vector<std::array<Vec3, 3> > casters;   // world triangles that cast shadows
    bool needOccluders = false, needCasters = false;
    for (const auto& kv : scene.prims()) {
        if ( (kv.second.type == PrimType::Light) && kv.second.light && kv.second.visible && kv.second.light->castShadows ) {
            needCasters = true;
        }
    }
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
        // hidden parent hides children; proxy / guide purposes are not rendered (purpose is inherited)
        bool hidden = false;
        auto notRendered = [](const Prim* q) {
            std::map<std::string, std::string>::const_iterator it = q->attributes.find("purpose");

            return ( it != q->attributes.end() ) && ( (it->second == "proxy") || (it->second == "guide") );
        };
        hidden = notRendered(&p);
        for (std::string par = parentPath(kv.first); !par.empty() && !hidden; par = parentPath(par)) {
            const Prim* pp = scene.find(par);
            if ( pp && ( !pp->visible || notRendered(pp) ) ) {
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
        std::vector<int> faceEnd( m.faceVertexIndices.size(), 0 );
        {
            size_t base = 0;
            for (int n : m.faceVertexCounts) {
                for (int k = 0; k < n; ++k) {
                    faceEnd[base + k] = (int)(base + n);
                }
                base += n;
            }
        }
        const int lineMode = (p.render == 1) ? 1 : ( (p.render == 3) || (p.render == 5) ) ? 2 : 0;
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
            if (needCasters && p.castShadow) {
                casters.push_back( { { v[0].P, v[1].P, v[2].P } } );
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
            // face edges of this fan triangle (no lines on the diagonals of quads / polygons)
            unsigned char edges = 2; // 1-2 is always a face edge
            if (fv[t][1] == fv[t][0] + 1) {
                edges |= 1;
            }
            if ( (fv[t][2] + 1) == faceEnd[fv[t][0]] ) {
                edges |= 4;
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
                tr.receiveShadow = p.receiveShadow;
                tr.hasUV = hasUV;
                tr.textured = p.render >= 4;
                tr.lineMode = lineMode;
                tr.edges = (outp.size() == 3) ? edges : 0;
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

    // lights (+ shadow maps)
    stp.ambientLight = Vec3();
    stp.anyLight = false;
    Vec3 casLo(1e30, 1e30, 1e30), casHi(-1e30, -1e30, -1e30);
    for (const std::array<Vec3, 3>& t : casters) {
        for (int q = 0; q < 3; ++q) {
            casLo = Vec3( std::min(casLo.x, t[q].x), std::min(casLo.y, t[q].y), std::min(casLo.z, t[q].z) );
            casHi = Vec3( std::max(casHi.x, t[q].x), std::max(casHi.y, t[q].y), std::max(casHi.z, t[q].z) );
        }
    }
    for (const auto& kv : scene.prims()) {
        const Prim& lp = kv.second;
        if ( (lp.type != PrimType::Light) || !lp.light || !lp.visible ) {
            continue;
        }
        bool hiddenL = false;
        for (std::string par = parentPath(kv.first); !par.empty(); par = parentPath(par)) {
            const Prim* pp = scene.find(par);
            if (pp && !pp->visible) {
                hiddenL = true;
                break;
            }
        }
        if (hiddenL) {
            continue;
        }
        const Light& L = *lp.light;
        stp.anyLight = true;
        const Vec3 col = L.color * L.intensity;
        if (L.kind == "ambient" || L.kind == "dome") {
            stp.ambientLight = stp.ambientLight + col;
            continue;
        }
        const Mat4 lw = scene.worldMatrix(kv.first);
        LightEval le;
        le.kind = (L.kind == "spot") ? 1 : (L.kind == "distant") ? 2 : 0;
        le.col = col;
        le.pos = lw.point( Vec3() );
        le.dir = normalize( lw.vector( Vec3(0, 0, -1) ) );
        le.falloff = L.falloff;
        const double half = L.coneAngle * 0.5, pen = std::max(0., L.conePenumbra) * 0.5;
        le.cosInner = std::cos( std::max(0., half - pen) * kPi / 180. );
        le.cosOuter = std::cos( std::min(89.9, half + pen) * kPi / 180. );
        le.shadows = L.castShadows && !casters.empty();
        le.samples = std::max(4, std::min(256, L.shadowSamples) );
        le.bias = std::max(0., L.shadowBias);
        le.size = (le.kind == 2) ? std::tan( std::max(0., L.shadowSoftness) * 0.5 * kPi / 180. ) : std::max(0., L.shadowSoftness);
        if (le.shadows) {
            const int R = std::max( 128, std::min(8192, L.shadowResolution) );
            // light frame without scale
            const Vec3 zAx = le.dir * -1.;
            const Vec3 up0 = std::fabs(zAx.y) < 0.99 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
            const Vec3 xAx = normalize( cross(up0, zAx) ), yAx = cross(zAx, xAx);
            auto frame = [](const Vec3& o, const Vec3& x, const Vec3& y, const Vec3& z) {
                Mat4 m;
                m.m[0][0] = x.x; m.m[1][0] = x.y; m.m[2][0] = x.z;
                m.m[0][1] = y.x; m.m[1][1] = y.y; m.m[2][1] = y.z;
                m.m[0][2] = z.x; m.m[1][2] = z.y; m.m[2][2] = z.z;
                m.m[0][3] = o.x; m.m[1][3] = o.y; m.m[2][3] = o.z;

                return m;
            };
            const Vec3 ext = casHi - casLo;
            const double sceneSize = std::max( 1e-3, std::sqrt( dot(ext, ext) ) );
            const double nearC = std::max(1e-3, sceneSize * 1e-4), farC2 = sceneSize * 4. + 1e4;
            if (le.kind == 2) {
                // distant: orthographic box around the casters, seen along the light
                const Mat4 lv = frame(Vec3(), xAx, yAx, zAx).inverse();
                Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
                for (int c8 = 0; c8 < 8; ++c8) {
                    const Vec3 cpt( (c8 & 1) ? casHi.x : casLo.x, (c8 & 2) ? casHi.y : casLo.y, (c8 & 4) ? casHi.z : casLo.z );
                    const Vec3 q = lv.point(cpt);
                    lo = Vec3( std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z) );
                    hi = Vec3( std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z) );
                }
                const double mx = std::max(hi.x - lo.x, hi.y - lo.y) * 0.02 + 1e-3;
                ShadowView sv;
                sv.ortho = true;
                sv.R = R;
                sv.n = 0.;
                sv.f = (hi.z - lo.z) + 2. * mx;
                // camera at the front (largest z) looking down -z of the light frame
                const Mat4 eye = Mat4::translate( Vec3(0, 0, hi.z + mx) );
                const Mat4 v2 = eye.inverse() * lv;
                const Mat4 P = orthoMatrix(lo.x - mx, hi.x + mx, lo.y - mx, hi.y + mx, sv.n, sv.f);
                sv.vp = P * v2;
                sv.k = P.m[0][0] * R * 0.5;
                sv.build(casters);
                le.views.push_back(sv);
            } else if (le.kind == 1) {
                ShadowView sv;
                sv.R = R;
                const double fov = std::min( 170., (half + pen) * 2. + 4. );
                const Mat4 P = perspMatrix(fov, nearC, farC2);
                sv.vp = P * frame(le.pos, xAx, yAx, zAx).inverse();
                sv.n = nearC; sv.f = farC2;
                sv.k = P.m[0][0] * R * 0.5;
                sv.build(casters);
                le.views.push_back(sv);
            } else {
                // point: cube of 6 perspective maps (+x -x +y -y +z -z)
                const Vec3 dirs[6] = { Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1) };
                const int Rp = std::max(128, R / 2);
                for (int fI = 0; fI < 6; ++fI) {
                    const Vec3 zz = dirs[fI] * -1.;
                    const Vec3 up = std::fabs(zz.y) < 0.99 ? Vec3(0, 1, 0) : Vec3(0, 0, 1);
                    const Vec3 xx = normalize( cross(up, zz) ), yy = cross(zz, xx);
                    ShadowView sv;
                    sv.R = Rp;
                    const Mat4 P = perspMatrix(92., nearC, farC2);
                    sv.vp = P * frame(le.pos, xx, yy, zz).inverse();
                    sv.n = nearC; sv.f = farC2;
                    sv.k = P.m[0][0] * Rp * 0.5;
                    sv.build(casters);
                    le.views.push_back(sv);
                }
            }
        }
        stp.lights.push_back(le);
    }

    stp.view = view;
    stp.viewProj = viewProj;
    stp.camPos = camPos;
    stp.tris.swap(tris);
    stp.occMaps.swap(occMaps);
    }; // buildStep

    // build each distinct step once (unchanged scenes share the same pointer)
    std::vector<Step> stepsData( scenes.size() );
    std::vector<int> stepIndex( scenes.size() );
    for (size_t i = 0; i < scenes.size(); ++i) {
        stepIndex[i] = (int)i;
        for (size_t j = 0; j < i; ++j) {
            if (scenes[j] == scenes[i]) {
                stepIndex[i] = stepIndex[j];
                break;
            }
        }
        if (stepIndex[i] == (int)i) {
            buildStep(*scenes[i], stepsData[i]);
        }
    }
    const int nSteps = (int)scenes.size();
    // motion vectors: the scene one frame later, same triangles
    Step nextStep;
    const bool wantMotion = aov.motion && aov.motionNext;
    if (wantMotion) {
        buildStep(*aov.motionNext, nextStep);
    }
    const bool dataAov = aov.position || aov.normal || aov.uv || aov.motion;
    std::vector<float> closest;
    if (dataAov) {
        closest.assign( npx, std::numeric_limits<float>::max() );
    }

    EnvMips env;
    if ( environment && (environment->w > 0) && (environment->h > 0) ) {
        env.build(*environment);
    }

    // samples shared by the motion blur steps: max(samples, steps) passes in total
    int grid = (int)std::ceil( std::sqrt( (double)std::max( std::max( std::max(1, s.samples), nSteps ), s.timeSamples ) ) - 1e-9 );
    grid = std::max(1, std::min(16, grid) );
    const int nS = grid * grid;
    // time step of each sample: stratified in time, decorrelated from the sub-pixel position
    std::vector<int> sampleStep(nS, 0);
    std::vector<double> sampleTime(nS, 0.5);
    {
        int P = 1;
        for (int c = (int)(nS * 0.618) | 1; c < nS + 1000; c += 2) {
            int a = c, b = nS;
            while (b) { const int t = a % b; a = b; b = t; }
            if (a == 1) { P = c % std::max(1, nS); break; }
        }
        for (int k = 0; k < nS; ++k) {
            const int perm = (int)( ( (long long)k * std::max(1, P) ) % nS );
            // continuous time in [0, 1]: stratified, small jitter inside the stratum
            sampleTime[k] = (perm + 0.25 + 0.5 * hash01(k, 7u, 13u) ) / nS;
            const int nearest = (int)std::lround( sampleTime[k] * (nSteps - 1) );
            sampleStep[k] = stepIndex[ std::max(0, std::min(nSteps - 1, nearest) ) ];
        }
    }
    std::vector<Tri> lerpTris;
    std::vector<float> shadowAcc, shadowSample;
    if (aov.shadow) {
        shadowAcc.assign( (size_t)W * H, 0.f );
        shadowSample.assign( (size_t)W * H, 0.f );
    }
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
        const Step& stp = stepsData[ sampleStep[si] ];
        // motion blur: every sample has its own time; geometry (and camera, through the screen positions) is
        // interpolated between the two evaluated steps around it when they match (same triangles)
        const std::vector<Tri>* trisPtr = &stp.tris;
        const std::vector<Tri>* velA = 0;   // motion between the two steps around this sample (smear)
        const std::vector<Tri>* velB = 0;
        double velScale = 0.;
        if (nSteps > 1) {
            const double u = std::min( 0.999999, sampleTime[si] ) * (nSteps - 1);
            const int i0 = std::min(nSteps - 2, (int)u);
            const double fr = u - i0;
            const Step& A = stepsData[ stepIndex[i0] ];
            const Step& B = stepsData[ stepIndex[i0 + 1] ];
            if ( (&A != &B) && (A.tris.size() == B.tris.size()) ) {
                lerpTris.resize( A.tris.size() );
                const double g = 1. - fr;
                for (size_t q = 0; q < A.tris.size(); ++q) {
                    const Tri& ta = A.tris[q];
                    const Tri& tb = B.tris[q];
                    Tri& o = lerpTris[q];
                    o = ta;
                    for (int v = 0; v < 3; ++v) {
                        o.P[v] = ta.P[v] * g + tb.P[v] * fr;
                        o.N[v] = ta.N[v] * g + tb.N[v] * fr;
                        o.sx[v] = ta.sx[v] * g + tb.sx[v] * fr;
                        o.sy[v] = ta.sy[v] * g + tb.sy[v] * fr;
                        o.iw[v] = ta.iw[v] * g + tb.iw[v] * fr;
                        o.zw[v] = ta.zw[v] * g + tb.zw[v] * fr;
                    }
                }
                trisPtr = &lerpTris;
                velA = &A.tris;
                velB = &B.tris;
                velScale = (double)(nSteps - 1);   // screen motion per unit of shutter time
            } else if (&A == &B) {
                trisPtr = &A.tris;
            }
        }
        const std::vector<Tri>& tris = *trisPtr;
        const Vec3& camPos = stp.camPos;
        const std::map<const Projection*, std::shared_ptr<OcclusionMap> >& occMaps = stp.occMaps;
        if (aov.shadow) {
            std::fill(shadowSample.begin(), shadowSample.end(), 0.f);
        }
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
                        if ( (t.lineMode == 1) && (edgeDistance(t, px, py) > kLineHalfWidth) ) {
                            continue; // wireframe: only the edges exist
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
                    if (aov.depth) {
                        const double zc = -stp.view.point(P).z;
                        const float dv = (float)( aov.depthInverse ? (zc > 1e-9 ? 1. / zc : 0.) : zc );
                        float& dd = (*aov.depth)[k];
                        // closest surface wins (1/z: largest)
                        if ( (dd == 0.f) || (aov.depthInverse ? dv > dd : dv < dd) ) {
                            dd = dv;
                        }
                    }
                    if (dot(N, V) < 0.) {
                        N = N * -1.;                         // two-sided
                    }
                    if (dataAov) {
                        const float zc = (float)( -stp.view.point(P).z );
                        if ( zc < closest[k] ) {
                            closest[k] = zc;
                            if (aov.position) {
                                float* d = &(*aov.position)[k * 3];
                                d[0] = (float)P.x; d[1] = (float)P.y; d[2] = (float)P.z;
                            }
                            if (aov.normal) {
                                float* d = &(*aov.normal)[k * 3];
                                d[0] = (float)N.x; d[1] = (float)N.y; d[2] = (float)N.z;
                            }
                            if (aov.uv) {
                                float* d = &(*aov.uv)[k * 2];
                                d[0] = (float)uv.x; d[1] = (float)uv.y;
                            }
                            if (wantMotion && ( nextStep.tris.size() == tris.size() ) ) {
                                const Tri& tn = nextStep.tris[ti];
                                // screen barycentrics (not perspective-corrected) for the screen motion
                                float* d = &(*aov.motion)[k * 2];
                                double mx = 0., my = 0.;
                                const double bb[3] = { b0, b1, b2 };
                                for (int v = 0; v < 3; ++v) {
                                    mx += bb[v] * (tn.sx[v] - t.sx[v]);
                                    my += bb[v] * (tn.sy[v] - t.sy[v]);
                                }
                                d[0] = (float)mx; d[1] = (float)my;
                            }
                        }
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
                        if (!stp.anyLight || mt.emission >= 1.) {
                            // no lights: headlight lambert for the lit part
                            const float lam = (float)std::max( s.ambient, dot(N, V) );
                            const float lit = (float)( mt.emission + (1. - mt.emission) * lam );
                            for (int ch = 0; ch < 3; ++ch) {
                                c[ch] = base[ch] * bc[ch] * lit;
                            }
                        } else {
                            // lights: lambert + normalised Blinn-Phong, soft shadows
                            Vec3 dif = stp.ambientLight, spc;
                            double litLum = 0., unlitLum = 0.;
                            const double rough = std::max(0.02, mt.roughness);
                            const double shin = std::max( 1., 2. / std::pow(rough, 4.) - 2. );
                            const double rot = hash01( (unsigned)x, (unsigned)y, (unsigned)si ) * 6.283185307;
                            for (const LightEval& le : stp.lights) {
                                Vec3 L;
                                double att = 1.;
                                if (le.kind == 2) {
                                    L = le.dir * -1.;
                                } else {
                                    const Vec3 d = le.pos - P;
                                    const double dist = std::max( 1e-6, std::sqrt( dot(d, d) ) );
                                    L = d * (1. / dist);
                                    att = (le.falloff == 1) ? 1. / dist : (le.falloff == 2) ? 1. / (dist * dist) : 1.;
                                    if (le.kind == 1) {
                                        const double ca = dot(L * -1., le.dir);
                                        const double tt = (ca - le.cosOuter) / std::max(1e-6, le.cosInner - le.cosOuter);
                                        const double sm = std::max( 0., std::min(1., tt) );
                                        att *= sm * sm * (3. - 2. * sm);
                                    }
                                }
                                const double ndl = dot(N, L);
                                if ( (ndl <= 0.) || (att <= 0.) ) {
                                    continue;
                                }
                                const double vis = t.receiveShadow ? le.visibility(P, ndl, rot) : 1.;
                                const double w = ndl * att;
                                const double lum = (le.col.x + le.col.y + le.col.z) * w;
                                unlitLum += lum;
                                litLum += lum * vis;
                                dif = dif + le.col * (w * vis);
                                if (mt.specular > 0. || mt.metalness > 0.) {
                                    const Vec3 Hh = normalize(L + V);
                                    const double nh = std::max( 0., dot(N, Hh) );
                                    const double sp = std::pow(nh, shin) * (shin + 8.) / 25.1327 * att * vis * ndl;
                                    spc = spc + le.col * sp;
                                }
                            }
                            if ( !shadowSample.empty() && (unlitLum > 1e-12) ) {
                                shadowSample[k] = (float)( 1. - litLum / unlitLum );
                            }
                            const double em = mt.emission, kd = (1. - em) * (1. - mt.metalness);
                            const double ks = (1. - em) * std::max(mt.specular, mt.metalness);
                            const double dl[3] = { dif.x, dif.y, dif.z }, sl[3] = { spc.x, spc.y, spc.z };
                            for (int ch = 0; ch < 3; ++ch) {
                                const double albedo = base[ch] * bc[ch];
                                // metals tint the highlight with their colour (premultiplied by alpha)
                                const double sc = (1. - mt.metalness) * a + mt.metalness * albedo;
                                c[ch] = (float)( albedo * (em + kd * dl[ch]) + ks * sl[ch] * sc );
                            }
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
                    // wireframe / lines: rgba output only
                    if ( (t.lineMode != 0) && (s.output == ScanlineOutput::RGBA) ) {
                        const double pxs = x + ox, pys = y + oy;
                        if (t.lineMode == 1) {
                            // wireframe: flat lines in the base colour
                            const Vec3& bcol = t.mesh->material.baseColor;
                            c[0] = (float)bcol.x; c[1] = (float)bcol.y; c[2] = (float)bcol.z; c[3] = 1.f;
                        } else if (edgeDistance(t, pxs, pys) <= kLineHalfWidth) {
                            c[0] = c[1] = c[2] = 0.02f;
                            c[3] = 1.f;
                        }
                    }
                    if ( (s.output == ScanlineOutput::RGBA) && (t.mesh->material.opacity < 1.) ) {
                        const float op = (float)std::max(0., t.mesh->material.opacity);
                        for (int ch = 0; ch < 4; ++ch) {
                            c[ch] *= op;
                        }
                    }
                    float* o = &sampleCol[k * 4];
                    for (int ch = 0; ch < 4; ++ch) {
                        o[ch] = c[ch];
                    }
                }
            }
        });
        // motion blur smear: each time sample covers 1 / nS of the shutter, so its colour is spread along the
        // screen motion over that slice. Few samples then give a continuous streak instead of separate copies.
        // Premultiplied colours scatter exactly; a target closer to the camera than the source keeps its own.
        if ( velA && (nS > 1) ) {
            std::vector<float> sm( sampleCol.size(), 0.f );
            std::vector<float> stat( sampleCol.size(), 0.f ); // surfaces that do not move in this slice
            std::vector<float> movedLen( triId.size(), 0.f ), movedDir( triId.size() * 2, 0.f );
            const double slice = 1. / nS;
            for (size_t k = 0; k < triId.size(); ++k) {
                if (triId[k] < 0) {
                    // nothing rendered here: keep what the shading pass wrote (transparent)
                    for (int ch = 0; ch < 4; ++ch) {
                        stat[k * 4 + ch] = sampleCol[k * 4 + ch];
                    }
                }
            }
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    const size_t k = (size_t)y * W + x;
                    const int ti = triId[k];
                    const float* c = &sampleCol[k * 4];
                    if ( (ti < 0) || ( (size_t)ti >= velA->size() ) ) {
                        continue;
                    }
                    const Tri& ta = (*velA)[ti];
                    const Tri& tb = (*velB)[ti];
                    const double b0 = bary[k * 2], b1 = bary[k * 2 + 1], b2 = 1. - b0 - b1;
                    const double vx = ( b0 * (tb.sx[0] - ta.sx[0]) + b1 * (tb.sx[1] - ta.sx[1]) + b2 * (tb.sx[2] - ta.sx[2]) ) * velScale * slice;
                    const double vy = ( b0 * (tb.sy[0] - ta.sy[0]) + b1 * (tb.sy[1] - ta.sy[1]) + b2 * (tb.sy[2] - ta.sy[2]) ) * velScale * slice;
                    const double len = std::sqrt(vx * vx + vy * vy);
                    if (len < 0.75) {
                        float* d = &stat[k * 4];
                        d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = c[3];
                        continue;
                    }
                    movedLen[k] = (float)len;
                    movedDir[k * 2] = (float)(vx / len);
                    movedDir[k * 2 + 1] = (float)(vy / len);
                    const int K = std::min( 64, (int)std::ceil(len * 1.5) );
                    const float wk = 1.f / K;
                    const float zs = depth[k];
                    for (int q = 0; q < K; ++q) {
                        const double f = ( (q + 0.5) / K ) - 0.5;
                        const int xx = (int)std::floor(x + 0.5 + vx * f), yy = (int)std::floor(y + 0.5 + vy * f);
                        if ( (xx < 0) || (xx >= W) || (yy < 0) || (yy >= H) ) {
                            continue;
                        }
                        const size_t kk = (size_t)yy * W + xx;
                        if ( (triId[kk] >= 0) && (depth[kk] < zs - 1e-5f) && (tris[triId[kk]].mesh != tris[ti].mesh) ) {
                            continue;   // occluded by a closer surface at that pixel
                        }
                        float* d = &sm[kk * 4];
                        d[0] += c[0] * wk; d[1] += c[1] * wk; d[2] += c[2] * wk; d[3] += c[3] * wk;
                    }
                }
            }
            // what is behind a moving surface is unknown in this pass: borrow the nearest still surface along the
            // motion (the background the streak passes over), so the streak does not show holes
            {
                std::vector<float> fill(stat);
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        const size_t k = (size_t)y * W + x;
                        const int ti = triId[k];
                        if ( (ti < 0) || (movedLen[k] <= 0.f) ) {
                            continue;
                        }
                        const double ux = movedDir[k * 2], uy = movedDir[k * 2 + 1];
                        float accC[4] = { 0, 0, 0, 0 };
                        int found = 0;
                        for (int sgn = -1; sgn <= 1; sgn += 2) {
                            for (int r = 1; r <= 160; ++r) {
                                const int xx = (int)std::floor(x + 0.5 + sgn * ux * r), yy = (int)std::floor(y + 0.5 + sgn * uy * r);
                                if ( (xx < 0) || (xx >= W) || (yy < 0) || (yy >= H) ) {
                                    break;
                                }
                                const size_t kk = (size_t)yy * W + xx;
                                if ( (triId[kk] < 0) || (movedLen[kk] <= 0.f) ) {
                                    for (int ch = 0; ch < 4; ++ch) {
                                        accC[ch] += stat[kk * 4 + ch];
                                    }
                                    ++found;
                                    break;
                                }
                            }
                        }
                        if (found) {
                            for (int ch = 0; ch < 4; ++ch) {
                                fill[k * 4 + ch] = accC[ch] / found;
                            }
                        }
                    }
                }
                stat.swap(fill);
            }
            // moving streaks over the still surfaces
            for (size_t k = 0; k < triId.size(); ++k) {
                float* d = &sm[k * 4];
                const float* st0 = &stat[k * 4];
                const float a = std::min(1.f, d[3]);
                for (int ch = 0; ch < 4; ++ch) {
                    d[ch] = std::min(ch == 3 ? 1.f : 1e30f, d[ch]) + st0[ch] * (1.f - a);
                }
            }
            sampleCol.swap(sm);
        }
        // filter pass (gather): every pixel takes the weighted samples around it
        runRows([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < W; ++x) {
                    float acc[4] = { 0, 0, 0, 0 };
                    float accS = 0.f;
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
                            if (!shadowSample.empty()) {
                                accS += (float)w * shadowSample[(size_t)yy * W + xx];
                            }
                            ws += (float)w;
                        }
                    }
                    const size_t k = (size_t)y * W + x;
                    float* o = &(*out)[k * 4];
                    for (int ch = 0; ch < 4; ++ch) {
                        o[ch] += acc[ch];
                    }
                    if (!shadowAcc.empty()) {
                        shadowAcc[k] += accS;
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
        if (aov.shadow) {
            (*aov.shadow)[k] = w != 0.f ? shadowAcc[k] / w : 0.f;
        }
    }
}
} // namespace Solv3D
