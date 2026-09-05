//=============================================================================
//  ScenarioEditor.cpp
//-----------------------------------------------------------------------------
//  Implements CScenarioEditorApp. Defines the theApp singleton and its
//  InitInstance: enables high-DPI awareness, locates and loads the entity
//  catalog / settings / plays files, and shows the main editor dialog.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "ScenarioEditor.h"
#include "ScenarioEditorDialog.h"
#include "Resource.h"
#include "Direct2DContext.h"
#include "../log.h"

#include <string>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CScenarioEditorApp theApp;

CScenarioEditorApp::CScenarioEditorApp() = default;

//
// InitInstance — one-time app startup. Declares per-monitor-v2 DPI awareness
// before any window exists, initializes common controls, resolves the canonical
// EntityTypeCatalog.ini (nearest ancestor of the exe dir, so editor writes hit
// the source-controlled copy), loads settings.ini and plays.ini (seeding plays
// on first run), then runs the main dialog modally. Returns FALSE so MFC exits
// after the modal dialog closes.
//
BOOL CScenarioEditorApp::InitInstance()
{
    // DPI awareness: declare per-monitor v2 BEFORE any window is created.
    // Without this, on Windows 11 with high-DPI scaling (e.g. 4K @ 150%),
    // MFC dialog buttons can have rendered position vs. hit-test position
    // mismatch, making them visually present but click-unreachable.
    typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
    if (HMODULE hUser = ::GetModuleHandleW(L"user32.dll"))
    {
        auto pSet = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
            ::GetProcAddress(hUser, "SetProcessDpiAwarenessContext"));
        if (pSet)
        {
            // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            pSet((HANDLE)(-4));
        }
    }

    INITCOMMONCONTROLSEX initCtrls = { sizeof(INITCOMMONCONTROLSEX),
                                       ICC_WIN95_CLASSES | ICC_DATE_CLASSES };
    InitCommonControlsEx(&initCtrls);

    CWinApp::InitInstance();

    // NOTE: AfxEnableControlContainer was here but removed — it can subclass
    // dialog box management in a way that breaks BN_CLICKED routing for
    // buttons inside child dialogs (the asset/motion page action buttons).
    // We don't host any ActiveX controls, so we don't need it.
    SetRegistryKey(_T("ScenarioEditor"));

    LOG("ScenarioEditor V1 starting");

    // Compute the exe directory once; settings.ini lives next to the exe
    // (spec §11.5). The catalog is different: the in-app Catalog Editor reads
    // AND writes it, so it must live in a stable, source-controlled location
    // that the build/deploy step never overwrites. Otherwise every rebuild
    // copies the source catalog over the exe-dir copy and silently clobbers
    // any edits made through the editor.
    wchar_t exePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    const size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        exeDir.resize(slash + 1);

    // Resolve the catalog to the canonical source copy: the first ANCESTOR of
    // the exe dir whose config\ subdirectory contains EntityTypeCatalog.ini
    // (the repo copy, ...\ScenarioEditor\config\EntityTypeCatalog.ini — the same
    // file the DISBrowser UE project reads via Hanger.ini's EntityCatalog path).
    // We start one level ABOVE the exe dir so the flat build-output copy sitting
    // next to the exe is skipped, not matched. Falls back to the exe-dir copy for
    // a shipped standalone where no such ancestor exists (the post-build step
    // copies config\*.ini flat next to the exe).
    auto fileExists = [](const std::wstring& p) -> bool {
        const DWORD attrs = ::GetFileAttributesW(p.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES &&
               !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    };

    m_catalogPath = exeDir + L"EntityTypeCatalog.ini";   // standalone fallback
    std::wstring dir = exeDir;
    for (int up = 0; up < 8; ++up)
    {
        // Strip the trailing separator, then ascend one path component.
        if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
            dir.pop_back();
        const size_t upSlash = dir.find_last_of(L"\\/");
        if (upSlash == std::wstring::npos)
            break;
        dir.resize(upSlash + 1);

        const std::wstring candidate = dir + L"config\\EntityTypeCatalog.ini";
        if (fileExists(candidate))
        {
            m_catalogPath = candidate;
            break;
        }
    }

    sprintf_s(szError, sizeof(szError), "Catalog path: %S", m_catalogPath.c_str());
    LOG(szError);
    m_catalog.LoadFromIni(m_catalogPath);

    // Camera-view presets resolve exactly like the catalog: first ancestor
    // config\Cameras.ini, else the flat exe-dir copy.
    m_cameraPresetsPath = exeDir + L"Cameras.ini";       // standalone fallback
    {
        std::wstring cdir = exeDir;
        for (int up = 0; up < 8; ++up)
        {
            if (!cdir.empty() && (cdir.back() == L'\\' || cdir.back() == L'/'))
                cdir.pop_back();
            const size_t upSlash = cdir.find_last_of(L"\\/");
            if (upSlash == std::wstring::npos)
                break;
            cdir.resize(upSlash + 1);
            const std::wstring candidate = cdir + L"config\\Cameras.ini";
            if (fileExists(candidate))
            {
                m_cameraPresetsPath = candidate;
                break;
            }
        }
    }
    sprintf_s(szError, sizeof(szError), "Camera presets path: %S", m_cameraPresetsPath.c_str());
    LOG(szError);
    m_cameraPresets.LoadFromIni(m_cameraPresetsPath);
    // m_hangerTypes is deliberately NOT loaded here — see HangerTypes().

    m_settingsPath = exeDir + L"settings.ini";
    SettingsIO::Load(m_settings, m_settingsPath);

    // Plays catalog lives next to the exe, like settings.ini. On first run (or
    // a missing/foreign file) seed it from the historical hard-coded plays and
    // write it out so the accordion looks unchanged and the file is editable.
    m_playsPath = exeDir + L"plays.ini";
    if (!m_plays.LoadFromIni(m_playsPath))
    {
        m_plays.SeedDefaults();
        m_plays.SaveToIni(m_playsPath);
    }

    CScenarioEditorDialog dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    Direct2DContext::Shutdown();

    LOG("ScenarioEditor V1 exiting");
    return FALSE;
}

