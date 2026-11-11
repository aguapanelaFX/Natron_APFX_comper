/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
#include <Python.h>

#include "Solv3DNodes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>

#include "Global/GLIncludes.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NoOpBase.h"
#include "Engine/OverlaySupport.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/Solv3D/SolvAlembic.h"
#include "Engine/Solv3D/SolvScanline.h"
#include "Engine/Solv3D/SolvVanishingCamera.h"

NATRON_NAMESPACE_ENTER

using namespace Solv3D;

// ============================================================================ helpers
static KnobDoublePtr
makeDouble(EffectInstance* fx, const KnobPagePtr& page, const char* name, const char* label, int dims,
           double def, double dmin, double dmax, const char* hint = "")
{
    KnobDoublePtr k = AppManager::createKnob<KnobDouble>( fx, std::string(label), dims );
    k->setName(name);
    for (int d = 0; d < dims; ++d) {
        k->setDefaultValue(def, d);
        k->setDisplayMinimum(dmin, d);
        k->setDisplayMaximum(dmax, d);
    }
    if (hint && *hint) {
        k->setHintToolTip( std::string(hint) );
    }
    page->addKnob(k);

    return k;
}

static KnobPagePtr
makePage(EffectInstance* fx, const char* label)
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( fx, std::string(label) );

    return page;
}

static Vec3
getV3(const KnobDoubleWPtr& w, double t)
{
    KnobDoublePtr k = w.lock();
    if (!k) {
        return Vec3();
    }

    return Vec3( k->getValueAtTime(t, 0), k->getValueAtTime(t, 1), k->getValueAtTime(t, 2) );
}

void
Solv3DXformKnobs::create(EffectInstance* fx, const KnobPagePtr& page)
{
    translate = makeDouble(fx, page, "translate", "translate", 3, 0., -10., 10.);
    rotate = makeDouble(fx, page, "rotate", "rotate", 3, 0., -180., 180., "Degrees, applied X then Y then Z.");
    scale = makeDouble(fx, page, "scaling", "scale", 3, 1., 0., 4.);
    uniform = makeDouble(fx, page, "uniform_scale", "uniform scale", 1, 1., 0., 4.);
    pivot = makeDouble(fx, page, "pivot", "pivot", 3, 0., -10., 10.);
}

Mat4
Solv3DXformKnobs::matrix(double t) const
{
    const double u = uniform.lock() ? uniform.lock()->getValueAtTime(t) : 1.;

    return Mat4::trs( getV3(translate, t), getV3(rotate, t), getV3(scale, t) * u, getV3(pivot, t) );
}

// ============================================================================ Solv3DNode
Solv3DNode::Solv3DNode(NodePtr n)
    : EffectInstance(n)
{
}

void
Solv3DNode::getPluginGrouping(std::list<std::string>* grouping) const
{
    grouping->push_back(PLUGIN_GROUP_3D);
}

void
Solv3DNode::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
Solv3DNode::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Solv3DNode::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Solv3DNode::projectFormat(RectD* rod) const
{
    Format f;
    getApp()->getProject()->getProjectDefaultFormat(&f);
    *rod = f.toCanonicalFormat();
}

StatusEnum
Solv3DNode::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale & /*scale*/, ViewIdx /*view*/, RectD* rod)
{
    projectFormat(rod);

    return eStatusOK;
}

void
Solv3DNode::getRegionsOfInterest(double, const RenderScale &, const RectD &, const RectD &, ViewIdx, RoIMap* ret)
{
    ret->clear(); // 3D inputs are read as scenes, never as images
}

StatusEnum
Solv3DNode::render(const RenderActionArgs& args)
{
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin(); it != args.outputPlanes.end(); ++it) {
        it->second->fillZero(args.roi);
    }

    return eStatusOK;
}

std::string
Solv3DNode::primPath() const
{
    return "/" + makeValidName( getNode()->getScriptName_mt_safe() );
}

Solv3DNode*
Solv3DNode::as3D(const EffectInstancePtr& e)
{
    return dynamic_cast<Solv3DNode*>( e.get() );
}

bool
Solv3DNode::inputScene(EffectInstance* fx, int inputNb, double time, Scene* scene)
{
    EffectInstancePtr in = fx->getInput(inputNb);
    // follow Dots / Stamps / Anchors (pass-through nodes)
    for (int guard = 0; in && !as3D(in) && guard < 64; ++guard) {
        if ( !dynamic_cast<NoOpBase*>( in.get() ) ) {
            return false;
        }
        in = in->getInput(0);
    }
    Solv3DNode* n = in ? as3D(in) : 0;
    if (!n) {
        return false;
    }
    n->buildScene(time, scene);

    return true;
}

static std::shared_ptr<void>
texRef(const EffectInstancePtr& e)
{
    if (!e) {
        return std::shared_ptr<void>();
    }
    std::shared_ptr<Solv3DTexRef> r = std::make_shared<Solv3DTexRef>();
    r->effect = e;

    return r;
}

// ============================================================================ primitives
std::string
Solv3DPrimitive::getPluginID() const
{
    switch (_kind) {
    case eCube: return PLUGINID_SOLV3D_CUBE;
    case eSphere: return PLUGINID_SOLV3D_SPHERE;
    case eCylinder: return PLUGINID_SOLV3D_CYLINDER;
    default: return PLUGINID_SOLV3D_CARD;
    }
}

std::string
Solv3DPrimitive::getPluginLabel() const
{
    switch (_kind) {
    case eCube: return "Cube";
    case eSphere: return "Sphere";
    case eCylinder: return "Cylinder";
    default: return "Card";
    }
}

std::string
Solv3DPrimitive::getPluginDescription() const
{
    switch (_kind) {
    case eCube: return "A cube (size 1). The img input is used as its texture on every face.";
    case eSphere: return "A sphere (diameter 1). The img input is wrapped around it (lat-long).";
    case eCylinder: return "A cylinder (diameter 1, height 1). The img input is wrapped around it.";
    default: return "A flat card. The img input is its texture; with 'image aspect' the card takes the image proportions (width 1).";
    }
}

