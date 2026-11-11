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
#include <sstream>

#include "Global/GLIncludes.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/TimeLine.h"
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
// knobs go on a page or into a group (both have addKnob)
template <class C>
static KnobDoublePtr
makeDouble(EffectInstance* fx, const std::shared_ptr<C>& parent, const char* name, const char* label, int dims,
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
    parent->addKnob(k);

    return k;
}

template <class C>
static KnobChoicePtr
makeChoice(EffectInstance* fx, const std::shared_ptr<C>& parent, const char* name, const char* label,
           const std::vector<std::string>& options, int def, const char* hint = "", bool newLine = true)
{
    KnobChoicePtr k = AppManager::createKnob<KnobChoice>( fx, std::string(label) );
    k->setName(name);
    std::vector<ChoiceOption> o;
    for (const std::string& s : options) {
        o.push_back( ChoiceOption(s, s, "") );
    }
    k->populateChoices(o);
    k->setDefaultValue(def);
    k->setAnimationEnabled(false);
    if (hint && *hint) {
        k->setHintToolTip( std::string(hint) );
    }
    k->setAddNewLine(newLine);
    parent->addKnob(k);

    return k;
}

template <class C>
static KnobBoolPtr
makeBool(EffectInstance* fx, const std::shared_ptr<C>& parent, const char* name, const char* label, bool def,
         const char* hint = "", bool newLine = true)
{
    KnobBoolPtr k = AppManager::createKnob<KnobBool>( fx, std::string(label) );
    k->setName(name);
    k->setDefaultValue(def);
    if (hint && *hint) {
        k->setHintToolTip( std::string(hint) );
    }
    k->setAddNewLine(newLine);
    parent->addKnob(k);

    return k;
}

static KnobGroupPtr
makeGroup(EffectInstance* fx, const KnobPagePtr& page, const char* name, const char* label, bool open)
{
    KnobGroupPtr g = AppManager::createKnob<KnobGroup>( fx, std::string(label) );
    g->setName(name);
    g->setDefaultValue(open);
    page->addKnob(g);

    return g;
}

