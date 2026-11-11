/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include "SolvScene.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Solv3D {

static const double kPi = 3.14159265358979323846;

double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) { return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
Vec3 normalize(const Vec3& v)
{
    const double l = std::sqrt( dot(v, v) );

    return l > 0. ? v * (1. / l) : v;
}

// ------------------------------------------------------------------ Mat4
Mat4::Mat4()
{
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[i][j] = (i == j) ? 1. : 0.;
        }
    }
}

Mat4 Mat4::translate(const Vec3& t)
{
    Mat4 r;
    r.m[0][3] = t.x; r.m[1][3] = t.y; r.m[2][3] = t.z;

    return r;
}

Mat4 Mat4::scale(const Vec3& s)
{
    Mat4 r;
    r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;

    return r;
}

Mat4 Mat4::rotateXYZ(const Vec3& d)
{
    const double rx = d.x * kPi / 180., ry = d.y * kPi / 180., rz = d.z * kPi / 180.;
    Mat4 X, Y, Z;
    X.m[1][1] = std::cos(rx); X.m[1][2] = -std::sin(rx); X.m[2][1] = std::sin(rx); X.m[2][2] = std::cos(rx);
    Y.m[0][0] = std::cos(ry); Y.m[0][2] = std::sin(ry); Y.m[2][0] = -std::sin(ry); Y.m[2][2] = std::cos(ry);
    Z.m[0][0] = std::cos(rz); Z.m[0][1] = -std::sin(rz); Z.m[1][0] = std::sin(rz); Z.m[1][1] = std::cos(rz);

    return Z * Y * X;
}

Mat4 Mat4::trs(const Vec3& t, const Vec3& r, const Vec3& s, const Vec3& pivot)
{
    return translate(t) * translate(pivot) * rotateXYZ(r) * scale(s) * translate(pivot * -1.);
}

Mat4 Mat4::operator*(const Mat4& o) const
{
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.;
            for (int k = 0; k < 4; ++k) {
                s += m[i][k] * o.m[k][j];
            }
            r.m[i][j] = s;
        }
    }

    return r;
}

Vec3 Mat4::point(const Vec3& p) const
{
    const double x = m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3];
    const double y = m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3];
    const double z = m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3];
    const double w = m[3][0] * p.x + m[3][1] * p.y + m[3][2] * p.z + m[3][3];

    return (std::fabs(w) > 1e-15 && w != 1.) ? Vec3(x / w, y / w, z / w) : Vec3(x, y, z);
}

Vec3 Mat4::vector(const Vec3& v) const
{
    return Vec3(m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z);
}

Mat4 Mat4::transposed() const
{
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = m[j][i];
        }
    }

    return r;
}

Mat4 Mat4::inverse() const
{
    // Gauss-Jordan with partial pivoting
    double a[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            a[i][j] = m[i][j];
            a[i][j + 4] = (i == j) ? 1. : 0.;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) {
            if ( std::fabs(a[r][c]) > std::fabs(a[p][c]) ) {
                p = r;
            }
        }
        if (std::fabs(a[p][c]) < 1e-300) {
            return Mat4();
        }
        if (p != c) {
            for (int j = 0; j < 8; ++j) {
                std::swap(a[p][j], a[c][j]);
            }
        }
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) {
            a[c][j] /= d;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != c) {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j) {
                    a[r][j] -= f * a[c][j];
                }
            }
        }
    }
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a[i][j + 4];
        }
    }

    return r;
}

// ------------------------------------------------------------------ Mesh
void
Mesh::computeFlatNormals()
{
    normals.clear();
    normals.reserve( faceVertexIndices.size() );
    size_t base = 0;
    for (int n : faceVertexCounts) {
        Vec3 nrm;
        // Newell's method (works for any planar-ish polygon)
        for (int i = 0; i < n; ++i) {
            const Vec3& a = points[faceVertexIndices[base + i]];
            const Vec3& b = points[faceVertexIndices[base + (i + 1) % n]];
            nrm.x += (a.y - b.y) * (a.z + b.z);
            nrm.y += (a.z - b.z) * (a.x + b.x);
            nrm.z += (a.x - b.x) * (a.y + b.y);
        }
        nrm = normalize(nrm);
        for (int i = 0; i < n; ++i) {
            normals.push_back(nrm);
        }
        base += n;
    }
}

