/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX 3D core: a small USD-shaped scene description.
//  - prims live at paths ("/world/geo/card1"), parents are implied by the path
//  - each prim has a type (Xform, Mesh, Camera, Light), a local transform and attributes
//  - meshes follow UsdGeomMesh: points, faceVertexCounts, faceVertexIndices, faceVarying normals / st
//  - cameras follow UsdGeomCamera (focal length and apertures in mm, clipping range)
// Pure standard C++17 (no Qt / Natron / platform code) so it builds with MSVC as is and the backend
// can later be swapped for real OpenUSD.

#ifndef SOLVFX_SOLVSCENE_H
#define SOLVFX_SOLVSCENE_H

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Solv3D {

struct Vec2 { double x = 0, y = 0; };
struct Vec3
{
    double x = 0, y = 0, z = 0;
    Vec3() {}
    Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(double s) const { return Vec3(x * s, y * s, z * s); }
};

double dot(const Vec3& a, const Vec3& b);
Vec3 cross(const Vec3& a, const Vec3& b);
Vec3 normalize(const Vec3& v);

// row-major 4x4, column vectors (p' = M * p), like the maths in the papers; USD stores the transpose
struct Mat4
{
    double m[4][4];
    Mat4();                                   // identity
    static Mat4 translate(const Vec3& t);
    static Mat4 scale(const Vec3& s);
    static Mat4 rotateXYZ(const Vec3& degrees); // R = Rz * Ry * Rx
    // translate * rotate * scale around a pivot (Nuke / USD xformOp order: T * pivot * R * S * -pivot)
    static Mat4 trs(const Vec3& t, const Vec3& rDeg, const Vec3& s, const Vec3& pivot = Vec3());
    Mat4 operator*(const Mat4& o) const;
    Vec3 point(const Vec3& p) const;           // with translation (and perspective divide)
    Vec3 vector(const Vec3& v) const;          // no translation
    Mat4 inverse() const;
    Mat4 transposed() const;
};

enum class PrimType { Xform, Mesh, Camera, Light };

// surface look used by ScanlineRender (MaterialX-shaped subset: OpenPBR base / specular / metalness)
struct Material
{
    Vec3 baseColor = Vec3(1, 1, 1);  // multiplies the texture (or is the colour without texture)
    double emission = 1.;            // unlit amount: 1 = texture shown as is (cards), 0 = fully lit
    double metalness = 0.;
    double specular = 0.;            // reflection weight (dielectric)
    double roughness = 0.2;
    double ior = 1.5;
    double opacity = 1.;             // multiplies the alpha
};

// image projected through a camera (Project3D): texture coordinates from world positions
struct Projection
{
    Mat4 viewProj;                    // world -> clip space of the projector camera
    std::shared_ptr<void> texture;    // opaque handle resolved by the renderer
    bool crop = true;                 // outside the projector frame = transparent
    int projectOn = 0;                // 0 both sides, 1 front faces only, 2 back faces only (seen from the projector)
    bool occlusion = false;           // self occlusion: surfaces hidden from the projector get nothing
    Vec3 origin;                      // projector position (world)
    Vec3 forward = Vec3(0, 0, -1);    // projector view direction (world), used when orthographic
    bool orthographic = false;
};

struct Mesh
{
    std::vector<Vec3> points;
    std::vector<int> faceVertexCounts;
    std::vector<int> faceVertexIndices;
    std::vector<Vec3> normals;   // faceVarying (one per face vertex) or empty
    std::vector<Vec2> st;        // faceVarying uvs or empty
    Vec3 displayColor = Vec3(0.18, 0.18, 0.18);
    std::shared_ptr<void> texture; // opaque image handle (resolved by the renderer), null = none
    std::string textureFile;       // diffuse texture file from the source file (USD), loaded by the reader node
    Material material;
    std::vector<Projection> projections;

    void computeFlatNormals();
    void triangulate(std::vector<std::array<int, 3> >* tris, std::vector<std::array<int, 3> >* faceVaryingIdx) const;
};

struct Camera
{
    double focalLength = 35.;          // mm
    double horizontalAperture = 36.;   // mm
    double verticalAperture = 24.;     // mm
    double horizontalApertureOffset = 0., verticalApertureOffset = 0.;
    double nearClip = 0.1, farClip = 10000.;
    int projectionMode = 0;            // 0 perspective, 1 orthographic (aperture = view size in tenths of a unit, like USD)
    double winTranslate[2] = { 0., 0. }; // window: shift of the image (-1..1 = half the frame)
    double winScale[2] = { 1., 1. };     // window: zoom of the image
    double winRoll = 0.;                 // window: roll (degrees)
    double focusDistance = 2., fStop = 16.; // depth of field (stored, used by the renderer later)
    double iconScale = 1.;               // 3D viewer: size of the camera icon (display only)
    bool iconWorldScale = false;         // 3D viewer: icon at world size (off: same size on screen)
    double shutter = 0.5;                // frames
    int shutterOffset = 0;               // 0 centred, 1 start, 2 end, 3 custom
    double shutterCustom = 0.;           // custom start (frames, relative to the frame)

    // projection to normalised device coordinates (x, y in -1..1), aspect = image width / height
    Mat4 projection(double aspect) const;
};

// light (UsdLux-shaped). Lights look down -Z of their transform (like cameras).
struct Light
{
    std::string kind = "point";    // point, spot, distant, ambient, dome
    Vec3 color = Vec3(1, 1, 1);
    double intensity = 1.;
    int falloff = 0;               // point / spot: 0 none, 1 linear, 2 quadratic
    double coneAngle = 30.;        // spot: full angle (degrees)
    double conePenumbra = 5.;      // spot: soft edge width (degrees), centred on the cone edge
    // shadows (off by default)
    bool castShadows = false;
    double shadowSoftness = 0.;    // point / spot: light size (scene units); distant: angular size (degrees)
    int shadowSamples = 16;        // filter samples (quality)
    int shadowResolution = 2048;   // shadow map size
    double shadowBias = 0.1;       // scene units
    std::shared_ptr<void> texture; // dome: environment image handle
};

struct Prim
{
    std::string path;
    PrimType type = PrimType::Xform;
    Mat4 local;
    bool visible = true;
    int display = 4;          // viewer: 0 off, 1 wireframe, 2 solid, 3 solid + lines, 4 textured, 5 textured + lines
    int render = 4;           // ScanlineRender: 0 off, 1 wireframe, 2 solid, 3 solid + lines, 4 textured, 5 textured + lines
    bool selectable = true;   // can be picked in the 3D viewer
    bool editable = true;     // the 3D viewer handles can move it
    bool castShadow = true;   // blocks light (lights with shadows on)
    bool receiveShadow = true;
    std::string node;         // script name of the node that made the prim (viewer selection / highlight)
    double axisSize = 0.;     // > 0: the viewer draws a locator (Axis node) of this size
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<Camera> camera;
    std::shared_ptr<Light> light;
    std::map<std::string, std::string> attributes; // free attributes (USD "custom" attributes)
};

// rotations applied in the order of the letters (order 0 XYZ .. 5 ZYX, XYZ = X first), degrees
Mat4 eulerMatrix(const Vec3& deg, int order);
// inverse of eulerMatrix; angles kept close to 'near' (no 360 jumps)
Vec3 matrixEuler(const Mat4& rotation, int order, const Vec3& nearDeg = Vec3());
// M = T * R * S (no shear): translate, rotation (euler in 'order'), scale
void decompose(const Mat4& m, int order, const Vec3& nearRot, Vec3* t, Vec3* r, Vec3* s);

std::string parentPath(const std::string& path);
std::string leafName(const std::string& path);
std::string makeValidName(const std::string& name);

// a scene at one time: prims by path (ordered, so parents come before children)
class Scene
{
public:
    Prim& define(const std::string& path, PrimType type); // creates missing parent Xforms
    Prim* find(const std::string& path);
    const Prim* find(const std::string& path) const;
    void remove(const std::string& path);                 // with children
    const std::map<std::string, Prim>& prims() const { return _prims; }
    std::map<std::string, Prim>& prims() { return _prims; }

    // stronger opinion wins (like a USD sublayer over this one): prims of 'over' replace / add
    void merge(const Scene& over);
    // puts every prim under a new root path (MergeGeo / TransformGeo use it)
    Scene reparented(const std::string& root) const;
    // applies m on top of the root prims (TransformGeo)
    void transformRoots(const Mat4& m);

    Mat4 worldMatrix(const std::string& path) const;
    std::vector<std::string> pathsOfType(PrimType t) const;
    // first visible camera (path) or empty
    std::string firstCamera() const;
    // world bounding box of the meshes; false when empty
    bool bounds(Vec3* lo, Vec3* hi) const;
    bool empty() const { return _prims.empty(); }

    // USD text (.usda); frame is written as the time sample when animated == true
    std::string toUsda(double metersPerUnit = 1.) const;

private:
    std::map<std::string, Prim> _prims;
};

// primitives (in the Y-up, right-handed USD convention; unit size like the Nuke / USD gprims)
std::shared_ptr<Mesh> makeCard(double width, double height, int rows = 1, int cols = 1);
std::shared_ptr<Mesh> makeCube(double size = 1.);
std::shared_ptr<Mesh> makeSphere(double radius = 0.5, int rows = 24, int cols = 48);
std::shared_ptr<Mesh> makeCylinder(double radius = 0.5, double height = 1., int cols = 48, bool caps = true);

// OBJ reader: one Mesh prim per object / group under root; false + error on failure
bool readObj(const std::string& filename, const std::string& root, Scene* scene, std::string* error);
bool readObjText(const std::string& text, const std::string& root, Scene* scene, std::string* error);
} // namespace Solv3D

#endif // SOLVFX_SOLVSCENE_H
