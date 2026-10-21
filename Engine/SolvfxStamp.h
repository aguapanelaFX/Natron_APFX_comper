/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (a Natron fork) <https://github.com/aguapanelaFX/Natron_APFX_comper>,
 * Copyright (C) AGUAPANELA FX
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// SOLVFX Stamps (Nuke "Stamps"-like):
//  - Anchor: a pass-through placed under a source node, carrying a title (its label)
//  - Stamp:  a pass-through wired to an Anchor whose link is never drawn, so it can
//            sit anywhere in the graph; it shows the Anchor's title
// Both are identities on their input (no render cost).

#ifndef Engine_SolvfxStamp_h
#define Engine_SolvfxStamp_h

#include "Engine/NoOpBase.h"
#include "Engine/EngineFwd.h"

#define PLUGINID_SOLVFX_ANCHOR "com.aguapanelafx.solvfx.Anchor"
#define PLUGINID_SOLVFX_STAMP  "com.aguapanelafx.solvfx.Stamp"

NATRON_NAMESPACE_ENTER

class SolvfxAnchor
    : public NoOpBase
{
public:

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new SolvfxAnchor(n);
    }

    SolvfxAnchor(NodePtr n)
        : NoOpBase(n)
    {
    }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return PLUGINID_SOLVFX_ANCHOR;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Anchor";
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Stamps anchor: put it under a node and give it a title (its label). "
               "Select it and press F10 to create Stamps: wireless copies of this stream to use anywhere in the graph.";
    }

    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Source";
    }
};

class SolvfxStamp
    : public NoOpBase
{
public:

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new SolvfxStamp(n);
    }

    SolvfxStamp(NodePtr n)
        : NoOpBase(n)
    {
    }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return PLUGINID_SOLVFX_STAMP;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Stamp";
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Stamp: a wireless copy of an Anchor's stream (the link is hidden). "
               "Right-click it for 'Go to anchor' and 'Reconnect to anchor'.";
    }

    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Anchor";
    }
};

inline bool
solvfxIsAnchorID(const std::string& id)
{
    return id == PLUGINID_SOLVFX_ANCHOR;
}

inline bool
solvfxIsStampID(const std::string& id)
{
    return id == PLUGINID_SOLVFX_STAMP;
}

NATRON_NAMESPACE_EXIT

#endif // Engine_SolvfxStamp_h
