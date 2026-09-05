//=============================================================================
//  CameraPresetCatalog.h
//-----------------------------------------------------------------------------
//  Declares CameraPresetCatalog, the in-memory list of saved camera views read
//  from config\Cameras.ini. Each preset is one [CameraViews.<Type>.<Angle>]
//  section (e.g. Octopus/front) carrying an X/Y/Z offset, Pitch/Yaw/Roll, FOV,
//  and a FocusCenter flag. The catalog feeds the Preview-tab "Camera" dropdown
//  and the New/Edit Camera dialog. Nothing in THIS file mutates Cameras.ini —
//  writing the gimbal envelope back is CameraPresetIO's job, and theApp's
//  ReloadCameraPresets() re-reads through here afterwards.
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

    // Body-relative gimbal envelope, authored either in DISBrowser's hanger Settings
    // tab or in this editor's New/Edit Camera dialog (which writes it back through
    // CameraPresetIO). hasLimits gates the per-frame "Gimbal limits" toggle; the six
    // angles are what the dialog edits and what DISBrowser actually clamps against.
    bool   hasLimits     = false;   // at least one Min*/Max*/LimitsEnabled key was present
    bool   limitsEnabled = false;   // LimitsEnabled=1

    // Degrees, body-relative, defaulting fully permissive to match DISBrowser's
    // FCameraGimbalLimits (CameraViewStore.h). Pitch is a plain interval held in
    // [-90,90]; yaw and roll are ARCS swept Min->Max increasing, so Min > Max is
    // legal and means the arc wraps through +/-180. Never "correct" a reversed
    // yaw/roll pair — that turns a 120-degree cone into a 240-degree one facing
    // the other way.
    double minPitch = -90.0,  maxPitch =  90.0;
    double minYaw   = -180.0, maxYaw   = 180.0;
    double minRoll  = -180.0, maxRoll  = 180.0;

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

//-----------------------------------------------------------------------------
// Authoring bounds for the gimbal envelope. These live here rather than beside
// Scenario.h's kZoom*/kTransition* constants because the envelope belongs to the
// PRESET, not to a camera frame — keeping the scenario model free of a preset
// dependency.
//
// Pitch is bounded by the engine, not by taste: DISBrowser canonicalises through
// FQuat::Rotator(), which only ever produces pitch in [-90,90] (CameraViewStore.cpp
// clamps to the same range on load). Yaw and roll take the full circle.
//-----------------------------------------------------------------------------
constexpr double kGimbalPitchMinDeg   =  -90.0;
constexpr double kGimbalPitchMaxDeg   =   90.0;
constexpr double kGimbalYawRollMinDeg = -180.0;
constexpr double kGimbalYawRollMaxDeg =  180.0;
