//=============================================================================
//  ScrollablePage.cpp
//-----------------------------------------------------------------------------
//  Implements CScrollablePage: measures the page's controls, adds/removes the
//  standard scroll bars when the client area can't show them all, and scrolls
//  the controls (rather than clipping them) in response to the bars, the mouse
//  wheel, and EnsureControlVisible. Also implements the monitor work-area
//  fitting helpers.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-27
//=============================================================================
#include "pch.h"
#include "ScrollablePage.h"

#include <algorithm>

namespace
{
    // Breathing room past the last control, added only once a page actually
    // scrolls — adding it unconditionally would push a page whose controls
    // exactly fill the client area (Deploy's full-bleed list) over the edge and
    // give it a permanent, pointless scroll bar.
    constexpr int kEdgePadPx  = 8;
    // Arrow-button step, and the per-line step used for wheel notches.
    constexpr int kLineStepPx = 24;

    // Deferred "measure again" tick. A subclass that lays its own controls out
    // in OnSize (CDeployPage stretches its list to the client area) does that
    // *after* chaining to our handler, so the extent we measured inline is one
    // step stale. Posting the re-check lets that layout finish first.
    constexpr UINT WM_APP_RESCROLL = WM_APP + 0x51;

    // Deferred "did the focus land off-screen?" tick, posted from
    // PreTranslateMessage because the dialog manager hasn't moved the focus yet
    // when the navigation key comes through.
    constexpr UINT WM_APP_FOLLOWFOCUS = WM_APP + 0x52;
}

BEGIN_MESSAGE_MAP(CScrollablePage, CDialogEx)
    ON_WM_SIZE()
    ON_WM_VSCROLL()
    ON_WM_HSCROLL()
    ON_WM_MOUSEWHEEL()
    ON_MESSAGE(WM_APP_RESCROLL,    &CScrollablePage::OnRescroll)
    ON_MESSAGE(WM_APP_FOLLOWFOCUS, &CScrollablePage::OnFollowFocus)
END_MESSAGE_MAP()

//
// EnableAutoScroll — opt a page in/out of the automatic bars. Opting out
//   unscrolls first so the subclass sees its controls where it put them.
//
void CScrollablePage::EnableAutoScroll(bool enable)
{
    if (m_autoScroll == enable) return;
    m_autoScroll = enable;

    if (!::IsWindow(GetSafeHwnd()))
        return;

    if (!enable)
    {
        ScrollTo(0, 0);
        ShowScrollBar(SB_BOTH, FALSE);
        m_hasV = m_hasH = false;
    }
    else
    {
        UpdateScrollLayout();
    }
}

//
// OnSize — the host resized the page; re-measure and refresh the bars.
//
void CScrollablePage::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    UpdateScrollLayout();

    // ...and once more after the subclass has finished its own layout.
    if (m_autoScroll && !m_rescrollPosted && ::IsWindow(GetSafeHwnd()))
    {
        m_rescrollPosted = true;
        PostMessage(WM_APP_RESCROLL);
    }
}

//
// OnRescroll — deferred re-measure (see WM_APP_RESCROLL).
//
LRESULT CScrollablePage::OnRescroll(WPARAM, LPARAM)
{
    m_rescrollPosted = false;
    UpdateScrollLayout();
    return 0;
}

//
// MeasureContent — union of the child-control rectangles in unscrolled client
//   coords. Hidden controls are included on purpose: a control that is hidden
//   now (a mode-specific settings group) can be shown later without another
//   WM_SIZE, and under-measuring would leave it unreachable. Tooltips are
//   WS_POPUP, not children, so they don't skew the extent.
//
CSize CScrollablePage::MeasureContent()
{
    CSize content(0, 0);

    for (CWnd* child = GetWindow(GW_CHILD); child != nullptr;
         child = child->GetWindow(GW_HWNDNEXT))
    {
        const HWND hChild = child->GetSafeHwnd();
        if (!hChild) continue;

        CRect rc;
        ::GetWindowRect(hChild, &rc);
        if (rc.Width() <= 0 || rc.Height() <= 0)
            continue;              // parked/zero-sized controls carry no extent
        ScreenToClient(&rc);

        content.cx = std::max<int>(content.cx, rc.right  + m_scrollPos.x);
        content.cy = std::max<int>(content.cy, rc.bottom + m_scrollPos.y);
    }

    return content;
}

