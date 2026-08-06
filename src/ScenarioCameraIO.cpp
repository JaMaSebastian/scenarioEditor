//=============================================================================
//  ScenarioCameraIO.cpp
//-----------------------------------------------------------------------------
//  Implements ScenarioCameraIO: reads/writes a scenario's camera schedule as a
//  standalone <scenario>-camera.ini. Mirrors ScenarioIO's WPP idiom — delete
//  then write, [Format] Version gate, and GetPrivateProfileSectionNames-based
//  discovery of numbered [Camera.N] sections on load.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#include "pch.h"
#include "ScenarioCameraIO.h"
#include "Scenario.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // ---------- UTF-8 <-> UTF-16 ----------
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                            static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                              out.data(), n);
        return out;
    }
    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                            static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                              out.data(), n, nullptr, nullptr);
        return out;
    }

    // ---------- WPP helpers (mirror ScenarioIO) ----------
    bool WriteStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v,
                  const std::wstring& path)
    {
        return ::WritePrivateProfileStringW(sec, key, v.c_str(), path.c_str()) != 0;
    }
    bool WriteInt(const wchar_t* sec, const wchar_t* key, long long v,
                  const std::wstring& path)
    {
        wchar_t buf[32]; swprintf_s(buf, L"%lld", v);
        return WriteStr(sec, key, buf, path);
    }
    bool WriteDouble(const wchar_t* sec, const wchar_t* key, double v,
                     const std::wstring& path)
    {
        wchar_t buf[64]; swprintf_s(buf, L"%.17g", v);
        return WriteStr(sec, key, buf, path);
    }
    std::wstring ReadStr(const wchar_t* sec, const wchar_t* key,
                         const wchar_t* def, const std::wstring& path)
    {
        wchar_t buf[2048];
        const DWORD n = ::GetPrivateProfileStringW(sec, key, def, buf,
                                                   _countof(buf), path.c_str());
        return std::wstring(buf, n);
    }
    long long ReadInt(const wchar_t* sec, const wchar_t* key, long long def,
                      const std::wstring& path)
    {
        const std::wstring s = ReadStr(sec, key, L"", path);
        if (s.empty()) return def;
        wchar_t* end = nullptr;
        const long long v = std::wcstoll(s.c_str(), &end, 10);
        return (end == s.c_str()) ? def : v;
    }
    double ReadDouble(const wchar_t* sec, const wchar_t* key, double def,
                      const std::wstring& path)
    {
        const std::wstring s = ReadStr(sec, key, L"", path);
        if (s.empty()) return def;
        wchar_t* end = nullptr;
        const double v = std::wcstod(s.c_str(), &end);
        return (end == s.c_str()) ? def : v;
    }

    bool EnsureParentDir(const std::wstring& filePath)
    {
        std::wstring dir = filePath;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return true;
        dir.resize(slash);
        if (dir.empty()) return true;
        const int rc = ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS
            || rc == ERROR_FILE_EXISTS;
    }

    std::wstring CameraSection(size_t idx)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"Camera.%u", static_cast<unsigned>(idx + 1));
        return buf;
    }
    int CountDots(const wchar_t* s)
    {
        int n = 0;
        for (const wchar_t* p = s; *p; ++p) if (*p == L'.') ++n;
        return n;
    }

    // ---------- enum <-> name ----------
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
}

//
// ScenarioCameraIO::Load — populate scenario.cameras from `path` (cleared
//   first). A missing file yields Ok with an empty schedule. Frames are read in
//   [Camera.N] index order; the caller normalizes the tiling afterward.
//
ScenarioCameraIO::Result
ScenarioCameraIO::Load(Scenario& scenario, const std::wstring& path)
{
    scenario.cameras.clear();

    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return Result::Ok;   // no camera file — simply no cameras

    const long long ver = ReadInt(L"Format", L"Version", -1, path);
    if (ver < 0)                       return Result::Malformed;
    if (ver > kCurrentFormatVersion)   return Result::VersionTooNew;

    // Discover [Camera.N] sections (prefix "Camera." with exactly one dot).
    wchar_t namesBuf[16384];
    const DWORD nameLen = ::GetPrivateProfileSectionNamesW(namesBuf,
                                                           _countof(namesBuf),
                                                           path.c_str());
    // Collect (index, section) so we honor Camera.N numeric order.
    std::vector<std::pair<unsigned, std::wstring>> found;
    for (DWORD i = 0; i < nameLen; )
    {
        const wchar_t* sec = namesBuf + i;
        const size_t len = std::wcslen(sec);
        i += static_cast<DWORD>(len + 1);
        if (len == 0) continue;
        if (std::wcsncmp(sec, L"Camera.", 7) != 0) continue;
        if (CountDots(sec) != 1) continue;
        const unsigned idx = static_cast<unsigned>(std::wcstoul(sec + 7, nullptr, 10));
        found.emplace_back(idx, sec);
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& fs : found)
    {
        const wchar_t* sec = fs.second.c_str();
        CameraFrame c;
        c.kind = ParseKind(ReadStr(sec, L"Kind", KindName(c.kind), path), c.kind);
        if (c.kind == CameraKind::Entity)
        {
            c.sourceEntityId = static_cast<uint16_t>(ReadInt(sec, L"SourceEntityID", 0, path));
            c.presetType  = Narrow(ReadStr(sec, L"PresetType",  L"", path));
            c.presetAngle = Narrow(ReadStr(sec, L"PresetAngle", L"", path));
        }
        else
        {
            c.vantEastM  = ReadDouble(sec, L"VantEastMeters",  0.0, path);
            c.vantNorthM = ReadDouble(sec, L"VantNorthMeters", 0.0, path);
            c.vantUpM    = ReadDouble(sec, L"VantUpMeters",    0.0, path);
        }
        c.targetEntityId = static_cast<uint16_t>(ReadInt(sec, L"TargetEntityID", 0, path));
        c.transition = ParseTrans(ReadStr(sec, L"Transition", TransName(c.transition), path),
                                  c.transition);
        c.beginSecond = ReadDouble(sec, L"BeginSecond", 0.0, path);
        c.endSecond   = ReadDouble(sec, L"EndSecond",   0.0, path);
        c.label = Narrow(ReadStr(sec, L"Label", L"", path));

        // Zoom — kept in step with ScenarioIO so this importer stays an honest
        // mirror. Side-files all predate the feature, so in practice every one of
        // these falls through to the struct default (zoom off).
        c.zoomEnabled = ReadInt(sec, L"ZoomEnabled", c.zoomEnabled ? 1 : 0, path) != 0;
        c.zoomFovDeg  = ReadDouble(sec, L"ZoomFovDeg", c.zoomFovDeg, path);
        c.dynamicZoom = ReadInt(sec, L"DynamicZoom", c.dynamicZoom ? 1 : 0, path) != 0;
        c.zoomFillPct = ReadDouble(sec, L"ZoomFillPercent",    c.zoomFillPct,  path);
        c.targetLengthM = ReadDouble(sec, L"TargetLengthMeters", c.targetLengthM, path);
        c.targetWidthM  = ReadDouble(sec, L"TargetWidthMeters",  c.targetWidthM,  path);
        c.targetHeightM = ReadDouble(sec, L"TargetHeightMeters", c.targetHeightM, path);

        scenario.cameras.push_back(std::move(c));
    }

    return Result::Ok;
}
