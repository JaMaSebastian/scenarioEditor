//=============================================================================
//  ScrollablePage.h
//-----------------------------------------------------------------------------
//  Declares CScrollablePage, a CDialogEx base that keeps every control on a
//  fixed-layout page reachable on any screen. The tab pages are authored as
//  700x420-dialog-unit templates; when the host window is smaller than that
//  (small laptop panel, secondary monitor, high DPI scaling) the page used to
//  simply clip its bottom/right controls. This base measures the controls,
//  shows standard scroll bars when they don't fit, and scrolls them into view.
//
//  Also declares the monitor-work-area fitting helpers used by the top-level
//  windows so they open on-screen on whichever display they land on.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-27
//=============================================================================
#pragma once

#include "pch.h"

//-----------------------------------------------------------------------------
// CScrollablePage — dialog base with automatic scrolling
//   Content extent is measured from the live child-control rectangles (not the
//   .rc template), so pages that lay themselves out at runtime are handled too:
//   a page whose controls always fit the client area simply never gets a bar.
//   Pages that implement their own scrolling call EnableAutoScroll(false).
//-----------------------------------------------------------------------------
class CScrollablePage : public CDialogEx
{
public:
    CScrollablePage(UINT idd, CWnd* pParent) : CDialogEx(idd, pParent) {}

    // Turn the automatic bars on/off. Off resets any scroll offset first, so
    // the controls go back to their unscrolled positions.
    void EnableAutoScroll(bool enable);
    bool IsAutoScrollEnabled() const { return m_autoScroll; }

    // Scroll the given control fully into view (no-op if it already is).
    void EnsureControlVisible(UINT controlId);
    void EnsureVisible(CWnd* ctrl);

protected:
    BOOL PreTranslateMessage(MSG* pMsg) override;

    // Re-measure the content and refresh the bars. WM_SIZE runs this already;
    // subclasses that create or move controls at runtime call it directly.
    void UpdateScrollLayout();

    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg LRESULT OnRescroll(WPARAM, LPARAM);
    afx_msg LRESULT OnFollowFocus(WPARAM, LPARAM);
    afx_msg void OnVScroll(UINT code, UINT pos, CScrollBar* sb);
    afx_msg void OnHScroll(UINT code, UINT pos, CScrollBar* sb);
    afx_msg BOOL OnMouseWheel(UINT flags, short zDelta, CPoint pt);

    DECLARE_MESSAGE_MAP()

private:
    // Union of every child control's rectangle, in unscrolled client coords.
    CSize MeasureContent();
    // Show/hide one standard scroll bar and push its range/page/position.
    void  ApplyBar(int bar, bool show, int total, int page, int pos);
    // Move the controls so the client origin sits at (x, y) of the content.
    void  ScrollTo(int x, int y);
    int   MaxScrollX() const;
    int   MaxScrollY() const;

    bool   m_autoScroll     = true;
    bool   m_inLayout       = false;  // re-entrancy guard (showing a bar resizes us)
    bool   m_rescrollPosted = false;  // a deferred re-measure is already queued
    bool   m_hasV       = false;
    bool   m_hasH       = false;
    CPoint m_scrollPos  = CPoint(0, 0);   // scrolled-away px (left, top)
    CSize  m_content    = CSize(0, 0);    // full content extent, px
    CSize  m_viewport   = CSize(0, 0);    // client area minus the shown bars
};

//
// FitWindowToMonitorWorkArea — shrink (and optionally re-center) a top-level
// window so it fits the work area of the monitor it is on, or the nearest
// monitor if it is off-screen. Multi-monitor safe, unlike SM_CXFULLSCREEN /
// SM_CYFULLSCREEN, which only ever describe the primary display.
//
void FitWindowToMonitorWorkArea(CWnd* wnd, bool center = false);

//
// ClampRectToMonitorWorkArea — same clamp applied to a screen rectangle; used
// when restoring a saved window placement that may name a monitor that has
// since changed size or gone away.
//
void ClampRectToMonitorWorkArea(CRect& rc);
