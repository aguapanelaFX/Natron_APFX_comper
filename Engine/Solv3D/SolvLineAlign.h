/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// Single-frame camera alignment from lines (VanishingCamera).
// Each line is parallel to a world axis (X, Y or Z), optionally lying ON that axis (its arrow = + direction,
// optional real length = scene scale). The 3D direction of an axis must lie in the plane through the camera
// centre and the image line: residual n . d_axis. Rotation (+ focal when free / approximate) is solved by a
// coarse search + Levenberg-Marquardt. Signs: on-axis arrows, else Y up in the image and X to the right;
// flip Y / flip Z switch to the other orientations that fit the same lines.
// Image coordinates: pixels, y up. Camera: x right, y up, looking down -Z.

#ifndef SOLVFX_SOLVLINEALIGN_H
#define SOLVFX_SOLVLINEALIGN_H

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "SolvScene.h"

namespace SolvAlign {

using Solv3D::Vec3;
using Solv3D::Mat4;

struct Line
{
    double x1, y1, x2, y2;
    int axis = 0;          // 0 X, 1 Y, 2 Z
    bool onAxis = false;   // lies on the axis (arrow x1,y1 -> x2,y2 = + direction)
    double length = 0.;    // real length (scene units), 0 = unknown
};

enum FocalMode { eFocalSolve = 0, eFocalKnown, eFocalApprox };

struct Input
{
    std::vector<Line> lines;
    double cx = 0., cy = 0.;       // lens centre (pixels)
    double pxPerMM = 1.;           // image width (px) / film back width (mm)
    int focalMode = eFocalSolve;
    double focalMM = 35., tolMM = 10.;
    bool hasOrigin = false;
    double ox = 0., oy = 0.;       // origin point hint (pixels)
    double distance = 500.;        // camera to origin when no line has a length
    bool flipY = false, flipZ = false;
};

struct Output
{
    bool ok = false;
    std::string error;
    double focalMM = 0.;
    double rmsPx = 0.;
    Mat4 worldFromCam;             // rotation only
    Vec3 camPos;
    Vec3 rotateXYZ;                // degrees, rotation order XYZ (R = Rz * Ry * Rx)
    double distance = 0.;
};

namespace detail {
inline Vec3 ray(double x, double y, double cx, double cy, double f)
{
    return Vec3( (x - cx) / f, (y - cy) / f, -1. );
}

inline Vec3 cross3(const Vec3& a, const Vec3& b)
{
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

inline double dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 unit(const Vec3& a)
{
    const double l = std::sqrt( dot3(a, a) );

    return l > 1e-300 ? a * (1. / l) : a;
}

// 3x3 rotation stored in a Mat4 (columns = world axes in camera space)
inline Mat4 rotFromVector(const Vec3& w)
{
    const double th = std::sqrt( dot3(w, w) );
    Mat4 r;
    if (th < 1e-12) {
        return r;
    }
    const Vec3 k = w * (1. / th);
    const double c = std::cos(th), s = std::sin(th), v = 1. - c;
    r.m[0][0] = k.x * k.x * v + c;       r.m[0][1] = k.x * k.y * v - k.z * s; r.m[0][2] = k.x * k.z * v + k.y * s;
    r.m[1][0] = k.y * k.x * v + k.z * s; r.m[1][1] = k.y * k.y * v + c;       r.m[1][2] = k.y * k.z * v - k.x * s;
    r.m[2][0] = k.z * k.x * v - k.y * s; r.m[2][1] = k.z * k.y * v + k.x * s; r.m[2][2] = k.z * k.z * v + c;

    return r;
}

inline Vec3 col(const Mat4& R, int a) { return Vec3(R.m[0][a], R.m[1][a], R.m[2][a]); }

inline void setCol(Mat4* R, int a, const Vec3& v) { R->m[0][a] = v.x; R->m[1][a] = v.y; R->m[2][a] = v.z; }

struct Problem
{
    const Input* in;
    bool freeF;
    double f0px, tolPx;

    // residuals for rotation R and focal f (pixels)
    void residuals(const Mat4& R, double f, std::vector<double>* r) const
    {
        r->clear();
        for (const Line& l : in->lines) {
            const Vec3 n = unit( cross3( ray(l.x1, l.y1, in->cx, in->cy, f), ray(l.x2, l.y2, in->cx, in->cy, f) ) );
            r->push_back( dot3( n, col(R, l.axis) ) );
        }
        if ( freeF && (in->focalMode == eFocalApprox) ) {
            // prior: one tolerance ~ one pixel of angular error
            r->push_back( (f - f0px) / std::max(1e-9, tolPx) / std::max(1., f0px) );
        }
    }

    double cost(const Mat4& R, double f) const
    {
        std::vector<double> r;
        residuals(R, f, &r);
        double c = 0.;
        for (double v : r) {
            c += v * v;
        }

        return c;
    }

    // Levenberg-Marquardt on (rotation vector, log f)
    void refine(Mat4* R, double* f) const
    {
        const int np = freeF ? 4 : 3;
        double lambda = 1e-3;
        double c = cost(*R, *f);
        for (int it = 0; it < 100; ++it) {
            std::vector<double> r0;
            residuals(*R, *f, &r0);
            const int m = (int)r0.size();
            std::vector<double> J( (size_t)m * np );
            const double h = 1e-6;
            for (int p = 0; p < np; ++p) {
                Mat4 Rp = *R;
                double fp = *f;
                if (p < 3) {
                    Vec3 w;
                    (p == 0 ? w.x : (p == 1 ? w.y : w.z) ) = h;
                    Rp = rotFromVector(w) * *R;
                } else {
                    fp = *f * std::exp(h);
                }
                std::vector<double> r1;
                residuals(Rp, fp, &r1);
                for (int i = 0; i < m; ++i) {
                    J[(size_t)i * np + p] = (r1[i] - r0[i]) / h;
                }
            }
            // normal equations
            double A[4][4] = { { 0 } }, g[4] = { 0 };
            for (int i = 0; i < m; ++i) {
                for (int a = 0; a < np; ++a) {
                    g[a] += J[(size_t)i * np + a] * r0[i];
                    for (int b = 0; b < np; ++b) {
                        A[a][b] += J[(size_t)i * np + a] * J[(size_t)i * np + b];
                    }
                }
            }
            bool improved = false;
            for (int tries = 0; tries < 10 && !improved; ++tries) {
                double M[4][5];
                for (int a = 0; a < np; ++a) {
                    for (int b = 0; b < np; ++b) {
                        M[a][b] = A[a][b] + (a == b ? lambda * (A[a][a] + 1e-12) : 0.);
                    }
                    M[a][np] = -g[a];
                }
                // Gauss-Jordan
                bool sing = false;
                for (int a = 0; a < np && !sing; ++a) {
                    int piv = a;
                    for (int b = a + 1; b < np; ++b) {
                        if ( std::fabs(M[b][a]) > std::fabs(M[piv][a]) ) {
                            piv = b;
                        }
                    }
                    if (std::fabs(M[piv][a]) < 1e-30) {
                        sing = true;
                        break;
                    }
                    for (int k = 0; k <= np; ++k) {
                        std::swap(M[a][k], M[piv][k]);
                    }
                    for (int b = 0; b < np; ++b) {
                        if (b == a) {
                            continue;
                        }
                        const double fct = M[b][a] / M[a][a];
                        for (int k = a; k <= np; ++k) {
                            M[b][k] -= fct * M[a][k];
                        }
                    }
                }
                if (sing) {
                    lambda *= 10.;
                    continue;
                }
                double d[4] = { 0 };
                for (int a = 0; a < np; ++a) {
                    d[a] = M[a][np] / M[a][a];
                }
                const Mat4 Rn = rotFromVector( Vec3(d[0], d[1], d[2]) ) * *R;
                const double fn = freeF ? *f * std::exp( std::max( -1., std::min(1., d[3]) ) ) : *f;
                const double cn = cost(Rn, fn);
                if (cn < c) {
                    *R = Rn;
                    *f = fn;
                    c = cn;
                    lambda = std::max(1e-9, lambda * 0.3);
                    improved = true;
                } else {
                    lambda *= 10.;
                }
            }
            if (!improved || c < 1e-24) {
                break;
            }
        }
    }
};

inline Mat4 eulerYXZ(double yaw, double pitch, double roll)
{
    return Mat4::rotateXYZ( Vec3(0, yaw, 0) ) * Mat4::rotateXYZ( Vec3(pitch, 0, 0) ) * Mat4::rotateXYZ( Vec3(0, 0, roll) );
}

// orthonormalise the rotation (columns), right handed
inline Mat4 cleanRotation(const Mat4& R)
{
    Vec3 x = unit( col(R, 0) );
    Vec3 y = col(R, 1);
    y = unit( y - x * dot3(x, y) );
    Vec3 z = cross3(x, y);
    Mat4 o;
    setCol(&o, 0, x);
    setCol(&o, 1, y);
    setCol(&o, 2, z);

    return o;
}

// image projection of camera-space point
inline bool project(const Vec3& P, double f, double cx, double cy, double* x, double* y)
{
    if (P.z > -1e-12) {
        return false;
    }
    *x = cx + f * P.x / -P.z;
    *y = cy + f * P.y / -P.z;

    return true;
}

// closest point parameter t on line A + t * dA to line B + s * dB
inline double closestT(const Vec3& A, const Vec3& dA, const Vec3& B, const Vec3& dB)
{
    const Vec3 w = A - B;
    const double a = dot3(dA, dA), b = dot3(dA, dB), c = dot3(dB, dB), d = dot3(dA, w), e = dot3(dB, w);
    const double den = a * c - b * b;
    if (std::fabs(den) < 1e-18) {
        return 0.;
    }

    return (b * e - c * d) / den;
}
} // namespace detail

inline Output
solve(const Input& in)
{
    using namespace detail;
    Output out;
    int perAxis[3] = { 0, 0, 0 };
    for (const Line& l : in.lines) {
        if ( (l.axis >= 0) && (l.axis < 3) ) {
            ++perAxis[l.axis];
        }
    }
    const int axesUsed = (perAxis[0] > 0) + (perAxis[1] > 0) + (perAxis[2] > 0);
    const int nLines = (int)in.lines.size();
    if (axesUsed < 2) {
        out.error = "Lines on at least two different axes are needed.";

        return out;
    }
    const bool freeF = in.focalMode != eFocalKnown;
    const int need = (in.focalMode == eFocalSolve) ? 4 : 3;
    if (nLines < need) {
        out.error = (in.focalMode == eFocalSolve) ?
                    "Free focal: at least 4 lines (two parallel lines on one axis + two more), or use known / approximate focal." :
                    "At least 3 lines are needed.";

        return out;
    }
    if ( (in.focalMode == eFocalSolve) && (std::max( perAxis[0], std::max(perAxis[1], perAxis[2]) ) < 2) ) {
        out.error = "Free focal: one axis needs at least two parallel lines.";

        return out;
    }
    Problem pb;
    pb.in = &in;
    pb.freeF = freeF;
    pb.f0px = in.focalMM * in.pxPerMM;
    pb.tolPx = std::max(0.01, in.tolMM) * in.pxPerMM;

    // coarse search over orientations (and focal), then refine the best candidates
    std::vector<double> focals;
    if (in.focalMode == eFocalSolve) {
        // 35 mm-equivalent 10 .. 300 mm on a 36 mm film back
        const double wPx = 36. * in.pxPerMM;
        for (int i = 0; i < 10; ++i) {
            focals.push_back( wPx / 36. * 10. * std::pow(30., i / 9.) );
        }
    } else {
        focals.push_back(pb.f0px);
    }
    struct Cand { double c; Mat4 R; double f; };
    std::vector<Cand> cands;
    for (double f : focals) {
        for (int yi = 0; yi < 12; ++yi) {
            for (int pi = -6; pi <= 6; ++pi) {
                for (int ri = -6; ri <= 6; ++ri) {
                    const Mat4 R = eulerYXZ(yi * 15., pi * 15., ri * 15.);
                    const double c = pb.cost(R, f);
                    cands.push_back( { c, R, f } );
                }
            }
        }
    }
    const size_t keep = std::min<size_t>(12, cands.size());
    std::partial_sort( cands.begin(), cands.begin() + keep, cands.end(), [](const Cand& a, const Cand& b) { return a.c < b.c; } );
    Mat4 bestR;
    double bestF = pb.f0px, bestC = 1e300;
    for (size_t i = 0; i < keep; ++i) {
        Mat4 R = cands[i].R;
        double f = cands[i].f;
        pb.refine(&R, &f);
        const double c = pb.cost(R, f);
        if ( (c < bestC) && (f > 1e-6) ) {
            bestC = c;
            bestR = R;
            bestF = f;
        }
    }
    Mat4 R = cleanRotation(bestR);
    const double f = bestF;

    // ---- signs
    Vec3 d[3] = { col(R, 0), col(R, 1), col(R, 2) };
    int fixedBy[3] = { 0, 0, 0 }; // 1 = set by an on-axis arrow
    for (const Line& l : in.lines) {
        if ( !l.onAxis || fixedBy[l.axis] ) {
            continue;
        }
        // move along +d from the start of the line: the image must move towards the arrow
        const Vec3 X1 = ray(l.x1, l.y1, in.cx, in.cy, f) * 10.;
        const Vec3 X2 = X1 + d[l.axis] * 0.01;
        double ax, ay, bx, by;
        if ( project(X1, f, in.cx, in.cy, &ax, &ay) && project(X2, f, in.cx, in.cy, &bx, &by) ) {
            if ( (bx - ax) * (l.x2 - l.x1) + (by - ay) * (l.y2 - l.y1) < 0. ) {
                d[l.axis] = d[l.axis] * -1.;
            }
            fixedBy[l.axis] = 1;
        }
    }
    if ( !fixedBy[1] && (d[1].y < 0.) ) {
        d[1] = d[1] * -1.; // Y up in the image
    }
    if ( !fixedBy[0] && (d[0].x < 0.) ) {
        d[0] = d[0] * -1.; // X to the right
    }
    // right handed: Z = X x Y; respect a Z arrow by flipping a free axis
    const Vec3 z = cross3(d[0], d[1]);
    if ( fixedBy[2] && (dot3(z, d[2]) < 0.) ) {
        if (!fixedBy[0]) {
            d[0] = d[0] * -1.;
        } else if (!fixedBy[1]) {
            d[1] = d[1] * -1.;
        }
    }
    d[2] = cross3(d[0], d[1]);
    if (in.flipY) { // 180 deg around Z: upside down <-> upright
        d[0] = d[0] * -1.;
        d[1] = d[1] * -1.;
    }
    if (in.flipZ) { // 180 deg around Y: facing the scene <-> facing away
        d[0] = d[0] * -1.;
        d[2] = d[2] * -1.;
    }
    Mat4 camFromWorld;
    for (int a = 0; a < 3; ++a) {
        setCol(&camFromWorld, a, d[a]);
    }
    const Mat4 worldFromCam = camFromWorld.transposed();

    // ---- origin (image point)
    double ox = in.lines[0].x1, oy = in.lines[0].y1;
    if (in.hasOrigin) {
        ox = in.ox;
        oy = in.oy;
    } else {
        // least squares crossing of the on-axis lines
        double A00 = 0, A01 = 0, A11 = 0, b0 = 0, b1 = 0;
        int n = 0;
        const Line* firstOn = 0;
        for (const Line& l : in.lines) {
            if (!l.onAxis) {
                continue;
            }
            if (!firstOn) {
                firstOn = &l;
            }
            double nx = -(l.y2 - l.y1), ny = l.x2 - l.x1;
            const double nl = std::sqrt(nx * nx + ny * ny);
            if (nl < 1e-9) {
                continue;
            }
            nx /= nl; ny /= nl;
            const double c = nx * l.x1 + ny * l.y1;
            A00 += nx * nx; A01 += nx * ny; A11 += ny * ny; b0 += nx * c; b1 += ny * c;
            ++n;
        }
        const double det = A00 * A11 - A01 * A01;
        if ( (n >= 2) && (std::fabs(det) > 1e-9) ) {
            ox = (A11 * b0 - A01 * b1) / det;
            oy = (A00 * b1 - A01 * b0) / det;
        } else if (firstOn) {
            ox = firstOn->x1;
            oy = firstOn->y1;
        }
    }
    const Vec3 oDir = unit( ray(ox, oy, in.cx, in.cy, f) );
    // ---- scale: a line with a real length
    double D = in.distance;
    for (const Line& l : in.lines) {
        if ( (l.length <= 0.) || !l.onAxis ) {
            continue;
        }
        // with the origin at distance 1, find the 3D points of the line ends on the axis
        const Vec3 O1 = oDir;
        const Vec3 da = d[l.axis];
        const double t1 = closestT( O1, da, Vec3(), ray(l.x1, l.y1, in.cx, in.cy, f) );
        const double t2 = closestT( O1, da, Vec3(), ray(l.x2, l.y2, in.cx, in.cy, f) );
        if (std::fabs(t2 - t1) > 1e-9) {
            D = l.length / std::fabs(t2 - t1);
        }
        break;
    }
    const Vec3 originCam = oDir * D;
    out.camPos = worldFromCam.vector(originCam) * -1.;
    out.worldFromCam = worldFromCam;
    out.distance = D;
    // euler XYZ (R = Rz * Ry * Rx)
    const double toDeg = 180. / 3.14159265358979323846;
    const double ry = -std::asin( std::max( -1., std::min(1., worldFromCam.m[2][0]) ) );
    const double rx = std::atan2(worldFromCam.m[2][1], worldFromCam.m[2][2]);
    const double rz = std::atan2(worldFromCam.m[1][0], worldFromCam.m[0][0]);
    out.rotateXYZ = Vec3(rx * toDeg, ry * toDeg, rz * toDeg);
    out.focalMM = f / in.pxPerMM;

    // ---- error: distance of each line end to the line through the other end and the vanishing point
    double se = 0.;
    int ne = 0;
    for (const Line& l : in.lines) {
        const Vec3 da = d[l.axis];
        double vx, vy;
        double dirx, diry;
        if (std::fabs(da.z) > 1e-9) {
            vx = in.cx + f * da.x / -da.z;
            vy = in.cy + f * da.y / -da.z;
            (void)vx;
        }
        for (int e = 0; e < 2; ++e) {
            const double px = e ? l.x2 : l.x1, py = e ? l.y2 : l.y1;
            const double qx = e ? l.x1 : l.x2, qy = e ? l.y1 : l.y2;
            if (std::fabs(da.z) > 1e-9) {
                vx = in.cx + f * da.x / -da.z;
                vy = in.cy + f * da.y / -da.z;
                dirx = vx - px;
                diry = vy - py;
            } else {
                dirx = da.x;
                diry = da.y;
            }
            const double dl = std::sqrt(dirx * dirx + diry * diry);
            if (dl < 1e-12) {
                continue;
            }
            const double dist = std::fabs( (qx - px) * diry - (qy - py) * dirx ) / dl;
            se += dist * dist;
            ++ne;
        }
    }
    out.rmsPx = ne ? std::sqrt(se / ne) : 0.;
    out.ok = true;

    return out;
}

} // namespace SolvAlign

#endif // SOLVFX_SOLVLINEALIGN_H