static void
makeDivider(EffectInstance* fx, const KnobPagePtr& page, const char* name)
{
    KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>( fx, std::string() );
    sep->setName(name);
    page->addKnob(sep);
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

static const char* kRotOrders[6] = { "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX" };
static const char* kXformOrders[6] = { "SRT", "STR", "RST", "RTS", "TSR", "TRS" };

// rotation applied in the order of the letters (XYZ = X first)
static Mat4
rotationInOrder(const Vec3& deg, int order)
{
    const char* o = kRotOrders[std::max( 0, std::min(5, order) )];
    Mat4 r;
    for (int i = 0; i < 3; ++i) {
        Vec3 v;
        switch (o[i]) {
        case 'X': v.x = deg.x; break;
        case 'Y': v.y = deg.y; break;
        default: v.z = deg.z; break;
        }
        r = Mat4::rotateXYZ(v) * r;
    }

    return r;
}

void
Solv3DXformKnobs::create(EffectInstance* fx, const KnobPagePtr& page, int defaultRotOrder)
{
    order = makeChoice(fx, page, "xform_order", "transform order", std::vector<std::string>(kXformOrders, kXformOrders + 6), 0,
                       "Order of scale (S), rotation (R) and translation (T): SRT = scale first, then rotate, then move.", false);
    rotOrder = makeChoice(fx, page, "rot_order", "rotation order", std::vector<std::string>(kRotOrders, kRotOrders + 6), defaultRotOrder,
                          "Order of the X / Y / Z rotations (ZXY = Z first).");
    translate = makeDouble(fx, page, "translate", "translate", 3, 0., -10., 10.);
    rotate = makeDouble(fx, page, "rotate", "rotate", 3, 0., -180., 180., "Degrees.");
    scale = makeDouble(fx, page, "scaling", "scale", 3, 1., 0., 4.);
    uniform = makeDouble(fx, page, "uniform_scale", "uniform scale", 1, 1., 0., 10.);
    pivot = makeDouble(fx, page, "pivot", "pivot translate", 3, 0., -10., 10., "Centre of rotation and scale.");
    pivotRotate = makeDouble(fx, page, "pivot_rotate", "pivot rotate", 3, 0., -180., 180., "Orientation of the rotation and scale axes.");
}

Mat4
Solv3DXformKnobs::matrix(double t) const
{
    const double u = uniform.lock() ? uniform.lock()->getValueAtTime(t) : 1.;
    const int ro = rotOrder.lock() ? rotOrder.lock()->getValueAtTime(t) : 0;
    const int xo = order.lock() ? order.lock()->getValueAtTime(t) : 0;
    const Vec3 pv = getV3(pivot, t);
    const Mat4 P = Mat4::translate(pv), Pi = Mat4::translate(pv * -1.);
    const Mat4 Pr = rotationInOrder(getV3(pivotRotate, t), ro), Pri = Pr.inverse();
    const Mat4 T = Mat4::translate( getV3(translate, t) );
    const Mat4 R = P * Pr * rotationInOrder(getV3(rotate, t), ro) * Pri * Pi;
    const Mat4 S = P * Pr * Mat4::scale(getV3(scale, t) * u) * Pri * Pi;
    Mat4 m;
    const char* o = kXformOrders[std::max( 0, std::min(5, xo) )];
    for (int i = 0; i < 3; ++i) {
        m = (o[i] == 'S' ? S : (o[i] == 'R' ? R : T)) * m;
    }

    return m;
}

static const char* kDisplayModes[6] = { "off", "wireframe", "solid", "solid+lines", "textured", "textured+lines" };

void
Solv3DGeoKnobs::create(EffectInstance* fx, const KnobPagePtr& page)
{
    display = makeChoice(fx, page, "display", "display", std::vector<std::string>(kDisplayModes, kDisplayModes + 6), 4,
                         "How it is drawn in the 3D viewer (does not change the render).", false);
    selectable = makeBool(fx, page, "selectable", "selectable", true, "Can be picked with a click in the 3D viewer.");
    const char* rm[3] = { "off", "solid", "textured" };
    render = makeChoice(fx, page, "render_mode", "render", std::vector<std::string>(rm, rm + 3), 2,
                        "ScanlineRender: off = not rendered, solid = colour only (no texture / projections), textured = everything.");
}

void
Solv3DGeoKnobs::apply(Scene* scene, double t, const std::string& node, const std::string& root) const
{
    const int d = display.lock() ? display.lock()->getValueAtTime(t) : 4;
    const int r = render.lock() ? render.lock()->getValueAtTime(t) : 2;
    const bool sel = selectable.lock() ? selectable.lock()->getValueAtTime(t) : true;
    for (auto& kv : scene->prims()) {
        const std::string& path = kv.first;
        if ( !root.empty() && (path != root) && (path.compare(0, root.size() + 1, root + "/") != 0) ) {
            continue;
        }
        kv.second.display = d;
        kv.second.render = r;
        kv.second.selectable = sel;
        kv.second.node = node;
    }
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

std::string
Solv3DNode::nodeName() const
{
    return getNode()->getFullyQualifiedName();
}

Solv3DNode*
Solv3DNode::as3D(const EffectInstancePtr& e)
{
    Solv3DNode* n = dynamic_cast<Solv3DNode*>( e.get() );

    return (n && n->is3D()) ? n : 0;
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
    _geo.create(this, page);
    makeDivider(this, page, "geo_divider");
    if (_kind == eCard) {
        const char* orients[3] = { "XY", "YZ", "ZX" };
        _orientation = makeChoice(this, page, "orientation", "orientation", std::vector<std::string>(orients, orients + 3), 0,
                                  "Plane of the card: XY faces +Z (default), YZ faces +X, ZX faces +Y (a floor).", false);
        _imageAspect = makeBool(this, page, "image_aspect", "image aspect", true,
                                "Card height follows the img input proportions (width = size). Off: square.");
    }
    _size = makeDouble(this, page, "size", "size", 1, 1., 0., 10., "Overall size of the shape.");
    if ( (_kind == eCard) || (_kind == eSphere) || (_kind == eCylinder) ) {
        KnobIntPtr r = AppManager::createKnob<KnobInt>( this, std::string("rows/columns") );
        r->setName("rows");
        r->setDefaultValue(_kind == eCard ? 1 : 24);
        r->setDisplayMinimum(1);
        r->setDisplayMaximum(64);
        r->setHintToolTip( std::string("Subdivisions (rows, then columns on the next field).") );
        r->setAddNewLine(false);
        page->addKnob(r);
        _rows = r;
        KnobIntPtr c = AppManager::createKnob<KnobInt>( this, std::string("") );
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
    _lit = makeBool(this, page, "lit", "lit", _kind != eCard,
                    "Off: the texture is shown as is (like a card). On: simple shading from the camera.");
    makeDivider(this, page, "xform_divider");
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
    Mat4 orient;
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
        const int o = _orientation.lock() ? _orientation.lock()->getValueAtTime(t) : 0;
        if (o == 1) {
            orient = Mat4::rotateXYZ( Vec3(0, 90, 0) );   // faces +X
        } else if (o == 2) {
            orient = Mat4::rotateXYZ( Vec3(-90, 0, 0) );  // faces +Y (floor)
        }
        break;
    }
    }
    m->texture = texRef(img);
    KnobColorPtr c = _color.lock();
    m->material.baseColor = Vec3( c->getValueAtTime(t, 0), c->getValueAtTime(t, 1), c->getValueAtTime(t, 2) );
    m->material.emission = _lit.lock()->getValueAtTime(t) ? 0. : 1.;
    Prim& p = scene->define(primPath(), PrimType::Mesh);
    p.mesh = m;
    p.local = _xf.matrix(t) * orient;
    _geo.apply( scene, t, nodeName(), primPath() );
}

// ============================================================================ Camera
std::string
Solv3DCamera::getPluginDescription() const
{
    return "3D camera. Focal length and film back (apertures) in mm, perspective or orthographic, clipping planes, "
           "window (shift / zoom / roll of the image) and depth of field values. Look through it in the 3D viewer; "
           "ScanlineRender and Project3D use it through their cam input. A camera can be made from vanishing lines "
           "with the VanishingCamera node.";
}

void
Solv3DCamera::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "Camera");
    _display = makeChoice(this, page, "display", "display", std::vector<std::string>(kDisplayModes, kDisplayModes + 6), 1,
                          "off hides the camera in the 3D viewer.");
    makeDivider(this, page, "xform_divider");
    _xf.create(this, page, 4);

    KnobPagePtr proj = makePage(this, "Projection");
    const char* pm[2] = { "perspective", "orthographic" };
    _projection = makeChoice(this, proj, "projection", "projection", std::vector<std::string>(pm, pm + 2), 0,
                             "Orthographic: the horizontal aperture is the width of the view in tenths of a scene unit (USD).");
    KnobGroupPtr fr = makeGroup(this, proj, "frustum", "Frustum", true);
    _focal = makeDouble(this, fr, "focal", "focal length", 1, 50., 5., 200., "mm");
    _hAperture = makeDouble(this, fr, "haperture", "horiz aperture", 1, 24.576, 1., 70., "Film back width (mm). The image width fits it.");
    _vAperture = makeDouble(this, fr, "vaperture", "vert aperture", 1, 18.672, 1., 70., "Film back height (mm), kept for export: the image height follows the format.");
    _near = makeDouble(this, fr, "near", "clipping near", 1, 0.1, 0.001, 10.);
    _far = makeDouble(this, fr, "far", "clipping far", 1, 10000., 10., 100000.);
    KnobGroupPtr win = makeGroup(this, proj, "window", "Window", true);
    _winTranslate = makeDouble(this, win, "win_translate", "translate", 2, 0., -1., 1., "Shifts the image (1 = half the frame).");
    _winScale = makeDouble(this, win, "win_scale", "scale", 2, 1., 0.1, 4., "Zooms the image (2 = shows twice the area).");
    _winRoll = makeDouble(this, win, "winroll", "roll", 1, 0., -45., 45., "Rolls the image (degrees).");
    KnobGroupPtr dof = makeGroup(this, proj, "dof", "Depth of Field", false);
    _focusDist = makeDouble(this, dof, "focal_point", "focal distance", 1, 2., 0.1, 100., "Distance in focus (stored with the camera and exported).");
    _fStop = makeDouble(this, dof, "fstop", "f-stop", 1, 16., 0.7, 32.);
}

