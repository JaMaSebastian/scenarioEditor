#include "SettingsIO.h"
#include "../log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

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

    bool WriteStr(const wchar_t* sec, const wchar_t* key, const wchar_t* val,
                  const std::wstring& path)
    {
        return ::WritePrivateProfileStringW(sec, key, val, path.c_str()) != 0;
    }
    bool WriteInt(const wchar_t* sec, const wchar_t* key, long long val,
                  const std::wstring& path)
    {
        wchar_t buf[32]; swprintf_s(buf, L"%lld", val);
        return WriteStr(sec, key, buf, path);
    }

    std::wstring ReadStr(const wchar_t* sec, const wchar_t* key,
                         const wchar_t* def, const std::wstring& path)
    {
        wchar_t buf[1024];
        const DWORD n = ::GetPrivateProfileStringW(sec, key, def, buf,
                                                   _countof(buf), path.c_str());
        return std::wstring(buf, n);
    }
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

    sprintf_s(szError, sizeof(szError),
              "SettingsIO::Load read %s (window %dx%d at %d,%d max=%d)",
              Narrow(path).c_str(),
              out.windowWidth, out.windowHeight,
              out.windowX, out.windowY,
              out.windowMaximized ? 1 : 0);
    LOG(szError);
    return true;
}

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
