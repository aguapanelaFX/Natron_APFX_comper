/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (based on Natron) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * SOLVFX is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
#include <Python.h>

#include "SolvRotoTrack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <sstream>

#include <QtCore/QMetaObject>
#include <QtCore/QString>

#include <libmv/image/image.h>
#include <libmv/tracking/track_region.h>

#include "Global/GLIncludes.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaint.h"
#include "Engine/TimeLine.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"

NATRON_NAMESPACE_ENTER

// --------------------------------------------------------------------------------------------
// knobs

struct SolvRotoTrack::Knobs
{
    KnobPageWPtr page;
    KnobStringWPtr status;
    KnobChoiceWPtr motion;
    KnobIntWPtr refFrame;
    KnobButtonWPtr trackBw, trackPrev, stop, trackNext, trackFw;
    KnobBoolWPtr showSurface, showGrid;
    KnobIntWPtr gridSize;
    KnobButtonWPtr alignSurface, bake, clearAll, clearBw, clearFw;
    KnobChoiceWPtr exportMenu;
    KnobButtonWPtr exportCreate;
    // hidden data
    KnobDoubleWPtr trackCorners;   // 8 dims, animated: tracked quad per frame
    KnobDoubleWPtr surfaceRef;     // 8 dims: surface corners at the reference frame
    KnobDoubleWPtr surface[4];     // animated surface corners (baked, for linked exports)
    KnobDoubleWPtr xfTranslate, xfRotate, xfScale, xfCenter; // animated transform (linked exports)
    KnobStringWPtr shapes;         // script names of the tracked shapes, ';' separated
};

SolvRotoTrack::SolvRotoTrack(RotoPaint* effect)
    : QObject()
    , _effect(effect)
    , _k(new Knobs)
    , _abort(false)
    , _running(false)
    , _dragCorner(-1)
{
}

SolvRotoTrack::~SolvRotoTrack()
{
    _abort = true;
    if ( _worker.joinable() ) {
        _worker.join();
    }
}

std::vector<std::string>
SolvRotoTrack::exportOptions()
{
    std::vector<std::string> o = TrackerContext::solvfxExportOptions();
    o.push_back("Tracker (4 surface corners)");

    return o;
}

static KnobButtonPtr
makeButton(RotoPaint* fx, const KnobPagePtr& page, const char* name, const char* label, const char* hint, const char* icon, bool newLine)
{
    KnobButtonPtr b = AppManager::createKnob<KnobButton>( fx, std::string(label) );
    b->setName(name);
    b->setHintToolTip( std::string(hint) );
    b->setEvaluateOnChange(false);
    b->setIsPersistent(false);
    b->setAddNewLine(newLine);
    if (icon) {
        b->setIconLabel(icon);
    }
    page->addKnob(b);

    return b;
}

