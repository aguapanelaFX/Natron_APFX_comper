/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Noise: Nuke-like noise generator with Houdini-style noise types
// (Perlin, Simplex, Sparse Convolution, Alligator, Worley F1/F2/F2-F1, White),
// metrics, fractal modes (fBm, Turbulence, Ridged, Terrain, Hybrid Terrain),
// domain warp, seamless time loop and output shaping.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.H"

#include "APFXNoiseMath.h"

#ifndef OVERRIDE
#define OVERRIDE override
#endif
#ifndef FINAL
#define FINAL final
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace OFX;

namespace {

#define kPluginName "Noise"
#define kPluginGrouping "APFX"
#define kPluginDescription \
    "APFX Noise: procedural noise like Nuke's Noise node, with Houdini-style noise types.\n\n" \
    "Noise type picks the basic pattern, Fractal adds layers (octaves) on top of it.\n" \
    "Z moves through the noise (animate it, or use Z speed). With Loop, the animation " \
    "repeats seamlessly every Loop period frames.\n" \
    "If Source is connected, the noise is drawn into the chosen channels of the Source image."
#define kPluginIdentifier "com.aguapanelafx.APFX.Noise"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

#define kParamOutput "output"
#define kParamColor "colorNoise"
#define kParamType "type"
#define kParamMetric "metric"
#define kParamJitter "jitter"
#define kParamFractal "fractal"
#define kParamSize "size"
#define kParamZ "z"
#define kParamZSpeed "zSpeed"
#define kParamLoop "loop"
#define kParamLoopPeriod "loopPeriod"
#define kParamSeed "seed"
#define kParamOctaves "octaves"
#define kParamLacunarity "lacunarity"
#define kParamGain "gain"
#define kParamNyquist "nyquist"
#define kParamWarpAmount "warpAmount"
#define kParamWarpSize "warpSize"
#define kParamWarpOctaves "warpOctaves"
#define kParamGamma "gamma"
#define kParamContrast "contrast"
#define kParamOffset "offset"
#define kParamLow "low"
#define kParamHigh "high"
#define kParamInvert "invert"
#define kParamTranslate "translate"
#define kParamRotate "rotate"
#define kParamScale "scale"
#define kParamCenter "center"

enum OutputEnum
{
    eOutputRGBA = 0,
    eOutputRGB,
    eOutputAlpha
};

struct NoiseSettings
{
    APFXNoise::BasisParams bp;
    APFXNoise::FractalParams fp;
    int output = eOutputRGBA;
    bool color = false;
    double sizeX = 350., sizeY = 350.;
    double z = 0.;
    bool use4D = false;
    double loopA = 0., loopB = 0.; // 4D loop coordinates (z circle)
    uint32_t seed = 0;
    double warpAmount = 0., warpSize = 500.;
    int warpOctaves = 3;
    double gamma = 1., contrast = 1., offset = 0., low = 0., high = 1.;
    bool invert = false;
    // canonical position -> noise position (inverse transform)
    double cx = 0., cy = 0., tx = 0., ty = 0., cosR = 1., sinR = 0., sx = 1., sy = 1.;
};

class NoiseProcessor
    : public ImageProcessor
{
public:
    NoiseProcessor(ImageEffect& effect)
        : ImageProcessor(effect)
        , _src(NULL)
        , _par(1.)
    {
    }

    void setValues(const NoiseSettings& s,
                   const Image* src,
                   double par)
    {
        _s = s;
        _src = src;
        _par = par;
        // seeded lattice tables (one per independent noise)
        _perm = APFXNoise::Perm(s.seed * 8U);
        _permR = APFXNoise::Perm(s.seed * 8U + 1U);
        _permG = APFXNoise::Perm(s.seed * 8U + 2U);
        _permB = APFXNoise::Perm(s.seed * 8U + 3U);
        _permWarpX = APFXNoise::Perm(s.seed * 8U + 4U);
        _permWarpY = APFXNoise::Perm(s.seed * 8U + 5U);
    }

private:
    double shape(double v) const
    {
        if (_s.gamma > 0. && _s.gamma != 1.) {
            v = std::pow(std::max(0., v), 1. / _s.gamma);
        }
        v = (v - 0.5) * _s.contrast + 0.5 + _s.offset;
        v = _s.low + v * (_s.high - _s.low);
        if (_s.invert) {
            v = 1. - v;
        }

        return v;
    }

    double sample(double nx, double ny, const APFXNoise::Perm& perm) const
    {
        if (_s.use4D) {
            const double p[4] = { nx, ny, _s.loopA, _s.loopB };

            return APFXNoise::fractal<4>(p, perm, _s.bp, _s.fp);
        }
        const double p[3] = { nx, ny, _s.z };

        return APFXNoise::fractal<3>(p, perm, _s.bp, _s.fp);
    }

    void multiThreadProcessImages(const OfxRectI& win,
                                  const OfxPointD& rs) OVERRIDE FINAL
    {
        APFXNoise::BasisParams warpBasis;
        warpBasis.basis = APFXNoise::eBasisPerlin;
        APFXNoise::FractalParams warpFractal;
        warpFractal.fractal = APFXNoise::eFractalFBM;
        warpFractal.octaves = _s.warpOctaves;
        const bool doWarp = (_s.warpAmount != 0.) && (_s.warpSize > 0.);

        for (int y = win.y1; y < win.y2; ++y) {
            if ( _effect.abort() ) {
                break;
            }
            float* dst = (float*)_dstImg->getPixelAddress(win.x1, y);
            if (!dst) {
                continue;
            }
            for (int x = win.x1; x < win.x2; ++x, dst += 4) {
                // pixel centre in canonical coordinates
                const double px = (x + 0.5) * _par / rs.x;
                const double py = (y + 0.5) / rs.y;
                // inverse transform: undo translate, then rotate / scale around the centre
                double qx = px - _s.tx - _s.cx;
                double qy = py - _s.ty - _s.cy;
                double rx = _s.cosR * qx + _s.sinR * qy;
                double ry = -_s.sinR * qx + _s.cosR * qy;
                rx = rx / _s.sx + _s.cx;
                ry = ry / _s.sy + _s.cy;
                if (doWarp) {
                    const double wp[3] = { rx / _s.warpSize, ry / _s.warpSize, _s.z * 0.37 + 11.3 };
                    const double wq[3] = { rx / _s.warpSize + 31.7, ry / _s.warpSize - 17.1, _s.z * 0.37 - 5.9 };
                    const double dx = APFXNoise::fractal<3>(wp, _permWarpX, warpBasis, warpFractal) * 2. - 1.;
                    const double dy = APFXNoise::fractal<3>(wq, _permWarpY, warpBasis, warpFractal) * 2. - 1.;
                    rx += dx * _s.warpAmount;
                    ry += dy * _s.warpAmount;
                }
                const double nx = rx / _s.sizeX;
                const double ny = ry / _s.sizeY;

                float r, g, b, a;
                if (_s.color) {
                    r = (float)shape( sample(nx, ny, _permR) );
                    g = (float)shape( sample(nx, ny, _permG) );
                    b = (float)shape( sample(nx, ny, _permB) );
                    a = (r + g + b) / 3.f;
                } else {
                    r = g = b = a = (float)shape( sample(nx, ny, _perm) );
                }

                const float* s = NULL;
                if (_src) {
                    s = (const float*)_src->getPixelAddress(x, y);
                }
                switch (_s.output) {
                case eOutputRGB:
                    dst[0] = r; dst[1] = g; dst[2] = b;
                    dst[3] = s ? s[3] : 1.f;
                    break;
                case eOutputAlpha:
                    dst[0] = s ? s[0] : 0.f;
                    dst[1] = s ? s[1] : 0.f;
                    dst[2] = s ? s[2] : 0.f;
                    dst[3] = a;
                    break;
                case eOutputRGBA:
                default:
                    dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
                    break;
                }
            }
        }
    } // multiThreadProcessImages

    NoiseSettings _s;
    APFXNoise::Perm _perm, _permR, _permG, _permB, _permWarpX, _permWarpY;
    const Image* _src;
    double _par;
};

class APFXNoisePlugin
    : public ImageEffect
{
public:
    APFXNoisePlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        _srcClip = (getContext() == eContextGenerator) ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
        _output = fetchChoiceParam(kParamOutput);
        _color = fetchBooleanParam(kParamColor);
        _type = fetchChoiceParam(kParamType);
        _metric = fetchChoiceParam(kParamMetric);
        _jitter = fetchDoubleParam(kParamJitter);
        _fractal = fetchChoiceParam(kParamFractal);
        _size = fetchDouble2DParam(kParamSize);
        _z = fetchDoubleParam(kParamZ);
        _zSpeed = fetchDoubleParam(kParamZSpeed);
        _loop = fetchBooleanParam(kParamLoop);
        _loopPeriod = fetchIntParam(kParamLoopPeriod);
        _seed = fetchIntParam(kParamSeed);
        _octaves = fetchIntParam(kParamOctaves);
        _lacunarity = fetchDoubleParam(kParamLacunarity);
        _gain = fetchDoubleParam(kParamGain);
        _nyquist = fetchBooleanParam(kParamNyquist);
        _warpAmount = fetchDoubleParam(kParamWarpAmount);
        _warpSize = fetchDoubleParam(kParamWarpSize);
        _warpOctaves = fetchIntParam(kParamWarpOctaves);
        _gamma = fetchDoubleParam(kParamGamma);
        _contrast = fetchDoubleParam(kParamContrast);
        _offset = fetchDoubleParam(kParamOffset);
        _low = fetchDoubleParam(kParamLow);
        _high = fetchDoubleParam(kParamHigh);
        _invert = fetchBooleanParam(kParamInvert);
        _translate = fetchDouble2DParam(kParamTranslate);
        _rotate = fetchDoubleParam(kParamRotate);
        _scale = fetchDouble2DParam(kParamScale);
        _center = fetchDouble2DParam(kParamCenter);
        updateVisibility();
    }

private:
    bool srcConnected() const
    {
        return _srcClip && _srcClip->isConnected();
    }

