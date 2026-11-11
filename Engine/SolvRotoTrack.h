/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (based on Natron) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * SOLVFX is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef Engine_SolvRotoTrack_h
#define Engine_SolvRotoTrack_h

// SOLVFX SolvRoto: planar tracking inside a Roto node (Mocha / Silhouette basics).
//  - the selected shapes are the tracked region; shapes in a layer whose name contains
//    "exclude" are masked out (occluders)
//  - each tracked frame stores the 4 corners of the tracked quad (trackCorners); the
//    tracked shapes get the planar motion baked in their extra matrix (keyframes)
//  - the blue surface (4 corners + grid) shows where a corner pin / trackers will land;
//    drag its corners in the viewer
//  - export: CornerPin2D / Transform (linked or baked, current or reference frame), Tracker
// Tracking runs in a background thread with libmv region tracking (homography on the masked
// pattern, coarse-to-fine).

#include "Global/Macros.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <QtCore/QObject>

#include "Engine/EngineFwd.h"
#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

class RotoPaint;

class SolvRotoTrack
    : public QObject
{
public:
    explicit SolvRotoTrack(RotoPaint* effect);
    virtual ~SolvRotoTrack();

    // creates the "Track" page and the viewer buttons (called from RotoPaint::initializeKnobs)
    void createKnobs();

    // returns true when the knob belongs to SolvRoto (handled)
    bool knobChanged(KnobI* k);

    void drawOverlay(double time, double pixelScaleX, double pixelScaleY);
    bool penDown(double time, double x, double y, double pixelScaleX);
    bool penMotion(double time, double x, double y);
    bool penUp();

    static std::vector<std::string> exportOptions();

    // timeline: tracked frames of the SolvRoto whose keyframes are shown (drawn blue)
    static std::vector<double> visibleTrackedFrames();
    void publishTrackedFrames(bool shown);

private:
    struct Quad
    {
        double x[4], y[4];
    };

    bool getQuadAtTime(double time, Quad* q) const;     // tracked + adjust track offsets
    bool getRawQuadAtTime(double time, Quad* q) const;  // tracked only (tracker data)
    void addAdjust(double time, Quad* q) const;
    void reapplyAll();                                  // re-bake shapes + derived data after an adjust
    bool getRefQuad(Quad* q) const;
    void getSurfaceRef(Quad* q) const;
    void setSurfaceRef(const Quad& q);
    Quad surfaceAtTime(double time) const;
    static bool homography(const Quad& from, const Quad& to, Transform::Matrix3x3* H);

    void startTracking(int direction, bool oneFrame);
    void stopTracking();
    void onFrameTracked(int frame, Quad q, bool ok, double correlation);
    void onTrackingEnded(QString message);
    void applyFrame(int frame, const Quad& q);
    void refreshDerived();      // surface corners + transform knobs for every tracked frame
    void refreshDerivedAt(double frame);
    void bakeToSelectedShapes();
    void clearTrack(int which); // 0 all, -1 backward, +1 forward (from the current frame)
    void exportNode(int option);
    void exportRoto();
    std::list<RotoDrawableItemPtr> trackedShapes() const;
    void getTrackedFrames(std::vector<double>* frames) const;
    void redraw();

    RotoPaint* _effect;
    struct Knobs;
    std::unique_ptr<Knobs> _k;

    std::thread _worker;
    std::atomic<bool> _abort;
    std::atomic<bool> _running;
    int _dragCorner;
    bool _adjustDrag = false;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_SolvRotoTrack_h
