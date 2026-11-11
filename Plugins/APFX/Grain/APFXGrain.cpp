/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Grain: one node, two modes (only the chosen mode computes).
//  Add:   synthetic film / digital grain. Per channel size, irregularity, intensity; channel
//         correlation; response (black, minimum, shadows / mids / highlights); presets (approximate
//         film stocks and digital cameras) + user presets saved in a text file.
//  Match: DasGrain-like regrain written from scratch. Plate + degrained plate (or an internal
//         degrain) give the plate grain; "analyse" measures its per-channel response to luminance
//         on ~10 frames; the normalised grain is scattered with jittered Voronoi cells and added to
//         the comp only where the comp differs from the plate (replace mask), scaled by the response.

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "APFXOfxCommon.h"

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Grain"

enum ModeEnum { eModeAdd = 0, eModeMatch };
const int kBins = 8; // response curve bins over luminance 0..1 (last bin = >= 1)

// ---------------------------------------------------------------- presets (approximate looks, not measured)
struct GrainPreset
{
    const char* label;
    double size[3], irr[3], inten[3];
    double corr, black, minimum;
};

const GrainPreset kPresets[] = {
    { "Custom", { 1, 1, 1 }, { 0.6, 0.6, 0.6 }, { 0.05, 0.05, 0.05 }, 0.3, 0.0, 0.0 },
    { "Kodak 5203 50D (35mm)", { 1.0, 1.0, 1.3 }, { 0.5, 0.5, 0.6 }, { 0.018, 0.020, 0.032 }, 0.25, 0.0, 0.05 },
    { "Kodak 5207 250D (35mm)", { 1.2, 1.2, 1.6 }, { 0.6, 0.6, 0.7 }, { 0.028, 0.030, 0.050 }, 0.25, 0.0, 0.05 },
    { "Kodak 5213 200T (35mm)", { 1.15, 1.15, 1.5 }, { 0.6, 0.6, 0.7 }, { 0.026, 0.028, 0.046 }, 0.25, 0.0, 0.05 },
    { "Kodak 5219 500T (35mm)", { 1.4, 1.4, 1.9 }, { 0.65, 0.65, 0.75 }, { 0.040, 0.044, 0.070 }, 0.25, 0.0, 0.06 },
    { "Kodak 7219 500T (16mm)", { 2.0, 2.0, 2.6 }, { 0.7, 0.7, 0.8 }, { 0.060, 0.065, 0.100 }, 0.25, 0.0, 0.08 },
    { "Kodak 5222 Double-X (B&W)", { 1.5, 1.5, 1.5 }, { 0.7, 0.7, 0.7 }, { 0.050, 0.050, 0.050 }, 1.0, 0.0, 0.06 },
    { "ARRI Alexa ISO 800", { 0.9, 0.9, 1.0 }, { 0.3, 0.3, 0.3 }, { 0.008, 0.007, 0.012 }, 0.6, 0.002, 0.15 },
    { "ARRI Alexa ISO 1600", { 0.95, 0.95, 1.05 }, { 0.3, 0.3, 0.3 }, { 0.014, 0.012, 0.020 }, 0.6, 0.004, 0.15 },
    { "ARRI Alexa ISO 3200", { 1.0, 1.0, 1.1 }, { 0.35, 0.35, 0.35 }, { 0.024, 0.021, 0.034 }, 0.6, 0.008, 0.15 },
    { "RED Monstro ISO 800", { 0.85, 0.85, 0.95 }, { 0.3, 0.3, 0.3 }, { 0.010, 0.009, 0.016 }, 0.5, 0.003, 0.12 },
    { "Sony Venice ISO 500", { 0.85, 0.85, 0.95 }, { 0.25, 0.25, 0.25 }, { 0.006, 0.005, 0.009 }, 0.6, 0.002, 0.15 },
    { "Sony Venice ISO 2500", { 0.9, 0.9, 1.0 }, { 0.3, 0.3, 0.3 }, { 0.016, 0.014, 0.024 }, 0.6, 0.006, 0.15 },
};
const int kNbPresets = (int)( sizeof(kPresets) / sizeof(kPresets[0]) );

// user presets: one per line "name|sr sg sb|ir ig ib|nr ng nb|corr black minimum"
std::string
userPresetFile()
{
    const char* home = std::getenv("USERPROFILE");
    if (!home) {
        home = std::getenv("HOME");
    }
    std::string dir = home ? std::string(home) : std::string(".");

    return dir + "/.solvfx_grain_presets.txt";
}

std::vector<GrainPreset>
loadUserPresets(std::vector<std::string>* names)
{
    std::vector<GrainPreset> out;
    std::ifstream f( userPresetFile().c_str() );
    std::string line;
    while ( std::getline(f, line) ) {
        const size_t bar = line.find('|');
        if (bar == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, bar);
        std::string rest = line.substr(bar + 1);
        for (char& c : rest) {
            if (c == '|') {
                c = ' ';
            }
        }
        std::istringstream is(rest);
        GrainPreset p;
        p.label = NULL;
        if ( is >> p.size[0] >> p.size[1] >> p.size[2] >> p.irr[0] >> p.irr[1] >> p.irr[2]
                >> p.inten[0] >> p.inten[1] >> p.inten[2] >> p.corr >> p.black >> p.minimum ) {
            out.push_back(p);
            names->push_back(name);
        }
    }

    return out;
}

