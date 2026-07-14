//=============================================================================
//  PlaysEditorDialog.h
//-----------------------------------------------------------------------------
//  Declares CPlaysEditorDialog, the modal dark-themed editor for the Plays
//  catalog (launched by the Plays tab's gear button). Lets the user add,
//  delete, reorder, rename, and scenario-bind categories/subcategories on a
//  working deep copy that is committed to the live catalog + plays.ini on Save.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-13
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "PlaysCatalog.h"

#include <string>

// Modal editor for the Plays catalog (opened by the main window's gear button).
// Black background / white text. A flat grid lists one row per category and one
// indented row per subcategory; buttons add/delete/reorder rows, associate a
// scenario .ini per subcategory, and a "Run Scenario" checkbox controls whether
// clicking a subcategory also starts playback. Operates on a deep copy and
// commits to *m_live + plays.ini on Save (discards on Cancel), mirroring
// CCatalogEditorDialog.
class CPlaysEditorDialog : public CDialogEx
{
public:
    enum { IDD = IDD_PLAYS_EDITOR };
    CPlaysEditorDialog(PlaysCatalog* live, std::wstring path, CWnd* parent = nullptr);

protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* pMsg) override;
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnAddCategory();
    afx_msg void OnAddSub();
    afx_msg void OnDeleteRow();
    afx_msg void OnMoveUp();
    afx_msg void OnMoveDown();
    afx_msg void OnBrowseScenario();
    afx_msg void OnRunToggle();
    afx_msg void OnListDblClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnSave();
    afx_msg void OnSaveClose();
    void OnCancel() override;

    DECLARE_MESSAGE_MAP()

private:
    // A grid row addresses either a category (sub < 0) or a subcategory.
    struct Ref { int cat; int sub; };   // sub == -1 => category row

    void Rebuild(int selectCat = -1, int selectSub = -1);
    int  SelectedRow() const;
    Ref  RefAt(int listRow) const;      // decode the row's stored lParam
    static DWORD_PTR Pack(int cat, int sub);
    static Ref       Unpack(DWORD_PTR p);

    PlaysCatalog* m_live = nullptr;
    std::wstring  m_path;
    PlaysCatalog  m_edit;               // working deep copy
    CBrush        m_blackBrush;
    CBrush        m_whiteBrush;
    CToolTipCtrl  m_toolTip;            // hover help for the editor buttons
};
