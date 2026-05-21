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

    const wchar_t* ForceIdName(uint8_t id)
    {
        switch (id) {
            case 1:  return L"Friendly";
            case 2:  return L"Opposing";
            case 3:  return L"Neutral";
            default: return L"Other";
        }
    }
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

    const wchar_t* CoordModeName(CoordMode m)
    {
        switch (m) {
            case CoordMode::LatLonAlt: return L"LatLonAlt";
            case CoordMode::Local:     return L"Local";
            case CoordMode::ECEF:      return L"ECEF";
        }
        return L"LatLonAlt";
    }
    CoordMode ParseCoordMode(const std::wstring& v, CoordMode fallback)
    {
        if (v == L"LatLonAlt") return CoordMode::LatLonAlt;
        if (v == L"Local")     return CoordMode::Local;
        if (v == L"ECEF")      return CoordMode::ECEF;
        return fallback;
    }

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
    MotionType ParseMotionType(const std::wstring& v, MotionType fallback)
    {
        if (v == L"Stationary") return MotionType::Stationary;
        if (v == L"StopHold")   return MotionType::StopHold;
        if (v == L"Stop")       return MotionType::StopHold; // spec §11.2 alias
        if (v == L"Line")       return MotionType::Line;
        if (v == L"Ellipse")    return MotionType::Ellipse;
        return fallback;
    }

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
    OutputMode ParseOutputMode(const std::wstring& v, OutputMode fallback)
    {
        if (v == L"UdpUnicast")    return OutputMode::UdpUnicast;
        if (v == L"UdpMulticast")  return OutputMode::UdpMulticast;
        if (v == L"Tcp")           return OutputMode::Tcp;
        if (v == L"FileRecording") return OutputMode::FileRecording;
        if (v == L"PreviewOnly")   return OutputMode::PreviewOnly;
        return fallback;
    }

    const wchar_t* DirectionName(EllipseDirection d)
    {
        return d == EllipseDirection::Clockwise ? L"Clockwise" : L"CounterClockwise";
    }
    EllipseDirection ParseDirection(const std::wstring& v, EllipseDirection fb)
    {
        if (v == L"Clockwise")        return EllipseDirection::Clockwise;
        if (v == L"CounterClockwise") return EllipseDirection::CounterClockwise;
        return fb;
    }

    const wchar_t* AltModeName(AltitudeMode m)
    {
        return m == AltitudeMode::Linear ? L"Linear" : L"Constant";
    }
    AltitudeMode ParseAltMode(const std::wstring& v, AltitudeMode fb)
    {
        if (v == L"Linear")   return AltitudeMode::Linear;
        if (v == L"Constant") return AltitudeMode::Constant;
        return fb;
    }

    // ---------- WPP helpers ----------

    bool WriteStr(const wchar_t* section, const wchar_t* key, const wchar_t* value,
                  const std::wstring& path)
    {
        return ::WritePrivateProfileStringW(section, key, value, path.c_str()) != 0;
    }
    bool WriteStr(const wchar_t* section, const wchar_t* key, const std::wstring& value,
                  const std::wstring& path)
    {
        return WriteStr(section, key, value.c_str(), path);
    }
    bool WriteInt(const wchar_t* section, const wchar_t* key, long long value,
                  const std::wstring& path)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"%lld", value);
        return WriteStr(section, key, buf, path);
    }
    bool WriteDouble(const wchar_t* section, const wchar_t* key, double value,
                     const std::wstring& path)
    {
        // %.17g is round-trip safe for IEEE 754 doubles.
        wchar_t buf[64];
        swprintf_s(buf, L"%.17g", value);
        return WriteStr(section, key, buf, path);
    }

    std::wstring ReadStr(const wchar_t* section, const wchar_t* key,
                         const wchar_t* defaultValue, const std::wstring& path)
    {
        wchar_t buf[2048];
        const DWORD n = ::GetPrivateProfileStringW(section, key, defaultValue,
                                                   buf, _countof(buf), path.c_str());
        return std::wstring(buf, n);
    }
    long long ReadInt(const wchar_t* section, const wchar_t* key,
                      long long defaultValue, const std::wstring& path)
    {
        const std::wstring s = ReadStr(section, key, L"", path);
        if (s.empty()) return defaultValue;
        wchar_t* end = nullptr;
        const long long v = std::wcstoll(s.c_str(), &end, 10);
        return (end == s.c_str()) ? defaultValue : v;
    }
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

    std::wstring EntitySection(uint16_t entityId)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"Entity.%u", static_cast<unsigned>(entityId));
        return buf;
    }

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
}

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

    WriteDouble(L"Origin", L"LatitudeDeg",    scenario.originLatDeg, path);
    WriteDouble(L"Origin", L"LongitudeDeg",   scenario.originLonDeg, path);
    WriteDouble(L"Origin", L"AltitudeMeters", scenario.originAltM, path);

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
            WriteDouble(M, L"EndHeadingDeg",     m.endHeadingDeg, path);
            WriteDouble(M, L"EndPitchDeg",       m.endPitchDeg, path);
            WriteDouble(M, L"EndRollDeg",        m.endRollDeg, path);

            WriteStr   (M, L"SpeedMode",   Widen(m.speedMode),   path);
            WriteStr   (M, L"HeadingMode", Widen(m.headingMode), path);

            // Ellipse
            WriteDouble(M, L"CenterLatitudeDeg",  m.centerLat, path);
            WriteDouble(M, L"CenterLongitudeDeg", m.centerLon, path);
            WriteDouble(M, L"CenterAltitudeMeters", m.centerAlt, path);
            WriteDouble(M, L"RadiusXMeters",   m.radiusXMeters, path);
            WriteDouble(M, L"RadiusYMeters",   m.radiusYMeters, path);
            WriteDouble(M, L"RotationDeg",     m.rotationDeg, path);
            WriteDouble(M, L"StartAngleDeg",   m.startAngleDeg, path);
            WriteStr   (M, L"Direction",       DirectionName(m.direction), path);
            WriteDouble(M, L"PeriodSeconds",   m.periodSeconds, path);
            WriteStr   (M, L"AltitudeMode",    AltModeName(m.altitudeMode), path);

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
        WriteStr   (L"Output", L"RecordingPath",     Widen(o.recordingPath), path);
        WriteStr   (L"Output", L"ReplayPath",        Widen(o.replayPath), path);
        WriteDouble(L"Output", L"PlaybackSpeed",     o.playbackSpeed, path);
        WriteInt   (L"Output", L"LoopEnabled",       o.loopEnabled ? 1 : 0, path);
    }

    // Force the cache to disk.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    sprintf_s(szError, sizeof(szError), "ScenarioIO::Save wrote scenario.ini");
    LOG(szError);
    return Result::Ok;
}

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

    s.originLatDeg = ReadDouble(L"Origin", L"LatitudeDeg",    s.originLatDeg, path);
    s.originLonDeg = ReadDouble(L"Origin", L"LongitudeDeg",   s.originLonDeg, path);
    s.originAltM   = ReadDouble(L"Origin", L"AltitudeMeters", s.originAltM,   path);

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
    if (s.entities.empty())
        s.entities.push_back(Entity{}); // keep entity() valid

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
            m.startHeadingDeg = ReadDouble(M, L"StartHeadingDeg",    0.0, path);
            m.startPitchDeg   = ReadDouble(M, L"StartPitchDeg",      0.0, path);
            m.startRollDeg    = ReadDouble(M, L"StartRollDeg",       0.0, path);

            m.endLat         = ReadDouble(M, L"EndLatitudeDeg",      0.0, path);
            m.endLon         = ReadDouble(M, L"EndLongitudeDeg",     0.0, path);
            m.endAlt         = ReadDouble(M, L"EndAltitudeMeters",   0.0, path);
            m.endEcefX       = ReadDouble(M, L"EndEcefX",            0.0, path);
            m.endEcefY       = ReadDouble(M, L"EndEcefY",            0.0, path);
            m.endEcefZ       = ReadDouble(M, L"EndEcefZ",            0.0, path);
            m.endHeadingDeg  = ReadDouble(M, L"EndHeadingDeg",       0.0, path);
            m.endPitchDeg    = ReadDouble(M, L"EndPitchDeg",         0.0, path);
            m.endRollDeg     = ReadDouble(M, L"EndRollDeg",          0.0, path);

            m.speedMode      = Narrow(ReadStr(M, L"SpeedMode",   L"CalculateFromTime", path));
            m.headingMode    = Narrow(ReadStr(M, L"HeadingMode", L"CalculateFromPath", path));

            m.centerLat      = ReadDouble(M, L"CenterLatitudeDeg",    0.0, path);
            m.centerLon      = ReadDouble(M, L"CenterLongitudeDeg",   0.0, path);
            m.centerAlt      = ReadDouble(M, L"CenterAltitudeMeters", 0.0, path);
            m.radiusXMeters  = ReadDouble(M, L"RadiusXMeters", 0.0, path);
            m.radiusYMeters  = ReadDouble(M, L"RadiusYMeters", 0.0, path);
            m.rotationDeg    = ReadDouble(M, L"RotationDeg",   0.0, path);
            m.startAngleDeg  = ReadDouble(M, L"StartAngleDeg", 0.0, path);
            m.direction      = ParseDirection(ReadStr(M, L"Direction", L"Clockwise", path),
                                              EllipseDirection::Clockwise);
            m.periodSeconds  = ReadDouble(M, L"PeriodSeconds", 60.0, path);
            m.altitudeMode   = ParseAltMode(ReadStr(M, L"AltitudeMode", L"Constant", path),
                                            AltitudeMode::Constant);

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
        o.unicastPort      = static_cast<uint16_t>(ReadInt(L"Output", L"UnicastPort", 3000, path));
        o.multicastGroup   = Narrow(ReadStr(L"Output", L"MulticastGroup", L"239.1.2.3", path));
        o.multicastPort    = static_cast<uint16_t>(ReadInt(L"Output", L"MulticastPort", 3000, path));
        o.multicastInterface = Narrow(ReadStr(L"Output", L"MulticastInterface", L"0.0.0.0", path));
        o.multicastTtl     = static_cast<int>(ReadInt(L"Output", L"TTL", 1, path));
        o.multicastLoopback = ReadInt(L"Output", L"LoopbackEnabled", 1, path) != 0;
        o.recordingPath    = Narrow(ReadStr(L"Output", L"RecordingPath", L"", path));
        o.replayPath       = Narrow(ReadStr(L"Output", L"ReplayPath",    L"", path));
        o.playbackSpeed    = ReadDouble(L"Output", L"PlaybackSpeed", 1.0, path);
        o.loopEnabled      = ReadInt(L"Output", L"LoopEnabled", 0, path) != 0;
    }

    sprintf_s(szError, sizeof(szError),
              "ScenarioIO::Load read scenario.ini, version=%lld, entities=%zu",
              version, s.entities.size());
    LOG(szError);
    return Result::Ok;
}