void
SolvRotoTrack::createKnobs()
{
    RotoPaint* fx = _effect;
    KnobPagePtr page = AppManager::createKnob<KnobPage>( fx, std::string("Track") );
    page->setName("solvTrackPage");
    _k->page = page;

    KnobStringPtr status = AppManager::createKnob<KnobString>( fx, std::string("tracking") );
    status->setName("solvTrackStatus");
    status->setAsLabel();
    status->setIsPersistent(false);
    status->setEvaluateOnChange(false);
    status->setDefaultValue("select the shapes to track, then press track forward / backward");
    page->addKnob(status);
    _k->status = status;

    KnobChoicePtr motion = AppManager::createKnob<KnobChoice>( fx, std::string("motion") );
    motion->setName("solvTrackMotion");
    motion->setHintToolTip( std::string("Motion of the tracked surface: translation, + rotation, + scale, affine (+ shear) or perspective (planar).") );
    {
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("translation", "T", "") );
        o.push_back( ChoiceOption("rotation", "T R", "") );
        o.push_back( ChoiceOption("scale", "T R S", "") );
        o.push_back( ChoiceOption("affine", "T R S + shear", "") );
        o.push_back( ChoiceOption("perspective", "perspective (planar)", "") );
        motion->populateChoices(o);
    }
    motion->setDefaultValue(4);
    motion->setAnimationEnabled(false);
    motion->setEvaluateOnChange(false);
    page->addKnob(motion);
    _k->motion = motion;

    KnobIntPtr ref = AppManager::createKnob<KnobInt>( fx, std::string("reference frame") );
    ref->setName("solvTrackRefFrame");
    ref->setHintToolTip( std::string("Frame where the track started: the shapes are defined at this frame and the surface corners are set here. "
                        "Clear the track to start again from another frame.") );
    ref->setAnimationEnabled(false);
    ref->setEvaluateOnChange(false);
    ref->setDefaultValue(1);
    ref->setAllDimensionsEnabled(false);
    ref->disableSlider();
    page->addKnob(ref);
    _k->refFrame = ref;

    // track buttons (panel + viewer bar)
    _k->trackBw = makeButton(fx, page, "solvTrackBw", "track backward", "Track the selected shapes backward until stopped (or the start of the comp).", NATRON_IMAGES_PATH "trackBackwardOff.png", false);
    _k->trackPrev = makeButton(fx, page, "solvTrackPrev", "track previous", "Track one frame backward.", NATRON_IMAGES_PATH "trackPrev.png", false);
    _k->stop = makeButton(fx, page, "solvTrackStop", "stop", "Stop tracking (Esc).", NATRON_IMAGES_PATH "pauseDisabled.png", false);
    _k->trackNext = makeButton(fx, page, "solvTrackNext", "track next", "Track one frame forward.", NATRON_IMAGES_PATH "trackNext.png", false);
    _k->trackFw = makeButton(fx, page, "solvTrackFw", "track forward", "Track the selected shapes forward until stopped (or the end of the comp).", NATRON_IMAGES_PATH "trackForwardOff.png", true);

    // surface
    KnobBoolPtr showSurface = AppManager::createKnob<KnobBool>( fx, std::string("surface") );
    showSurface->setName("solvTrackShowSurface");
    showSurface->setHintToolTip( std::string("Show the blue surface: 4 corners (drag them to the real surface) where a corner pin or trackers will land.") );
    showSurface->setDefaultValue(true);
    showSurface->setAnimationEnabled(false);
    showSurface->setEvaluateOnChange(false);
    showSurface->setAddNewLine(false);
    page->addKnob(showSurface);
    _k->showSurface = showSurface;

    KnobBoolPtr showGrid = AppManager::createKnob<KnobBool>( fx, std::string("grid") );
    showGrid->setName("solvTrackShowGrid");
    showGrid->setDefaultValue(true);
    showGrid->setAnimationEnabled(false);
    showGrid->setEvaluateOnChange(false);
    showGrid->setAddNewLine(false);
    page->addKnob(showGrid);
    _k->showGrid = showGrid;

    KnobIntPtr gridSize = AppManager::createKnob<KnobInt>( fx, std::string("") );
    gridSize->setName("solvTrackGridSize");
    gridSize->setHintToolTip( std::string("Grid divisions.") );
    gridSize->setDefaultValue(4);
    gridSize->setMinimum(1);
    gridSize->setMaximum(32);
    gridSize->disableSlider();
    gridSize->setAnimationEnabled(false);
    gridSize->setEvaluateOnChange(false);
    gridSize->setAddNewLine(false);
    page->addKnob(gridSize);
    _k->gridSize = gridSize;

    _k->alignSurface = makeButton(fx, page, "solvTrackAlignSurface", "align to shapes", "Put the surface corners on the bounding box of the tracked shapes (at the reference frame).", 0, true);

    // result
    _k->bake = makeButton(fx, page, "solvTrackBake", "bake to selected shapes", "Bake the track into the selected shapes (they move with the surface). The tracked shapes get it while tracking.", 0, false);
    _k->clearAll = makeButton(fx, page, "solvTrackClear", "clear track", "Remove the track and its motion from the tracked shapes.", 0, false);
    _k->clearBw = makeButton(fx, page, "solvTrackClearBw", "clear backward", "Remove the track before the current frame.", 0, false);
    _k->clearFw = makeButton(fx, page, "solvTrackClearFw", "clear forward", "Remove the track after the current frame.", 0, true);

    // export
    KnobChoicePtr exportMenu = AppManager::createKnob<KnobChoice>( fx, std::string("Export") );
    exportMenu->setName("solvTrackExport");
    exportMenu->setHintToolTip( std::string("Node to create from the surface: linked (follows this track) or baked (keyframes). "
                                            "'use current frame' makes the current frame the identity frame, otherwise the reference frame is used. 1-pt = translation only.") );
    {
        std::vector<ChoiceOption> o;
        const std::vector<std::string> opts = exportOptions();
        for (std::size_t i = 0; i < opts.size(); ++i) {
            o.push_back( ChoiceOption(opts[i], "", "") );
        }
        exportMenu->populateChoices(o);
    }
    exportMenu->setDefaultValue(1);
    exportMenu->setAnimationEnabled(false);
    exportMenu->setEvaluateOnChange(false);
    exportMenu->setAddNewLine(false);
    page->addKnob(exportMenu);
    _k->exportMenu = exportMenu;
    _k->exportCreate = makeButton(fx, page, "solvTrackExportCreate", "create", "Create the node selected in the Export menu.", 0, true);

    // hidden data
    KnobDoublePtr corners = AppManager::createKnob<KnobDouble>( fx, std::string("trackCorners"), 8 );
    corners->setName("solvTrackCorners");
    corners->setSecretByDefault(true);
    corners->setEvaluateOnChange(false);
    page->addKnob(corners);
    _k->trackCorners = corners;

    KnobDoublePtr sref = AppManager::createKnob<KnobDouble>( fx, std::string("surfaceRef"), 8 );
    sref->setName("solvTrackSurfaceRef");
    sref->setSecretByDefault(true);
    sref->setAnimationEnabled(false);
    sref->setEvaluateOnChange(false);
    page->addKnob(sref);
    _k->surfaceRef = sref;

    for (int i = 0; i < 4; ++i) {
        std::stringstream ss;
        ss << "solvTrackSurface" << (i + 1);
        std::stringstream sl;
        sl << "surface " << (i + 1);
        KnobDoublePtr s = AppManager::createKnob<KnobDouble>( fx, sl.str(), 2 );
        s->setName( ss.str() );
        s->setSecretByDefault(true);
        s->setEvaluateOnChange(false);
        page->addKnob(s);
        _k->surface[i] = s;
    }
    KnobDoublePtr t = AppManager::createKnob<KnobDouble>( fx, std::string("translate"), 2 );
    t->setName("solvTrackTranslate"); t->setSecretByDefault(true); t->setEvaluateOnChange(false); page->addKnob(t); _k->xfTranslate = t;
    KnobDoublePtr r = AppManager::createKnob<KnobDouble>( fx, std::string("rotate"), 1 );
    r->setName("solvTrackRotate"); r->setSecretByDefault(true); r->setEvaluateOnChange(false); page->addKnob(r); _k->xfRotate = r;
    KnobDoublePtr sc = AppManager::createKnob<KnobDouble>( fx, std::string("scale"), 2 );
    sc->setName("solvTrackScale"); sc->setSecretByDefault(true); sc->setEvaluateOnChange(false); sc->setDefaultValue(1., 0); sc->setDefaultValue(1., 1); page->addKnob(sc); _k->xfScale = sc;
    KnobDoublePtr c = AppManager::createKnob<KnobDouble>( fx, std::string("center"), 2 );
    c->setName("solvTrackCenter"); c->setSecretByDefault(true); c->setAnimationEnabled(false); c->setEvaluateOnChange(false); page->addKnob(c); _k->xfCenter = c;

    KnobStringPtr shapes = AppManager::createKnob<KnobString>( fx, std::string("trackedShapes") );
    shapes->setName("solvTrackShapes");
    shapes->setSecretByDefault(true);
    shapes->setAnimationEnabled(false);
    shapes->setEvaluateOnChange(false);
    page->addKnob(shapes);
    _k->shapes = shapes;

    // viewer bar (after the roto tools)
    KnobButtonPtr vb[5] = { _k->trackBw.lock(), _k->trackPrev.lock(), _k->stop.lock(), _k->trackNext.lock(), _k->trackFw.lock() };
    for (int i = 0; i < 5; ++i) {
        vb[i]->setInViewerContextCanHaveShortcut(true);
        fx->addKnobToViewerUI(vb[i]);
        vb[i]->setInViewerContextItemSpacing(1);
    }
    vb[0]->setInViewerContextItemSpacing(1);
    fx->addKnobToViewerUI(showSurface);
    fx->addKnobToViewerUI(showGrid);
    showSurface->setInViewerContextLabel( QString::fromUtf8("surface") );
    showGrid->setInViewerContextLabel( QString::fromUtf8("grid") );
    fx->addOverlaySlaveParam(showSurface);
    fx->addOverlaySlaveParam(showGrid);
    fx->addOverlaySlaveParam(gridSize);
    fx->addOverlaySlaveParam(sref);
    fx->addOverlaySlaveParam(corners);
}

// --------------------------------------------------------------------------------------------
// geometry

bool
SolvRotoTrack::homography(const Quad& from,
                          const Quad& to,
                          Transform::Matrix3x3* H)
{
    Transform::Point3D p[4], q[4];
    for (int i = 0; i < 4; ++i) {
        p[i] = Transform::Point3D(from.x[i], from.y[i], 1.);
        q[i] = Transform::Point3D(to.x[i], to.y[i], 1.);
    }

    return H->setHomographyFromFourPoints(p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3]);
}

static void
applyH(const Transform::Matrix3x3& H,
       double* x,
       double* y)
{
    double z = 1.;
    Transform::matApply(H, x, y, &z);
    if (z != 0.) {
        *x /= z;
        *y /= z;
    }
}

