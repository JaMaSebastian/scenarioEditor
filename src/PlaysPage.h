//=============================================================================
//  PlaysPage.h
//-----------------------------------------------------------------------------
//  Declares CPlaysPage, the "Plays" tab: a scrollable, collapsible accordion
//  of play categories/subcategories built dynamically from the app's Plays
//  catalog, plus a client-area blurb pane and a gear button that opens the
//  Plays editor. Clicking a subcategory can load/run its associated scenario.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-10
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

#include <atlimage.h>   // CImage — gear button PNG (alpha-blended)
#include <vector>

class CScenarioEditorDialog;

// "Plays" tab: a vertical accordion of collapsible category panels down the
// left side. Each category is a full-width-of-its-text header button; clicking
// it expands / collapses an indented list of subcategory buttons beneath it,
// and the panels below reflow. If the accordion runs taller than the tab, a
// vertical scrollbar appears just to its right. The remaining client area (to
// the right of the accordion + scrollbar) shows the plain-English meaning of
// the most recently clicked play, centered near the top.
//
// Controls are created dynamically (the category / subcategory catalog lives in
// BuildAccordion) rather than in the .rc, so relayout is a single pass and new
// plays can be added by editing one table. Header buttons use the
// IDC_PLAYS_HEADER_* id range and subcategory buttons the IDC_PLAYS_ITEM_*
// range so the ON_COMMAND_RANGE handlers can catch each group.
class CPlaysPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_PLAYS_PAGE };
    CPlaysPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

    // The main dialog handles loading/running the scenario when a subcategory
    // with an associated .ini is clicked. Set once after page creation.
    void SetMainDialog(CScenarioEditorDialog* main) { m_main = main; }

    // Destroy the accordion's buttons and rebuild from theApp.Plays(). Called
    // by the main dialog after the Plays editor closes.
    void ReloadCatalog();

protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* pMsg) override;   // relay tooltip events
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;

    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnPaint();
    afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
    afx_msg void OnHeaderClicked(UINT nID);
    afx_msg void OnItemClicked(UINT nID);
    afx_msg void OnEditPlays();   // gear button — opens the Plays editor

    DECLARE_MESSAGE_MAP()

private:
    struct Play
    {
        CString name;
        CString blurb;         // plain-English meaning shown in the client area
        CString scenarioPath;  // associated scenario .ini (may be empty)
    };
    struct Category
    {
        CString            name;
        std::vector<Play>  items;
        UINT               headerId = 0;
        std::vector<UINT>  itemIds;
        bool               expanded = true;
    };

    void BuildAccordion();   // create the header / item buttons once
    void LoadGearImage();    // decode the gear PNG resource into m_gearImg
    void Relayout();         // position everything, honoring collapse + scroll
    void UpdateHeaderText(const Category& cat);  // refresh the expand/collapse glyph
    const Play* FindPlay(UINT itemId) const;
    CRect BlurbRect() const; // client region where the meaning is drawn

    std::vector<Category> m_categories;

    CScrollBar m_scroll;          // shown only when the accordion overflows
    int        m_scrollY   = 0;   // current vertical scroll offset (px)
    int        m_viewportH = 0;   // visible height (client height), for paging
    int        m_maxScroll = 0;   // clamp bound for m_scrollY
    int        m_blurbLeft = 0;   // left edge of the client blurb region (px)

    CString    m_selName;         // selected play name / meaning (client-area text)
    CString    m_selBlurb;
    CFont      m_titleFont;       // bold font for the selected play's name
    int        m_blurbTop = 14;   // top inset of the blurb text (pushed below the gear)

    CButton      m_gearBtn;       // top-right of the client area; opens the editor
    CImage       m_gearImg;       // gear icon (PNG, alpha), drawn owner-draw
    CToolTipCtrl m_toolTip;       // hover help for the gear button

    CScenarioEditorDialog* m_main = nullptr;  // for load/run on subcategory click
};
