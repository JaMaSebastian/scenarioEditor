//=============================================================================
//  CameraPresetCatalog.h
//-----------------------------------------------------------------------------
//  Declares CameraPresetCatalog, the in-memory list of saved camera views read
//  from config\Cameras.ini. Each preset is one [CameraViews.<Type>.<Angle>]
//  section (e.g. Octopus/front) carrying an X/Y/Z offset, Pitch/Yaw/Roll, FOV,
//  and a FocusCenter flag. The catalog is load-once (owned by theApp) and feeds
//  the Preview-tab "Camera" dropdown; nothing here mutates the file.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#pragma once

#include <string>
#include <vector>

//-----------------------------------------------------------------------------
// CameraPreset — one saved camera view (a [CameraViews.<Type>.<Angle>] section)
//   POD mirror of the Cameras.ini fields plus the two key parts that name it.
//-----------------------------------------------------------------------------
struct CameraPreset
{
    std::string type;    // <Type>  e.g. "Octopus", "ChineseDestroyer", "Skelmir S12"
    std::string angle;   // <Angle> e.g. "front", "back", "FarBack"

    double x = 0.0, y = 0.0, z = 0.0;          // position offset (model-local metres)
    double pitch = 0.0, yaw = 0.0, roll = 0.0; // rotation (degrees)
    double fov = 90.0;                         // field of view (degrees)
    int    focusCenter = 0;                    // optional flag

    // Body-relative gimbal envelope, authored in DISBrowser's hanger Settings tab.
    // We never edit these here — the editor only needs to know whether a camera HAS a
    // usable envelope, because that is what gates the per-frame "Gimbal limits" toggle.
    bool   hasLimits     = false;   // at least one Min*/Max*/LimitsEnabled key was present
    bool   limitsEnabled = false;   // LimitsEnabled=1

    // True when this preset can actually enforce anything. Defining an envelope and
    // switching it off are different states, and only the first is offerable.
    bool LimitsAvailable() const { return hasLimits && limitsEnabled; }

    // "Type / angle" — the label shown in the Preview-tab camera dropdown.
    std::string DisplayLabel() const;
};

//-----------------------------------------------------------------------------
// CameraPresetCatalog — read-only list of CameraPresets loaded from Cameras.ini
//   Preserves file order so the dropdown groups presets the way the file does.
//-----------------------------------------------------------------------------
class CameraPresetCatalog
{
public:
    // Clear and repopulate from an absolute path to Cameras.ini. Returns false
    // on a missing file (leaving the list empty); never throws.
    bool LoadFromIni(const std::wstring& path);

    const std::vector<CameraPreset>& Presets() const { return m_presets; }

    // Look a preset up the way a CameraFrame stores it — by type/angle string, not by
    // index. Returns nullptr when the mounted preset isn't in this catalog (a stale play,
    // or a camera authored in DISBrowser that this copy of Cameras.ini predates).
    const CameraPreset* Find(const std::string& type, const std::string& angle) const;

    bool   Loaded() const { return !m_presets.empty(); }
    size_t Count()  const { return m_presets.size(); }

private:
    std::vector<CameraPreset> m_presets;
};