void
Solv3DCamera::buildScene(double t, Scene* scene)
{
    std::shared_ptr<Camera> c = std::make_shared<Camera>();
    c->projectionMode = _projection.lock()->getValueAtTime(t);
    c->focalLength = _focal.lock()->getValueAtTime(t);
    c->horizontalAperture = _hAperture.lock()->getValueAtTime(t);
    c->verticalAperture = _vAperture.lock()->getValueAtTime(t);
    c->nearClip = _near.lock()->getValueAtTime(t);
    c->farClip = _far.lock()->getValueAtTime(t);
    KnobDoublePtr wt = _winTranslate.lock(), ws = _winScale.lock();
    c->winTranslate[0] = wt->getValueAtTime(t, 0);
    c->winTranslate[1] = wt->getValueAtTime(t, 1);
    c->winScale[0] = ws->getValueAtTime(t, 0);
    c->winScale[1] = ws->getValueAtTime(t, 1);
    c->winRoll = _winRoll.lock()->getValueAtTime(t);
    c->focusDistance = _focusDist.lock()->getValueAtTime(t);
    c->fStop = _fStop.lock()->getValueAtTime(t);
    Prim& p = scene->define(primPath(), PrimType::Camera);
    p.camera = c;
    p.local = _xf.matrix(t);
    p.display = _display.lock()->getValueAtTime(t);
    p.node = nodeName();
}

