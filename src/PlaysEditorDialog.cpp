//=============================================================================
//  PlaysEditorDialog.cpp
//-----------------------------------------------------------------------------
//  Implements CPlaysEditorDialog: builds the dark-themed grid of categories
//  and subcategories, handles add/delete/reorder/rename/scenario-browse
//  actions and inline double-click editing (via a small name-prompt dialog),
//  and commits the working copy to the live catalog + plays.ini on Save.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-13
//=============================================================================
#include "pch.h"
#include "PlaysEditorDialog.h"
#include "../log.h"

#include <uxtheme.h>   // SetWindowTheme — declassic the checkbox for dark text
#include <utility>     // std::swap, std::move

namespace
{
    constexpr int kMaxCategories = 10;
    constexpr int kMaxSubsTotal  = 40;

    // Tiny inline modal for prompting a single name string (own copy so this
    // TU doesn't depend on CatalogEditorDialog's file-local prompt).
    class CPlayNamePrompt : public CDialogEx
    {
    public:
        enum { IDD = IDD_PROMPT_NAME };
        CPlayNamePrompt(const CString& caption, const CString& prompt,
                        const CString& initial, CWnd* parent)
            : CDialogEx(IDD, parent), m_caption(caption), m_prompt(prompt),
              m_value(initial) {}
        const CString& Value() const { return m_value; }
    protected:
        BOOL OnInitDialog() override
        {
            CDialogEx::OnInitDialog();
            SetWindowText(m_caption);
            SetDlgItemText(IDC_LBL_PROMPT,  m_prompt);
            SetDlgItemText(IDC_EDIT_PROMPT, m_value);
            if (CEdit* e = (CEdit*)GetDlgItem(IDC_EDIT_PROMPT))
            {
                e->SetSel(0, -1);
                e->SetFocus();
                return FALSE;
            }
            return TRUE;
        }
        void OnOK() override
        {
            GetDlgItemText(IDC_EDIT_PROMPT, m_value);
            CDialogEx::OnOK();
        }
        CString m_caption, m_prompt, m_value;
    };
}

BEGIN_MESSAGE_MAP(CPlaysEditorDialog, CDialogEx)
    ON_WM_CTLCOLOR()
    ON_BN_CLICKED(IDC_BTN_PLAYS_ADD_CAT,    &CPlaysEditorDialog::OnAddCategory)
    ON_BN_CLICKED(IDC_BTN_PLAYS_ADD_SUB,    &CPlaysEditorDialog::OnAddSub)
    ON_BN_CLICKED(IDC_BTN_PLAYS_DELETE,     &CPlaysEditorDialog::OnDeleteRow)
    ON_BN_CLICKED(IDC_BTN_PLAYS_MOVE_UP,    &CPlaysEditorDialog::OnMoveUp)
    ON_BN_CLICKED(IDC_BTN_PLAYS_MOVE_DOWN,  &CPlaysEditorDialog::OnMoveDown)
    ON_BN_CLICKED(IDC_BTN_PLAYS_BROWSE,     &CPlaysEditorDialog::OnBrowseScenario)
    ON_BN_CLICKED(IDC_CHK_PLAYS_RUN,        &CPlaysEditorDialog::OnRunToggle)
    ON_BN_CLICKED(IDC_BTN_PLAYS_SAVE,       &CPlaysEditorDialog::OnSave)
    ON_BN_CLICKED(IDC_BTN_PLAYS_SAVE_CLOSE, &CPlaysEditorDialog::OnSaveClose)
    ON_NOTIFY(NM_DBLCLK, IDC_LIST_PLAYS_EDIT, &CPlaysEditorDialog::OnListDblClick)
END_MESSAGE_MAP()

//
// CPlaysEditorDialog::CPlaysEditorDialog — takes the live catalog + its ini
//   path; deep-copies the live catalog into m_edit so edits stay uncommitted
//   until Save.
//
CPlaysEditorDialog::CPlaysEditorDialog(PlaysCatalog* live, std::wstring path,
                                       CWnd* parent)
    : CDialogEx(IDD, parent), m_live(live), m_path(std::move(path))
{
    if (m_live) m_edit = *m_live;   // deep copy
}

