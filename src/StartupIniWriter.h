#pragma once

#include <string>

// Writes the DISBrowser handoff file <projectDir>\Config\Startup.ini, which DISBrowser's Unreal
// reads at boot: the Boot map's router opens the chosen level, and (for Generic) the level's
// manager overrides its EarthOrigin + basemap from the [Generic] section. This is the only
// file-based coupling between ScenarioEditor and DISBrowser; entity motion still flows over DIS/UDP.
namespace StartupIniWriter
{
    // levelToken   : "Generic" | "Beach" | "Forest" | "Main" | "Hanger" | "GodView"
    // basemap      : "Satellite" | "Topographic" | "Cesium 3D (self-hosted)" | "None"
    //                (only consumed when level == "Generic"; the Cesium option -> Map=Custom + terrain URLs)
    // dynamicTiles : stream/cache tiles around the camera (only consumed when level == "Generic")
    // Returns true on success; on failure returns false and fills outError.
    // terrainBoundsValid + the lat/lon box are the painted 3D-terrain boundary (Preview tab). When
    // valid and level == "Generic" they are emitted as [Generic] TerrainBoundsLatMin/LatMax/LonMin/LonMax
    // so DISBrowser knows the terrain extent. Pass terrainBoundsValid=false to omit them.
    bool Write(const std::wstring& disBrowserProjectDir,
               const std::string&  levelToken,
               const std::string&  basemap,
               bool                dynamicTiles,
               double originLatDeg, double originLonDeg, double originAltMeters,
               bool   terrainBoundsValid,
               double terrainLatMinDeg, double terrainLatMaxDeg,
               double terrainLonMinDeg, double terrainLonMaxDeg,
               std::wstring&        outError);
}
