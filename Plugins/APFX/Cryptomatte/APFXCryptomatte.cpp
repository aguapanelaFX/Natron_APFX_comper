/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX Cryptomatte (Nuke-like), following the Cryptomatte specification (Psyop, BSD-3):
//  - layers: EXR planes named <layer>00, <layer>01, ... each RGBA = (id, coverage, id, coverage)
//  - ids are float32 bit patterns of MurmurHash3_x86_32(name) with the exponent clamped to 1..254
//  - the manifest (name -> hex hash) comes from the EXR header attributes
//    cryptomatte/<key>/name, cryptomatte/<key>/manifest or cryptomatte/<key>/manif_file (sidecar json)
// Picking: Ctrl+click in the viewer adds the object under the cursor, Ctrl+Shift+click removes it.
// Output: matte in alpha (sum of the coverages of the selected ids); preview colours in rgb.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsInteract.h"
#include "ofxsMultiThread.h"
#include "ofxNatron.h"
#include "ofxKeySyms.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif

#ifndef OVERRIDE
#define OVERRIDE override
#endif
#ifndef FINAL
#define FINAL final
#endif

using namespace OFX;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.Cryptomatte"

// ------------------------------------------------------------------------------------------
// MurmurHash3_x86_32 (public domain, Austin Appleby) + Cryptomatte float conversion

inline uint32_t
rotl32(uint32_t x, int8_t r) { return (x << r) | (x >> (32 - r)); }

uint32_t
murmur3_32(const void* key, int len, uint32_t seed)
{
    const uint8_t* data = (const uint8_t*)key;
    const int nblocks = len / 4;
    uint32_t h1 = seed;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    for (int i = 0; i < nblocks; ++i) {
        uint32_t k1;
        std::memcpy(&k1, data + i * 4, 4);
        k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2;
        h1 ^= k1; h1 = rotl32(h1, 13); h1 = h1 * 5 + 0xe6546b64;
    }
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
    case 3: k1 ^= tail[2] << 16; // fall through
    case 2: k1 ^= tail[1] << 8;  // fall through
    case 1: k1 ^= tail[0];
        k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
    }
    h1 ^= (uint32_t)len;
    h1 ^= h1 >> 16; h1 *= 0x85ebca6b; h1 ^= h1 >> 13; h1 *= 0xc2b2ae35; h1 ^= h1 >> 16;

    return h1;
}

// name -> id bits (the float stored in the image, as bits)
uint32_t
nameToBits(const std::string& name)
{
    const uint32_t hash = murmur3_32( name.data(), (int)name.size(), 0 );
    uint32_t mantissa = hash & ( (1u << 23) - 1 );
    uint32_t exp = (hash >> 23) & 0xff;
    exp = std::max(exp, 1u);
    exp = std::min(exp, 254u);

    return (hash & 0x80000000u) | (exp << 23) | mantissa;
}

inline uint32_t
floatBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

std::string
bitsToHex(uint32_t b)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", b);

    return buf;
}

// ------------------------------------------------------------------------------------------
// manifest: EXR header attributes + minimal JSON

std::string
trim(const std::string& s)
{
    std::size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");

    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string
lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);

    return s;
}

// {"name": "hexhash", ...}
void
parseManifestJson(const std::string& js, std::map<uint32_t, std::string>* out)
{
    std::size_t i = 0;
    auto readString = [&](std::string* s) -> bool {
        while (i < js.size() && js[i] != '"') {
            ++i;
        }
        if (i >= js.size()) {
            return false;
        }
        ++i;
        s->clear();
        while (i < js.size() && js[i] != '"') {
            if ( (js[i] == '\\') && (i + 1 < js.size()) ) {
                ++i;
                const char c = js[i];
                if (c == 'n') { *s += '\n'; } else if (c == 't') { *s += '\t'; } else if (c == 'u' && i + 4 < js.size()) {
                    const unsigned v = (unsigned)std::strtoul(js.substr(i + 1, 4).c_str(), 0, 16);
                    if (v < 0x80) { *s += (char)v; } else if (v < 0x800) { *s += (char)(0xc0 | (v >> 6)); *s += (char)(0x80 | (v & 0x3f)); }
                    else { *s += (char)(0xe0 | (v >> 12)); *s += (char)(0x80 | ((v >> 6) & 0x3f)); *s += (char)(0x80 | (v & 0x3f)); }
                    i += 4;
                } else { *s += c; }
            } else {
                *s += js[i];
            }
            ++i;
        }
        ++i;

        return true;
    };
    std::string key, val;
    while ( readString(&key) ) {
        while (i < js.size() && js[i] != ':') {
            ++i;
        }
        if ( !readString(&val) ) {
            break;
        }
        const uint32_t bits = (uint32_t)std::strtoul(val.c_str(), 0, 16);
        (*out)[bits] = key;
    }
}

