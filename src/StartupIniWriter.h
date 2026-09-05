//=============================================================================
//  StartupIniWriter.h
//-----------------------------------------------------------------------------
//  Declares Write(), which emits the DISBrowser handoff file
//  <projectDir>\Config\Startup.ini. DISBrowser's Boot map reads it to open the
//  chosen level and, for the Generic level, override the EarthOrigin, basemap,
//  and painted terrain bounds. This is the only file-based coupling between
//  ScenarioEditor and DISBrowser.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-13
//=============================================================================
#pragma once

#include <string>

// Writes the DISBrowser handoff file <projectDir>\Config\Startup.ini, which DISBrowser's Unreal
// reads at boot: the Boot map's router opens the chosen level, and (for Generic) the level's
// manager overrides its EarthOrigin + basemap from the [Generic] section. This is the only
// file-based coupling between ScenarioEditor and DISBrowser; entity motion still flows over DIS/UDP.
namespace StartupIniWriter
{
    // Preview-tab Foliage selection, handed to DISBrowser as a trailing [Foliage]
    // block. Kept as plain strings so this header stays decoupled from Scenario.h;
    // palmKind = "All"|"Tall"|"Straight", renderMode = "InEngine"|"i3dm"|"BlenderGIS".
    // The block is emitted only when at least one tree type is selected.
    struct FoliageHandoff
    {
        bool        oak      = false;
        bool        bigTrees = false;
        bool        palm     = false;
        std::string palmKind   = "All";
        std::string renderMode = "InEngine";
    };

    // levelToken   : "Generic" | "Beach" | "Forest" | "Main" | "Hanger" | "GodView"
    // basemap      : "Satellite" | "Topographic" | "Cesium 3D (self-hosted)" | "None"
    //                (only consumed when level == "Generic"; the Cesium option -> Map=Custom + terrain URLs)
    // dynamicTiles : stream/cache tiles around the camera (only consumed when level == "Generic")
    // Returns true on success; on failure returns false and fills outError.
    // terrainBoundsValid + the lat/lon box are the painted 3D-terrain boundary (Preview tab). When
    // valid and level == "Generic" they are emitted as [Generic] TerrainBoundsLatMin/LatMax/LonMin/LonMax
    // so DISBrowser knows the terrain extent. Pass terrainBoundsValid=false to omit them.
    // cameraScheduleAbsPath : absolute path to the scenario.ini whose [Camera.N] sections are the
    //   schedule. Emitted as [ScenarioCameras] ScheduleFile for EVERY level (DISBrowser's director
    //   reads it regardless of the selected level). Pass "" to clear any stale schedule on the runtime.
    bool Write(const std::wstring& disBrowserProjectDir,
               const std::string&  levelToken,
               const std::string&  basemap,
               bool                dynamicTiles,
               double originLatDeg, double originLonDeg, double originAltMeters,
               bool   terrainBoundsValid,
               double terrainLatMinDeg, double terrainLatMaxDeg,
               double terrainLonMinDeg, double terrainLonMaxDeg,
               const std::wstring& cameraScheduleAbsPath,
               const FoliageHandoff& foliage,
               // Where DISBrowser should fetch self-hosted terrain from. Usually
               // localhost:8088, but [Terrain] Mode=Remote points it at another
               // machine, so this cannot be hardcoded.
               const std::string&  terrainHost,
               unsigned short      terrainPort,
               std::wstring&        outError);
}
