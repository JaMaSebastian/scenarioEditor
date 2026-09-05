//=============================================================================
//  TerrainEndpoint.h  —  the single answer to "where is the terrain server?"
//-----------------------------------------------------------------------------
//  There used to be TWO copies of this decision that had already drifted:
//  CPreviewPage::TerrainHost/TerrainPort defaulted to "127.0.0.1", while
//  COutputPlaybackPage's inline ternaries (feeding StartupIniWriter) defaulted
//  to "localhost". Same intent, different strings, and nothing forcing them to
//  agree. Everything that probes the server, submits a job to it, or hands
//  DISBrowser a tile URL now goes through Resolve() so the four modes cannot
//  disagree about the address.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-20
//=============================================================================
#pragma once

#include <string>
#include <cstdint>

struct Settings;

//
// TerrainEndpoint - where the server is, and what we are allowed to do with it.
//   managedHere : this editor starts/stops the process (Local and Legacy only).
//                 Remote and Service are somebody else's lifecycle.
//   jobApi      : the server speaks /api/v1 and builds are submitted over HTTP
//                 instead of by running the WSL .cmd scripts locally.
//
struct TerrainEndpoint
{
    std::string host;
    uint16_t    port        = 8088;
    bool        managedHere = true;
    bool        jobApi      = false;
};

TerrainEndpoint ResolveTerrainEndpoint(const Settings& s);