bool
readFile(const std::string& path, std::string* out)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();

    return true;
}

// reads the string attributes of every header of an OpenEXR file
bool
readExrStringAttributes(const std::string& path, std::map<std::string, std::string>* attrs)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    unsigned char hdr[8];
    if ( !f.read( (char*)hdr, 8 ) ) {
        return false;
    }
    if ( !(hdr[0] == 0x76 && hdr[1] == 0x2f && hdr[2] == 0x31 && hdr[3] == 0x01) ) {
        return false;
    }
    const bool multipart = (hdr[5] & 0x10) != 0; // version flag 0x1000
    auto readZ = [&](std::string* s) -> bool {
        s->clear();
        char c;
        while ( f.get(c) ) {
            if (c == 0) {
                return true;
            }
            *s += c;
            if (s->size() > 1024) {
                return false;
            }
        }

        return false;
    };
    for (int part = 0; part < 4096; ++part) {
        bool any = false;
        for (;;) {
            std::string name, type;
            if ( !readZ(&name) ) {
                return !attrs->empty();
            }
            if ( name.empty() ) {
                break; // end of this header
            }
            any = true;
            if ( !readZ(&type) ) {
                return !attrs->empty();
            }
            int32_t size = 0;
            if ( !f.read( (char*)&size, 4 ) || (size < 0) || (size > (1 << 30)) ) {
                return !attrs->empty();
            }
            if ( (type == "string") && (name.compare(0, 12, "cryptomatte/") == 0) ) {
                std::string v( (std::size_t)size, '\0' );
                f.read(&v[0], size);
                (*attrs)[name] = v;
            } else {
                f.seekg(size, std::ios::cur);
            }
        }
        if (!multipart || !any) {
            break;
        }
    }

    return true;
}

// manifest of one layer (base name, e.g. "CryptoObject") from an EXR file
bool
loadManifest(const std::string& exrPath, const std::string& layer, std::map<uint32_t, std::string>* out, std::string* status)
{
    std::map<std::string, std::string> attrs;
    if ( !readExrStringAttributes(exrPath, &attrs) ) {
        *status = "manifest: could not read " + exrPath;

        return false;
    }
    std::string key;
    for (std::map<std::string, std::string>::iterator it = attrs.begin(); it != attrs.end(); ++it) {
        const std::string& n = it->first;
        if ( (n.size() > 17) && (n.compare(n.size() - 5, 5, "/name") == 0) && (lower( trim(it->second) ) == lower(layer)) ) {
            key = n.substr(0, n.size() - 5); // cryptomatte/<key>
        }
    }
    if ( key.empty() ) {
        *status = "manifest: no cryptomatte metadata for " + layer + " in the file";

        return false;
    }
    std::map<std::string, std::string>::iterator m = attrs.find(key + "/manifest");
    if ( m != attrs.end() ) {
        parseManifestJson(m->second, out);
    } else {
        m = attrs.find(key + "/manif_file");
        if ( m == attrs.end() ) {
            *status = "manifest: none in the file";

            return false;
        }
        std::string dir = exrPath;
        const std::size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
        std::string js;
        if ( !readFile(dir + trim(m->second), &js) ) {
            *status = "manifest: sidecar not found: " + trim(m->second);

            return false;
        }
        parseManifestJson(js, out);
    }
    std::stringstream ss;
    ss << out->size() << " names in the manifest";
    *status = ss.str();

    return true;
}

