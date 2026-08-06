//=============================================================================
//  CameraPresetCatalog.cpp
//-----------------------------------------------------------------------------
//  Implements CameraPresetCatalog: parses config\Cameras.ini into an ordered
//  list of CameraPreset records. Enumerates every [CameraViews.<Type>.<Angle>]
//  section (mirroring EntityTypeCatalog's GetPrivateProfileSectionNamesW idiom),
//  splits the key on its LAST dot into Type/Angle (types may contain spaces or
//  dashes but never dots), and reads the numeric fields.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#include "pch.h"
#include "CameraPresetCatalog.h"

#include <windows.h>
#include <cstdlib>
#include <cwchar>
#include <utility>

namespace
{
    // UTF-16 -> UTF-8 (Cameras.ini keys/types are plain ASCII in practice).
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

    // Read one section's raw key=value lines (source order).
    std::vector<std::pair<std::wstring, std::wstring>>
    ReadAllKeys(const wchar_t* sec, const std::wstring& path)
    {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        std::vector<wchar_t> buf(4096);
        DWORD got = 0;
        for (;;)
        {
            got = ::GetPrivateProfileSectionW(sec, buf.data(),
                                              static_cast<DWORD>(buf.size()),
                                              path.c_str());
            if (got + 2 < buf.size()) break;
            buf.resize(buf.size() * 2);
            if (buf.size() > (1 << 18)) break;
        }
        for (DWORD i = 0; i < got; )
        {
            const wchar_t* line = buf.data() + i;
            const size_t len = std::wcslen(line);
            i += static_cast<DWORD>(len + 1);
            if (len == 0) continue;
            const wchar_t* eq = std::wcschr(line, L'=');
            if (!eq) continue;
            out.emplace_back(std::wstring(line, eq), std::wstring(eq + 1));
        }
        return out;
    }

    double ToD(const std::wstring& v, double def)
    {
        if (v.empty()) return def;
        wchar_t* end = nullptr;
        const double d = std::wcstod(v.c_str(), &end);
        return (end == v.c_str()) ? def : d;
    }
}

//
// CameraPreset::DisplayLabel — "Type / angle" for the dropdown.
//
std::string CameraPreset::DisplayLabel() const
{
    return type + " / " + angle;
}

//
// CameraPresetCatalog::LoadFromIni — clear and repopulate from Cameras.ini.
//   Skips non-"CameraViews." sections and any section whose remainder has no dot
//   (i.e. lacks an angle). Returns false when the file is absent.
//
bool CameraPresetCatalog::LoadFromIni(const std::wstring& path)
{
    m_presets.clear();

    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    // Full section list (double-null-terminated), grown if truncated.
    std::vector<wchar_t> namesBuf(32768);
    DWORD nameLen = 0;
    for (;;)
    {
        nameLen = ::GetPrivateProfileSectionNamesW(namesBuf.data(),
                                                   static_cast<DWORD>(namesBuf.size()),
                                                   path.c_str());
        if (nameLen + 2 < namesBuf.size()) break;
        namesBuf.resize(namesBuf.size() * 2);
        if (namesBuf.size() > (1 << 20)) break;
    }

    static const wchar_t kPrefix[] = L"CameraViews.";
    const size_t kPrefixLen = (sizeof(kPrefix) / sizeof(wchar_t)) - 1;

    for (DWORD i = 0; i < nameLen; )
    {
        const wchar_t* sec = namesBuf.data() + i;
        const size_t len = std::wcslen(sec);
        i += static_cast<DWORD>(len + 1);
        if (len <= kPrefixLen) continue;
        if (std::wcsncmp(sec, kPrefix, kPrefixLen) != 0) continue;

        // Remainder = "<Type>.<Angle>"; split on the LAST dot.
        const std::wstring rest(sec + kPrefixLen);
        const size_t dot = rest.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= rest.size())
            continue;

        CameraPreset p;
        p.type  = Narrow(rest.substr(0, dot));
        p.angle = Narrow(rest.substr(dot + 1));

        for (const auto& kv : ReadAllKeys(sec, path))
        {
            const std::wstring& k = kv.first;
            const std::wstring& v = kv.second;
            if      (k == L"X")     p.x     = ToD(v, p.x);
            else if (k == L"Y")     p.y     = ToD(v, p.y);
            else if (k == L"Z")     p.z     = ToD(v, p.z);
            else if (k == L"Pitch") p.pitch = ToD(v, p.pitch);
            else if (k == L"Yaw")   p.yaw   = ToD(v, p.yaw);
            else if (k == L"Roll")  p.roll  = ToD(v, p.roll);
            else if (k == L"FOV")   p.fov   = ToD(v, p.fov);
            else if (k == L"FocusCenter")
                p.focusCenter = static_cast<int>(ToD(v, 0.0));
            // Gimbal envelope. We only care whether one exists and is switched on; the
            // angles themselves are DISBrowser's business, so they are not mirrored here.
            else if (k == L"LimitsEnabled")
            {
                p.limitsEnabled = (ToD(v, 0.0) != 0.0);
                p.hasLimits = true;
            }
            else if (k == L"MinPitch" || k == L"MaxPitch" ||
                     k == L"MinYaw"   || k == L"MaxYaw"   ||
                     k == L"MinRoll"  || k == L"MaxRoll")
            {
                p.hasLimits = true;
            }
        }

        m_presets.push_back(std::move(p));
    }

    return !m_presets.empty();
}

//
// Find — see header. Linear scan; the catalog is ~60 entries and this is called only on
//   menu-build and dialog-init, so an index would be more bookkeeping than it saves.
//
const CameraPreset* CameraPresetCatalog::Find(const std::string& type,
                                              const std::string& angle) const
{
    for (const CameraPreset& p : m_presets)
        if (p.type == type && p.angle == angle) return &p;
    return nullptr;
}