void
Mesh::triangulate(std::vector<std::array<int, 3> >* tris, std::vector<std::array<int, 3> >* fv) const
{
    size_t base = 0;
    for (int n : faceVertexCounts) {
        for (int i = 1; i + 1 < n; ++i) { // fan (faces are convex in practice)
            tris->push_back( { faceVertexIndices[base], faceVertexIndices[base + i], faceVertexIndices[base + i + 1] } );
            if (fv) {
                fv->push_back( { (int)base, (int)(base + i), (int)(base + i + 1) } );
            }
        }
        base += n;
    }
}

// ------------------------------------------------------------------ Camera
Mat4
Camera::projection(double aspect) const
{
    // film back fitted horizontally (Nuke / USD default): the image width matches the horizontal aperture
    const double ha = horizontalAperture;
    const double va = ha / std::max(1e-9, aspect);
    Mat4 p;
    for (int i = 0; i < 4; ++i) {
        p.m[i][i] = 0.;
    }
    const double n = nearClip, f = farClip;
    p.m[0][0] = 2. * focalLength / ha;
    p.m[1][1] = 2. * focalLength / va;
    p.m[0][2] = 2. * horizontalApertureOffset / ha;
    p.m[1][2] = 2. * verticalApertureOffset / va;
    // camera looks down -Z (USD / OpenGL)
    p.m[2][2] = -(f + n) / (f - n);
    p.m[2][3] = -2. * f * n / (f - n);
    p.m[3][2] = -1.;

    return p;
}

// ------------------------------------------------------------------ paths
std::string
parentPath(const std::string& path)
{
    const size_t s = path.find_last_of('/');
    if ( (s == std::string::npos) || (s == 0) ) {
        return std::string();
    }

    return path.substr(0, s);
}

