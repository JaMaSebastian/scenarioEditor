//=============================================================================
//  AttributesDialog.cpp
//-----------------------------------------------------------------------------
//  Implements CAttributesDialog: hosts the Scenario Setup, Asset / Entity
//  Editor, and Motion Path Editor pages in a tab control, laying them out over
//  the tab's content area. Populates them from the shared scenario, flushes
//  edits on close, tracks the dirty state, and routes keyboard shortcuts.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-10
//=============================================================================
#include "pch.h"
#include "AttributesDialog.h"
#include "Scenario.h"
#include "ScenarioWorker.h"   // WM_APP_MARK_DIRTY
#include "ScrollablePage.h"   // FitWindowToMonitorWorkArea()

namespace
{
    // Gap between the Close button and the dialog's bottom/right edges, and the
    // same gap again between it and the tab control above.
    constexpr int kBottomMarginPx = 6;

    // Fallback reserve, used only if the Close button can't be measured; the
    // real reserve is derived from the button's height in LayoutChildren().
    constexpr int kBottomReservePx = 28;

    // Synthesize a BN_CLICKED to a specific control on a hosted page — the
    // same trick the main dialog uses so keyboard shortcuts drive the button
    // handlers directly (see CScenarioEditorDialog::FireBnClicked).
    void FireBnClicked(CWnd* page, UINT controlId)
    {
        if (!page || !::IsWindow(page->GetSafeHwnd())) return;
        if (CWnd* ctrl = page->GetDlgItem(controlId))
            page->SendMessage(WM_COMMAND,
                              MAKEWPARAM(controlId, BN_CLICKED),
                              reinterpret_cast<LPARAM>(ctrl->GetSafeHwnd()));
    }
}

//
// CAttributesDialog — cache the scenario, catalog, help state, and start tab
// for use during OnInitDialog.
//
CAttributesDialog::CAttributesDialog(Scenario* scenario,
                                     const EntityTypeCatalog* catalog,
                                     bool helpActive,
                                     int  startTab,
                                     CWnd* pParent)
    : CDialogEx(IDD, pParent)
    , m_scenario(scenario)
    , m_catalog(catalog)
    , m_helpActive(helpActive)
    , m_startTab(startTab)
{
}

//
// DoDataExchange — bind the tab control member to IDC_ATTR_TABCTRL.
//
void CAttributesDialog::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_ATTR_TABCTRL, m_tabCtrl);
}

BEGIN_MESSAGE_MAP(CAttributesDialog, CDialogEx)
    ON_WM_SIZE()
    ON_NOTIFY(TCN_SELCHANGE, IDC_ATTR_TABCTRL, &CAttributesDialog::OnTabSelChange)
    ON_MESSAGE(WM_APP_MARK_DIRTY, &CAttributesDialog::OnMarkDirtyMessage)
    ON_COMMAND(ID_KB_ADD_ASSET,        &CAttributesDialog::OnKbAddAsset)
    ON_COMMAND(ID_KB_DELETE_ASSET,     &CAttributesDialog::OnKbDeleteAsset)
    ON_COMMAND(ID_KB_DUPLICATE_ASSET,  &CAttributesDialog::OnKbDuplicateAsset)
    ON_COMMAND(ID_KB_MOVE_ASSET,       &CAttributesDialog::OnKbMoveAsset)
    ON_COMMAND(ID_KB_ADD_SEGMENT,      &CAttributesDialog::OnKbAddSegment)
    ON_COMMAND(ID_KB_DELETE_SEGMENT,   &CAttributesDialog::OnKbDeleteSegment)
    ON_COMMAND(ID_KB_DUPLICATE_SEGMENT,&CAttributesDialog::OnKbDuplicateSegment)
END_MESSAGE_MAP()

