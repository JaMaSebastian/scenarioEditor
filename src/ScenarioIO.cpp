//=============================================================================
//  ScenarioIO.cpp
//-----------------------------------------------------------------------------
//  Implements Save/Load for a Scenario against a Windows-profile (INI) file.
//  Uses the Win32 WritePrivateProfile*/GetPrivateProfile* APIs plus a set of
//  local helpers for UTF-8 <-> wide conversion, enum <-> name mapping, typed
//  read/write, directory creation, and Entity/Motion section naming. Handles
//  the [Format] version gate and reconstructs entities and their indexed
//  motion sections on load.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "ScenarioIO.h"
#include "Scenario.h"
#include "../log.h"

#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "Shlwapi.lib")

namespace
{
    // ---------- string utilities ----------

    //
    // Widen — convert a UTF-8 std::string to a std::wstring (UTF-16).
    //
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                            static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                              static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    //
    // Narrow — convert a std::wstring (UTF-16) back to a UTF-8 std::string.
    //
    std::string Narrow(const std::wstring& s)
    {
        if (s.empty()) return {};
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                            static_cast<int>(s.size()), nullptr, 0,
                                            nullptr, nullptr);
        std::string out(n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                              static_cast<int>(s.size()), out.data(), n,
                              nullptr, nullptr);
        return out;
    }

    // ---------- enum <-> name maps ----------

    //
    // ForceIdName — map a DIS Force ID (0-3) to its spec name string.
    //
    const wchar_t* ForceIdName(uint8_t id)
    {
        switch (id) {
            case 1:  return L"Friendly";
            case 2:  return L"Opposing";
            case 3:  return L"Neutral";
            default: return L"Other";
        }
    }
    //
    // ParseForceId — parse a Force ID name (or numeric 0-3) back to its id,
    // returning `fallback` if unrecognized.
    //
    uint8_t ParseForceId(const std::wstring& v, uint8_t fallback)
    {
        if (v == L"Friendly") return 1;
        if (v == L"Opposing") return 2;
        if (v == L"Neutral")  return 3;
        if (v == L"Other")    return 0;
        // Accept numeric fallback so older / hand-edited files still load.
        wchar_t* end = nullptr;
        const long n = std::wcstol(v.c_str(), &end, 10);
        if (end != v.c_str() && n >= 0 && n <= 3)
            return static_cast<uint8_t>(n);
        return fallback;
    }

    //
    // CoordModeName — map a CoordMode enum to its INI name string.
    //
    const wchar_t* CoordModeName(CoordMode m)
    {
        switch (m) {
            case CoordMode::LatLonAlt: return L"LatLonAlt";
            case CoordMode::Local:     return L"Local";
            case CoordMode::ECEF:      return L"ECEF";
        }
        return L"LatLonAlt";
    }
    //
    // ParseCoordMode — parse a CoordMode name back to the enum, else `fallback`.
    //
    CoordMode ParseCoordMode(const std::wstring& v, CoordMode fallback)
    {
        if (v == L"LatLonAlt") return CoordMode::LatLonAlt;
        if (v == L"Local")     return CoordMode::Local;
        if (v == L"ECEF")      return CoordMode::ECEF;
        return fallback;
    }

    //
    // PhysModeName — map a PhysicalModelMode enum to its INI name string.
    //
    const wchar_t* PhysModeName(PhysicalModelMode m)
    {
        switch (m) {
            case PhysicalModelMode::Ignore:   return L"Ignore";
            case PhysicalModelMode::Validate: return L"Validate";
            case PhysicalModelMode::Limit:    return L"Limit";
        }
        return L"Ignore";
    }
    //
    // ParsePhysMode — parse a PhysicalModelMode name back to the enum, else `fallback`.
    //
    PhysicalModelMode ParsePhysMode(const std::wstring& v, PhysicalModelMode fallback)
    {
        if (v == L"Ignore")   return PhysicalModelMode::Ignore;
        if (v == L"Validate") return PhysicalModelMode::Validate;
        if (v == L"Limit")    return PhysicalModelMode::Limit;
        return fallback;
    }

    //
    // PhysOverrideName — map a PhysicalModelOverride enum to its INI name string.
    //
    const wchar_t* PhysOverrideName(PhysicalModelOverride o)
    {
        switch (o) {
            case PhysicalModelOverride::Inherit:  return L"Inherit";
            case PhysicalModelOverride::Ignore:   return L"Ignore";
            case PhysicalModelOverride::Validate: return L"Validate";
            case PhysicalModelOverride::Limit:    return L"Limit";
        }
        return L"Inherit";
    }
    //
    // ParsePhysOverride — parse a PhysicalModelOverride name back to the enum, else `fallback`.
    //
    PhysicalModelOverride ParsePhysOverride(const std::wstring& v, PhysicalModelOverride fallback)
    {
        if (v == L"Inherit")  return PhysicalModelOverride::Inherit;
        if (v == L"Ignore")   return PhysicalModelOverride::Ignore;
        if (v == L"Validate") return PhysicalModelOverride::Validate;
        if (v == L"Limit")    return PhysicalModelOverride::Limit;
        return fallback;
    }

    //
    // MotionTypeName — map a MotionType enum to its INI name string.
    //
    const wchar_t* MotionTypeName(MotionType t)
    {
        switch (t) {
            case MotionType::Stationary: return L"Stationary";
            case MotionType::StopHold:   return L"StopHold";
            case MotionType::Line:       return L"Line";
            case MotionType::Ellipse:    return L"Ellipse";
        }
        return L"Stationary";
    }
    //
    // ParseMotionType — parse a MotionType name (incl. the "Stop" alias) back to
    // the enum, else `fallback`.
    //
    MotionType ParseMotionType(const std::wstring& v, MotionType fallback)
    {
        if (v == L"Stationary") return MotionType::Stationary;
        if (v == L"StopHold")   return MotionType::StopHold;
        if (v == L"Stop")       return MotionType::StopHold; // spec §11.2 alias
        if (v == L"Line")       return MotionType::Line;
        if (v == L"Ellipse")    return MotionType::Ellipse;
        return fallback;
    }

    //
    // OutputModeName — map an OutputMode enum to its INI name string.
    //
    const wchar_t* OutputModeName(OutputMode m)
    {
        switch (m) {
            case OutputMode::UdpUnicast:    return L"UdpUnicast";
            case OutputMode::UdpMulticast:  return L"UdpMulticast";
            case OutputMode::Tcp:           return L"Tcp";
            case OutputMode::FileRecording: return L"FileRecording";
            case OutputMode::PreviewOnly:   return L"PreviewOnly";
        }
        return L"UdpUnicast";
    }
    //
    // ParseOutputMode — parse an OutputMode name back to the enum, else `fallback`.
    //
    OutputMode ParseOutputMode(const std::wstring& v, OutputMode fallback)
    {
        if (v == L"UdpUnicast")    return OutputMode::UdpUnicast;
        if (v == L"UdpMulticast")  return OutputMode::UdpMulticast;
        if (v == L"Tcp")           return OutputMode::Tcp;
        if (v == L"FileRecording") return OutputMode::FileRecording;
        if (v == L"PreviewOnly")   return OutputMode::PreviewOnly;
        return fallback;
    }

    //
    // DirectionName — map an EllipseDirection enum to its INI name string.
    //
    const wchar_t* DirectionName(EllipseDirection d)
    {
        return d == EllipseDirection::Clockwise ? L"Clockwise" : L"CounterClockwise";
    }
    //
    // ParseDirection — parse an EllipseDirection name back to the enum, else `fb`.
    //
    EllipseDirection ParseDirection(const std::wstring& v, EllipseDirection fb)
    {
        if (v == L"Clockwise")        return EllipseDirection::Clockwise;
        if (v == L"CounterClockwise") return EllipseDirection::CounterClockwise;
        return fb;
    }

    // ---------- WPP helpers ----------

    //
    // WriteStr — write a string value under [section]Key= in the INI at `path`.
    //
    bool WriteStr(const wchar_t* section, const wchar_t* key, const wchar_t* value,
                  const std::wstring& path)
    {
        return ::WritePrivateProfileStringW(section, key, value, path.c_str()) != 0;
    }
    //
    // WriteStr — std::wstring overload; forwards to the const wchar_t* version.
    //
    bool WriteStr(const wchar_t* section, const wchar_t* key, const std::wstring& value,
                  const std::wstring& path)
    {
        return WriteStr(section, key, value.c_str(), path);
    }
    //
    // WriteInt — write a 64-bit integer as a decimal string under [section]Key=.
    //
    bool WriteInt(const wchar_t* section, const wchar_t* key, long long value,
                  const std::wstring& path)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"%lld", value);
        return WriteStr(section, key, buf, path);
    }
    //
    // WriteDouble — write a double under [section]Key= using round-trip-safe %.17g.
    //
    bool WriteDouble(const wchar_t* section, const wchar_t* key, double value,
                     const std::wstring& path)
    {
        // %.17g is round-trip safe for IEEE 754 doubles.
        wchar_t buf[64];
        swprintf_s(buf, L"%.17g", value);
        return WriteStr(section, key, buf, path);
    }

    //
    // ReadStr — read [section]Key= as a string, returning `defaultValue` if absent.
    //
    std::wstring ReadStr(const wchar_t* section, const wchar_t* key,
                         const wchar_t* defaultValue, const std::wstring& path)
    {
        wchar_t buf[2048];
        const DWORD n = ::GetPrivateProfileStringW(section, key, defaultValue,
                                                   buf, _countof(buf), path.c_str());
        return std::wstring(buf, n);
    }
    //
    // ReadInt — read [section]Key= as a 64-bit integer, returning `defaultValue`
    // if absent or unparseable.
    //
    long long ReadInt(const wchar_t* section, const wchar_t* key,
                      long long defaultValue, const std::wstring& path)
    {
        const std::wstring s = ReadStr(section, key, L"", path);
        if (s.empty()) return defaultValue;
        wchar_t* end = nullptr;
        const long long v = std::wcstoll(s.c_str(), &end, 10);
        return (end == s.c_str()) ? defaultValue : v;
    }
    //
    // ReadDouble — read [section]Key= as a double, returning `defaultValue`
    // if absent or unparseable.
    //
    double ReadDouble(const wchar_t* section, const wchar_t* key,
                      double defaultValue, const std::wstring& path)
    {
        const std::wstring s = ReadStr(section, key, L"", path);
        if (s.empty()) return defaultValue;
        wchar_t* end = nullptr;
        const double v = std::wcstod(s.c_str(), &end);
        return (end == s.c_str()) ? defaultValue : v;
    }

    // ---------- path utilities ----------

    bool EnsureParentDirectoryExists(const std::wstring& filePath)
    {
        // SHCreateDirectoryEx creates intermediate directories. We need to
        // strip the trailing filename first.
        std::wstring dir = filePath;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return true; // current dir
        dir.resize(slash);
        if (dir.empty())
            return true;
        const int rc = ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS
            || rc == ERROR_FILE_EXISTS;
    }

    // ---------- per-entity section name ----------

    //
    // EntitySection — build the "Entity.<id>" INI section name for an entity.
    //
    std::wstring EntitySection(uint16_t entityId)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"Entity.%u", static_cast<unsigned>(entityId));
        return buf;
    }

    //
    // MotionSection — build the "Entity.<id>.Motion.<n>" section name (1-based n).
    //
    std::wstring MotionSection(uint16_t entityId, size_t motionIdx)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"Entity.%u.Motion.%u",
                   static_cast<unsigned>(entityId),
                   static_cast<unsigned>(motionIdx + 1));
        return buf;
    }

    // Count the dots in a section name, to distinguish "Entity.<N>" (1)
    // from "Entity.<N>.Motion.<M>" (3).
    int CountDots(const wchar_t* s)
    {
        int n = 0;
        for (const wchar_t* p = s; *p; ++p) if (*p == L'.') ++n;
        return n;
    }

    // CameraSection — build the "Camera.<n>" INI section name (1-based n).
    std::wstring CameraSection(size_t idx)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"Camera.%u", static_cast<unsigned>(idx + 1));
        return buf;
    }

    // ---------- camera enum <-> name (mirror of ScenarioCameraIO) ----------
    const wchar_t* KindName(CameraKind k)
    {
        return (k == CameraKind::Stationary) ? L"Stationary" : L"Entity";
    }
    CameraKind ParseKind(const std::wstring& v, CameraKind fb)
    {
        if (v == L"Stationary") return CameraKind::Stationary;
        if (v == L"Entity")     return CameraKind::Entity;
        return fb;
    }
    const wchar_t* TransName(CameraTransition t)
    {
        return (t == CameraTransition::HardCut) ? L"HardCut" : L"CrossfadeBlend";
    }
    CameraTransition ParseTrans(const std::wstring& v, CameraTransition fb)
    {
        if (v == L"HardCut")        return CameraTransition::HardCut;
        if (v == L"CrossfadeBlend") return CameraTransition::CrossfadeBlend;
        return fb;
    }

    // ---------- foliage enum <-> name ----------
    const wchar_t* PalmKindName(PalmKind k)
    {
        return (k == PalmKind::Tall)     ? L"Tall"
             : (k == PalmKind::Straight) ? L"Straight"
                                         : L"All";
    }
    PalmKind ParsePalmKind(const std::wstring& v, PalmKind fb)
    {
        if (v == L"All")      return PalmKind::All;
        if (v == L"Tall")     return PalmKind::Tall;
        if (v == L"Straight") return PalmKind::Straight;
        return fb;
    }
    const wchar_t* RenderModeName(FoliageRenderMode m)
    {
        return (m == FoliageRenderMode::I3dm)       ? L"i3dm"
             : (m == FoliageRenderMode::BlenderGIS) ? L"BlenderGIS"
                                                    : L"InEngine";
    }
    FoliageRenderMode ParseRenderMode(const std::wstring& v, FoliageRenderMode fb)
    {
        if (v == L"InEngine")   return FoliageRenderMode::InEngine;
        if (v == L"i3dm")       return FoliageRenderMode::I3dm;
        if (v == L"BlenderGIS") return FoliageRenderMode::BlenderGIS;
        return fb;
    }
}