// ------------------------------------------------------------------------------------------
// planes

struct CryptoPlane
{
    std::string ofxPlane; // string given by the host (fetchImagePlane)
    std::string name;     // e.g. CryptoObject00
};

std::string
planeNameOf(const std::string& ofx)
{
    const std::string pre = kNatronOfxImageComponentsPlaneName;
    if (ofx.compare(0, pre.size(), pre) != 0) {
        return std::string();
    }
    std::string rest = ofx.substr( pre.size() );
    std::size_t cut = rest.size();
    const char* seps[3] = { kNatronOfxImageComponentsPlaneLabel, kNatronOfxImageComponentsPlaneChannelsLabel, kNatronOfxImageComponentsPlaneChannel };
    for (int i = 0; i < 3; ++i) {
        const std::size_t p = rest.find(seps[i]);
        if (p != std::string::npos) {
            cut = std::min(cut, p);
        }
    }

    return rest.substr(0, cut);
}

// layer base names (planes ending with 2 digits, 4 channels) -> their planes in rank order
void
findLayers(const std::vector<std::string>& present, std::map<std::string, std::vector<CryptoPlane> >* layers)
{
    layers->clear();
    for (std::size_t i = 0; i < present.size(); ++i) {
        const std::string n = planeNameOf(present[i]);
        if ( (n.size() < 3) || !std::isdigit( (unsigned char)n[n.size() - 1] ) || !std::isdigit( (unsigned char)n[n.size() - 2] ) ) {
            continue;
        }
        if (lower(n).find("crypto") == std::string::npos) {
            continue;
        }
        CryptoPlane p;
        p.ofxPlane = present[i];
        p.name = n;
        (*layers)[ n.substr(0, n.size() - 2) ].push_back(p);
    }
    for (std::map<std::string, std::vector<CryptoPlane> >::iterator it = layers->begin(); it != layers->end(); ++it) {
        std::sort( it->second.begin(), it->second.end(), [](const CryptoPlane& a, const CryptoPlane& b) { return a.name < b.name; } );
    }
}

// preview colour of an id (stable, from its bits)
inline void
idColour(uint32_t b, float* c)
{
    uint32_t h = b * 2654435761u;
    c[0] = 0.15f + 0.85f * ( (h >> 8) & 0xff ) / 255.f;
    c[1] = 0.15f + 0.85f * ( (h >> 16) & 0xff ) / 255.f;
    c[2] = 0.15f + 0.85f * ( (h >> 24) & 0xff ) / 255.f;
}

// ------------------------------------------------------------------------------------------

class APFXCryptomattePlugin;

class CryptoInteract
    : public OverlayInteract
{
public:
    CryptoInteract(OfxInteractHandle h, ImageEffect* fx);

    virtual bool penDown(const PenArgs& args) OVERRIDE FINAL;
    virtual bool keyDown(const KeyArgs& args) OVERRIDE FINAL;
    virtual bool keyUp(const KeyArgs& args) OVERRIDE FINAL;
    virtual void loseFocus(const FocusArgs& args) OVERRIDE FINAL;

private:
    APFXCryptomattePlugin* _fx;
    bool _ctrl;
    bool _shift;
};