//
// CPlaysEditorDialog::Pack — encode a (category, sub) pair into a list item's
//   lParam; sub == -1 (0xFFFF) marks a category row.
//
DWORD_PTR CPlaysEditorDialog::Pack(int cat, int sub)
{
    return (static_cast<DWORD_PTR>(cat & 0xFFFF) << 16) |
           static_cast<DWORD_PTR>(sub & 0xFFFF);
}

//
// CPlaysEditorDialog::Unpack — inverse of Pack; decodes an lParam back into a
//   Ref, restoring sub == -1 for category rows.
//
CPlaysEditorDialog::Ref CPlaysEditorDialog::Unpack(DWORD_PTR p)
{
    Ref r;
    r.cat = static_cast<int>((p >> 16) & 0xFFFF);
    const int s = static_cast<int>(p & 0xFFFF);
    r.sub = (s == 0xFFFF) ? -1 : s;
    return r;
}

//
// CPlaysEditorDialog::OnInitDialog — apply the dark theme, create the grid
//   columns, seed the Run-Scenario checkbox, wire per-control tooltips, and
//   populate the grid via Rebuild.
//
BOOL CPlaysEditorDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // Dark theme: black page background + white text; declassic the checkbox
    // so its label honors the text color (same idiom as CPreviewPage).
    m_blackBrush.CreateSolidBrush(RGB(0, 0, 0));
    m_whiteBrush.CreateSolidBrush(RGB(255, 255, 255));
    SetBackgroundColor(RGB(0, 0, 0));
    if (CWnd* chk = GetDlgItem(IDC_CHK_PLAYS_RUN))
        ::SetWindowTheme(chk->GetSafeHwnd(), L"", L"");

    if (CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_PLAYS_EDIT))
    {
        lv->SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        lv->SetBkColor(RGB(0, 0, 0));
        lv->SetTextBkColor(RGB(0, 0, 0));
        lv->SetTextColor(RGB(255, 255, 255));

        CRect rc; lv->GetClientRect(&rc);
        const int total = rc.Width() - ::GetSystemMetrics(SM_CXVSCROLL) - 4;
        const int wType = 60;
        const int wName = 150;
        int remain = total - wType - wName;
        if (remain < 300) remain = 300;
        const int wDef  = remain * 55 / 100;   // Definition takes the lion's share
        const int wScen = remain - wDef;
        lv->InsertColumn(0, _T("Type"),          LVCFMT_LEFT, wType);
        lv->InsertColumn(1, _T("Name"),          LVCFMT_LEFT, wName);
        lv->InsertColumn(2, _T("Definition"),    LVCFMT_LEFT, wDef);
        lv->InsertColumn(3, _T("Scenario .ini"), LVCFMT_LEFT, wScen);
    }

    CheckDlgButton(IDC_CHK_PLAYS_RUN, m_edit.runScenario ? BST_CHECKED : BST_UNCHECKED);

    // Hover help for every control.
    if (m_toolTip.Create(this))
    {
        struct Tip { UINT id; const TCHAR* text; };
        static const Tip kTips[] = {
            { IDC_LIST_PLAYS_EDIT,
              _T("One row per category and per subcategory. Double-click a cell to edit: ")
              _T("Name, Definition, or (subcategory) Scenario .ini.") },
            { IDC_BTN_PLAYS_ADD_CAT,   _T("Add a new play category (max 10).") },
            { IDC_BTN_PLAYS_ADD_SUB,   _T("Add a subcategory under the selected category (max 40 total).") },
            { IDC_BTN_PLAYS_DELETE,    _T("Delete the selected row. Deleting a category removes its subcategories.") },
            { IDC_BTN_PLAYS_MOVE_UP,   _T("Move the selected category or subcategory up.") },
            { IDC_BTN_PLAYS_MOVE_DOWN, _T("Move the selected category or subcategory down.") },
            { IDC_BTN_PLAYS_BROWSE,    _T("Associate a scenario .ini with the selected subcategory.") },
            { IDC_CHK_PLAYS_RUN,       _T("When checked, clicking a subcategory not only loads its scenario but also starts playback.") },
            { IDC_BTN_PLAYS_SAVE,      _T("Save changes to plays.ini and keep editing.") },
            { IDC_BTN_PLAYS_SAVE_CLOSE,_T("Save changes to plays.ini and close.") },
            { IDCANCEL,                _T("Discard all changes and close.") },
        };
        for (const Tip& t : kTips)
            if (CWnd* w = GetDlgItem(t.id))
                m_toolTip.AddTool(w, t.text);
        m_toolTip.SetMaxTipWidth(360);
        m_toolTip.SetDelayTime(TTDT_AUTOPOP, 20000);
        m_toolTip.Activate(TRUE);
    }

    Rebuild();
    return TRUE;
}