void
Solv3DPrimitive::initializeKnobs()
{
    KnobPagePtr page = makePage( this, getPluginLabel().c_str() );
    if (_kind == eCard) {
        KnobBoolPtr ia = AppManager::createKnob<KnobBool>( this, std::string("image aspect") );
        ia->setName("image_aspect");
        ia->setDefaultValue(true);
        ia->setHintToolTip( std::string("Card height follows the img input proportions (width 1). Off: uses the size.") );
        page->addKnob(ia);
        _imageAspect = ia;
    }
    _size = makeDouble(this, page, "size", "size", 1, 1., 0., 10., "Overall size of the shape.");
    if ( (_kind == eCard) || (_kind == eSphere) || (_kind == eCylinder) ) {
        KnobIntPtr r = AppManager::createKnob<KnobInt>( this, std::string("rows") );
        r->setName("rows");
        r->setDefaultValue(_kind == eCard ? 1 : 24);
        r->setDisplayMinimum(1);
        r->setDisplayMaximum(64);
        r->setAddNewLine(false);
        page->addKnob(r);
        _rows = r;
        KnobIntPtr c = AppManager::createKnob<KnobInt>( this, std::string("columns") );
        c->setName("columns");
        c->setDefaultValue(_kind == eCard ? 1 : 48);
        c->setDisplayMinimum(1);
        c->setDisplayMaximum(128);
        page->addKnob(c);
        _cols = c;
    }
    KnobColorPtr col = AppManager::createKnob<KnobColor>( this, std::string("color"), 3 );
    col->setName("color");
    for (int d = 0; d < 3; ++d) {
        col->setDefaultValue(1., d);
    }
    col->setHintToolTip( std::string("Multiplies the texture (the colour of the shape without texture).") );
    page->addKnob(col);
    _color = col;
    KnobBoolPtr lit = AppManager::createKnob<KnobBool>( this, std::string("lit") );
    lit->setName("lit");
    lit->setDefaultValue(_kind != eCard);
    lit->setHintToolTip( std::string("Off: the texture is shown as is (like a card). On: simple shading from the camera.") );
    page->addKnob(lit);
    _lit = lit;
    _xf.create(this, page);
}

void
Solv3DPrimitive::buildScene(double t, Scene* scene)
{
    const double size = _size.lock()->getValueAtTime(t);
    const int rows = _rows.lock() ? std::max( 1, _rows.lock()->getValueAtTime(t) ) : 1;
    const int cols = _cols.lock() ? std::max( 1, _cols.lock()->getValueAtTime(t) ) : 1;
    EffectInstancePtr img = getInput(0);
    std::shared_ptr<Mesh> m;
    switch (_kind) {
    case eCube: m = makeCube(size); break;
    case eSphere: m = makeSphere(size * 0.5, rows, cols); break;
    case eCylinder: m = makeCylinder(size * 0.5, size, cols, true); break;
    default: {
        double h = size;
        if ( img && _imageAspect.lock()->getValueAtTime(t) ) {
            RectD rod;
            bool isProjectFormat = false;
            if ( (img->getRegionOfDefinition_public(img->getRenderHash(), t, RenderScale(), ViewIdx(0), &rod, &isProjectFormat) != eStatusFailed) &&
                 (rod.width() > 0) ) {
                h = size * rod.height() / rod.width();
            }
        }
        m = makeCard(size, h, rows, cols);
        break;
    }
    }
    m->texture = texRef(img);
    KnobColorPtr c = _color.lock();
    m->material.baseColor = Vec3( c->getValueAtTime(t, 0), c->getValueAtTime(t, 1), c->getValueAtTime(t, 2) );
    m->material.emission = _lit.lock()->getValueAtTime(t) ? 0. : 1.;
    Prim& p = scene->define(primPath(), PrimType::Mesh);
    p.mesh = m;
    p.local = _xf.matrix(t);
}

// ============================================================================ Camera
std::string
Solv3DCamera::getPluginDescription() const
{
    return "3D camera (focal length and film back in mm). Connect a plate to bg to see it while placing the camera.\n"
           "Camera from vanishing lines: turn on 'edit lines', drag the X (red), Y (green) and Z (blue) line pairs onto "
           "parallel edges of the plate (each pair = two edges that are parallel in the real world, the three directions "
           "at right angles), then press 'solve': focal length and rotation come from the vanishing points, the camera "
           "is placed 'distance' away from the start of the first red line.";
}

void
Solv3DCamera::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "Camera");
    _xf.create(this, page);
    _focal = makeDouble(this, page, "focal", "focal length", 1, 35., 8., 200., "mm");
    _hAperture = makeDouble(this, page, "haperture", "horiz aperture", 1, 36., 5., 70., "Film back width (mm).");
    _vAperture = makeDouble(this, page, "vaperture", "vert aperture", 1, 24., 5., 70., "Film back height (mm).");
    _near = makeDouble(this, page, "near", "near", 1, 0.1, 0.001, 10.);
    _far = makeDouble(this, page, "far", "far", 1, 10000., 10., 100000.);

    KnobPagePtr vp = makePage(this, "Vanishing lines");
    KnobBoolPtr edit = AppManager::createKnob<KnobBool>( this, std::string("edit lines") );
    edit->setName("edit_lines");
    edit->setDefaultValue(false);
    edit->setEvaluateOnChange(false);
    edit->setAnimationEnabled(false);
    edit->setHintToolTip( std::string("Show and drag the vanishing lines in the viewer (view this Camera with the plate in bg).") );
    vp->addKnob(edit);
    _editLines = edit;
    KnobChoicePtr axes = AppManager::createKnob<KnobChoice>( this, std::string("axes") );
    axes->setName("axes");
    {
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("xz", "X and Z (focal from 2 directions, lens centre = image centre)", "") );
        o.push_back( ChoiceOption("xy", "X and Y", "") );
        o.push_back( ChoiceOption("xyz", "X, Y and Z (focal + lens centre)", "") );
        axes->populateChoices(o);
    }
    axes->setDefaultValue(0);
    axes->setAnimationEnabled(false);
    axes->setEvaluateOnChange(false);
    vp->addKnob(axes);
    _axes = axes;
    const char* names[6] = { "lineX1", "lineX2", "lineY1", "lineY2", "lineZ1", "lineZ2" };
    const char* labels[6] = { "X line 1", "X line 2", "Y line 1", "Y line 2", "Z line 1", "Z line 2" };
    for (int i = 0; i < 6; ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, std::string(labels[i]), 4 );
        k->setName(names[i]);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setSecretByDefault(true);
        vp->addKnob(k);
        _lines[i] = k;
    }
    _distance = makeDouble(this, vp, "distance", "distance", 1, 5., 0.1, 100., "Distance from the camera to the start of the first red line (sets the scene scale).");
    _distance.lock()->setEvaluateOnChange(false);
    KnobButtonPtr solve = AppManager::createKnob<KnobButton>( this, std::string("solve") );
    solve->setName("solve");
    solve->setHintToolTip( std::string("Compute focal length, rotation and position from the lines.") );
    solve->setAddNewLine(false);
    vp->addKnob(solve);
    _solve = solve;
    KnobButtonPtr reset = AppManager::createKnob<KnobButton>( this, std::string("reset lines") );
    reset->setName("reset_lines");
    vp->addKnob(reset);
    _reset = reset;
    addOverlaySlaveParam(edit);
    for (int i = 0; i < 6; ++i) {
        addOverlaySlaveParam( _lines[i].lock() );
    }
}

