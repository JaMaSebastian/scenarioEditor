//=============================================================================
//  ScenarioCameraIO.h
//-----------------------------------------------------------------------------
//  Reads a legacy standalone "<scenario>-camera.ini" camera schedule. As of the
//  single-file consolidation the schedule is saved INSIDE scenario.ini (see
//  ScenarioIO); this Load remains only to import old side-files during migration.
//  Save was retired — nothing should write the two-file layout anymore.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#pragma once

#include <string>

struct Scenario;

namespace ScenarioCameraIO
{
    // Current <scenario>-camera.ini schema version.
    constexpr int kCurrentFormatVersion = 1;

    enum class Result
    {
        Ok,
        IoError,           // file could not be written
        VersionTooNew,     // [Format] Version > kCurrentFormatVersion
        Malformed,         // [Format] present but unparseable
    };

    // Load a legacy side-file camera schedule into scenario.cameras (cleared
    // first). A missing file is not an error (Ok with an empty schedule). Does NOT
    // normalize the tiling — the caller runs NormalizeCameraSchedule() once the
    // model is live. Used only to migrate old "<scenario>-camera.ini" files; new
    // saves write the schedule inside scenario.ini via ScenarioIO.
    Result Load(Scenario& scenario, const std::wstring& absoluteCameraIniPath);
}