//
// CPlaysEditorDialog::PreTranslateMessage — relay messages to the tooltip
//   control so hover help works.
//
BOOL CPlaysEditorDialog::PreTranslateMessage(MSG* pMsg)
{
    if (m_toolTip.GetSafeHwnd())
        m_toolTip.RelayEvent(pMsg);
    return CDialogEx::PreTranslateMessage(pMsg);
}

//
// CPlaysEditorDialog::OnCtlColor — paint dialog/label backgrounds black with
//   white text for the dark theme; defers to the base for other control types.
//
HBRUSH CPlaysEditorDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    if (!m_blackBrush.GetSafeHandle())
        return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);

    switch (nCtlColor)
    {
        case CTLCOLOR_DLG:
        case CTLCOLOR_STATIC:   // labels + declassic'd checkbox
            pDC->SetBkMode(TRANSPARENT);
            pDC->SetTextColor(RGB(255, 255, 255));
            pDC->SetBkColor(RGB(0, 0, 0));
            return static_cast<HBRUSH>(m_blackBrush);
        default:
            return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
    }
}

// -------------------------------------------------------------------------
// Grid rebuild + selection helpers
// -------------------------------------------------------------------------

//
// CPlaysEditorDialog::Rebuild — refill the grid from m_edit (category rows and
//   indented subcategory rows, each stamped with its packed lParam) and
//   optionally reselect the row identified by (selectCat, selectSub).
//
void CPlaysEditorDialog::Rebuild(int selectCat, int selectSub)
{
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_PLAYS_EDIT);
    if (!lv) return;
    lv->DeleteAllItems();

    int row = 0;
    int wantRow = -1;
    for (int ci = 0; ci < (int)m_edit.categories.size(); ++ci)
    {
        const PlayCategory& cat = m_edit.categories[ci];
        const int r = lv->InsertItem(row, _T("Category"));
        lv->SetItemText(r, 1, cat.name);
        lv->SetItemText(r, 2, _T(""));
        lv->SetItemText(r, 3, _T(""));
        lv->SetItemData(r, Pack(ci, -1));
        if (ci == selectCat && selectSub < 0) wantRow = r;
        ++row;

        for (int si = 0; si < (int)cat.subs.size(); ++si)
        {
            const PlaySub& sub = cat.subs[si];
            const int rs = lv->InsertItem(row, _T("   Sub"));
            lv->SetItemText(rs, 1, CString(_T("    ")) + sub.name);
            lv->SetItemText(rs, 2, sub.blurb);
            lv->SetItemText(rs, 3, sub.scenarioPath);
            lv->SetItemData(rs, Pack(ci, si));
            if (ci == selectCat && si == selectSub) wantRow = rs;
            ++row;
        }
    }

    if (wantRow >= 0)
    {
        lv->SetItemState(wantRow, LVIS_SELECTED | LVIS_FOCUSED,
                         LVIS_SELECTED | LVIS_FOCUSED);
        lv->EnsureVisible(wantRow, FALSE);
    }
}

//
// CPlaysEditorDialog::SelectedRow — index of the currently selected grid row,
//   or -1 if none.
//
int CPlaysEditorDialog::SelectedRow() const
{
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_PLAYS_EDIT);
    return lv ? lv->GetNextItem(-1, LVNI_SELECTED) : -1;
}

//
// CPlaysEditorDialog::RefAt — decode a grid row's stored lParam into a Ref;
//   returns {-1,-1} for an invalid row.
//
CPlaysEditorDialog::Ref CPlaysEditorDialog::RefAt(int listRow) const
{
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_PLAYS_EDIT);
    if (!lv || listRow < 0) return { -1, -1 };
    return Unpack(lv->GetItemData(listRow));
}

// -------------------------------------------------------------------------
// Add / delete / reorder
// -------------------------------------------------------------------------