class APFXCryptomattePlugin
    : public ImageEffect
{
public:
    APFXCryptomattePlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _layer = fetchChoiceParam("layer");
        _layerName = fetchStringParam("layerName");
        _preview = fetchBooleanParam("preview");
        _matteOnly = fetchBooleanParam("matteOnly");
        _single = fetchBooleanParam("singleSelection");
        _unpremult = fetchBooleanParam("unpremultiply");
        _list = fetchStringParam("matteList");
        _clear = fetchPushButtonParam("clear");
        _manifestFile = fetchStringParam("manifestFile");
        _status = fetchStringParam("status");
        _pickPos = fetchDouble2DParam("pickPos");
        _pickTime = fetchDoubleParam("pickTime");
        _pickMode = fetchIntParam("pickMode");
        _pickCount = fetchIntParam("pickCount");
        refreshLayerMenu();
    }

    // SOLVFX: one undo step per pick (Ctrl+Z / Ctrl+Shift+Z in the viewer)
    bool undoPick(bool redo)
    {
        std::vector<std::string>& from = redo ? _redo : _undo;
        std::vector<std::string>& to = redo ? _undo : _redo;
        if ( from.empty() ) {
            return false;
        }
        std::string cur;
        _list->getValue(cur);
        to.push_back(cur);
        _restoring = true;
        _list->setValue( from.back() );
        _restoring = false;
        from.pop_back();

        return true;
    }

    // called by the interact (Ctrl+click / Ctrl+Shift+click)
    void requestPick(double time, const OfxPointD& pos, bool remove)
    {
        beginEditBlock("pick");
        _pickTime->setValue(time);
        _pickPos->setValue(pos.x, pos.y);
        _pickMode->setValue(remove ? 2 : 1);
        _pickCount->setValue( _pickCount->getValue() + 1 ); // triggers changedParam
        endEditBlock();
    }

