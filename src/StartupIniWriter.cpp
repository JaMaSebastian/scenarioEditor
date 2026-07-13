#include "pch.h"
#include "StartupIniWriter.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace
{
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

bool StartupIniWriter::Write(const std::wstring& disBrowserProjectDir,
                             const std::string&  levelToken,
                             const std::string&  basemap,
                             bool                dynamicTiles,
                             double originLatDeg, double originLonDeg, double originAltMeters,
                             bool   terrainBoundsValid,
                             double terrainLatMinDeg, double terrainLatMaxDeg,
                             double terrainLonMinDeg, double terrainLonMaxDeg,
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
        static const char* const kUrlLines =
            "CesiumTilesetUrl=\"http://localhost:8088/layer.json\"\r\n"
            "CesiumRasterOverlayUrlTemplate=\"https://server.arcgisonline.com/ArcGIS/rest/services/"
            "World_Imagery/MapServer/tile/{z}/{reverseY}/{x}\"\r\n";
        HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            ::WriteFile(h, kUrlLines, static_cast<DWORD>(strlen(kUrlLines)), &wrote, nullptr);
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