// ============================================================================ VanishingCamera
std::string
Solv3DVanishingCamera::getPluginDescription() const
{
    return "Finds a camera from a still plate. Drag the X (red), Y (green) and Z (blue) line pairs onto edges of the "
           "plate that are parallel in the real world (the three directions at right angles), then press 'solve': "
           "the focal length comes from where the lines meet (vanishing points) and the rotation from their "
           "directions; the camera is placed 'distance' away from the start of the first red line. 'Export' creates a "
           "Camera node, linked to this result or baked. The image passes through unchanged.";
}

void
Solv3DVanishingCamera::initializeKnobs()
{
    KnobPagePtr vp = makePage(this, "VanishingCamera");
    const char* ax[3] = { "X and Z", "X and Y", "X, Y and Z" };
    _axes = makeChoice(this, vp, "axes", "axes", std::vector<std::string>(ax, ax + 3), 0,
                       "Which line pairs are used. Two directions: the lens centre is the image centre. Three: the lens centre is solved too.");
    _axes.lock()->setEvaluateOnChange(false);
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
        addOverlaySlaveParam(k);
    }
    _hAperture = makeDouble(this, vp, "haperture", "film back width", 1, 24.576, 1., 70., "Horizontal aperture (mm) of the exported camera.");
    _hAperture.lock()->setEvaluateOnChange(false);
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
    KnobStringPtr st = AppManager::createKnob<KnobString>( this, std::string("status") );
    st->setName("status");
    st->setAsLabel();
    vp->addKnob(st);
    _status = st;

    KnobGroupPtr res = makeGroup(this, vp, "result", "Result", true);
    _rFocal = makeDouble(this, res, "focal", "focal length", 1, 50., 5., 200., "mm (solved)");
    _rVAperture = makeDouble(this, res, "vaperture", "vert aperture", 1, 18.672, 1., 70., "mm (film back width / plate aspect)");
    _rTranslate = makeDouble(this, res, "translate", "translate", 3, 0., -10., 10.);
    _rRotate = makeDouble(this, res, "rotate", "rotate", 3, 0., -180., 180., "Degrees, rotation order XYZ.");
    KnobDoublePtr rk[4] = { _rFocal.lock(), _rVAperture.lock(), _rTranslate.lock(), _rRotate.lock() };
    for (int i = 0; i < 4; ++i) {
        rk[i]->setEvaluateOnChange(false);
        rk[i]->setAnimationEnabled(false);
        rk[i]->setAllDimensionsEnabled(false);
    }

    makeDivider(this, vp, "export_divider");
    const char* em[2] = { "camera (linked)", "camera (baked)" };
    _exportMode = makeChoice(this, vp, "export_mode", "export", std::vector<std::string>(em, em + 2), 0,
                             "linked: the Camera follows this node (re-solving updates it). baked: a copy of the values.", false);
    _exportMode.lock()->setEvaluateOnChange(false);
    KnobButtonPtr ex = AppManager::createKnob<KnobButton>( this, std::string("create") );
    ex->setName("export_create");
    ex->setHintToolTip( std::string("Creates the Camera node next to this one.") );
    vp->addKnob(ex);
    _export = ex;
}

