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
#include <cstring>


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
#include "Engine/Solv3D/SolvCameraPresets.h"
#include "Engine/Solv3D/SolvLineAlign.h"
#include "Engine/Solv3D/SolvUsd.h"

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

// film back presets (cascading menu: maker / camera mode)
static KnobChoicePtr
makePresetChoice(EffectInstance* fx, const KnobPagePtr& page)
{
    std::vector<std::string> names;
    for (int i = 0; i < kCameraPresetCount; ++i) {
        names.push_back(kCameraPresets[i].name);
    }
    KnobChoicePtr k = makeChoice(fx, page, "film_back_preset", "film back preset", names, 0,
                                 "Sets the film back (sensor area of the recording mode) of known cameras. Phones: 35 mm-equivalent "
                                 "film back + the lens' equivalent focal length (video is cropped a little by stabilisation: approximate).");
    k->setCascading(true);
    k->setEvaluateOnChange(false);

    return k;
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
    castShadow = makeBool(fx, page, "cast_shadow", "cast shadow", true, "Blocks the light of lights with shadows on.", false);
    receiveShadow = makeBool(fx, page, "receive_shadow", "receive shadow", true, "Shadows fall on it.");
    render = makeChoice(fx, page, "render_mode", "render", std::vector<std::string>(kDisplayModes, kDisplayModes + 6), 4,
                        "How ScanlineRender draws it: off, wireframe (edges in the colour), solid (colour, no texture / projections), "
                        "solid+lines, textured (everything), textured+lines (dark edges over the surface).");
}