// ---------------------------------------------------------------- noise helpers
inline uint32_t
hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;

    return x;
}

inline float
gauss(int x, int y, uint32_t seed)
{
    // Box-Muller from two hashes: N(0, 1) per pixel, position deterministic (tiles agree)
    const uint32_t h1 = hash32( (uint32_t)x * 73856093U ^ (uint32_t)y * 19349663U ^ seed );
    const uint32_t h2 = hash32(h1 ^ 0x9e3779b9U);
    const float u1 = ( (h1 >> 8) + 1.f ) / 16777217.f;
    const float u2 = (h2 >> 8) / 16777216.f;

    return std::sqrt(-2.f * std::log(u1) ) * std::cos(6.2831853f * u2);
}

inline float
uniform01(uint32_t h)
{
    return (hash32(h) >> 8) / 16777216.f;
}

// box blur radius r (pixels, fractional = blended) on a w*h single channel buffer, in place
void
boxBlur(std::vector<float>& v, int w, int h, double r)
{
    if (r <= 0.01) {
        return;
    }
    const int ri = (int)std::floor(r);
    const float frac = (float)(r - ri);
    std::vector<float> tmp( v.size() );
    std::vector<float> line( std::max(w, h) );
    for (int pass = 0; pass < 2; ++pass) { // two passes ~ triangle ~ soft gaussian-like
        // horizontal
        for (int y = 0; y < h; ++y) {
            const float* in = &v[(size_t)y * w];
            float* o = &tmp[(size_t)y * w];
            for (int x = 0; x < w; ++x) {
                float s = 0.f;
                for (int k = -ri; k <= ri; ++k) {
                    s += in[std::max( 0, std::min(w - 1, x + k) )];
                }
                const float e = in[std::max( 0, std::min(w - 1, x - ri - 1) )] + in[std::max( 0, std::min(w - 1, x + ri + 1) )];
                o[x] = (s + frac * e) / (2.f * ri + 1.f + 2.f * frac);
            }
        }
        // vertical
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) {
                line[y] = tmp[(size_t)y * w + x];
            }
            for (int y = 0; y < h; ++y) {
                float s = 0.f;
                for (int k = -ri; k <= ri; ++k) {
                    s += line[std::max( 0, std::min(h - 1, y + k) )];
                }
                const float e = line[std::max( 0, std::min(h - 1, y - ri - 1) )] + line[std::max( 0, std::min(h - 1, y + ri + 1) )];
                v[(size_t)y * w + x] = (s + frac * e) / (2.f * ri + 1.f + 2.f * frac);
            }
        }
    }
}

