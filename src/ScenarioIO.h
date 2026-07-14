//=============================================================================
//  ScenarioIO.h
//-----------------------------------------------------------------------------
//  Public interface for persisting a Scenario to/from an INI file. Declares
//  the schema version, the Result status codes, and the Save/Load entry
//  points used by the editor to round-trip a scenario.ini.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include <string>

struct Scenario;

namespace ScenarioIO
{
    // Current scenario.ini schema version. Bumped on breaking changes;
    // additive optional keys do not require a bump.
    constexpr int kCurrentFormatVersion = 1;

    enum class Result
    {
        Ok,
        IoError,           // file could not be opened / written
        VersionTooNew,     // [Format] Version > kCurrentFormatVersion
        Malformed,         // required key missing or unparseable
    };

    // Write a scenario to an absolute scenario.ini path. Creates parent
    // directories as needed. Returns Ok on success.
    Result Save(const Scenario& scenario, const std::wstring& absoluteIniPath);

    // Load a scenario from an absolute scenario.ini path into `out`.
    // Missing optional keys fall back to the model defaults. Returns
    // VersionTooNew if the file's [Format] Version exceeds what we know;
    // Malformed if [Format] is missing entirely.
    Result Load(Scenario& out, const std::wstring& absoluteIniPath);
}
