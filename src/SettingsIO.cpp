//=============================================================================
//  SettingsIO.cpp
//-----------------------------------------------------------------------------
//  Implements loading and atomic saving of settings.ini using the Win32
//  private-profile (INI) API. Values are stored as UTF-8 narrowed <-> wide,
//  and Save writes a sibling .tmp then ReplaceFile/MoveFileEx's it into place
//  so a mid-write crash cannot corrupt the live file.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "SettingsIO.h"
#include "../log.h"
#include <algorithm>   // std::transform — case-insensitive [Terrain] Mode parse

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace
{
    //
    // Widen — UTF-8 std::string to std::wstring via MultiByteToWideChar.
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
    // Narrow — std::wstring to UTF-8 std::string via WideCharToMultiByte.
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

    //
    // WriteStr — write one string key/value under [sec] in the INI at path.
    //
    bool WriteStr(const wchar_t* sec, const wchar_t* key, const wchar_t* val,
                  const std::wstring& path)
    {
        return ::WritePrivateProfileStringW(sec, key, val, path.c_str()) != 0;
    }
    //
    // WriteInt — write an integer key by formatting it as decimal text.
    //
    bool WriteInt(const wchar_t* sec, const wchar_t* key, long long val,
                  const std::wstring& path)
    {
        wchar_t buf[32]; swprintf_s(buf, L"%lld", val);
        return WriteStr(sec, key, buf, path);
    }

    //
    // ReadStr — read a string key under [sec], returning def when absent.
    //
    std::wstring ReadStr(const wchar_t* sec, const wchar_t* key,
                         const wchar_t* def, const std::wstring& path)
    {
        wchar_t buf[1024];
        const DWORD n = ::GetPrivateProfileStringW(sec, key, def, buf,
                                                   _countof(buf), path.c_str());
        return std::wstring(buf, n);
    }
    //
    // ReadInt — read a key and parse it as a base-10 integer; returns def when
    // the key is absent or not numeric.
    //
    long long ReadInt(const wchar_t* sec, const wchar_t* key,
                      long long def, const std::wstring& path)
    {
        const std::wstring s = ReadStr(sec, key, L"", path);
        if (s.empty()) return def;
        wchar_t* end = nullptr;
        const long long v = std::wcstoll(s.c_str(), &end, 10);
        return (end == s.c_str()) ? def : v;
    }
}