//
// UpdateScrollLayout — decide which bars are needed for the current client
//   size, clamp the scroll offset to the new limits, and publish the ranges.
//   Guarded against re-entry: showing or hiding a bar changes the client area,
//   which sends another WM_SIZE straight back here.
//
void CScrollablePage::UpdateScrollLayout()
{
    if (!m_autoScroll || m_inLayout || !::IsWindow(GetSafeHwnd()))
        return;

    m_inLayout = true;

    const int sbW = ::GetSystemMetrics(SM_CXVSCROLL);
    const int sbH = ::GetSystemMetrics(SM_CYHSCROLL);

    CRect rcClient;
    GetClientRect(&rcClient);

    // Decide against the bar-free client size, so the decision can't flip-flop
    // with the bars it causes to appear.
    const int fullW = rcClient.Width()  + (m_hasV ? sbW : 0);
    const int fullH = rcClient.Height() + (m_hasH ? sbH : 0);
    if (fullW <= 0 || fullH <= 0)
    {
        m_inLayout = false;
        return;
    }

    CSize content = MeasureContent();

    // Each bar eats a strip of the viewport, which can be what tips the other
    // axis over — so resolve them together.
    bool needV = content.cy > fullH;
    bool needH = content.cx > fullW;
    if (needV && content.cx > fullW - sbW) needH = true;
    if (needH && content.cy > fullH - sbH) needV = true;

    m_viewport.cx = needV ? fullW - sbW : fullW;
    m_viewport.cy = needH ? fullH - sbH : fullH;
    if (needH) content.cx += kEdgePadPx;
    if (needV) content.cy += kEdgePadPx;
    m_content = content;

    m_hasV = needV;
    m_hasH = needH;

    // The window may have grown: pull the content back into view before the
    // bars are published so thumb and controls agree.
    ScrollTo(std::min<int>(m_scrollPos.x, MaxScrollX()),
             std::min<int>(m_scrollPos.y, MaxScrollY()));

    ApplyBar(SB_VERT, needV, m_content.cy, m_viewport.cy, m_scrollPos.y);
    ApplyBar(SB_HORZ, needH, m_content.cx, m_viewport.cx, m_scrollPos.x);

    m_inLayout = false;
}

//
// ApplyBar — show one standard scroll bar with the given range/page/position,
//   or hide it when the axis fits.
//
void CScrollablePage::ApplyBar(int bar, bool show, int total, int page, int pos)
{
    if (!show)
    {
        ShowScrollBar(bar, FALSE);
        return;
    }

    // ShowScrollBar first: it adds WS_VSCROLL/WS_HSCROLL and recalculates the
    // non-client area, which SetScrollInfo alone would not do.
    ShowScrollBar(bar, TRUE);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = total > 0 ? total - 1 : 0;
    si.nPage  = page > 0 ? static_cast<UINT>(page) : 1;
    si.nPos   = pos;
    SetScrollInfo(bar, &si, TRUE);
}

int CScrollablePage::MaxScrollX() const
{
    return m_hasH ? std::max<int>(0, m_content.cx - m_viewport.cx) : 0;
}

int CScrollablePage::MaxScrollY() const
{
    return m_hasV ? std::max<int>(0, m_content.cy - m_viewport.cy) : 0;
}

