//=============================================================================
//  StartupIniWriter.cpp
//-----------------------------------------------------------------------------
//  Implements Write(), which produces DISBrowser's Config\Startup.ini: the
//  [Startup] level token plus, for the Generic level, [Generic] origin, basemap
//  (mapping the self-hosted "Cesium 3D" option to Map=Custom), dynamic-tile
//  flag, and optional painted terrain bounds. Self-hosted Cesium URLs are
//  appended as raw quoted lines because WritePrivateProfileStringW mangles them.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-13
//=============================================================================
#include "pch.h"
#include "StartupIniWriter.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

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

    // Create every missing directory along an absolute path.
    void EnsureDirTree(const std::wstring& dir)
    {
        for (size_t i = 0; i < dir.size(); ++i)
        {
            if (dir[i] == L'\\' || dir[i] == L'/')
            {
                if (i >= 2) ::CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
            }
        }
        ::CreateDirectoryW(dir.c_str(), nullptr);
    }
}

//
// StartupIniWriter::Write — validate the project dir, ensure Config\ exists,
// rewrite Startup.ini from scratch, and (for Generic) emit origin/basemap/
// terrain-bounds and any self-hosted Cesium URLs. On success outError receives
// the written path; on failure it receives an error message and returns false.
//
bool StartupIniWriter::Write(const std::wstring& disBrowserProjectDir,
                             const std::string&  levelToken,
                             const std::string&  basemap,
                             bool                dynamicTiles,
                             double originLatDeg, double originLonDeg, double originAltMeters,
                             bool   terrainBoundsValid,
                             double terrainLatMinDeg, double terrainLatMaxDeg,
                             double terrainLonMinDeg, double terrainLonMaxDeg,
                             const std::wstring& cameraScheduleAbsPath,
                             const FoliageHandoff& foliage,
                             const std::string&  terrainHost,
                             unsigned short      terrainPort,
                             std::wstring&        outError)
{
    if (disBrowserProjectDir.empty())
    {
        outError = L"DISBrowser project directory is not set.";
        return false;
    }

    std::wstring root = disBrowserProjectDir;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();

    const std::wstring configDir = root + L"\\Config";
    if (::GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        outError = L"DISBrowser project directory does not exist: " + root;
        return false;
    }
    EnsureDirTree(configDir);

    const std::wstring path = configDir + L"\\Startup.ini";

    // Start from a clean file so no stale keys survive a rewrite.
    ::DeleteFileW(path.c_str());

    bool ok = true;
    bool appendCesiumUrls = false;   // self-hosted Cesium: append quoted URL lines after the WPP flush
    ok &= ::WritePrivateProfileStringW(L"Startup", L"Level", Widen(levelToken).c_str(), path.c_str()) != 0;

    if (_stricmp(levelToken.c_str(), "Generic") == 0)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%.8f", originLatDeg);
        ok &= ::WritePrivateProfileStringW(L"Generic", L"OriginLatitude", buf, path.c_str()) != 0;
        swprintf_s(buf, L"%.8f", originLonDeg);
        ok &= ::WritePrivateProfileStringW(L"Generic", L"OriginLongitude", buf, path.c_str()) != 0;
        swprintf_s(buf, L"%.3f", originAltMeters);
        ok &= ::WritePrivateProfileStringW(L"Generic", L"OriginAltitudeMeters", buf, path.c_str()) != 0;

        // Map the friendly basemap label onto DISBrowser's [Cesium] Layer token. The self-hosted
        // "Cesium 3D" option becomes Layer=Custom; its terrain + imagery URLs must be emitted HERE
        // (DISBrowser's Generic.ini [Cesium] fallback does not reach runtime). WritePrivateProfileStringW
        // strips the quotes UE needs (and UE then truncates the URL at "//"), so the URL keys are
        // appended as raw quoted lines after the WPP flush below — see `appendCesiumUrls`.
        const bool selfHostedTerrain = (_stricmp(basemap.c_str(), "Cesium 3D (self-hosted)") == 0);
        appendCesiumUrls = selfHostedTerrain;
        const std::wstring mapTokenStr = selfHostedTerrain ? std::wstring(L"Custom") : Widen(basemap);
        ok &= ::WritePrivateProfileStringW(L"Generic", L"Map", mapTokenStr.c_str(), path.c_str()) != 0;
        ok &= ::WritePrivateProfileStringW(L"Generic", L"DynamicTiles", dynamicTiles ? L"true" : L"false", path.c_str()) != 0;

        // Painted 3D-terrain boundary (Preview tab). Plain numbers → WritePrivateProfileStringW is safe
        // here (no quoting issue, unlike the URL keys which live in Generic.ini).
        if (terrainBoundsValid)
        {
            swprintf_s(buf, L"%.8f", terrainLatMinDeg);
            ok &= ::WritePrivateProfileStringW(L"Generic", L"TerrainBoundsLatMin", buf, path.c_str()) != 0;
            swprintf_s(buf, L"%.8f", terrainLatMaxDeg);
            ok &= ::WritePrivateProfileStringW(L"Generic", L"TerrainBoundsLatMax", buf, path.c_str()) != 0;
            swprintf_s(buf, L"%.8f", terrainLonMinDeg);
            ok &= ::WritePrivateProfileStringW(L"Generic", L"TerrainBoundsLonMin", buf, path.c_str()) != 0;
            swprintf_s(buf, L"%.8f", terrainLonMaxDeg);
            ok &= ::WritePrivateProfileStringW(L"Generic", L"TerrainBoundsLonMax", buf, path.c_str()) != 0;
        }
    }

    // Flush WPP's cache to disk.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    // Append the self-hosted Cesium URLs as raw QUOTED lines under [Generic] (the last section).
    // WritePrivateProfileStringW mangles the quotes/`//`, so we write the exact bytes UE expects: it
    // strips the outer quotes on read and preserves the `//` + `{}` inside. DISBrowser's
    // ApplyStartupOverride reads CesiumTilesetUrl / CesiumRasterOverlayUrlTemplate from here.
    if (appendCesiumUrls)
    {
        // The terrain host is NOT always localhost: with [Terrain] Mode=Remote the
        // tiles are served by another machine, and DISBrowser has to be pointed at
        // it. terrainHost/terrainPort carry that through from settings.ini.
        //
        // {y}, NOT {reverseY}: ArcGIS numbers its tile rows from the NORTH, which is what
        // both Cesium and DISBrowser's God-view basemap mean by {y}. {reverseY} is the
        // south-up (TMS) index, and on this service it lands the imagery in the wrong
        // hemisphere. MapTileService's esriOrder and CesiumView's own template have always
        // used {y} — this line was the odd one out.
        char urlLines[1024];
        sprintf_s(urlLines, sizeof(urlLines),
            "CesiumTilesetUrl=\"http://%s:%u/layer.json\"\r\n"
            "CesiumRasterOverlayUrlTemplate=\"https://server.arcgisonline.com/ArcGIS/rest/services/"
            "World_Imagery/MapServer/tile/{z}/{y}/{x}\"\r\n",
            terrainHost.empty() ? "localhost" : terrainHost.c_str(),
            static_cast<unsigned>(terrainPort ? terrainPort : 8088));

        const char* const kUrlLines = urlLines;
        HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            ::WriteFile(h, kUrlLines, static_cast<DWORD>(strlen(kUrlLines)), &wrote, nullptr);
            ::CloseHandle(h);
        }
    }

    // Scenario camera track — appended as a raw TRAILING section (after any Cesium
    // lines) so WPP never re-parses/mangles the quoted URLs above. Written for EVERY
    // level: DISBrowser's ApplyStartupOverride reads [ScenarioCameras] ScheduleFile
    // regardless of the selected level and points its director at this file. Forward
    // slashes match the DISBrowser.ini convention; an empty value writes a bare
    // ScheduleFile= that actively clears any stale schedule on the runtime.
    {
        std::wstring sched = cameraScheduleAbsPath;
        for (wchar_t& c : sched) if (c == L'\\') c = L'/';
        const std::wstring block = L"[ScenarioCameras]\r\nScheduleFile=" + sched + L"\r\n";
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, block.c_str(),
                                            static_cast<int>(block.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, block.c_str(), static_cast<int>(block.size()),
                              &utf8[0], n, nullptr, nullptr);
        HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
            ::CloseHandle(h);
        }
    }

    // Foliage selection — trailing raw [Foliage] section (ASCII keys), emitted only
    // when a tree type is chosen. DISBrowser scatters these across the terrain box.
    if (foliage.oak || foliage.bigTrees || foliage.palm)
    {
        std::string block = "[Foliage]\r\n";
        block += std::string("Oak=")      + (foliage.oak      ? "1" : "0") + "\r\n";
        block += std::string("BigTrees=") + (foliage.bigTrees ? "1" : "0") + "\r\n";
        block += std::string("Palm=")     + (foliage.palm     ? "1" : "0") + "\r\n";
        block += "PalmKind="   + foliage.palmKind   + "\r\n";
        block += "RenderMode=" + foliage.renderMode + "\r\n";
        HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            ::WriteFile(h, block.data(), static_cast<DWORD>(block.size()), &wrote, nullptr);
            ::CloseHandle(h);
        }
    }

    if (!ok)
    {
        outError = L"Failed to write " + path + L" (error " + std::to_wstring(::GetLastError()) + L").";
        return false;
    }

    outError = path;   // on success, hand back the path for the confirmation message
    return true;
}