//
// ScenarioIO::Save — serialize a Scenario to the INI at `path`.
//   Ensures the parent directory exists, deletes any pre-existing file so
//   removed keys don't linger, then writes [Format], [Scenario], [Origin],
//   optional [TerrainBounds]/[Level*], per-entity and per-motion sections,
//   and [Output], flushing the profile cache at the end. Returns IoError on
//   a write/mkdir failure, otherwise Ok.
//
ScenarioIO::Result ScenarioIO::Save(const Scenario& scenario,
                                    const std::wstring& path)
{
    if (!EnsureParentDirectoryExists(path))
    {
        sprintf_s(szError, sizeof(szError),
                  "ScenarioIO::Save: parent directory could not be created");
        LOG(szError);
        return Result::IoError;
    }

    // GetPrivateProfile* keeps the file open across calls in a cache that
    // gets flushed on demand. Wipe any pre-existing file so removed keys
    // don't ghost into the saved output.
    ::DeleteFileW(path.c_str());

    if (!WriteInt(L"Format", L"Version", kCurrentFormatVersion, path))
        return Result::IoError;

    WriteStr   (L"Scenario", L"Name",                Widen(scenario.name), path);
    WriteStr   (L"Scenario", L"Description",         Widen(scenario.description), path);
    WriteInt   (L"Scenario", L"DISVersion",          scenario.protocolVersion, path);
    WriteInt   (L"Scenario", L"ExerciseID",          scenario.exerciseId, path);
    WriteInt   (L"Scenario", L"SiteID",              scenario.siteId, path);
    WriteInt   (L"Scenario", L"ApplicationID",       scenario.applicationId, path);
    WriteDouble(L"Scenario", L"DurationSeconds",     scenario.durationSeconds, path);
    WriteDouble(L"Scenario", L"DefaultUpdateRateHz", scenario.defaultUpdateRateHz, path);
    WriteStr   (L"Scenario", L"DefaultCoordinateMode",
                CoordModeName(scenario.defaultCoordMode), path);
    WriteStr   (L"Scenario", L"DefaultPhysicalModel",
                PhysModeName(scenario.defaultPhysicalModel), path);
    WriteInt   (L"Scenario", L"SpeedMultiplierEnabled",
                scenario.speedMultiplierEnabled ? 1 : 0, path);
    WriteDouble(L"Scenario", L"SpeedMultiplier", scenario.speedMultiplier, path);

    WriteDouble(L"Origin", L"LatitudeDeg",    scenario.originLatDeg, path);
    WriteDouble(L"Origin", L"LongitudeDeg",   scenario.originLonDeg, path);
    WriteDouble(L"Origin", L"AltitudeMeters", scenario.originAltM, path);

    // ---- [TerrainBounds] painted 3D-terrain box (Preview tab). Only emitted once painted. ----
    if (scenario.terrainBoundsValid)
    {
        WriteInt   (L"TerrainBounds", L"Valid",     1, path);
        WriteDouble(L"TerrainBounds", L"LatMinDeg", scenario.terrainLatMinDeg, path);
        WriteDouble(L"TerrainBounds", L"LatMaxDeg", scenario.terrainLatMaxDeg, path);
        WriteDouble(L"TerrainBounds", L"LonMinDeg", scenario.terrainLonMinDeg, path);
        WriteDouble(L"TerrainBounds", L"LonMaxDeg", scenario.terrainLonMaxDeg, path);
    }

    // ---- [Level] terrain overlay (optional; consumed by the Preview tab) ----
    // Only emitted when enabled so vanilla scenarios stay free of the section.
    if (scenario.level.enabled)
    {
        WriteInt   (L"Level", L"Enabled",        1, path);
        WriteStr   (L"Level", L"Name",           Widen(scenario.level.name), path);
        WriteDouble(L"Level", L"SeaLevelMeters", scenario.level.seaLevelMeters, path);

        auto writeRect = [&](const wchar_t* sec, const LevelRect& r)
        {
            WriteInt   (sec, L"Enabled",        r.enabled ? 1 : 0, path);
            WriteDouble(sec, L"EastMinMeters",  r.eastMinM,  path);
            WriteDouble(sec, L"EastMaxMeters",  r.eastMaxM,  path);
            WriteDouble(sec, L"NorthMinMeters", r.northMinM, path);
            WriteDouble(sec, L"NorthMaxMeters", r.northMaxM, path);
        };
        writeRect(L"Level.Land",  scenario.level.land);
        writeRect(L"Level.Ocean", scenario.level.ocean);

        // Named sub-regions as indexed [Level.Zone<i>] sections.
        WriteInt(L"Level", L"ZoneCount",
                 static_cast<long long>(scenario.level.zones.size()), path);
        for (size_t i = 0; i < scenario.level.zones.size(); ++i)
        {
            const LevelZone& z = scenario.level.zones[i];
            wchar_t sec[32];
            swprintf_s(sec, L"Level.Zone%u", static_cast<unsigned>(i));
            wchar_t color[16];
            swprintf_s(color, L"0x%06X", z.colorRgb & 0xFFFFFFu);

            WriteInt   (sec, L"Enabled",         z.enabled ? 1 : 0,  path);
            WriteStr   (sec, L"Name",            Widen(z.name),      path);
            WriteDouble(sec, L"EastMinMeters",   z.eastMinM,         path);
            WriteDouble(sec, L"EastMaxMeters",   z.eastMaxM,         path);
            WriteDouble(sec, L"NorthMinMeters",  z.northMinM,        path);
            WriteDouble(sec, L"NorthMaxMeters",  z.northMaxM,        path);
            WriteDouble(sec, L"HeightMinMeters", z.heightMinMeters,  path);
            WriteDouble(sec, L"HeightMaxMeters", z.heightMaxMeters,  path);
            WriteStr   (sec, L"ColorRGB",        color,              path);
        }
    }

    for (const Entity& e : scenario.entities)
    {
        const std::wstring sec = EntitySection(e.entityId);
        const wchar_t* S = sec.c_str();

        WriteInt   (S, L"Enabled",        e.enabled ? 1 : 0, path);
        WriteStr   (S, L"Name",           Widen(e.name), path);
        WriteStr   (S, L"Description",    Widen(e.description), path);
        WriteStr   (S, L"ForceID",        ForceIdName(e.forceId), path);
        WriteInt   (S, L"SiteID",         e.siteId, path);
        WriteInt   (S, L"ApplicationID",  e.applicationId, path);
        WriteInt   (S, L"EntityID",       e.entityId, path);

        // Entity Type — numeric IDs for now. EntityTypeCatalog ingestion
        // (a later slice) will let us emit/accept named aliases on top.
        WriteInt   (S, L"Kind",           e.kind, path);
        WriteInt   (S, L"Domain",         e.domain, path);
        WriteInt   (S, L"Country",        e.country, path);
        WriteInt   (S, L"Category",       e.category, path);
        WriteInt   (S, L"Subcategory",    e.subcategory, path);
        WriteInt   (S, L"Specific",       e.specific, path);
        WriteInt   (S, L"Extra",          e.extra, path);

        WriteStr   (S, L"Marking",        Widen(e.marking), path);

        WriteDouble(S, L"BeginSecond",    e.beginSecond, path);
        WriteDouble(S, L"EndSecond",      e.endSecond, path);
        WriteDouble(S, L"UpdateRateHz",   e.updateRateHz, path);
        WriteDouble(S, L"InitialSpeedMetersPerSecond", e.initialSpeedMps, path);

        WriteStr   (S, L"PhysicalModelOverride",
                    PhysOverrideName(e.physicalModelOverride), path);
        WriteInt   (S, L"OverrideSpeedMultiplier",
                    e.overrideSpeedMultiplier ? 1 : 0, path);
        WriteDouble(S, L"SpeedMultiplier", e.speedMultiplier, path);

        WriteStr   (S, L"InitialCoordinateMode",
                    CoordModeName(e.initialCoordMode), path);
        WriteDouble(S, L"InitialLatitudeDeg",  e.lat, path);
        WriteDouble(S, L"InitialLongitudeDeg", e.lon, path);
        WriteDouble(S, L"InitialAltitudeMeters", e.alt, path);
        WriteDouble(S, L"InitialLocalX",       e.localX, path);
        WriteDouble(S, L"InitialLocalY",       e.localY, path);
        WriteDouble(S, L"InitialLocalZ",       e.localZ, path);
        WriteDouble(S, L"InitialEcefX",        e.ecefX, path);
        WriteDouble(S, L"InitialEcefY",        e.ecefY, path);
        WriteDouble(S, L"InitialEcefZ",        e.ecefZ, path);
        WriteDouble(S, L"InitialHeadingDeg",   e.headingDeg, path);
        WriteDouble(S, L"InitialPitchDeg",     e.pitchDeg, path);
        WriteDouble(S, L"InitialRollDeg",      e.rollDeg, path);

        // ---- Per-entity motion segments (spec §11.2) ----
        for (size_t mi = 0; mi < e.motionSegments.size(); ++mi)
        {
            const MotionSegment& m = e.motionSegments[mi];
            const std::wstring msec = MotionSection(e.entityId, mi);
            const wchar_t* M = msec.c_str();

            WriteInt   (M, L"Enabled",        m.enabled ? 1 : 0, path);
            WriteStr   (M, L"Type",           MotionTypeName(m.type), path);
            WriteDouble(M, L"StartSecond",    m.startSecond, path);
            WriteDouble(M, L"EndSecond",      m.endSecond,   path);
            WriteStr   (M, L"CoordinateMode", CoordModeName(m.coordMode), path);

            // Start pose
            WriteDouble(M, L"StartLatitudeDeg",  m.startLat, path);
            WriteDouble(M, L"StartLongitudeDeg", m.startLon, path);
            WriteDouble(M, L"StartAltitudeMeters", m.startAlt, path);
            WriteDouble(M, L"StartEcefX",        m.startEcefX, path);
            WriteDouble(M, L"StartEcefY",        m.startEcefY, path);
            WriteDouble(M, L"StartEcefZ",        m.startEcefZ, path);
            WriteDouble(M, L"StartLocalX",       m.startLocalX, path);
            WriteDouble(M, L"StartLocalY",       m.startLocalY, path);
            WriteDouble(M, L"StartLocalZ",       m.startLocalZ, path);
            WriteDouble(M, L"StartHeadingDeg",   m.startHeadingDeg, path);
            WriteDouble(M, L"StartPitchDeg",     m.startPitchDeg, path);
            WriteDouble(M, L"StartRollDeg",      m.startRollDeg, path);

            // End pose (Line)
            WriteDouble(M, L"EndLatitudeDeg",    m.endLat, path);
            WriteDouble(M, L"EndLongitudeDeg",   m.endLon, path);
            WriteDouble(M, L"EndAltitudeMeters", m.endAlt, path);
            WriteDouble(M, L"EndEcefX",          m.endEcefX, path);
            WriteDouble(M, L"EndEcefY",          m.endEcefY, path);
            WriteDouble(M, L"EndEcefZ",          m.endEcefZ, path);
            WriteDouble(M, L"EndLocalX",         m.endLocalX, path);
            WriteDouble(M, L"EndLocalY",         m.endLocalY, path);
            WriteDouble(M, L"EndLocalZ",         m.endLocalZ, path);
            WriteDouble(M, L"EndHeadingDeg",     m.endHeadingDeg, path);
            WriteDouble(M, L"EndPitchDeg",       m.endPitchDeg, path);
            WriteDouble(M, L"EndRollDeg",        m.endRollDeg, path);

            WriteStr   (M, L"SpeedMode",   Widen(m.speedMode),   path);
            WriteStr   (M, L"HeadingMode", Widen(m.headingMode), path);
            WriteInt   (M, L"AccelerateFromStop", m.accelerateFromStop ? 1 : 0, path);

            // Ellipse (two-foci + length form)
            WriteDouble(M, L"Focus1LatitudeDeg",    m.f1Lat, path);
            WriteDouble(M, L"Focus1LongitudeDeg",   m.f1Lon, path);
            WriteDouble(M, L"Focus1AltitudeMeters", m.f1Alt, path);
            WriteDouble(M, L"Focus1LocalX",         m.f1LocalX, path);
            WriteDouble(M, L"Focus1LocalY",         m.f1LocalY, path);
            WriteDouble(M, L"Focus1LocalZ",         m.f1LocalZ, path);
            WriteDouble(M, L"Focus1EcefX",          m.f1EcefX, path);
            WriteDouble(M, L"Focus1EcefY",          m.f1EcefY, path);
            WriteDouble(M, L"Focus1EcefZ",          m.f1EcefZ, path);
            WriteDouble(M, L"Focus2LatitudeDeg",    m.f2Lat, path);
            WriteDouble(M, L"Focus2LongitudeDeg",   m.f2Lon, path);
            WriteDouble(M, L"Focus2AltitudeMeters", m.f2Alt, path);
            WriteDouble(M, L"Focus2LocalX",         m.f2LocalX, path);
            WriteDouble(M, L"Focus2LocalY",         m.f2LocalY, path);
            WriteDouble(M, L"Focus2LocalZ",         m.f2LocalZ, path);
            WriteDouble(M, L"Focus2EcefX",          m.f2EcefX, path);
            WriteDouble(M, L"Focus2EcefY",          m.f2EcefY, path);
            WriteDouble(M, L"Focus2EcefZ",          m.f2EcefZ, path);
            WriteDouble(M, L"LengthMeters",         m.lengthMeters, path);
            WriteDouble(M, L"StartBearingDeg",      m.startBearingDeg, path);
            WriteDouble(M, L"SpeedMetersPerSecond", m.speedMps, path);
            WriteStr   (M, L"Direction",            DirectionName(m.direction), path);
            WriteInt   (M, L"FollowTargetEntityID", m.followEntityId, path);

            WriteStr   (M, L"Description",     Widen(m.description), path);
        }
    }

    // ---- [Output] section (spec §11.2) ----
    {
        const OutputConfig& o = scenario.output;
        WriteStr   (L"Output", L"Mode",              OutputModeName(o.mode), path);
        WriteStr   (L"Output", L"UnicastIP",         Widen(o.unicastIp), path);
        WriteInt   (L"Output", L"UnicastPort",       o.unicastPort, path);
        WriteStr   (L"Output", L"MulticastGroup",    Widen(o.multicastGroup), path);
        WriteInt   (L"Output", L"MulticastPort",     o.multicastPort, path);
        WriteStr   (L"Output", L"MulticastInterface", Widen(o.multicastInterface), path);
        WriteInt   (L"Output", L"TTL",               o.multicastTtl, path);
        WriteInt   (L"Output", L"LoopbackEnabled",   o.multicastLoopback ? 1 : 0, path);
        WriteInt   (L"Output", L"TcpListen",         o.tcpListen ? 1 : 0, path);
        WriteStr   (L"Output", L"TcpRemoteHost",     Widen(o.tcpRemoteHost), path);
        WriteInt   (L"Output", L"TcpRemotePort",     o.tcpRemotePort, path);
        WriteInt   (L"Output", L"TcpListenPort",     o.tcpListenPort, path);
        WriteInt   (L"Output", L"TcpReconnect",      o.tcpReconnect ? 1 : 0, path);
        WriteInt   (L"Output", L"TcpTimeoutMs",      o.tcpTimeoutMs, path);
        WriteStr   (L"Output", L"RecordingPath",     Widen(o.recordingPath), path);
        WriteStr   (L"Output", L"ReplayPath",        Widen(o.replayPath), path);
        WriteDouble(L"Output", L"PlaybackSpeed",     o.playbackSpeed, path);
        WriteInt   (L"Output", L"LoopEnabled",       o.loopEnabled ? 1 : 0, path);
    }

    // ---- [Camera.N] scenario camera schedule (Preview tab; played by DISBrowser) ----
    // Lives inside scenario.ini (no separate side-file). One 1-based section per
    // frame; DISBrowser's reader keeps only [Camera.N] sections and ignores the rest.
    for (size_t i = 0; i < scenario.cameras.size(); ++i)
    {
        const CameraFrame& c = scenario.cameras[i];
        const std::wstring S = CameraSection(i);
        const wchar_t* C = S.c_str();

        WriteStr(C, L"Kind", KindName(c.kind), path);
        if (c.kind == CameraKind::Entity)
        {
            WriteInt(C, L"SourceEntityID", c.sourceEntityId, path);
            WriteStr(C, L"PresetType",  Widen(c.presetType),  path);
            WriteStr(C, L"PresetAngle", Widen(c.presetAngle), path);
            // Entity-only: a stationary vantage has no airframe to be body-relative to.
            WriteInt(C, L"LimitsEnabled", c.limitsEnabled ? 1 : 0, path);
        }
        else
        {
            WriteDouble(C, L"VantEastMeters",  c.vantEastM,  path);
            WriteDouble(C, L"VantNorthMeters", c.vantNorthM, path);
            WriteDouble(C, L"VantUpMeters",    c.vantUpM,    path);
        }
        WriteInt   (C, L"TargetEntityID", c.targetEntityId, path);
        WriteStr   (C, L"Transition",     TransName(c.transition), path);
        WriteDouble(C, L"BeginSecond",    c.beginSecond, path);
        WriteDouble(C, L"EndSecond",      c.endSecond,   path);
        WriteStr   (C, L"Label",          Widen(c.label), path);

        // Zoom (both kinds). Always emitted so a play saved by this build is
        // self-describing; DISBrowser defaults any key it can't find, and older
        // editor builds ignore what they don't know.
        WriteInt   (C, L"ZoomEnabled",        c.zoomEnabled ? 1 : 0, path);
        WriteDouble(C, L"ZoomFovDeg",         c.zoomFovDeg,   path);
        WriteInt   (C, L"DynamicZoom",        c.dynamicZoom ? 1 : 0, path);
        WriteDouble(C, L"ZoomFillPercent",    c.zoomFillPct,  path);
        WriteDouble(C, L"TargetLengthMeters", c.targetLengthM, path);
        WriteDouble(C, L"TargetWidthMeters",  c.targetWidthM,  path);
        WriteDouble(C, L"TargetHeightMeters", c.targetHeightM, path);
    }

    // ---- [Foliage] Preview-tab tree selection (Preview "Foliage" dialog) ----
    // Only emitted when at least one tree pack is enabled so vanilla scenarios
    // stay free of the section. RenderMode is always written when present so the
    // chosen backend round-trips with the selection.
    {
        const FoliageConfig& fo = scenario.foliage;
        if (fo.oak || fo.bigTrees || fo.palm)
        {
            WriteInt(L"Foliage", L"Oak",      fo.oak      ? 1 : 0, path);
            WriteInt(L"Foliage", L"BigTrees", fo.bigTrees ? 1 : 0, path);
            WriteInt(L"Foliage", L"Palm",     fo.palm     ? 1 : 0, path);
            WriteStr(L"Foliage", L"PalmKind",   PalmKindName(fo.palmKind),     path);
            WriteStr(L"Foliage", L"RenderMode", RenderModeName(fo.renderMode), path);
        }
    }

    // Force the cache to disk.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    sprintf_s(szError, sizeof(szError), "ScenarioIO::Save wrote scenario.ini");
    LOG(szError);
    return Result::Ok;
}