inline float
luma(const float* p)
{
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

// response curve lookup: bins over luminance 0..1 (linear interpolation, clamped)
inline float
curveAt(const float* c, float l)
{
    const float x = std::max(0.f, std::min( (float)(kBins - 1), l * (kBins - 1) ) );
    const int i = std::min(kBins - 2, (int)x);
    const float f = x - i;

    return c[i] + (c[i + 1] - c[i]) * f;
}

// ---------------------------------------------------------------- plugin
class APFXGrainPlugin
    : public ImageEffect
{
public:
    APFXGrainPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _plate = fetchClip("Plate");
        _degrain = fetchClip("Degrain");
        _std.fetch(*this, true, false);
        _mode = fetchChoiceParam("mode");
        _preset = fetchChoiceParam("preset");
        _userPreset = fetchChoiceParam("userPreset");
        _presetName = fetchStringParam("presetName");
        _savePreset = fetchPushButtonParam("savePreset");
        _seed = fetchIntParam("seed");
        _static = fetchBooleanParam("staticGrain");
        _size = fetchRGBParam("size");
        _irr = fetchRGBParam("irregularity");
        _inten = fetchRGBParam("intensity");
        _corr = fetchDoubleParam("correlation");
        _black = fetchDoubleParam("black");
        _minimum = fetchDoubleParam("minimum");
        _shadows = fetchDoubleParam("shadows");
        _mids = fetchDoubleParam("midtones");
        _highs = fetchDoubleParam("highlights");
        _amount = fetchDoubleParam("amount");
        _degrainAmt = fetchDoubleParam("degrainAmount");
        _analyse = fetchPushButtonParam("analyse");
        _frames = fetchIntParam("analyseFrames");
        _analysis = fetchStringParam("analysis");
        _cell = fetchDoubleParam("cellSize");
        _edge = fetchDoubleParam("edgeBlend");
        _replaceThresh = fetchDoubleParam("replaceThreshold");
        _replaceSoft = fetchDoubleParam("replaceSoftness");
        _adjust = fetchRGBParam("responseGain");
        _output = fetchChoiceParam("output");
        refreshUserPresets();
        updateVisibility();
    }

private:
    void refreshUserPresets()
    {
        std::vector<std::string> names;
        _user = loadUserPresets(&names);
        _userPreset->resetOptions();
        _userPreset->appendOption("-");
        for (size_t i = 0; i < names.size(); ++i) {
            _userPreset->appendOption(names[i]);
        }
    }

    void updateVisibility()
    {
        const bool add = _mode->getValue() == eModeAdd;
        _preset->setIsSecretAndDisabled(!add);
        _userPreset->setIsSecretAndDisabled(!add);
        _presetName->setIsSecretAndDisabled(!add);
        _savePreset->setIsSecretAndDisabled(!add);
        _size->setIsSecretAndDisabled(!add);
        _irr->setIsSecretAndDisabled(!add);
        _inten->setIsSecretAndDisabled(!add);
        _corr->setIsSecretAndDisabled(!add);
        _black->setIsSecretAndDisabled(!add);
        _minimum->setIsSecretAndDisabled(!add);
        _shadows->setIsSecretAndDisabled(!add);
        _mids->setIsSecretAndDisabled(!add);
        _highs->setIsSecretAndDisabled(!add);
        _degrainAmt->setIsSecretAndDisabled(add);
        _analyse->setIsSecretAndDisabled(add);
        _frames->setIsSecretAndDisabled(add);
        _cell->setIsSecretAndDisabled(add);
        _edge->setIsSecretAndDisabled(add);
        _replaceThresh->setIsSecretAndDisabled(add);
        _replaceSoft->setIsSecretAndDisabled(add);
        _adjust->setIsSecretAndDisabled(add);
        _output->setIsSecretAndDisabled(add);
    }

    void applyPreset(const GrainPreset& p)
    {
        beginEditBlock("grain preset");
        _size->setValue(p.size[0], p.size[1], p.size[2]);
        _irr->setValue(p.irr[0], p.irr[1], p.irr[2]);
        _inten->setValue(p.inten[0], p.inten[1], p.inten[2]);
        _corr->setValue(p.corr);
        _black->setValue(p.black);
        _minimum->setValue(p.minimum);
        endEditBlock();
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) OVERRIDE FINAL
    {
        if (name == "mode") {
            updateVisibility();
        } else if ( (name == "preset") && (args.reason == eChangeUserEdit) ) {
            const int i = _preset->getValue();
            if ( (i > 0) && (i < kNbPresets) ) {
                applyPreset(kPresets[i]);
            }
        } else if ( (name == "userPreset") && (args.reason == eChangeUserEdit) ) {
            const int i = _userPreset->getValue() - 1;
            if ( (i >= 0) && ( i < (int)_user.size() ) ) {
                applyPreset(_user[i]);
            }
        } else if (name == "savePreset") {
            savePreset();
        } else if (name == "analyse") {
            analyse(args.time);
        }
    }

    void savePreset()
    {
        std::string n;
        _presetName->getValue(n);
        if ( n.empty() ) {
            sendMessage(Message::eMessageError, "", "Type a preset name first.");

            return;
        }
        for (char& c : n) {
            if ( (c == '|') || (c == '\n') ) {
                c = ' ';
            }
        }
        double s[3], ir[3], it[3];
        _size->getValue(s[0], s[1], s[2]);
        _irr->getValue(ir[0], ir[1], ir[2]);
        _inten->getValue(it[0], it[1], it[2]);
        std::ofstream f(userPresetFile().c_str(), std::ios::app);
        if (!f) {
            sendMessage(Message::eMessageError, "", "Could not write " + userPresetFile());

            return;
        }
        f << n << "|" << s[0] << " " << s[1] << " " << s[2] << "|" << ir[0] << " " << ir[1] << " " << ir[2] << "|"
          << it[0] << " " << it[1] << " " << it[2] << "|" << _corr->getValue() << " " << _black->getValue() << " "
          << _minimum->getValue() << "\n";
        f.close();
        refreshUserPresets();
        sendMessage(Message::eMessageMessage, "", "Preset \"" + n + "\" saved (all comps): " + userPresetFile());
    }

    // ------------------------------------------------ match: grain of the plate
    // fills g (rgb, normalised later) = plate - degrained plate, and lum = luminance of the degrained plate
    bool plateGrain(double t, const OfxPointD& rs, std::vector<float>& g, std::vector<float>& lum, OfxRectI& b)
    {
        if ( !_plate->isConnected() ) {
            return false;
        }
        Image* p = _plate->fetchImage(t);
        if ( !p || (p->getPixelDepth() != eBitDepthFloat) || (p->getPixelComponents() != ePixelComponentRGBA) ) {
            delete p;

            return false;
        }
        b = p->getBounds();
        const int w = b.x2 - b.x1, h = b.y2 - b.y1;
        if ( (w <= 0) || (h <= 0) ) {
            delete p;

            return false;
        }
        g.assign( (size_t)w * h * 3, 0.f );
        lum.assign( (size_t)w * h, 0.f );
        Image* d = _degrain->isConnected() ? _degrain->fetchImage(t) : NULL;
        if ( d && ( (d->getPixelDepth() != eBitDepthFloat) || (d->getPixelComponents() != ePixelComponentRGBA) ) ) {
            delete d;
            d = NULL;
        }
        std::vector<float> clean[3];
        if (!d) {
            // internal degrain: soft blur of the plate, mixed by "degrain amount"
            const double r = 1.5 * std::max(0.25, rs.x);
            for (int c = 0; c < 3; ++c) {
                clean[c].assign( (size_t)w * h, 0.f );
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const float* s = (const float*)p->getPixelAddress(b.x1 + x, b.y1 + y);
                        clean[c][(size_t)y * w + x] = s ? s[c] : 0.f;
                    }
                }
                boxBlur(clean[c], w, h, r);
            }
        }
        const float amt = (float)_degrainAmt->getValueAtTime(t);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* s = (const float*)p->getPixelAddress(b.x1 + x, b.y1 + y);
                const float* dd = d ? (const float*)d->getPixelAddress(b.x1 + x, b.y1 + y) : NULL;
                float cl[3];
                for (int c = 0; c < 3; ++c) {
                    const float sv = s ? s[c] : 0.f;
                    const float cv = dd ? dd[c] : clean[c][(size_t)y * w + x];
                    const float clean2 = sv + (cv - sv) * (d ? 1.f : amt);
                    cl[c] = clean2;
                    g[( (size_t)y * w + x ) * 3 + c] = sv - clean2;
                }
                lum[(size_t)y * w + x] = luma(cl);
            }
        }
        delete d;
        delete p;

        return true;
    }

    // stored analysis: 3 * kBins std values ("r0 .. r7 g0 .. b7")
    bool storedCurve(float* curve)
    {
        std::string s;
        _analysis->getValue(s);
        std::istringstream is(s);
        for (int i = 0; i < 3 * kBins; ++i) {
            if ( !(is >> curve[i]) ) {
                return false;
            }
        }

        return true;
    }

    void analyse(double t0)
    {
        if ( !_plate->isConnected() ) {
            sendMessage(Message::eMessageError, "", "Connect the Plate input (the original grainy plate) first.");

            return;
        }
        OfxRangeD range = _plate->getFrameRange();
        const int n = std::max( 1, _frames->getValue() );
        std::vector<double> sum(3 * kBins, 0.), sum2(3 * kBins, 0.), cnt(kBins, 0.);
        OfxPointD rs = { 1., 1. };
        for (int i = 0; i < n; ++i) {
            const double t = (n == 1 || range.max <= range.min) ? t0 : std::floor( range.min + (range.max - range.min) * i / (n - 1.) );
            std::vector<float> g, lum;
            OfxRectI b;
            if ( !plateGrain(t, rs, g, lum, b) ) {
                continue;
            }
            const size_t np = lum.size();
            // sample every 3rd pixel, more weight in the darks (they are rare and matter)
            for (size_t k = 0; k < np; k += 3) {
                const float l = std::max(0.f, std::min(1.f, lum[k]) );
                const int bin = std::min(kBins - 1, (int)(l * (kBins - 1) + 0.5f) );
                cnt[bin] += 1.;
                for (int c = 0; c < 3; ++c) {
                    const double v = g[k * 3 + c];
                    sum[c * kBins + bin] += v;
                    sum2[c * kBins + bin] += v * v;
                }
            }
        }
        std::ostringstream os;
        for (int c = 0; c < 3; ++c) {
            // std per bin; empty bins take the nearest measured one
            std::vector<double> sd(kBins, -1.);
            for (int k = 0; k < kBins; ++k) {
                if (cnt[k] > 50.) {
                    const double m = sum[c * kBins + k] / cnt[k];
                    sd[k] = std::sqrt( std::max(0., sum2[c * kBins + k] / cnt[k] - m * m) );
                }
            }
            for (int k = 0; k < kBins; ++k) {
                if (sd[k] < 0.) {
                    for (int dk = 1; dk < kBins; ++dk) {
                        if ( (k - dk >= 0) && (sd[k - dk] >= 0.) ) { sd[k] = sd[k - dk]; break; }
                        if ( (k + dk < kBins) && (sd[k + dk] >= 0.) ) { sd[k] = sd[k + dk]; break; }
                    }
                }
                os << std::max(1e-5, sd[k] < 0. ? 0.01 : sd[k]) << " ";
            }
        }
        _analysis->setValue( os.str() );
        sendMessage(Message::eMessageMessage, "", "Grain analysed on " + std::to_string(n) + " frames.");
    }

    // ------------------------------------------------ OFX
    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL
    {
        if ( !_src->isConnected() ) {
            return false;
        }
        rod = _src->getRegionOfDefinition(args.time);

        return true;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setOutputFrameVarying(true); // grain changes every frame
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        if (_mode->getValueAtTime(args.time) == eModeAdd) {
            renderAdd(args);
        } else {
            renderMatch(args);
        }
    }

    Image* fetchSrc(double t, Image** dst)
    {
        *dst = _dst->fetchImage(t);
        Image* src = _src->isConnected() ? _src->fetchImage(t) : NULL;
        if ( !*dst || !src || (src->getPixelComponents() != ePixelComponentRGBA) || (src->getPixelDepth() != eBitDepthFloat) ) {
            delete src;
            delete *dst;
            *dst = NULL;
            throwSuiteStatusException(kOfxStatFailed);

            return NULL;
        }

        return src;
    }

    void renderAdd(const RenderArguments& args)
    {
        const double t = args.time;
        Image* dst = NULL;
        Image* src = fetchSrc(t, &dst);
        if (!src) {
            return;
        }
        double size[3], irr[3], inten[3];
        _size->getValueAtTime(t, size[0], size[1], size[2]);
        _irr->getValueAtTime(t, irr[0], irr[1], irr[2]);
        _inten->getValueAtTime(t, inten[0], inten[1], inten[2]);
        const float corr = (float)std::max( 0., std::min(1., _corr->getValueAtTime(t) ) );
        const float black = (float)_black->getValueAtTime(t);
        const float minimum = (float)_minimum->getValueAtTime(t);
        const float sh = (float)_shadows->getValueAtTime(t), mi = (float)_mids->getValueAtTime(t), hi = (float)_highs->getValueAtTime(t);
        const float amount = (float)_amount->getValueAtTime(t);
        const uint32_t seed = (uint32_t)_seed->getValueAtTime(t) * 2654435761U + ( _static->getValueAtTime(t) ? 0U : (uint32_t)( (int)std::floor(t) * 97 ) );
        const OfxRectI win = args.renderWindow;
        const double rs = args.renderScale.x;
        int maxR = 0;
        for (int c = 0; c < 3; ++c) {
            maxR = std::max( maxR, (int)std::ceil( std::max(0., size[c] - 0.5) * rs * 2 ) + 2 );
        }
        // noise is generated on the render window grown by the blur reach (tiles stay seamless)
        const int x0 = win.x1 - maxR, y0 = win.y1 - maxR;
        const int w = (win.x2 - win.x1) + 2 * maxR, h = (win.y2 - win.y1) + 2 * maxR;
        std::vector<float> n[3], shared( (size_t)w * h ), irrField( (size_t)w * h );
        // noise positions in canonical pixels so proxy renders keep the same pattern scale
        const double inv = 1. / std::max(1e-6, rs);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int cx = (int)std::floor( (x0 + x) * inv ), cy = (int)std::floor( (y0 + y) * inv );
                shared[(size_t)y * w + x] = gauss(cx, cy, seed ^ 0xA5A5A5A5U);
                irrField[(size_t)y * w + x] = gauss(cx >> 3, cy >> 3, seed ^ 0x1234567U);
            }
        }
        for (int c = 0; c < 3; ++c) {
            n[c].assign( (size_t)w * h, 0.f );
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int cx = (int)std::floor( (x0 + x) * inv ), cy = (int)std::floor( (y0 + y) * inv );
                    const float own = gauss(cx, cy, seed + 0x51ED27U * (c + 1) );
                    const float s = shared[(size_t)y * w + x];
                    n[c][(size_t)y * w + x] = own * std::sqrt(1.f - corr * corr) + s * corr;
                }
            }
            const double r = std::max(0., size[c] - 0.5) * rs;
            boxBlur(n[c], w, h, r);
            // blurring lowers the deviation: renormalise to 1 (measured, robust)
            double s2 = 0.;
            const size_t np = n[c].size();
            for (size_t k = 0; k < np; k += 7) {
                s2 += (double)n[c][k] * n[c][k];
            }
            const float norm = (float)( 1. / std::sqrt( std::max( 1e-12, s2 / std::max( (size_t)1, np / 7 ) ) ) );
            // irregularity: slow variation of the grain strength (clumping)
            const float ir = (float)std::max( 0., std::min(1., irr[c]) );
            for (size_t k = 0; k < np; ++k) {
                n[c][k] *= norm * ( 1.f + ir * 0.5f * irrField[k] );
            }
        }
        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        APFXOfx::hostParallelFor()([&](int ya, int yb) {
            for (int y = win.y1 + ya; y < win.y1 + yb; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                for (int x = win.x1; x < win.x2; ++x, d += 4) {
                    const float* s = (const float*)src->getPixelAddress(x, y);
                    if (!s) {
                        d[0] = d[1] = d[2] = d[3] = 0.f;
                        continue;
                    }
                    const float l = std::max(0.f, luma(s) );
                    // response: shadows / mids / highlights weights over luminance + minimum on black
                    const float ws = std::max(0.f, 1.f - l * 2.f), wh = std::max(0.f, std::min(1.f, (l - 0.5f) * 2.f) ), wm = 1.f - ws - wh;
                    const float resp = std::max( minimum, (ws * sh + wm * mi + wh * hi) * std::sqrt( std::min(l, 4.f) + 1e-4f ) );
                    const size_t k = (size_t)(y - y0) * w + (x - x0);
                    float mv = o.mix;
                    if (mask) {
                        const float* mp = (const float*)mask->getPixelAddress(x, y);
                        float v = mp ? *mp : 0.f;
                        if (o.maskInvert) {
                            v = 1.f - v;
                        }
                        mv *= std::max(0.f, std::min(1.f, v) );
                    }
                    for (int c = 0; c < 3; ++c) {
                        const float g = n[c][k] * (float)inten[c] * amount * resp + n[c][k] * black;
                        d[c] = s[c] + g * mv;
                    }
                    d[3] = s[3];
                }
            }
        }, win.y2 - win.y1);
        delete mask;
        delete src;
        delete dst;
    }

    void renderMatch(const RenderArguments& args)
    {
        const double t = args.time;
        Image* dst = NULL;
        Image* src = fetchSrc(t, &dst);
        if (!src) {
            return;
        }
        float curve[3 * kBins];
        std::vector<float> g, lum;
        OfxRectI pb;
        const bool haveCurve = storedCurve(curve);
        if ( !haveCurve || !plateGrain(t, args.renderScale, g, lum, pb) ) {
            // nothing to match yet: pass the comp through (message once)
            if (!haveCurve) {
                setPersistentMessage(Message::eMessageWarning, "", "Connect Plate and press \"analyse\".");
            }
            const OfxRectI win = args.renderWindow;
            for (int y = win.y1; y < win.y2; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                for (int x = win.x1; d && x < win.x2; ++x, d += 4) {
                    const float* s = (const float*)src->getPixelAddress(x, y);
                    for (int c = 0; c < 4; ++c) {
                        d[c] = s ? s[c] : 0.f;
                    }
                }
            }
            delete src;
            delete dst;

            return;
        }
        clearPersistentMessage();
        double adj[3];
        _adjust->getValueAtTime(t, adj[0], adj[1], adj[2]);
        const int pw = pb.x2 - pb.x1, ph = pb.y2 - pb.y1;
        // normalised grain texture: grain / response(lum)
        for (size_t k = 0; k < lum.size(); ++k) {
            for (int c = 0; c < 3; ++c) {
                g[k * 3 + c] /= std::max( 1e-5f, curveAt(curve + c * kBins, lum[k]) );
            }
        }
        const double rs = args.renderScale.x;
        const double cell = std::max(4., _cell->getValueAtTime(t) * rs);
        const float edge = (float)std::max( 0., std::min(1., _edge->getValueAtTime(t) ) );
        const float thr = (float)_replaceThresh->getValueAtTime(t);
        const float soft = (float)std::max(1e-5, _replaceSoft->getValueAtTime(t) );
        const int outMode = _output->getValueAtTime(t);
        const uint32_t seed = (uint32_t)_seed->getValueAtTime(t) * 2654435761U + ( _static->getValueAtTime(t) ? 0U : (uint32_t)( (int)std::floor(t) * 131 ) );
        Image* plateImg = _plate->fetchImage(t);
        APFXOfx::OutputArgs o;
        Image* mask = _std.get(t, o);
        const OfxRectI win = args.renderWindow;
        // a cell's grain comes from a random spot of the plate texture (scatter, no visible repeats)
        auto cellOffset = [&](int ix, int iy, int* ox, int* oy) {
            const uint32_t hh = hash32( (uint32_t)ix * 0x8da6b343U ^ (uint32_t)iy * 0xd8163841U ^ seed );
            *ox = (int)( uniform01(hh) * pw );
            *oy = (int)( uniform01(hh ^ 0x68e31da4U) * ph );
        };
        APFXOfx::hostParallelFor()([&](int ya, int yb) {
            for (int y = win.y1 + ya; y < win.y1 + yb; ++y) {
                float* d = (float*)dst->getPixelAddress(win.x1, y);
                if (!d) {
                    continue;
                }
                for (int x = win.x1; x < win.x2; ++x, d += 4) {
                    const float* s = (const float*)src->getPixelAddress(x, y);
                    if (!s) {
                        d[0] = d[1] = d[2] = d[3] = 0.f;
                        continue;
                    }
                    // jittered Voronoi: nearest and second nearest cell centres
                    const double gx = x / cell, gy = y / cell;
                    const int cx = (int)std::floor(gx), cy = (int)std::floor(gy);
                    double d1 = 1e9, d2 = 1e9;
                    int b1x = cx, b1y = cy, b2x = cx, b2y = cy;
                    for (int jy = -1; jy <= 1; ++jy) {
                        for (int jx = -1; jx <= 1; ++jx) {
                            const int ix = cx + jx, iy = cy + jy;
                            const uint32_t hh = hash32( (uint32_t)ix * 0x27d4eb2dU ^ (uint32_t)iy * 0x165667b1U ^ (seed * 3U) );
                            const double px = ix + 0.15 + 0.7 * uniform01(hh), py = iy + 0.15 + 0.7 * uniform01(hh ^ 0x1b873593U);
                            const double dd = (px - gx) * (px - gx) + (py - gy) * (py - gy);
                            if (dd < d1) {
                                d2 = d1; b2x = b1x; b2y = b1y;
                                d1 = dd; b1x = ix; b1y = iy;
                            } else if (dd < d2) {
                                d2 = dd; b2x = ix; b2y = iy;
                            }
                        }
                    }
                    // blend weight towards the second cell near the boundary (edge blend)
                    const double dist = ( std::sqrt(d2) - std::sqrt(d1) ); // 0 on the boundary
                    float w2 = 0.f;
                    if (edge > 0.f) {
                        w2 = 0.5f * std::max( 0.f, 1.f - (float)dist / (0.5f * edge) );
                    }
                    int o1x, o1y, o2x, o2y;
                    cellOffset(b1x, b1y, &o1x, &o1y);
                    cellOffset(b2x, b2y, &o2x, &o2y);
                    auto texel = [&](int ox, int oy, int c) {
                        int tx = ( (x + ox) % pw + pw ) % pw, ty = ( (y + oy) % ph + ph ) % ph;
                        return g[( (size_t)ty * pw + tx ) * 3 + c];
                    };
                    // replace mask: where the comp differs from the plate
                    float rep = 1.f;
                    if (plateImg) {
                        const float* pp = (const float*)plateImg->getPixelAddress(x, y);
                        if (pp) {
                            float diff = 0.f;
                            for (int c = 0; c < 3; ++c) {
                                diff = std::max(diff, std::abs(pp[c] - s[c]) );
                            }
                            rep = std::max(0.f, std::min(1.f, (diff - thr) / soft) );
                        }
                    }
                    const float l = std::max(0.f, luma(s) );
                    float mv = o.mix;
                    if (mask) {
                        const float* mp = (const float*)mask->getPixelAddress(x, y);
                        float v = mp ? *mp : 0.f;
                        if (o.maskInvert) {
                            v = 1.f - v;
                        }
                        mv *= std::max(0.f, std::min(1.f, v) );
                    }
                    for (int c = 0; c < 3; ++c) {
                        // blending two independent grains lowers the deviation: renormalise
                        const float a = texel(o1x, o1y, c), b = texel(o2x, o2y, c);
                        const float gr = ( a * (1.f - w2) + b * w2 ) / std::sqrt( (1.f - w2) * (1.f - w2) + w2 * w2 );
                        const float add = gr * curveAt(curve + c * kBins, l) * (float)adj[c];
                        if (outMode == 1) {
                            d[c] = add + 0.5f;        // grain only (QC)
                        } else if (outMode == 2) {
                            d[c] = rep;               // replace mask (QC)
                        } else {
                            d[c] = s[c] + add * rep * mv;
                        }
                    }
                    d[3] = s[3];
                }
            }
        }, win.y2 - win.y1);
        delete mask;
        delete plateImg;
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    Clip* _plate;
    Clip* _degrain;
    APFXOfx::StdParams _std;
    std::vector<GrainPreset> _user;
    ChoiceParam* _mode;
    ChoiceParam* _preset;
    ChoiceParam* _userPreset;
    StringParam* _presetName;
    PushButtonParam* _savePreset;
    IntParam* _seed;
    BooleanParam* _static;
    RGBParam* _size;
    RGBParam* _irr;
    RGBParam* _inten;
    DoubleParam* _corr;
    DoubleParam* _black;
    DoubleParam* _minimum;
    DoubleParam* _shadows;
    DoubleParam* _mids;
    DoubleParam* _highs;
    DoubleParam* _amount;
    DoubleParam* _degrainAmt;
    PushButtonParam* _analyse;
    IntParam* _frames;
    StringParam* _analysis;
    DoubleParam* _cell;
    DoubleParam* _edge;
    DoubleParam* _replaceThresh;
    DoubleParam* _replaceSoft;
    RGBParam* _adjust;
    ChoiceParam* _output;
};