void
Solv3DCamera::resetLines()
{
    RectD f;
    projectFormat(&f);
    const double w = f.width(), h = f.height(), x0 = f.x1, y0 = f.y1;
    // starting positions: X lines converge to the right, Z to the left, Y vertical
    const double L[6][4] = {
        { 0.25, 0.30, 0.45, 0.33 }, { 0.25, 0.60, 0.45, 0.55 },
        { 0.70, 0.25, 0.70, 0.70 }, { 0.82, 0.25, 0.82, 0.70 },
        { 0.75, 0.33, 0.55, 0.30 }, { 0.75, 0.55, 0.55, 0.60 }
    };
    for (int i = 0; i < 6; ++i) {
        KnobDoublePtr k = _lines[i].lock();
        k->setValue(x0 + L[i][0] * w, ViewSpec::all(), 0);
        k->setValue(y0 + L[i][1] * h, ViewSpec::all(), 1);
        k->setValue(x0 + L[i][2] * w, ViewSpec::all(), 2);
        k->setValue(y0 + L[i][3] * h, ViewSpec::all(), 3);
    }
}

bool
Solv3DCamera::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/, bool /*mainThread*/)
{
    if ( k == _solve.lock().get() ) {
        solveFromLines();

        return true;
    }
    if ( k == _reset.lock().get() ) {
        resetLines();

        return true;
    }
    if ( k == _editLines.lock().get() ) {
        // first use: put the lines somewhere sensible
        KnobDoublePtr l0 = _lines[0].lock();
        if ( _editLines.lock()->getValue() && (l0->getValue(0) == 0.) && (l0->getValue(2) == 0.) ) {
            resetLines();
        }

        return true;
    }

    return false;
}

void
Solv3DCamera::solveFromLines()
{
    RectD f;
    projectFormat(&f);
    const int mode = _axes.lock()->getValue();
    const int axisIdx[3] = { 0, mode == 1 ? 1 : 2, 1 }; // which line pairs: X, then Z (or Y), then Y
    const int n = (mode == 2) ? 3 : 2;
    SolvVP::V2 vps[3];
    SolvVP::Line dirs[3];
    for (int i = 0; i < n; ++i) {
        const int a = (i == 2) ? 1 : axisIdx[i];
        SolvVP::Line l[2];
        for (int j = 0; j < 2; ++j) {
            KnobDoublePtr k = _lines[a * 2 + j].lock();
            l[j].a = SolvVP::V2{ k->getValue(0), k->getValue(1) };
            l[j].b = SolvVP::V2{ k->getValue(2), k->getValue(3) };
        }
        if ( !SolvVP::vanishingPoint(l[0], l[1], &vps[i]) ) {
            setPersistentMessage(eMessageTypeError, "The two lines of an axis are parallel in the image: tilt them so they meet.");

            return;
        }
        dirs[i] = l[0];
    }
    const SolvVP::V2 centre{ (f.x1 + f.x2) * 0.5, (f.y1 + f.y2) * 0.5 };
    SolvVP::Result r = SolvVP::solve(vps, n, centre, dirs);
    if (!r.ok) {
        setPersistentMessage(eMessageTypeError, std::string("Camera from lines: ") + r.error);

        return;
    }
    clearPersistentMessage(false);
    // solver frame: x right, y up (image y up), z into the scene -> GL camera frame (looks down -Z)
    Vec3 cAx[3];
    for (int i = 0; i < 2; ++i) {
        cAx[i] = Vec3(r.R[0][i], r.R[1][i], -r.R[2][i]);
    }
    // the two solved directions are world X and (Z or Y)
    Vec3 wx = cAx[0], w2 = cAx[1], wy, wz;
    if (mode == 1) {
        wy = w2;
        wz = normalize( cross(wx, wy) );
    } else {
        wz = w2;
        wy = normalize( cross(wz, wx) );
    }
    // camera <- world rotation has the world axes (in camera space) as columns; camera world rotation = transpose
    Mat4 camFromWorld;
    const Vec3 cols[3] = { wx, wy, wz };
    for (int c = 0; c < 3; ++c) {
        camFromWorld.m[0][c] = cols[c].x;
        camFromWorld.m[1][c] = cols[c].y;
        camFromWorld.m[2][c] = cols[c].z;
    }
    const Mat4 worldFromCam = camFromWorld.transposed();
    // position: world origin on the start of the first X line, 'distance' away
    KnobDoublePtr l0 = _lines[0].lock();
    const Vec3 d = normalize( Vec3( (l0->getValue(0) - r.principal.x) / r.f, (l0->getValue(1) - r.principal.y) / r.f, -1. ) );
    const double dist = _distance.lock()->getValue();
    const Vec3 originCam = d * dist;                       // world origin in camera space
    const Vec3 camPos = worldFromCam.vector(originCam) * -1.;
    // euler XYZ (R = Rz * Ry * Rx)
    const double ry = -std::asin( std::max( -1., std::min(1., worldFromCam.m[2][0]) ) );
    const double rx = std::atan2(worldFromCam.m[2][1], worldFromCam.m[2][2]);
    const double rz = std::atan2(worldFromCam.m[1][0], worldFromCam.m[0][0]);
    const double toDeg = 180. / 3.14159265358979323846;
    beginChanges();
    KnobDoublePtr t = _xf.translate.lock(), ro = _xf.rotate.lock();
    t->setValue(camPos.x, ViewSpec::all(), 0); t->setValue(camPos.y, ViewSpec::all(), 1); t->setValue(camPos.z, ViewSpec::all(), 2);
    ro->setValue(rx * toDeg, ViewSpec::all(), 0); ro->setValue(ry * toDeg, ViewSpec::all(), 1); ro->setValue(rz * toDeg, ViewSpec::all(), 2);
    _focal.lock()->setValue( SolvVP::focalMM( r.f, _hAperture.lock()->getValue(), f.width() ) );
    endChanges();
}