bool
SolvRotoTrack::getQuadAtTime(double time,
                             Quad* q) const
{
    KnobDoublePtr k = _k->trackCorners.lock();
    if ( !k || (k->getKeyFramesCount(ViewSpec(0), 0) == 0) ) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        q->x[i] = k->getValueAtTime(time, 2 * i);
        q->y[i] = k->getValueAtTime(time, 2 * i + 1);
    }

    return true;
}

bool
SolvRotoTrack::getRefQuad(Quad* q) const
{
    return getQuadAtTime(_k->refFrame.lock()->getValue(), q);
}

void
SolvRotoTrack::getSurfaceRef(Quad* q) const
{
    KnobDoublePtr k = _k->surfaceRef.lock();
    for (int i = 0; i < 4; ++i) {
        q->x[i] = k->getValue(2 * i);
        q->y[i] = k->getValue(2 * i + 1);
    }
}

void
SolvRotoTrack::setSurfaceRef(const Quad& q)
{
    KnobDoublePtr k = _k->surfaceRef.lock();
    k->beginChanges();
    for (int i = 0; i < 4; ++i) {
        k->setValue(q.x[i], ViewSpec::all(), 2 * i);
        k->setValue(q.y[i], ViewSpec::all(), 2 * i + 1);
    }
    k->endChanges();
}

SolvRotoTrack::Quad
SolvRotoTrack::surfaceAtTime(double time) const
{
    Quad s;
    getSurfaceRef(&s);
    Quad qr, qt;
    Transform::Matrix3x3 H;
    if ( getRefQuad(&qr) && getQuadAtTime(time, &qt) && homography(qr, qt, &H) ) {
        for (int i = 0; i < 4; ++i) {
            applyH(H, &s.x[i], &s.y[i]);
        }
    }

    return s;
}

std::list<RotoDrawableItemPtr>
SolvRotoTrack::trackedShapes() const
{
    std::list<RotoDrawableItemPtr> ret;
    RotoContextPtr ctx = _effect->getNode()->getRotoContext();
    if (!ctx) {
        return ret;
    }
    const std::string s = _k->shapes.lock()->getValue();
    std::stringstream ss(s);
    std::string name;
    while ( std::getline(ss, name, ';') ) {
        if ( name.empty() ) {
            continue;
        }
        RotoItemPtr it = ctx->getItemByName(name);
        RotoDrawableItemPtr d = std::dynamic_pointer_cast<RotoDrawableItem>(it);
        if (d) {
            ret.push_back(d);
        }
    }

    return ret;
}

void
SolvRotoTrack::getTrackedFrames(std::vector<double>* frames) const
{
    frames->clear();
    KnobDoublePtr k = _k->trackCorners.lock();
    CurvePtr c = k->getCurve(ViewSpec(0), 0);
    if (!c) {
        return;
    }
    KeyFrameSet keys = c->getKeyFrames_mt_safe();
    for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        frames->push_back( it->getTime() );
    }
}

static bool
isExcludeLayer(const RotoItemPtr& item)
{
    for (RotoLayerPtr l = item->getParentLayer(); l; l = l->getParentLayer()) {
        std::string n = l->getLabel();
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n.find("exclude") != std::string::npos) {
            return true;
        }
    }

    return false;
}

typedef std::vector<std::pair<double, double> > Polygon;

static void
shapePolygon(const BezierPtr& b,
             double time,
             Polygon* poly)
{
    poly->clear();
    std::list<std::list<ParametricPoint> > pts;
    RectD bbox;
    b->evaluateAtTime_DeCasteljau_autoNbPoints(false, time, 0, &pts, &bbox);
    for (std::list<std::list<ParametricPoint> >::const_iterator s = pts.begin(); s != pts.end(); ++s) {
        for (std::list<ParametricPoint>::const_iterator p = s->begin(); p != s->end(); ++p) {
            poly->push_back( std::make_pair(p->x, p->y) );
        }
    }
}

// --------------------------------------------------------------------------------------------
// worker: images and masks

struct LevelImage
{
    libmv::FloatImage img;
    RectI bounds;   // pixel bounds at this level
    unsigned int level;
};

// canonical <-> libmv pixel (top-left origin, pixel centers)
static inline double
toCol(const LevelImage& L, double x) { return x / (double)(1 << L.level) - L.bounds.x1 - 0.5; }
static inline double
toRow(const LevelImage& L, double y) { return L.bounds.y2 - y / (double)(1 << L.level) - 0.5; }
static inline double
fromCol(const LevelImage& L, double c) { return (c + L.bounds.x1 + 0.5) * (double)(1 << L.level); }
static inline double
fromRow(const LevelImage& L, double r) { return (L.bounds.y2 - r - 0.5) * (double)(1 << L.level); }

