#include "pch.h"
#include "ScenarioEditor.h"
#include "ScenarioEditorDialog.h"
#include "Resource.h"
#include "../log.h"

#include <string>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CScenarioEditorApp theApp;

CScenarioEditorApp::CScenarioEditorApp() = default;

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

    // Compute the exe directory once; both EntityTypeCatalog.ini and
    // settings.ini live next to the exe (spec §7.5, §11.5).
    wchar_t exePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    const size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        exeDir.resize(slash + 1);

    m_catalog.LoadFromIni(exeDir + L"EntityTypeCatalog.ini");

    m_settingsPath = exeDir + L"settings.ini";
    SettingsIO::Load(m_settings, m_settingsPath);

    CScenarioEditorDialog dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    LOG("ScenarioEditor V1 exiting");
    return FALSE;
}