//
// ScrollTo — move every child by the delta and repaint. SW_SCROLLCHILDREN does
//   the moving; the follow-up redraw is because group boxes and owner-draw
//   buttons leave artifacts behind a partial blit.
//
void CScrollablePage::ScrollTo(int x, int y)
{
    const int dx = m_scrollPos.x - x;
    const int dy = m_scrollPos.y - y;
    if (dx == 0 && dy == 0) return;

    m_scrollPos.x = x;
    m_scrollPos.y = y;

    ScrollWindowEx(dx, dy, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    RedrawWindow(nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);

    if (m_hasV) SetScrollPos(SB_VERT, m_scrollPos.y, TRUE);
    if (m_hasH) SetScrollPos(SB_HORZ, m_scrollPos.x, TRUE);
}

//
// OnVScroll — drive the page's own vertical bar. Notifications carrying a
//   control pointer belong to a child scroll bar (the Plays accordion, the
//   Preview camera strip), so those go straight to the base class.
//
void CScrollablePage::OnVScroll(UINT code, UINT pos, CScrollBar* sb)
{
    if (sb != nullptr || !m_autoScroll || !m_hasV)
    {
        CDialogEx::OnVScroll(code, pos, sb);
        return;
    }

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_TRACKPOS;
    GetScrollInfo(SB_VERT, &si);

    int p = m_scrollPos.y;
    switch (code)
    {
        case SB_LINEUP:        p -= kLineStepPx;     break;
        case SB_LINEDOWN:      p += kLineStepPx;     break;
        case SB_PAGEUP:        p -= m_viewport.cy;   break;
        case SB_PAGEDOWN:      p += m_viewport.cy;   break;
        case SB_TOP:           p  = 0;               break;
        case SB_BOTTOM:        p  = MaxScrollY();    break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: p  = si.nTrackPos;    break;
        default: return;
    }

    ScrollTo(m_scrollPos.x, std::clamp(p, 0, MaxScrollY()));
}

//
// OnHScroll — same as OnVScroll for the horizontal bar; slider and child
//   scroll-bar notifications (which always carry a control) fall through.
//
void CScrollablePage::OnHScroll(UINT code, UINT pos, CScrollBar* sb)
{
    if (sb != nullptr || !m_autoScroll || !m_hasH)
    {
        CDialogEx::OnHScroll(code, pos, sb);
        return;
    }

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_TRACKPOS;
    GetScrollInfo(SB_HORZ, &si);

    int p = m_scrollPos.x;
    switch (code)
    {
        case SB_LINELEFT:      p -= kLineStepPx;     break;
        case SB_LINERIGHT:     p += kLineStepPx;     break;
        case SB_PAGELEFT:      p -= m_viewport.cx;   break;
        case SB_PAGERIGHT:     p += m_viewport.cx;   break;
        case SB_LEFT:          p  = 0;               break;
        case SB_RIGHT:         p  = MaxScrollX();    break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: p  = si.nTrackPos;    break;
        default: return;
    }

    ScrollTo(std::clamp(p, 0, MaxScrollX()), m_scrollPos.y);
}

//
// OnMouseWheel — wheel scrolls the page vertically when it has a bar; anything
//   else (a canvas that zooms on the wheel handles it before we ever see it)
//   goes to the base class.
//
BOOL CScrollablePage::OnMouseWheel(UINT flags, short zDelta, CPoint pt)
{
    if (!m_autoScroll || !m_hasV)
        return CDialogEx::OnMouseWheel(flags, zDelta, pt);

    UINT lines = 3;
    ::SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    int step = (lines == WHEEL_PAGESCROLL)
                   ? static_cast<int>(m_viewport.cy)
                   : static_cast<int>(lines) * kLineStepPx;
    if (step <= 0) step = kLineStepPx;

    const int delta = -zDelta * step / WHEEL_DELTA;
    ScrollTo(m_scrollPos.x,
             std::clamp<int>(m_scrollPos.y + delta, 0, MaxScrollY()));
    return TRUE;
}

//
// PreTranslateMessage — keyboard navigation can hand the focus to a control
//   that is currently scrolled out of sight. The dialog manager hasn't moved
//   the focus yet at this point, so queue the check for afterwards.
//
BOOL CScrollablePage::PreTranslateMessage(MSG* pMsg)
{
    if (m_autoScroll && (m_hasV || m_hasH) && pMsg && pMsg->message == WM_KEYDOWN)
    {
        switch (pMsg->wParam)
        {
            case VK_TAB:
            case VK_UP:
            case VK_DOWN:
            case VK_LEFT:
            case VK_RIGHT:
                PostMessage(WM_APP_FOLLOWFOCUS);
                break;
            default: break;
        }
    }
    return CDialogEx::PreTranslateMessage(pMsg);
}

//
// OnFollowFocus — bring whatever now has the focus back into view.
//
LRESULT CScrollablePage::OnFollowFocus(WPARAM, LPARAM)
{
    CWnd* focus = GetFocus();
    if (focus && ::IsChild(GetSafeHwnd(), focus->GetSafeHwnd()))
        EnsureVisible(focus);
    return 0;
}

//
// EnsureControlVisible / EnsureVisible — scroll the smallest amount that brings
//   the control fully into the viewport (top/left-aligned if it is taller or
//   wider than the viewport itself).
//
void CScrollablePage::EnsureControlVisible(UINT controlId)
{
    EnsureVisible(GetDlgItem(controlId));
}

void CScrollablePage::EnsureVisible(CWnd* ctrl)
{
    if (!m_autoScroll || (!m_hasV && !m_hasH)) return;
    if (!ctrl || !::IsWindow(ctrl->GetSafeHwnd())) return;

    CRect rc;
    ctrl->GetWindowRect(&rc);
    ScreenToClient(&rc);

    int x = m_scrollPos.x;
    int y = m_scrollPos.y;
    if (rc.bottom > m_viewport.cy) y += rc.bottom - m_viewport.cy;
    if (rc.top    < 0)             y += rc.top;
    if (rc.right  > m_viewport.cx) x += rc.right - m_viewport.cx;
    if (rc.left   < 0)             x += rc.left;

    ScrollTo(std::clamp(x, 0, MaxScrollX()), std::clamp(y, 0, MaxScrollY()));
}

//
// ClampRectToMonitorWorkArea — see header. Sizes down first, then slides the
// rectangle back inside the work area so the caption stays grabbable.
//
void ClampRectToMonitorWorkArea(CRect& rc)
{
    RECT r = rc;
    HMONITOR hMon = ::MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!hMon || !::GetMonitorInfo(hMon, &mi))
        return;

    const CRect work(mi.rcWork);
    int w = std::min<int>(rc.Width(),  work.Width());
    int h = std::min<int>(rc.Height(), work.Height());
    int x = rc.left;
    int y = rc.top;

    if (x + w > work.right)  x = work.right  - w;
    if (y + h > work.bottom) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top)  y = work.top;

    rc.SetRect(x, y, x + w, y + h);
}

//
// FitWindowToMonitorWorkArea — see header.
//
void FitWindowToMonitorWorkArea(CWnd* wnd, bool center)
{
    if (!wnd || !::IsWindow(wnd->GetSafeHwnd())) return;

    CRect rc;
    wnd->GetWindowRect(&rc);
    const CRect before(rc);
    ClampRectToMonitorWorkArea(rc);

    if (center)
    {
        RECT r = rc;
        HMONITOR hMon = ::MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (hMon && ::GetMonitorInfo(hMon, &mi))
        {
            const CRect work(mi.rcWork);
            rc.MoveToXY(work.left + (work.Width()  - rc.Width())  / 2,
                        work.top  + (work.Height() - rc.Height()) / 2);
        }
    }

    if (rc != before)
        wnd->SetWindowPos(nullptr, rc.left, rc.top, rc.Width(), rc.Height(),
                          SWP_NOZORDER | SWP_NOACTIVATE);
}