std::string
leafName(const std::string& path)
{
    const size_t s = path.find_last_of('/');

    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string
makeValidName(const std::string& name)
{
    std::string r;
    for (char c : name) {
        r += ( std::isalnum( (unsigned char)c ) || c == '_' ) ? c : '_';
    }
    if ( r.empty() || std::isdigit( (unsigned char)r[0] ) ) {
        r = "_" + r;
    }

    return r;
}

// ------------------------------------------------------------------ Scene
Prim&
Scene::define(const std::string& path, PrimType type)
{
    const std::string parent = parentPath(path);
    if ( !parent.empty() && !find(parent) ) {
        define(parent, PrimType::Xform);
    }
    Prim& p = _prims[path];
    p.path = path;
    p.type = type;

    return p;
}

Prim*
Scene::find(const std::string& path)
{
    std::map<std::string, Prim>::iterator it = _prims.find(path);

    return it == _prims.end() ? nullptr : &it->second;
}

const Prim*
Scene::find(const std::string& path) const
{
    std::map<std::string, Prim>::const_iterator it = _prims.find(path);

    return it == _prims.end() ? nullptr : &it->second;
}

void
Scene::remove(const std::string& path)
{
    for (std::map<std::string, Prim>::iterator it = _prims.begin(); it != _prims.end(); ) {
        if ( (it->first == path) || (it->first.compare(0, path.size() + 1, path + "/") == 0) ) {
            it = _prims.erase(it);
        } else {
            ++it;
        }
    }
}

void
Scene::merge(const Scene& over)
{
    for (std::map<std::string, Prim>::const_iterator it = over._prims.begin(); it != over._prims.end(); ++it) {
        _prims[it->first] = it->second;
    }
}

Scene
Scene::reparented(const std::string& root) const
{
    Scene s;
    for (std::map<std::string, Prim>::const_iterator it = _prims.begin(); it != _prims.end(); ++it) {
        Prim p = it->second;
        p.path = root + it->first;
        s._prims[p.path] = p;
    }
    if ( !root.empty() && !s.find(root) ) {
        s.define(root, PrimType::Xform);
    }

    return s;
}

void
Scene::transformRoots(const Mat4& m)
{
    for (std::map<std::string, Prim>::iterator it = _prims.begin(); it != _prims.end(); ++it) {
        if ( parentPath(it->first).empty() ) {
            it->second.local = m * it->second.local;
        }
    }
}

Mat4
Scene::worldMatrix(const std::string& path) const
{
    const Prim* p = find(path);
    if (!p) {
        return Mat4();
    }
    const std::string parent = parentPath(path);

    return parent.empty() ? p->local : worldMatrix(parent) * p->local;
}

std::vector<std::string>
Scene::pathsOfType(PrimType t) const
{
    std::vector<std::string> r;
    for (std::map<std::string, Prim>::const_iterator it = _prims.begin(); it != _prims.end(); ++it) {
        if (it->second.type == t && it->second.visible) {
            r.push_back(it->first);
        }
    }

    return r;
}

static const char*
usdTypeName(PrimType t)
{
    switch (t) {
    case PrimType::Mesh: return "Mesh";
    case PrimType::Camera: return "Camera";
    case PrimType::Light: return "DistantLight";
    default: return "Xform";
    }
}

static void
writePrim(std::ostringstream& o, const Scene& s, const Prim& p, int depth)
{
    const std::string ind( (size_t)depth * 4, ' ' );
    o << ind << "def " << usdTypeName(p.type) << " \"" << leafName(p.path) << "\"\n" << ind << "{\n";
    const std::string in2 = ind + "    ";
    // USD matrices are row-vector (p * M): write the transpose
    const Mat4 t = p.local.transposed();
    o << in2 << "matrix4d xformOp:transform = ( ";
    for (int i = 0; i < 4; ++i) {
        o << "(" << t.m[i][0] << ", " << t.m[i][1] << ", " << t.m[i][2] << ", " << t.m[i][3] << ")" << (i < 3 ? ", " : " ");
    }
    o << ")\n" << in2 << "uniform token[] xformOpOrder = [\"xformOp:transform\"]\n";
    if (!p.visible) {
        o << in2 << "token visibility = \"invisible\"\n";
    }
    if ( (p.type == PrimType::Mesh) && p.mesh ) {
        const Mesh& m = *p.mesh;
        o << in2 << "int[] faceVertexCounts = [";
        for (size_t i = 0; i < m.faceVertexCounts.size(); ++i) {
            o << (i ? ", " : "") << m.faceVertexCounts[i];
        }
        o << "]\n" << in2 << "int[] faceVertexIndices = [";
        for (size_t i = 0; i < m.faceVertexIndices.size(); ++i) {
            o << (i ? ", " : "") << m.faceVertexIndices[i];
        }
        o << "]\n" << in2 << "point3f[] points = [";
        for (size_t i = 0; i < m.points.size(); ++i) {
            o << (i ? ", " : "") << "(" << m.points[i].x << ", " << m.points[i].y << ", " << m.points[i].z << ")";
        }
        o << "]\n";
        if ( !m.normals.empty() ) {
            o << in2 << "normal3f[] normals = [";
            for (size_t i = 0; i < m.normals.size(); ++i) {
                o << (i ? ", " : "") << "(" << m.normals[i].x << ", " << m.normals[i].y << ", " << m.normals[i].z << ")";
            }
            o << "] (\n" << in2 << "    interpolation = \"faceVarying\"\n" << in2 << ")\n";
        }
        if ( !m.st.empty() ) {
            o << in2 << "texCoord2f[] primvars:st = [";
            for (size_t i = 0; i < m.st.size(); ++i) {
                o << (i ? ", " : "") << "(" << m.st[i].x << ", " << m.st[i].y << ")";
            }
            o << "] (\n" << in2 << "    interpolation = \"faceVarying\"\n" << in2 << ")\n";
        }
        o << in2 << "color3f[] primvars:displayColor = [(" << m.displayColor.x << ", " << m.displayColor.y << ", " << m.displayColor.z << ")]\n";
        o << in2 << "uniform token subdivisionScheme = \"none\"\n";
    }
    if ( (p.type == PrimType::Camera) && p.camera ) {
        const Camera& c = *p.camera;
        // USD camera apertures / focal length are in tenths of a scene unit: written in mm (the common convention)
        o << in2 << "float focalLength = " << c.focalLength << "\n";
        o << in2 << "float horizontalAperture = " << c.horizontalAperture << "\n";
        o << in2 << "float verticalAperture = " << c.verticalAperture << "\n";
        o << in2 << "float horizontalApertureOffset = " << c.horizontalApertureOffset << "\n";
        o << in2 << "float verticalApertureOffset = " << c.verticalApertureOffset << "\n";
        o << in2 << "float2 clippingRange = (" << c.nearClip << ", " << c.farClip << ")\n";
    }
    for (std::map<std::string, std::string>::const_iterator a = p.attributes.begin(); a != p.attributes.end(); ++a) {
        o << in2 << "custom string " << makeValidName(a->first) << " = \"" << a->second << "\"\n";
    }
    // children
    for (std::map<std::string, Prim>::const_iterator it = s.prims().begin(); it != s.prims().end(); ++it) {
        if (parentPath(it->first) == p.path) {
            o << "\n";
            writePrim(o, s, it->second, depth + 1);
        }
    }
    o << ind << "}\n";
}

std::string
Scene::toUsda(double metersPerUnit) const
{
    std::ostringstream o;
    o << std::setprecision(9);
    o << "#usda 1.0\n(\n    defaultPrim = \"";
    std::string firstRoot;
    for (std::map<std::string, Prim>::const_iterator it = _prims.begin(); it != _prims.end(); ++it) {
        if ( parentPath(it->first).empty() ) {
            firstRoot = leafName(it->first);
            break;
        }
    }
    o << firstRoot << "\"\n    metersPerUnit = " << metersPerUnit << "\n    upAxis = \"Y\"\n    doc = \"SOLVFX\"\n)\n\n";
    for (std::map<std::string, Prim>::const_iterator it = _prims.begin(); it != _prims.end(); ++it) {
        if ( parentPath(it->first).empty() ) {
            writePrim(o, *this, it->second, 0);
            o << "\n";
        }
    }

    return o.str();
}

// ------------------------------------------------------------------ primitives
std::shared_ptr<Mesh>
makeCard(double w, double h, int rows, int cols)
{
    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    rows = std::max(1, rows);
    cols = std::max(1, cols);
    for (int r = 0; r <= rows; ++r) {
        for (int c = 0; c <= cols; ++c) {
            m->points.push_back( Vec3(-w / 2. + w * c / cols, -h / 2. + h * r / rows, 0.) );
        }
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int a = r * (cols + 1) + c;
            m->faceVertexCounts.push_back(4);
            m->faceVertexIndices.insert( m->faceVertexIndices.end(), { a, a + 1, a + cols + 2, a + cols + 1 } );
            const int ids[4] = { a, a + 1, a + cols + 2, a + cols + 1 };
            for (int k = 0; k < 4; ++k) {
                const int rr = ids[k] / (cols + 1), cc = ids[k] % (cols + 1);
                Vec2 uv;
                uv.x = (double)cc / cols;
                uv.y = (double)rr / rows;
                m->st.push_back(uv);
            }
        }
    }
    m->computeFlatNormals();

    return m;
}