static bool
renderLuma(const NodePtr& rotoNode,
           int frame,
           unsigned int mip,
           LevelImage* out)
{
    NodePtr input = rotoNode->getInput(0);
    if (!input) {
        return false;
    }
    EffectInstancePtr effect = input->getEffectInstance();
    if (!effect) {
        return false;
    }
    const RenderScale scale = RenderScale::fromMipmapLevel(mip);
    RectD rod;
    bool isProjectFormat = false;
    if (effect->getRegionOfDefinition_public(input->getHashValue(), frame, scale, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    const double par = effect->getAspectRatio(-1);
    RectI roi = rod.toPixelEnclosing(mip, par);
    if ( roi.isNull() ) {
        return false;
    }
    std::list<ImagePlaneDesc> comps;
    comps.push_back( ImagePlaneDesc::getRGBComponents() );
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter setter( frame, ViewIdx(0), true, false, abortInfo, rotoNode, 0,
                                     rotoNode->getApp()->getTimeLine().get(), NodePtr(), true, false, RenderStatsPtr() );
    EffectInstance::RenderRoIArgs args( frame, scale, mip, ViewIdx(0), false, roi, rod, comps, eImageBitDepthFloat, true,
                                        rotoNode->getEffectInstance().get(), eStorageModeRAM, frame );
    std::map<ImagePlaneDesc, ImagePtr> planes;
    if ( (effect->renderRoI(args, &planes) != EffectInstance::eRenderRoIRetCodeOk) || planes.empty() ) {
        return false;
    }
    const ImagePtr& img = planes.begin()->second;
    const RectI b = roi.intersect( img->getBounds() );
    if ( b.isNull() ) {
        return false;
    }
    const int w = b.width(), h = b.height();
    out->img.Resize(h, w, 1);
    out->bounds = b;
    out->level = mip;
    Image::ReadAccess racc( img.get() );
    const int nc = (int)img->getComponentsCount();
    for (int r = 0; r < h; ++r) {
        const int y = b.y2 - 1 - r;
        const float* src = (const float*)racc.pixelAt(b.x1, y);
        float* dst = out->img.Data() + (std::size_t)r * w;
        if (!src) {
            std::fill(dst, dst + w, 0.f);
            continue;
        }
        for (int c = 0; c < w; ++c, src += nc) {
            dst[c] = nc >= 3 ? (0.2126f * src[0] + 0.7152f * src[1] + 0.0722f * src[2]) : src[0];
        }
    }

    return true;
}

static void
halfSize(const LevelImage& in,
         LevelImage* out)
{
    const int w = std::max(1, in.img.Width() / 2), h = std::max(1, in.img.Height() / 2);
    out->img.Resize(h, w, 1);
    out->level = in.level + 1;
    out->bounds.x1 = (int)std::floor(in.bounds.x1 / 2.);
    out->bounds.x2 = out->bounds.x1 + w;
    out->bounds.y2 = (int)std::ceil(in.bounds.y2 / 2.);
    out->bounds.y1 = out->bounds.y2 - h;
    const int iw = in.img.Width();
    for (int r = 0; r < h; ++r) {
        const float* a = in.img.Data() + (std::size_t)(2 * r) * iw;
        const float* b = in.img.Data() + (std::size_t)std::min(2 * r + 1, in.img.Height() - 1) * iw;
        float* d = out->img.Data() + (std::size_t)r * w;
        for (int c = 0; c < w; ++c) {
            const int c1 = std::min(2 * c + 1, iw - 1);
            d[c] = 0.25f * (a[2 * c] + a[c1] + b[2 * c] + b[c1]);
        }
    }
}

// even-odd scanline fill of canonical polygons into a mask the size of L
static void
rasterize(const LevelImage& L,
          const std::vector<Polygon>& polys,
          float value,
          libmv::FloatImage* mask)
{
    const int w = L.img.Width(), h = L.img.Height();
    std::vector<double> xs;
    for (std::size_t p = 0; p < polys.size(); ++p) {
        const Polygon& poly = polys[p];
        const std::size_t n = poly.size();
        if (n < 3) {
            continue;
        }
        std::vector<std::pair<double, double> > pix(n);
        double rmin = 1e30, rmax = -1e30;
        for (std::size_t i = 0; i < n; ++i) {
            pix[i].first = toCol(L, poly[i].first);
            pix[i].second = toRow(L, poly[i].second);
            rmin = std::min(rmin, pix[i].second);
            rmax = std::max(rmax, pix[i].second);
        }
        const int r0 = std::max(0, (int)std::floor(rmin)), r1 = std::min(h - 1, (int)std::ceil(rmax));
        for (int r = r0; r <= r1; ++r) {
            const double yc = r + 0.5;
            xs.clear();
            for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                const double yi = pix[i].second, yj = pix[j].second;
                if ( (yi > yc) != (yj > yc) ) {
                    xs.push_back( pix[i].first + (yc - yi) * (pix[j].first - pix[i].first) / (yj - yi) );
                }
            }
            std::sort( xs.begin(), xs.end() );
            float* row = mask->Data() + (std::size_t)r * w;
            for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int c0 = std::max(0, (int)std::ceil(xs[k] - 0.5)), c1 = std::min(w - 1, (int)std::floor(xs[k + 1] - 0.5));
                for (int c = c0; c <= c1; ++c) {
                    row[c] = value;
                }
            }
        }
    }
}

// --------------------------------------------------------------------------------------------
// tracking