//
// OnInitDialog — load accelerators, insert the three tabs, create and populate
// the hosted pages from the scenario (with dirty tracking suppressed), carry
// the help state into them, lay them out, and show the requested start tab.
//
BOOL CAttributesDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    m_hAccel = ::LoadAccelerators(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_MAIN_ACCEL));

    // The template is 740x460 dialog units at 12pt — taller than the work area
    // of a small or scaled display, which would put the Close button (pinned to
    // the client bottom) off-screen. Fit to the monitor we're opening on; the
    // hosted pages scroll their own content once they get less room.
    FitWindowToMonitorWorkArea(this, /*center*/ true);

    m_tabCtrl.InsertItem(0, _T("Scenario Setup"));
    m_tabCtrl.InsertItem(1, _T("Asset / Entity Editor"));
    m_tabCtrl.InsertItem(2, _T("Motion Path Editor"));

    CreatePages();

    // ReadFrom() SetWindowText's every edit control, firing EN_CHANGE which
    // relays WM_APP_MARK_DIRTY back to us — gate the dirty handler off so the
    // initial population doesn't flag the scenario as modified.
    m_suppressDirty = true;

    // Populate the pages from the shared scenario, then reveal the requested
    // starting tab. Order mirrors the main dialog's RefreshUiFromScenario.
    if (m_scenario)
    {
        m_pageSetup.ReadFrom(*m_scenario);
        m_pageAssets.RefreshAssetTree();
        if (!m_scenario->entities.empty())
            m_pageAssets.ReadFrom(m_scenario->entities[0]);
        m_pageMotion.Refresh();
    }

    // Carry the toolbar's field-help state into the hosted pages.
    m_pageSetup.SetHelpActive(m_helpActive);
    m_pageAssets.SetHelpActive(m_helpActive);
    m_pageMotion.SetHelpActive(m_helpActive);

    LayoutChildren();
    int startTab = (m_startTab >= 0 && m_startTab < 3) ? m_startTab : 0;
    ShowPage(startTab);

    m_suppressDirty = false;
    return TRUE;
}

//
// CreatePages — wire each page to the shared scenario/catalog, then create all
// three as hidden child dialogs overlaid on the tab content area.
//
void CAttributesDialog::CreatePages()
{
    // Parent the pages to the dialog (NOT the tab control) and overlay them on
    // the tab's content area — the same PUSHBUTTON-click workaround the main
    // dialog uses (see CScenarioEditorDialog::CreatePages).
    auto create = [this](CDialogEx& page, UINT idd) -> CDialogEx*
    {
        if (page.Create(idd, this))
        {
            page.ShowWindow(SW_HIDE);
            return &page;
        }
        return nullptr;
    };

    m_pageAssets.SetCatalog(m_catalog);
    m_pageSetup.SetScenario(m_scenario);
    m_pageAssets.SetScenario(m_scenario);
    m_pageMotion.SetScenario(m_scenario);

    m_pages[0] = create(m_pageSetup,  IDD_SCENARIO_SETUP_PAGE);
    m_pages[1] = create(m_pageAssets, IDD_ASSET_ENTITY_EDITOR_PAGE);
    m_pages[2] = create(m_pageMotion, IDD_MOTION_PATH_EDITOR_PAGE);
}

