//=============================================================================
//  DeployPage.h
//-----------------------------------------------------------------------------
//  Declares CDeployPage, the dark-themed "Deploy" tab. It owns an owner-drawn
//  list of deployable resources (e.g. AI servers) with per-row status glyphs and
//  five action buttons (Deploy/Stop/Go/Pause/Release) driven by a small per-row
//  state machine; column widths persist to settings.ini.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-10
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

#include <vector>

// "Deploy" tab: a dark-themed report list of deployable resources (e.g. AI
// servers). Each row shows a status icon + word, the resource name, who has it,
// the reservation window, and five per-row action buttons: Deploy, Stop, Go,
// Pause, Release. The list is owner-drawn (LVS_OWNERDRAWFIXED) so the rows,
// status glyphs, and action buttons can all be painted on the black background;
// clicks are hit-tested to a button and drive a small per-row state machine.
//
// Shares the Run tab's dark color scheme (black background, white text) — the
// same OnCtlColor / brush pattern used by COutputPlaybackPage.
class CDeployPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_DEPLOY_PAGE };
    CDeployPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

protected:
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;

    afx_msg void OnSize(UINT nType, int cx, int cy);
    //
    // OnShowWindow / OnDestroy — persist the user's column widths when the tab
    // is hidden or the app closes.
    //
    afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
    afx_msg void OnDestroy();
    //
    // OnCtlColor — dark-theme brushes (black bg, white text) for the page.
    //
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    //
    // OnDrawItem — owner-draws a full list row: status icon, data columns, and
    // the five action buttons (enabled per the row's state).
    //
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
    //
    // OnListClick — hit-tests a click to an action button and applies it.
    //
    afx_msg void OnListClick(NMHDR* pNMHDR, LRESULT* pResult);

    DECLARE_MESSAGE_MAP()

private:
    enum class Status { Open, InUse, Unavailable };
    enum class Btn { Deploy, Stop, Go, Pause, Release };  // column order after data columns

    struct DeployRow
    {
        Status  status    = Status::Open;
        bool    ownedByUs = false;   // true once WE deploy it
        bool    running   = false;
        bool    paused    = false;
        CString name, contact, start, end;
    };

    // Populate m_rows with the demo resource list and mirror them as list items.
    void SeedRows();
    // Size the list to fill the page client area.
    void LayoutList();
    // Persist the current hand-set column widths into settings.ini so they
    // survive across sessions (no hardcoded default layout).
    void SaveColumnWidths();

    // Whether action button b is currently valid for row r (the state machine).
    bool BtnEnabled(const DeployRow& r, Btn b) const;
    // Apply action button b to row r, transitioning its state.
    void ApplyBtn(DeployRow& r, Btn b);

    // Owner-draw helpers.
    // Paint the status glyph (blue circle / green check / red dash) in rc.
    void DrawStatusIcon(CDC& dc, const CRect& rc, Status s) const;
    // Paint a centered action button; greys out label/fill when disabled.
    void DrawButton(CDC& dc, const CRect& rc, const CString& label,
                    COLORREF base, bool enabled) const;
    // Which action button (Btn) the point hit on the given row, or -1 if none.
    int  ButtonHit(int row, CPoint pt) const;

    // Display text for a Status value ("Open" / "In use" / "Unavailable").
    static const TCHAR* StatusWord(Status s);

    CListCtrl m_list;
    std::vector<DeployRow> m_rows;

    // Dark theme brushes (same pattern as COutputPlaybackPage).
    CBrush m_blackBrush;
    CBrush m_whiteBrush;

    CImageList m_rowHeight;   // 1x N dummy list to force the owner-draw row height
};