void
SolvRotoTrack::startTracking(int direction,
                             bool oneFrame)
{
    if (_running) {
        return;
    }
    if ( _worker.joinable() ) {
        _worker.join();
    }
    NodePtr node = _effect->getNode();
    if ( !node->getInput(0) ) {
        Dialogs::errorDialog("SolvRoto", "Connect the plate to the Bg input to track.");

        return;
    }
    RotoContextPtr ctx = node->getRotoContext();
    const int start = node->getApp()->getTimeLine()->currentFrame();

    // tracked shapes: the selection, or the shapes already tracked
    std::list<RotoDrawableItemPtr> shapes;
    {
        std::list<RotoDrawableItemPtr> sel = ctx->getSelectedCurves();
        for (std::list<RotoDrawableItemPtr>::iterator it = sel.begin(); it != sel.end(); ++it) {
            if ( std::dynamic_pointer_cast<Bezier>(*it) && !isExcludeLayer(*it) ) {
                shapes.push_back(*it);
            }
        }
    }
    Quad dummy;
    const bool hasTrack = getQuadAtTime(start, &dummy);
    if ( shapes.empty() ) {
        shapes = trackedShapes();
    }
    if ( shapes.empty() ) {
        Dialogs::errorDialog("SolvRoto", "Select the shapes to track (closed Bezier shapes).");

        return;
    }

    KnobDoublePtr corners = _k->trackCorners.lock();
    Quad qStart;
    if (hasTrack) {
        // continue: start from the tracked quad at this frame (must be a tracked frame)
        std::vector<double> frames;
        getTrackedFrames(&frames);
        if ( std::find(frames.begin(), frames.end(), (double)start) == frames.end() ) {
            Dialogs::errorDialog("SolvRoto", "Go to a tracked frame to continue tracking (or clear the track).");

            return;
        }
        getQuadAtTime(start, &qStart);
    } else {
        // new track: the reference frame is this frame, quad = shapes bounding box
        RectD bb;
        bool first = true;
        for (std::list<RotoDrawableItemPtr>::iterator it = shapes.begin(); it != shapes.end(); ++it) {
            (*it)->setExtraMatrix( false, start, Transform::Matrix3x3(1, 0, 0, 0, 1, 0, 0, 0, 1) );
            KnobIPtr extra = (*it)->getKnobByName("extraMatrix");
            if (extra) {
                for (int d = 0; d < extra->getDimension(); ++d) {
                    extra->removeAnimation(ViewSpec::all(), d);
                }
            }
            Polygon p;
            shapePolygon(std::dynamic_pointer_cast<Bezier>(*it), start, &p);
            for (std::size_t i = 0; i < p.size(); ++i) {
                if (first) {
                    bb.x1 = bb.x2 = p[i].first;
                    bb.y1 = bb.y2 = p[i].second;
                    first = false;
                } else {
                    bb.x1 = std::min(bb.x1, p[i].first); bb.x2 = std::max(bb.x2, p[i].first);
                    bb.y1 = std::min(bb.y1, p[i].second); bb.y2 = std::max(bb.y2, p[i].second);
                }
            }
        }
        if ( first || (bb.width() < 8) || (bb.height() < 8) ) {
            Dialogs::errorDialog("SolvRoto", "The selected shapes are too small to track.");

            return;
        }
        // corners: top-left, top-right, bottom-right, bottom-left (canonical y up)
        qStart.x[0] = bb.x1; qStart.y[0] = bb.y2;
        qStart.x[1] = bb.x2; qStart.y[1] = bb.y2;
        qStart.x[2] = bb.x2; qStart.y[2] = bb.y1;
        qStart.x[3] = bb.x1; qStart.y[3] = bb.y1;
        _k->refFrame.lock()->setValue(start);
        corners->beginChanges();
        for (int i = 0; i < 4; ++i) {
            corners->setValueAtTime(start, qStart.x[i], ViewSpec::all(), 2 * i);
            corners->setValueAtTime(start, qStart.y[i], ViewSpec::all(), 2 * i + 1);
        }
        corners->endChanges();
        setSurfaceRef(qStart);
        std::string names;
        for (std::list<RotoDrawableItemPtr>::iterator it = shapes.begin(); it != shapes.end(); ++it) {
            names += (*it)->getScriptName() + ";";
        }
        _k->shapes.lock()->setValue(names);
        // identity at the reference frame (keys are written on every tracked frame)
        for (std::list<RotoDrawableItemPtr>::iterator it = shapes.begin(); it != shapes.end(); ++it) {
            (*it)->setExtraMatrix( true, start, Transform::Matrix3x3(1, 0, 0, 0, 1, 0, 0, 0, 1) );
        }
        refreshDerivedAt(start);
    }

    // polygons at the start frame (include / exclude), in canonical coordinates
    std::vector<Polygon> include, exclude;
    for (std::list<RotoDrawableItemPtr>::iterator it = shapes.begin(); it != shapes.end(); ++it) {
        Polygon p;
        shapePolygon(std::dynamic_pointer_cast<Bezier>(*it), start, &p);
        include.push_back(p);
    }
    {
        std::list<RotoDrawableItemPtr> all = ctx->getCurvesByRenderOrder();
        for (std::list<RotoDrawableItemPtr>::iterator it = all.begin(); it != all.end(); ++it) {
            BezierPtr b = std::dynamic_pointer_cast<Bezier>(*it);
            if ( b && isExcludeLayer(*it) ) {
                Polygon p;
                shapePolygon(b, start, &p);
                exclude.push_back(p);
            }
        }
    }

    double first = 0, last = 0;
    node->getApp()->getProject()->getFrameRange(&first, &last);
    const int endFrame = oneFrame ? start + direction : (direction > 0 ? (int)last : (int)first);
    if ( (direction > 0 && start >= last) || (direction < 0 && start <= first) ) {
        return;
    }

    int modeIdx = _k->motion.lock()->getValue();
    libmv::TrackRegionOptions::Mode mode = libmv::TrackRegionOptions::HOMOGRAPHY;
    switch (modeIdx) {
    case 0: mode = libmv::TrackRegionOptions::TRANSLATION; break;
    case 1: mode = libmv::TrackRegionOptions::TRANSLATION_ROTATION; break;
    case 2: mode = libmv::TrackRegionOptions::TRANSLATION_ROTATION_SCALE; break;
    case 3: mode = libmv::TrackRegionOptions::AFFINE; break;
    default: mode = libmv::TrackRegionOptions::HOMOGRAPHY; break;
    }

    // tracking resolution: the quad's longest side ~<= 512 px
    double side = 0;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        side = std::max( side, std::hypot(qStart.x[j] - qStart.x[i], qStart.y[j] - qStart.y[i]) );
    }
    unsigned int mip = 0;
    while (side / (double)(1 << mip) > 512. && mip < 3) {
        ++mip;
    }

    _abort = false;
    _running = true;
    _k->status.lock()->setValue("tracking... (stop or Esc to stop)");
    SolvRotoTrack* self = this;
    NodePtr rotoNode = node;
    _worker = std::thread([self, rotoNode, start, endFrame, direction, qStart, include, exclude, mode, mip]() {
        Quad q = qStart, prev = qStart;
        bool havePrev = false;
        LevelImage cur;
        QString message = QString::fromUtf8("track done");
        if ( !renderLuma(rotoNode, start, mip, &cur) ) {
            message = QString::fromUtf8("could not read the input image");
        } else {
            for (int f = start; (direction > 0 ? f < endFrame : f > endFrame) && !self->_abort; f += direction) {
                const int nf = f + direction;
                LevelImage next;
                if ( !renderLuma(rotoNode, nf, mip, &next) ) {
                    message = QString::fromUtf8("could not read frame %1").arg(nf);
                    break;
                }
                // mask: shapes moved by the motion so far
                Transform::Matrix3x3 H;
                std::vector<Polygon> inc = include, exc = exclude;
                if ( SolvRotoTrack::homography(qStart, q, &H) ) {
                    for (std::size_t p = 0; p < inc.size(); ++p) {
                        for (std::size_t i = 0; i < inc[p].size(); ++i) {
                            applyH(H, &inc[p][i].first, &inc[p][i].second);
                        }
                    }
                }
                // occluders are not tracked: they stay where they were drawn at their own frame
                libmv::FloatImage mask(cur.img.Height(), cur.img.Width(), 1);
                mask.Fill(0.f);
                rasterize(cur, inc, 1.f, &mask);
                rasterize(cur, exc, 0.f, &mask);

                // initial guess: constant velocity
                Quad guess = q;
                if (havePrev) {
                    for (int i = 0; i < 4; ++i) {
                        guess.x[i] = q.x[i] + (q.x[i] - prev.x[i]);
                        guess.y[i] = q.y[i] + (q.y[i] - prev.y[i]);
                    }
                }

                libmv::TrackRegionOptions opts;
                opts.mode = mode;
                opts.minimum_correlation = 0.0;
                opts.max_iterations = 50;
                opts.use_esm = true;
                opts.use_brute_initialization = false;
                opts.use_normalized_intensities = true;
                opts.sigma = 0.9;
                opts.num_extra_points = 0;
                opts.regularization_coefficient = 0.0;
                opts.minimum_corner_shift_tolerance_pixels = 0.005;

                // coarse pass (half resolution) then full
                double x1[4], y1[4], x2[4], y2[4];
                {
                    LevelImage c1, c2;
                    halfSize(cur, &c1);
                    halfSize(next, &c2);
                    libmv::FloatImage cmask(c1.img.Height(), c1.img.Width(), 1);
                    cmask.Fill(0.f);
                    rasterize(c1, inc, 1.f, &cmask);
                    rasterize(c1, exc, 0.f, &cmask);
                    for (int i = 0; i < 4; ++i) {
                        x1[i] = toCol(c1, q.x[i]); y1[i] = toRow(c1, q.y[i]);
                        x2[i] = toCol(c2, guess.x[i]); y2[i] = toRow(c2, guess.y[i]);
                    }
                    libmv::TrackRegionOptions co = opts;
                    co.image1_mask = &cmask;
                    co.sigma = 0.6;
                    libmv::TrackRegionResult cres;
                    libmv::TrackRegion(c1.img, c2.img, x1, y1, co, x2, y2, &cres);
                    if ( cres.is_usable() ) {
                        for (int i = 0; i < 4; ++i) {
                            guess.x[i] = fromCol(c2, x2[i]);
                            guess.y[i] = fromRow(c2, y2[i]);
                        }
                    }
                }
                for (int i = 0; i < 4; ++i) {
                    x1[i] = toCol(cur, q.x[i]); y1[i] = toRow(cur, q.y[i]);
                    x2[i] = toCol(next, guess.x[i]); y2[i] = toRow(next, guess.y[i]);
                }
                opts.image1_mask = &mask;
                libmv::TrackRegionResult res;
                libmv::TrackRegion(cur.img, next.img, x1, y1, opts, x2, y2, &res);
                if ( !res.is_usable() ) {
                    message = QString::fromUtf8("lost the surface at frame %1 (adjust the shapes or the motion, then continue)").arg(nf);
                    break;
                }
                Quad nq;
                for (int i = 0; i < 4; ++i) {
                    nq.x[i] = fromCol(next, x2[i]);
                    nq.y[i] = fromRow(next, y2[i]);
                }
                const double corr = res.correlation;
                std::fprintf(stderr, "[SOLVFX] SolvRoto frame %d tracked (correlation %.3f, %d iterations)\n", nf, corr, res.num_iterations);
                QMetaObject::invokeMethod(self, [self, nf, nq, corr]() { self->onFrameTracked(nf, nq, true, corr); }, Qt::QueuedConnection);
                prev = q;
                havePrev = true;
                q = nq;
                cur.img = next.img;
                cur.bounds = next.bounds;
            }
            if (self->_abort) {
                message = QString::fromUtf8("stopped");
            }
        }
        QMetaObject::invokeMethod(self, [self, message]() { self->onTrackingEnded(message); }, Qt::QueuedConnection);
    });
}