void
Solv3DCamera::buildScene(double t, Scene* scene)
{
    std::shared_ptr<Camera> c = std::make_shared<Camera>();
    c->focalLength = _focal.lock()->getValueAtTime(t);
    c->horizontalAperture = _hAperture.lock()->getValueAtTime(t);
    c->verticalAperture = _vAperture.lock()->getValueAtTime(t);
    c->nearClip = _near.lock()->getValueAtTime(t);
    c->farClip = _far.lock()->getValueAtTime(t);
    Prim& p = scene->define(primPath(), PrimType::Camera);
    p.camera = c;
    p.local = _xf.matrix(t);
}

StatusEnum
Solv3DCamera::getRegionOfDefinition(U64 hash, double time, const RenderScale & scale, ViewIdx view, RectD* rod)
{
    EffectInstancePtr bg = getInput(0);
    if (bg) {
        bool isProjectFormat = false;

        return bg->getRegionOfDefinition_public(bg->getRenderHash(), time, scale, view, rod, &isProjectFormat);
    }

    return Solv3DNode::getRegionOfDefinition(hash, time, scale, view, rod);
}

void
Solv3DCamera::getRegionsOfInterest(double, const RenderScale &, const RectD &, const RectD & renderWindow, ViewIdx, RoIMap* ret)
{
    ret->clear();
    EffectInstancePtr bg = getInput(0);
    if (bg) {
        (*ret)[bg] = renderWindow;
    }
}

StatusEnum
Solv3DCamera::render(const RenderActionArgs& args)
{
    EffectInstancePtr bg = getInput(0);
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin(); it != args.outputPlanes.end(); ++it) {
        it->second->fillZero(args.roi);
        if (!bg) {
            continue;
        }
        RectI roiPixel;
        ImagePtr src = getImage(0, args.time, args.originalScale, args.view, NULL, &it->first, false, true, eStorageModeRAM, 0, &roiPixel);
        if (src) {
            it->second->pasteFrom( *src, args.roi, false );
        }
    }

    return eStatusOK;
}

static const float kAxisCol[3][3] = { { 0.93f, 0.33f, 0.31f }, { 0.4f, 0.85f, 0.42f }, { 0.36f, 0.55f, 1.f } };

void
Solv3DCamera::drawOverlay(double /*time*/, const RenderScale & /*renderScale*/, ViewIdx /*view*/)
{
    if ( !_editLines.lock()->getValue() ) {
        return;
    }
    double psx = 1., psy = 1.;
    OverlaySupport* ov = getCurrentViewportForOverlays();
    if (ov) {
        ov->getPixelScale(psx, psy);
    }
    GLProtectAttrib a(GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_POINT_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);
    for (int i = 0; i < 6; ++i) {
        KnobDoublePtr k = _lines[i].lock();
        const float* c = kAxisCol[i / 2];
        const double x1 = k->getValue(0), y1 = k->getValue(1), x2 = k->getValue(2), y2 = k->getValue(3);
        // extended guide (dim) so the convergence is visible
        const double dx = x2 - x1, dy = y2 - y1;
        glLineWidth(1.f);
        glColor4f(c[0], c[1], c[2], 0.35f);
        glBegin(GL_LINES);
        glVertex2d(x1 - dx * 3., y1 - dy * 3.);
        glVertex2d(x2 + dx * 3., y2 + dy * 3.);
        glEnd();
        glLineWidth(2.f);
        glColor4f(c[0], c[1], c[2], 1.f);
        glBegin(GL_LINES);
        glVertex2d(x1, y1);
        glVertex2d(x2, y2);
        glEnd();
        glPointSize(8.f);
        glBegin(GL_POINTS);
        for (int e = 0; e < 2; ++e) {
            if (_drag == i * 2 + e) {
                glColor4f(1.f, 0.82f, 0.34f, 1.f);
            } else {
                glColor4f(c[0], c[1], c[2], 1.f);
            }
            glVertex2d(e ? x2 : x1, e ? y2 : y1);
        }
        glEnd();
    }
    (void)psx; (void)psy;
}

bool
Solv3DCamera::onOverlayPenDown(double, const RenderScale &, ViewIdx, const QPointF &, const QPointF & pos, double, double, PenType)
{
    if ( !_editLines.lock()->getValue() ) {
        return false;
    }
    double psx = 1., psy = 1.;
    OverlaySupport* ov = getCurrentViewportForOverlays();
    if (ov) {
        ov->getPixelScale(psx, psy);
    }
    const double tol = 10. * psx;
    double best = tol;
    _drag = -1;
    for (int i = 0; i < 6; ++i) {
        KnobDoublePtr k = _lines[i].lock();
        for (int e = 0; e < 2; ++e) {
            const double d = std::hypot( pos.x() - k->getValue(e * 2), pos.y() - k->getValue(e * 2 + 1) );
            if (d < best) {
                best = d;
                _drag = i * 2 + e;
            }
        }
    }

    return _drag >= 0;
}

bool
Solv3DCamera::onOverlayPenMotion(double, const RenderScale &, ViewIdx, const QPointF &, const QPointF & pos, double, double)
{
    if (_drag < 0) {
        return false;
    }
    KnobDoublePtr k = _lines[_drag / 2].lock();
    const int e = _drag % 2;
    k->setValue(pos.x(), ViewSpec::all(), e * 2);
    k->setValue(pos.y(), ViewSpec::all(), e * 2 + 1);

    return true;
}

bool
Solv3DCamera::onOverlayPenUp(double, const RenderScale &, ViewIdx, const QPointF &, const QPointF &, double, double)
{
    if (_drag < 0) {
        return false;
    }
    _drag = -1;

    return true;
}