    void updateVisibility()
    {
        const int t = _type->getValue();
        const bool cellular = (t == APFXNoise::eBasisAlligator) || (t == APFXNoise::eBasisWorleyF1) ||
                              (t == APFXNoise::eBasisWorleyF2) || (t == APFXNoise::eBasisWorleyF2F1);
        _metric->setEnabled(cellular);
        _jitter->setEnabled(cellular);
        const bool frac = _fractal->getValue() != APFXNoise::eFractalNone;
        _octaves->setEnabled(frac);
        _lacunarity->setEnabled(frac);
        _gain->setEnabled(frac);
        _nyquist->setEnabled(frac);
        _loopPeriod->setEnabled( _loop->getValue() );
        const bool warp = _warpAmount->getValue() != 0.;
        _warpSize->setEnabled(warp);
        _warpOctaves->setEnabled(warp);
    }

    void changedParam(const InstanceChangedArgs& /*args*/,
                      const std::string& paramName) OVERRIDE FINAL
    {
        if ( (paramName == kParamType) || (paramName == kParamFractal) || (paramName == kParamLoop) || (paramName == kParamWarpAmount) ) {
            updateVisibility();
        }
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args,
                               OfxRectD& rod) OVERRIDE FINAL
    {
        if ( srcConnected() ) {
            rod = _srcClip->getRegionOfDefinition(args.time);

            return true;
        }
        const OfxPointD off = getProjectOffset();
        const OfxPointD sz = getProjectSize();
        rod.x1 = off.x;
        rod.y1 = off.y;
        rod.x2 = off.x + sz.x;
        rod.y2 = off.y + sz.y;

        return true;
    }