private:
    // -------------------------------------------------------------------- layers / menu
    void currentLayers(std::map<std::string, std::vector<CryptoPlane> >* layers)
    {
        std::vector<std::string> present;
        if ( _src->isConnected() ) {
            _src->getPlanesPresent(&present);
        }
        findLayers(present, layers);
    }

    std::string selectedLayer(const std::map<std::string, std::vector<CryptoPlane> >& layers)
    {
        std::string name = _layerName->getValue();
        if ( layers.find(name) != layers.end() ) {
            return name;
        }
        // default: CryptoObject if present, else the first one
        for (std::map<std::string, std::vector<CryptoPlane> >::const_iterator it = layers.begin(); it != layers.end(); ++it) {
            if (lower(it->first).find("object") != std::string::npos) {
                return it->first;
            }
        }

        return layers.empty() ? std::string() : layers.begin()->first;
    }

    void refreshLayerMenu()
    {
        std::map<std::string, std::vector<CryptoPlane> > layers;
        currentLayers(&layers);
        const std::string sel = selectedLayer(layers);
        _layer->resetOptions();
        int idx = 0, i = 0;
        for (std::map<std::string, std::vector<CryptoPlane> >::iterator it = layers.begin(); it != layers.end(); ++it, ++i) {
            _layer->appendOption(it->first);
            if (it->first == sel) {
                idx = i;
            }
        }
        if ( layers.empty() ) {
            _layer->appendOption("(no cryptomatte layer)");
        }
        _layer->setValue(idx);
    }

    // -------------------------------------------------------------------- manifest
    const std::map<uint32_t, std::string>& manifest(const std::string& layer)
    {
        std::lock_guard<std::mutex> l(_manifestMutex);
        const std::string file = _manifestFile->getValue();
        if ( (file != _manifestLoadedFile) || (layer != _manifestLoadedLayer) ) {
            _names.clear();
            _manifestLoadedFile = file;
            _manifestLoadedLayer = layer;
            _manifestStatus.clear();
            if ( !file.empty() && !layer.empty() ) {
                loadManifest(file, layer, &_names, &_manifestStatus);
            } else {
                _manifestStatus = "no manifest (picks are shown as ids)";
            }
        }

        return _names;
    }

    // matte list text -> ids
    void parseList(const std::string& layer, std::vector<uint32_t>* ids)
    {
        ids->clear();
        const std::string s = _list->getValue();
        std::string item;
        std::stringstream ss(s);
        while ( std::getline(ss, item, ',') ) {
            item = trim(item);
            if ( item.empty() ) {
                continue;
            }
            if ( (item.size() > 2) && (item[0] == '<') && (item[item.size() - 1] == '>') ) {
                ids->push_back( (uint32_t)std::strtoul(item.substr(1, item.size() - 2).c_str(), 0, 16) );
            } else {
                if ( (item.size() >= 2) && (item[0] == '"') && (item[item.size() - 1] == '"') ) {
                    item = item.substr(1, item.size() - 2);
                }
                ids->push_back( nameToBits(item) );
            }
        }
        (void)layer;
        std::sort( ids->begin(), ids->end() );
        ids->erase( std::unique( ids->begin(), ids->end() ), ids->end() );
    }

    std::vector<std::string> listItems()
    {
        std::vector<std::string> v;
        std::stringstream ss( _list->getValue() );
        std::string item;
        while ( std::getline(ss, item, ',') ) {
            item = trim(item);
            if ( !item.empty() ) {
                v.push_back(item);
            }
        }

        return v;
    }

    void setListItems(const std::vector<std::string>& v)
    {
        std::string s;
        for (std::size_t i = 0; i < v.size(); ++i) {
            s += (i ? ", " : "") + v[i];
        }
        _list->setValue(s);
    }

    // -------------------------------------------------------------------- pick
    void doPick(const InstanceChangedArgs& args)
    {
        std::map<std::string, std::vector<CryptoPlane> > layers;
        currentLayers(&layers);
        const std::string layer = selectedLayer(layers);
        if ( layer.empty() ) {
            return;
        }
        const double t = _pickTime->getValue();
        double px, py;
        _pickPos->getValue(px, py);
        const std::vector<CryptoPlane>& planes = layers[layer];
        // rank 0 (highest coverage) is R/G of the first plane
        Image* img = _src->fetchImagePlane( t, planes[0].ofxPlane.c_str() );
        if (!img) {
            return;
        }
        const int x = (int)std::floor(px * args.renderScale.x), y = (int)std::floor(py * args.renderScale.y);
        const float* p = (const float*)img->getPixelAddress(x, y);
        uint32_t bits = 0;
        bool found = false;
        if (p && (img->getPixelDepth() == eBitDepthFloat) && (img->getPixelComponentCount() >= 2) && (p[1] > 0.f)) {
            bits = floatBits(p[0]);
            found = true;
        }
        delete img;
        if (!found) {
            return;
        }
        const std::map<uint32_t, std::string>& names = manifest(layer);
        std::map<uint32_t, std::string>::const_iterator n = names.find(bits);
        const std::string item = n != names.end() ? n->second : "<" + bitsToHex(bits) + ">";

        std::vector<std::string> items = _single->getValue() ? std::vector<std::string>() : listItems();
        // remove any entry with the same id
        std::vector<std::string> kept;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string& it = items[i];
            const uint32_t b = ( it.size() > 2 && it[0] == '<' ) ? (uint32_t)std::strtoul(it.substr(1, it.size() - 2).c_str(), 0, 16) : nameToBits(it);
            if (b != bits) {
                kept.push_back(it);
            }
        }
        if (_pickMode->getValue() != 2) {
            kept.push_back(item);
        }
        {
            std::string before;
            _list->getValue(before);
            _undo.push_back(before);
            if (_undo.size() > 200) {
                _undo.erase( _undo.begin() );
            }
            _redo.clear();
        }
        setListItems(kept);
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) OVERRIDE FINAL
    {
        if (name == "pickCount") {
            doPick(args);
        } else if (name == "clear") {
            _list->setValue("");
        } else if ( (name == "layer") && (args.reason == eChangeUserEdit) ) {
            std::map<std::string, std::vector<CryptoPlane> > layers;
            currentLayers(&layers);
            int i = 0;
            const int v = _layer->getValue();
            for (std::map<std::string, std::vector<CryptoPlane> >::iterator it = layers.begin(); it != layers.end(); ++it, ++i) {
                if (i == v) {
                    _layerName->setValue(it->first);
                }
            }
            updateStatus();
        } else if (name == "manifestFile") {
            updateStatus();
        }
    }

    void changedClip(const InstanceChangedArgs& /*args*/, const std::string& name) OVERRIDE FINAL
    {
        if (name == kOfxImageEffectSimpleSourceClipName) {
            refreshLayerMenu();
            updateStatus();
        }
    }

    void updateStatus()
    {
        std::map<std::string, std::vector<CryptoPlane> > layers;
        currentLayers(&layers);
        const std::string layer = selectedLayer(layers);
        if ( layer.empty() ) {
            _status->setValue("no Cryptomatte layers on the input (EXR planes like CryptoObject00)");

            return;
        }
        manifest(layer);
        std::stringstream ss;
        ss << layer << ": " << layers[layer].size() * 2 << " ranks · " << _manifestStatus;
        _status->setValue( ss.str() );
    }

    // -------------------------------------------------------------------- render
    OfxStatus getClipComponents(const ClipComponentsArguments& args, ClipComponentsSetter& cs) OVERRIDE FINAL
    {
        cs.addClipPlane(*_dst, kFnOfxImagePlaneColour);
        cs.addClipPlane(*_src, kFnOfxImagePlaneColour);
        std::map<std::string, std::vector<CryptoPlane> > layers;
        currentLayers(&layers);
        const std::string layer = selectedLayer(layers);
        if ( !layer.empty() ) {
            const std::vector<CryptoPlane>& planes = layers[layer];
            for (std::size_t i = 0; i < planes.size(); ++i) {
                cs.addClipPlane(*_src, planes[i].ofxPlane);
            }
        }
        cs.setPassThroughClip(_src, args.time, args.view);

        return kOfxStatOK;
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        cp.setClipComponents(*_src, ePixelComponentRGBA);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setOutputPremultiplication(eImageUnPreMultiplied);
    }

    bool isIdentity(const IsIdentityArguments& /*args*/, Clip*& /*identityClip*/, double& /*identityTime*/
#ifdef OFX_EXTENSIONS_NUKE
                    , int& /*view*/, std::string& /*plane*/
#endif
                    ) OVERRIDE FINAL
    {
        return false;
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        Image* dst = _dst->fetchImage(args.time);
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        Image* src = _src->isConnected() ? _src->fetchImage(args.time) : NULL;
        std::map<std::string, std::vector<CryptoPlane> > layers;
        currentLayers(&layers);
        const std::string layer = selectedLayer(layers);
        std::vector<Image*> crypto;
        if ( !layer.empty() ) {
            const std::vector<CryptoPlane>& planes = layers[layer];
            for (std::size_t i = 0; i < planes.size(); ++i) {
                Image* im = _src->fetchImagePlane( args.time, planes[i].ofxPlane.c_str() );
                if ( im && (im->getPixelDepth() == eBitDepthFloat) && (im->getPixelComponentCount() == 4) ) {
                    crypto.push_back(im);
                } else {
                    delete im;
                }
            }
        }
        std::vector<uint32_t> ids;
        parseList(layer, &ids);
        const bool preview = _preview->getValueAtTime(args.time);
        const bool matteOnly = _matteOnly->getValueAtTime(args.time);
        const bool unpremult = _unpremult->getValueAtTime(args.time);
        const int dnc = dst->getPixelComponentCount();
        const int snc = src ? src->getPixelComponentCount() : 0;
        const OfxRectI win = args.renderWindow;

        for (int y = win.y1; y < win.y2; ++y) {
            if ( abort() ) {
                break;
            }
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (!d) {
                continue;
            }
            for (int x = win.x1; x < win.x2; ++x, d += dnc) {
                float rgba[4] = { 0.f, 0.f, 0.f, 0.f };
                const float* s = src ? (const float*)src->getPixelAddress(x, y) : NULL;
                if (s) {
                    for (int c = 0; c < std::min(4, snc); ++c) {
                        rgba[c] = s[c];
                    }
                    if (snc == 3) {
                        rgba[3] = 1.f;
                    }
                }
                float matte = 0.f;
                float pc[3] = { 0.f, 0.f, 0.f };
                for (std::size_t k = 0; k < crypto.size(); ++k) {
                    const float* c = (const float*)crypto[k]->getPixelAddress(x, y);
                    if (!c) {
                        continue;
                    }
                    for (int r = 0; r < 2; ++r) {
                        const float cov = c[2 * r + 1];
                        if (cov == 0.f) {
                            continue;
                        }
                        const uint32_t b = floatBits(c[2 * r]);
                        if ( std::binary_search( ids.begin(), ids.end(), b ) ) {
                            matte += cov;
                        }
                        if (preview) {
                            float col[3];
                            idColour(b, col);
                            pc[0] += col[0] * cov; pc[1] += col[1] * cov; pc[2] += col[2] * cov;
                        }
                    }
                }
                matte = std::max(0.f, std::min(1.f, matte));
                float out[4];
                if (matteOnly) {
                    out[0] = out[1] = out[2] = out[3] = matte;
                } else {
                    out[0] = rgba[0]; out[1] = rgba[1]; out[2] = rgba[2];
                    if (unpremult && matte > 0.f) {
                        out[0] /= matte; out[1] /= matte; out[2] /= matte;
                    }
                    if (preview) {
                        // keyable surface: id colours over a dimmed image, selection highlighted
                        for (int c = 0; c < 3; ++c) {
                            out[c] = 0.35f * out[c] + 0.4f * pc[c] + 0.6f * matte;
                        }
                    }
                    out[3] = matte;
                }
                for (int c = 0; c < dnc && c < 4; ++c) {
                    d[c] = dnc == 1 ? out[3] : out[c];
                }
            }
        }
        for (std::size_t k = 0; k < crypto.size(); ++k) {
            delete crypto[k];
        }
        delete src;
        delete dst;
    }

    Clip* _dst;
    Clip* _src;
    ChoiceParam* _layer;
    StringParam* _layerName;
    BooleanParam* _preview;
    BooleanParam* _matteOnly;
    BooleanParam* _single;
    BooleanParam* _unpremult;
    StringParam* _list;
    PushButtonParam* _clear;
    StringParam* _manifestFile;
    StringParam* _status;
    Double2DParam* _pickPos;
    DoubleParam* _pickTime;
    IntParam* _pickMode;
    IntParam* _pickCount;
    std::vector<std::string> _undo, _redo;
    bool _restoring = false;

    std::mutex _manifestMutex;
    std::string _manifestLoadedFile, _manifestLoadedLayer, _manifestStatus;
    std::map<uint32_t, std::string> _names;
};