std::shared_ptr<Mesh>
makeCube(double s)
{
    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    const double h = s / 2.;
    const Vec3 p[8] = { Vec3(-h, -h, h), Vec3(h, -h, h), Vec3(h, h, h), Vec3(-h, h, h),
                        Vec3(-h, -h, -h), Vec3(h, -h, -h), Vec3(h, h, -h), Vec3(-h, h, -h) };
    m->points.assign(p, p + 8);
    const int f[6][4] = { { 0, 1, 2, 3 }, { 5, 4, 7, 6 }, { 1, 5, 6, 2 }, { 4, 0, 3, 7 }, { 3, 2, 6, 7 }, { 4, 5, 1, 0 } };
    const double uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 6; ++i) {
        m->faceVertexCounts.push_back(4);
        for (int k = 0; k < 4; ++k) {
            m->faceVertexIndices.push_back(f[i][k]);
            Vec2 t;
            t.x = uv[k][0];
            t.y = uv[k][1];
            m->st.push_back(t);
        }
    }
    m->computeFlatNormals();

    return m;
}

std::shared_ptr<Mesh>
makeSphere(double radius, int rows, int cols)
{
    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    rows = std::max(3, rows);
    cols = std::max(3, cols);
    for (int r = 0; r <= rows; ++r) {
        const double v = (double)r / rows, th = v * kPi;
        for (int c = 0; c <= cols; ++c) {
            const double u = (double)c / cols, ph = u * 2. * kPi;
            m->points.push_back( Vec3( -radius * std::sin(th) * std::sin(ph), -radius * std::cos(th), -radius * std::sin(th) * std::cos(ph) ) );
        }
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int a = r * (cols + 1) + c;
            const int ids[4] = { a, a + 1, a + cols + 2, a + cols + 1 };
            m->faceVertexCounts.push_back(4);
            for (int k = 0; k < 4; ++k) {
                m->faceVertexIndices.push_back(ids[k]);
                Vec2 t;
                t.x = (double)(ids[k] % (cols + 1)) / cols;
                t.y = (double)(ids[k] / (cols + 1)) / rows;
                m->st.push_back(t);
                m->normals.push_back( normalize(m->points[ids[k]]) ); // smooth
            }
        }
    }

    return m;
}