    void getClipPreferences(ClipPreferencesSetter& clipPreferences) OVERRIDE FINAL
    {
        clipPreferences.setClipComponents(*_dstClip, ePixelComponentRGBA);
        clipPreferences.setClipBitDepth(*_dstClip, eBitDepthFloat);
        if (_srcClip) {
            clipPreferences.setClipComponents(*_srcClip, ePixelComponentRGBA);
        }
        clipPreferences.setOutputPremultiplication(eImagePreMultiplied);
        // only a z speed makes the noise change by itself over time; keyframed knobs are
        // seen by the host, so static noise is rendered once and reused on every frame
        clipPreferences.setOutputFrameVarying( _zSpeed->getValue() != 0. || _zSpeed->getNumKeys() > 0 );
        clipPreferences.setOutputHasContinuousSamples(true);
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        Image* dst = _dstClip->fetchImage(args.time);
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        if ( (dst->getPixelDepth() != eBitDepthFloat) || (dst->getPixelComponents() != ePixelComponentRGBA) ) {
            delete dst;
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        Image* src = NULL;
        if ( srcConnected() ) {
            src = _srcClip->fetchImage(args.time);
            if ( src && ( (src->getPixelDepth() != eBitDepthFloat) || (src->getPixelComponents() != ePixelComponentRGBA) ) ) {
                delete src;
                src = NULL;
            }
        }

        const double t = args.time;
        NoiseSettings s;
        s.output = _output->getValueAtTime(t);
        s.color = _color->getValueAtTime(t);
        s.bp.basis = _type->getValueAtTime(t);
        s.bp.metric = _metric->getValueAtTime(t);
        s.bp.jitter = _jitter->getValueAtTime(t);
        s.fp.fractal = _fractal->getValueAtTime(t);
        s.fp.lacunarity = std::max(1.01, _lacunarity->getValueAtTime(t));
        s.fp.gain = _gain->getValueAtTime(t);
        _size->getValueAtTime(t, s.sizeX, s.sizeY);
        s.sizeX = std::max(1e-3, s.sizeX);
        s.sizeY = std::max(1e-3, s.sizeY);
        s.seed = (uint32_t)_seed->getValueAtTime(t);

        // Z: animated value + speed * frame; Loop runs Z around a circle in a 4th dimension
        const double z0 = _z->getValueAtTime(t);
        const double speed = _zSpeed->getValueAtTime(t);
        const int period = std::max( 1, _loopPeriod->getValueAtTime(t) );
        if ( _loop->getValueAtTime(t) && (speed != 0.) ) {
            const double radius = std::fabs(speed) * period / (2. * M_PI);
            const double phase = 2. * M_PI * std::fmod(t, (double)period) / period * (speed < 0. ? -1. : 1.);
            s.use4D = true;
            s.loopA = z0 + radius * std::cos(phase);
            s.loopB = radius * std::sin(phase);
        } else {
            s.z = z0 + speed * t;
        }

        // transform
        _translate->getValueAtTime(t, s.tx, s.ty);
        _center->getValueAtTime(t, s.cx, s.cy);
        double scx, scy;
        _scale->getValueAtTime(t, scx, scy);
        s.sx = (std::fabs(scx) < 1e-6) ? 1e-6 : scx;
        s.sy = (std::fabs(scy) < 1e-6) ? 1e-6 : scy;
        const double rot = _rotate->getValueAtTime(t) * M_PI / 180.;
        s.cosR = std::cos(rot);
        s.sinR = std::sin(rot);

        // Nyquist: drop the octaves whose detail would be smaller than ~2 output pixels
        int oct = std::max( 1, _octaves->getValueAtTime(t) );
        if ( _nyquist->getValueAtTime(t) && (s.fp.fractal != APFXNoise::eFractalNone) ) {
            const double featurePx = std::min(s.sizeX * std::fabs(s.sx), s.sizeY * std::fabs(s.sy)) *
                                     std::min(args.renderScale.x, args.renderScale.y);
            int maxOct = 1;
            double f = featurePx;
            while (f / s.fp.lacunarity >= 2. && maxOct < 32) {
                f /= s.fp.lacunarity;
                ++maxOct;
            }
            oct = std::min(oct, maxOct);
        }
        s.fp.octaves = oct;

        s.warpAmount = _warpAmount->getValueAtTime(t);
        s.warpSize = std::max(1e-3, _warpSize->getValueAtTime(t));
        s.warpOctaves = std::max( 1, _warpOctaves->getValueAtTime(t) );
        s.gamma = _gamma->getValueAtTime(t);
        s.contrast = _contrast->getValueAtTime(t);
        s.offset = _offset->getValueAtTime(t);
        s.low = _low->getValueAtTime(t);
        s.high = _high->getValueAtTime(t);
        s.invert = _invert->getValueAtTime(t);

        NoiseProcessor proc(*this);
        proc.setDstImg(dst);
        proc.setRenderWindow(args.renderWindow, args.renderScale);
        proc.setValues( s, src, dst->getPixelAspectRatio() );
        proc.process();

        delete src;
        delete dst;
    } // render

    Clip* _dstClip;
    Clip* _srcClip;
    ChoiceParam* _output;
    BooleanParam* _color;
    ChoiceParam* _type;
    ChoiceParam* _metric;
    DoubleParam* _jitter;
    ChoiceParam* _fractal;
    Double2DParam* _size;
    DoubleParam* _z;
    DoubleParam* _zSpeed;
    BooleanParam* _loop;
    IntParam* _loopPeriod;
    IntParam* _seed;
    IntParam* _octaves;
    DoubleParam* _lacunarity;
    DoubleParam* _gain;
    BooleanParam* _nyquist;
    DoubleParam* _warpAmount;
    DoubleParam* _warpSize;
    IntParam* _warpOctaves;
    DoubleParam* _gamma;
    DoubleParam* _contrast;
    DoubleParam* _offset;
    DoubleParam* _low;
    DoubleParam* _high;
    BooleanParam* _invert;
    Double2DParam* _translate;
    DoubleParam* _rotate;
    Double2DParam* _scale;
    Double2DParam* _center;
};

mDeclarePluginFactory(APFXNoisePluginFactory, {}, {});

void
APFXNoisePluginFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);
    desc.addSupportedContext(eContextGenerator);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
}

