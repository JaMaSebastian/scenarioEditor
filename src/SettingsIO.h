#pragma once

#include <string>

// settings.ini — per-user UI state next to the executable (spec §11.5).
// Loaded once at app startup, saved once at clean shutdown. Atomic write
// (tmp file + rename) prevents corruption on a mid-write crash.

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
