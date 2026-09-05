//=============================================================================
//  CameraPresetIO.cpp
//-----------------------------------------------------------------------------
//  Implements CameraPresetIO. Uses WritePrivateProfileStringW rather than a
//  hand-rolled rewriter: it is surgical by construction (one key, inside one
//  named section, everything else byte-for-byte), which is exactly the guarantee
//  we need for a file DISBrowser also owns and rewrites wholesale.
//
//  Encoding: Cameras.ini is UTF-8 with a BOM and CRLF line ends. Every key and
//  value written here is pure ASCII, so the ANSI bytes WritePrivateProfileStringW
//  emits are identical to what UE's FString::Printf writes; the BOM sits on the
//  first comment line and is never touched.
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-05
//=============================================================================
#include "pch.h"
#include "CameraPresetIO.h"
#include "CameraPresetCatalog.h"
#include "log.h"

#include <windows.h>
#include <vector>
#include <utility>

namespace
{
    // UTF-8 -> UTF-16. Preset type/angle come from the model as narrow strings.
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                            static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                              static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    // The seven keys that make up an envelope, in the order CameraViewStore writes
    // them. Deleting iterates this same list, so the two can never drift.
    const wchar_t* const kEnvKeys[] = { L"LimitsEnabled",
                                        L"MinPitch", L"MaxPitch",
                                        L"MinYaw",   L"MaxYaw",
                                        L"MinRoll",  L"MaxRoll" };

    std::wstring LastErrorText()
    {
        const DWORD e = ::GetLastError();
        wchar_t buf[64];
        swprintf_s(buf, L"Windows error %lu", static_cast<unsigned long>(e));
        return buf;
    }

    // True when [sec] already exists in the file. GetPrivateProfileSectionW returns
    // 0 for both "absent" and "present but empty"; a preset section always carries
    // at least X/Y/Z, so 0 means absent for our purposes.
    bool SectionExists(const std::wstring& sec, const std::wstring& path)
    {
        std::vector<wchar_t> buf(4096);
        const DWORD got = ::GetPrivateProfileSectionW(sec.c_str(), buf.data(),
                                                      static_cast<DWORD>(buf.size()),
                                                      path.c_str());
        return got > 0;
    }
}

//
// WriteEnvelope — see header.
//
bool CameraPresetIO::WriteEnvelope(const std::wstring& iniPath,
                                   const std::string& type, const std::string& angle,
                                   const GimbalEnvelope& env, std::wstring& outError)
{
    outError.clear();

    if (iniPath.empty() || type.empty() || angle.empty())
    {
        outError = L"Internal error: empty Cameras.ini path or preset name.";
        return false;
    }

    if (::GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        outError = L"File not found: " + iniPath;
        return false;
    }

    const std::wstring sec = L"CameraViews." + Widen(type) + L"." + Widen(angle);

    // Guard against inventing a preset. WritePrivateProfileStringW appends a new
    // section at EOF when the named one is missing, so a stale or mistyped
    // type/angle would silently grow the file a phantom camera.
    if (!SectionExists(sec, iniPath))
    {
        outError = L"No [" + sec + L"] section in " + iniPath +
                   L"\n\nThe camera preset this shot is mounted on is not in that file.";
        return false;
    }

    bool ok = true;

    if (!env.defined)
    {
        // Whole block or nothing: a nullptr value DELETES the key. Writing zeros
        // instead would leave a preset claiming a fully-locked envelope.
        for (const wchar_t* k : kEnvKeys)
            ok &= (::WritePrivateProfileStringW(sec.c_str(), k, nullptr, iniPath.c_str()) != 0);
    }
    else
    {
        wchar_t buf[64];

        swprintf_s(buf, L"%d", env.enabled ? 1 : 0);
        ok &= (::WritePrivateProfileStringW(sec.c_str(), L"LimitsEnabled", buf, iniPath.c_str()) != 0);

        // "%f" (6 decimals) matches FString::Printf(TEXT("MinPitch=%f")) exactly, so a
        // round trip through DISBrowser's own save produces no spurious textual diff.
        const std::pair<const wchar_t*, double> vals[] = {
            { L"MinPitch", env.minPitch }, { L"MaxPitch", env.maxPitch },
            { L"MinYaw",   env.minYaw   }, { L"MaxYaw",   env.maxYaw   },
            { L"MinRoll",  env.minRoll  }, { L"MaxRoll",  env.maxRoll  },
        };
        for (const auto& kv : vals)
        {
            swprintf_s(buf, L"%f", kv.second);
            ok &= (::WritePrivateProfileStringW(sec.c_str(), kv.first, buf, iniPath.c_str()) != 0);
        }
    }

    if (!ok)
    {
        outError = L"Could not write " + iniPath + L" (" + LastErrorText() + L").";
        return false;
    }

    // Flush the profile cache so the bytes are on disk before anyone re-reads.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());

    char msg[512];
    sprintf_s(msg, sizeof(msg), "CameraPresetIO: %S envelope in %S",
              env.defined ? L"wrote" : L"cleared", iniPath.c_str());
    LOG(msg);
    return true;
}

//
// DisBrowserCamerasIniPath — see header.
//
std::wstring CameraPresetIO::DisBrowserCamerasIniPath(const std::wstring& disBrowserProjectDir)
{
    if (disBrowserProjectDir.empty()) return {};

    const std::wstring path = disBrowserProjectDir + L"\\Config\\Cameras.ini";

    // Only report a path that is really there. The sibling fallback is a guess, and
    // silently "succeeding" against a non-existent folder would be worse than saying
    // the copy was skipped. Note we do NOT create this file: Cameras.ini without
    // DISBrowser's own presets would be an empty catalog, not a useful one.
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return path;
}

//
// WriteEnvelopeBothCopies — see header.
//
bool CameraPresetIO::WriteEnvelopeBothCopies(const std::wstring& editorIniPath,
                                             const std::wstring& disBrowserProjectDir,
                                             const std::string& type, const std::string& angle,
                                             const GimbalEnvelope& env,
                                             std::wstring& outWarning, std::wstring& outError)
{
    outWarning.clear();
    outError.clear();

    // The editor's copy is the one that must succeed — it is what this app reads
    // back, and what the operator sees next time the dialog opens.
    if (!WriteEnvelope(editorIniPath, type, angle, env, outError))
        return false;

    const std::wstring dbPath = DisBrowserCamerasIniPath(disBrowserProjectDir);
    if (dbPath.empty())
    {
        outWarning = L"Saved to the editor's config\\Cameras.ini only.\n\n"
                     L"DISBrowser's own Config\\Cameras.ini could not be found, so the "
                     L"envelope will not affect playback until the DISBrowser project "
                     L"folder is set on the Run tab and the file is copied across.";
        return true;
    }

    std::wstring dbError;
    if (!WriteEnvelope(dbPath, type, angle, env, dbError))
    {
        outWarning = L"Saved to the editor's config\\Cameras.ini, but DISBrowser's copy "
                     L"could not be written:\n\n" + dbError +
                     L"\n\nPlayback will ignore the envelope until that file is updated.";
        return true;
    }

    return true;
}