void
SolvRotoTrack::stopTracking()
{
    _abort = true;
}

void
SolvRotoTrack::onFrameTracked(int frame,
                              Quad q,
                              bool /*ok*/,
                              double correlation)
{
    applyFrame(frame, q);
    std::stringstream ss;
    ss << "tracking frame " << frame << " (match " << (int)std::floor(correlation * 100 + 0.5) << "%)";
    _k->status.lock()->setValue( ss.str() );
    NodePtr node = _effect->getNode();
    node->getApp()->getTimeLine()->seekFrame(frame, false, 0, eTimelineChangeReasonOtherSeek);
    redraw();
}

void
SolvRotoTrack::onTrackingEnded(QString message)
{
    if ( _worker.joinable() ) {
        _worker.join();
    }
    _running = false;
    _k->status.lock()->setValue( message.toStdString() );
    redraw();
}

void
SolvRotoTrack::applyFrame(int frame,
                          const Quad& q)
{
    KnobDoublePtr corners = _k->trackCorners.lock();
    corners->beginChanges();
    for (int i = 0; i < 4; ++i) {
        corners->setValueAtTime(frame, q.x[i], ViewSpec::all(), 2 * i);
        corners->setValueAtTime(frame, q.y[i], ViewSpec::all(), 2 * i + 1);
    }
    corners->endChanges();

    // tracked shapes follow the surface (baked keyframes)
    Quad qr;
    Transform::Matrix3x3 H;
    if ( getRefQuad(&qr) && homography(qr, q, &H) ) {
        std::list<RotoDrawableItemPtr> shapes = trackedShapes();
        for (std::list<RotoDrawableItemPtr>::iterator it = shapes.begin(); it != shapes.end(); ++it) {
            (*it)->setExtraMatrix(true, frame, H);
        }
    }
    refreshDerivedAt(frame);
}

// surface corners + transform (match-move, relative to the reference frame) at one frame
void
SolvRotoTrack::refreshDerivedAt(double frame)
{
    const Quad s = surfaceAtTime(frame);
    Quad sr;
    getSurfaceRef(&sr);
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr k = _k->surface[i].lock();
        k->setValueAtTime(frame, s.x[i], ViewSpec::all(), 0);
        k->setValueAtTime(frame, s.y[i], ViewSpec::all(), 1);
    }
    double cx = 0, cy = 0, rx = 0, ry = 0;
    for (int i = 0; i < 4; ++i) {
        cx += s.x[i] / 4.; cy += s.y[i] / 4.;
        rx += sr.x[i] / 4.; ry += sr.y[i] / 4.;
    }
    // rotation: mean of the two horizontal edges; scale: mean edge length ratio
    const double a0 = std::atan2(sr.y[1] - sr.y[0], sr.x[1] - sr.x[0]) + std::atan2(sr.y[2] - sr.y[3], sr.x[2] - sr.x[3]);
    const double a1 = std::atan2(s.y[1] - s.y[0], s.x[1] - s.x[0]) + std::atan2(s.y[2] - s.y[3], s.x[2] - s.x[3]);
    double l0 = 0, l1 = 0;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        l0 += std::hypot(sr.x[j] - sr.x[i], sr.y[j] - sr.y[i]);
        l1 += std::hypot(s.x[j] - s.x[i], s.y[j] - s.y[i]);
    }
    _k->xfTranslate.lock()->setValueAtTime(frame, cx - rx, ViewSpec::all(), 0);
    _k->xfTranslate.lock()->setValueAtTime(frame, cy - ry, ViewSpec::all(), 1);
    _k->xfRotate.lock()->setValueAtTime(frame, Transform::toDegrees( (a1 - a0) / 2. ), ViewSpec::all(), 0);
    const double sc = l0 > 0 ? l1 / l0 : 1.;
    _k->xfScale.lock()->setValueAtTime(frame, sc, ViewSpec::all(), 0);
    _k->xfScale.lock()->setValueAtTime(frame, sc, ViewSpec::all(), 1);
    _k->xfCenter.lock()->setValue(rx, ViewSpec::all(), 0);
    _k->xfCenter.lock()->setValue(ry, ViewSpec::all(), 1);
}

void
SolvRotoTrack::refreshDerived()
{
    std::vector<double> frames;
    getTrackedFrames(&frames);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        refreshDerivedAt(frames[i]);
    }
}

void
SolvRotoTrack::bakeToSelectedShapes()
{
    RotoContextPtr ctx = _effect->getNode()->getRotoContext();
    std::list<RotoDrawableItemPtr> sel = ctx->getSelectedCurves();
    Quad qr;
    if ( sel.empty() || !getRefQuad(&qr) ) {
        Dialogs::errorDialog("SolvRoto", "Track first, then select the shapes to bake the track into.");

        return;
    }
    std::vector<double> frames;
    getTrackedFrames(&frames);
    std::string names = _k->shapes.lock()->getValue();
    for (std::list<RotoDrawableItemPtr>::iterator it = sel.begin(); it != sel.end(); ++it) {
        for (std::size_t f = 0; f < frames.size(); ++f) {
            Quad q;
            Transform::Matrix3x3 H;
            if ( getQuadAtTime(frames[f], &q) && homography(qr, q, &H) ) {
                (*it)->setExtraMatrix(true, frames[f], H);
            }
        }
        const std::string n = (*it)->getScriptName() + ";";
        if (names.find(n) == std::string::npos) {
            names += n; // keeps following the track when tracking continues
        }
    }
    _k->shapes.lock()->setValue(names);
    redraw();
}