// ============================================================================ TransformGeo / Scene
void
Solv3DTransformGeo::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "TransformGeo");
    _xf.create(this, page);
}

void
Solv3DTransformGeo::buildScene(double t, Scene* scene)
{
    Scene in;
    inputScene(this, 0, t, &in);
    // one transform prim above the input (keeps the input paths, like a parent group)
    Scene r = in.reparented( primPath() );
    Prim* root = r.find( primPath() );
    if (root) {
        root->local = _xf.matrix(t);
    }
    scene->merge(r);
}

void
Solv3DSceneNode::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "Scene");
    KnobStringPtr info = AppManager::createKnob<KnobString>( this, std::string("info") );
    info->setName("info");
    info->setAsLabel();
    info->setDefaultValue("Connect cards, geometry, cameras and lights; later inputs win on the same paths.");
    page->addKnob(info);
}

void
Solv3DSceneNode::buildScene(double t, Scene* scene)
{
    for (int i = 0; i < getNInputs(); ++i) {
        Scene s;
        if ( inputScene(this, i, t, &s) ) {
            scene->merge(s);
        }
    }
}

// ============================================================================ ReadGeo
struct Solv3DReadGeo::Cache
{
    std::mutex m;
    std::string file;
    double time = -1e30;
    Scene scene;
    std::string error;
};

Solv3DReadGeo::Solv3DReadGeo(NodePtr n)
    : Solv3DNode(n)
    , _cache( std::make_shared<Cache>() )
{
}

std::string
Solv3DReadGeo::getPluginDescription() const
{
    return std::string("Reads geometry: OBJ (meshes with uv / normals) and ") +
           ( alembicAvailable() ? "Alembic (.abc: animated meshes, xforms and cameras, read at the current frame)." : "Alembic (not in this build)." ) +
           " The img input is used as the texture of all its meshes. USD import comes with the USD bridge.";
}

void
Solv3DReadGeo::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "ReadGeo");
    KnobFilePtr f = AppManager::createKnob<KnobFile>( this, std::string("file") );
    f->setName("file");
    f->setHintToolTip( std::string(".obj or .abc file") );
    f->setAnimationEnabled(false);
    page->addKnob(f);
    _file = f;
    KnobIntPtr off = AppManager::createKnob<KnobInt>( this, std::string("frame offset") );
    off->setName("frame_offset");
    off->setDefaultValue(0);
    off->disableSlider();
    page->addKnob(off);
    _frameOffset = off;
    KnobBoolPtr lit = AppManager::createKnob<KnobBool>( this, std::string("lit") );
    lit->setName("lit");
    lit->setDefaultValue(true);
    page->addKnob(lit);
    _lit = lit;
    KnobStringPtr info = AppManager::createKnob<KnobString>( this, std::string("status") );
    info->setName("status");
    info->setAsLabel();
    info->setIsPersistent(false);
    page->addKnob(info);
    _info = info;
    _xf.create(this, page);
}

bool
Solv3DReadGeo::knobChanged(KnobI* k, ValueChangedReasonEnum, ViewSpec, double, bool)
{
    if ( k == _file.lock().get() ) {
        std::lock_guard<std::mutex> l(_cache->m);
        _cache->file.clear();

        return true;
    }

    return false;
}

void
Solv3DReadGeo::buildScene(double t, Scene* scene)
{
    const std::string file = _file.lock()->getValue();
    if ( file.empty() ) {
        return;
    }
    std::string lower = file;
    std::transform( lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); } );
    const bool isAbc = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".abc") == 0;
    const double ft = t + _frameOffset.lock()->getValueAtTime(t);
    Scene loaded;
    {
        std::lock_guard<std::mutex> l(_cache->m);
        if ( (_cache->file != file) || (isAbc && _cache->time != ft) ) {
            _cache->scene = Scene();
            _cache->error.clear();
            bool ok;
            if (isAbc) {
                const double fps = std::max( 1., getApp()->getProjectFrameRate() );
                ok = readAlembic(file, ft / fps, "/geo", &_cache->scene, &_cache->error);
            } else {
                ok = readObj(file, "/geo", &_cache->scene, &_cache->error);
            }
            if (!ok && _cache->error.empty()) {
                _cache->error = "cannot read " + file;
            }
            _cache->file = file;
            _cache->time = ft;
        }
        loaded = _cache->scene;
    }
    EffectInstancePtr img = getInput(0);
    const bool lit = _lit.lock()->getValueAtTime(t);
    for (auto& kv : loaded.prims()) {
        if (kv.second.mesh) {
            std::shared_ptr<Mesh> m = std::make_shared<Mesh>(*kv.second.mesh); // keep the cache intact
            m->material.emission = lit ? 0. : 1.;
            if (img) {
                m->texture = texRef(img);
            }
            kv.second.mesh = m;
        }
    }
    Scene r = loaded.reparented( primPath() );
    Prim* root = r.find( primPath() );
    if (root) {
        root->local = _xf.matrix(t);
    }
    scene->merge(r);
}

// ============================================================================ WriteGeo
std::string
Solv3DWriteGeo::getPluginDescription() const
{
    return "Writes the 3D input to a file at the current frame: .usda (USD text, opens in any USD application) or "
           ".abc (Alembic). Press 'write'.";
}

void
Solv3DWriteGeo::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "WriteGeo");
    KnobOutputFilePtr f = AppManager::createKnob<KnobOutputFile>( this, std::string("file") );
    f->setName("file");
    f->setHintToolTip( std::string(".usda or .abc") );
    f->setAnimationEnabled(false);
    page->addKnob(f);
    _file = f;
    KnobButtonPtr w = AppManager::createKnob<KnobButton>( this, std::string("write") );
    w->setName("write");
    page->addKnob(w);
    _write = w;
}

void
Solv3DWriteGeo::buildScene(double t, Scene* scene)
{
    inputScene(this, 0, t, scene);
}

