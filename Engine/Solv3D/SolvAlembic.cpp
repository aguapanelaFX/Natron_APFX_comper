/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

#include "SolvAlembic.h"

#ifdef SOLVFX_HAS_ALEMBIC

#include <Alembic/AbcCoreFactory/IFactory.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <algorithm>

namespace Solv3D {

using namespace Alembic::AbcGeom;

bool
alembicAvailable()
{
    return true;
}

static Mat4
toMat4(const M44d& m)
{
    // Imath is row-vector (p * M): our Mat4 is column-vector -> transpose
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = m[j][i];
        }
    }

    return r;
}

static void
walk(const IObject& obj, const ISampleSelector& sel, const std::string& path, Scene* scene)
{
    for (size_t i = 0; i < obj.getNumChildren(); ++i) {
        IObject child = obj.getChild(i);
        const std::string cpath = path + "/" + makeValidName( child.getName() );
        const MetaData& md = child.getMetaData();
        if ( IXform::matches(md) ) {
            IXform x(child, kWrapExisting);
            XformSample xs;
            x.getSchema().get(xs, sel);
            Prim& p = scene->define(cpath, PrimType::Xform);
            p.local = toMat4( xs.getMatrix() );
            if ( !x.getSchema().getInheritsXforms(sel) ) {
                p.attributes["inheritsTransform"] = "0";
            }
        } else if ( IPolyMesh::matches(md) ) {
            IPolyMesh pm(child, kWrapExisting);
            IPolyMeshSchema& s = pm.getSchema();
            IPolyMeshSchema::Sample ms;
            s.get(ms, sel);
            std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
            P3fArraySamplePtr P = ms.getPositions();
            Int32ArraySamplePtr C = ms.getFaceCounts(), I = ms.getFaceIndices();
            if (P) {
                for (size_t k = 0; k < P->size(); ++k) {
                    m->points.push_back( Vec3( (*P)[k].x, (*P)[k].y, (*P)[k].z ) );
                }
            }
            // Alembic polygons are clockwise (Maya convention): reverse each face to counter-clockwise (USD)
            size_t base = 0;
            if (C && I) {
                for (size_t f = 0; f < C->size(); ++f) {
                    const int n = (*C)[f];
                    m->faceVertexCounts.push_back(n);
                    for (int k = n - 1; k >= 0; --k) {
                        m->faceVertexIndices.push_back( (*I)[base + k] );
                    }
                    base += n;
                }
            }
            // uvs (face varying or indexed)
            IV2fGeomParam uvParam = s.getUVsParam();
            if ( uvParam.valid() ) {
                IV2fGeomParam::Sample uvs = uvParam.getIndexedValue(sel);
                V2fArraySamplePtr vals = uvs.getVals();
                UInt32ArraySamplePtr idx = uvs.getIndices();
                if ( vals && idx && (uvParam.getScope() == kFacevaryingScope) && ( idx->size() == m->faceVertexIndices.size() ) ) {
                    size_t b = 0;
                    for (size_t f = 0; f < m->faceVertexCounts.size(); ++f) {
                        const int n = m->faceVertexCounts[f];
                        for (int k = n - 1; k >= 0; --k) {
                            const V2f& v = (*vals)[ (*idx)[b + k] ];
                            Vec2 t;
                            t.x = v.x;
                            t.y = v.y;
                            m->st.push_back(t);
                        }
                        b += n;
                    }
                }
            }
            IN3fGeomParam nParam = s.getNormalsParam();
            if ( nParam.valid() ) {
                IN3fGeomParam::Sample ns = nParam.getExpandedValue(sel);
                N3fArraySamplePtr nv = ns.getVals();
                if ( nv && (nParam.getScope() == kFacevaryingScope) && ( nv->size() == m->faceVertexIndices.size() ) ) {
                    size_t b = 0;
                    for (size_t f = 0; f < m->faceVertexCounts.size(); ++f) {
                        const int n = m->faceVertexCounts[f];
                        for (int k = n - 1; k >= 0; --k) {
                            const N3f& v = (*nv)[b + k];
                            m->normals.push_back( Vec3(v.x, v.y, v.z) );
                        }
                        b += n;
                    }
                }
            }
            if ( m->normals.empty() ) {
                m->computeFlatNormals();
            }
            m->material.emission = 0.; // geometry from DCCs is lit by default
            Prim& p = scene->define(cpath, PrimType::Mesh);
            p.mesh = m;
        } else if ( ICamera::matches(md) ) {
            ICamera c(child, kWrapExisting);
            CameraSample cs;
            c.getSchema().get(cs, sel);
            std::shared_ptr<Camera> cam = std::make_shared<Camera>();
            cam->focalLength = cs.getFocalLength();                 // mm
            cam->horizontalAperture = cs.getHorizontalAperture() * 10.; // cm -> mm
            cam->verticalAperture = cs.getVerticalAperture() * 10.;
            cam->horizontalApertureOffset = cs.getHorizontalFilmOffset() * 10.;
            cam->verticalApertureOffset = cs.getVerticalFilmOffset() * 10.;
            cam->nearClip = cs.getNearClippingPlane();
            cam->farClip = cs.getFarClippingPlane();
            Prim& p = scene->define(cpath, PrimType::Camera);
            p.camera = cam;
        } else {
            scene->define(cpath, PrimType::Xform);
        }
        walk(child, sel, cpath, scene);
    }
}