void
SolvRotoTrack::clearTrack(int which)
{
    const double now = _effect->getNode()->getApp()->getTimeLine()->currentFrame();
    std::vector<double> frames;
    getTrackedFrames(&frames);
    std::list<double> toDelete;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if ( (which == 0) || (which < 0 && frames[i] < now) || (which > 0 && frames[i] > now) ) {
            toDelete.push_back(frames[i]);
        }
    }
    if ( toDelete.empty() ) {
        return;
    }
    std::list<RotoDrawableItemPtr> shapes = trackedShapes();
    std::vector<KnobIPtr> knobs;
    knobs.push_back( _k->trackCorners.lock() );
    for (int i = 0; i < 4; ++i) {
        knobs.push_back( _k->surface[i].lock() );
    }
    knobs.push_back( _k->xfTranslate.lock() );
    knobs.push_back( _k->xfRotate.lock() );
    knobs.push_back( _k->xfScale.lock() );
    for (std::list<RotoDrawableItemPtr>::iterator it = shapes.begin(); it != shapes.end(); ++it) {
        KnobIPtr extra = (*it)->getKnobByName("extraMatrix");
        if (extra) {
            knobs.push_back(extra);
        }
    }
    for (std::size_t k = 0; k < knobs.size(); ++k) {
        for (int d = 0; d < knobs[k]->getDimension(); ++d) {
            if (which == 0) {
                knobs[k]->removeAnimation(ViewSpec::all(), d);
            } else {
                knobs[k]->deleteValuesAtTime(eCurveChangeReasonInternal, toDelete, ViewSpec::all(), d, false);
            }
        }
    }
    if (which == 0) {
        for (std::list<RotoDrawableItemPtr>::iterator it = shapes.begin(); it != shapes.end(); ++it) {
            (*it)->setExtraMatrix( false, now, Transform::Matrix3x3(1, 0, 0, 0, 1, 0, 0, 0, 1) );
        }
        _k->shapes.lock()->setValue("");
        _k->status.lock()->setValue("track cleared");
    }
    redraw();
}

// --------------------------------------------------------------------------------------------
// export

void
SolvRotoTrack::exportNode(int option)
{
    Quad qr;
    if ( !getRefQuad(&qr) ) {
        Dialogs::errorDialog("SolvRoto", "Track a surface first.");

        return;
    }
    NodePtr thisNode = _effect->getNode();
    AppInstancePtr app = thisNode->getApp();
    const int nOpts = (int)TrackerContext::solvfxExportOptions().size();
    const double now = app->getTimeLine()->currentFrame();
    std::vector<double> frames;
    getTrackedFrames(&frames);

    std::string pluginID;
    if (option == nOpts) {
        pluginID = PLUGINID_NATRON_TRACKER;
    } else if (option < 4) {
        pluginID = PLUGINID_OFX_CORNERPIN;
    } else {
        pluginID = PLUGINID_OFX_TRANSFORM;
    }
    CreateNodeArgs args( pluginID, thisNode->getGroup() );
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    NodePtr created = app->createNode(args);
    if (!created) {
        return;
    }
    double pos[2], size[2];
    thisNode->getPosition(&pos[0], &pos[1]);
    thisNode->getSize(&size[0], &size[1]);
    created->setPosition(pos[0] + size[0] * 2., pos[1]);

    if (option == nOpts) {
        // Tracker with the 4 surface corners as tracks
        TrackerContextPtr tctx = created->getTrackerContext();
        if (!tctx) {
            return;
        }
        for (int i = 0; i < 4; ++i) {
            TrackMarkerPtr m = tctx->createMarker();
            if (!m) {
                continue;
            }
            std::stringstream ss;
            ss << "corner" << (i + 1);
            m->setLabel( ss.str() );
            KnobDoublePtr center = m->getCenterKnob();
            KnobDoublePtr src = _k->surface[i].lock();
            for (std::size_t f = 0; f < frames.size(); ++f) {
                center->setValueAtTime(frames[f], src->getValueAtTime(frames[f], 0), ViewSpec::all(), 0);
                center->setValueAtTime(frames[f], src->getValueAtTime(frames[f], 1), ViewSpec::all(), 1);
            }
        }

        return;
    }

    const bool linked = (option <= 1) || (option >= 4 && option <= 7);
    const bool useCurrentFrame = (option == 0) || (option == 2);
    const bool stabilize = (option == 4) || (option == 5) || (option == 8) || (option == 9);
    const bool onePoint = (option == 5) || (option == 7) || (option == 9) || (option == 11);

    if (option < 4) {
        // CornerPin2D: from = surface at the identity frame, to = surface per frame
        const Quad from = useCurrentFrame ? surfaceAtTime(now) : surfaceAtTime( _k->refFrame.lock()->getValue() );
        for (int i = 0; i < 4; ++i) {
            std::stringstream fs, ts;
            fs << "from" << (i + 1);
            ts << "to" << (i + 1);
            KnobDoublePtr fk = std::dynamic_pointer_cast<KnobDouble>( created->getKnobByName( fs.str() ) );
            KnobDoublePtr tk = std::dynamic_pointer_cast<KnobDouble>( created->getKnobByName( ts.str() ) );
            if (fk) {
                fk->setValue(from.x[i], ViewSpec::all(), 0);
                fk->setValue(from.y[i], ViewSpec::all(), 1);
            }
            if (tk) {
                KnobDoublePtr src = _k->surface[i].lock();
                if (linked) {
                    for (int d = 0; d < 2; ++d) {
                        dynamic_cast<KnobI*>( tk.get() )->slaveTo(d, src, d);
                    }
                } else {
                    for (std::size_t f = 0; f < frames.size(); ++f) {
                        tk->setValueAtTime(frames[f], src->getValueAtTime(frames[f], 0), ViewSpec::all(), 0);
                        tk->setValueAtTime(frames[f], src->getValueAtTime(frames[f], 1), ViewSpec::all(), 1);
                    }
                }
            }
        }

        return;
    }

    // Transform (match-move / stabilize = invert), relative to the reference frame
    KnobDoublePtr t = std::dynamic_pointer_cast<KnobDouble>( created->getKnobByName("translate") );
    KnobDoublePtr r = std::dynamic_pointer_cast<KnobDouble>( created->getKnobByName("rotate") );
    KnobDoublePtr s = std::dynamic_pointer_cast<KnobDouble>( created->getKnobByName("scale") );
    KnobDoublePtr c = std::dynamic_pointer_cast<KnobDouble>( created->getKnobByName("center") );
    KnobBoolPtr inv = std::dynamic_pointer_cast<KnobBool>( created->getKnobByName("invert") );
    KnobBoolPtr uni = std::dynamic_pointer_cast<KnobBool>( created->getKnobByName("uniform") );
    if (c) {
        c->setValue(_k->xfCenter.lock()->getValue(0), ViewSpec::all(), 0);
        c->setValue(_k->xfCenter.lock()->getValue(1), ViewSpec::all(), 1);
    }
    if (inv) {
        inv->setValue(stabilize);
    }
    if (uni) {
        uni->setValue(false);
    }
    struct Map { KnobDoublePtr dst; KnobDoublePtr src; int dims; bool skip; };
    Map maps[3] = {
        { t, _k->xfTranslate.lock(), 2, false },
        { r, _k->xfRotate.lock(), 1, onePoint },
        { s, _k->xfScale.lock(), 2, onePoint },
    };
    for (int m = 0; m < 3; ++m) {
        if (!maps[m].dst || maps[m].skip) {
            continue;
        }
        for (int d = 0; d < maps[m].dims; ++d) {
            if (linked) {
                dynamic_cast<KnobI*>( maps[m].dst.get() )->slaveTo(d, maps[m].src, d);
            } else {
                for (std::size_t f = 0; f < frames.size(); ++f) {
                    maps[m].dst->setValueAtTime(frames[f], maps[m].src->getValueAtTime(frames[f], d), ViewSpec::all(), d);
                }
            }
        }
    }
} // exportNode