bool
Solv3DWriteGeo::knobChanged(KnobI* k, ValueChangedReasonEnum, ViewSpec, double time, bool)
{
    if ( k != _write.lock().get() ) {
        return false;
    }
    std::string file = _file.lock()->getValue();
    if ( file.empty() ) {
        setPersistentMessage(eMessageTypeError, "Set the output file first (.usda or .abc).");

        return true;
    }
    Scene s;
    buildScene(time, &s);
    std::string lower = file;
    std::transform( lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); } );
    std::string err;
    bool ok;
    if ( (lower.size() > 4) && (lower.compare(lower.size() - 4, 4, ".abc") == 0) ) {
        ok = writeAlembic(file, s, &err);
    } else {
        std::ofstream o( file.c_str() );
        o << s.toUsda();
        ok = (bool)o;
        if (!ok) {
            err = "cannot write " + file;
        }
    }
    if (ok) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeError, err);
    }

    return true;
}

// ============================================================================ Project3D
std::string
Solv3DProject3D::getPluginDescription() const
{
    return "Projects the img input through the camera onto the geometry (camera projection). Several Project3D can be "
           "stacked: later ones go over earlier ones. 'frame hold' freezes the projection camera at one frame.";
}

void
Solv3DProject3D::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "Project3D");
    KnobBoolPtr crop = AppManager::createKnob<KnobBool>( this, std::string("crop") );
    crop->setName("crop");
    crop->setDefaultValue(true);
    crop->setHintToolTip( std::string("Outside the camera frame the projection is transparent.") );
    page->addKnob(crop);
    _crop = crop;
    KnobBoolPtr uh = AppManager::createKnob<KnobBool>( this, std::string("frame hold") );
    uh->setName("use_frame_hold");
    uh->setDefaultValue(false);
    uh->setAddNewLine(false);
    page->addKnob(uh);
    _useHold = uh;
    KnobIntPtr fh = AppManager::createKnob<KnobInt>( this, std::string("") );
    fh->setName("frame_hold");
    fh->setDefaultValue(1001);
    fh->disableSlider();
    page->addKnob(fh);
    _frameHold = fh;
}

void
Solv3DProject3D::buildScene(double t, Scene* scene)
{
    inputScene(this, 0, t, scene);
    EffectInstancePtr img = getInput(2);
    if (!img) {
        return;
    }
    const double ct = _useHold.lock()->getValueAtTime(t) ? (double)_frameHold.lock()->getValueAtTime(t) : t;
    Scene cs;
    if ( !inputScene(this, 1, ct, &cs) ) {
        return;
    }
    const std::string camPath = cs.firstCamera();
    const Prim* cp = cs.find(camPath);
    if ( !cp || !cp->camera ) {
        return;
    }
    RectD rod;
    bool isProjectFormat = false;
    double aspect = 16. / 9.;
    if ( (img->getRegionOfDefinition_public(img->getRenderHash(), t, RenderScale(), ViewIdx(0), &rod, &isProjectFormat) != eStatusFailed) &&
         (rod.height() > 0) ) {
        aspect = rod.width() / rod.height();
    }
    Projection pr;
    pr.viewProj = cp->camera->projection(aspect) * cs.worldMatrix(camPath).inverse();
    pr.texture = texRef(img);
    pr.crop = _crop.lock()->getValueAtTime(t);
    for (auto& kv : scene->prims()) {
        if (kv.second.mesh) {
            std::shared_ptr<Mesh> m = std::make_shared<Mesh>(*kv.second.mesh);
            m->projections.push_back(pr);
            kv.second.mesh = m;
        }
    }
}

// ============================================================================ Material
std::string
Solv3DMaterial::getPluginDescription() const
{
    return "Surface look of the geometry for ScanlineRender: base colour, emission (1 = texture shown as is), "
           "metalness, specular, roughness and IOR. Reflections come from the env input of ScanlineRender "
           "(any lat-long image); roughness blurs them.";
}

void
Solv3DMaterial::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "Material");
    KnobColorPtr b = AppManager::createKnob<KnobColor>( this, std::string("base color"), 3 );
    b->setName("base_color");
    for (int d = 0; d < 3; ++d) {
        b->setDefaultValue(1., d);
    }
    page->addKnob(b);
    _base = b;
    _emission = makeDouble(this, page, "emission", "emission", 1, 0., 0., 1., "1 = unlit (the texture as is), 0 = shaded.");
    _metal = makeDouble(this, page, "metalness", "metalness", 1, 0., 0., 1., "1 = metal: reflections tinted by the base colour.");
    _spec = makeDouble(this, page, "specular", "specular", 1, 0.5, 0., 1., "Reflection weight of non-metals (Fresnel from the IOR).");
    _rough = makeDouble(this, page, "roughness", "roughness", 1, 0.2, 0., 1., "0 = mirror, 1 = very blurry reflections.");
    _ior = makeDouble(this, page, "ior", "IOR", 1, 1.5, 1., 3., "Index of refraction (glass 1.5, water 1.33).");
}

void
Solv3DMaterial::buildScene(double t, Scene* scene)
{
    inputScene(this, 0, t, scene);
    KnobColorPtr b = _base.lock();
    Material mt;
    mt.baseColor = Vec3( b->getValueAtTime(t, 0), b->getValueAtTime(t, 1), b->getValueAtTime(t, 2) );
    mt.emission = _emission.lock()->getValueAtTime(t);
    mt.metalness = _metal.lock()->getValueAtTime(t);
    mt.specular = _spec.lock()->getValueAtTime(t);
    mt.roughness = _rough.lock()->getValueAtTime(t);
    mt.ior = _ior.lock()->getValueAtTime(t);
    for (auto& kv : scene->prims()) {
        if (kv.second.mesh) {
            std::shared_ptr<Mesh> m = std::make_shared<Mesh>(*kv.second.mesh);
            m->material = mt;
            kv.second.mesh = m;
        }
    }
}

// ============================================================================ ScanlineRender
std::string
Solv3DScanline::getPluginDescription() const
{
    return "Renders the 3D scene (obj/scn) through the camera (cam input, or the first camera of the scene) over bg "
           "(which also sets the format). env: a lat-long image used for reflections (Material metalness / specular). "
           "Anti-aliasing: samples per pixel and a reconstruction filter (Blackman-Harris and Mitchell give the "
           "cleanest edges). Output: rgba or a data pass (depth, world position, world normal, uv).";
}

std::string
Solv3DScanline::getInputLabel(int i) const
{
    switch (i) {
    case 0: return "obj/scn";
    case 1: return "cam";
    case 2: return "bg";
    default: return "env";
    }
}