bool
readAlembic(const std::string& filename, double seconds, const std::string& root, Scene* scene, std::string* error)
{
    try {
        Alembic::AbcCoreFactory::IFactory factory;
        IArchive archive = factory.getArchive(filename);
        if ( !archive.valid() ) {
            if (error) {
                *error = "cannot open Alembic archive " + filename;
            }

            return false;
        }
        scene->define(root, PrimType::Xform);
        walk( archive.getTop(), ISampleSelector(seconds), root, scene );

        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = std::string("Alembic: ") + e.what();
        }

        return false;
    }
}

bool
alembicTimeRange(const std::string& filename, double* start, double* end)
{
    try {
        Alembic::AbcCoreFactory::IFactory factory;
        IArchive archive = factory.getArchive(filename);
        if ( !archive.valid() ) {
            return false;
        }
        double s = 1e30, e = -1e30;
        for (uint32_t i = 0; i < archive.getNumTimeSamplings(); ++i) {
            const index_t n = archive.getMaxNumSamplesForTimeSamplingIndex(i);
            if ( (n == INDEX_UNKNOWN) || (n < 2) ) {
                continue;
            }
            TimeSamplingPtr ts = archive.getTimeSampling(i);
            s = std::min( s, ts->getSampleTime(0) );
            e = std::max( e, ts->getSampleTime(n - 1) );
        }
        if (s > e) {
            return false;
        }
        *start = s;
        *end = e;

        return true;
    } catch (...) {
        return false;
    }
}

static void
writeChildren(OObject parent, const Scene& scene, const std::string& path)
{
    for (const auto& kv : scene.prims()) {
        if (parentPath(kv.first) != path) {
            continue;
        }
        const Prim& p = kv.second;
        // every prim gets an Xform carrying its local matrix; geometry is a child shape
        OXform x( parent, leafName(kv.first) );
        XformSample xs;
        M44d m;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                m[i][j] = p.local.m[j][i];
            }
        }
        xs.setMatrix(m);
        x.getSchema().set(xs);
        if ( (p.type == PrimType::Mesh) && p.mesh ) {
            const Mesh& me = *p.mesh;
            std::vector<V3f> pts;
            for (const Vec3& v : me.points) {
                pts.push_back( V3f( (float)v.x, (float)v.y, (float)v.z ) );
            }
            std::vector<int32_t> idx, counts( me.faceVertexCounts.begin(), me.faceVertexCounts.end() );
            std::vector<V2f> uvs;
            size_t base = 0;
            for (int n : me.faceVertexCounts) { // back to clockwise
                for (int k = n - 1; k >= 0; --k) {
                    idx.push_back(me.faceVertexIndices[base + k]);
                    if ( me.st.size() == me.faceVertexIndices.size() ) {
                        uvs.push_back( V2f( (float)me.st[base + k].x, (float)me.st[base + k].y ) );
                    }
                }
                base += n;
            }
            OPolyMesh om( x, leafName(kv.first) + "Shape" );
            const V3fArraySample ptsS(pts);
            const Int32ArraySample idxS(idx), cntS(counts);
            OPolyMeshSchema::Sample s(ptsS, idxS, cntS);
            if ( !uvs.empty() ) {
                OV2fGeomParam::Sample uvS(V2fArraySample(uvs), kFacevaryingScope);
                s.setUVs(uvS);
            }
            om.getSchema().set(s);
        } else if ( (p.type == PrimType::Camera) && p.camera ) {
            OCamera oc( x, leafName(kv.first) + "Shape" );
            CameraSample cs;
            cs.setFocalLength(p.camera->focalLength);
            cs.setHorizontalAperture(p.camera->horizontalAperture / 10.);
            cs.setVerticalAperture(p.camera->verticalAperture / 10.);
            cs.setNearClippingPlane(p.camera->nearClip);
            cs.setFarClippingPlane(p.camera->farClip);
            oc.getSchema().set(cs);
        }
        writeChildren(x, scene, kv.first);
    }
}

bool
writeAlembic(const std::string& filename, const Scene& scene, std::string* error)
{
    try {
        OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), filename);
        writeChildren(archive.getTop(), scene, std::string() );

        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = std::string("Alembic: ") + e.what();
        }

        return false;
    }
}
} // namespace Solv3D

#else // !SOLVFX_HAS_ALEMBIC

namespace Solv3D {

bool alembicAvailable() { return false; }

bool
readAlembic(const std::string&, double, const std::string&, Scene*, std::string* error)
{
    if (error) {
        *error = "this SOLVFX build has no Alembic support";
    }

    return false;
}

bool alembicTimeRange(const std::string&, double*, double*) { return false; }

bool
writeAlembic(const std::string&, const Scene&, std::string* error)
{
    if (error) {
        *error = "this SOLVFX build has no Alembic support";
    }

    return false;
}
} // namespace Solv3D

#endif // SOLVFX_HAS_ALEMBIC
