#include "pch.h"
#include "AttributesDialog.h"
#include "Scenario.h"
#include "ScenarioWorker.h"   // WM_APP_MARK_DIRTY

namespace
{
    // Height reserved at the bottom of the dialog for the Close button.
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

BOOL CAttributesDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    m_hAccel = ::LoadAccelerators(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_MAIN_ACCEL));

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

void CAttributesDialog::OnTabSelChange(NMHDR*, LRESULT* pResult)
{
    ShowPage(m_tabCtrl.GetCurSel());
    if (pResult) *pResult = 0;
}

void CAttributesDialog::LayoutChildren()
{
    if (!::IsWindow(m_tabCtrl.GetSafeHwnd())) return;

    CRect rcClient;
    GetClientRect(&rcClient);

    const int tabHeight = (rcClient.Height() - kBottomReservePx > 60)
                              ? rcClient.Height() - kBottomReservePx
                              : 60;
    m_tabCtrl.SetWindowPos(nullptr,
                           4, 4,
                           rcClient.Width() - 8, tabHeight,
                           SWP_NOZORDER | SWP_NOACTIVATE);

    // Pin the Close button to the bottom-right corner.
    if (CWnd* close = GetDlgItem(IDOK))
    {
        CRect rcBtn;
        close->GetWindowRect(&rcBtn);
        const int bw = rcBtn.Width(), bh = rcBtn.Height();
        close->SetWindowPos(nullptr,
                            rcClient.right - bw - 6,
                            rcClient.bottom - bh - 6,
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

void CAttributesDialog::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    LayoutChildren();
}

BOOL CAttributesDialog::PreTranslateMessage(MSG* pMsg)
{
    if (m_hAccel && ::TranslateAccelerator(m_hWnd, m_hAccel, pMsg))
        return TRUE;
    return CDialogEx::PreTranslateMessage(pMsg);
}

LRESULT CAttributesDialog::OnMarkDirtyMessage(WPARAM /*w*/, LPARAM /*l*/)
{
    if (!m_suppressDirty)
        m_modified = true;
    return 0;
}

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

void CAttributesDialog::OnKbAddAsset()        { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_ADD_ASSET); }
void CAttributesDialog::OnKbDeleteAsset()     { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_DELETE_ASSET); }
void CAttributesDialog::OnKbDuplicateAsset()  { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_DUPLICATE_ASSET); }
void CAttributesDialog::OnKbMoveAsset()       { ShowPage(1); FireBnClicked(m_pages[1], IDC_BTN_MOVE_ASSET); }
void CAttributesDialog::OnKbAddSegment()      { ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_ADD_SEGMENT); }
void CAttributesDialog::OnKbDeleteSegment()   { ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_DELETE_SEGMENT); }
void CAttributesDialog::OnKbDuplicateSegment(){ ShowPage(2); FireBnClicked(m_pages[2], IDC_BTN_DUPLICATE_SEGMENT); }