mDeclarePluginFactory(APFXGrainFactory, {}, {});

void
APFXGrainFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "Grain",
                            "APFX Grain. Add: synthetic grain with film / camera presets (approximate starting points), per "
                            "channel size, irregularity and intensity, channel correlation, response by luminance; save your own "
                            "presets (shared by all comps). Match: regrain like the plate. Connect Plate (original) and optionally "
                            "Degrain (denoised plate; otherwise a soft internal degrain is used), press analyse, and the grain is "
                            "scattered (Voronoi cells) and added to the comp where it differs from the plate. Tip: prerender the "
                            "denoised plate.");
    desc.setPluginGrouping("Draw");
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(true);
}

void
APFXGrainFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectSimpleSourceClipName, false);
    APFXOfx::defineRGBAClip(desc, "Plate", true)->setTemporalClipAccess(true);
    APFXOfx::defineRGBAClip(desc, "Degrain", true)->setTemporalClipAccess(true);
    APFXOfx::defineRGBAClip(desc, kOfxImageEffectOutputClipName, false);
    APFXOfx::defineMaskClip(desc);
    PageParamDescriptor* page = desc.definePageParam("Grain");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("mode");
        p->setLabel("mode");
        p->appendOption("add", "Synthetic grain (presets / custom).");
        p->appendOption("match", "Regrain matching the plate's grain (DasGrain-like).");
        p->setDefault(eModeAdd);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("preset");
        p->setLabel("preset");
        p->setHint("Film stocks and digital cameras (approximate starting points, not measured).");
        for (int i = 0; i < kNbPresets; ++i) {
            p->appendOption(kPresets[i].label);
        }
        p->setDefault(2);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("userPreset");
        p->setLabel("my presets");
        p->setHint("Presets you saved (shared by all comps).");
        p->appendOption("-");
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        StringParamDescriptor* p = desc.defineStringParam("presetName");
        p->setLabel("preset name");
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        PushButtonParamDescriptor* p = desc.definePushButtonParam("savePreset");
        p->setLabel("save preset");
        p->setHint("Saves the current settings as a preset available in every comp.");
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("size");
        p->setLabel("size");
        p->setHint("Grain size per channel (pixels at full resolution).");
        p->setDefault(1.2, 1.2, 1.6);
        p->setRange(0., 0., 0., 50., 50., 50.);
        p->setDisplayRange(0., 0., 0., 5., 5., 5.);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("irregularity");
        p->setLabel("irregularity");
        p->setHint("Clumping: slow variation of the grain strength.");
        p->setDefault(0.6, 0.6, 0.7);
        p->setRange(0., 0., 0., 1., 1., 1.);
        p->setDisplayRange(0., 0., 0., 1., 1., 1.);
        page->addChild(*p);
    }
    {
        RGBParamDescriptor* p = desc.defineRGBParam("intensity");
        p->setLabel("intensity");
        p->setHint("Grain strength per channel.");
        p->setDefault(0.028, 0.030, 0.050);
        p->setRange(0., 0., 0., 10., 10., 10.);
        p->setDisplayRange(0., 0., 0., 0.2, 0.2, 0.2);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "correlation", "correlation", "How much the channels share the same grain (1 = monochrome grain).", 0.25, 0., 1., 0., 1.);
    APFXOfx::defineDouble(desc, page, "black", "black", "Grain added everywhere, also on pure black.", 0., 0., 0.05, 0., 1.);
    APFXOfx::defineDouble(desc, page, "minimum", "minimum", "Minimum grain response in the darks.", 0.05, 0., 1., 0., 10.);
    APFXOfx::defineDouble(desc, page, "shadows", "shadows", "Response in the shadows.", 1., 0., 2., 0., 10.);
    APFXOfx::defineDouble(desc, page, "midtones", "midtones", "Response in the midtones.", 1., 0., 2., 0., 10.);
    APFXOfx::divider( APFXOfx::defineDouble(desc, page, "highlights", "highlights", "Response in the highlights.", 0.8, 0., 2., 0., 10.) );
    // match
    APFXOfx::defineDouble(desc, page, "degrainAmount", "degrain amount", "Internal degrain strength (only without a Degrain input).", 0.8, 0., 1., 0., 1.);
    {
        IntParamDescriptor* p = desc.defineIntParam("analyseFrames");
        p->setLabel("frames");
        p->setHint("Frames spread over the plate used by analyse.");
        p->setDefault(10);
        p->setRange(1, 200);
        p->setDisplayRange(1, 30);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*p);
    }
    {
        PushButtonParamDescriptor* p = desc.definePushButtonParam("analyse");
        p->setLabel("analyse");
        p->setHint("Measures the plate grain response (per channel, by luminance) and stores it in the comp.");
        page->addChild(*p);
    }
    {
        StringParamDescriptor* p = desc.defineStringParam("analysis");
        p->setLabel("analysis");
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "cellSize", "cell size", "Voronoi scatter cell size (pixels).", 60., 10., 300., 4., 5000.);
    APFXOfx::defineDouble(desc, page, "edgeBlend", "edge blend", "Blend between neighbouring cells (hides cell edges).", 0.5, 0., 1., 0., 1.);
    APFXOfx::defineDouble(desc, page, "replaceThreshold", "replace threshold", "The comp gets new grain where it differs from the plate by more than this.", 0.002, 0., 0.05, 0., 10.);
    APFXOfx::defineDouble(desc, page, "replaceSoftness", "replace softness", "Softness of the replace mask.", 0.01, 0.0001, 0.1, 0.00001, 10.);
    {
        RGBParamDescriptor* p = desc.defineRGBParam("responseGain");
        p->setLabel("adjust");
        p->setHint("Fine-tune the matched grain strength per channel.");
        p->setDefault(1., 1., 1.);
        p->setRange(0., 0., 0., 10., 10., 10.);
        p->setDisplayRange(0., 0., 0., 2., 2., 2.);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("output");
        p->setLabel("output");
        p->appendOption("result");
        p->appendOption("grain (QC)");
        p->appendOption("replace mask (QC)");
        p->setDefault(0);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("seed");
        p->setLabel("seed");
        p->setDefault(0);
        p->setRange(0, 1000000);
        p->setDisplayRange(0, 100);
        p->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*p);
    }
    APFXOfx::defineBool(desc, page, "staticGrain", "static", "Same grain on every frame.", false);
    APFXOfx::defineDouble(desc, page, "amount", "amount", "Overall grain amount (add mode).", 1., 0., 2., 0., 100.);
    APFXOfx::defineMaskMix(desc, page);
}

ImageEffect*
APFXGrainFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXGrainPlugin(h);
}
} // anon namespace

static APFXGrainFactory apfxGrainFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxGrainFactory)
