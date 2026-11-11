/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX 3D nodes (built-in effects). A 3D node does not make an image: it describes a scene
// (buildScene). Nodes read the scenes of their 3D inputs and add / change prims. ScanlineRender turns
// a scene into an image; the 3D Viewer draws it. Textures are references to upstream image nodes
// (Solv3DTexRef), fetched by ScanlineRender at render time.

#ifndef SOLVFX_SOLV3DNODES_H
#define SOLVFX_SOLV3DNODES_H

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/Solv3D/SolvScene.h"

#define PLUGINID_SOLV3D_CARD           "com.aguapanelafx.solv3d.Card"
#define PLUGINID_SOLV3D_CUBE           "com.aguapanelafx.solv3d.Cube"
#define PLUGINID_SOLV3D_SPHERE         "com.aguapanelafx.solv3d.Sphere"
#define PLUGINID_SOLV3D_CYLINDER       "com.aguapanelafx.solv3d.Cylinder"
#define PLUGINID_SOLV3D_CAMERA         "com.aguapanelafx.solv3d.Camera"
#define PLUGINID_SOLV3D_TRANSFORMGEO   "com.aguapanelafx.solv3d.TransformGeo"
#define PLUGINID_SOLV3D_SCENE          "com.aguapanelafx.solv3d.Scene"
#define PLUGINID_SOLV3D_READGEO        "com.aguapanelafx.solv3d.ReadGeo"
#define PLUGINID_SOLV3D_WRITEGEO       "com.aguapanelafx.solv3d.WriteGeo"
#define PLUGINID_SOLV3D_PROJECT3D      "com.aguapanelafx.solv3d.Project3D"
#define PLUGINID_SOLV3D_MATERIAL       "com.aguapanelafx.solv3d.Material"
#define PLUGINID_SOLV3D_SCANLINE       "com.aguapanelafx.solv3d.ScanlineRender"
#define PLUGINID_SOLV3D_VANISHINGCAM   "com.aguapanelafx.solv3d.VanishingCamera"
#define PLUGINID_SOLV3D_POINTLIGHT     "com.aguapanelafx.solv3d.PointLight"
#define PLUGINID_SOLV3D_SPOTLIGHT      "com.aguapanelafx.solv3d.SpotLight"
#define PLUGINID_SOLV3D_DIRLIGHT       "com.aguapanelafx.solv3d.DirectLight"
#define PLUGINID_SOLV3D_AMBIENTLIGHT   "com.aguapanelafx.solv3d.AmbientLight"

NATRON_NAMESPACE_ENTER

// texture handle stored in Solv3D::Mesh::texture / Projection::texture
struct Solv3DTexRef
{
    EffectInstanceWPtr effect;
};

// transform knobs shared by the 3D nodes: transform order, rotation order, translate / rotate / scale /
// uniform scale / pivot translate / pivot rotate
struct Solv3DXformKnobs
{
    KnobChoiceWPtr order, rotOrder;
    KnobDoubleWPtr translate, rotate, scale, uniform, pivot, pivotRotate;

    void create(EffectInstance* fx, const KnobPagePtr& page, int defaultRotOrder = 4 /* ZXY */);
    Solv3D::Mat4 matrix(double time) const;
};

// display / render / selectable knobs of the geometry nodes
struct Solv3DGeoKnobs
{
    KnobChoiceWPtr display, render;
    KnobBoolWPtr selectable, castShadow, receiveShadow;

    void create(EffectInstance* fx, const KnobPagePtr& page);
    // stamps the settings and the owner node on every prim of the scene under 'root' ("" = all)
    void apply(Solv3D::Scene* scene, double time, const std::string& nodeName, const std::string& root) const;
};