void
Solv3DVanishingCamera::plateFormat(RectD* f)
{
    EffectInstancePtr in = getInput(0);
    if (in) {
        bool isProjectFormat = false;
        const double t = getApp() ? getApp()->getTimeLine()->currentFrame() : 0.;
        if ( (in->getRegionOfDefinition_public(in->getRenderHash(), t, RenderScale(), ViewIdx(0), f, &isProjectFormat) != eStatusFailed) &&
             (f->width() > 0) && (f->height() > 0) ) {
            return;
        }
    }
    projectFormat(f);
}

bool
Solv3DVanishingCamera::isIdentity(double time, const RenderScale &, const RectI &, ViewIdx view, double* inputTime, ViewIdx* inputView, int* inputNb)
{
    if ( !getInput(0) ) {
        return false;
    }
    *inputTime = time;
    *inputView = view;
    *inputNb = 0;

    return true;
}

StatusEnum
Solv3DVanishingCamera::getRegionOfDefinition(U64 hash, double time, const RenderScale & scale, ViewIdx view, RectD* rod)
{
    EffectInstancePtr in = getInput(0);
    if (in) {
        bool isProjectFormat = false;

        return in->getRegionOfDefinition_public(in->getRenderHash(), time, scale, view, rod, &isProjectFormat);
    }

    return Solv3DNode::getRegionOfDefinition(hash, time, scale, view, rod);
}

void
Solv3DVanishingCamera::getRegionsOfInterest(double, const RenderScale &, const RectD &, const RectD & renderWindow, ViewIdx, RoIMap* ret)
{
    ret->clear();
    EffectInstancePtr in = getInput(0);
    if (in) {
        (*ret)[in] = renderWindow;
    }
}

void
Solv3DVanishingCamera::resetLines()
{
    RectD f;
    plateFormat(&f);
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
Solv3DVanishingCamera::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/, bool /*mainThread*/)
{
    if ( k == _solve.lock().get() ) {
        solveFromLines();

        return true;
    }
    if ( k == _reset.lock().get() ) {
        resetLines();

        return true;
    }
    if ( k == _export.lock().get() ) {
        exportCamera();

        return true;
    }

    return false;
}

void
Solv3DVanishingCamera::solveFromLines()
{
    RectD f;
    plateFormat(&f);
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
            _status.lock()->setValue("The two lines of an axis are parallel in the image: tilt them so they meet.");

            return;
        }
        dirs[i] = l[0];
    }
    const SolvVP::V2 centre{ (f.x1 + f.x2) * 0.5, (f.y1 + f.y2) * 0.5 };
    SolvVP::Result r = SolvVP::solve(vps, n, centre, dirs);
    if (!r.ok) {
        _status.lock()->setValue( std::string("Cannot solve: ") + r.error );

        return;
    }
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
    const double ha = _hAperture.lock()->getValue();
    const double focal = SolvVP::focalMM( r.f, ha, f.width() );
    beginChanges();
    KnobDoublePtr t = _rTranslate.lock(), ro = _rRotate.lock();
    t->setValue(camPos.x, ViewSpec::all(), 0); t->setValue(camPos.y, ViewSpec::all(), 1); t->setValue(camPos.z, ViewSpec::all(), 2);
    ro->setValue(rx * toDeg, ViewSpec::all(), 0); ro->setValue(ry * toDeg, ViewSpec::all(), 1); ro->setValue(rz * toDeg, ViewSpec::all(), 2);
    _rFocal.lock()->setValue(focal);
    _rVAperture.lock()->setValue( ha * f.height() / std::max(1e-9, f.width()) );
    endChanges();
    std::stringstream ss;
    ss.precision(3);
    ss << std::fixed << "solved: focal " << focal << " mm, horizontal field of view "
       << 2. * std::atan( ha / (2. * focal) ) * toDeg << " deg";
    _status.lock()->setValue( ss.str() );
}