void
Solv3DScanline::getPluginGrouping(std::list<std::string>* grouping) const
{
    grouping->push_back(PLUGIN_GROUP_3D);
}

void
Solv3DScanline::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "ScanlineRender");
    KnobChoicePtr out = AppManager::createKnob<KnobChoice>( this, std::string("output") );
    out->setName("output");
    {
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("rgba", "rgba", "") );
        o.push_back( ChoiceOption("depth", "depth (distance to camera)", "") );
        o.push_back( ChoiceOption("position", "world position", "") );
        o.push_back( ChoiceOption("normal", "world normal", "") );
        o.push_back( ChoiceOption("uv", "uv", "") );
        out->populateChoices(o);
    }
    out->setDefaultValue(0);
    page->addKnob(out);
    _output = out;
    KnobChoicePtr smp = AppManager::createKnob<KnobChoice>( this, std::string("samples") );
    smp->setName("samples");
    {
        std::vector<ChoiceOption> o;
        const char* n[8] = { "1", "4", "9", "16", "25", "36", "49", "64" };
        for (int i = 0; i < 8; ++i) {
            o.push_back( ChoiceOption(n[i], n[i], "") );
        }
        smp->populateChoices(o);
    }
    smp->setDefaultValue(3);
    smp->setHintToolTip( std::string("Samples per pixel (anti-aliasing). 16 is a good default, 64 for final hard edges.") );
    page->addKnob(smp);
    _samples = smp;
    KnobChoicePtr flt = AppManager::createKnob<KnobChoice>( this, std::string("filter") );
    flt->setName("filter");
    {
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("box", "box", "Sharpest, can alias.") );
        o.push_back( ChoiceOption("triangle", "triangle", "Soft.") );
        o.push_back( ChoiceOption("gaussian", "gaussian", "Softest, no ringing.") );
        o.push_back( ChoiceOption("mitchell", "Mitchell", "Sharp and clean (slight ringing on hard edges).") );
        o.push_back( ChoiceOption("blackmanharris", "Blackman-Harris", "Clean, smooth edges (default).") );
        flt->populateChoices(o);
    }
    flt->setDefaultValue(4);
    flt->setAddNewLine(false);
    page->addKnob(flt);
    _filter = flt;
    _filterWidth = makeDouble(this, page, "filter_width", "filter width", 1, 2., 1., 4., "Filter diameter in pixels.");
    KnobBoolPtr tf = AppManager::createKnob<KnobBool>( this, std::string("texture filtering") );
    tf->setName("texture_filtering");
    tf->setDefaultValue(true);
    tf->setHintToolTip( std::string("Mip-mapped textures: no shimmering when textures are small on screen.") );
    page->addKnob(tf);
    _texFilter = tf;
    _ambient = makeDouble(this, page, "ambient", "ambient", 1, 0.15, 0., 1., "Minimum light on lit surfaces facing away from the camera.");
}

void
Solv3DScanline::buildScene(double t, Scene* scene)
{
    inputScene(this, 0, t, scene);
    inputScene(this, 1, t, scene);
}

StatusEnum
Solv3DScanline::getRegionOfDefinition(U64 hash, double time, const RenderScale & scale, ViewIdx view, RectD* rod)
{
    EffectInstancePtr bg = getInput(2);
    if (bg) {
        bool isProjectFormat = false;

        return bg->getRegionOfDefinition_public(bg->getRenderHash(), time, scale, view, rod, &isProjectFormat);
    }

    return Solv3DNode::getRegionOfDefinition(hash, time, scale, view, rod);
}

void
Solv3DScanline::getRegionsOfInterest(double time, const RenderScale & scale, const RectD &, const RectD & renderWindow, ViewIdx view, RoIMap* ret)
{
    ret->clear();
    EffectInstancePtr bg = getInput(2);
    if (bg) {
        (*ret)[bg] = renderWindow;
    }
    EffectInstancePtr env = getInput(3);
    if (env) {
        RectD r;
        bool isProjectFormat = false;
        if (env->getRegionOfDefinition_public(env->getRenderHash(), time, scale, view, &r, &isProjectFormat) != eStatusFailed) {
            (*ret)[env] = r;
        }
    }
}

// copies a fetched image into a TexImage (float rgba)
static bool
toTexImage(const ImagePtr& img, const RectD& rodCanonical, double sx, double sy, TexImage* out)
{
    if ( !img || (img->getBitDepth() != eImageBitDepthFloat) ) {
        return false;
    }
    const RectI b = img->getBounds();
    const int nc = (int)img->getComponentsCount();
    out->x1 = b.x1;
    out->y1 = b.y1;
    out->w = std::max(0, b.x2 - b.x1);
    out->h = std::max(0, b.y2 - b.y1);
    out->data.assign( (size_t)out->w * out->h * 4, 0.f );
    Image::ReadAccess acc( img.get() );
    for (int y = 0; y < out->h; ++y) {
        const float* p = (const float*)acc.pixelAt(b.x1, b.y1 + y);
        if (!p) {
            continue;
        }
        float* d = &out->data[(size_t)y * out->w * 4];
        for (int x = 0; x < out->w; ++x, p += nc, d += 4) {
            if (nc >= 4) {
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3];
            } else if (nc == 3) {
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 1.f;
            } else {
                d[0] = d[1] = d[2] = d[3] = p[0];
            }
        }
    }
    out->fx1 = rodCanonical.x1 * sx;
    out->fy1 = rodCanonical.y1 * sy;
    out->fx2 = rodCanonical.x2 * sx;
    out->fy2 = rodCanonical.y2 * sy;

    return true;
}