class Solv3DNode
    : public EffectInstance
{
public:
    explicit Solv3DNode(NodePtr n);
    virtual ~Solv3DNode() {}

    // the scene this node outputs at 'time'
    virtual void buildScene(double time, Solv3D::Scene* scene) = 0;
    // false for image nodes that share the 3D helpers (VanishingCamera)
    virtual bool is3D() const { return true; }

    // scene of input 'inputNb' (walks through Dots / Stamps); false when not a 3D input
    static bool inputScene(EffectInstance* fx, int inputNb, double time, Solv3D::Scene* scene);
    static Solv3DNode* as3D(const EffectInstancePtr& e);

    // prim path of this node ("/<script name>")
    std::string primPath() const;
    std::string nodeName() const;

    // ---- EffectInstance boilerplate
    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE;
    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;
    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE WARN_UNUSED_RETURN { return eRenderSafetyFullySafeFrame; }
    virtual bool supportsTiles() const OVERRIDE WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool* r, bool* g, bool* b, bool* a) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale & scale, ViewIdx view, RectD* rod) OVERRIDE WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time, const RenderScale & scale, const RectD & outputRoD, const RectD & renderWindow, ViewIdx view, RoIMap* ret) OVERRIDE;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN; // transparent black

protected:
    void projectFormat(RectD* rod) const;
};

