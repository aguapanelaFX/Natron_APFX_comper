/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX 3D: camera from vanishing lines (single image calibration, fSpy-like).
// Header only, no Natron / USD dependency (unit-testable).
//
// Input: for 2 or 3 mutually orthogonal world directions, two image lines each (pixels).
// Each pair of lines meets at a vanishing point v_i. With K = [f 0 px; 0 f py; 0 0 1]:
//   orthogonal directions i, j:  (v_i - p) . (v_j - p) + f^2 = 0
//   3 vanishing points -> principal point = orthocentre of the triangle (v1 v2 v3), then f
//   2 vanishing points -> principal point given (image centre by default), then f
// Rotation: column i of R (camera <- world) = normalize(K^-1 v_i); the 3rd column = cross of the first two
// (sign chosen so the axes form a right-handed frame).

#ifndef SOLVFX_SOLVVANISHINGCAMERA_H
#define SOLVFX_SOLVVANISHINGCAMERA_H

#include <cmath>

namespace SolvVP {

struct V2 { double x, y; };
struct V3 { double x, y, z; };
struct Line { V2 a, b; };

inline V3 cross(const V3& a, const V3& b) { return V3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 normalize(const V3& v) { const double l = std::sqrt( dot(v, v) ); return l > 0 ? V3{ v.x / l, v.y / l, v.z / l } : v; }

// intersection of the two lines (homogeneous); false if parallel (vanishing point at infinity)
inline bool vanishingPoint(const Line& l1, const Line& l2, V2* v)
{
    const V3 h1 = cross( V3{ l1.a.x, l1.a.y, 1. }, V3{ l1.b.x, l1.b.y, 1. } );
    const V3 h2 = cross( V3{ l2.a.x, l2.a.y, 1. }, V3{ l2.b.x, l2.b.y, 1. } );
    const V3 p = cross(h1, h2);
    if (std::fabs(p.z) < 1e-12) {
        return false;
    }
    v->x = p.x / p.z;
    v->y = p.y / p.z;

    return true;
}

// orthocentre of triangle abc (= principal point for 3 orthogonal vanishing points)
inline bool orthocentre(const V2& a, const V2& b, const V2& c, V2* h)
{
    // altitudes: (h - a).(b - c) = 0 and (h - b).(a - c) = 0
    const double a1 = b.x - c.x, b1 = b.y - c.y, c1 = a.x * a1 + a.y * b1;
    const double a2 = a.x - c.x, b2 = a.y - c.y, c2 = b.x * a2 + b.y * b2;
    const double det = a1 * b2 - a2 * b1;
    if (std::fabs(det) < 1e-12) {
        return false;
    }
    h->x = (c1 * b2 - c2 * b1) / det;
    h->y = (a1 * c2 - a2 * c1) / det;

    return true;
}

struct Result
{
    bool ok = false;
    double f = 0.;          // focal length in pixels
    V2 principal{ 0, 0 };   // principal point (pixels)
    double R[3][3];         // camera <- world rotation (columns = world X, Y, Z axes in camera space)
    const char* error = "";
};

// vps: 2 or 3 vanishing points of orthogonal directions (world X, Y[, Z]); principal used when n == 2.
// dirs (optional): one line per axis drawn from a to b in the +axis direction. A point moving along +axis
// approaches the vanishing point when the axis points away from the camera, so the sign of
// (vp - a).(b - a) gives the axis sign (otherwise the sign is ambiguous).
inline Result solve(const V2* vps, int n, V2 principal, const Line* dirs = 0)
{
    Result r;
    if ( (n == 3) && !orthocentre(vps[0], vps[1], vps[2], &principal) ) {
        r.error = "vanishing points are collinear";

        return r;
    }
    const double d = (vps[0].x - principal.x) * (vps[1].x - principal.x) + (vps[0].y - principal.y) * (vps[1].y - principal.y);
    if (d >= 0.) {
        r.error = "lines are not consistent with orthogonal directions (focal length would be imaginary)";

        return r;
    }
    r.f = std::sqrt(-d);
    r.principal = principal;
    // directions in camera space: K^-1 v = ((vx - px) / f, (vy - py) / f, 1)
    V3 c[3];
    for (int i = 0; i < 2; ++i) {
        c[i] = normalize( V3{ (vps[i].x - principal.x) / r.f, (vps[i].y - principal.y) / r.f, 1. } );
        if (dirs) {
            const double sgn = (vps[i].x - dirs[i].a.x) * (dirs[i].b.x - dirs[i].a.x) + (vps[i].y - dirs[i].a.y) * (dirs[i].b.y - dirs[i].a.y);
            if (sgn < 0.) {
                c[i] = V3{ -c[i].x, -c[i].y, -c[i].z };
            }
        }
    }
    // re-orthogonalise the second axis against the first (noise in the lines)
    c[1] = normalize( V3{ c[1].x - dot(c[0], c[1]) * c[0].x, c[1].y - dot(c[0], c[1]) * c[0].y, c[1].z - dot(c[0], c[1]) * c[0].z } );
    c[2] = cross(c[0], c[1]);
    for (int i = 0; i < 3; ++i) {
        r.R[0][i] = (i == 0 ? c[0].x : i == 1 ? c[1].x : c[2].x);
        r.R[1][i] = (i == 0 ? c[0].y : i == 1 ? c[1].y : c[2].y);
        r.R[2][i] = (i == 0 ? c[0].z : i == 1 ? c[1].z : c[2].z);
    }
    r.ok = true;

    return r;
}

// focal length in mm for a sensor width (mm) and image width (pixels)
inline double focalMM(double fPixels, double sensorWidthMM, double imageWidthPx)
{
    return fPixels * sensorWidthMM / imageWidthPx;
}
} // namespace SolvVP

#endif // SOLVFX_SOLVVANISHINGCAMERA_H