// --------------------------------------------------------------------------------------------
// knobs / overlay

void
SolvRotoTrack::redraw()
{
    _effect->redrawOverlayInteract();
}

bool
SolvRotoTrack::knobChanged(KnobI* k)
{
    if (!k) {
        return false;
    }
    if ( k == _k->trackFw.lock().get() ) {
        startTracking(1, false);
    } else if ( k == _k->trackBw.lock().get() ) {
        startTracking(-1, false);
    } else if ( k == _k->trackNext.lock().get() ) {
        startTracking(1, true);
    } else if ( k == _k->trackPrev.lock().get() ) {
        startTracking(-1, true);
    } else if ( k == _k->stop.lock().get() ) {
        stopTracking();
    } else if ( k == _k->alignSurface.lock().get() ) {
        Quad qr;
        if ( getRefQuad(&qr) ) {
            setSurfaceRef(qr);
            refreshDerived();
            redraw();
        }
    } else if ( k == _k->bake.lock().get() ) {
        bakeToSelectedShapes();
    } else if ( k == _k->clearAll.lock().get() ) {
        clearTrack(0);
    } else if ( k == _k->clearBw.lock().get() ) {
        clearTrack(-1);
    } else if ( k == _k->clearFw.lock().get() ) {
        clearTrack(1);
    } else if ( k == _k->exportCreate.lock().get() ) {
        exportNode( _k->exportMenu.lock()->getValue() );
    } else if ( (k == _k->showSurface.lock().get()) || (k == _k->showGrid.lock().get()) || (k == _k->gridSize.lock().get()) ) {
        redraw();
    } else {
        return false;
    }

    return true;
}

void
SolvRotoTrack::drawOverlay(double time,
                           double pixelScaleX,
                           double /*pixelScaleY*/)
{
    Quad qt;
    if ( !getQuadAtTime(time, &qt) || !_k->showSurface.lock()->getValue() ) {
        return;
    }
    const Quad s = surfaceAtTime(time);
    GLProtectAttrib a(GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_POINT_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);

    // grid (bilinear in the quad's projective space)
    if ( _k->showGrid.lock()->getValue() ) {
        const int n = std::max(1, _k->gridSize.lock()->getValue());
        Quad unit;
        unit.x[0] = 0; unit.y[0] = 0; unit.x[1] = 1; unit.y[1] = 0; unit.x[2] = 1; unit.y[2] = 1; unit.x[3] = 0; unit.y[3] = 1;
        Transform::Matrix3x3 H;
        if ( homography(unit, s, &H) ) {
            glLineWidth(1.);
            glColor4f(0.24f, 0.55f, 1.f, 0.45f);
            glBegin(GL_LINES);
            for (int i = 1; i < n; ++i) {
                const double u = (double)i / n;
                double ax = u, ay = 0, bx = u, by = 1;
                applyH(H, &ax, &ay); applyH(H, &bx, &by);
                glVertex2d(ax, ay); glVertex2d(bx, by);
                double cx = 0, cy = u, dx = 1, dy = u;
                applyH(H, &cx, &cy); applyH(H, &dx, &dy);
                glVertex2d(cx, cy); glVertex2d(dx, dy);
            }
            glEnd();
        }
    }
    // outline
    glLineWidth(1.8f);
    glColor4f(0.24f, 0.55f, 1.f, 1.f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 4; ++i) {
        glVertex2d(s.x[i], s.y[i]);
    }
    glEnd();
    // center cross
    {
        double cx = 0, cy = 0;
        for (int i = 0; i < 4; ++i) {
            cx += s.x[i] / 4.; cy += s.y[i] / 4.;
        }
        const double l = 7. * pixelScaleX;
        glBegin(GL_LINES);
        glVertex2d(cx - l, cy); glVertex2d(cx + l, cy);
        glVertex2d(cx, cy - l); glVertex2d(cx, cy + l);
        glEnd();
    }
    // corners
    glPointSize(9.f);
    glColor4f(0.74f, 0.84f, 1.f, 1.f);
    glBegin(GL_POINTS);
    for (int i = 0; i < 4; ++i) {
        glVertex2d(s.x[i], s.y[i]);
    }
    glEnd();
    glPointSize(6.f);
    glColor4f(0.24f, 0.55f, 1.f, 1.f);
    glBegin(GL_POINTS);
    for (int i = 0; i < 4; ++i) {
        if (i == _dragCorner) {
            glColor4f(1.f, 0.82f, 0.34f, 1.f);
        } else {
            glColor4f(0.24f, 0.55f, 1.f, 1.f);
        }
        glVertex2d(s.x[i], s.y[i]);
    }
    glEnd();
} // drawOverlay

bool
SolvRotoTrack::penDown(double time,
                       double x,
                       double y,
                       double pixelScaleX)
{
    Quad qt;
    if ( _running || !getQuadAtTime(time, &qt) || !_k->showSurface.lock()->getValue() ) {
        return false;
    }
    const Quad s = surfaceAtTime(time);
    const double tol = 8. * pixelScaleX;
    for (int i = 0; i < 4; ++i) {
        if ( (std::fabs(s.x[i] - x) < tol) && (std::fabs(s.y[i] - y) < tol) ) {
            _dragCorner = i;
            redraw();

            return true;
        }
    }

    return false;
}

bool
SolvRotoTrack::penMotion(double time,
                         double x,
                         double y)
{
    if (_dragCorner < 0) {
        return false;
    }
    // move the corner at this frame = move it in reference space through the inverse motion
    Quad qr, qt;
    Transform::Matrix3x3 H;
    double rx = x, ry = y;
    if ( getRefQuad(&qr) && getQuadAtTime(time, &qt) && homography(qt, qr, &H) ) {
        applyH(H, &rx, &ry);
    }
    Quad sr;
    getSurfaceRef(&sr);
    sr.x[_dragCorner] = rx;
    sr.y[_dragCorner] = ry;
    setSurfaceRef(sr);
    redraw();

    return true;
}

bool
SolvRotoTrack::penUp()
{
    if (_dragCorner < 0) {
        return false;
    }
    _dragCorner = -1;
    refreshDerived();
    redraw();

    return true;
}

NATRON_NAMESPACE_EXIT