// Card / Cube / Sphere / Cylinder (one class, the shape is fixed per plugin)
class Solv3DPrimitive
    : public Solv3DNode
{
public:
    enum Kind { eCard = 0, eCube, eSphere, eCylinder };

    Solv3DPrimitive(NodePtr n, Kind k) : Solv3DNode(n), _kind(k) {}

    static EffectInstance* BuildCard(NodePtr n) { return new Solv3DPrimitive(n, eCard); }
    static EffectInstance* BuildCube(NodePtr n) { return new Solv3DPrimitive(n, eCube); }
    static EffectInstance* BuildSphere(NodePtr n) { return new Solv3DPrimitive(n, eSphere); }
    static EffectInstance* BuildCylinder(NodePtr n) { return new Solv3DPrimitive(n, eCylinder); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual std::string getInputLabel(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return "img"; }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;

private:
    Kind _kind;
    Solv3DXformKnobs _xf;
    Solv3DGeoKnobs _geo;
    KnobChoiceWPtr _orientation;
    KnobBoolWPtr _imageAspect;
    KnobDoubleWPtr _size;
    KnobIntWPtr _rows, _cols;
    KnobColorWPtr _color;
    KnobBoolWPtr _lit;
};

#define SOLV3D_PRIMITIVE_CLASS(NAME, KIND) \
    class NAME : public Solv3DPrimitive \
    { \
public: \
        explicit NAME(NodePtr n) : Solv3DPrimitive(n, KIND) {} \
        static EffectInstance* BuildEffect(NodePtr n) { return new NAME(n); } \
    };
SOLV3D_PRIMITIVE_CLASS(Solv3DCard, Solv3DPrimitive::eCard)
SOLV3D_PRIMITIVE_CLASS(Solv3DCube, Solv3DPrimitive::eCube)
SOLV3D_PRIMITIVE_CLASS(Solv3DSphere, Solv3DPrimitive::eSphere)
SOLV3D_PRIMITIVE_CLASS(Solv3DCylinder, Solv3DPrimitive::eCylinder)

class Solv3DLight
    : public Solv3DNode
{
public:
    enum Kind { ePoint = 0, eSpot, eDirectional, eAmbient };

    Solv3DLight(NodePtr n, Kind k) : Solv3DNode(n), _kind(k) {}

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual std::string getInputLabel(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return std::string(); }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;

private:
    Kind _kind;
    Solv3DXformKnobs _xf;
    KnobChoiceWPtr _display, _falloff, _shadowQuality, _shadowRes;
    KnobColorWPtr _color;
    KnobDoubleWPtr _intensity, _cone, _penumbra, _softness, _bias;
    KnobBoolWPtr _castShadows;
};

#define SOLV3D_LIGHT_CLASS(NAME, KIND) \
    class NAME : public Solv3DLight \
    { \
public: \
        explicit NAME(NodePtr n) : Solv3DLight(n, KIND) {} \
        static EffectInstance* BuildEffect(NodePtr n) { return new NAME(n); } \
    };
SOLV3D_LIGHT_CLASS(Solv3DPointLight, Solv3DLight::ePoint)
SOLV3D_LIGHT_CLASS(Solv3DSpotLight, Solv3DLight::eSpot)
SOLV3D_LIGHT_CLASS(Solv3DDirectLight, Solv3DLight::eDirectional)
SOLV3D_LIGHT_CLASS(Solv3DAmbientLight, Solv3DLight::eAmbient)

class Solv3DCamera
    : public Solv3DNode
{
public:
    explicit Solv3DCamera(NodePtr n) : Solv3DNode(n) {}

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DCamera(n); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_CAMERA; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "Camera"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual std::string getInputLabel(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return std::string(); }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;

private:
    Solv3DXformKnobs _xf;
    KnobChoiceWPtr _display, _projection, _preset;
    KnobDoubleWPtr _focal, _hAperture, _vAperture, _near, _far;
    KnobDoubleWPtr _winTranslate, _winScale, _winRoll, _focusDist, _fStop;
};

// 2D node: solves a camera from vanishing lines drawn on the plate; "export" creates a Camera
class Solv3DVanishingCamera
    : public Solv3DNode
{
public:
    explicit Solv3DVanishingCamera(NodePtr n) : Solv3DNode(n), _drag(-1), _selLine(-1), _ctrl(false) {}

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DVanishingCamera(n); }

    virtual bool is3D() const OVERRIDE FINAL { return false; }
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_VANISHINGCAM; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "VanishingCamera"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual std::string getInputLabel(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return "img"; }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double, Solv3D::Scene*) OVERRIDE FINAL {}
    virtual bool isIdentity(double time, const RenderScale & scale, const RectI & roi, ViewIdx view, double* inputTime, ViewIdx* inputView, int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale & scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time, const RenderScale & scale, const RectD & outputRoD, const RectD & renderWindow, ViewIdx view, RoIMap* ret) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;

    virtual bool hasOverlay() const OVERRIDE FINAL { return true; }
    virtual void onKnobsLoaded() OVERRIDE FINAL { refreshLineRows(); }
    virtual void drawOverlay(double time, const RenderScale & renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenDown(double time, const RenderScale & renderScale, ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure, double timestamp, PenType pen) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenMotion(double time, const RenderScale & renderScale, ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure, double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenUp(double time, const RenderScale & renderScale, ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure, double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;

    static const int kMaxLines = 16;

private:
    void plateFormat(RectD* f);
    void solveFromLines();
    void addLine(double x1, double y1, double x2, double y2);
    void refreshLineRows();
    void exportCamera();
    int activeLineCount() const;

    KnobChoiceWPtr _preset, _focalMode;
    KnobDoubleWPtr _filmBack, _focal, _tolerance, _distance;
    KnobButtonWPtr _addLine, _solve;
    // per line: end points (hidden), on (hidden), type, swap, delete, length
    KnobDoubleWPtr _lines[kMaxLines];
    KnobBoolWPtr _lineOn[kMaxLines];
    KnobChoiceWPtr _lineType[kMaxLines];
    KnobButtonWPtr _lineSwap[kMaxLines], _lineDel[kMaxLines];
    KnobDoubleWPtr _lineLen[kMaxLines];
    KnobBoolWPtr _useOrigin, _flipY, _flipZ, _drawMode;
    KnobDoubleWPtr _origin;
    KnobDoubleWPtr _rFocal, _rVAperture, _rTranslate, _rRotate; // results
    KnobStringWPtr _status;
    KnobChoiceWPtr _exportMode;
    KnobButtonWPtr _export;
    int _drag;                  // line * 2 + end, -2 = origin, -3 = drawing a new line
    int _selLine;
    bool _ctrl;
};

class Solv3DTransformGeo
    : public Solv3DNode
{
public:
    explicit Solv3DTransformGeo(NodePtr n) : Solv3DNode(n) {}

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DTransformGeo(n); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_TRANSFORMGEO; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "TransformGeo"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "Moves, rotates and scales everything in its 3D input."; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual std::string getInputLabel(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return "geo"; }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;

private:
    Solv3DXformKnobs _xf;
};

class Solv3DSceneNode
    : public Solv3DNode
{
public:
    explicit Solv3DSceneNode(NodePtr n) : Solv3DNode(n) {}

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DSceneNode(n); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_SCENE; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "Scene"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "Merges 3D inputs into one scene. Only the connected inputs and one free input arrow are shown: connect it and the next one appears (up to 32)."; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 32; }
    virtual std::string getInputLabel(int i) const OVERRIDE FINAL WARN_UNUSED_RETURN { return std::to_string(i + 1); }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onEffectCreated(bool mayCreateFileDialog, const CreateNodeArgs& args) OVERRIDE FINAL;

private:
    void refreshInputVisibility();
};

class Solv3DReadGeo
    : public Solv3DNode
{
public:
    explicit Solv3DReadGeo(NodePtr n);

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DReadGeo(n); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_READGEO; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "ReadGeo"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual std::string getInputLabel(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return "img"; }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;

private:
    struct Cache;
    std::shared_ptr<Cache> _cache;
    Solv3DXformKnobs _xf;
    Solv3DGeoKnobs _geo;
    KnobFileWPtr _file;
    KnobIntWPtr _frameOffset;
    KnobBoolWPtr _lit;
    KnobStringWPtr _info;
};

class Solv3DWriteGeo
    : public Solv3DNode
{
public:
    explicit Solv3DWriteGeo(NodePtr n) : Solv3DNode(n) {}

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DWriteGeo(n); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_WRITEGEO; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "WriteGeo"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual std::string getInputLabel(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return "geo"; }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;

private:
    KnobOutputFileWPtr _file;
    KnobButtonWPtr _write;
    KnobChoiceWPtr _range;
    KnobIntWPtr _first, _last;
};

class Solv3DProject3D
    : public Solv3DNode
{
public:
    explicit Solv3DProject3D(NodePtr n) : Solv3DNode(n) {}

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DProject3D(n); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_PROJECT3D; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "Project3D"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 3; }
    virtual std::string getInputLabel(int i) const OVERRIDE FINAL WARN_UNUSED_RETURN { return i == 0 ? "geo" : (i == 1 ? "cam" : "img"); }
    virtual bool isInputOptional(int i) const OVERRIDE FINAL WARN_UNUSED_RETURN { return i != 0; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;

private:
    KnobChoiceWPtr _projectOn, _occlusion;
    KnobBoolWPtr _crop;
    KnobIntWPtr _frameHold;
    KnobBoolWPtr _useHold;
};

class Solv3DMaterial
    : public Solv3DNode
{
public:
    explicit Solv3DMaterial(NodePtr n) : Solv3DNode(n) {}

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DMaterial(n); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_MATERIAL; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "Material"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual std::string getInputLabel(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return "geo"; }
    virtual bool isInputOptional(int) const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL;

private:
    KnobColorWPtr _base;
    KnobDoubleWPtr _emission, _metal, _spec, _rough, _ior;
};

class Solv3DScanline
    : public Solv3DNode
{
public:
    explicit Solv3DScanline(NodePtr n) : Solv3DNode(n) {}

    static EffectInstance* BuildEffect(NodePtr n) { return new Solv3DScanline(n); }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_SOLV3D_SCANLINE; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "ScanlineRender"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 4; }
    virtual std::string getInputLabel(int i) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isInputOptional(int i) const OVERRIDE FINAL WARN_UNUSED_RETURN { return i != 0; }
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void buildScene(double time, Solv3D::Scene* scene) OVERRIDE FINAL; // = its obj input (3D viewer)
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale & scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time, const RenderScale & scale, const RectD & outputRoD, const RectD & renderWindow, ViewIdx view, RoIMap* ret) OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL;
    // AOVs: rgba + depth + shadow planes
    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual EffectInstance::PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN { return EffectInstance::ePassThroughBlockNonRenderedPlanes; }
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view, EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime, int* passThroughView, int* passThroughInput) OVERRIDE FINAL;
    static const ImagePlaneDesc& depthPlane();
    static const ImagePlaneDesc& shadowPlane();

private:
    KnobChoiceWPtr _samples, _filter, _output, _projMode, _shutterOffset, _depthMode;
    KnobDoubleWPtr _filterWidth, _ambient, _shutter, _shutterCustom;
    KnobIntWPtr _overscan, _mbSamples;
    KnobBoolWPtr _texFilter, _transparency, _zBuffer;
};

NATRON_NAMESPACE_EXIT

#endif // SOLVFX_SOLV3DNODES_H
