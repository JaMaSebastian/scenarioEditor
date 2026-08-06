//=============================================================================
//  ScenarioEditorDialog.cpp
//-----------------------------------------------------------------------------
//  Implements CScenarioEditorDialog: dialog init, toolbar/status-bar/page
//  creation and layout, tab switching, File menu (New/Open/Save/Save As),
//  playback control and worker status handling, validation, dirty tracking,
//  and window-placement persistence.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "ScenarioEditorDialog.h"
#include "AttributesDialog.h"
#include "PduBuilder.h"
#include "ScenarioEditor.h"   // for theApp.Catalog()/Settings()/Plays()
#include "ScenarioIO.h"
#include "ScenarioCameraIO.h"   // legacy <scenario>-camera.ini import (migration only)
#include "ScenarioPaths.h"      // CameraPathFor()
#include "ScrollablePage.h"     // FitWindowToMonitorWorkArea()
#include "SettingsIO.h"
#include "Validator.h"
#include "../log.h"
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    constexpr int kStatusBarReserveBottomPx = 40;
    constexpr int kToolBarReserveTopPx = 72;

    static UINT s_statusIndicators[] = {
        ID_INDICATOR_STATE,
        ID_INDICATOR_TIME,
        ID_INDICATOR_PROGRESS,
        ID_INDICATOR_ERRORS,
        ID_INDICATOR_WARNINGS,
        ID_INDICATOR_INFO,
        ID_INDICATOR_PDUS,
        ID_INDICATOR_BW,
    };

    void LogCommand(UINT nID, const char* label)
    {
        sprintf_s(szError, sizeof(szError), "Placeholder command id=%u (%s)", nID, label);
        LOG(szError);
    }

    //
    // IsIpv4MulticastGroup — true if text is a dotted-quad IPv4 address inside
    //   224.0.0.0/4. Used to reject a unicast address typed into the multicast
    //   group field, which would otherwise send as plain unicast without warning
    //   (UdpSender only applies the multicast socket options for this range).
    //
    bool IsIpv4MulticastGroup(const std::string& text)
    {
        unsigned a = 0, b = 0, c = 0, d = 0;
        char trailing = 0;
        // %c catches trailing junk such as "239.1.2.3x" or "239.1.2.3.4".
        const int fields = sscanf_s(text.c_str(), "%u.%u.%u.%u%c",
                                    &a, &b, &c, &d, &trailing, 1);
        if (fields != 4) return false;
        if (a > 255 || b > 255 || c > 255 || d > 255) return false;
        return a >= 224 && a <= 239;   // 224.0.0.0/4
    }

    // The camera schedule now lives inside scenario.ini ([Camera.N] sections).
    // Older scenarios kept it in a sibling "<scenario>-camera.ini". When a freshly
    // loaded scenario carries no cameras, import that legacy side-file (if present)
    // so those frames aren't lost; they fold into scenario.ini on the next save,
    // after which DoSaveTo deletes the now-redundant side-file.
    void MigrateLegacyCameras(Scenario& loaded, const CString& iniPath)
    {
        if (!loaded.cameras.empty()) return;
        ScenarioCameraIO::Load(loaded,
            std::wstring(CT2W(CameraPathFor(iniPath))));
    }

    // Leaf filename of a path (after the last \ or /), e.g. "harburtField.ini".
    CString LeafName(const CString& path)
    {
        const int b = path.ReverseFind(_T('\\'));
        const int f = path.ReverseFind(_T('/'));
        const int slash = (b > f) ? b : f;
        return (slash >= 0) ? path.Mid(slash + 1) : path;
    }

    // When even the NEAREST enabled entity is this far from the origin, the origin
    // is effectively disconnected from the scene: the flat map/preview projection
    // (valid only near the origin) smears and entities render on the far side of the
    // globe. A normal local scenario keeps its entities within a few tens of km.
    constexpr double kOriginDisconnectMeters = 500000.0;   // 500 km

    // Horizontal distance (m) from the origin to the nearest enabled entity, using
    // each entity's local ENU-from-origin coordinates (kept in sync with the origin
    // by load/relocate). HUGE_VAL when there are no enabled entities.
    double NearestEntityDistanceM(const Scenario& s)
    {
        double best = HUGE_VAL;
        for (const Entity& e : s.entities)
        {
            if (!e.enabled) continue;
            best = std::min(best, std::hypot(e.localX, e.localY));
        }
        return best;
    }

    // Non-blocking warning shown after a load whose origin sits far from its
    // entities (a valid but confusing file — e.g. the origin was relocated with
    // "Move entities" off and the entities stayed fixed on the Earth).
    void WarnIfOriginDisconnected(const Scenario& s, const CString& leaf)
    {
        const double d = NearestEntityDistanceM(s);
        // Warn only for a real, finite disconnect; HUGE_VAL means no enabled entities.
        if (!(d > kOriginDisconnectMeters && d < HUGE_VAL)) return;
        CString msg;
        msg.Format(_T("%s loaded, but its origin is about %.0f km from the scenario's ")
                   _T("entities, so the map and preview will look wrong (the entities ")
                   _T("render far from the origin).\n\nOn the Preview tab, pick a Location ")
                   _T("near the entities, or enable \"Move entities\" before relocating the origin."),
                   static_cast<LPCTSTR>(leaf), d / 1000.0);
        AfxMessageBox(msg, MB_OK | MB_ICONWARNING);
    }
}

CScenarioEditorDialog::CScenarioEditorDialog(CWnd* pParent)
    : CDialogEx(IDD, pParent)
{
}

void CScenarioEditorDialog::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_MAIN_TABCTRL, m_tabCtrl);
}