//
// SettingsIO::Load — read settings.ini into `out`. Missing file, unknown
// (future) format version, or missing keys leave struct defaults in place;
// returns false when the file is absent or too new, true on a successful read.
//
bool SettingsIO::Load(Settings& out, const std::wstring& path)
{
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        sprintf_s(szError, sizeof(szError),
                  "SettingsIO::Load: %s not found, using defaults",
                  Narrow(path).c_str());
        LOG(szError);
        return false;
    }

    const long long version = ReadInt(L"Format", L"Version", 0, path);
    if (version > kCurrentFormatVersion)
    {
        sprintf_s(szError, sizeof(szError),
                  "SettingsIO::Load: version %lld > known %d, ignoring",
                  version, kCurrentFormatVersion);
        LOG(szError);
        return false;
    }

    out.windowX        = static_cast<int>(ReadInt(L"Window", L"PositionX", out.windowX, path));
    out.windowY        = static_cast<int>(ReadInt(L"Window", L"PositionY", out.windowY, path));
    out.windowWidth    = static_cast<int>(ReadInt(L"Window", L"Width",     out.windowWidth, path));
    out.windowHeight   = static_cast<int>(ReadInt(L"Window", L"Height",    out.windowHeight, path));
    out.windowMaximized = ReadInt(L"Window", L"Maximized", 0, path) != 0;

    out.lastScenarioPath = Narrow(ReadStr(L"Paths", L"LastScenario", L"", path));
    out.disBrowserProjectDir = Narrow(ReadStr(L"Paths", L"DisBrowserProjectDir", L"", path));

    out.unrealTargetLevel = Narrow(ReadStr(L"Unreal", L"TargetLevel", L"Generic",   path));
    out.unrealBasemap     = Narrow(ReadStr(L"Unreal", L"Basemap",     L"Satellite", path));
    out.unrealDynamicTiles = ReadInt(L"Unreal", L"DynamicTiles", 0, path) != 0;

    // [Terrain] — server mode + remote endpoint. Unknown/misspelt names fall back
    // to Local rather than failing the load, so a hand-edited file can't brick
    // startup; the name is matched case-insensitively for the same reason.
    {
        std::wstring mode = ReadStr(L"Terrain", L"Mode", L"Local", path);
        std::transform(mode.begin(), mode.end(), mode.begin(), ::towlower);
        if      (mode == L"legacy")  out.terrainMode = TerrainServerMode::Legacy;
        else if (mode == L"remote")  out.terrainMode = TerrainServerMode::Remote;
        else if (mode == L"service") out.terrainMode = TerrainServerMode::Service;
        else                         out.terrainMode = TerrainServerMode::Local;

        out.terrainRemoteHost = Narrow(ReadStr(L"Terrain", L"RemoteHost", L"127.0.0.1", path));

        // Service defaults to 8089 so the container and a native TerrainServer.exe
        // on 8088 can both be up; the other modes keep 8088.
        const int defPort = (out.terrainMode == TerrainServerMode::Service) ? 8089 : 8088;
        out.terrainRemotePort = static_cast<uint16_t>(
            ReadInt(L"Terrain", L"RemotePort", defPort, path) & 0xFFFF);
    }

    // [Deploy] ColumnWidths — comma-separated pixel widths.
    out.deployColumnWidths.clear();
    {
        const std::wstring csv = ReadStr(L"Deploy", L"ColumnWidths", L"", path);
        size_t start = 0;
        while (start <= csv.size() && !csv.empty())
        {
            const size_t comma = csv.find(L',', start);
            const std::wstring tok =
                csv.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            if (!tok.empty())
                out.deployColumnWidths.push_back(_wtoi(tok.c_str()));
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
    }

    // [Places] — named map locations: Count + PlaceN=<lat>|<lon>|<label>.
    out.mapPlaces.clear();
    {
        const int count = static_cast<int>(ReadInt(L"Places", L"Count", 0, path));
        for (int i = 0; i < count; ++i)
        {
            wchar_t key[24]; swprintf_s(key, L"Place%d", i);
            const std::wstring v = ReadStr(L"Places", key, L"", path);
            if (v.empty()) continue;
            const size_t p1 = v.find(L'|');
            const size_t p2 = (p1 == std::wstring::npos) ? p1 : v.find(L'|', p1 + 1);
            if (p1 == std::wstring::npos || p2 == std::wstring::npos) continue;
            MapPlace mp;
            mp.lat   = _wtof(v.substr(0, p1).c_str());
            mp.lon   = _wtof(v.substr(p1 + 1, p2 - p1 - 1).c_str());
            mp.label = Narrow(v.substr(p2 + 1));
            // Altitude in a parallel key so the pipe format stays backward
            // compatible (old files have no Alt key -> 0).
            wchar_t akey[28]; swprintf_s(akey, L"Place%dAlt", i);
            mp.alt   = _wtof(ReadStr(L"Places", akey, L"0", path).c_str());
            if (!mp.label.empty()) out.mapPlaces.push_back(std::move(mp));
        }
    }

    sprintf_s(szError, sizeof(szError),
              "SettingsIO::Load read %s (window %dx%d at %d,%d max=%d)",
              Narrow(path).c_str(),
              out.windowWidth, out.windowHeight,
              out.windowX, out.windowY,
              out.windowMaximized ? 1 : 0);
    LOG(szError);
    return true;
}

