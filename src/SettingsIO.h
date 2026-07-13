#pragma once

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

    // [Deploy] — persisted Deploy-tab list column widths (px), one per column.
    // Empty until the user first sets them; the Deploy page then hand-applies
    // and re-saves these instead of any hardcoded default layout.
    std::vector<int> deployColumnWidths;

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