//
// ShowPage — hide the currently active page and show the one at `index`,
// positioned/sized to the tab's content rect, and sync the tab selection.
//
void CAttributesDialog::ShowPage(int index)
{
    if (index < 0 || index >= 3) return;
    if (m_activePage == index) return;

    if (m_activePage >= 0 && m_pages[m_activePage])
        m_pages[m_activePage]->ShowWindow(SW_HIDE);

    m_activePage = index;
    if (m_pages[m_activePage])
    {
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
// OnTabSelChange — show the page matching the tab control's new selection.
//
void CAttributesDialog::OnTabSelChange(NMHDR*, LRESULT* pResult)
{
    ShowPage(m_tabCtrl.GetCurSel());
    if (pResult) *pResult = 0;
}

//
// LayoutChildren — size the tab control to the client area (reserving space at
// the bottom), pin the Close button bottom-right, and resize the active page.
//
void CAttributesDialog::LayoutChildren()
{
    if (!::IsWindow(m_tabCtrl.GetSafeHwnd())) return;

    CRect rcClient;
    GetClientRect(&rcClient);

    // Measure the Close button FIRST: the strip reserved at the bottom has to be
    // derived from its real height, not assumed. The template's 14-DLU button
    // renders 34px at this dialog's 12pt font, so the old fixed 28px reserve was
    // 12px short. The tab control then ran past the button's top edge, and since
    // the active page is raised to wndTop over the tab's display area, the page
    // covered the upper ~2/3 of the button and swallowed clicks aimed at it --
    // only an ~11px strip along the bottom edge still reached the button, which
    // read as "the Close button doesn't work".
    int bw = 0, bh = 0;
    CWnd* close = GetDlgItem(IDOK);
    if (close)
    {
        CRect rcBtn;
        close->GetWindowRect(&rcBtn);
        bw = rcBtn.Width();
        bh = rcBtn.Height();
    }
    const int reserve = (bh > 0) ? (bh + 2 * kBottomMarginPx) : kBottomReservePx;

    const int tabHeight = (rcClient.Height() - reserve > 60)
                              ? rcClient.Height() - reserve
                              : 60;
    m_tabCtrl.SetWindowPos(nullptr,
                           4, 4,
                           rcClient.Width() - 8, tabHeight,
                           SWP_NOZORDER | SWP_NOACTIVATE);

    // Pin the Close button to the bottom-right corner, clear of the tab control.
    if (close)
    {
        close->SetWindowPos(nullptr,
                            rcClient.right - bw - kBottomMarginPx,
                            rcClient.bottom - bh - kBottomMarginPx,
                            0, 0,
                            SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    if (m_activePage >= 0 && m_pages[m_activePage])
    {
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
// OnSize — re-run the child layout whenever the dialog is resized.
//
void CAttributesDialog::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    LayoutChildren();
}

//
// PreTranslateMessage — give the accelerator table first crack at each message
// so shortcuts fire the ON_COMMAND handlers.
//
BOOL CAttributesDialog::PreTranslateMessage(MSG* pMsg)
{
    if (m_hAccel && ::TranslateAccelerator(m_hWnd, m_hAccel, pMsg))
        return TRUE;
    return CDialogEx::PreTranslateMessage(pMsg);
}

//
// OnMarkDirtyMessage — a hosted page reported an edit; flag the dialog modified
// unless dirty tracking is suppressed (during initial population).
//
LRESULT CAttributesDialog::OnMarkDirtyMessage(WPARAM /*w*/, LPARAM /*l*/)
{
    if (!m_suppressDirty)
        m_modified = true;
    return 0;
}

//
// CaptureIntoScenario — write the Setup page, the selected Asset/Entity, and
// the Motion page's pending edits back into the shared scenario.
//
void CAttributesDialog::CaptureIntoScenario()
{
    if (!m_scenario) return;
    m_pageSetup.WriteTo(*m_scenario);
    const int idx = m_pageAssets.SelectedIndex();
    if (idx >= 0 && idx < (int)m_scenario->entities.size())
    {
        m_pageAssets.WriteTo(m_scenario->entities[idx],
                             m_scenario->originLatDeg,
                             m_scenario->originLonDeg,
                             m_scenario->originAltM);
    }
    m_pageMotion.CommitPendingEdits();
}

void CAttributesDialog::OnOK()
{
    // Flush any in-progress control edits into the shared scenario before the
    // owner reads it back.
    CaptureIntoScenario();
    CDialogEx::OnOK();
}

void CAttributesDialog::OnCancel()
{
    // There is no working copy to discard — the pages edit the shared scenario
    // in place (Add/Delete etc. mutate it immediately), so Escape is a plain
    // "close": flush the last in-progress edit too, same as the Close button.
    CaptureIntoScenario();
    CDialogEx::OnCancel();
}

//
// OnKb* — keyboard-shortcut handlers: switch to the owning page (Assets = 1,
// Motion = 2) and synthesize a click on the matching toolbar button.
//
void CAttributesDialog::OnKbAddAsset()        { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_ADD_ASSET); }
void CAttributesDialog::OnKbDeleteAsset()     { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_DELETE_ASSET); }
void CAttributesDialog::OnKbDuplicateAsset()  { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_DUPLICATE_ASSET); }
void CAttributesDialog::OnKbMoveAsset()       { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_MOVE_ASSET); }
void CAttributesDialog::OnKbAddSegment()      { ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_ADD_SEGMENT); }
void CAttributesDialog::OnKbDeleteSegment()   { ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_DELETE_SEGMENT); }
void CAttributesDialog::OnKbDuplicateSegment(){ ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_DUPLICATE_SEGMENT); }