StatusEnum
Solv3DScanline::render(const RenderActionArgs& args)
{
    const double t = args.time;
    const double sx = args.originalScale.toOfxPointD().x, sy = args.originalScale.toOfxPointD().y;
    Scene scene, camScene;
    inputScene(this, 0, t, &scene);
    std::string camPath;
    if ( inputScene(this, 1, t, &camScene) ) {
        camPath = camScene.firstCamera();
        // the cam input lives next to the scene (its own namespace to avoid clashes)
        Scene ns = camScene.reparented("/__cam");
        scene.merge(ns);
        camPath = camPath.empty() ? std::string() : "/__cam" + camPath;
    }
    if ( camPath.empty() ) {
        camPath = scene.firstCamera();
    }
    // textures: fetch every referenced upstream image once
    std::map<const void*, std::shared_ptr<TexImage> > texCache;
    auto fetchTex = [&](const std::shared_ptr<void>& h) {
        Solv3DTexRef* ref = static_cast<Solv3DTexRef*>( h.get() );
        if ( !ref || texCache.count( h.get() ) ) {
            return;
        }
        EffectInstancePtr e = ref->effect.lock();
        std::shared_ptr<TexImage> ti;
        if (e) {
            RectD rod;
            bool isProjectFormat = false;
            if (e->getRegionOfDefinition_public(e->getRenderHash(), t, args.originalScale, args.view, &rod, &isProjectFormat) != eStatusFailed) {
                const ImagePlaneDesc rgba = ImagePlaneDesc::getRGBAComponents();
                RectI roiPixel;
                EffectInstance::solvfxSetInputOverride(e);
                ImagePtr img = getImage(0, t, args.originalScale, args.view, &rod, &rgba, false, true, eStorageModeRAM, 0, &roiPixel);
                EffectInstance::solvfxSetInputOverride( EffectInstancePtr() );
                ti = std::make_shared<TexImage>();
                if ( toTexImage(img, rod, sx, sy, ti.get() ) ) {
                    if ( _texFilter.lock()->getValueAtTime(t) ) {
                        ti->buildMips();
                    }
                } else {
                    ti.reset();
                }
            }
        }
        texCache[h.get()] = ti;
    };
    for (const auto& kv : scene.prims()) {
        if (kv.second.mesh) {
            fetchTex(kv.second.mesh->texture);
            for (const Projection& pr : kv.second.mesh->projections) {
                fetchTex(pr.texture);
            }
        }
    }
    // environment
    std::shared_ptr<TexImage> envTex;
    if ( getInput(3) ) {
        RectI roiPixel;
        const ImagePlaneDesc rgba = ImagePlaneDesc::getRGBAComponents();
        ImagePtr env = getImage(3, t, args.originalScale, args.view, NULL, &rgba, false, true, eStorageModeRAM, 0, &roiPixel);
        envTex = std::make_shared<TexImage>();
        RectD dummy;
        if ( !toTexImage(env, dummy, sx, sy, envTex.get() ) ) {
            envTex.reset();
        }
    }
    // format (pixels at this scale)
    RectD fmt;
    {
        EffectInstancePtr bg = getInput(2);
        bool isProjectFormat = false;
        if ( !bg || (bg->getRegionOfDefinition_public(bg->getRenderHash(), t, args.originalScale, args.view, &fmt, &isProjectFormat) == eStatusFailed) ) {
            projectFormat(&fmt);
        }
    }
    ScanlineSettings st;
    st.originX = args.roi.x1;
    st.originY = args.roi.y1;
    st.width = args.roi.x2 - args.roi.x1;
    st.height = args.roi.y2 - args.roi.y1;
    st.formatX1 = fmt.x1 * sx; st.formatY1 = fmt.y1 * sy; st.formatX2 = fmt.x2 * sx; st.formatY2 = fmt.y2 * sy;
    {
        Format pf;
        getApp()->getProject()->getProjectDefaultFormat(&pf);
        st.pixelAspect = pf.getPixelAspectRatio();
    }
    const int smpIdx = _samples.lock()->getValueAtTime(t);
    st.samples = (smpIdx + 1) * (smpIdx + 1);
    st.filter = (ScanlineFilter)_filter.lock()->getValueAtTime(t);
    st.filterWidth = _filterWidth.lock()->getValueAtTime(t);
    st.textureFiltering = _texFilter.lock()->getValueAtTime(t);
    st.output = (ScanlineOutput)_output.lock()->getValueAtTime(t);
    st.ambient = _ambient.lock()->getValueAtTime(t);
    std::vector<float> buf;
    scanlineRender(scene, camPath, st,
                   [&](const std::shared_ptr<void>& h) -> const TexImage* {
                       std::map<const void*, std::shared_ptr<TexImage> >::const_iterator it = texCache.find( h.get() );
                       return it == texCache.end() ? (const TexImage*)0 : it->second.get();
                   },
                   envTex.get(), &buf);
    // over bg
    ImagePtr bgImg;
    if ( getInput(2) ) {
        RectI roiPixel;
        const ImagePlaneDesc rgba = ImagePlaneDesc::getRGBAComponents();
        bgImg = getImage(2, t, args.originalScale, args.view, NULL, &rgba, false, true, eStorageModeRAM, 0, &roiPixel);
    }
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin(); it != args.outputPlanes.end(); ++it) {
        const ImagePtr& dst = it->second;
        const int dnc = (int)dst->getComponentsCount();
        Image::WriteAccess wa( dst.get() );
        std::shared_ptr<Image::ReadAccess> ra;
        int bnc = 0;
        RectI bb;
        if (bgImg && (bgImg->getBitDepth() == eImageBitDepthFloat)) {
            ra = std::make_shared<Image::ReadAccess>( bgImg.get() );
            bnc = (int)bgImg->getComponentsCount();
            bb = bgImg->getBounds();
        }
        for (int y = args.roi.y1; y < args.roi.y2; ++y) {
            float* d = (float*)wa.pixelAt(args.roi.x1, y);
            if (!d) {
                continue;
            }
            for (int x = args.roi.x1; x < args.roi.x2; ++x, d += dnc) {
                const float* s = &buf[( (size_t)(y - args.roi.y1) * st.width + (x - args.roi.x1) ) * 4];
                float b[4] = { 0, 0, 0, 0 };
                if ( ra && (x >= bb.x1) && (x < bb.x2) && (y >= bb.y1) && (y < bb.y2) ) {
                    const float* p = (const float*)ra->pixelAt(x, y);
                    if (p) {
                        for (int c = 0; c < std::min(4, bnc); ++c) {
                            b[c] = p[c];
                        }
                        if (bnc == 3) {
                            b[3] = 1.f;
                        }
                    }
                }
                float o[4];
                for (int c = 0; c < 4; ++c) {
                    o[c] = s[c] + b[c] * (1.f - s[3]);
                }
                for (int c = 0; c < dnc; ++c) {
                    d[c] = dnc == 1 ? o[3] : o[c];
                }
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