void
Solv3DVanishingCamera::exportCamera()
{
    NodePtr thisNode = getNode();
    AppInstancePtr app = getApp();
    if (!thisNode || !app) {
        return;
    }
    CreateNodeArgs args( PLUGINID_SOLV3D_CAMERA, thisNode->getGroup() );
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    NodePtr cam = app->createNode(args);
    if (!cam) {
        return;
    }
    double pos[2], size[2];
    thisNode->getPosition(&pos[0], &pos[1]);
    thisNode->getSize(&size[0], &size[1]);
    cam->setPosition(pos[0] + size[0] * 1.6, pos[1]);
    const bool linked = _exportMode.lock()->getValue() == 0;
    // the solve uses rotation order XYZ and SRT
    KnobChoicePtr ro = std::dynamic_pointer_cast<KnobChoice>( cam->getKnobByName("rot_order") );
    if (ro) {
        ro->setValue(0);
    }
    KnobChoicePtr xo = std::dynamic_pointer_cast<KnobChoice>( cam->getKnobByName("xform_order") );
    if (xo) {
        xo->setValue(0);
    }
    struct Pair { const char* camKnob; KnobDoublePtr src; int dims; };
    const Pair pairs[5] = {
        { "translate", _rTranslate.lock(), 3 }, { "rotate", _rRotate.lock(), 3 }, { "focal", _rFocal.lock(), 1 },
        { "haperture", _hAperture.lock(), 1 }, { "vaperture", _rVAperture.lock(), 1 }
    };
    for (int i = 0; i < 5; ++i) {
        KnobDoublePtr dst = std::dynamic_pointer_cast<KnobDouble>( cam->getKnobByName(pairs[i].camKnob) );
        if ( !dst || !pairs[i].src ) {
            continue;
        }
        for (int d = 0; d < pairs[i].dims; ++d) {
            if (linked) {
                dst->slaveTo(d, pairs[i].src, d);
            } else {
                dst->setValue(pairs[i].src->getValue(d), ViewSpec::all(), d);
            }
        }
    }
}

static const float kAxisCol[3][3] = { { 0.93f, 0.33f, 0.31f }, { 0.4f, 0.85f, 0.42f }, { 0.36f, 0.55f, 1.f } };

void
Solv3DVanishingCamera::drawOverlay(double /*time*/, const RenderScale & /*renderScale*/, ViewIdx /*view*/)
{
    // first use: put the lines somewhere sensible
    KnobDoublePtr l0 = _lines[0].lock();
    if ( (l0->getValue(0) == 0.) && (l0->getValue(2) == 0.) ) {
        resetLines();
    }
    GLProtectAttrib a(GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_POINT_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);
    const int mode = _axes.lock()->getValue();
    for (int i = 0; i < 6; ++i) {
        const int axis = i / 2;
        const bool used = (axis == 0) || (mode == 2) || (axis == 1 && mode == 1) || (axis == 2 && mode == 0);
        KnobDoublePtr k = _lines[i].lock();
        const float* c = kAxisCol[axis];
        const float alpha = used ? 1.f : 0.3f;
        const double x1 = k->getValue(0), y1 = k->getValue(1), x2 = k->getValue(2), y2 = k->getValue(3);
        // extended guide (dim) so the convergence is visible
        const double dx = x2 - x1, dy = y2 - y1;
        glLineWidth(1.f);
        glColor4f(c[0], c[1], c[2], 0.35f * alpha);
        glBegin(GL_LINES);
        glVertex2d(x1 - dx * 3., y1 - dy * 3.);
        glVertex2d(x2 + dx * 3., y2 + dy * 3.);
        glEnd();
        glLineWidth(2.f);
        glColor4f(c[0], c[1], c[2], alpha);
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
                glColor4f(c[0], c[1], c[2], alpha);
            }
            glVertex2d(e ? x2 : x1, e ? y2 : y1);
        }
        glEnd();
    }
}