std::shared_ptr<Mesh>
makeCylinder(double radius, double height, int cols, bool caps)
{
    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    cols = std::max(3, cols);
    for (int r = 0; r <= 1; ++r) {
        for (int c = 0; c <= cols; ++c) {
            const double ph = 2. * kPi * c / cols;
            m->points.push_back( Vec3( radius * std::sin(ph), -height / 2. + height * r, radius * std::cos(ph) ) );
        }
    }
    for (int c = 0; c < cols; ++c) {
        const int ids[4] = { c, c + 1, c + cols + 2, c + cols + 1 };
        m->faceVertexCounts.push_back(4);
        for (int k = 0; k < 4; ++k) {
            m->faceVertexIndices.push_back(ids[k]);
            Vec2 t;
            t.x = (double)(ids[k] % (cols + 1)) / cols;
            t.y = (double)(ids[k] / (cols + 1));
            m->st.push_back(t);
            const Vec3& p = m->points[ids[k]];
            m->normals.push_back( normalize( Vec3(p.x, 0., p.z) ) );
        }
    }
    if (caps) {
        for (int top = 0; top <= 1; ++top) {
            const int centre = (int)m->points.size();
            m->points.push_back( Vec3(0., top ? height / 2. : -height / 2., 0.) );
            for (int c = 0; c < cols; ++c) {
                const int a = top * (cols + 1) + c, b = a + 1;
                m->faceVertexCounts.push_back(3);
                const int ids[3] = { centre, top ? a : b, top ? b : a };
                for (int k = 0; k < 3; ++k) {
                    m->faceVertexIndices.push_back(ids[k]);
                    const Vec3& p = m->points[ids[k]];
                    Vec2 t;
                    t.x = 0.5 + p.x / (2. * radius);
                    t.y = 0.5 + p.z / (2. * radius);
                    m->st.push_back(t);
                    m->normals.push_back( Vec3(0., top ? 1. : -1., 0.) );
                }
            }
        }
    }

    return m;
}