//
// ScenarioIO::Load — deserialize the INI at `path` into `s`.
//   Verifies the file exists and its [Format] Version is known (returns
//   IoError / Malformed / VersionTooNew otherwise), resets `s` to defaults,
//   then reads scenario/origin/terrain/level fields, discovers all "Entity.<N>"
//   sections, and in a second pass attaches each entity's sorted "Motion.<M>"
//   segments plus the [Output] config. Guarantees at least one entity. Ok on
//   success.
//
ScenarioIO::Result ScenarioIO::Load(Scenario& s, const std::wstring& path)
{
    // GetPrivateProfile* will happily report defaults for a missing file;
    // detect that explicitly.
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        sprintf_s(szError, sizeof(szError),
                  "ScenarioIO::Load: file not found: %s",
                  Narrow(path).c_str());
        LOG(szError);
        return Result::IoError;
    }

    const long long version = ReadInt(L"Format", L"Version", -1, path);
    if (version < 0)
        return Result::Malformed;
    if (version > kCurrentFormatVersion)
        return Result::VersionTooNew;

    // Reset to defaults so unspecified keys fall back cleanly.
    s = Scenario{};

    s.name           = Narrow(ReadStr(L"Scenario", L"Name",        Widen(s.name).c_str(),        path));
    s.description    = Narrow(ReadStr(L"Scenario", L"Description", L"", path));
    s.protocolVersion = static_cast<uint8_t>(ReadInt(L"Scenario", L"DISVersion", s.protocolVersion, path));
    s.exerciseId     = static_cast<uint8_t> (ReadInt(L"Scenario", L"ExerciseID",    s.exerciseId, path));
    s.siteId         = static_cast<uint16_t>(ReadInt(L"Scenario", L"SiteID",        s.siteId, path));
    s.applicationId  = static_cast<uint16_t>(ReadInt(L"Scenario", L"ApplicationID", s.applicationId, path));
    s.durationSeconds      = ReadDouble(L"Scenario", L"DurationSeconds",     s.durationSeconds, path);
    s.defaultUpdateRateHz  = ReadDouble(L"Scenario", L"DefaultUpdateRateHz", s.defaultUpdateRateHz, path);
    s.defaultCoordMode     = ParseCoordMode(ReadStr(L"Scenario", L"DefaultCoordinateMode", L"", path),
                                            s.defaultCoordMode);
    s.defaultPhysicalModel = ParsePhysMode(ReadStr(L"Scenario", L"DefaultPhysicalModel", L"", path),
                                           s.defaultPhysicalModel);
    s.speedMultiplierEnabled = ReadInt(L"Scenario", L"SpeedMultiplierEnabled", 0, path) != 0;
    s.speedMultiplier      = ReadDouble(L"Scenario", L"SpeedMultiplier", 1.0, path);

    s.originLatDeg = ReadDouble(L"Origin", L"LatitudeDeg",    s.originLatDeg, path);
    s.originLonDeg = ReadDouble(L"Origin", L"LongitudeDeg",   s.originLonDeg, path);
    s.originAltM   = ReadDouble(L"Origin", L"AltitudeMeters", s.originAltM,   path);

    // ---- [TerrainBounds] painted 3D-terrain box (absent in vanilla scenarios → stays invalid) ----
    s.terrainBoundsValid = ReadInt(L"TerrainBounds", L"Valid", s.terrainBoundsValid ? 1 : 0, path) != 0;
    s.terrainLatMinDeg   = ReadDouble(L"TerrainBounds", L"LatMinDeg", s.terrainLatMinDeg, path);
    s.terrainLatMaxDeg   = ReadDouble(L"TerrainBounds", L"LatMaxDeg", s.terrainLatMaxDeg, path);
    s.terrainLonMinDeg   = ReadDouble(L"TerrainBounds", L"LonMinDeg", s.terrainLonMinDeg, path);
    s.terrainLonMaxDeg   = ReadDouble(L"TerrainBounds", L"LonMaxDeg", s.terrainLonMaxDeg, path);

    // ---- [Level] terrain overlay (absent in vanilla scenarios → stays off) ----
    s.level.enabled = ReadInt(L"Level", L"Enabled", s.level.enabled ? 1 : 0, path) != 0;
    s.level.name    = Narrow(ReadStr(L"Level", L"Name", Widen(s.level.name).c_str(), path));
    s.level.seaLevelMeters =
        ReadDouble(L"Level", L"SeaLevelMeters", s.level.seaLevelMeters, path);

    auto readRect = [&](const wchar_t* sec, LevelRect& r)
    {
        r.enabled   = ReadInt(sec, L"Enabled", r.enabled ? 1 : 0, path) != 0;
        r.eastMinM  = ReadDouble(sec, L"EastMinMeters",  r.eastMinM,  path);
        r.eastMaxM  = ReadDouble(sec, L"EastMaxMeters",  r.eastMaxM,  path);
        r.northMinM = ReadDouble(sec, L"NorthMinMeters", r.northMinM, path);
        r.northMaxM = ReadDouble(sec, L"NorthMaxMeters", r.northMaxM, path);
    };
    readRect(L"Level.Land",  s.level.land);
    readRect(L"Level.Ocean", s.level.ocean);

    // Named sub-regions ([Level.Zone<i>]); ZoneCount in [Level] bounds the scan.
    s.level.zones.clear();
    const long long zoneCount = ReadInt(L"Level", L"ZoneCount", 0, path);
    for (long long i = 0; i < zoneCount; ++i)
    {
        wchar_t sec[32];
        swprintf_s(sec, L"Level.Zone%u", static_cast<unsigned>(i));

        LevelZone z;
        z.enabled         = ReadInt(sec, L"Enabled", 1, path) != 0;
        z.name            = Narrow(ReadStr(sec, L"Name", L"", path));
        z.eastMinM        = ReadDouble(sec, L"EastMinMeters",   z.eastMinM,        path);
        z.eastMaxM        = ReadDouble(sec, L"EastMaxMeters",   z.eastMaxM,        path);
        z.northMinM       = ReadDouble(sec, L"NorthMinMeters",  z.northMinM,       path);
        z.northMaxM       = ReadDouble(sec, L"NorthMaxMeters",  z.northMaxM,       path);
        z.heightMinMeters = ReadDouble(sec, L"HeightMinMeters", z.heightMinMeters, path);
        z.heightMaxMeters = ReadDouble(sec, L"HeightMaxMeters", z.heightMaxMeters, path);
        // ColorRGB is "0xRRGGBB"; wcstoul with base 16 accepts the 0x prefix.
        const std::wstring colorStr = ReadStr(sec, L"ColorRGB", L"", path);
        if (!colorStr.empty())
            z.colorRgb = static_cast<uint32_t>(
                std::wcstoul(colorStr.c_str(), nullptr, 16)) & 0xFFFFFFu;

        s.level.zones.push_back(std::move(z));
    }

    // Discover Entity.* sections. GetPrivateProfileSectionNames returns a
    // double-null-terminated list of names.
    wchar_t namesBuf[16384];
    const DWORD nameLen = ::GetPrivateProfileSectionNamesW(namesBuf,
                                                           _countof(namesBuf),
                                                           path.c_str());
    s.entities.clear();
    for (DWORD i = 0; i < nameLen; )
    {
        const wchar_t* sec = namesBuf + i;
        const size_t len = std::wcslen(sec);
        i += static_cast<DWORD>(len + 1);
        if (len == 0) continue;

        // Match "Entity.<N>" but not "Entity.<N>.Motion.<M>"
        if (std::wcsncmp(sec, L"Entity.", 7) != 0) continue;
        if (std::wcschr(sec + 7, L'.') != nullptr) continue;

        Entity e;  // start from defaults
        e.enabled        = ReadInt(sec, L"Enabled", e.enabled ? 1 : 0, path) != 0;
        e.name           = Narrow(ReadStr(sec, L"Name",        Widen(e.name).c_str(), path));
        e.description    = Narrow(ReadStr(sec, L"Description", L"", path));
        e.forceId        = ParseForceId(ReadStr(sec, L"ForceID", ForceIdName(e.forceId), path),
                                        e.forceId);

        e.siteId         = static_cast<uint16_t>(ReadInt(sec, L"SiteID",        e.siteId, path));
        e.applicationId  = static_cast<uint16_t>(ReadInt(sec, L"ApplicationID", e.applicationId, path));
        e.entityId       = static_cast<uint16_t>(ReadInt(sec, L"EntityID",      e.entityId, path));

        e.kind           = static_cast<uint8_t> (ReadInt(sec, L"Kind",        e.kind, path));
        e.domain         = static_cast<uint8_t> (ReadInt(sec, L"Domain",      e.domain, path));
        e.country        = static_cast<uint16_t>(ReadInt(sec, L"Country",     e.country, path));
        e.category       = static_cast<uint8_t> (ReadInt(sec, L"Category",    e.category, path));
        e.subcategory    = static_cast<uint8_t> (ReadInt(sec, L"Subcategory", e.subcategory, path));
        e.specific       = static_cast<uint8_t> (ReadInt(sec, L"Specific",    e.specific, path));
        e.extra          = static_cast<uint8_t> (ReadInt(sec, L"Extra",       e.extra, path));

        e.marking        = Narrow(ReadStr(sec, L"Marking", Widen(e.marking).c_str(), path));
        if (e.marking.size() > 11) e.marking.resize(11);

        e.beginSecond    = ReadDouble(sec, L"BeginSecond",  e.beginSecond, path);
        e.endSecond      = ReadDouble(sec, L"EndSecond",    e.endSecond, path);
        e.updateRateHz   = ReadDouble(sec, L"UpdateRateHz", e.updateRateHz, path);
        e.initialSpeedMps = ReadDouble(sec, L"InitialSpeedMetersPerSecond",
                                       e.initialSpeedMps, path);

        e.physicalModelOverride =
            ParsePhysOverride(ReadStr(sec, L"PhysicalModelOverride", L"", path),
                              e.physicalModelOverride);
        e.overrideSpeedMultiplier =
            ReadInt(sec, L"OverrideSpeedMultiplier", 0, path) != 0;
        e.speedMultiplier =
            ReadDouble(sec, L"SpeedMultiplier", e.speedMultiplier, path);

        e.initialCoordMode = ParseCoordMode(ReadStr(sec, L"InitialCoordinateMode", L"", path),
                                            e.initialCoordMode);
        e.lat            = ReadDouble(sec, L"InitialLatitudeDeg",    e.lat, path);
        e.lon            = ReadDouble(sec, L"InitialLongitudeDeg",   e.lon, path);
        e.alt            = ReadDouble(sec, L"InitialAltitudeMeters", e.alt, path);
        e.localX         = ReadDouble(sec, L"InitialLocalX",         e.localX, path);
        e.localY         = ReadDouble(sec, L"InitialLocalY",         e.localY, path);
        e.localZ         = ReadDouble(sec, L"InitialLocalZ",         e.localZ, path);
        e.ecefX          = ReadDouble(sec, L"InitialEcefX",          e.ecefX, path);
        e.ecefY          = ReadDouble(sec, L"InitialEcefY",          e.ecefY, path);
        e.ecefZ          = ReadDouble(sec, L"InitialEcefZ",          e.ecefZ, path);
        e.headingDeg     = ReadDouble(sec, L"InitialHeadingDeg",     e.headingDeg, path);
        e.pitchDeg       = ReadDouble(sec, L"InitialPitchDeg",       e.pitchDeg, path);
        e.rollDeg        = ReadDouble(sec, L"InitialRollDeg",        e.rollDeg, path);

        s.entities.push_back(e);
    }
    // Note: a file may legitimately carry zero entities; do NOT force-seed one.

    // ---- Second pass: gather motion segments under each entity. They live
    //      in Entity.<entityId>.Motion.<idx> sections. ----
    for (Entity& e : s.entities)
    {
        wchar_t prefix[64];
        swprintf_s(prefix, L"Entity.%u.Motion.",
                   static_cast<unsigned>(e.entityId));
        const size_t prefixLen = std::wcslen(prefix);

        struct IndexedSection { unsigned idx; std::wstring name; };
        std::vector<IndexedSection> motionSections;

        for (DWORD i = 0; i < nameLen; )
        {
            const wchar_t* sec = namesBuf + i;
            const size_t len = std::wcslen(sec);
            i += static_cast<DWORD>(len + 1);
            if (len == 0) continue;
            if (std::wcsncmp(sec, prefix, prefixLen) != 0) continue;

            wchar_t* end = nullptr;
            const unsigned long idx = std::wcstoul(sec + prefixLen, &end, 10);
            if (end == sec + prefixLen) continue;       // not a number
            if (end && *end != L'\0') continue;          // extra suffix; skip
            motionSections.push_back({ static_cast<unsigned>(idx), std::wstring(sec) });
        }
        std::sort(motionSections.begin(), motionSections.end(),
                  [](const auto& a, const auto& b){ return a.idx < b.idx; });

        for (const auto& msSec : motionSections)
        {
            const wchar_t* M = msSec.name.c_str();
            MotionSegment m;
            m.enabled        = ReadInt(M, L"Enabled", 1, path) != 0;
            m.type           = ParseMotionType(ReadStr(M, L"Type", L"Stationary", path),
                                               MotionType::Stationary);
            m.startSecond    = ReadDouble(M, L"StartSecond", 0.0, path);
            m.endSecond      = ReadDouble(M, L"EndSecond",   0.0, path);
            m.coordMode      = ParseCoordMode(ReadStr(M, L"CoordinateMode", L"LatLonAlt", path),
                                              CoordMode::LatLonAlt);

            m.startLat       = ReadDouble(M, L"StartLatitudeDeg",    0.0, path);
            m.startLon       = ReadDouble(M, L"StartLongitudeDeg",   0.0, path);
            m.startAlt       = ReadDouble(M, L"StartAltitudeMeters", 0.0, path);
            m.startEcefX     = ReadDouble(M, L"StartEcefX",          0.0, path);
            m.startEcefY     = ReadDouble(M, L"StartEcefY",          0.0, path);
            m.startEcefZ     = ReadDouble(M, L"StartEcefZ",          0.0, path);
            m.startLocalX    = ReadDouble(M, L"StartLocalX",         0.0, path);
            m.startLocalY    = ReadDouble(M, L"StartLocalY",         0.0, path);
            m.startLocalZ    = ReadDouble(M, L"StartLocalZ",         0.0, path);
            m.startHeadingDeg = ReadDouble(M, L"StartHeadingDeg",    0.0, path);
            m.startPitchDeg   = ReadDouble(M, L"StartPitchDeg",      0.0, path);
            m.startRollDeg    = ReadDouble(M, L"StartRollDeg",       0.0, path);

            m.endLat         = ReadDouble(M, L"EndLatitudeDeg",      0.0, path);
            m.endLon         = ReadDouble(M, L"EndLongitudeDeg",     0.0, path);
            m.endAlt         = ReadDouble(M, L"EndAltitudeMeters",   0.0, path);
            m.endEcefX       = ReadDouble(M, L"EndEcefX",            0.0, path);
            m.endEcefY       = ReadDouble(M, L"EndEcefY",            0.0, path);
            m.endEcefZ       = ReadDouble(M, L"EndEcefZ",            0.0, path);
            m.endLocalX      = ReadDouble(M, L"EndLocalX",           0.0, path);
            m.endLocalY      = ReadDouble(M, L"EndLocalY",           0.0, path);
            m.endLocalZ      = ReadDouble(M, L"EndLocalZ",           0.0, path);
            m.endHeadingDeg  = ReadDouble(M, L"EndHeadingDeg",       0.0, path);
            m.endPitchDeg    = ReadDouble(M, L"EndPitchDeg",         0.0, path);
            m.endRollDeg     = ReadDouble(M, L"EndRollDeg",          0.0, path);

            m.speedMode      = Narrow(ReadStr(M, L"SpeedMode",   L"CalculateFromTime", path));
            m.headingMode    = Narrow(ReadStr(M, L"HeadingMode", L"CalculateFromPath", path));
            m.accelerateFromStop = ReadInt(M, L"AccelerateFromStop", 0, path) != 0;

            m.f1Lat          = ReadDouble(M, L"Focus1LatitudeDeg",    0.0, path);
            m.f1Lon          = ReadDouble(M, L"Focus1LongitudeDeg",   0.0, path);
            m.f1Alt          = ReadDouble(M, L"Focus1AltitudeMeters", 0.0, path);
            m.f1LocalX       = ReadDouble(M, L"Focus1LocalX",         0.0, path);
            m.f1LocalY       = ReadDouble(M, L"Focus1LocalY",         0.0, path);
            m.f1LocalZ       = ReadDouble(M, L"Focus1LocalZ",         0.0, path);
            m.f1EcefX        = ReadDouble(M, L"Focus1EcefX",          0.0, path);
            m.f1EcefY        = ReadDouble(M, L"Focus1EcefY",          0.0, path);
            m.f1EcefZ        = ReadDouble(M, L"Focus1EcefZ",          0.0, path);
            m.f2Lat          = ReadDouble(M, L"Focus2LatitudeDeg",    0.0, path);
            m.f2Lon          = ReadDouble(M, L"Focus2LongitudeDeg",   0.0, path);
            m.f2Alt          = ReadDouble(M, L"Focus2AltitudeMeters", 0.0, path);
            m.f2LocalX       = ReadDouble(M, L"Focus2LocalX",         0.0, path);
            m.f2LocalY       = ReadDouble(M, L"Focus2LocalY",         0.0, path);
            m.f2LocalZ       = ReadDouble(M, L"Focus2LocalZ",         0.0, path);
            m.f2EcefX        = ReadDouble(M, L"Focus2EcefX",          0.0, path);
            m.f2EcefY        = ReadDouble(M, L"Focus2EcefY",          0.0, path);
            m.f2EcefZ        = ReadDouble(M, L"Focus2EcefZ",          0.0, path);
            m.lengthMeters   = ReadDouble(M, L"LengthMeters",         0.0, path);
            m.startBearingDeg = ReadDouble(M, L"StartBearingDeg",     0.0, path);
            m.speedMps       = ReadDouble(M, L"SpeedMetersPerSecond", 0.0, path);
            m.direction      = ParseDirection(ReadStr(M, L"Direction", L"Clockwise", path),
                                              EllipseDirection::Clockwise);
            m.followEntityId = static_cast<int>(ReadInt(M, L"FollowTargetEntityID", -1, path));

            m.description    = Narrow(ReadStr(M, L"Description", L"", path));

            e.motionSegments.push_back(std::move(m));
        }
    }

    // ---- [Output] section ----
    {
        OutputConfig& o = s.output;
        o.mode             = ParseOutputMode(ReadStr(L"Output", L"Mode", L"UdpUnicast", path),
                                             OutputMode::UdpUnicast);
        o.unicastIp        = Narrow(ReadStr(L"Output", L"UnicastIP", L"127.0.0.1", path));
        // Port fallbacks must match the in-memory defaults in Scenario.h (3001,
        // DISBrowser's [DIS] ListenPort). They used to read 3000, so any .ini
        // written without these keys silently moved output off DISBrowser's port.
        o.unicastPort      = static_cast<uint16_t>(ReadInt(L"Output", L"UnicastPort", 3001, path));
        o.multicastGroup   = Narrow(ReadStr(L"Output", L"MulticastGroup", L"224.252.0.1", path));
        o.multicastPort    = static_cast<uint16_t>(ReadInt(L"Output", L"MulticastPort", 3001, path));
        o.multicastInterface = Narrow(ReadStr(L"Output", L"MulticastInterface", L"0.0.0.0", path));
        o.multicastTtl     = static_cast<int>(ReadInt(L"Output", L"TTL", 1, path));
        o.multicastLoopback = ReadInt(L"Output", L"LoopbackEnabled", 1, path) != 0;
        o.tcpListen        = ReadInt(L"Output", L"TcpListen", 0, path) != 0;
        o.tcpRemoteHost    = Narrow(ReadStr(L"Output", L"TcpRemoteHost", L"127.0.0.1", path));
        o.tcpRemotePort    = static_cast<uint16_t>(ReadInt(L"Output", L"TcpRemotePort", 3002, path));
        o.tcpListenPort    = static_cast<uint16_t>(ReadInt(L"Output", L"TcpListenPort", 3002, path));
        o.tcpReconnect     = ReadInt(L"Output", L"TcpReconnect", 1, path) != 0;
        o.tcpTimeoutMs     = static_cast<int>(ReadInt(L"Output", L"TcpTimeoutMs", 3000, path));
        o.recordingPath    = Narrow(ReadStr(L"Output", L"RecordingPath", L"", path));
        o.replayPath       = Narrow(ReadStr(L"Output", L"ReplayPath",    L"", path));
        o.playbackSpeed    = ReadDouble(L"Output", L"PlaybackSpeed", 1.0, path);
        o.loopEnabled      = ReadInt(L"Output", L"LoopEnabled", 0, path) != 0;
    }

    // ---- [Camera.N] scenario camera schedule (absent in older/vanilla files) ----
    // Discover "Camera.<N>" sections (prefix "Camera." with exactly one dot) and
    // read them in numeric index order. The caller runs NormalizeCameraSchedule()
    // once the model is live, so no tiling/clamp is done here.
    {
        struct IndexedSection { unsigned idx; std::wstring name; };
        std::vector<IndexedSection> camSections;
        for (DWORD i = 0; i < nameLen; )
        {
            const wchar_t* sec = namesBuf + i;
            const size_t len = std::wcslen(sec);
            i += static_cast<DWORD>(len + 1);
            if (len == 0) continue;
            if (std::wcsncmp(sec, L"Camera.", 7) != 0) continue;
            if (CountDots(sec) != 1) continue;
            const unsigned idx = static_cast<unsigned>(std::wcstoul(sec + 7, nullptr, 10));
            camSections.push_back({ idx, std::wstring(sec) });
        }
        std::sort(camSections.begin(), camSections.end(),
                  [](const auto& a, const auto& b){ return a.idx < b.idx; });

        for (const auto& cs : camSections)
        {
            const wchar_t* C = cs.name.c_str();
            CameraFrame c;
            c.kind = ParseKind(ReadStr(C, L"Kind", KindName(c.kind), path), c.kind);
            if (c.kind == CameraKind::Entity)
            {
                c.sourceEntityId = static_cast<uint16_t>(ReadInt(C, L"SourceEntityID", 0, path));
                c.presetType  = Narrow(ReadStr(C, L"PresetType",  L"", path));
                c.presetAngle = Narrow(ReadStr(C, L"PresetAngle", L"", path));
                // Absent in plays authored before gimbal limits existed; the default must
                // match CameraFrame's own (off) or reloading an old play would change it.
                c.limitsEnabled = ReadInt(C, L"LimitsEnabled", c.limitsEnabled ? 1 : 0, path) != 0;
            }
            else
            {
                c.vantEastM  = ReadDouble(C, L"VantEastMeters",  0.0, path);
                c.vantNorthM = ReadDouble(C, L"VantNorthMeters", 0.0, path);
                c.vantUpM    = ReadDouble(C, L"VantUpMeters",    0.0, path);
            }
            c.targetEntityId = static_cast<uint16_t>(ReadInt(C, L"TargetEntityID", 0, path));
            c.transition = ParseTrans(ReadStr(C, L"Transition", TransName(c.transition), path),
                                      c.transition);
            c.beginSecond = ReadDouble(C, L"BeginSecond", 0.0, path);
            c.endSecond   = ReadDouble(C, L"EndSecond",   0.0, path);
            c.label = Narrow(ReadStr(C, L"Label", L"", path));

            // Zoom — absent in plays saved before the feature; the default-
            // constructed member supplies the fallback, so those load with zoom off.
            c.zoomEnabled = ReadInt(C, L"ZoomEnabled", c.zoomEnabled ? 1 : 0, path) != 0;
            c.zoomFovDeg  = ReadDouble(C, L"ZoomFovDeg", c.zoomFovDeg, path);
            c.dynamicZoom = ReadInt(C, L"DynamicZoom", c.dynamicZoom ? 1 : 0, path) != 0;
            c.zoomFillPct = ReadDouble(C, L"ZoomFillPercent",    c.zoomFillPct,  path);
            c.targetLengthM = ReadDouble(C, L"TargetLengthMeters", c.targetLengthM, path);
            c.targetWidthM  = ReadDouble(C, L"TargetWidthMeters",  c.targetWidthM,  path);
            c.targetHeightM = ReadDouble(C, L"TargetHeightMeters", c.targetHeightM, path);

            s.cameras.push_back(std::move(c));
        }
    }

    // ---- [Foliage] Preview-tab tree selection (absent → struct defaults) ----
    {
        FoliageConfig& fo = s.foliage;
        fo.oak        = ReadInt(L"Foliage", L"Oak",      fo.oak      ? 1 : 0, path) != 0;
        fo.bigTrees   = ReadInt(L"Foliage", L"BigTrees", fo.bigTrees ? 1 : 0, path) != 0;
        fo.palm       = ReadInt(L"Foliage", L"Palm",     fo.palm     ? 1 : 0, path) != 0;
        fo.palmKind   = ParsePalmKind(ReadStr(L"Foliage", L"PalmKind", L"", path), fo.palmKind);
        fo.renderMode = ParseRenderMode(ReadStr(L"Foliage", L"RenderMode", L"", path), fo.renderMode);
    }

    // A loaded scenario's origin is committed (not provisional). Not persisted as
    // a key — any file we successfully read carries a real, committed origin.
    s.originSet = true;

    sprintf_s(szError, sizeof(szError),
              "ScenarioIO::Load read scenario.ini, version=%lld, entities=%zu, cameras=%zu",
              version, s.entities.size(), s.cameras.size());
    LOG(szError);
    return Result::Ok;
}