CryptoInteract::CryptoInteract(OfxInteractHandle h, ImageEffect* fx)
    : OverlayInteract(h)
    , _fx( dynamic_cast<APFXCryptomattePlugin*>(fx) )
    , _ctrl(false)
    , _shift(false)
{
}

bool
CryptoInteract::penDown(const PenArgs& args)
{
    if (!_ctrl || !_fx) {
        return false;
    }
    _fx->requestPick(args.time, args.penPosition, _shift);

    return true;
}

bool
CryptoInteract::keyDown(const KeyArgs& args)
{
    if ( _ctrl && ( (args.keySymbol == kOfxKey_z) || (args.keySymbol == kOfxKey_Z) ) ) {
        APFXCryptomattePlugin* fx = dynamic_cast<APFXCryptomattePlugin*>(_effect);
        if ( fx && fx->undoPick(_shift) ) {
            return true;
        }
    }
    if ( (args.keySymbol == kOfxKey_Control_L) || (args.keySymbol == kOfxKey_Control_R) ) {
        _ctrl = true;
    } else if ( (args.keySymbol == kOfxKey_Shift_L) || (args.keySymbol == kOfxKey_Shift_R) ) {
        _shift = true;
    }

    return false;
}

bool
CryptoInteract::keyUp(const KeyArgs& args)
{
    if ( (args.keySymbol == kOfxKey_Control_L) || (args.keySymbol == kOfxKey_Control_R) ) {
        _ctrl = false;
    } else if ( (args.keySymbol == kOfxKey_Shift_L) || (args.keySymbol == kOfxKey_Shift_R) ) {
        _shift = false;
    }

    return false;
}