//
// CPlaysEditorDialog::OnAddCategory — prompt for a name and append a new
//   category (up to the 10-category cap), then select it.
//
void CPlaysEditorDialog::OnAddCategory()
{
    if ((int)m_edit.categories.size() >= kMaxCategories)
    {
        AfxMessageBox(_T("Maximum of 10 categories reached."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    CPlayNamePrompt dlg(_T("Add Category"), _T("Category name:"), _T(""), this);
    if (dlg.DoModal() != IDOK) return;
    CString name = dlg.Value(); name.Trim();
    if (name.IsEmpty()) return;

    PlayCategory cat;
    cat.name = name;
    m_edit.categories.push_back(cat);
    Rebuild((int)m_edit.categories.size() - 1, -1);
}

//
// CPlaysEditorDialog::OnAddSub — prompt for a name and append a subcategory
//   under the selected category (up to the 40-sub cap), then select it.
//
void CPlaysEditorDialog::OnAddSub()
{
    if (m_edit.SubCount() >= (size_t)kMaxSubsTotal)
    {
        AfxMessageBox(_T("Maximum of 40 subcategories reached."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const Ref r = RefAt(SelectedRow());
    if (r.cat < 0 || r.cat >= (int)m_edit.categories.size())
    {
        AfxMessageBox(_T("Select a category (or one of its subcategories) first."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    CPlayNamePrompt dlg(_T("Add Subcategory"), _T("Subcategory name:"), _T(""), this);
    if (dlg.DoModal() != IDOK) return;
    CString name = dlg.Value(); name.Trim();
    if (name.IsEmpty()) return;

    PlaySub sub;
    sub.name = name;
    m_edit.categories[r.cat].subs.push_back(sub);
    Rebuild(r.cat, (int)m_edit.categories[r.cat].subs.size() - 1);
}

//
// CPlaysEditorDialog::OnDeleteRow — delete the selected row after confirmation;
//   deleting a category also removes its subcategories.
//
void CPlaysEditorDialog::OnDeleteRow()
{
    const Ref r = RefAt(SelectedRow());
    if (r.cat < 0 || r.cat >= (int)m_edit.categories.size())
    {
        AfxMessageBox(_T("Select a row to delete."), MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (r.sub < 0)
    {
        PlayCategory& cat = m_edit.categories[r.cat];
        CString msg;
        msg.Format(_T("Delete category \"%s\" and its %d subcategory item(s)?"),
                   cat.name.GetString(), (int)cat.subs.size());
        if (AfxMessageBox(msg, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
        m_edit.categories.erase(m_edit.categories.begin() + r.cat);
        Rebuild();
    }
    else
    {
        PlayCategory& cat = m_edit.categories[r.cat];
        if (r.sub >= (int)cat.subs.size()) return;
        CString msg;
        msg.Format(_T("Delete subcategory \"%s\"?"), cat.subs[r.sub].name.GetString());
        if (AfxMessageBox(msg, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
        cat.subs.erase(cat.subs.begin() + r.sub);
        Rebuild(r.cat, -1);
    }
}

//
// CPlaysEditorDialog::OnMoveUp — swap the selected category or subcategory with
//   its predecessor (no-op at the top).
//
void CPlaysEditorDialog::OnMoveUp()
{
    const Ref r = RefAt(SelectedRow());
    if (r.cat < 0) return;

    if (r.sub < 0)
    {
        if (r.cat == 0) return;
        std::swap(m_edit.categories[r.cat], m_edit.categories[r.cat - 1]);
        Rebuild(r.cat - 1, -1);
    }
    else
    {
        if (r.sub == 0) return;
        auto& subs = m_edit.categories[r.cat].subs;
        std::swap(subs[r.sub], subs[r.sub - 1]);
        Rebuild(r.cat, r.sub - 1);
    }
}

//
// CPlaysEditorDialog::OnMoveDown — swap the selected category or subcategory
//   with its successor (no-op at the bottom).
//
void CPlaysEditorDialog::OnMoveDown()
{
    const Ref r = RefAt(SelectedRow());
    if (r.cat < 0) return;

    if (r.sub < 0)
    {
        if (r.cat + 1 >= (int)m_edit.categories.size()) return;
        std::swap(m_edit.categories[r.cat], m_edit.categories[r.cat + 1]);
        Rebuild(r.cat + 1, -1);
    }
    else
    {
        auto& subs = m_edit.categories[r.cat].subs;
        if (r.sub + 1 >= (int)subs.size()) return;
        std::swap(subs[r.sub], subs[r.sub + 1]);
        Rebuild(r.cat, r.sub + 1);
    }
}

//
// CPlaysEditorDialog::OnBrowseScenario — open a file dialog to associate a
//   scenario .ini with the selected subcategory.
//
void CPlaysEditorDialog::OnBrowseScenario()
{
    const Ref r = RefAt(SelectedRow());
    if (r.cat < 0 || r.sub < 0)
    {
        AfxMessageBox(_T("Select a subcategory row to associate a scenario .ini."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    PlaySub& sub = m_edit.categories[r.cat].subs[r.sub];

    CFileDialog dlg(/*open*/TRUE, _T("ini"), sub.scenarioPath,
                    OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST,
                    _T("Scenario INI (*.ini)|*.ini|All Files (*.*)|*.*||"),
                    this);
    if (dlg.DoModal() != IDOK) return;
    sub.scenarioPath = dlg.GetPathName();
    Rebuild(r.cat, r.sub);
}

//
// CPlaysEditorDialog::OnRunToggle — mirror the Run-Scenario checkbox state into
//   the working catalog.
//
void CPlaysEditorDialog::OnRunToggle()
{
    m_edit.runScenario = (IsDlgButtonChecked(IDC_CHK_PLAYS_RUN) == BST_CHECKED);
}

//
// CPlaysEditorDialog::OnListDblClick — inline edit the double-clicked row: rename
//   a category, or for a subcategory edit the definition (col 2), browse a
//   scenario (col 3), or rename (other columns).
//
void CPlaysEditorDialog::OnListDblClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)pNMHDR;
    const Ref r = RefAt(ia->iItem);
    if (r.cat < 0) return;

    if (r.sub < 0)
    {
        // Rename category.
        PlayCategory& cat = m_edit.categories[r.cat];
        CPlayNamePrompt dlg(_T("Rename Category"), _T("Category name:"),
                            cat.name, this);
        if (dlg.DoModal() != IDOK) return;
        CString name = dlg.Value(); name.Trim();
        if (name.IsEmpty()) return;
        cat.name = name;
        Rebuild(r.cat, -1);
    }
    else
    {
        // Subcategory: which cell was double-clicked decides the edit.
        //   col 2 (Definition) -> edit the blurb
        //   col 3 (Scenario)   -> browse for a .ini
        //   else (Type / Name) -> rename
        PlaySub& sub = m_edit.categories[r.cat].subs[r.sub];
        if (ia->iSubItem == 3)
        {
            OnBrowseScenario();
            return;
        }
        if (ia->iSubItem == 2)
        {
            CPlayNamePrompt dlg(_T("Edit Definition"),
                                _T("Plain-English meaning (shown when the play is selected):"),
                                sub.blurb, this);
            if (dlg.DoModal() != IDOK) return;
            sub.blurb = dlg.Value();
            Rebuild(r.cat, r.sub);
            return;
        }
        CPlayNamePrompt dlg(_T("Rename Subcategory"), _T("Subcategory name:"),
                            sub.name, this);
        if (dlg.DoModal() != IDOK) return;
        CString name = dlg.Value(); name.Trim();
        if (name.IsEmpty()) return;
        sub.name = name;
        Rebuild(r.cat, r.sub);
    }
}

// -------------------------------------------------------------------------
// Save / Cancel
// -------------------------------------------------------------------------

//
// CPlaysEditorDialog::OnSave — persist the working copy to plays.ini and, on
//   success, commit it into the live catalog; reports errors and keeps the
//   dialog open.
//
void CPlaysEditorDialog::OnSave()
{
    if (!m_live) return;
    // Capture the checkbox in case a toggle notification was missed.
    m_edit.runScenario = (IsDlgButtonChecked(IDC_CHK_PLAYS_RUN) == BST_CHECKED);
    if (!m_edit.SaveToIni(m_path))
    {
        AfxMessageBox(_T("Failed to save plays.ini. See error.log."),
                      MB_OK | MB_ICONERROR);
        return;
    }
    *m_live = m_edit;
}

//
// CPlaysEditorDialog::OnSaveClose — Save then close the dialog with IDOK.
//
void CPlaysEditorDialog::OnSaveClose()
{
    OnSave();
    EndDialog(IDOK);
}

//
// CPlaysEditorDialog::OnCancel — close without committing; the working copy is
//   discarded.
//
void CPlaysEditorDialog::OnCancel()
{
    // Discard m_edit (never written back).
    CDialogEx::OnCancel();
}