bool
Solv3DVanishingCamera::onOverlayPenDown(double, const RenderScale &, ViewIdx, const QPointF &, const QPointF & pos, double, double, PenType)
{
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
Solv3DVanishingCamera::onOverlayPenMotion(double, const RenderScale &, ViewIdx, const QPointF &, const QPointF & pos, double, double)
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
Solv3DVanishingCamera::onOverlayPenUp(double, const RenderScale &, ViewIdx, const QPointF &, const QPointF &, double, double)
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
        root->node = nodeName();
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
    _geo.create(this, page);
    makeDivider(this, page, "geo_divider");
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
    makeDivider(this, page, "xform_divider");
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
    _geo.apply( &r, t, nodeName(), primPath() );
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
    const char* po[3] = { "both", "front", "back" };
    _projectOn = makeChoice(this, page, "project_on", "project on", std::vector<std::string>(po, po + 3), 0,
                            "Which faces get the image, as seen from the projection camera: both, only the faces turned towards it, or only the ones turned away.");
    const char* oc[2] = { "none", "self" };
    _occlusion = makeChoice(this, page, "occlusion_mode", "occlusion mode", std::vector<std::string>(oc, oc + 2), 0,
                            "self: surfaces hidden from the projection camera by other geometry get nothing (no projection through objects).");
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
    const Mat4 camWorld = cs.worldMatrix(camPath);
    pr.viewProj = cp->camera->projection(aspect) * camWorld.inverse();
    pr.texture = texRef(img);
    pr.crop = _crop.lock()->getValueAtTime(t);
    pr.projectOn = _projectOn.lock()->getValueAtTime(t);
    pr.occlusion = _occlusion.lock()->getValueAtTime(t) == 1;
    pr.origin = camWorld.point( Vec3() );
    pr.forward = normalize( camWorld.vector( Vec3(0, 0, -1) ) );
    pr.orthographic = cp->camera->projectionMode == 1;
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
    _transparency = makeBool(this, page, "transparency", "transparency", true,
                             "Keep the alpha of the textures (holes in cards). Off: every surface is opaque.", false);
    _zBuffer = makeBool(this, page, "ztest_enabled", "Z-buffer", true,
                        "Hide what is behind other surfaces. Off: later objects draw over earlier ones.");
    const char* outs[5] = { "rgba", "depth (distance to camera)", "world position", "world normal", "uv" };
    _output = makeChoice(this, page, "output", "output", std::vector<std::string>(outs, outs + 5), 0,
                         "rgba, or a data pass for comp work.");
    const char* pm[5] = { "render camera", "perspective", "orthographic", "uv", "spherical" };
    _projMode = makeChoice(this, page, "projection_mode", "projection mode", std::vector<std::string>(pm, pm + 5), 0,
                           "render camera: as the camera is set. perspective / orthographic: forces it. uv: unwraps the geometry "
                           "(the 0..1 uv square fills the format, for baking projections into textures). spherical: lat-long "
                           "around the camera position (environment maps).");
    KnobIntPtr os = AppManager::createKnob<KnobInt>( this, std::string("overscan") );
    os->setName("overscan");
    os->setDefaultValue(0);
    os->setDisplayMinimum(0);
    os->setDisplayMaximum(500);
    os->setHintToolTip( std::string("Pixels rendered outside the format on every side (for distortion or blur later).") );
    page->addKnob(os);
    _overscan = os;
    _ambient = makeDouble(this, page, "ambient", "ambient", 1, 0.15, 0., 1., "Minimum light on lit surfaces facing away from the camera.");

    KnobPagePtr ms = makePage(this, "MultiSample");
    const char* n[8] = { "1", "4", "9", "16", "25", "36", "49", "64" };
    _samples = makeChoice(this, ms, "samples", "samples", std::vector<std::string>(n, n + 8), 3,
                          "Samples per pixel (anti-aliasing). 16 is a good default, 64 for final hard edges.");
    const char* fl[5] = { "box", "triangle", "gaussian", "Mitchell", "Blackman-Harris" };
    _filter = makeChoice(this, ms, "filter", "filter", std::vector<std::string>(fl, fl + 5), 4,
                         "box: sharpest, can alias. triangle: soft. gaussian: softest. Mitchell: sharp, slight ringing. "
                         "Blackman-Harris: clean smooth edges (default).");
    _filterWidth = makeDouble(this, ms, "filter_width", "filter width", 1, 2., 1., 4., "Filter diameter in pixels.");
    _texFilter = makeBool(this, ms, "texture_filtering", "texture filtering", true,
                          "Mip-mapped textures: no shimmering when textures are small on screen.");
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
    StatusEnum st = eStatusOK;
    if (bg) {
        bool isProjectFormat = false;
        st = bg->getRegionOfDefinition_public(bg->getRenderHash(), time, scale, view, rod, &isProjectFormat);
    } else {
        st = Solv3DNode::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    const int o = _overscan.lock() ? _overscan.lock()->getValueAtTime(time) : 0;
    if ( (st != eStatusFailed) && (o > 0) ) {
        rod->x1 -= o; rod->y1 -= o; rod->x2 += o; rod->y2 += o;
    }

    return st;
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
    st.transparency = _transparency.lock()->getValueAtTime(t);
    st.zBuffer = _zBuffer.lock()->getValueAtTime(t);
    st.projectionMode = _projMode.lock()->getValueAtTime(t);
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