// ------------------------------------------------------------------ OBJ
bool
readObjText(const std::string& text, const std::string& root, Scene* scene, std::string* error)
{
    std::vector<Vec3> v, vn;
    std::vector<Vec2> vt;
    struct Group { std::string name; std::shared_ptr<Mesh> mesh; std::map<int, int> remap; bool hasN = true, hasT = true; };
    std::vector<Group> groups;
    auto current = [&]() -> Group& {
        if ( groups.empty() ) {
            Group g;
            g.name = "mesh";
            g.mesh = std::make_shared<Mesh>();
            groups.push_back(g);
        }

        return groups.back();
    };
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while ( std::getline(in, line) ) {
        ++lineNo;
        if ( !line.empty() && (line.back() == '\r') ) {
            line.pop_back();
        }
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            Vec3 p;
            ls >> p.x >> p.y >> p.z;
            v.push_back(p);
        } else if (tag == "vn") {
            Vec3 n;
            ls >> n.x >> n.y >> n.z;
            vn.push_back(n);
        } else if (tag == "vt") {
            Vec2 t;
            ls >> t.x >> t.y;
            vt.push_back(t);
        } else if ( (tag == "o") || (tag == "g") ) {
            std::string name;
            std::getline(ls, name);
            name.erase( 0, name.find_first_not_of(" \t") );
            Group g;
            g.name = name.empty() ? "mesh" : name;
            g.mesh = std::make_shared<Mesh>();
            // reuse an empty group just opened (o then g)
            if ( !groups.empty() && groups.back().mesh->faceVertexCounts.empty() ) {
                groups.back() = g;
            } else {
                groups.push_back(g);
            }
        } else if (tag == "f") {
            Group& g = current();
            std::string tok;
            int count = 0;
            std::vector<Vec3> fn;
            std::vector<Vec2> ft;
            bool faceHasN = true, faceHasT = true;
            while (ls >> tok) {
                int iv = 0, it = 0, in_ = 0;
                // v, v/vt, v//vn, v/vt/vn ; negative = relative
                const size_t s1 = tok.find('/');
                iv = std::atoi( tok.substr(0, s1).c_str() );
                if (s1 != std::string::npos) {
                    const size_t s2 = tok.find('/', s1 + 1);
                    const std::string a = tok.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
                    if ( !a.empty() ) {
                        it = std::atoi( a.c_str() );
                    }
                    if (s2 != std::string::npos) {
                        in_ = std::atoi( tok.substr(s2 + 1).c_str() );
                    }
                }
                if (iv < 0) { iv = (int)v.size() + iv + 1; }
                if (it < 0) { it = (int)vt.size() + it + 1; }
                if (in_ < 0) { in_ = (int)vn.size() + in_ + 1; }
                if ( (iv <= 0) || ( iv > (int)v.size() ) ) {
                    if (error) {
                        *error = "OBJ: bad vertex index on line " + std::to_string(lineNo);
                    }

                    return false;
                }
                std::map<int, int>::iterator r = g.remap.find(iv);
                int local;
                if ( r == g.remap.end() ) {
                    local = (int)g.mesh->points.size();
                    g.mesh->points.push_back(v[iv - 1]);
                    g.remap[iv] = local;
                } else {
                    local = r->second;
                }
                g.mesh->faceVertexIndices.push_back(local);
                if ( (in_ > 0) && ( in_ <= (int)vn.size() ) ) { fn.push_back(vn[in_ - 1]); } else { faceHasN = false; }
                if ( (it > 0) && ( it <= (int)vt.size() ) ) { ft.push_back(vt[it - 1]); } else { faceHasT = false; }
                ++count;
            }
            g.mesh->faceVertexCounts.push_back(count);
            g.hasN = g.hasN && faceHasN;
            g.hasT = g.hasT && faceHasT;
            if (g.hasN) { g.mesh->normals.insert( g.mesh->normals.end(), fn.begin(), fn.end() ); }
            if (g.hasT) { g.mesh->st.insert( g.mesh->st.end(), ft.begin(), ft.end() ); }
        }
    }
    int n = 0;
    std::map<std::string, int> used;
    for (Group& g : groups) {
        if ( g.mesh->faceVertexCounts.empty() ) {
            continue;
        }
        if (!g.hasN) { g.mesh->normals.clear(); g.mesh->computeFlatNormals(); }
        if (!g.hasT) { g.mesh->st.clear(); }
        std::string name = makeValidName(g.name);
        if ( used[name]++ ) {
            name += "_" + std::to_string(used[name] - 1);
        }
        Prim& p = scene->define(root + "/" + name, PrimType::Mesh);
        p.mesh = g.mesh;
        ++n;
    }
    if (n == 0) {
        if (error) {
            *error = "OBJ: no faces found";
        }

        return false;
    }

    return true;
}

bool
readObj(const std::string& filename, const std::string& root, Scene* scene, std::string* error)
{
    std::ifstream f( filename.c_str(), std::ios::binary );
    if (!f) {
        if (error) {
            *error = "cannot open " + filename;
        }

        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    return readObjText(ss.str(), root, scene, error);
}
} // namespace Solv3D
