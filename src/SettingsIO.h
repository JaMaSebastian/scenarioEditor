//=============================================================================
//  SettingsIO.h
//-----------------------------------------------------------------------------
//  Declares the persisted per-user UI state (window geometry, paths, Unreal
//  handoff target, Deploy column widths, saved map places) and the Load/Save
//  API that reads and atomically writes settings.ini next to the executable.
//  Anything that describes a SCENARIO rather than the user lives in
//  scenario.ini instead (see Scenario::preview).
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// settings.ini — per-user UI state next to the executable (spec §11.5).
// Loaded once at app startup, saved once at clean shutdown. Atomic write
// (tmp file + rename) prevents corruption on a mid-write crash.

// A named map location the user can pick from the Preview tab's Location
// drop-down. Persisted in settings.ini so add/delete survive across sessions.
struct MapPlace
{
    std::string label;
    double      lat = 0.0;
    double      lon = 0.0;
    double      alt = 0.0;   // eye/viewing altitude in metres (globe camera height)
};

// Which terrain tile server the Preview tab drives. Persisted as a name in
// settings.ini ([Terrain] Mode) so it is readable and hand-editable.
enum class TerrainServerMode : uint8_t
{
    Legacy  = 0,   // serve_terrain.cmd -> serve.py in WSL (console window)
    Local   = 1,   // native TerrainServer.exe under ProcessSupervisor
    Remote  = 2,   // a server on another machine; nothing is run here
    Service = 3,   // containerized Linux terrainserver: serves tiles AND builds
                   // them, driven over its /api/v1 job API. Nothing runs here.
};

//-----------------------------------------------------------------------------
// Settings — the complete persisted UI/app state, one instance per session.
//   Grouped to mirror the settings.ini sections ([Window], [Paths], [Unreal],
//   [Terrain], [Deploy], [Places]); members default to sensible first-run values.
//-----------------------------------------------------------------------------
struct Settings
{
    // [Window]
    int  windowX        = -1;   // -1 = "leave to OS / use default"
    int  windowY        = -1;
    int  windowWidth    = 2800;
    int  windowHeight   = 1850;
    bool windowMaximized = false;

    // [Paths]
    std::string lastScenarioPath;
    // Root of the DISBrowser Unreal project (the folder that contains Config\). The
    // "Configure Unreal" action on the Output tab writes <this>\Config\Startup.ini there.
    // Empty => resolved at use time to "..\DISBrowser" relative to the ScenarioEditor exe.
    std::string disBrowserProjectDir;

    // [Unreal] — target level + basemap pushed to DISBrowser via Config\Startup.ini.
    std::string unrealTargetLevel = "Generic";   // Generic|Beach|Forest|Main|Hanger|GodView
    std::string unrealBasemap     = "Satellite";  // Satellite|Topographic|None (used when level=Generic)
    bool        unrealDynamicTiles = false;       // stream/cache tiles around the camera in the generic level

    // [Terrain] — which terrain tile server the Preview tab drives, and where it is.
    //
    //   Legacy : the original serve_terrain.cmd -> serve.py inside WSL, in its own
    //            console window. Builds also revert to the original fire-and-forget
    //            ShellExecute behaviour, so this is a true fallback to the old code
    //            path if the newer one misbehaves.
    //   Local  : native TerrainServer.exe, run and monitored by ProcessSupervisor,
    //            serving tiles published to NTFS. No WSL, no console window.
    //   Remote : no server is run here at all; the tiles are served by another
    //            machine at terrainRemoteHost:terrainRemotePort, and that address is
    //            what gets written into DISBrowser's Startup.ini.
    //
    //   Service: the containerized Linux terrainserver at
    //            terrainRemoteHost:terrainRemotePort (default port 8089, so it can
    //            run alongside a native TerrainServer.exe on 8088). Builds are
    //            submitted to its HTTP job API instead of run locally through WSL.
    //
    // Host/port are consulted in Remote and Service modes only; Legacy and Local
    // always use localhost:8088 (the port both serve.py and TerrainServer.exe
    // bind). Resolve all four through ResolveTerrainEndpoint() in
    // TerrainEndpoint.h -- never re-derive the address at a call site.
    TerrainServerMode terrainMode       = TerrainServerMode::Local;
    std::string       terrainRemoteHost = "127.0.0.1";
    uint16_t          terrainRemotePort = 8088;

    // [Deploy] — persisted Deploy-tab list column widths (px), one per column.
    // Empty until the user first sets them; the Deploy page then hand-applies
    // and re-saves these instead of any hardcoded default layout.
    std::vector<int> deployColumnWidths;

    // NOTE: the Preview tab's display toggles used to live here as a [Preview]
    // section. They are per-SCENARIO state now (Scenario::preview, written to
    // scenario.ini) because which overlays are worth drawing depends on the
    // scenario, not the operator. Old settings.ini files may still carry the
    // dead [Preview] keys; nothing reads them.

    // [Places] — named map locations for the Preview tab's Location drop-down.
    std::vector<MapPlace> mapPlaces;
};

namespace SettingsIO
{
    constexpr int kCurrentFormatVersion = 1;

    // Read settings.ini at `absolutePath`. Missing file or missing keys
    // fall back to the struct's defaults; returns false in that case
    // (caller logs a single-line note and continues with defaults).
    bool Load(Settings& out, const std::wstring& absolutePath);

    // Write settings.ini atomically: writes <path>.tmp then ReplaceFile()s
    // it over `absolutePath`. Returns false on I/O failure.
    bool Save(const Settings& s, const std::wstring& absolutePath);
}