BEGIN_MESSAGE_MAP(CScenarioEditorDialog, CDialogEx)
    ON_WM_SIZE()
    ON_NOTIFY(TCN_SELCHANGE, IDC_MAIN_TABCTRL, &CScenarioEditorDialog::OnTabSelChange)
    ON_COMMAND(ID_APP_EXIT, &CScenarioEditorDialog::OnFileExit)
    ON_COMMAND(ID_HELP_ABOUT, &CScenarioEditorDialog::OnHelpAbout)
    ON_COMMAND(ID_TB_LOOP_HOST, &CScenarioEditorDialog::OnLoopToggle)
    ON_UPDATE_COMMAND_UI(ID_TB_LOOP_HOST, &CScenarioEditorDialog::OnUpdateLoopToggle)
    ON_COMMAND(ID_TB_HELP_TOGGLE, &CScenarioEditorDialog::OnHelpToggle)
    ON_UPDATE_COMMAND_UI(ID_TB_HELP_TOGGLE, &CScenarioEditorDialog::OnUpdateHelpToggle)
    ON_COMMAND(ID_PLAYBACK_START,        &CScenarioEditorDialog::OnPlaybackStart)
    ON_COMMAND(ID_PLAYBACK_PAUSE,        &CScenarioEditorDialog::OnPlaybackPause)
    ON_COMMAND(ID_PLAYBACK_RESUME,       &CScenarioEditorDialog::OnPlaybackResume)
    ON_COMMAND(ID_PLAYBACK_STOP,         &CScenarioEditorDialog::OnPlaybackStop)
    ON_MESSAGE(WM_APP_PLAYBACK_STATUS,   &CScenarioEditorDialog::OnPlaybackStatusMessage)
    ON_MESSAGE(WM_APP_PLAYBACK_PROGRESS, &CScenarioEditorDialog::OnPlaybackProgressMessage)
    ON_MESSAGE(WM_APP_PLAYBACK_ERROR,    &CScenarioEditorDialog::OnPlaybackErrorMessage)
    ON_WM_DESTROY()
    ON_WM_CLOSE()
    ON_WM_TIMER()
    ON_COMMAND(ID_FILE_NEW_SCENARIO,     &CScenarioEditorDialog::OnFileNew)
    ON_COMMAND(ID_FILE_OPEN_SCENARIO,    &CScenarioEditorDialog::OnFileOpen)
    ON_COMMAND(ID_FILE_SAVE_SCENARIO,    &CScenarioEditorDialog::OnFileSave)
    ON_COMMAND(ID_FILE_SAVE_SCENARIO_AS, &CScenarioEditorDialog::OnFileSaveAs)
    ON_COMMAND(ID_FILE_OPEN_RECORDING,   &CScenarioEditorDialog::OnOpenRecording)
    ON_COMMAND(ID_TOOLS_REPLAY_FILE,     &CScenarioEditorDialog::OnReplayFile)
    ON_COMMAND(ID_SCENARIO_VALIDATE,         &CScenarioEditorDialog::OnScenarioValidate)
    ON_MESSAGE(WM_APP_MARK_DIRTY,            &CScenarioEditorDialog::OnMarkDirtyMessage)
    ON_MESSAGE(WM_APP_REFRESH_UI,            &CScenarioEditorDialog::OnRefreshUiMessage)
    // Keyboard shortcuts that synthesize BN_CLICKED to the appropriate page,
    // so users can drive the app entirely from the keyboard if mouse clicks
    // on push-buttons aren't being delivered by their input setup.
    ON_COMMAND(ID_KB_ADD_ASSET,        &CScenarioEditorDialog::OnKbAddAsset)
    ON_COMMAND(ID_KB_DELETE_ASSET,     &CScenarioEditorDialog::OnKbDeleteAsset)
    ON_COMMAND(ID_KB_DUPLICATE_ASSET,  &CScenarioEditorDialog::OnKbDuplicateAsset)
    ON_COMMAND(ID_KB_MOVE_ASSET,       &CScenarioEditorDialog::OnKbMoveAsset)
    ON_COMMAND(ID_KB_ADD_SEGMENT,      &CScenarioEditorDialog::OnKbAddSegment)
    ON_COMMAND(ID_KB_DELETE_SEGMENT,   &CScenarioEditorDialog::OnKbDeleteSegment)
    ON_COMMAND(ID_KB_DUPLICATE_SEGMENT,&CScenarioEditorDialog::OnKbDuplicateSegment)
    ON_COMMAND(ID_TOOLS_OPEN_ATTRIBUTES,&CScenarioEditorDialog::OnOpenAttributes)
    ON_COMMAND_RANGE(ID_SCENARIO_GENERATE_RECORDING, ID_SCENARIO_RESET_CLOCK, &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND_RANGE(ID_PLAYBACK_LOOP,        ID_PLAYBACK_SPEED_10,      &CScenarioEditorDialog::OnPlaceholderCommand)
    // Specific entry must precede the catch-all range below — MFC walks the
    // message map in order, so this claims ID_NETWORK_TEST_MULTICAST before
    // OnPlaceholderCommand's range (ID_NETWORK_OPEN_OUTPUT..ID_NETWORK_TEST_TCP)
    // can swallow it.
    ON_COMMAND(ID_NETWORK_TEST_MULTICAST, &CScenarioEditorDialog::OnNetworkTestMulticast)
    ON_COMMAND(ID_NETWORK_TEST_TCP,       &CScenarioEditorDialog::OnNetworkTestTcp)
    ON_COMMAND_RANGE(ID_NETWORK_OPEN_OUTPUT,  ID_NETWORK_TEST_TCP,       &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND_RANGE(ID_VIEW_VALIDATION_PANEL, ID_VIEW_VALIDATION_PANEL, &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND_RANGE(ID_HELP_PROTOCOL_NOTES,  ID_HELP_FILE_FORMAT_NOTES, &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND(ID_TOOLS_RECORD,          &CScenarioEditorDialog::OnPlaybackStart)
    ON_COMMAND_RANGE(ID_TOOLS_SEND_LIVE,       ID_TOOLS_SEND_LIVE,        &CScenarioEditorDialog::OnPlaceholderCommand)
    // Speed combo is captured into m_scenario.output.playbackSpeed at
    // CaptureUiIntoScenario time; no per-change handler needed.
END_MESSAGE_MAP()

//
// OnInitDialog — build the UI: tab labels, status bar (with 12pt font),
// toolbar, and hosted pages; restore persisted window placement (clamped to
// the work area); auto-load the last scenario if it still exists; then clear
// the dirty flag once initial population is done.
//
BOOL CScenarioEditorDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // Gate dirty tracking until the dialog finishes initial population. The
    // tab pages' first ReadFrom() calls fire EN_CHANGE on every edit.
    m_suppressDirty = true;

    m_hAccel = ::LoadAccelerators(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_MAIN_ACCEL));

    // Tab control labels (§5.2). The Scenario Setup / Asset-Entity / Motion
    // Path authoring tabs moved to the modal Attributes notebook, opened from
    // the Output/Playback page's "Attributes..." button.
    m_tabCtrl.InsertItem(0, _T("Run"));
    m_tabCtrl.InsertItem(1, _T("Plays"));
    m_tabCtrl.InsertItem(2, _T("Deploy"));
    m_tabCtrl.InsertItem(3, _T("Preview"));

    // Status bar (§5.4). The status bar is created at runtime — not from
    // a dialog template — so it doesn't pick up the dialog's 12pt font.
    // Build a matching CFont once and apply it so its pane text scales
    // with the rest of the UI.
    if (m_statusBar.Create(this) && m_statusBar.SetIndicators(s_statusIndicators,
                                                              sizeof(s_statusIndicators) / sizeof(UINT)))
    {
        if (m_uiFont.GetSafeHandle() == nullptr)
            m_uiFont.CreatePointFont(120 /* tenths of a pt = 12pt */,
                                     _T("MS Shell Dlg"));
        m_statusBar.SetFont(&m_uiFont);
        SeedStatusBar();
        // Positioning + sizing of the status bar is owned by LayoutChildren
        // — MFC's CStatusBar doesn't recompute its height when SetFont is
        // called, so we don't trust its auto-layout for the 12pt font.
    }

    CreateToolBar();
    CreatePages();

    // Initial layout + activate first tab
    LayoutChildren();
    ShowPage(0);
    UpdateTitle();

    // Apply persisted window state, clamped to the work area of the monitor the
    // saved rectangle lands on (nearest monitor if that display is gone). The
    // clamp used to go through SM_CXFULLSCREEN/SM_CYFULLSCREEN, which only ever
    // describe the PRIMARY display — on a multi-monitor desk that either
    // refused to restore a window saved at negative coordinates (a screen left
    // of / above the primary one) or squeezed it to the primary monitor's size.
    // With no saved placement, still fit the template-sized dialog to the
    // current monitor so a small panel doesn't push the status bar off-screen.
    {
        const Settings& cfg = theApp.Settings();
        const bool hasSavedPos = !(cfg.windowX == -1 && cfg.windowY == -1);
        if (hasSavedPos && cfg.windowWidth >= 320 && cfg.windowHeight >= 240)
        {
            CRect rc(cfg.windowX, cfg.windowY,
                     cfg.windowX + cfg.windowWidth,
                     cfg.windowY + cfg.windowHeight);
            ClampRectToMonitorWorkArea(rc);
            ::SetWindowPos(GetSafeHwnd(), nullptr,
                           rc.left, rc.top, rc.Width(), rc.Height(),
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            FitWindowToMonitorWorkArea(this, /*center*/ true);
        }

        if (cfg.windowMaximized)
            ShowWindow(SW_SHOWMAXIMIZED);
    }

    // Auto-load last opened scenario. If settings still record a path, report any
    // problem by name and continue with the default scenario rather than blocking:
    //   - file gone (moved/deleted)  -> "<name>.ini file not found."
    //   - present but won't load     -> malformed / newer-version / read-error box.
    //   - loads Ok but origin sits far from its entities -> disconnect warning
    //     (WarnIfOriginDisconnected; the file is valid, just confusing to view).
    // An empty path (no last scenario) auto-loads nothing and is silent. Either way,
    // RefreshUiFromScenario() runs below so the asset tree / motion list populate.
    if (!theApp.Settings().lastScenarioPath.empty())
    {
        const std::wstring lastPath(CA2W(theApp.Settings().lastScenarioPath.c_str()));

        // Leaf filename for user-facing messages (e.g. "harburtField.ini").
        const CString leaf = LeafName(CString(lastPath.c_str()));

        if (::GetFileAttributesW(lastPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            // Recorded last scenario no longer exists (moved/deleted).
            sprintf_s(szError, sizeof(szError),
                      "Startup: last scenario not found: %S", lastPath.c_str());
            LOG(szError);

            CString msg;
            msg.Format(_T("%s file not found."), static_cast<LPCTSTR>(leaf));
            AfxMessageBox(msg, MB_OK | MB_ICONWARNING);
        }
        else
        {
            Scenario loaded;
            const ScenarioIO::Result rc = ScenarioIO::Load(loaded, lastPath);
            if (rc == ScenarioIO::Result::Ok)
            {
                MigrateLegacyCameras(loaded, CString(lastPath.c_str()));
                m_scenario = std::move(loaded);
                SetCurrentScenarioPath(CString(lastPath.c_str()));
                WarnIfOriginDisconnected(m_scenario, leaf);
            }
            else
            {
                // File exists but couldn't be loaded. Report by name (details in the
                // log) and fall through to the default scenario.
                sprintf_s(szError, sizeof(szError),
                          "Startup: failed to load last scenario (result=%d): %S",
                          static_cast<int>(rc), lastPath.c_str());
                LOG(szError);

                CString msg;
                switch (rc)
                {
                    case ScenarioIO::Result::VersionTooNew:
                        msg.Format(_T("%s was written by a newer version of ScenarioEditor ")
                                   _T("and can't be loaded."), static_cast<LPCTSTR>(leaf));
                        break;
                    case ScenarioIO::Result::Malformed:
                        msg.Format(_T("%s is malformed or corrupt. See error.log."),
                                   static_cast<LPCTSTR>(leaf));
                        break;
                    case ScenarioIO::Result::IoError:
                    default:
                        msg.Format(_T("Could not read %s. See error.log."),
                                   static_cast<LPCTSTR>(leaf));
                        break;
                }
                AfxMessageBox(msg, MB_OK | MB_ICONWARNING);
            }
        }
    }
    RefreshUiFromScenario();

    m_suppressDirty = false;
    ClearDirty();

    LOG("Main dialog initialized");
    return TRUE;
}

void CScenarioEditorDialog::OnTimer(UINT_PTR nIDEvent)
{
    CDialogEx::OnTimer(nIDEvent);
}

//
// CreateToolBar — create the flat command toolbar hosted in the dialog, load
// its bitmap, and swap the Speed placeholder button for a real combo box.
//
void CScenarioEditorDialog::CreateToolBar()
{
    // Toolbar buttons/images sized to match the 54x54 cells emitted by
    // src/make_toolbar_bmp.py. SetSizes is static and must be called before
    // any CMFCToolBar is created.
    CMFCToolBar::SetSizes(CSize(60, 60), CSize(54, 54));

    // Bitmapped command bar (§5.3). Hosted directly in the dialog rather than
    // docked in a CFrameWnd — see LayoutChildren() for positioning.
    if (!m_toolBar.CreateEx(this,
                            TBSTYLE_FLAT,
                            WS_CHILD | WS_VISIBLE | CBRS_TOP | CBRS_TOOLTIPS | CBRS_FLYBY))
    {
        LOG("CMFCToolBar::CreateEx failed");
        return;
    }
    if (!m_toolBar.LoadToolBar(IDR_MAIN_TOOLBAR))
    {
        LOG("CMFCToolBar::LoadToolBar(IDR_MAIN_TOOLBAR) failed");
        return;
    }

    // Replace the Speed dummy button with a real combo (§5.3). The image
    // index keeps the speedometer glyph from the loaded bitmap.
    int speedImage = -1;
    const int speedIdx = m_toolBar.CommandToIndex(ID_TB_SPEED_HOST);
    if (speedIdx >= 0)
    {
        if (CMFCToolBarButton* dummy = m_toolBar.GetButton(speedIdx))
            speedImage = dummy->GetImage();
    }
    CMFCToolBarComboBoxButton speedCombo(ID_TB_SPEED_HOST,
                                         speedImage,
                                         CBS_DROPDOWNLIST | WS_VSCROLL,
                                         70);
    const TCHAR* speeds[] = { _T("0.25x"), _T("0.5x"), _T("1x"), _T("2x"), _T("10x") };
    for (const auto* s : speeds)
        speedCombo.AddItem(s);
    speedCombo.SelectItem(2); // 1x
    m_toolBar.ReplaceButton(ID_TB_SPEED_HOST, speedCombo);

    m_toolBar.SetWindowText(_T("Command Toolbar"));
}

//
// PreTranslateMessage — route the accelerator table first so keyboard
// shortcuts fire before default dialog handling.
//
BOOL CScenarioEditorDialog::PreTranslateMessage(MSG* pMsg)
{
    if (m_hAccel && ::TranslateAccelerator(m_hWnd, m_hAccel, pMsg))
        return TRUE;
    return CDialogEx::PreTranslateMessage(pMsg);
}

namespace
{
    struct StatusBarPane { int idx; UINT id; const TCHAR* seedText; int widthPx; };
    const StatusBarPane kStatusBarPanes[] = {
        // Pane widths sized for the 12pt status bar font at 150% DPI.
        { 0, ID_INDICATOR_STATE,    _T("State: Idle"),         260 },
        { 1, ID_INDICATOR_TIME,     _T("Time: 000.0s / 000s"), 340 },
        { 2, ID_INDICATOR_PROGRESS, _T("Progress: 0%"),        240 },
        { 3, ID_INDICATOR_ERRORS,   _T("Errors: 0"),           180 },
        { 4, ID_INDICATOR_WARNINGS, _T("Warnings: 0"),         210 },
        { 5, ID_INDICATOR_INFO,     _T("Info: 0"),             160 },
        { 6, ID_INDICATOR_PDUS,     _T("PDUs: 0"),             190 },
        { 7, ID_INDICATOR_BW,       _T("BW: 0.00 Mbps"),       260 },
    };
}

//
// ApplyStatusBarPaneWidths — (re)apply the fixed pane widths from
// kStatusBarPanes; must be called after any status-bar WM_SIZE resets them.
//
void CScenarioEditorDialog::ApplyStatusBarPaneWidths()
{
    if (!::IsWindow(m_statusBar.GetSafeHwnd())) return;
    for (const auto& p : kStatusBarPanes)
        m_statusBar.SetPaneInfo(p.idx, p.id, SBPS_NORMAL, p.widthPx);
}

//
// SeedStatusBar — set pane widths and initial placeholder text for each pane.
//
void CScenarioEditorDialog::SeedStatusBar()
{
    if (!::IsWindow(m_statusBar.GetSafeHwnd())) return;
    ApplyStatusBarPaneWidths();
    for (const auto& p : kStatusBarPanes)
        m_statusBar.SetPaneText(p.idx, p.seedText);
}

//
// CreatePages — create the four tab pages as siblings of the tab control
// (a workaround for broken button-click delivery when pages are children of a
// CTabCtrl), hidden initially, and wire each page to the shared scenario.
//
void CScenarioEditorDialog::CreatePages()
{
    // Parent the pages directly to the main dialog (NOT to the tab control)
    // and overlay them on the tab's content area. Hosting PUSHBUTTONs inside
    // child dialogs that are children of a CTabCtrl breaks button click
    // delivery on some MFC + Windows configurations; making the pages
    // siblings of the tab control instead of children avoids the bug.
    auto create = [this](CDialogEx& page, UINT idd) -> CDialogEx*
    {
        if (page.Create(idd, this))
        {
            page.ShowWindow(SW_HIDE);
            return &page;
        }
        return nullptr;
    };

    // Output page edits Scenario::output. Seed the open-scenario path too so the
    // Run tab's "Configure Unreal" can locate this scenario's camera schedule.
    m_pageOutput.SetScenario(&m_scenario);
    m_pageOutput.SetScenarioPath(m_currentScenarioPath);

    // Preview page renders the scenario independently of the worker.
    m_pagePreview.SetScenario(&m_scenario);

    // Setup / Asset / Motion pages are created inside CAttributesDialog when
    // the user opens the Attributes notebook. Plays / Deploy are placeholder
    // scaffolds for now.
    m_pages[0] = create(m_pageOutput,  IDD_OUTPUT_PLAYBACK_PAGE);
    m_pages[1] = create(m_pagePlays,   IDD_PLAYS_PAGE);
    m_pages[2] = create(m_pageDeploy,  IDD_DEPLOY_PAGE);
    m_pages[3] = create(m_pagePreview, IDD_PREVIEW_PAGE);

    // The Plays page owns its own gear button (top-right of its client area)
    // and routes subcategory clicks back here to load/run scenarios.
    m_pagePlays.SetMainDialog(this);
}

//
// OpenAttributes — run the modal Attributes notebook on startTab; it edits the
// shared scenario in place, so on close refresh the UI and mark dirty if the
// user changed anything.
//
void CScenarioEditorDialog::OpenAttributes(int startTab)
{
    // The Attributes notebook edits the shared Scenario in place. Hand it the
    // live model + catalog + current field-help state; on close, pull the
    // edits back into the Preview/validation surfaces and flag dirty if the
    // user changed anything.
    CAttributesDialog dlg(&m_scenario, &theApp.Catalog(), m_helpChecked, startTab, this);
    dlg.DoModal();
    // The notebook edits the shared scenario in place and flushes pending edits
    // on close (both Close and Escape), so refresh regardless of how it was
    // dismissed; mark dirty if any page reported an edit.
    RefreshUiFromScenario();
    if (dlg.WasModified())
        MarkDirty();
}

void CScenarioEditorDialog::OnOpenAttributes()
{
    OpenAttributes(0);
}

//
// LoadScenarioForPlay — load (and optionally start) the scenario associated
// with a Plays subcategory: prompt on unsaved changes, replace the model,
// refresh the UI, and report any load error via message box.
//
void CScenarioEditorDialog::LoadScenarioForPlay(const CString& iniPath, bool alsoRun)
{
    if (iniPath.IsEmpty()) return;
    if (!MaybePromptSaveOnDiscard()) return;

    Scenario loaded;
    const ScenarioIO::Result rc =
        ScenarioIO::Load(loaded, std::wstring(CT2W(iniPath)));

    switch (rc)
    {
        case ScenarioIO::Result::Ok:
            MigrateLegacyCameras(loaded, iniPath);
            m_scenario = std::move(loaded);
            SetCurrentScenarioPath(iniPath);
            RefreshUiFromScenario();
            ClearDirty();
            sprintf_s(szError, sizeof(szError),
                      "Play loaded scenario from %S (run=%d)",
                      static_cast<const wchar_t*>(CT2W(iniPath)), alsoRun ? 1 : 0);
            LOG(szError);
            WarnIfOriginDisconnected(m_scenario, LeafName(iniPath));
            if (alsoRun) OnPlaybackStart();
            break;
        case ScenarioIO::Result::VersionTooNew:
            AfxMessageBox(_T("This scenario.ini was written by a newer version of ScenarioEditor."),
                          MB_OK | MB_ICONWARNING);
            break;
        case ScenarioIO::Result::Malformed:
            AfxMessageBox(_T("The associated scenario .ini is missing or malformed. See error.log."),
                          MB_OK | MB_ICONERROR);
            break;
        case ScenarioIO::Result::IoError:
        default:
            AfxMessageBox(_T("Could not open the associated scenario .ini. See error.log."),
                          MB_OK | MB_ICONERROR);
            break;
    }
}

//
// ShowPage — make the page at index the visible one: hide the previous page,
// position the new page over the tab's content rect, and sync the tab selection.
//
void CScenarioEditorDialog::ShowPage(int index)
{
    if (index < 0 || index >= 4) return;
    if (m_activePage == index) return;

    if (m_activePage >= 0 && m_pages[m_activePage])
        m_pages[m_activePage]->ShowWindow(SW_HIDE);

    m_activePage = index;
    if (m_pages[m_activePage])
    {
        // Tab content rect in tab-control client coords → map to dialog
        // client coords because the page's parent is now the dialog, not
        // the tab control.
        CRect rcDisplay;
        m_tabCtrl.GetClientRect(&rcDisplay);
        m_tabCtrl.AdjustRect(FALSE, &rcDisplay);
        m_tabCtrl.ClientToScreen(&rcDisplay);
        ScreenToClient(&rcDisplay);
        m_pages[m_activePage]->SetWindowPos(&CWnd::wndTop,
                                            rcDisplay.left, rcDisplay.top,
                                            rcDisplay.Width(), rcDisplay.Height(),
                                            SWP_NOACTIVATE);
        m_pages[m_activePage]->ShowWindow(SW_SHOW);
    }
    m_tabCtrl.SetCurSel(index);
}

//
// OnTabSelChange — tab-selection notification handler: show the newly
// selected page.
//
void CScenarioEditorDialog::OnTabSelChange(NMHDR*, LRESULT* pResult)
{
    ShowPage(m_tabCtrl.GetCurSel());
    if (pResult) *pResult = 0;
}

//
// LayoutChildren — explicitly position the status bar (bottom), toolbar (top),
// tab control, and the active page for the current client size. Owns layout
// itself because MFC's auto-layout mis-sizes the 12pt status bar.
//
void CScenarioEditorDialog::LayoutChildren()
{
    if (!::IsWindow(m_tabCtrl.GetSafeHwnd())) return;

    CRect rcClient;
    GetClientRect(&rcClient);

    // Pin the status bar to the bottom at our chosen height. MFC's CStatusBar
    // would otherwise pick a height based on its original (8pt) font metrics
    // and clip the 12pt pane text — so we own the layout here.
    const int bottomReserve = kStatusBarReserveBottomPx;
    if (::IsWindow(m_statusBar.GetSafeHwnd()))
    {
        m_statusBar.SetWindowPos(nullptr,
                                 0, rcClient.bottom - bottomReserve,
                                 rcClient.Width(), bottomReserve,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        // MFC's CStatusBar resets pane widths when it receives WM_SIZE
        // (which Windows always sends from the SetWindowPos above), so
        // re-apply our pane widths AFTER repositioning.
        ApplyStatusBarPaneWidths();
    }

    int toolBarH = 0;
    if (::IsWindow(m_toolBar.GetSafeHwnd()))
    {
        CSize sz = m_toolBar.CalcFixedLayout(FALSE, TRUE);
        toolBarH = sz.cy > 0 ? sz.cy : kToolBarReserveTopPx;
        m_toolBar.SetWindowPos(nullptr,
                               0, 0,
                               rcClient.Width(), toolBarH,
                               SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else
    {
        toolBarH = kToolBarReserveTopPx;
    }

    int tabTop = toolBarH + 2;
    int tabHeight = rcClient.Height() - tabTop - bottomReserve;
    if (tabHeight < 60) tabHeight = 60;

    m_tabCtrl.SetWindowPos(nullptr,
                           4, tabTop,
                           rcClient.Width() - 8, tabHeight,
                           SWP_NOZORDER | SWP_NOACTIVATE);

    if (m_activePage >= 0 && m_pages[m_activePage])
    {
        // Page is parented to the dialog now, not the tab control, so map
        // the tab's display rect from tab-client coords to dialog-client.
        CRect rcDisplay;
        m_tabCtrl.GetClientRect(&rcDisplay);
        m_tabCtrl.AdjustRect(FALSE, &rcDisplay);
        m_tabCtrl.ClientToScreen(&rcDisplay);
        ScreenToClient(&rcDisplay);
        m_pages[m_activePage]->SetWindowPos(&CWnd::wndTop,
                                            rcDisplay.left, rcDisplay.top,
                                            rcDisplay.Width(), rcDisplay.Height(),
                                            SWP_NOACTIVATE);
    }
}

//
// OnSize — re-run LayoutChildren on resize (deliberately not forwarding
// WM_SIZE to the status bar, which would clobber its custom height).
//
void CScenarioEditorDialog::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    // Don't forward WM_SIZE to the status bar — MFC's CControlBar handler
    // would re-position the bar to its internal default height (computed
    // from the original font), clobbering the 40-px slot we want for the
    // 12pt pane text. LayoutChildren below positions the bar explicitly.
    LayoutChildren();
}

//
// OnPlaceholderCommand — catch-all handler for not-yet-implemented menu/
// toolbar commands; just logs the command id and control label.
//
void CScenarioEditorDialog::OnPlaceholderCommand(UINT nID)
{
    CString label;
    if (CWnd* src = GetDlgItem(nID))
        src->GetWindowText(label);
    if (label.IsEmpty())
        label = _T("(no label - menu or accel)");
    CT2A ascii(label);
    LogCommand(nID, ascii.m_psz);
}

//
// OnFileExit — File > Exit: prompt to save if dirty, then end the dialog.
//
void CScenarioEditorDialog::OnFileExit()
{
    LOG("File > Exit");
    if (!MaybePromptSaveOnDiscard()) return;
    EndDialog(IDOK);
}

//
// OnCancel — Escape/Cancel: prompt to save if dirty before closing.
//
void CScenarioEditorDialog::OnCancel()
{
    if (!MaybePromptSaveOnDiscard()) return;
    CDialogEx::OnCancel();
}

//
// OnClose — window close (X button): prompt to save if dirty before closing.
//
void CScenarioEditorDialog::OnClose()
{
    if (!MaybePromptSaveOnDiscard()) return;
    CDialogEx::OnCancel();
}

//
// OnHelpAbout — Help > About: show the version/info message box.
//
void CScenarioEditorDialog::OnHelpAbout()
{
    LOG("Help > About");
    AfxMessageBox(_T("ScenarioEditor V1\nUI shell only (spec section 23.1).\n\nNo scenario logic wired."),
                  MB_OK | MB_ICONINFORMATION);
}

//
// OnLoopToggle — toggle looped playback, mirror it into the scenario output,
// mark dirty, and update the toolbar button's checked state.
//
void CScenarioEditorDialog::OnLoopToggle()
{
    m_loopChecked = !m_loopChecked;
    m_scenario.output.loopEnabled = m_loopChecked;
    MarkDirty();
    LogCommand(ID_TB_LOOP_HOST, m_loopChecked ? "Loop ON" : "Loop OFF");

    if (::IsWindow(m_toolBar.GetSafeHwnd()))
    {
        const int idx = m_toolBar.CommandToIndex(ID_TB_LOOP_HOST);
        if (idx >= 0)
        {
            if (CMFCToolBarButton* btn = m_toolBar.GetButton(idx))
            {
                btn->SetStyle(m_loopChecked
                              ? btn->m_nStyle | TBBS_CHECKED
                              : btn->m_nStyle & ~TBBS_CHECKED);
                m_toolBar.InvalidateButton(idx);
            }
        }
    }
}

void CScenarioEditorDialog::OnUpdateLoopToggle(CCmdUI* pCmdUI)
{
    pCmdUI->SetCheck(m_loopChecked ? 1 : 0);
}

//
// OnHelpToggle — toggle field-help mode, update the toolbar/menu checked
// state, and propagate the new state to every page.
//
void CScenarioEditorDialog::OnHelpToggle()
{
    m_helpChecked = !m_helpChecked;
    LogCommand(ID_TB_HELP_TOGGLE, m_helpChecked ? "Field Help ON" : "Field Help OFF");

    if (::IsWindow(m_toolBar.GetSafeHwnd()))
    {
        const int idx = m_toolBar.CommandToIndex(ID_TB_HELP_TOGGLE);
        if (idx >= 0)
        {
            if (CMFCToolBarButton* btn = m_toolBar.GetButton(idx))
            {
                btn->SetStyle(m_helpChecked
                              ? btn->m_nStyle | TBBS_CHECKED
                              : btn->m_nStyle & ~TBBS_CHECKED);
                m_toolBar.InvalidateButton(idx);
            }
        }
    }

    if (CMenu* pMenu = GetMenu())
    {
        pMenu->CheckMenuItem(ID_TB_HELP_TOGGLE,
                             MF_BYCOMMAND | (m_helpChecked ? MF_CHECKED : MF_UNCHECKED));
    }

    PropagateHelpToggle();
}

void CScenarioEditorDialog::OnUpdateHelpToggle(CCmdUI* pCmdUI)
{
    pCmdUI->SetCheck(m_helpChecked ? 1 : 0);
}

//
// PropagateHelpToggle — push the current help-active flag to all hosted pages.
//
void CScenarioEditorDialog::PropagateHelpToggle()
{
    for (CDialogEx* page : m_pages)
    {
        if (CHelpAwarePage* helpPage = static_cast<CHelpAwarePage*>(page))
            helpPage->SetHelpActive(m_helpChecked);
    }
}

//
// UpdateStatusPduCount — refresh the PDUs pane text from m_pduCount.
//
void CScenarioEditorDialog::UpdateStatusPduCount()
{
    if (!::IsWindow(m_statusBar.GetSafeHwnd()))
        return;
    TCHAR buf[32];
    _stprintf_s(buf, _T("PDUs: %u"), m_pduCount);
    m_statusBar.SetPaneText(6 /* ID_INDICATOR_PDUS pane index */, buf);
}

//
// OnPlaybackStart — capture UI into the scenario, build an immutable runtime
// snapshot (filling default ellipse-orbit speeds from airframe cruise), and
// hand it to the worker thread to begin sending. No-op if already running.
//
//
// OnNetworkTestMulticast — Network > Test Multicast Send. Sends exactly one
//   Entity State PDU to the configured multicast group so the operator can
//   confirm the group/port/TTL/interface settings actually put a packet on the
//   wire before starting a real run. Reports the outcome in a message box.
//
void CScenarioEditorDialog::OnNetworkTestMulticast()
{
    LOG("OnNetworkTestMulticast: entered");

    // Pull the Run-tab fields into m_scenario so we test what's on screen,
    // not what was last saved.
    CaptureUiIntoScenario();

    const OutputConfig& out = m_scenario.output;

    // Guard the single most common misconfiguration. Without this the send
    // still "succeeds" — UdpSender only applies the multicast socket options
    // for 224.0.0.0/4, so a unicast address here silently degrades to a plain
    // unicast datagram and the test would report success misleadingly.
    if (!IsIpv4MulticastGroup(out.multicastGroup))
    {
        CString msg;
        msg.Format(_T("'%s' is not a multicast group.\n\n")
                   _T("Multicast addresses must be in 224.0.0.0/4 ")
                   _T("(first octet 224-239). The default is 224.252.0.1.\n\n")
                   _T("Set the group on the Run tab under ")
                   _T("\"UDP Multicast Settings\"."),
                   CA2T(out.multicastGroup.c_str()).m_psz);
        MessageBox(msg, _T("Test Multicast Send"), MB_OK | MB_ICONWARNING);
        LOG("OnNetworkTestMulticast: rejected - group not in 224.0.0.0/4");
        return;
    }

    // Build one PDU. Use the scenario's first entity so the packet carries
    // realistic content; fall back to a default-constructed Entity when the
    // scenario is empty so the command still works on a fresh document.
    const Entity  fallback{};
    const Entity& entity = m_scenario.entities.empty() ? fallback
                                                       : m_scenario.entities.front();

    DIS::EntityStatePdu pdu = PduBuilder::BuildEntityStatePdu(m_scenario, entity);
    std::vector<unsigned char> bytes;
    const size_t pduLen = PduBuilder::Serialize(pdu, bytes);
    if (pduLen == 0 || bytes.empty())
    {
        MessageBox(_T("Failed to build the test PDU."),
                   _T("Test Multicast Send"), MB_OK | MB_ICONERROR);
        LOG("OnNetworkTestMulticast: PduBuilder::Serialize produced 0 bytes");
        return;
    }

    m_udp.SetMulticastOptions(out.multicastTtl, out.multicastInterface,
                              out.multicastLoopback);

    const int sent = m_udp.Send(out.multicastGroup, out.multicastPort,
                                bytes.data(), bytes.size());

    if (sent > 0)
    {
        CString msg;
        msg.Format(_T("Sent %d bytes to %s:%u\n\n")
                   _T("TTL: %d    Loopback: %s    Interface: %s\n\n")
                   _T("If the receiver saw nothing, check that it joined the ")
                   _T("same group and is listening on port %u, and that TTL is ")
                   _T("high enough to reach it (1 = this LAN segment only)."),
                   sent,
                   CA2T(out.multicastGroup.c_str()).m_psz,
                   static_cast<unsigned>(out.multicastPort),
                   out.multicastTtl,
                   out.multicastLoopback ? _T("on") : _T("off"),
                   CA2T(out.multicastInterface.c_str()).m_psz,
                   static_cast<unsigned>(out.multicastPort));
        MessageBox(msg, _T("Test Multicast Send"), MB_OK | MB_ICONINFORMATION);

        sprintf_s(szError, sizeof(szError),
                  "OnNetworkTestMulticast: sent %d B to %s:%u",
                  sent, out.multicastGroup.c_str(),
                  static_cast<unsigned>(out.multicastPort));
        LOG(szError);
    }
    else
    {
        const std::string& why = m_udp.LastError();
        CString msg;
        msg.Format(_T("Failed to send to %s:%u.\n\n%s"),
                   CA2T(out.multicastGroup.c_str()).m_psz,
                   static_cast<unsigned>(out.multicastPort),
                   why.empty() ? _T("(no detail available)")
                               : CA2T(why.c_str()).m_psz);
        MessageBox(msg, _T("Test Multicast Send"), MB_OK | MB_ICONERROR);
        LOG("OnNetworkTestMulticast: send failed");
    }
}

//
// OnNetworkTestTcp — Network > Test TCP Connection. Establishes the configured
//   TCP link (dialling out, or accepting one client) and sends a single Entity
//   State PDU, so the operator can confirm reachability before a run. Closes
//   the link again so it doesn't hold the port or the peer.
//
void CScenarioEditorDialog::OnNetworkTestTcp()
{
    LOG("OnNetworkTestTcp: entered");

    CaptureUiIntoScenario();
    const OutputConfig& out = m_scenario.output;

    m_tcp.Configure(out.tcpListen, out.tcpRemoteHost, out.tcpRemotePort,
                    out.tcpListenPort, out.tcpReconnect, out.tcpTimeoutMs);

    // In server mode this blocks up to the timeout waiting for a client, so
    // tell the user what's about to happen rather than freezing silently.
    if (out.tcpListen)
    {
        CString ask;
        ask.Format(_T("Listen on port %u for a client to connect?\n\n")
                   _T("This waits up to %d ms. Start DISBrowser (or another ")
                   _T("receiver) so it can connect during that window."),
                   static_cast<unsigned>(out.tcpListenPort), out.tcpTimeoutMs);
        if (MessageBox(ask, _T("Test TCP Connection"),
                       MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
            return;
    }

    if (!m_tcp.Connect())
    {
        CString msg;
        msg.Format(_T("Could not establish the TCP link to %s.\n\n%s"),
                   CA2T(m_tcp.DescribeEndpoint().c_str()).m_psz,
                   m_tcp.LastError().empty()
                       ? _T("(no detail available)")
                       : CA2T(m_tcp.LastError().c_str()).m_psz);
        MessageBox(msg, _T("Test TCP Connection"), MB_OK | MB_ICONERROR);
        LOG("OnNetworkTestTcp: connect failed");
        m_tcp.Close();
        return;
    }

    const Entity  fallback{};
    const Entity& entity = m_scenario.entities.empty() ? fallback
                                                       : m_scenario.entities.front();

    DIS::EntityStatePdu pdu = PduBuilder::BuildEntityStatePdu(m_scenario, entity);
    std::vector<unsigned char> bytes;
    const size_t pduLen = PduBuilder::Serialize(pdu, bytes);
    if (pduLen == 0 || bytes.empty())
    {
        MessageBox(_T("Failed to build the test PDU."),
                   _T("Test TCP Connection"), MB_OK | MB_ICONERROR);
        LOG("OnNetworkTestTcp: PduBuilder::Serialize produced 0 bytes");
        m_tcp.Close();
        return;
    }

    const int sent = m_tcp.Send(bytes.data(), bytes.size());

    if (sent > 0)
    {
        CString msg;
        msg.Format(_T("Connected to %s and sent %d bytes.\n\n")
                   _T("The stream carries raw DIS PDUs back to back - each PDU's ")
                   _T("header Length field delimits it, so no extra framing is used."),
                   CA2T(m_tcp.DescribeEndpoint().c_str()).m_psz, sent);
        MessageBox(msg, _T("Test TCP Connection"), MB_OK | MB_ICONINFORMATION);

        sprintf_s(szError, sizeof(szError),
                  "OnNetworkTestTcp: sent %d B over %s",
                  sent, m_tcp.DescribeEndpoint().c_str());
        LOG(szError);
    }
    else
    {
        CString msg;
        msg.Format(_T("Connected to %s but the send failed.\n\n%s"),
                   CA2T(m_tcp.DescribeEndpoint().c_str()).m_psz,
                   m_tcp.LastError().empty()
                       ? _T("(no detail available)")
                       : CA2T(m_tcp.LastError().c_str()).m_psz);
        MessageBox(msg, _T("Test TCP Connection"), MB_OK | MB_ICONERROR);
        LOG("OnNetworkTestTcp: send failed");
    }

    // Don't hold the port (server) or the peer's connection (client) open.
    m_tcp.Close();
}

void CScenarioEditorDialog::OnPlaybackStart()
{
    LOG("OnPlaybackStart: entered");
    if (m_worker.IsRunning())
    {
        LOG("Playback > Start ignored — worker already running");
        return;
    }

    // Pull the latest UI state into the editable scenario, then build the
    // immutable snapshot the worker will run from (§18.0).
    CaptureUiIntoScenario();

    sprintf_s(szError, sizeof(szError),
              "OnPlaybackStart: scenario name='%s' dur=%.1fs entities=%zu outputMode=%d",
              m_scenario.name.c_str(), m_scenario.durationSeconds,
              m_scenario.entities.size(),
              static_cast<int>(m_scenario.output.mode));
    LOG(szError);

    auto snap = std::make_shared<RuntimeScenarioSnapshot>();
    snap->scenario = m_scenario;          // deep copy (includes OutputConfig)

    // Resolve "speed defaults to airframe cruise" for ellipse orbits that have
    // no explicit Speed: playback needs a concrete speed to space the
    // equidistant waypoints. Mutate only the throwaway snapshot copy.
    {
        const EntityTypeCatalog& cat = theApp.Catalog();
        for (Entity& e : snap->scenario.entities)
        {
            for (MotionSegment& seg : e.motionSegments)
            {
                if (seg.type != MotionType::Ellipse || seg.speedMps > 0.0) continue;
                const AirframeProfile prof =
                    cat.Profile(e.kind, e.domain, e.category, e.subcategory);
                if (prof.valid && prof.cruiseSpeedMps > 0.0)
                    seg.speedMps = prof.cruiseSpeedMps;
            }
        }
    }

    m_pduCount = 0;
    UpdateStatusPduCount();

    if (!m_worker.Start(GetSafeHwnd(), std::move(snap)))
    {
        LOG("OnPlaybackStart: worker.Start returned false");
        if (::IsWindow(m_statusBar.GetSafeHwnd()))
            m_statusBar.SetPaneText(0, _T("State: Error"));
        return;
    }

    LOG("Playback > Start: worker spawned, heartbeat begins");
}

//
// OnPlaybackPause — pause the worker.
//
void CScenarioEditorDialog::OnPlaybackPause()
{
    LOG("Playback > Pause");
    m_worker.Pause();
}

//
// OnPlaybackResume — resume the worker.
//
void CScenarioEditorDialog::OnPlaybackResume()
{
    LOG("Playback > Resume");
    m_worker.Resume();
}

//
// OnPlaybackStop — request stop and join the worker synchronously.
//
void CScenarioEditorDialog::OnPlaybackStop()
{
    LOG("Playback > Stop");
    m_worker.RequestStop();
    m_worker.Join();   // synchronous stop — the user clicked Stop
}

//
// OnPlaybackStatusMessage — worker-posted state change: map PlaybackState
// (wParam) to a label and show it in the State status-bar pane.
//
LRESULT CScenarioEditorDialog::OnPlaybackStatusMessage(WPARAM wParam, LPARAM /*lParam*/)
{
    const PlaybackState state = static_cast<PlaybackState>(wParam);
    const TCHAR* label = _T("State: Idle");
    switch (state)
    {
        case PlaybackState::Running:   label = _T("State: Running");   break;
        case PlaybackState::Paused:    label = _T("State: Paused");    break;
        case PlaybackState::Stopping:  label = _T("State: Stopping");  break;
        case PlaybackState::Stopped:   label = _T("State: Stopped");   break;
        case PlaybackState::Completed: label = _T("State: Completed"); break;
        case PlaybackState::Error:     label = _T("State: Error");     break;
        case PlaybackState::Idle:
        default:                       label = _T("State: Idle");      break;
    }
    if (::IsWindow(m_statusBar.GetSafeHwnd()))
        m_statusBar.SetPaneText(0 /* ID_INDICATOR_STATE pane */, label);
    return 0;
}

//
// OnPlaybackProgressMessage — worker-posted progress: update the PDU count and
// the Time / Progress panes from elapsed ms (wParam) and total PDUs (lParam).
//
LRESULT CScenarioEditorDialog::OnPlaybackProgressMessage(WPARAM timeMs, LPARAM totalPdus)
{
    m_pduCount = static_cast<unsigned>(totalPdus);
    UpdateStatusPduCount();

    // Time + Progress % panes (panes 1 and 2). wParam is the scenario-time
    // elapsed in ms; % is computed against the scenario's nominal duration.
    if (::IsWindow(m_statusBar.GetSafeHwnd()))
    {
        const double tSec = static_cast<double>(timeMs) / 1000.0;
        const double dur  = m_scenario.durationSeconds;
        CString buf;
        if (dur > 0.0)
            buf.Format(_T("Time: %.1fs / %.0fs"), tSec, dur);
        else
            buf.Format(_T("Time: %.1fs"), tSec);
        m_statusBar.SetPaneText(1, buf);

        if (dur > 0.0)
        {
            int pct = static_cast<int>(tSec / dur * 100.0 + 0.5);
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;
            buf.Format(_T("Progress: %d%%"), pct);
            m_statusBar.SetPaneText(2, buf);
        }
    }
    return 0;
}

//
// OnPlaybackErrorMessage — worker-posted error: log the code (wParam) and set
// the State pane to "Error".
//
LRESULT CScenarioEditorDialog::OnPlaybackErrorMessage(WPARAM code, LPARAM /*lParam*/)
{
    sprintf_s(szError, sizeof(szError), "WM_APP_PLAYBACK_ERROR code=%u", static_cast<unsigned>(code));
    LOG(szError);
    if (::IsWindow(m_statusBar.GetSafeHwnd()))
        m_statusBar.SetPaneText(0, _T("State: Error"));
    return 0;
}

//
// OnDestroy — persist window placement and last-scenario path to settings,
// then stop and join the worker before the HWND is destroyed.
//
void CScenarioEditorDialog::OnDestroy()
{
    // Snapshot window placement before we go away (§11.5). Use
    // GetWindowPlacement so the *restored* size — not the current
    // maximized rect — is what we persist.
    {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        if (::GetWindowPlacement(GetSafeHwnd(), &wp))
        {
            Settings& cfg = theApp.Settings();
            cfg.windowMaximized = (wp.showCmd == SW_SHOWMAXIMIZED);
            cfg.windowX      = wp.rcNormalPosition.left;
            cfg.windowY      = wp.rcNormalPosition.top;
            cfg.windowWidth  = wp.rcNormalPosition.right  - wp.rcNormalPosition.left;
            cfg.windowHeight = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        }
        if (!m_currentScenarioPath.IsEmpty())
        {
            CT2A ascii(m_currentScenarioPath);
            theApp.Settings().lastScenarioPath = ascii.m_psz;
        }
        SettingsIO::Save(theApp.Settings(), theApp.SettingsPath());
    }

    // Make sure the worker is stopped before the dialog HWND goes away,
    // otherwise PostMessage from the worker would target a dead window.
    m_worker.RequestStop();
    m_worker.Join();
    CDialogEx::OnDestroy();
}

// ---------- File menu ----------

//
// UpdateTitle — set the window title from the open file name (or the in-INI
// name, or "Untitled"), appending " *" when there are unsaved changes.
//
void CScenarioEditorDialog::UpdateTitle()
{
    // Title bar shows the FILE NAME (what the user clicked Open on) as
    // the primary identifier — the in-INI Name= field is independent
    // and edited via Scenario Setup. If no file is open yet, fall back
    // to the in-INI name (or "Untitled" if both are empty).
    CString display;
    if (!m_currentScenarioPath.IsEmpty())
    {
        const int slash = m_currentScenarioPath.ReverseFind(_T('\\'));
        const int fwd   = m_currentScenarioPath.ReverseFind(_T('/'));
        const int cut   = (slash > fwd) ? slash : fwd;
        display = (cut >= 0)
            ? m_currentScenarioPath.Mid(cut + 1)
            : m_currentScenarioPath;
    }
    else if (!m_scenario.name.empty())
    {
        display = CA2T(m_scenario.name.c_str());
    }
    else
    {
        display = _T("Untitled");
    }

    CString title;
    title.Format(_T("ScenarioEditor - %s"), display.GetString());
    if (m_isDirty) title += _T(" *");
    SetWindowText(title);
}

//
// MarkDirty — flag the scenario modified (unless suppressed) and refresh the
// title. No-op if already dirty.
//
void CScenarioEditorDialog::MarkDirty()
{
    if (m_suppressDirty || m_isDirty) return;
    m_isDirty = true;
    UpdateTitle();
}

//
// ClearDirty — clear the modified flag and refresh the title. No-op if clean.
//
void CScenarioEditorDialog::ClearDirty()
{
    if (!m_isDirty) return;
    m_isDirty = false;
    UpdateTitle();
}


//
// OnMarkDirtyMessage — WM_APP_MARK_DIRTY handler; lets a page request a
// dirty flag via PostMessage.
//
LRESULT CScenarioEditorDialog::OnMarkDirtyMessage(WPARAM /*w*/, LPARAM /*l*/)
{
    MarkDirty();
    return 0;
}

// A child page (today: the Preview tab's drag-to-set-start) mutated the shared
// Scenario directly. Reload every page's controls from the model so the other
// tabs reflect it, then flag the scenario modified. RefreshUiFromScenario gates
// the dirty handler off internally, so we MarkDirty afterwards.
LRESULT CScenarioEditorDialog::OnRefreshUiMessage(WPARAM /*w*/, LPARAM /*l*/)
{
    RefreshUiFromScenario();
    MarkDirty();
    return 0;
}

//
// MaybePromptSaveOnDiscard — if dirty, ask Save/Discard/Cancel. Returns true
// if the caller may proceed (saved or discarded), false on Cancel or a
// cancelled Save-As.
//
bool CScenarioEditorDialog::MaybePromptSaveOnDiscard()
{
    if (!m_isDirty) return true;
    const int rc = AfxMessageBox(
        _T("The current scenario has unsaved changes. Save before continuing?"),
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (rc == IDCANCEL) return false;
    if (rc == IDYES)
    {
        OnFileSave();
        // OnFileSave may have been cancelled in Save-As (no path picked); in
        // that case m_isDirty is still set — treat as Cancel.
        if (m_isDirty) return false;
    }
    return true;
}

//
// RefreshUiFromScenario — repopulate the Output page from the model (with
// dirty tracking suppressed), update the title, and revalidate. Other pages
// live in the Attributes notebook and refresh themselves when opened.
//
void CScenarioEditorDialog::RefreshUiFromScenario()
{
    // ReadFrom() helpers SetWindowText on every edit control, which fires
    // EN_CHANGE — gate the dirty handler off so a fresh load doesn't
    // immediately flip us to "modified".
    // The Setup / Asset / Motion pages live in the modal Attributes notebook
    // and refresh themselves from the shared scenario when opened; here we
    // only refresh the Output page (Preview reads the model live on paint).
    m_suppressDirty = true;
    m_pageOutput.ReadFrom(m_scenario.output);
    m_suppressDirty = false;
    UpdateTitle();
    Revalidate();
}

//
// CaptureUiIntoScenario — pull the Output page's controls plus the toolbar
// speed combo and loop toggle into the model, then revalidate.
//
void CScenarioEditorDialog::CaptureUiIntoScenario()
{
    // The Setup / Asset / Motion pages flush their own edits into the shared
    // scenario when the modal Attributes notebook closes, so here we only pull
    // the Output tab's controls.
    m_pageOutput.WriteTo(m_scenario.output);

    // Toolbar speed combo overrides the Output-tab speed if a value is
    // selected. The CMFCToolBarComboBoxButton lives on m_toolBar; its
    // current text is "0.25x" / "0.5x" / "1x" / "2x" / "10x".
    if (CMFCToolBarComboBoxButton* btn =
        DYNAMIC_DOWNCAST(CMFCToolBarComboBoxButton, m_toolBar.GetButton(
            m_toolBar.CommandToIndex(ID_TB_SPEED_HOST))))
    {
        const int idx = btn->GetCurSel();
        const double speeds[] = { 0.25, 0.5, 1.0, 2.0, 10.0 };
        if (idx >= 0 && idx < 5)
            m_scenario.output.playbackSpeed = speeds[idx];
    }
    m_scenario.output.loopEnabled = m_loopChecked;
    Revalidate();
}

//
// Revalidate — run the validator + bandwidth estimate and push the
// error/warning/info/PDU/BW counts into the status-bar panes.
//
void CScenarioEditorDialog::Revalidate()
{
    // §20: run rule set + bandwidth estimate; push counts into the status bar
    // panes seeded by SeedStatusBar(). Status bar may not exist yet during
    // very early init paths (e.g. RefreshUiFromScenario called from auto-load
    // before SeedStatusBar()), so guard on m_statusBar.GetSafeHwnd().
    if (!m_statusBar.GetSafeHwnd())
        return;

    const Validator::Report rep = Validator::Validate(m_scenario, theApp.Catalog());

    CString buf;
    buf.Format(_T("Errors: %d"),   rep.errorCount);   m_statusBar.SetPaneText(3, buf);
    buf.Format(_T("Warnings: %d"), rep.warningCount); m_statusBar.SetPaneText(4, buf);
    buf.Format(_T("Info: %d"),     rep.infoCount);    m_statusBar.SetPaneText(5, buf);
    buf.Format(_T("PDUs: %llu"),   static_cast<unsigned long long>(rep.pduCount));
                                                       m_statusBar.SetPaneText(6, buf);
    buf.Format(_T("BW: %.2f Mbps"), rep.mbpsAvg);     m_statusBar.SetPaneText(7, buf);
}

// The asset/segment editing controls moved to the Attributes notebook, which
// owns an accelerator table that fires these same shortcuts against its own
// pages. From the main window the shortcuts just open the notebook on the
// relevant tab (1 = Asset/Entity Editor, 2 = Motion Path Editor).
void CScenarioEditorDialog::OnKbAddAsset()        { OpenAttributes(1); }
void CScenarioEditorDialog::OnKbDeleteAsset()     { OpenAttributes(1); }
void CScenarioEditorDialog::OnKbDuplicateAsset()  { OpenAttributes(1); }
void CScenarioEditorDialog::OnKbMoveAsset()       { OpenAttributes(1); }
void CScenarioEditorDialog::OnKbAddSegment()      { OpenAttributes(2); }
void CScenarioEditorDialog::OnKbDeleteSegment()   { OpenAttributes(2); }
void CScenarioEditorDialog::OnKbDuplicateSegment(){ OpenAttributes(2); }

//
// OnScenarioValidate — capture the UI, run the validator, and present the
// full issue list (errors, then warnings, then info) in a message box.
//
void CScenarioEditorDialog::OnScenarioValidate()
{
    CaptureUiIntoScenario();
    const Validator::Report rep = Validator::Validate(m_scenario, theApp.Catalog());

    // Build a single multi-line message. Errors first, then warnings, then
    // info — easiest path that satisfies "pop a list dialog with the issues"
    // without a dedicated CDialog template.
    CString body;
    body.Format(_T("Errors: %d   Warnings: %d   Info: %d\nPDUs: %llu   Avg BW: %.2f Mbps\n\n"),
                rep.errorCount, rep.warningCount, rep.infoCount,
                static_cast<unsigned long long>(rep.pduCount), rep.mbpsAvg);

    auto sevLabel = [](Validator::Severity s) -> const TCHAR* {
        switch (s) {
            case Validator::Severity::Error:   return _T("ERROR");
            case Validator::Severity::Warning: return _T("WARN ");
            default:                           return _T("INFO ");
        }
    };

    if (rep.issues.empty())
    {
        body += _T("(No issues found.)");
    }
    else
    {
        for (const auto& i : rep.issues)
        {
            CString line;
            line.Format(_T("[%s] %s — %s\n"),
                        sevLabel(i.severity),
                        CString(CA2T(i.subject.c_str())).GetString(),
                        CString(CA2T(i.message.c_str())).GetString());
            body += line;
        }
    }

    AfxMessageBox(body, MB_OK | (rep.errorCount > 0 ? MB_ICONWARNING : MB_ICONINFORMATION));
}

//
// DoSaveTo — capture the UI and write the scenario to iniPath; on success
// record it as the current path and clear dirty. Returns false on I/O error.
//
//
// SetCurrentScenarioPath — record the open scenario's path and mirror it onto
// the Run tab, so "Configure Unreal" can derive this scenario's camera schedule
// (<scenario>-camera.ini) and write its path into DISBrowser's Startup.ini.
//
void CScenarioEditorDialog::SetCurrentScenarioPath(const CString& path)
{
    m_currentScenarioPath = path;
    m_pageOutput.SetScenarioPath(path);
}

bool CScenarioEditorDialog::DoSaveTo(const CString& iniPath)
{
    CaptureUiIntoScenario();
    // Saving commits the (until-now provisional) origin: whatever place the operator
    // navigated to becomes this scenario's real origin.
    m_scenario.originSet = true;
    const ScenarioIO::Result rc =
        ScenarioIO::Save(m_scenario, std::wstring(CT2W(iniPath)));
    if (rc != ScenarioIO::Result::Ok)
    {
        AfxMessageBox(_T("Failed to save scenario.ini. See error.log for details."),
                      MB_OK | MB_ICONERROR);
        return false;
    }
    // The camera schedule is now saved inside scenario.ini (ScenarioIO::Save writes
    // the [Camera.N] sections). Delete any legacy "<scenario>-camera.ini" side-file
    // so the two-file layout can't resurface and drift out of sync.
    ::DeleteFileW(std::wstring(CT2W(CameraPathFor(iniPath))).c_str());
    SetCurrentScenarioPath(iniPath);
    ClearDirty();
    UpdateTitle();
    return true;
}

//
// OnFileNew — File > New: prompt to save if dirty, then reset to a blank
// scenario and refresh the UI.
//
void CScenarioEditorDialog::OnFileNew()
{
    LOG("File > New Scenario");
    if (!MaybePromptSaveOnDiscard()) return;
    m_scenario = Scenario{};
    SetCurrentScenarioPath(CString());
    // The asset tree lives in the Attributes notebook now and rebuilds from
    // the model each time it opens.
    RefreshUiFromScenario();
    ClearDirty();
}

//
// OnFileOpen — File > Open: prompt to save if dirty, pick an .ini, load it,
// and refresh the UI (reporting any load error via message box).
//
void CScenarioEditorDialog::OnFileOpen()
{
    LOG("File > Open Scenario");
    if (!MaybePromptSaveOnDiscard()) return;
    CFileDialog dlg(/*open*/TRUE,
                    _T("ini"),
                    _T("scenario.ini"),
                    OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST,
                    _T("Scenario INI (*.ini)|*.ini|All Files (*.*)|*.*||"),
                    this);
    if (dlg.DoModal() != IDOK) return;

    const CString path = dlg.GetPathName();
    Scenario loaded;
    const ScenarioIO::Result rc =
        ScenarioIO::Load(loaded, std::wstring(CT2W(path)));

    switch (rc)
    {
        case ScenarioIO::Result::Ok:
            MigrateLegacyCameras(loaded, path);
            m_scenario = std::move(loaded);
            SetCurrentScenarioPath(path);
            RefreshUiFromScenario();
            ClearDirty();
            sprintf_s(szError, sizeof(szError),
                      "Loaded scenario from %S", static_cast<const wchar_t*>(CT2W(path)));
            LOG(szError);
            WarnIfOriginDisconnected(m_scenario, LeafName(path));
            break;
        case ScenarioIO::Result::VersionTooNew:
            AfxMessageBox(_T("This scenario.ini was written by a newer version of ScenarioEditor."),
                          MB_OK | MB_ICONWARNING);
            break;
        case ScenarioIO::Result::Malformed:
            AfxMessageBox(_T("scenario.ini is missing or malformed. See error.log."),
                          MB_OK | MB_ICONERROR);
            break;
        case ScenarioIO::Result::IoError:
        default:
            AfxMessageBox(_T("Could not open scenario.ini. See error.log."),
                          MB_OK | MB_ICONERROR);
            break;
    }
}

//
// OnFileSave — File > Save: save to the current path, or fall through to
// Save As if none is set yet.
//
void CScenarioEditorDialog::OnFileSave()
{
    LOG("File > Save Scenario");
    if (m_currentScenarioPath.IsEmpty())
    {
        OnFileSaveAs();
        return;
    }
    DoSaveTo(m_currentScenarioPath);
}

//
// OnReplayFile — Tools > Replay File: capture the UI, resolve the recording
// path (prompting if unset), and start the worker in replay mode.
//
void CScenarioEditorDialog::OnReplayFile()
{
    LOG("Tools > Replay File");
    if (m_worker.IsRunning())
    {
        LOG("Replay refused — worker already running");
        return;
    }
    CaptureUiIntoScenario();

    // Use the Output tab's configured replay path. If none, prompt now.
    if (m_scenario.output.replayPath.empty())
    {
        CFileDialog dlg(/*open*/TRUE, _T("disrec"), nullptr,
                        OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
                        _T("DIS Recording (*.disrec)|*.disrec|All Files (*.*)|*.*||"),
                        this);
        if (dlg.DoModal() != IDOK) return;
        CT2A ascii(dlg.GetPathName());
        m_scenario.output.replayPath = ascii.m_psz;
    }

    auto snap = std::make_shared<RuntimeScenarioSnapshot>();
    snap->scenario = m_scenario;
    snap->replay   = true;

    m_pduCount = 0;
    UpdateStatusPduCount();
    m_worker.Start(GetSafeHwnd(), std::move(snap));
}

//
// OnOpenRecording — File > Open Recording: pick a .disrec, store it as the
// Output tab's replay path, and refresh that page.
//
void CScenarioEditorDialog::OnOpenRecording()
{
    LOG("File > Open Recording");
    CFileDialog dlg(/*open*/TRUE, _T("disrec"), nullptr,
                    OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
                    _T("DIS Recording (*.disrec)|*.disrec|All Files (*.*)|*.*||"),
                    this);
    if (dlg.DoModal() != IDOK) return;
    CT2A ascii(dlg.GetPathName());
    m_scenario.output.replayPath = ascii.m_psz;
    m_pageOutput.ReadFrom(m_scenario.output);
}

//
// OnFileSaveAs — File > Save As: prompt for a target .ini and save to it.
//
void CScenarioEditorDialog::OnFileSaveAs()
{
    LOG("File > Save Scenario As");
    CFileDialog dlg(/*open*/FALSE,
                    _T("ini"),
                    _T("scenario.ini"),
                    OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
                    _T("Scenario INI (*.ini)|*.ini|All Files (*.*)|*.*||"),
                    this);
    if (dlg.DoModal() != IDOK) return;
    DoSaveTo(dlg.GetPathName());
}

