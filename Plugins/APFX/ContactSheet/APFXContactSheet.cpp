/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of the APFX tools for SOLVFX <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// APFX ContactSheet: all connected inputs on one image, never stretched. Every input keeps its own
// shape and pixel aspect (square, 16:9, 21:9, portrait, anamorphic ...). Auto layouts pick the rows /
// columns that show the images biggest (see APFXContactLayout.h). Downscaling is anti-aliased
// (pyramid), only the inputs inside the render window are read.

#include <cmath>
#include <vector>
#include <algorithm>

#include "APFXFlowOfx.h"
#include "APFXContactLayout.h"

using namespace OFX;
using APFXOps::Plane;

namespace {

#define kPluginIdentifier "com.aguapanelafx.APFX.ContactSheet"
#define kMaxInputs 32

enum FormatMode { eFormatProject = 0, eFormatCustom };
enum AreaMode { eAreaFormat = 0, eAreaBounds };

struct Item
{
    int clip;
    double time;
    OfxRectD src;     // canonical source rectangle shown
    double par;
};

class APFXContactSheetPlugin
    : public ImageEffect
{
public:
    APFXContactSheetPlugin(OfxImageEffectHandle h)
        : ImageEffect(h)
    {
        _dst = fetchClip(kOfxImageEffectOutputClipName);
        for (int i = 0; i < kMaxInputs; ++i) {
            char n[16];
            snprintf(n, sizeof(n), "%d", i + 1);
            _src.push_back( fetchClip(n) );
        }
        _format = fetchChoiceParam("formatMode");
        _size = fetchInt2DParam("size");
        _mode = fetchChoiceParam("layout");
        _rows = fetchIntParam("rows");
        _cols = fetchIntParam("columns");
        _fit = fetchChoiceParam("fit");
        _area = fetchChoiceParam("area");
        _gap = fetchDoubleParam("gap");
        _margin = fetchDoubleParam("margin");
        _center = fetchBooleanParam("center");
        _bg = fetchRGBAParam("background");
        _offset = fetchIntParam("frameOffset");
        _absolute = fetchBooleanParam("absoluteFrame");
        _report = fetchStringParam("report");
        refresh();
    }

private:
    void refresh()
    {
        _size->setEnabled(_format->getValue() == eFormatCustom);
        const int m = _mode->getValue();
        _rows->setEnabled(m == APFXCS::eModeGrid);
        _cols->setEnabled(m == APFXCS::eModeGrid);
        _fit->setEnabled(m != APFXCS::eModeRows);
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& n) OVERRIDE FINAL
    {
        if ( (n == "formatMode") || (n == "layout") ) {
            refresh();
        }
        if (n != "report") {
            updateReport(args.time);
        }
    }

    void changedClip(const InstanceChangedArgs& args, const std::string&) OVERRIDE FINAL
    {
        updateReport(args.time);
    }

    void outputSize(double* W, double* H) const
    {
        if (_format->getValue() == eFormatProject) {
            const OfxPointD s = getProjectSize();
            const double par = getProjectPixelAspectRatio();
            *W = std::max(1., std::floor(s.x / std::max(1e-6, par) + 0.5));   // project size is canonical
            *H = std::max(1., s.y);
        } else {
            int w, h;
            _size->getValue(w, h);
            *W = std::max(1, w);
            *H = std::max(1, h);
        }
    }

    double srcTime(double t) const
    {
        const int o = _offset->getValueAtTime(t);

        return _absolute->getValueAtTime(t) ? (double)o : t + o;
    }

    // connected inputs at time t with their source rectangles, and their placements
    void items(double t, std::vector<Item>& its, std::vector<APFXCS::Placement>& pl, int* rows = NULL, int* cols = NULL) const
    {
        its.clear();
        const double st = srcTime(t);
        const int area = _area->getValueAtTime(t);
        std::vector<double> asp, widths;
        for (int i = 0; i < kMaxInputs; ++i) {
            Clip* c = _src[i];
            if ( !c->isConnected() ) {
                continue;
            }
            Item it;
            it.clip = i;
            it.time = st;
            it.par = std::max(1e-6, c->getPixelAspectRatio() );
            OfxRectD r = c->getRegionOfDefinition(st);
            if (area == eAreaFormat) {
                OfxRectI f;
                c->getFormat(f);
                if ( (f.x2 > f.x1) && (f.y2 > f.y1) ) {
                    r.x1 = f.x1 * it.par;
                    r.x2 = f.x2 * it.par;
                    r.y1 = f.y1;
                    r.y2 = f.y2;
                }
            }
            if ( (r.x2 - r.x1 < 1.) || (r.y2 - r.y1 < 1.) ) {
                continue;
            }
            it.src = r;
            its.push_back(it);
            asp.push_back( (r.x2 - r.x1) / (r.y2 - r.y1) );   // canonical: pixel aspect included
            widths.push_back(r.x2 - r.x1);
        }
        double W, H;
        outputSize(&W, &H);
        APFXCS::Settings s;
        s.mode = _mode->getValueAtTime(t);
        s.fit = _fit->getValueAtTime(t);
        s.rows = _rows->getValueAtTime(t);
        s.cols = _cols->getValueAtTime(t);
        s.gap = std::max( 0., _gap->getValueAtTime(t) );
        s.margin = std::max( 0., _margin->getValueAtTime(t) );
        s.center = _center->getValueAtTime(t);
        pl = APFXCS::layout(asp, widths, W, H, s, rows, cols);
    }

    void getClipPreferences(ClipPreferencesSetter& cp) OVERRIDE FINAL
    {
        double W, H;
        outputSize(&W, &H);
        OfxRectI f = { 0, 0, (int)W, (int)H };
        cp.setOutputFormat(f);
        cp.setPixelAspectRatio(*_dst, 1.);
        cp.setClipComponents(*_dst, ePixelComponentRGBA);
        cp.setOutputPremultiplication(eImagePreMultiplied);
        cp.setOutputFrameVarying(true);
    }

    void updateReport(double t)
    {
        std::vector<Item> its;
        std::vector<APFXCS::Placement> pl;
        int r = 0, c = 0;
        items(t, its, pl, &r, &c);
        char buf[128];
        if ( its.empty() ) {
            snprintf(buf, sizeof(buf), "no inputs");
        } else if (c > 0) {
            snprintf(buf, sizeof(buf), "%d images, %d x %d grid", (int)its.size(), r, c);
        } else {
            snprintf(buf, sizeof(buf), "%d images in %d row%s", (int)its.size(), r, r == 1 ? "" : "s");
        }
        std::string cur;
        _report->getValue(cur);
        if (cur != buf) {
            _report->setValue(buf);
        }
    }

    bool getRegionOfDefinition(const RegionOfDefinitionArguments&, OfxRectD& rod) OVERRIDE FINAL
    {
        double W, H;
        outputSize(&W, &H);
        rod.x1 = 0.;
        rod.y1 = 0.;
        rod.x2 = W;
        rod.y2 = H;

        return true;
    }

    void getFramesNeeded(const FramesNeededArguments& args, FramesNeededSetter& frames) OVERRIDE FINAL
    {
        const double st = srcTime(args.time);
        OfxRangeD r = { st, st };
        for (int i = 0; i < kMaxInputs; ++i) {
            frames.setFramesNeeded(*_src[i], r);
        }
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL
    {
        std::vector<Item> its;
        std::vector<APFXCS::Placement> pl;
        items(args.time, its, pl);
        const OfxRectD& w = args.regionOfInterest;
        for (int i = 0; i < kMaxInputs; ++i) {
            OfxRectD none = { 0., 0., 0., 0. };
            rois.setRegionOfInterest(*_src[i], none);
        }
        for (size_t k = 0; k < its.size() && k < pl.size(); ++k) {
            const APFXCS::Rect& d = pl[k].dst;
            if ( (d.w() <= 0.) || (d.x2 <= w.x1) || (d.x1 >= w.x2) || (d.y2 <= w.y1) || (d.y1 >= w.y2) ) {
                continue;
            }
            rois.setRegionOfInterest(*_src[its[k].clip], its[k].src);
        }
    }

    void render(const RenderArguments& args) OVERRIDE FINAL
    {
        std::unique_ptr<Image> dst( _dst->fetchImage(args.time) );
        if (!dst) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        const OfxRectI win = args.renderWindow;
        const OfxPointD rs = args.renderScale;
        const int W = win.x2 - win.x1, H = win.y2 - win.y1;
        const APFXOps::ParallelFor pf = APFXOfx::hostParallelFor();
        double bg[4];
        _bg->getValueAtTime(args.time, bg[0], bg[1], bg[2], bg[3]);
        // background, premultiplied
        Plane out(W, H, 4);
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            out.d[i * 4 + 0] = (float)(bg[0] * bg[3]);
            out.d[i * 4 + 1] = (float)(bg[1] * bg[3]);
            out.d[i * 4 + 2] = (float)(bg[2] * bg[3]);
            out.d[i * 4 + 3] = (float)bg[3];
        }
        std::vector<Item> its;
        std::vector<APFXCS::Placement> pl;
        items(args.time, its, pl);
        for (size_t k = 0; k < its.size() && k < pl.size() && !abort(); ++k) {
            const Item& it = its[k];
            const APFXCS::Rect& D = pl[k].dst;
            const APFXCS::Rect& C = pl[k].clip;
            // output pixels covered (render scale)
            const int px0 = std::max( win.x1, (int)std::floor( std::max(D.x1, C.x1) * rs.x ) );
            const int px1 = std::min( win.x2, (int)std::ceil( std::min(D.x2, C.x2) * rs.x ) );
            const int py0 = std::max( win.y1, (int)std::floor( std::max(D.y1, C.y1) * rs.y ) );
            const int py1 = std::min( win.y2, (int)std::ceil( std::min(D.y2, C.y2) * rs.y ) );
            if ( (px1 <= px0) || (py1 <= py0) || (D.w() <= 0.) || (D.h() <= 0.) ) {
                continue;
            }
            std::unique_ptr<Image> img( _src[it.clip]->fetchImage(it.time) );
            if (!img) {
                continue;
            }
            const OfxRectI b = img->getBounds();
            img.reset();
            Plane lv0;
            if ( !APFXFlowOfx::fetchPlane(_src[it.clip], it.time, b, 4, lv0) || lv0.d.empty() ) {
                continue;
            }
            // source pixels per output pixel -> pyramid level (anti-aliased downscale)
            const double S_w = it.src.x2 - it.src.x1, S_h = it.src.y2 - it.src.y1;
            const double fx = (S_w / it.par) / D.w(), fy = S_h / D.h();
            const double f = std::max(fx, fy);
            int L = 0;
            while ( (f / (1 << (L + 1)) >= 1.) && (L < 12) ) {
                ++L;
            }
            const Plane* lvl = &lv0;
            Plane cur;
            if (L > 0) {
                cur = APFXOps::downsample(lv0, pf);
                for (int l = 1; l < L; ++l) {
                    cur = APFXOps::downsample(cur, pf);
                }
                lvl = &cur;
            }
            const double ls = 1. / (1 << L);
            const double sxp = rs.x / it.par;   // canonical x -> source pixels
            pf([&](int y0, int y1) {
                float c[4];
                for (int py = py0 + y0; py < py0 + y1; ++py) {
                    const double Y = (py + 0.5) / rs.y;
                    const double v = (Y - D.y1) / D.h();
                    const double sy = (it.src.y1 + v * S_h) * rs.y - b.y1;   // source pixel coordinate (edges)
                    if ( (sy < 0.) || (sy > b.y2 - b.y1) ) {
                        continue;
                    }
                    for (int px = px0; px < px1; ++px) {
                        const double X = (px + 0.5) / rs.x;
                        const double u = (X - D.x1) / D.w();
                        const double sx = (it.src.x1 + u * S_w) * sxp - b.x1;
                        if ( (sx < 0.) || (sx > b.x2 - b.x1) ) {
                            continue;
                        }
                        APFXFlow::samplePlane( *lvl, (float)(sx * ls - 0.5), (float)(sy * ls - 0.5), 0, c );
                        float* o = out.px(px - win.x1, py - win.y1);
                        const float a = std::max( 0.f, std::min(1.f, c[3]) );
                        for (int ch = 0; ch < 4; ++ch) {
                            o[ch] = c[ch] + o[ch] * (1.f - a);
                        }
                    }
                }
            }, py1 - py0);
        }
        const int nc = dst->getPixelComponentCount();
        for (int y = win.y1; y < win.y2; ++y) {
            float* d = (float*)dst->getPixelAddress(win.x1, y);
            if (!d) {
                continue;
            }
            const float* s = out.px(0, y - win.y1);
            for (int x = 0; x < W; ++x, d += nc, s += 4) {
                for (int ch = 0; ch < nc; ++ch) {
                    d[ch] = (nc == 1) ? s[3] : s[ch];
                }
            }
        }
    }

    Clip* _dst;
    std::vector<Clip*> _src;
    ChoiceParam* _format;
    Int2DParam* _size;
    ChoiceParam* _mode;
    IntParam* _rows;
    IntParam* _cols;
    ChoiceParam* _fit;
    ChoiceParam* _area;
    DoubleParam* _gap;
    DoubleParam* _margin;
    BooleanParam* _center;
    RGBAParam* _bg;
    IntParam* _offset;
    BooleanParam* _absolute;
    StringParam* _report;
};

mDeclarePluginFactory(APFXContactSheetFactory, {}, {});

void
APFXContactSheetFactory::describe(ImageEffectDescriptor& desc)
{
    APFXOfx::describeCommon(desc, "ContactSheet",
                            "APFX ContactSheet: every connected input on one image, never stretched: each keeps its own "
                            "shape and pixel aspect (square, 16:9, 21:9, portrait, anamorphic). layout rows (auto): "
                            "justified rows that fill the width, as many rows as show the images biggest. grid (auto): "
                            "equal cells, rows / columns picked automatically. grid: your rows / columns. Downscaling is "
                            "anti-aliased.");
    desc.setPluginGrouping("Merge");
    desc.setSupportsMultipleClipPARs(true);
    desc.setTemporalClipAccess(true);
}

void
APFXContactSheetFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    for (int i = 0; i < kMaxInputs; ++i) {
        char n[16];
        snprintf(n, sizeof(n), "%d", i + 1);
        ClipDescriptor* c = desc.defineClip(n);
        c->setLabel(n);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->addSupportedComponent(ePixelComponentRGB);
        c->addSupportedComponent(ePixelComponentAlpha);
        c->setTemporalClipAccess(true);
        c->setSupportsTiles(false);
        c->setOptional(true);
        c->setIsMask(false);
    }
    {
        ClipDescriptor* c = desc.defineClip(kOfxImageEffectOutputClipName);
        c->addSupportedComponent(ePixelComponentRGBA);
        c->setSupportsTiles(false);
    }
    PageParamDescriptor* page = desc.definePageParam("ContactSheet");
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("formatMode");
        p->setLabel("format");
        p->setHint("Size of the sheet: the comp format, or a custom size.");
        p->appendOption("comp format");
        p->appendOption("custom");
        p->setDefault(eFormatProject);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    {
        Int2DParamDescriptor* p = desc.defineInt2DParam("size");
        p->setLabel("");
        p->setHint("Custom sheet size (pixels).");
        p->setDefault(3840, 2160);
        p->setRange(1, 1, 65536, 65536);
        p->setDisplayRange(256, 256, 8192, 8192);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("layout");
        p->setLabel("layout");
        p->setHint("rows (auto): justified rows filling the width (best for mixed shapes). grid (auto): equal cells, rows / "
                   "columns picked to show the images biggest. grid: the rows / columns below (0 = automatic).");
        p->appendOption("rows (auto)");
        p->appendOption("grid (auto)");
        p->appendOption("grid");
        p->setDefault(APFXCS::eModeRows);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("rows");
        p->setLabel("rows");
        p->setHint("Grid rows (0 = from the columns).");
        p->setDefault(0);
        p->setRange(0, 64);
        p->setDisplayRange(0, 8);
        p->setAnimates(false);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("columns");
        p->setLabel("columns");
        p->setHint("Grid columns (0 = from the rows; both 0 = automatic).");
        p->setDefault(0);
        p->setRange(0, 64);
        p->setDisplayRange(0, 8);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("fit");
        p->setLabel("fit");
        p->setHint("Grid layouts: fit (whole image in its cell), fill (cell filled, the image cropped), original size. "
                   "Images are never stretched.");
        p->appendOption("fit");
        p->appendOption("fill");
        p->appendOption("original size");
        p->setDefault(APFXCS::eFitFit);
        p->setAnimates(false);
        page->addChild(*p);
    }
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam("area");
        p->setLabel("area");
        p->setHint("format: each input's format. image: each input's actual image bounds (after crops, transforms).");
        p->appendOption("format");
        p->appendOption("image");
        p->setDefault(eAreaFormat);
        p->setAnimates(false);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    APFXOfx::defineDouble(desc, page, "gap", "gap", "Space between images (pixels).", 16., 0., 100., 0., 10000.)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::defineDouble(desc, page, "margin", "margin", "Space around the sheet (pixels).", 16., 0., 200., 0., 10000.)
        ->setLayoutHint(eLayoutHintNoNewLine, 16);
    APFXOfx::defineBool(desc, page, "center", "centre", "Centre the images (off: top-left).", true);
    {
        RGBAParamDescriptor* p = desc.defineRGBAParam("background");
        p->setLabel("background");
        p->setHint("Sheet background.");
        p->setDefault(0., 0., 0., 0.);
        APFXOfx::divider(p);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam("frameOffset");
        p->setLabel("frame offset");
        p->setHint("Frame of the inputs: current frame + offset, or the offset itself when 'absolute' is on.");
        p->setDefault(0);
        p->setRange(-1000000, 1000000);
        p->setDisplayRange(-100, 100);
        p->setLayoutHint(eLayoutHintNoNewLine, 16);
        page->addChild(*p);
    }
    APFXOfx::divider( APFXOfx::defineBool(desc, page, "absoluteFrame", "absolute", "The offset is an absolute frame.", false) );
    {
        StringParamDescriptor* p = desc.defineStringParam("report");
        p->setLabel("");
        p->setStringType(eStringTypeLabel);
        p->setDefault("");
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        page->addChild(*p);
    }
}

ImageEffect*
APFXContactSheetFactory::createInstance(OfxImageEffectHandle h, ContextEnum)
{
    return new APFXContactSheetPlugin(h);
}
} // anon namespace

static APFXContactSheetFactory apfxContactSheetFactory(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(apfxContactSheetFactory)