static ChoiceParamDescriptor*
choice(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint)
{
    ChoiceParamDescriptor* p = desc.defineChoiceParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setAnimates(false);
    page->addChild(*p);

    return p;
}

static DoubleParamDescriptor*
dbl(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint,
    double def, double dmin, double dmax)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(def);
    p->setDisplayRange(dmin, dmax);
    page->addChild(*p);

    return p;
}

static IntParamDescriptor*
integer(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint,
        int def, int dmin, int dmax)
{
    IntParamDescriptor* p = desc.defineIntParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(def);
    p->setDisplayRange(dmin, dmax);
    page->addChild(*p);

    return p;
}

static BooleanParamDescriptor*
boolean(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label, const char* hint,
        bool def)
{
    BooleanParamDescriptor* p = desc.defineBooleanParam(name);
    p->setLabel(label);
    p->setHint(hint);
    p->setDefault(def);
    page->addChild(*p);

    return p;
}

void
APFXNoisePluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                          ContextEnum context)
{
    if (context == eContextGeneral) {
        ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        srcClip->addSupportedComponent(ePixelComponentRGBA);
        srcClip->setTemporalClipAccess(false);
        srcClip->setSupportsTiles(true);
        srcClip->setOptional(true);
    }
    ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(true);

    PageParamDescriptor* page = desc.definePageParam("Noise");

    {
        ChoiceParamDescriptor* p = choice(desc, page, kParamOutput, "output", "Channels the noise is written to. With Source connected, the other channels come from Source.");
        p->appendOption("rgba");
        p->appendOption("rgb");
        p->appendOption("alpha");
        p->setDefault(eOutputRGBA);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
    }
    boolean(desc, page, kParamColor, "color noise", "Different noise in red, green and blue (alpha is their average).", false);

    {
        ChoiceParamDescriptor* p = choice(desc, page, kParamType, "type", "The basic noise pattern.\n* = heavier to render (about 4x Perlin).");
        p->appendOption("Perlin", "Smooth gradient noise (Ken Perlin's improved noise). Fast.", "perlin");
        p->appendOption("Simplex", "Gradient noise on a simplex grid: fewer grid artifacts than Perlin. Fast.", "simplex");
        p->appendOption("Sparse Convolution *", "Random impulses blended with a smooth kernel: soft, blobby noise. Heavier.", "sparse");
        p->appendOption("Alligator *", "Bumpy plates with creases, like alligator skin. Heavier.", "alligator");
        p->appendOption("Worley F1 *", "Cellular: distance to the closest random point (cells, scales). Heavier.", "worleyF1");
        p->appendOption("Worley F2 *", "Cellular: distance to the second closest point. Heavier.", "worleyF2");
        p->appendOption("Worley F2-F1 *", "Cellular borders: difference between the two closest points (cracks, veins). Heavier.", "worleyF2F1");
        p->appendOption("White", "One random value per cell (blocky). Fastest.", "white");
        p->setDefault(APFXNoise::eBasisPerlin);
    }
    {
        ChoiceParamDescriptor* p = choice(desc, page, kParamMetric, "metric", "Distance measure of the cellular types (Worley, Alligator).");
        p->appendOption("Euclidean", "Round cells.");
        p->appendOption("Manhattan", "Diamond shaped cells.");
        p->appendOption("Chebyshev", "Square cells.");
        p->setDefault(APFXNoise::eMetricEuclidean);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
    }
    {
        DoubleParamDescriptor* p = dbl(desc, page, kParamJitter, "jitter", "How irregular the cells are: 0 = regular grid, 1 = fully random.", 1., 0., 1.);
        p->setRange(0., 1.);
    }
    {
        ChoiceParamDescriptor* p = choice(desc, page, kParamFractal, "fractal", "How the octaves are added together.");
        p->appendOption("None", "Only the basic noise, no octaves.");
        p->appendOption("fBm", "Fractal Brownian motion: the classic layered noise (Nuke fBm).");
        p->appendOption("Turbulence", "Absolute value of every octave: billowy, cloudy (Nuke turbulence).");
        p->appendOption("Ridged", "Sharp ridges (Houdini \"sharp\").");
        p->appendOption("Terrain", "Heterogeneous terrain: more detail on the high areas.");
        p->appendOption("Hybrid Terrain", "Smooth valleys and rough, rocky peaks.");
        p->setDefault(APFXNoise::eFractalFBM);
    }
    {
        Double2DParamDescriptor* p = desc.defineDouble2DParam(kParamSize);
        p->setLabel("size");
        p->setHint("Size of the noise pattern in pixels (its largest features).");
        p->setDefault(350., 350.);
        p->setDisplayRange(1., 1., 2000., 2000.);
        page->addChild(*p);
    }
    dbl(desc, page, kParamZ, "z", "Position along the third dimension of the noise: animate it to evolve the noise.", 0., 0., 10.);
    {
        DoubleParamDescriptor* zs = dbl(desc, page, kParamZSpeed, "z speed", "Added to Z every frame (0 = still noise, rendered once and reused on every frame).", 0., -0.2, 0.2);
        desc.addClipPreferencesSlaveParam(*zs);
    }
    {
        BooleanParamDescriptor* p = boolean(desc, page, kParamLoop, "loop", "Loop the Z animation seamlessly every Loop period frames (needs a Z speed).", false);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
    }
    integer(desc, page, kParamLoopPeriod, "loop period", "Length of the loop in frames.", 100, 2, 500);
    integer(desc, page, kParamSeed, "seed", "Changes the random pattern.", 0, 0, 100);

    {
        PageParamDescriptor* p = page;
        GroupParamDescriptor* grp = desc.defineGroupParam("fractalGroup");
        grp->setLabel("Octaves");
        grp->setOpen(true);
        p->addChild(*grp);
        IntParamDescriptor* oct = integer(desc, page, kParamOctaves, "octaves", "Number of noise layers (more = finer detail, slower).", 6, 1, 16);
        oct->setRange(1, 32);
        oct->setParent(*grp);
        DoubleParamDescriptor* lac = dbl(desc, page, kParamLacunarity, "lacunarity", "Size ratio between octaves (2 = each octave is half the size).", 2., 1.1, 4.);
        lac->setParent(*grp);
        DoubleParamDescriptor* gain = dbl(desc, page, kParamGain, "gain", "Strength ratio between octaves (Houdini roughness).", 0.5, 0., 1.);
        gain->setParent(*grp);
        BooleanParamDescriptor* ny = boolean(desc, page, kParamNyquist, "nyquist", "Skip the octaves smaller than a pixel (faster, no aliasing).", true);
        ny->setParent(*grp);
    }
    {
        GroupParamDescriptor* grp = desc.defineGroupParam("warpGroup");
        grp->setLabel("Warp");
        grp->setOpen(false);
        page->addChild(*grp);
        DoubleParamDescriptor* a = dbl(desc, page, kParamWarpAmount, "amount", "Distorts the noise with another noise, in pixels (smoke, clouds, dirt).", 0., 0., 300.);
        a->setParent(*grp);
        DoubleParamDescriptor* sz = dbl(desc, page, kParamWarpSize, "warp size", "Size of the distortion noise in pixels.", 500., 10., 2000.);
        sz->setParent(*grp);
        IntParamDescriptor* o = integer(desc, page, kParamWarpOctaves, "warp octaves", "Detail of the distortion noise.", 3, 1, 8);
        o->setParent(*grp);
    }
    {
        GroupParamDescriptor* grp = desc.defineGroupParam("colorGroup");
        grp->setLabel("Output");
        grp->setOpen(true);
        page->addChild(*grp);
        DoubleParamDescriptor* g = dbl(desc, page, kParamGamma, "gamma", "Gamma of the noise values.", 1., 0.2, 5.);
        g->setParent(*grp);
        DoubleParamDescriptor* c = dbl(desc, page, kParamContrast, "contrast", "Contrast around mid grey.", 1., 0., 4.);
        c->setParent(*grp);
        DoubleParamDescriptor* o = dbl(desc, page, kParamOffset, "offset", "Added to the noise.", 0., -1., 1.);
        o->setParent(*grp);
        DoubleParamDescriptor* lo = dbl(desc, page, kParamLow, "low", "Output value of black noise.", 0., -1., 1.);
        lo->setParent(*grp);
        DoubleParamDescriptor* hi = dbl(desc, page, kParamHigh, "high", "Output value of white noise.", 1., 0., 2.);
        hi->setParent(*grp);
        BooleanParamDescriptor* inv = boolean(desc, page, kParamInvert, "invert", "Invert the noise.", false);
        inv->setParent(*grp);
    }
    {
        GroupParamDescriptor* grp = desc.defineGroupParam("transformGroup");
        grp->setLabel("Transform");
        grp->setOpen(false);
        page->addChild(*grp);
        Double2DParamDescriptor* tr = desc.defineDouble2DParam(kParamTranslate);
        tr->setLabel("translate");
        tr->setHint("Moves the noise pattern, in pixels.");
        tr->setDoubleType(eDoubleTypeXY);
        tr->setDefault(0., 0.);
        tr->setDisplayRange(-1000., -1000., 1000., 1000.);
        tr->setParent(*grp);
        page->addChild(*tr);
        DoubleParamDescriptor* rot = dbl(desc, page, kParamRotate, "rotate", "Rotates the noise pattern around the center, in degrees.", 0., -180., 180.);
        rot->setParent(*grp);
        Double2DParamDescriptor* sc = desc.defineDouble2DParam(kParamScale);
        sc->setLabel("scale");
        sc->setHint("Scales the noise pattern around the center.");
        sc->setDefault(1., 1.);
        sc->setDisplayRange(0.1, 0.1, 10., 10.);
        sc->setParent(*grp);
        page->addChild(*sc);
        Double2DParamDescriptor* ce = desc.defineDouble2DParam(kParamCenter);
        ce->setLabel("center");
        ce->setHint("Center of rotation and scale.");
        ce->setDoubleType(eDoubleTypeXYAbsolute);
        ce->setDefaultCoordinateSystem(eCoordinatesNormalised);
        ce->setDefault(0.5, 0.5);
        ce->setUseHostNativeOverlayHandle(true);
        ce->setParent(*grp);
        page->addChild(*ce);
    }
} // describeInContext

ImageEffect*
APFXNoisePluginFactory::createInstance(OfxImageEffectHandle handle,
                                       ContextEnum /*context*/)
{
    return new APFXNoisePlugin(handle);
}
} // anon namespace

static APFXNoisePluginFactory apfxNoiseFactory(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(apfxNoiseFactory)