void
Solv3DGeoKnobs::apply(Scene* scene, double t, const std::string& node, const std::string& root) const
{
    const int d = display.lock() ? display.lock()->getValueAtTime(t) : 4;
    const int r = render.lock() ? render.lock()->getValueAtTime(t) : 4;
    const bool sel = selectable.lock() ? selectable.lock()->getValueAtTime(t) : true;
    const bool cs = castShadow.lock() ? castShadow.lock()->getValueAtTime(t) : true;
    const bool rs = receiveShadow.lock() ? receiveShadow.lock()->getValueAtTime(t) : true;
    for (auto& kv : scene->prims()) {
        const std::string& path = kv.first;
        if ( !root.empty() && (path != root) && (path.compare(0, root.size() + 1, root + "/") != 0) ) {
            continue;
        }
        kv.second.display = d;
        kv.second.render = r;
        kv.second.selectable = sel;
        kv.second.castShadow = cs;
        kv.second.receiveShadow = rs;
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
    // Maya scale (1 unit = 1 cm): plane / cube 1, sphere radius 1, cylinder radius 1 height 2
    const double defSize = ( (_kind == eSphere) || (_kind == eCylinder) ) ? 2. : 1.;
    _size = makeDouble(this, page, "size", "size", 1, defSize, 0., 10., "Overall size of the shape (width; sphere and cylinder: diameter).");
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

// ============================================================================ Lights
std::string
Solv3DLight::getPluginID() const
{
    switch (_kind) {
    case eSpot: return PLUGINID_SOLV3D_SPOTLIGHT;
    case eDirectional: return PLUGINID_SOLV3D_DIRLIGHT;
    case eAmbient: return PLUGINID_SOLV3D_AMBIENTLIGHT;
    default: return PLUGINID_SOLV3D_POINTLIGHT;
    }
}

std::string
Solv3DLight::getPluginLabel() const
{
    switch (_kind) {
    case eSpot: return "Spotlight";
    case eDirectional: return "DirectLight";
    case eAmbient: return "AmbientLight";
    default: return "PointLight";
    }
}

std::string
Solv3DLight::getPluginDescription() const
{
    switch (_kind) {
    case eSpot:
        return "Spotlight: shines down its -Z axis in a cone (cone angle, soft edge = penumbra). Shadows are off by default; "
               "'softness' is the size of the light (bigger = softer shadows away from the contact).";
    case eDirectional:
        return "Directional light (sun): parallel light along its -Z axis, only the rotation matters. Shadows are off by default; "
               "'softness' is the angular size of the light in degrees (the sun is about 0.5).";
    case eAmbient:
        return "Ambient light: the same light everywhere, no direction and no shadows.";
    default:
        return "Point light: shines in all directions from its position. Shadows are off by default; 'softness' is the size "
               "of the light (bigger = softer shadows away from the contact).";
    }
}

void
Solv3DLight::initializeKnobs()
{
    KnobPagePtr page = makePage( this, getPluginLabel().c_str() );
    _display = makeChoice(this, page, "display", "display", std::vector<std::string>(kDisplayModes, kDisplayModes + 6), 1,
                          "off hides the light in the 3D viewer.");
    KnobColorPtr col = AppManager::createKnob<KnobColor>( this, std::string("color"), 3 );
    col->setName("color");
    for (int d = 0; d < 3; ++d) {
        col->setDefaultValue(1., d);
    }
    page->addKnob(col);
    _color = col;
    _intensity = makeDouble(this, page, "intensity", "intensity", 1, 1., 0., 10.);
    if ( (_kind == ePoint) || (_kind == eSpot) ) {
        const char* fo[3] = { "none", "linear", "quadratic" };
        _falloff = makeChoice(this, page, "falloff_type", "falloff", std::vector<std::string>(fo, fo + 3), 0,
                              "How the light gets weaker with distance (quadratic = physical; raise the intensity).");
    }
    if (_kind == eSpot) {
        _cone = makeDouble(this, page, "cone_angle", "cone angle", 1, 30., 1., 170., "Full angle of the cone (degrees).");
        _penumbra = makeDouble(this, page, "cone_penumbra_angle", "cone penumbra", 1, 5., 0., 60., "Width of the soft edge of the cone (degrees).");
    }
    if (_kind != eAmbient) {
        KnobGroupPtr sh = makeGroup(this, page, "shadows", "Shadows", false);
        _castShadows = makeBool(this, sh, "cast_shadows", "cast shadows", false, "Off by default (faster). On: soft shadow maps.");
        _softness = makeDouble(this, sh, "shadow_softness", "softness", 1, _kind == eDirectional ? 0.5 : 0., 0., _kind == eDirectional ? 10. : 50.,
                               _kind == eDirectional ? "Angular size of the light (degrees): 0 = hard, the sun is about 0.5." :
                               "Size of the light (scene units, 1 = 1 cm): 0 = hard. Shadows stay sharp at the contact and soften with distance.");
        const char* q[4] = { "low (8)", "medium (16)", "high (32)", "very high (64)" };
        _shadowQuality = makeChoice(this, sh, "shadow_quality", "quality", std::vector<std::string>(q, q + 4), 1,
                                    "Filter samples per shading sample (anti-aliasing samples add more).");
        const char* r[4] = { "1024", "2048", "4096", "8192" };
        _shadowRes = makeChoice(this, sh, "shadow_resolution", "resolution", std::vector<std::string>(r, r + 4), 1,
                                "Shadow map size. Raise it for large scenes with small details.");
        _bias = makeDouble(this, sh, "shadow_bias", "bias", 1, 0.1, 0., 5., "Pushes the shadow away from the surface (fixes dark speckles on lit surfaces).");
    }
    makeDivider(this, page, "xform_divider");
    _xf.create(this, page, 4);
}

void
Solv3DLight::buildScene(double t, Scene* scene)
{
    std::shared_ptr<Light> L = std::make_shared<Light>();
    L->kind = (_kind == eSpot) ? "spot" : (_kind == eDirectional) ? "distant" : (_kind == eAmbient) ? "ambient" : "point";
    KnobColorPtr c = _color.lock();
    L->color = Vec3( c->getValueAtTime(t, 0), c->getValueAtTime(t, 1), c->getValueAtTime(t, 2) );
    L->intensity = _intensity.lock()->getValueAtTime(t);
    if ( _falloff.lock() ) {
        L->falloff = _falloff.lock()->getValueAtTime(t);
    }
    if ( _cone.lock() ) {
        L->coneAngle = _cone.lock()->getValueAtTime(t);
        L->conePenumbra = _penumbra.lock()->getValueAtTime(t);
    }
    if ( _castShadows.lock() ) {
        L->castShadows = _castShadows.lock()->getValueAtTime(t);
        L->shadowSoftness = _softness.lock()->getValueAtTime(t);
        L->shadowSamples = 8 << _shadowQuality.lock()->getValueAtTime(t);
        L->shadowResolution = 1024 << _shadowRes.lock()->getValueAtTime(t);
        L->shadowBias = _bias.lock()->getValueAtTime(t);
    }
    Prim& p = scene->define(primPath(), PrimType::Light);
    p.light = L;
    p.local = _xf.matrix(t);
    p.display = _display.lock()->getValueAtTime(t);
    p.node = nodeName();
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
    _preset = makePresetChoice(this, proj);
    KnobGroupPtr fr = makeGroup(this, proj, "frustum", "Frustum", true);
    // defaults: 35 mm lens on a full frame film back (Maya default camera)
    _focal = makeDouble(this, fr, "focal", "focal length", 1, 35., 5., 200., "mm");
    _hAperture = makeDouble(this, fr, "haperture", "horiz aperture", 1, 36., 1., 70., "Film back width (mm). The image width fits it.");
    _vAperture = makeDouble(this, fr, "vaperture", "vert aperture", 1, 24., 1., 70., "Film back height (mm), kept for export: the image height follows the format.");
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

bool
Solv3DCamera::knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec /*view*/, double /*time*/, bool /*mainThread*/)
{
    if ( (k == _preset.lock().get()) && (reason != eValueChangedReasonTimeChanged) ) {
        const int i = _preset.lock()->getValue();
        if ( (i > 0) && (i < kCameraPresetCount) ) {
            const CameraPreset& p = kCameraPresets[i];
            _hAperture.lock()->setValue(p.width);
            _vAperture.lock()->setValue(p.height);
            if (p.focal > 0.) {
                _focal.lock()->setValue(p.focal);
            }
        }

        return true;
    }

    return false;
}

// ============================================================================ VanishingCamera
static const char* kLineTypes[7] = { "parallel X", "parallel Y", "parallel Z", "on X axis", "on Y axis", "on Z axis", "off" };

std::string
Solv3DVanishingCamera::getPluginDescription() const
{
    return "Finds a camera from a still plate with lines. Ctrl+drag on the plate (or 'add line') draws a line; set its type: "
           "parallel to the X, Y or Z axis of the scene, or ON an axis (its arrow points to + and where the on-axis lines cross "
           "is the origin; a length on one of them sets the scale). Needs lines on two axes: known focal 3 lines, approximate "
           "focal 3, free focal 4 (two parallel on one axis). Y points up and X to the right automatically; flip Y / flip Z "
           "switch to the other orientations that fit the same lines. 'create' makes a Camera, linked or baked. "
           "The plate passes through unchanged.";
}

void
Solv3DVanishingCamera::initializeKnobs()
{
    KnobPagePtr vp = makePage(this, "VanishingCamera");
    _preset = makePresetChoice(this, vp);
    _filmBack = makeDouble(this, vp, "haperture", "film back width", 1, 36., 1., 70., "Horizontal aperture (mm) of the camera.");
    _filmBack.lock()->setEvaluateOnChange(false);
    const char* fm[3] = { "solve", "known", "approximate" };
    _focalMode = makeChoice(this, vp, "focal_mode", "focal", std::vector<std::string>(fm, fm + 3), 2,
                            "solve: from the lines (4+ lines). known: locked to 'focal length'. approximate: stays near 'focal length' "
                            "within 'tolerance' (best with few or imprecise lines).", false);
    _focalMode.lock()->setEvaluateOnChange(false);
    _focal = makeDouble(this, vp, "focal_guess", "", 1, 35., 5., 200., "Focal length (mm) for known / approximate.");
    _focal.lock()->setAddNewLine(false);
    _focal.lock()->setEvaluateOnChange(false);
    _tolerance = makeDouble(this, vp, "focal_tolerance", "tolerance", 1, 10., 0.5, 50., "Approximate: how far (mm) the solve may move from the focal length.");
    _tolerance.lock()->setEvaluateOnChange(false);

    makeDivider(this, vp, "lines_divider");
    KnobButtonPtr add = AppManager::createKnob<KnobButton>( this, std::string("add line") );
    add->setName("add_line");
    add->setHintToolTip( std::string("Adds a line in the middle of the plate (or Ctrl+drag on the plate).") );
    add->setAddNewLine(false);
    vp->addKnob(add);
    _addLine = add;
    _drawMode = makeBool(this, vp, "draw_lines", "draw lines", false,
                         "On: click-drag on an empty part of the plate draws a new line (no Ctrl needed). Turn it off to "
                         "only move existing lines.", false);
    _drawMode.lock()->setEvaluateOnChange(false);
    KnobStringPtr hint = AppManager::createKnob<KnobString>( this, std::string("") );
    hint->setName("lines_hint");
    hint->setAsLabel();
    hint->setDefaultValue("Draw: 'draw lines' on, or Ctrl+drag. Red X, green Y, blue Z; arrows on on-axis lines.");
    hint->setIsPersistent(false);
    vp->addKnob(hint);
    for (int i = 0; i < kMaxLines; ++i) {
        const std::string n = std::to_string(i + 1);
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, "line " + n + " points", 4 );
        k->setName("line" + n);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setSecretByDefault(true);
        vp->addKnob(k);
        _lines[i] = k;
        addOverlaySlaveParam(k);
        KnobBoolPtr on = AppManager::createKnob<KnobBool>( this, "line " + n + " on" );
        on->setName("line" + n + "_on");
        on->setEvaluateOnChange(false);
        on->setSecretByDefault(true);
        vp->addKnob(on);
        _lineOn[i] = on;
        KnobChoicePtr ty = makeChoice(this, vp, ("line" + n + "_type").c_str(), ("line " + n).c_str(),
                                      std::vector<std::string>(kLineTypes, kLineTypes + 7), i % 3,
                                      "parallel: the edge is parallel to that axis of the scene. on axis: the edge lies on it (arrow = +).", false);
        ty->setEvaluateOnChange(false);
        ty->setSecretByDefault(true);
        _lineType[i] = ty;
        addOverlaySlaveParam(ty);
        KnobButtonPtr sw = AppManager::createKnob<KnobButton>( this, std::string("<->") );
        sw->setName("line" + n + "_swap");
        sw->setHintToolTip( std::string("Swap the ends (arrow direction).") );
        sw->setAddNewLine(false);
        sw->setSecretByDefault(true);
        vp->addKnob(sw);
        _lineSwap[i] = sw;
        KnobDoublePtr len = AppManager::createKnob<KnobDouble>( this, std::string("length") );
        len->setName("line" + n + "_length");
        len->setDefaultValue(0.);
        len->setDisplayMinimum(0.);
        len->setDisplayMaximum(1000.);
        len->setAnimationEnabled(false);
        len->setEvaluateOnChange(false);
        len->setHintToolTip( std::string("Real length of this on-axis line in scene units (1 = 1 cm). Sets the scale. 0 = unknown.") );
        len->setSecretByDefault(true);
        len->setAddNewLine(false);
        vp->addKnob(len);
        _lineLen[i] = len;
        KnobButtonPtr del = AppManager::createKnob<KnobButton>( this, std::string("x") );
        del->setName("line" + n + "_delete");
        del->setHintToolTip( std::string("Delete the line.") );
        del->setSecretByDefault(true);
        vp->addKnob(del);
        _lineDel[i] = del;
    }

    makeDivider(this, vp, "origin_divider");
    _useOrigin = makeBool(this, vp, "use_origin", "origin point", false,
                          "Place the origin on this point of the plate (drag the cross). Off: where the on-axis lines cross, "
                          "else the start of the first line.", false);
    _useOrigin.lock()->setEvaluateOnChange(false);
    addOverlaySlaveParam( _useOrigin.lock() );
    KnobDoublePtr org = AppManager::createKnob<KnobDouble>( this, std::string(""), 2 );
    org->setName("origin");
    org->setAnimationEnabled(false);
    org->setEvaluateOnChange(false);
    vp->addKnob(org);
    _origin = org;
    addOverlaySlaveParam(org);
    _distance = makeDouble(this, vp, "distance", "distance", 1, 500., 1., 5000.,
                           "Distance from the camera to the origin when no line has a length (scene units, 1 = 1 cm).");
    _distance.lock()->setEvaluateOnChange(false);
    _flipY = makeBool(this, vp, "flip_y", "flip Y", false, "Upside down <-> upright (180 deg around the Z axis).", false);
    _flipZ = makeBool(this, vp, "flip_z", "flip Z", false, "Facing the scene <-> facing away (180 deg around the Y axis).");
    _flipY.lock()->setEvaluateOnChange(false);
    _flipZ.lock()->setEvaluateOnChange(false);
    KnobButtonPtr solve = AppManager::createKnob<KnobButton>( this, std::string("solve") );
    solve->setName("solve");
    solve->setHintToolTip( std::string("Compute focal length, rotation and position from the lines.") );
    vp->addKnob(solve);
    _solve = solve;
    KnobStringPtr st = AppManager::createKnob<KnobString>( this, std::string("status") );
    st->setName("status");
    st->setAsLabel();
    vp->addKnob(st);
    _status = st;

    KnobGroupPtr res = makeGroup(this, vp, "result", "Result", true);
    _rFocal = makeDouble(this, res, "focal", "focal length", 1, 35., 5., 200., "mm (solved)");
    _rVAperture = makeDouble(this, res, "vaperture", "vert aperture", 1, 24., 1., 70., "mm (film back width / plate aspect)");
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

int
Solv3DVanishingCamera::activeLineCount() const
{
    int n = 0;
    for (int i = 0; i < kMaxLines; ++i) {
        if ( _lineOn[i].lock()->getValue() ) {
            ++n;
        }
    }

    return n;
}

void
Solv3DVanishingCamera::refreshLineRows()
{
    for (int i = 0; i < kMaxLines; ++i) {
        const bool on = _lineOn[i].lock()->getValue();
        _lineType[i].lock()->setSecret(!on);
        _lineSwap[i].lock()->setSecret(!on);
        _lineDel[i].lock()->setSecret(!on);
        const int ty = _lineType[i].lock()->getValue();
        _lineLen[i].lock()->setSecret( !on || (ty < 3) || (ty > 5) );
    }
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
Solv3DVanishingCamera::addLine(double x1, double y1, double x2, double y2)
{
    for (int i = 0; i < kMaxLines; ++i) {
        if ( _lineOn[i].lock()->getValue() ) {
            continue;
        }
        KnobDoublePtr k = _lines[i].lock();
        k->setValue(x1, ViewSpec::all(), 0);
        k->setValue(y1, ViewSpec::all(), 1);
        k->setValue(x2, ViewSpec::all(), 2);
        k->setValue(y2, ViewSpec::all(), 3);
        // cycle the default type through X, Z, Y so the first lines are usable at once
        const int order[3] = { 0, 2, 1 };
        _lineType[i].lock()->setValue( order[activeLineCount() % 3] );
        _lineLen[i].lock()->setValue(0.);
        _lineOn[i].lock()->setValue(true);
        _selLine = i;
        refreshLineRows();

        return;
    }
    _status.lock()->setValue("All 16 lines are used: delete one first.");
}

bool
Solv3DVanishingCamera::knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec /*view*/, double /*time*/, bool /*mainThread*/)
{
    if ( k == _solve.lock().get() ) {
        solveFromLines();

        return true;
    }
    if ( (k == _flipY.lock().get()) || (k == _flipZ.lock().get()) ) {
        if (reason == eValueChangedReasonUserEdited) {
            solveFromLines();
        }

        return true;
    }
    if ( k == _addLine.lock().get() ) {
        RectD f;
        plateFormat(&f);
        addLine(f.x1 + f.width() * 0.35, f.y1 + f.height() * 0.5, f.x1 + f.width() * 0.65, f.y1 + f.height() * 0.5);

        return true;
    }
    if ( k == _export.lock().get() ) {
        exportCamera();

        return true;
    }
    if ( (k == _preset.lock().get()) && (reason != eValueChangedReasonTimeChanged) ) {
        const int i = _preset.lock()->getValue();
        if ( (i > 0) && (i < kCameraPresetCount) ) {
            _filmBack.lock()->setValue(kCameraPresets[i].width);
            if (kCameraPresets[i].focal > 0.) {
                _focal.lock()->setValue(kCameraPresets[i].focal);
                if (_focalMode.lock()->getValue() == 0) {
                    _focalMode.lock()->setValue(2); // phone lens known roughly: approximate
                }
            }
        }

        return true;
    }
    for (int i = 0; i < kMaxLines; ++i) {
        if ( k == _lineSwap[i].lock().get() ) {
            KnobDoublePtr l = _lines[i].lock();
            const double v[4] = { l->getValue(0), l->getValue(1), l->getValue(2), l->getValue(3) };
            l->setValue(v[2], ViewSpec::all(), 0);
            l->setValue(v[3], ViewSpec::all(), 1);
            l->setValue(v[0], ViewSpec::all(), 2);
            l->setValue(v[1], ViewSpec::all(), 3);

            return true;
        }
        if ( k == _lineDel[i].lock().get() ) {
            _lineOn[i].lock()->setValue(false);
            if (_selLine == i) {
                _selLine = -1;
            }
            refreshLineRows();

            return true;
        }
        if ( (k == _lineType[i].lock().get()) || (k == _lineOn[i].lock().get()) ) {
            refreshLineRows();

            return true;
        }
    }

    return false;
}

void
Solv3DVanishingCamera::solveFromLines()
{
    RectD f;
    plateFormat(&f);
    SolvAlign::Input in;
    for (int i = 0; i < kMaxLines; ++i) {
        if ( !_lineOn[i].lock()->getValue() ) {
            continue;
        }
        const int ty = _lineType[i].lock()->getValue();
        if (ty == 6) {
            continue; // off
        }
        KnobDoublePtr k = _lines[i].lock();
        SolvAlign::Line l;
        l.x1 = k->getValue(0); l.y1 = k->getValue(1); l.x2 = k->getValue(2); l.y2 = k->getValue(3);
        l.axis = ty % 3;
        l.onAxis = ty >= 3;
        l.length = l.onAxis ? _lineLen[i].lock()->getValue() : 0.;
        in.lines.push_back(l);
    }
    const double fb = std::max(0.01, _filmBack.lock()->getValue() );
    in.cx = (f.x1 + f.x2) * 0.5;
    in.cy = (f.y1 + f.y2) * 0.5;
    in.pxPerMM = f.width() / fb;
    in.focalMode = _focalMode.lock()->getValue();
    in.focalMM = _focal.lock()->getValue();
    in.tolMM = _tolerance.lock()->getValue();
    in.hasOrigin = _useOrigin.lock()->getValue();
    in.ox = _origin.lock()->getValue(0);
    in.oy = _origin.lock()->getValue(1);
    in.distance = _distance.lock()->getValue();
    in.flipY = _flipY.lock()->getValue();
    in.flipZ = _flipZ.lock()->getValue();
    const SolvAlign::Output r = SolvAlign::solve(in);
    if (!r.ok) {
        _status.lock()->setValue("Cannot solve: " + r.error);

        return;
    }
    beginChanges();
    KnobDoublePtr t = _rTranslate.lock(), ro = _rRotate.lock();
    t->setValue(r.camPos.x, ViewSpec::all(), 0); t->setValue(r.camPos.y, ViewSpec::all(), 1); t->setValue(r.camPos.z, ViewSpec::all(), 2);
    ro->setValue(r.rotateXYZ.x, ViewSpec::all(), 0); ro->setValue(r.rotateXYZ.y, ViewSpec::all(), 1); ro->setValue(r.rotateXYZ.z, ViewSpec::all(), 2);
    _rFocal.lock()->setValue(r.focalMM);
    _rVAperture.lock()->setValue( fb * f.height() / std::max(1e-9, f.width()) );
    endChanges();
    const double toDeg = 180. / 3.14159265358979323846;
    std::stringstream ss;
    ss.precision(2);
    ss << std::fixed << "solved: focal " << r.focalMM << " mm, field of view " << 2. * std::atan( fb / (2. * r.focalMM) ) * toDeg
       << " deg, error " << r.rmsPx << " px" << (r.rmsPx > 3. ? " (high: check the lines and their types)" : "");
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
        { "haperture", _filmBack.lock(), 1 }, { "vaperture", _rVAperture.lock(), 1 }
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

static const float kAxisCol[4][3] = { { 0.93f, 0.33f, 0.31f }, { 0.4f, 0.85f, 0.42f }, { 0.36f, 0.55f, 1.f }, { 0.6f, 0.6f, 0.6f } };

void
Solv3DVanishingCamera::drawOverlay(double /*time*/, const RenderScale & /*renderScale*/, ViewIdx /*view*/)
{
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
    for (int i = 0; i < kMaxLines; ++i) {
        if ( !_lineOn[i].lock()->getValue() ) {
            continue;
        }
        const int ty = _lineType[i].lock()->getValue();
        const int axis = (ty == 6) ? 3 : ty % 3;
        const bool onAxis = (ty >= 3) && (ty <= 5);
        KnobDoublePtr k = _lines[i].lock();
        const float* c = kAxisCol[axis];
        const double x1 = k->getValue(0), y1 = k->getValue(1), x2 = k->getValue(2), y2 = k->getValue(3);
        const double dx = x2 - x1, dy = y2 - y1;
        // extended guide (dim) so the convergence is visible
        glLineWidth(1.f);
        glColor4f(c[0], c[1], c[2], 0.3f);
        glBegin(GL_LINES);
        glVertex2d(x1 - dx * 3., y1 - dy * 3.);
        glVertex2d(x2 + dx * 3., y2 + dy * 3.);
        glEnd();
        const bool sel = (i == _selLine);
        glLineWidth(onAxis ? 3.f : 2.f);
        glColor4f(c[0], c[1], c[2], 1.f);
        glBegin(GL_LINES);
        glVertex2d(x1, y1);
        glVertex2d(x2, y2);
        glEnd();
        if (onAxis) {
            // arrow head at the + end
            const double len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-9) {
                const double ux = dx / len, uy = dy / len, s = 12. * psx;
                glBegin(GL_TRIANGLES);
                glVertex2d(x2, y2);
                glVertex2d(x2 - ux * s - uy * s * 0.5, y2 - uy * s + ux * s * 0.5);
                glVertex2d(x2 - ux * s + uy * s * 0.5, y2 - uy * s - ux * s * 0.5);
                glEnd();
            }
        }
        glPointSize(sel ? 9.f : 7.f);
        glBegin(GL_POINTS);
        for (int e = 0; e < 2; ++e) {
            if ( (_drag == i * 2 + e) || sel ) {
                glColor4f(1.f, 0.82f, 0.34f, 1.f);
            } else {
                glColor4f(c[0], c[1], c[2], 1.f);
            }
            glVertex2d(e ? x2 : x1, e ? y2 : y1);
        }
        glEnd();
    }
    if ( _useOrigin.lock()->getValue() ) {
        const double ox = _origin.lock()->getValue(0), oy = _origin.lock()->getValue(1), s = 10. * psx;
        glLineWidth(2.f);
        glColor4f(1.f, 0.82f, 0.34f, 1.f);
        glBegin(GL_LINES);
        glVertex2d(ox - s, oy); glVertex2d(ox + s, oy);
        glVertex2d(ox, oy - s); glVertex2d(ox, oy + s);
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
    _drag = -1;
    if ( _useOrigin.lock()->getValue() &&
         (std::hypot( pos.x() - _origin.lock()->getValue(0), pos.y() - _origin.lock()->getValue(1) ) < tol) ) {
        _drag = -2;

        return true;
    }
    double best = tol;
    for (int i = 0; i < kMaxLines; ++i) {
        if ( !_lineOn[i].lock()->getValue() ) {
            continue;
        }
        KnobDoublePtr k = _lines[i].lock();
        for (int e = 0; e < 2; ++e) {
            const double d = std::hypot( pos.x() - k->getValue(e * 2), pos.y() - k->getValue(e * 2 + 1) );
            if (d < best) {
                best = d;
                _drag = i * 2 + e;
            }
        }
    }
    if (_drag >= 0) {
        _selLine = _drag / 2;

        return true;
    }
    // draw mode, or Ctrl + drag on empty plate: new line
    if ( _drawMode.lock()->getValue() || _ctrl ) {
        addLine( pos.x(), pos.y(), pos.x(), pos.y() );
        if (_selLine >= 0) {
            _drag = _selLine * 2 + 1;
        }

        return true;
    }

    return false;
}

bool
Solv3DVanishingCamera::onOverlayPenMotion(double, const RenderScale &, ViewIdx, const QPointF &, const QPointF & pos, double, double)
{
    if (_drag == -2) {
        _origin.lock()->setValue(pos.x(), ViewSpec::all(), 0);
        _origin.lock()->setValue(pos.y(), ViewSpec::all(), 1);

        return true;
    }
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
    if (_drag == -1) {
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

void
Solv3DSceneNode::refreshInputVisibility()
{
    NodePtr node = getNode();
    if (!node) {
        return;
    }
    // connected inputs + the first free one stay visible
    int lastConnected = -1;
    for (int i = 0; i < getNInputs(); ++i) {
        if ( node->getInput(i) ) {
            lastConnected = i;
        }
    }
    bool freeShown = false;
    for (int i = 0; i < getNInputs(); ++i) {
        bool vis = true;
        if ( !node->getInput(i) ) {
            vis = !freeShown && (i > lastConnected || i == 0);
            if (vis) {
                freeShown = true;
            }
            // gaps below the last connected input stay hidden
        }
        try {
            if (node->isInputVisible(i) != vis) {
                node->setInputVisible(i, vis);
            }
        } catch (const std::exception&) {
        }
    }
}

void
Solv3DSceneNode::onInputChanged(int /*inputNo*/)
{
    refreshInputVisibility();
}

void
Solv3DSceneNode::onEffectCreated(bool /*mayCreateFileDialog*/, const CreateNodeArgs& /*args*/)
{
    refreshInputVisibility();
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
    return std::string("Reads geometry: OBJ (meshes with uv / normals), ") +
           ( alembicAvailable() ? "Alembic (.abc: animated meshes, xforms and cameras, read at the current frame)" : "Alembic (not in this build)" ) +
           ( usdAvailable() ? " and USD (.usd .usda .usdc .usdz: meshes, cameras, lights, animation, UsdPreviewSurface values; "
                              "converted to 1 unit = 1 cm, Y up)." : " and USD (bridge not installed)." ) +
           " The img input is used as the texture of all its meshes.";
}

void
Solv3DReadGeo::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "ReadGeo");
    _geo.create(this, page);
    makeDivider(this, page, "geo_divider");
    KnobFilePtr f = AppManager::createKnob<KnobFile>( this, std::string("file") );
    f->setName("file");
    f->setHintToolTip( std::string(".obj, .abc, .usd, .usda, .usdc or .usdz file") );
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
    const bool isUsd = (lower.find(".usd") != std::string::npos) && (lower.rfind(".usd") >= lower.size() - 5);
    const double ft = t + _frameOffset.lock()->getValueAtTime(t);
    Scene loaded;
    {
        std::lock_guard<std::mutex> l(_cache->m);
        if ( (_cache->file != file) || ( (isAbc || isUsd) && (_cache->time != ft) ) ) {
            _cache->scene = Scene();
            _cache->error.clear();
            bool ok;
            if (isUsd) {
                ok = readUsd(file, ft, "/geo", &_cache->scene, &_cache->error);
            } else if (isAbc) {
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
    return "Writes the 3D input to a file: .usd / .usdc (binary USD), .usda (text USD), .usdz (packed USD) or .abc (Alembic, "
           "current frame). USD files can hold the animation of a frame range (meshes, cameras, lights, transforms, "
           "UsdPreviewSurface values), 1 unit = 1 cm, Y up. Press 'write'.";
}

void
Solv3DWriteGeo::initializeKnobs()
{
    KnobPagePtr page = makePage(this, "WriteGeo");
    KnobOutputFilePtr f = AppManager::createKnob<KnobOutputFile>( this, std::string("file") );
    f->setName("file");
    f->setHintToolTip( std::string(".usd, .usdc, .usda, .usdz or .abc") );
    f->setAnimationEnabled(false);
    page->addKnob(f);
    _file = f;
    const char* rg[3] = { "current frame", "project range", "custom" };
    _range = makeChoice(this, page, "frame_range", "frame range", std::vector<std::string>(rg, rg + 3), 0,
                        "USD: the frames written as animation. Alembic always writes the current frame.");
    KnobIntPtr fi = AppManager::createKnob<KnobInt>( this, std::string("first") );
    fi->setName("first_frame");
    fi->setDefaultValue(1);
    fi->disableSlider();
    fi->setAddNewLine(false);
    page->addKnob(fi);
    _first = fi;
    KnobIntPtr la = AppManager::createKnob<KnobInt>( this, std::string("last") );
    la->setName("last_frame");
    la->setDefaultValue(100);
    la->disableSlider();
    page->addKnob(la);
    _last = la;
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
        setPersistentMessage(eMessageTypeError, "Set the output file first (.usd, .usdc, .usda, .usdz or .abc).");

        return true;
    }
    std::string lower = file;
    std::transform( lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); } );
    auto ends = [&](const char* e) {
        const size_t n = std::strlen(e);

        return (lower.size() > n) && (lower.compare(lower.size() - n, n, e) == 0);
    };
    std::string err;
    bool ok;
    if ( ends(".abc") ) {
        Scene s;
        buildScene(time, &s);
        ok = writeAlembic(file, s, &err);
    } else if ( ends(".usda") && !usdAvailable() ) {
        // built-in text writer (current frame) when the bridge is not installed
        Scene s;
        buildScene(time, &s);
        std::ofstream o( file.c_str() );
        o << s.toUsda(0.01); // 1 unit = 1 cm
        ok = (bool)o;
        if (!ok) {
            err = "cannot write " + file;
        }
    } else {
        double first = time, last = time;
        const int rg = _range.lock()->getValue();
        if (rg == 1) {
            getApp()->getProject()->getFrameRange(&first, &last);
        } else if (rg == 2) {
            first = _first.lock()->getValue();
            last = std::max(first, (double)_last.lock()->getValue() );
        }
        std::vector<Scene> scenes;
        for (double f = first; f <= last + 1e-9; f += 1.) {
            scenes.push_back( Scene() );
            buildScene(f, &scenes.back() );
        }
        std::vector<std::pair<double, const Scene*> > frames;
        double f = first;
        for (const Scene& sc : scenes) {
            frames.push_back( std::make_pair(f, &sc) );
            f += 1.;
        }
        ok = writeUsd( file, frames, std::max( 1., getApp()->getProjectFrameRate() ), &err );
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
    makeDivider(this, ms, "mb_divider");
    KnobIntPtr mbs = AppManager::createKnob<KnobInt>( this, std::string("motion blur samples") );
    mbs->setName("motion_blur_samples");
    mbs->setDefaultValue(1);
    mbs->setDisplayMinimum(1);
    mbs->setDisplayMaximum(64);
    mbs->setMinimum(1);
    mbs->setHintToolTip( std::string("Time samples across the shutter. 1 = no motion blur (default). 8-16 for most shots, "
                                     "32-64 for fast motion. The scene is evaluated at up to 9 times and interpolated in "
                                     "between, and the samples are shared with the anti-aliasing samples, so the cost is "
                                     "about max(samples, motion blur samples) passes.") );
    ms->addKnob(mbs);
    _mbSamples = mbs;
    _shutter = makeDouble(this, ms, "shutter", "shutter", 1, 0.5, 0., 2., "Shutter length in frames (0.5 = 180 degrees).");
    const char* so[4] = { "centred", "start", "end", "custom" };
    _shutterOffset = makeChoice(this, ms, "shutter_offset", "shutter offset", std::vector<std::string>(so, so + 4), 0,
                                "centred: around the frame. start: from the frame on. end: up to the frame. custom: starts at 'offset'.", false);
    _shutterCustom = makeDouble(this, ms, "shutter_custom_offset", "", 1, 0., -1., 1., "Custom shutter start (frames, relative to the frame).");

    // AOVs
    KnobPagePtr aovp = makePage(this, "AOVs");
    KnobStringPtr ai = AppManager::createKnob<KnobString>( this, std::string("aov_info") );
    ai->setName("aov_info");
    ai->setAsLabel();
    ai->setDefaultValue("Extra layers next to rgba (viewer layer menu, Shuffle): depth.Z and shadow.A.");
    ai->setIsPersistent(false);
    aovp->addKnob(ai);
    const char* dm[2] = { "1/z (far = 0)", "distance" };
    _depthMode = makeChoice(this, aovp, "depth_mode", "depth", std::vector<std::string>(dm, dm + 2), 0,
                            "1/z: 0 at infinity, the usual convention for defocus (ZDefocus math 'depth / far = 0'). "
                            "distance: along the camera axis, in scene units.");
}

const ImagePlaneDesc&
Solv3DScanline::depthPlane()
{
    static const char* ch[1] = { "Z" };
    static const ImagePlaneDesc p("depth", "depth", "Z", ch, 1);

    return p;
}

const ImagePlaneDesc&
Solv3DScanline::shadowPlane()
{
    static const char* ch[1] = { "A" };
    static const ImagePlaneDesc p("shadow", "shadow", "A", ch, 1);

    return p;
}

void
Solv3DScanline::getComponentsNeededAndProduced(double time, ViewIdx view, EffectInstance::ComponentsNeededMap* comps,
                                               double* passThroughTime, int* passThroughView, int* passThroughInput)
{
    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInput = -1;
    std::list<ImagePlaneDesc>& outp = (*comps)[-1];
    outp.push_back( ImagePlaneDesc::getRGBAComponents() );
    outp.push_back( depthPlane() );
    outp.push_back( shadowPlane() );
    (*comps)[2].push_back( ImagePlaneDesc::getRGBAComponents() );
    (*comps)[3].push_back( ImagePlaneDesc::getRGBAComponents() );
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
    // scene at time tt (+ the cam input next to it, in its own namespace)
    auto sceneAt = [&](double tt, Scene* sc, std::string* cp) {
        Scene camScene;
        inputScene(this, 0, tt, sc);
        cp->clear();
        if ( inputScene(this, 1, tt, &camScene) ) {
            *cp = camScene.firstCamera();
            Scene ns = camScene.reparented("/__cam");
            sc->merge(ns);
            *cp = cp->empty() ? std::string() : "/__cam" + *cp;
        }
        if ( cp->empty() ) {
            *cp = sc->firstCamera();
        }
    };
    Scene scene;
    std::string camPath;
    sceneAt(t, &scene, &camPath);
    // motion blur: the scene at up to 9 times across the shutter (interpolated in between)
    const int mbSamples = std::max( 1, _mbSamples.lock()->getValueAtTime(t) );
    std::vector<Scene> mbScenes;
    if (mbSamples > 1) {
        const double sh = _shutter.lock()->getValueAtTime(t);
        const int om = _shutterOffset.lock()->getValueAtTime(t);
        const double start = (om == 0) ? -sh * 0.5 : (om == 1) ? 0. : (om == 2) ? -sh : _shutterCustom.lock()->getValueAtTime(t);
        const int nSteps = std::min(mbSamples, 9);
        mbScenes.resize(nSteps);
        for (int i = 0; i < nSteps; ++i) {
            const double tt = t + start + sh * (nSteps > 1 ? (double)i / (nSteps - 1) : 0.5);
            std::string cp;
            sceneAt(tt, &mbScenes[i], &cp);
        }
    }
    // textures: fetch every referenced upstream image once
    std::map<const void*, std::shared_ptr<TexImage> > texCache;
    auto fetchTex = [&](const std::shared_ptr<void>& h) {
        Solv3DTexRef* ref = static_cast<Solv3DTexRef*>( h.get() );
        if (!ref) {
            return;
        }
        EffectInstancePtr e = ref->effect.lock();
        // keyed by the image node: every motion blur step makes its own handles
        if ( !e || texCache.count( e.get() ) ) {
            return;
        }
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
        texCache[e.get()] = ti;
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
    st.timeSamples = mbSamples;
    // which planes are asked for
    bool wantDepth = false, wantShadow = false;
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin(); it != args.outputPlanes.end(); ++it) {
        wantDepth = wantDepth || (it->first.getPlaneID() == depthPlane().getPlaneID());
        wantShadow = wantShadow || (it->first.getPlaneID() == shadowPlane().getPlaneID());
    }
    std::vector<float> buf, depthBuf, shadowBuf;
    ScanlineAOV aov;
    aov.depth = wantDepth ? &depthBuf : 0;
    aov.depthInverse = _depthMode.lock()->getValueAtTime(t) == 0;
    aov.shadow = wantShadow ? &shadowBuf : 0;
    std::vector<const Scene*> steps;
    if ( mbScenes.empty() ) {
        steps.push_back(&scene);
    } else {
        for (const Scene& sc : mbScenes) {
            steps.push_back(&sc);
        }
    }
    scanlineRenderSteps(steps, camPath, st,
                        [&](const std::shared_ptr<void>& h) -> const TexImage* {
                            Solv3DTexRef* ref = static_cast<Solv3DTexRef*>( h.get() );
                            EffectInstancePtr e = ref ? ref->effect.lock() : EffectInstancePtr();
                            std::map<const void*, std::shared_ptr<TexImage> >::const_iterator it = texCache.find( e.get() );
                            return it == texCache.end() ? (const TexImage*)0 : it->second.get();
                        },
                        envTex.get(), &buf, aov);
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
        const bool isDepth = it->first.getPlaneID() == depthPlane().getPlaneID();
        const bool isShadow = it->first.getPlaneID() == shadowPlane().getPlaneID();
        if (isDepth || isShadow) {
            const std::vector<float>& src = isDepth ? depthBuf : shadowBuf;
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                float* d = (float*)wa.pixelAt(args.roi.x1, y);
                if (!d) {
                    continue;
                }
                for (int x = args.roi.x1; x < args.roi.x2; ++x, d += dnc) {
                    const size_t k = (size_t)(y - args.roi.y1) * st.width + (x - args.roi.x1);
                    const float v = k < src.size() ? src[k] : 0.f;
                    for (int c = 0; c < dnc; ++c) {
                        d[c] = v;
                    }
                }
            }
            continue;
        }
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