void
CryptoInteract::loseFocus(const FocusArgs& /*args*/)
{
    _ctrl = _shift = false;
}

class CryptoOverlayDescriptor
    : public DefaultEffectOverlayDescriptor<CryptoOverlayDescriptor, CryptoInteract>
{
};

mDeclarePluginFactory(APFXCryptomatteFactory, {}, {});

void
APFXCryptomatteFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("Cryptomatte");
    desc.setPluginGrouping("Keyer");
    desc.setPluginDescription("APFX Cryptomatte. Pick objects in the viewer: Ctrl+click adds the object under the cursor, "
                              "Ctrl+Shift+click removes it. The matte (coverage-correct) goes to alpha. Names come from the "
                              "manifest in the EXR header (the upstream Read file is used automatically); without one, picks show as <id>. "
                              "Follows the Cryptomatte specification (Psyop, BSD-3).");
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
#ifdef OFX_EXTENSIONS_NUKE
    desc.setIsMultiPlanar(true);
    desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelPassThroughNonRenderedPlanes);
#endif
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
    desc.setOverlayInteractDescriptor(new CryptoOverlayDescriptor);
}

void
APFXCryptomatteFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->setSupportsTiles(true);
        c->setIsMask(false);
    }
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectOutputClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->setSupportsTiles(true);
    }
    PageParamDescriptor* page = desc.definePageParam("Cryptomatte");
    {
        StringParamDescriptor* p = desc.defineStringParam("status");
        p->setLabel("");
        p->setStringType(eStringTypeLabel);
        p->setDefault("");
        p->setIsPersistent(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("layer");
        p->setLabel("Layer Selection");
        p->setHint("Cryptomatte layer (EXR planes <layer>00, <layer>01, ...).");
        p->appendOption("(no cryptomatte layer)");
        p->setAnimates(false);
        p->setEvaluateOnChange(true);
        page->addChild(*p);
    }
    {
        StringParamDescriptor* p = desc.defineStringParam("layerName");
        p->setLabel("layerName");
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("preview");
        p->setLabel("Preview");
        p->setHint("Show every object in its own colour, selected ones highlighted.");
        p->setDefault(true);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("matteOnly");
        p->setLabel("Matte Only");
        p->setHint("Output the matte in rgb and alpha.");
        p->setDefault(false);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("singleSelection");
        p->setLabel("Single Selection");
        p->setHint("Each pick replaces the matte list.");
        p->setDefault(false);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam("unpremultiply");
        p->setLabel("Unpremultiply");
        p->setHint("Divide rgb by the matte.");
        p->setDefault(false);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintDivider);
        page->addChild(*p);
    }
    {
        StringParamDescriptor* p = desc.defineStringParam("matteList");
        p->setLabel("Matte List");
        p->setHint("Selected objects, comma separated (names from the manifest, or <hex id>). Ctrl+click in the viewer adds, Ctrl+Shift+click removes.");
        p->setStringType(eStringTypeMultiLine);
        p->setDefault("");
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        PushButtonParamDescriptor* p = desc.definePushButtonParam("clear");
        p->setLabel("Clear");
        p->setHint("Empty the matte list.");
        p->setLayoutHint(eLayoutHintDivider);
        page->addChild(*p);
    }
    {
        StringParamDescriptor* p = desc.defineStringParam("manifestFile");
        p->setLabel("Manifest File");
        p->setHint("EXR file whose header holds the manifest (filled automatically from the upstream Read).");
        p->setStringType(eStringTypeFilePath);
        p->setFilePathExists(true);
        p->setDefault("");
        p->setAnimates(false);
        page->addChild(*p);
    }
    // hidden pick data (written by the viewer interact)
    {
        Double2DParamDescriptor* p = desc.defineDouble2DParam("pickPos");
        p->setLabel("pickPos");
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        DoubleParamDescriptor* p = desc.defineDoubleParam("pickTime");
        p->setLabel("pickTime");
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("pickMode");
        p->setLabel("pickMode");
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("pickCount");
        p->setLabel("pickCount");
        p->setIsSecretAndDisabled(true);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        p->setIsPersistent(false);
        page->addChild(*p);
    }
}

ImageEffect*
APFXCryptomatteFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXCryptomattePlugin(h);
}
} // anon namespace

static APFXCryptomatteFactory apfxCryptomatteFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxCryptomatteFactory)
