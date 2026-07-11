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
    // the exe dir that contains EntityTypeCatalog.ini (the repo root,
    // ...\ScenarioEditor\EntityTypeCatalog.ini — the same file the DISBrowser
    // UE project reads via Hanger.ini's EntityCatalog path). We start one level
    // ABOVE the exe dir so the build-output copy sitting next to the exe is
    // skipped, not matched. Falls back to the exe-dir copy for a shipped
    // standalone where no such ancestor exists.
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

        const std::wstring candidate = dir + L"EntityTypeCatalog.ini";
        if (fileExists(candidate))
        {
            m_catalogPath = candidate;
            break;
        }
    }

    sprintf_s(szError, sizeof(szError), "Catalog path: %S", m_catalogPath.c_str());
    LOG(szError);
    m_catalog.LoadFromIni(m_catalogPath);

    m_settingsPath = exeDir + L"settings.ini";
    SettingsIO::Load(m_settings, m_settingsPath);

    CScenarioEditorDialog dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    Direct2DContext::Shutdown();

    LOG("ScenarioEditor V1 exiting");
    return FALSE;
}