//
// SettingsIO::Save — atomically persist `s` to settings.ini: write all sections
// to <path>.tmp, flush, then ReplaceFile/MoveFileEx over the target. Returns
// false (and cleans up the .tmp) on any I/O failure.
//
bool SettingsIO::Save(const Settings& s, const std::wstring& path)
{
    // Atomic write: emit a sibling .tmp file then ReplaceFile() over the
    // target. ReplaceFile is the canonical Win32 atomic-replace primitive.
    const std::wstring tmp = path + L".tmp";
    ::DeleteFileW(tmp.c_str());

    bool ok = true;
    ok &= WriteInt(L"Format", L"Version",       kCurrentFormatVersion, tmp);
    ok &= WriteInt(L"Window", L"PositionX",     s.windowX,            tmp);
    ok &= WriteInt(L"Window", L"PositionY",     s.windowY,            tmp);
    ok &= WriteInt(L"Window", L"Width",         s.windowWidth,        tmp);
    ok &= WriteInt(L"Window", L"Height",        s.windowHeight,       tmp);
    ok &= WriteInt(L"Window", L"Maximized",     s.windowMaximized ? 1 : 0, tmp);
    ok &= WriteStr(L"Paths",  L"LastScenario",  Widen(s.lastScenarioPath).c_str(), tmp);
    ok &= WriteStr(L"Paths",  L"DisBrowserProjectDir", Widen(s.disBrowserProjectDir).c_str(), tmp);
    ok &= WriteStr(L"Unreal", L"TargetLevel",   Widen(s.unrealTargetLevel).c_str(), tmp);
    ok &= WriteStr(L"Unreal", L"Basemap",       Widen(s.unrealBasemap).c_str(),     tmp);
    ok &= WriteInt(L"Unreal", L"DynamicTiles",  s.unrealDynamicTiles ? 1 : 0,       tmp);

    // Written as a name, not a number, so the file stays hand-editable.
    ok &= WriteStr(L"Terrain", L"Mode",
                   s.terrainMode == TerrainServerMode::Legacy  ? L"Legacy"  :
                   s.terrainMode == TerrainServerMode::Remote  ? L"Remote"  :
                   s.terrainMode == TerrainServerMode::Service ? L"Service" : L"Local", tmp);
    ok &= WriteStr(L"Terrain", L"RemoteHost", Widen(s.terrainRemoteHost).c_str(), tmp);
    ok &= WriteInt(L"Terrain", L"RemotePort", s.terrainRemotePort, tmp);

    if (!s.deployColumnWidths.empty())
    {
        std::wstring csv;
        for (size_t i = 0; i < s.deployColumnWidths.size(); ++i)
        {
            if (i) csv += L',';
            wchar_t b[16]; swprintf_s(b, L"%d", s.deployColumnWidths[i]);
            csv += b;
        }
        ok &= WriteStr(L"Deploy", L"ColumnWidths", csv.c_str(), tmp);
    }

    // [Places] — named map locations.
    ok &= WriteInt(L"Places", L"Count", static_cast<long long>(s.mapPlaces.size()), tmp);
    for (size_t i = 0; i < s.mapPlaces.size(); ++i)
    {
        const MapPlace& mp = s.mapPlaces[i];
        wchar_t key[24]; swprintf_s(key, L"Place%zu", i);
        std::wstring val;
        wchar_t num[64];
        swprintf_s(num, L"%.8f|%.8f|", mp.lat, mp.lon);
        val = num;
        val += Widen(mp.label);
        ok &= WriteStr(L"Places", key, val.c_str(), tmp);
        wchar_t akey[28]; swprintf_s(akey, L"Place%zuAlt", i);
        wchar_t anum[48]; swprintf_s(anum, L"%.3f", mp.alt);
        ok &= WriteStr(L"Places", akey, anum, tmp);
    }

    // Flush WPP's internal cache so the tmp file is fully on disk before
    // the rename.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, tmp.c_str());

    if (!ok)
    {
        LOG("SettingsIO::Save: failed to write .tmp");
        ::DeleteFileW(tmp.c_str());
        return false;
    }

    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        // No prior file → MoveFile is sufficient.
        if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            sprintf_s(szError, sizeof(szError),
                      "SettingsIO::Save: MoveFileExW failed: %lu", ::GetLastError());
            LOG(szError);
            return false;
        }
    }
    else
    {
        if (!::ReplaceFileW(path.c_str(), tmp.c_str(), nullptr,
                            REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
        {
            sprintf_s(szError, sizeof(szError),
                      "SettingsIO::Save: ReplaceFileW failed: %lu", ::GetLastError());
            LOG(szError);
            ::DeleteFileW(tmp.c_str());
            return false;
        }
    }

    LOG("SettingsIO::Save wrote settings.ini");
    return true;
}
