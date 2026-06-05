#include "pch.h"
#include "ScenarioEditorDialog.h"
#include "PduBuilder.h"
#include "ScenarioEditor.h"   // for theApp.Catalog()/Settings()
#include "ScenarioIO.h"
#include "SettingsIO.h"
#include "Validator.h"
#include "../log.h"
#include <stdio.h>
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
    ON_COMMAND_RANGE(ID_SCENARIO_GENERATE_RECORDING, ID_SCENARIO_RESET_CLOCK, &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND_RANGE(ID_PLAYBACK_LOOP,        ID_PLAYBACK_SPEED_10,      &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND_RANGE(ID_NETWORK_OPEN_OUTPUT,  ID_NETWORK_TEST_TCP,       &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND_RANGE(ID_VIEW_VALIDATION_PANEL, ID_VIEW_VALIDATION_PANEL, &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND_RANGE(ID_HELP_PROTOCOL_NOTES,  ID_HELP_FILE_FORMAT_NOTES, &CScenarioEditorDialog::OnPlaceholderCommand)
    ON_COMMAND(ID_TOOLS_RECORD,          &CScenarioEditorDialog::OnPlaybackStart)
    ON_COMMAND_RANGE(ID_TOOLS_SEND_LIVE,       ID_TOOLS_SEND_LIVE,        &CScenarioEditorDialog::OnPlaceholderCommand)
    // Speed combo is captured into m_scenario.output.playbackSpeed at
    // CaptureUiIntoScenario time; no per-change handler needed.
END_MESSAGE_MAP()

BOOL CScenarioEditorDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // Gate dirty tracking until the dialog finishes initial population. The
    // tab pages' first ReadFrom() calls fire EN_CHANGE on every edit.
    m_suppressDirty = true;

    m_hAccel = ::LoadAccelerators(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_MAIN_ACCEL));

    // Tab control labels (§5.2)
    m_tabCtrl.InsertItem(0, _T("Scenario Setup"));
    m_tabCtrl.InsertItem(1, _T("Asset / Entity Editor"));
    m_tabCtrl.InsertItem(2, _T("Motion Path Editor"));
    m_tabCtrl.InsertItem(3, _T("Output / Playback"));
    m_tabCtrl.InsertItem(4, _T("Preview"));

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

    // Apply persisted window state, clamped to the primary monitor's work
    // area so a saved size larger than the screen doesn't leave the bottom
    // of the dialog under the taskbar.
    {
        const Settings& cfg = theApp.Settings();
        if (cfg.windowX >= 0 && cfg.windowY >= 0 &&
            cfg.windowWidth >= 320 && cfg.windowHeight >= 240)
        {
            const int wa_w = ::GetSystemMetrics(SM_CXFULLSCREEN);
            const int wa_h = ::GetSystemMetrics(SM_CYFULLSCREEN);
            int x = cfg.windowX, y = cfg.windowY;
            int w = cfg.windowWidth, h = cfg.windowHeight;
            if (wa_w > 0 && wa_h > 0)
            {
                if (w > wa_w) w = wa_w;
                if (h > wa_h) h = wa_h;
                if (x + w > wa_w) x = wa_w - w;
                if (y + h > wa_h) y = wa_h - h;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
            }
            ::SetWindowPos(GetSafeHwnd(), nullptr, x, y, w, h,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            if (cfg.windowMaximized)
                ShowWindow(SW_SHOWMAXIMIZED);
        }
    }

    // Auto-load last opened scenario if the path still exists. Either way,
    // RefreshUiFromScenario() runs below so the asset tree / motion list
    // are populated from the default Scenario on a fresh launch.
    if (!theApp.Settings().lastScenarioPath.empty())
    {
        const std::wstring lastPath(CA2W(theApp.Settings().lastScenarioPath.c_str()));
        if (::GetFileAttributesW(lastPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            Scenario loaded;
            if (ScenarioIO::Load(loaded, lastPath) == ScenarioIO::Result::Ok)
            {
                m_scenario = std::move(loaded);
                m_currentScenarioPath = CString(lastPath.c_str());
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

void CScenarioEditorDialog::ApplyStatusBarPaneWidths()
{
    if (!::IsWindow(m_statusBar.GetSafeHwnd())) return;
    for (const auto& p : kStatusBarPanes)
        m_statusBar.SetPaneInfo(p.idx, p.id, SBPS_NORMAL, p.widthPx);
}

void CScenarioEditorDialog::SeedStatusBar()
{
    if (!::IsWindow(m_statusBar.GetSafeHwnd())) return;
    ApplyStatusBarPaneWidths();
    for (const auto& p : kStatusBarPanes)
        m_statusBar.SetPaneText(p.idx, p.seedText);
}

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

    // Hand the Entity Type catalog (§7.5) to the Asset page before
    // Create() so its OnInitDialog can populate combos in one pass.
    m_pageAssets.SetCatalog(&theApp.Catalog());

    // Setup page needs the editable scenario so the Default Coord Mode
    // combo can live-commit (other pages read defaultCoordMode when
    // they add new entities / segments).
    m_pageSetup.SetScenario(&m_scenario);

    // Asset page needs the editable scenario to drive the asset tree.
    m_pageAssets.SetScenario(&m_scenario);

    // Motion page reads the same scenario for the segment list view.
    m_pageMotion.SetScenario(&m_scenario);

    // Output page edits Scenario::output.
    m_pageOutput.SetScenario(&m_scenario);

    // Preview page renders the scenario independently of the worker.
    m_pagePreview.SetScenario(&m_scenario);

    m_pages[0] = create(m_pageSetup,   IDD_SCENARIO_SETUP_PAGE);
    m_pages[1] = create(m_pageAssets,  IDD_ASSET_ENTITY_EDITOR_PAGE);
    m_pages[2] = create(m_pageMotion,  IDD_MOTION_PATH_EDITOR_PAGE);
    m_pages[3] = create(m_pageOutput,  IDD_OUTPUT_PLAYBACK_PAGE);
    m_pages[4] = create(m_pagePreview, IDD_PREVIEW_PAGE);
}

void CScenarioEditorDialog::ShowPage(int index)
{
    if (index < 0 || index >= 5) return;
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

void CScenarioEditorDialog::OnTabSelChange(NMHDR*, LRESULT* pResult)
{
    ShowPage(m_tabCtrl.GetCurSel());
    if (pResult) *pResult = 0;
}

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

void CScenarioEditorDialog::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    // Don't forward WM_SIZE to the status bar — MFC's CControlBar handler
    // would re-position the bar to its internal default height (computed
    // from the original font), clobbering the 40-px slot we want for the
    // 12pt pane text. LayoutChildren below positions the bar explicitly.
    LayoutChildren();
}

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

void CScenarioEditorDialog::OnFileExit()
{
    LOG("File > Exit");
    if (!MaybePromptSaveOnDiscard()) return;
    EndDialog(IDOK);
}

void CScenarioEditorDialog::OnCancel()
{
    if (!MaybePromptSaveOnDiscard()) return;
    CDialogEx::OnCancel();
}

void CScenarioEditorDialog::OnClose()
{
    if (!MaybePromptSaveOnDiscard()) return;
    CDialogEx::OnCancel();
}

void CScenarioEditorDialog::OnHelpAbout()
{
    LOG("Help > About");
    AfxMessageBox(_T("ScenarioEditor V1\nUI shell only (spec section 23.1).\n\nNo scenario logic wired."),
                  MB_OK | MB_ICONINFORMATION);
}

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

void CScenarioEditorDialog::PropagateHelpToggle()
{
    for (CDialogEx* page : m_pages)
    {
        if (CHelpAwarePage* helpPage = static_cast<CHelpAwarePage*>(page))
            helpPage->SetHelpActive(m_helpChecked);
    }
}

void CScenarioEditorDialog::UpdateStatusPduCount()
{
    if (!::IsWindow(m_statusBar.GetSafeHwnd()))
        return;
    TCHAR buf[32];
    _stprintf_s(buf, _T("PDUs: %u"), m_pduCount);
    m_statusBar.SetPaneText(6 /* ID_INDICATOR_PDUS pane index */, buf);
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

void CScenarioEditorDialog::OnPlaybackPause()
{
    LOG("Playback > Pause");
    m_worker.Pause();
}

void CScenarioEditorDialog::OnPlaybackResume()
{
    LOG("Playback > Resume");
    m_worker.Resume();
}

void CScenarioEditorDialog::OnPlaybackStop()
{
    LOG("Playback > Stop");
    m_worker.RequestStop();
    m_worker.Join();   // synchronous stop — the user clicked Stop
}

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

LRESULT CScenarioEditorDialog::OnPlaybackErrorMessage(WPARAM code, LPARAM /*lParam*/)
{
    sprintf_s(szError, sizeof(szError), "WM_APP_PLAYBACK_ERROR code=%u", static_cast<unsigned>(code));
    LOG(szError);
    if (::IsWindow(m_statusBar.GetSafeHwnd()))
        m_statusBar.SetPaneText(0, _T("State: Error"));
    return 0;
}

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

void CScenarioEditorDialog::MarkDirty()
{
    if (m_suppressDirty || m_isDirty) return;
    m_isDirty = true;
    UpdateTitle();
}

void CScenarioEditorDialog::ClearDirty()
{
    if (!m_isDirty) return;
    m_isDirty = false;
    UpdateTitle();
}


LRESULT CScenarioEditorDialog::OnMarkDirtyMessage(WPARAM /*w*/, LPARAM /*l*/)
{
    MarkDirty();
    return 0;
}

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

void CScenarioEditorDialog::RefreshUiFromScenario()
{
    // ReadFrom() helpers SetWindowText on every edit control, which fires
    // EN_CHANGE — gate the dirty handler off so a fresh load doesn't
    // immediately flip us to "modified".
    m_suppressDirty = true;
    m_pageSetup.ReadFrom(m_scenario);
    m_pageAssets.RefreshAssetTree();
    if (!m_scenario.entities.empty())
    {
        const int idx = m_pageAssets.SelectedIndex();
        const size_t ix = (idx >= 0 && idx < (int)m_scenario.entities.size())
                          ? static_cast<size_t>(idx) : 0;
        m_pageAssets.ReadFrom(m_scenario.entities[ix]);
    }
    m_pageMotion.Refresh();
    m_pageOutput.ReadFrom(m_scenario.output);
    m_suppressDirty = false;
    UpdateTitle();
    Revalidate();
}

void CScenarioEditorDialog::CaptureUiIntoScenario()
{
    m_pageSetup.WriteTo(m_scenario);
    const int idx = m_pageAssets.SelectedIndex();
    if (idx >= 0 && idx < (int)m_scenario.entities.size())
    {
        m_pageAssets.WriteTo(m_scenario.entities[idx],
                             m_scenario.originLatDeg,
                             m_scenario.originLonDeg,
                             m_scenario.originAltM);
    }
    m_pageOutput.WriteTo(m_scenario.output);
    m_pageMotion.CommitPendingEdits();

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

namespace
{
    // Synthesize a BN_CLICKED to a specific control on a child page. We use
    // this for keyboard shortcuts that drive the same action handlers the
    // mouse would normally fire.
    void FireBnClicked(CWnd* page, UINT controlId)
    {
        if (!page || !::IsWindow(page->GetSafeHwnd())) return;
        if (CWnd* ctrl = page->GetDlgItem(controlId))
            page->SendMessage(WM_COMMAND,
                              MAKEWPARAM(controlId, BN_CLICKED),
                              reinterpret_cast<LPARAM>(ctrl->GetSafeHwnd()));
    }
}

void CScenarioEditorDialog::OnKbAddAsset()        { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_ADD_ASSET); }
void CScenarioEditorDialog::OnKbDeleteAsset()     { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_DELETE_ASSET); }
void CScenarioEditorDialog::OnKbDuplicateAsset()  { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_DUPLICATE_ASSET); }
void CScenarioEditorDialog::OnKbMoveAsset()       { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_MOVE_ASSET); }
void CScenarioEditorDialog::OnKbAddSegment()      { ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_ADD_SEGMENT); }
void CScenarioEditorDialog::OnKbDeleteSegment()   { ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_DELETE_SEGMENT); }
void CScenarioEditorDialog::OnKbDuplicateSegment(){ ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_DUPLICATE_SEGMENT); }

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

bool CScenarioEditorDialog::DoSaveTo(const CString& iniPath)
{
    CaptureUiIntoScenario();
    const ScenarioIO::Result rc =
        ScenarioIO::Save(m_scenario, std::wstring(CT2W(iniPath)));
    if (rc != ScenarioIO::Result::Ok)
    {
        AfxMessageBox(_T("Failed to save scenario.ini. See error.log for details."),
                      MB_OK | MB_ICONERROR);
        return false;
    }
    m_currentScenarioPath = iniPath;
    ClearDirty();
    UpdateTitle();
    return true;
}

void CScenarioEditorDialog::OnFileNew()
{
    LOG("File > New Scenario");
    if (!MaybePromptSaveOnDiscard()) return;
    m_scenario = Scenario{};
    m_currentScenarioPath.Empty();
    m_pageAssets.RefreshAssetTree();
    RefreshUiFromScenario();
    ClearDirty();
}

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
            m_scenario = std::move(loaded);
            m_currentScenarioPath = path;
            RefreshUiFromScenario();
            ClearDirty();
            sprintf_s(szError, sizeof(szError),
                      "Loaded scenario from %S", static_cast<const wchar_t*>(CT2W(path)));
            LOG(szError);
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