//-----------------------------------------------------------------------------
// HangerTypes — lazily load DISBrowser's Hanger.ini tuple -> camera-type map
//   Resolved from the Run tab's DISBrowser project folder when set, else a
//   sibling "DISBrowser" beside the exe (the same fallback the Run tab uses).
//   Retried on every call while still empty so setting the folder mid-session
//   starts filtering without a restart; once loaded it is not re-read.
//
const HangerCameraTypeMap& CScenarioEditorApp::HangerTypes()
{
    if (!m_hangerTypes.Loaded())
        m_hangerTypes.LoadFromIni(DisBrowserProjectDir() + L"\\Config\\Hanger.ini");
    return m_hangerTypes;
}

//
// DisBrowserProjectDir — see header. Shared by HangerTypes() above and by
//   CameraPresetIO, which needs the same root to reach DISBrowser's own copy of
//   Cameras.ini. Kept in one place so the two can never disagree about where
//   DISBrowser lives.
//
std::wstring CScenarioEditorApp::DisBrowserProjectDir() const
{
    std::wstring projDir;
    if (!m_settings.disBrowserProjectDir.empty())
    {
        // Settings holds it narrow; CString does the ANSI->wide conversion.
        const CString wide(m_settings.disBrowserProjectDir.c_str());
        projDir = (LPCWSTR)wide;
    }
    else
    {
        wchar_t exe[MAX_PATH] = { 0 };
        ::GetModuleFileNameW(nullptr, exe, _countof(exe));
        std::wstring p(exe);
        const size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p.resize(slash);
        projDir = p + L"\\..\\..\\..\\DISBrowser";
    }
    if (!projDir.empty() && (projDir.back() == L'\\' || projDir.back() == L'/'))
        projDir.pop_back();
    return projDir;
}
