//=============================================================================
//  TerrainEndpoint.cpp  —  see TerrainEndpoint.h
//=============================================================================
#include "pch.h"
#include "TerrainEndpoint.h"
#include "SettingsIO.h"

//
// ResolveTerrainEndpoint - one table, four modes.
//   Legacy/Local always serve from this machine on 8088 (the port both
//   serve.py and TerrainServer.exe bind). Remote and Service are elsewhere,
//   and only they consult the persisted host/port -- Service defaulting to
//   8089 so a container can run alongside a native TerrainServer.exe on 8088
//   instead of fighting it for the port.
//
TerrainEndpoint ResolveTerrainEndpoint(const Settings& s)
{
    TerrainEndpoint ep;

    switch (s.terrainMode)
    {
    case TerrainServerMode::Remote:
        ep.host        = s.terrainRemoteHost.empty() ? "127.0.0.1" : s.terrainRemoteHost;
        ep.port        = s.terrainRemotePort ? s.terrainRemotePort : 8088;
        ep.managedHere = false;
        ep.jobApi      = false;
        break;

    case TerrainServerMode::Service:
        ep.host        = s.terrainRemoteHost.empty() ? "127.0.0.1" : s.terrainRemoteHost;
        ep.port        = s.terrainRemotePort ? s.terrainRemotePort : 8089;
        ep.managedHere = false;
        ep.jobApi      = true;
        break;

    case TerrainServerMode::Legacy:
    case TerrainServerMode::Local:
    default:
        ep.host        = "127.0.0.1";
        ep.port        = 8088;
        ep.managedHere = true;
        ep.jobApi      = false;
        break;
    }

    return ep;
}
