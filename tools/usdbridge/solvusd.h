/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

/* SOLVFX USD bridge: a small C interface around OpenUSD, built with the compiler USD supports
 * (MSVC on Windows) and loaded at run time by SOLVFX (any compiler: only plain C types cross it).
 * Matrices are row-major with column vectors (SOLVFX convention, p' = M * p).
 * Units: what the file says (metersPerUnit reported); time = USD time codes (frames). */

#ifndef SOLVFX_SOLVUSD_H
#define SOLVFX_SOLVUSD_H

#ifdef _WIN32
#  ifdef SOLVUSD_BUILD
#    define SOLVUSD_API __declspec(dllexport)
#  else
#    define SOLVUSD_API __declspec(dllimport)
#  endif
#else
#  define SOLVUSD_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SOLVUSD_VERSION 3

enum SolvUsdPrimType { SOLVUSD_XFORM = 0, SOLVUSD_MESH = 1, SOLVUSD_CAMERA = 2, SOLVUSD_LIGHT = 3 };
enum SolvUsdLightType { SOLVUSD_POINT = 0, SOLVUSD_SPOT = 1, SOLVUSD_DISTANT = 2, SOLVUSD_DOME = 3 };

typedef struct SolvUsdPrim
{
    const char* path;         /* "/root/child" */
    int type;                 /* SolvUsdPrimType */
    int visible;
    double local[16];         /* local transform, row-major, column vectors */

    /* mesh: points, faces, per face-vertex normals / uv (NULL when absent) */
    int nPoints;
    const float* points;      /* nPoints * 3 */
    int nFaces;
    const int* faceCounts;    /* nFaces */
    int nIndices;
    const int* indices;       /* nIndices */
    const float* normals;     /* nIndices * 3 or NULL */
    const float* uvs;         /* nIndices * 2 or NULL */
    /* material (UsdPreviewSurface values, or displayColor) */
    float color[3];
    float opacity;
    float metallic, roughness, ior, specular;
    float emissive[3];
    const char* diffuseTexture; /* file path of the diffuse texture, "" if none */

    /* camera (mm, like the USD attributes when 1 unit = 1 cm) */
    double focalLength, hAperture, vAperture, hOffset, vOffset, nearClip, farClip, focusDistance, fStop;
    int orthographic;

    /* light */
    int lightType;            /* SolvUsdLightType */
    float lightColor[3];
    float intensity, exposure, radius, angle, coneAngle, coneSoftness;
    int shadows;

    /* USD prim attributes ("" when not set) */
    const char* kind;         /* model kind: component, assembly, group, subcomponent */
    const char* purpose;      /* default, render, proxy, guide */
    const char* typeName;     /* Xform prims: "Xform" or "Scope" (writing) */
} SolvUsdPrim;

SOLVUSD_API int solvusd_version(void);

/* reading */
SOLVUSD_API void* solvusd_open(const char* file, char* err, int errLen);
/* opens with edits, one per line: "u <path>" payload not loaded, "d <path>" deactivated, "h <path>" invisible */
SOLVUSD_API void* solvusd_open2(const char* file, const char* edits, char* err, int errLen);
/* the whole prim tree (also inactive / not loaded prims), one line per prim:
   path TAB type TAB kind TAB purpose TAB flags   (flags: a arc, P payload, l loaded, i instance, p prototype,
   o only 'over', x inactive, h invisible). Returns the length needed (call again with a bigger buffer). */
SOLVUSD_API int solvusd_tree(void* stage, char* out, int len);
// composition errors / warnings collected when the stage was opened (missing payloads, references...)
SOLVUSD_API int solvusd_diagnostics(void* stage, char* out, int len);
SOLVUSD_API void solvusd_info(void* stage, double* start, double* end, double* fps, double* metersPerUnit, int* zUp);
/* evaluates the stage at a time code; returns the number of prims (valid until the next eval / close) */
SOLVUSD_API int solvusd_eval(void* stage, double timeCode);
SOLVUSD_API const SolvUsdPrim* solvusd_prim(void* stage, int index);
SOLVUSD_API void solvusd_close(void* stage);

/* writing: begin, one or more frames (all prims each time), end. .usda / .usdc / .usd / .usdz */
SOLVUSD_API void* solvusd_write_begin(const char* file, double start, double end, double fps, double metersPerUnit, char* err, int errLen);
SOLVUSD_API int solvusd_write_frame(void* writer, double timeCode, const SolvUsdPrim* prims, int n);
SOLVUSD_API int solvusd_write_end(void* writer, char* err, int errLen);

#ifdef __cplusplus
}
#endif

#endif /* SOLVFX_SOLVUSD_H */
