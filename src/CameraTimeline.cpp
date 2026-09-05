//=============================================================================
//  CameraTimeline.cpp
//-----------------------------------------------------------------------------
//  Implements CCameraTimeline: double-buffered GDI painting of the camera
//  schedule as translucent labeled boxes over a time axis, plus mouse handling
//  for scrub (click), move (drag body of an interior frame), and resize (drag a
//  frame edge). Move/resize edit the shared boundary between neighbors so the
//  frames stay contiguous with no gaps or overlaps. All model access goes
//  through the CPreviewPage owner.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#include "pch.h"
#include "CameraTimeline.h"
#include "PreviewPage.h"
#include "Scenario.h"
#include "FormatUtil.h"      // FormatDurationCompact for the per-box duration line

#include <algorithm>
#include <cstdlib>
#include <string>

namespace
{
    constexpr int    kMarginPx    = 4;    // left/right padding inside the strip
    constexpr int    kEdgePx      = 4;    // resize grab zone at each box edge
    constexpr int    kDragSlopPx  = 3;    // move beyond this before it's a drag
    constexpr double kMinFrameSec = 1.0;  // a frame can't shrink below this

    // Blend foreground over background by alpha a in [0,1] (per channel).
    COLORREF Blend(COLORREF bg, COLORREF fg, double a)
    {
        auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
        const double ia = 1.0 - a;
        const int r = static_cast<int>(GetRValue(bg) * ia + GetRValue(fg) * a);
        const int g = static_cast<int>(GetGValue(bg) * ia + GetGValue(fg) * a);
        const int b = static_cast<int>(GetBValue(bg) * ia + GetBValue(fg) * a);
        return RGB(cl(r), cl(g), cl(b));
    }
}

BEGIN_MESSAGE_MAP(CCameraTimeline, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_LBUTTONDOWN()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_RBUTTONUP()
    ON_WM_SETCURSOR()
    ON_WM_NCHITTEST()
END_MESSAGE_MAP()

//
// OnNcHitTest — a subclassed STATIC returns HTTRANSPARENT by default, which
//   passes clicks through to the parent. Return HTCLIENT so the strip receives
//   its own mouse messages (same trick as CPreviewCanvas).
//
LRESULT CCameraTimeline::OnNcHitTest(CPoint)
{
    return HTCLIENT;
}

//
// TimeToX / XToTime — map scenario seconds to/from a client-x pixel. The mapping
//   is a fixed pixels-per-second (set by the time scale) minus the horizontal
//   scroll offset, so a fixed on-screen distance always represents `scale`
//   seconds and box widths scale with zoom (the `w` argument is now unused).
//
double CCameraTimeline::TimeToX(double t, int /*w*/) const
{
    return kMarginPx + t * PxPerSecond() - (double)m_scrollPx;
}

double CCameraTimeline::XToTime(double x, int /*w*/) const
{
    const double pps = PxPerSecond();
    double t = (pps > 0.0) ? (x - kMarginPx + (double)m_scrollPx) / pps : 0.0;
    if (t < 0.0) t = 0.0;
    const double D = m_owner ? m_owner->GetPreviewDuration() : 0.0;
    if (D > 0.0 && t > D) t = D;
    return t;
}

//
// ContentWidthPx — total pixel width the schedule spans at the current scale
//   ([0, duration] plus the two margins). Drives the owner's scroll-bar range.
//
int CCameraTimeline::ContentWidthPx() const
{
    double D = m_owner ? m_owner->GetPreviewDuration() : 0.0;
    if (D < 0.0) D = 0.0;
    return (int)(D * PxPerSecond() + 2.0 * kMarginPx + 0.5);
}

//
// VisibleSpanSeconds — the time span the strip currently shows: its drawable
//   pixel width (client minus margins) divided by the pixels-per-second scale.
//
double CCameraTimeline::VisibleSpanSeconds() const
{
    if (!GetSafeHwnd()) return 0.0;
    CRect rc; GetClientRect(&rc);
    const double drawable = (double)std::max<LONG>(rc.Width() - 2 * kMarginPx, 1);
    const double pps = PxPerSecond();
    return pps > 0.0 ? drawable / pps : 0.0;
}

//
// HitTest — return the frame index under client-x `px` and, via outZone, whether
//   the cursor is on the left/right resize edge (only for a shared boundary) or
//   the body. Returns -1 with Zone::None when nothing is under the cursor.
//
int CCameraTimeline::HitTest(int px, int w, Zone& outZone) const
{
    outZone = Zone::None;
    const std::vector<CameraFrame>* cams = m_owner ? m_owner->GetCameras() : nullptr;
    if (!cams || cams->empty()) return -1;

    // Boxes are free-floating and may overlap; scan topmost-first (later-drawn
    // sits on top) so the visible box gets the hit. Every box is fully editable:
    // either edge resizes it, its interior moves it.
    for (size_t r = cams->size(); r-- > 0; )
    {
        const int xb = (int)(TimeToX((*cams)[r].beginSecond, w) + 0.5);
        const int xe = (int)(TimeToX((*cams)[r].endSecond,   w) + 0.5);
        if (px < xb || px > xe) continue;

        if      (px <= xb + kEdgePx) outZone = Zone::ResizeL;
        else if (px >= xe - kEdgePx) outZone = Zone::ResizeR;
        else                         outZone = Zone::Body;
        return (int)r;
    }
    return -1;
}

//
// OnEraseBkgnd — suppress default erase; OnPaint fully repaints (no flicker).
//
BOOL CCameraTimeline::OnEraseBkgnd(CDC*) { return TRUE; }

//
// OnPaint — double-buffered render of the background, each frame box (translucent
//   fill + border + clipped label over a duration line), and the playhead at
//   the current time.
//
void CCameraTimeline::OnPaint()
{
    CPaintDC dc(this);
    CRect rc; GetClientRect(&rc);
    const int w = rc.Width(), h = rc.Height();
    if (w <= 0 || h <= 0) return;

    // Off-screen buffer.
    CDC mem; mem.CreateCompatibleDC(&dc);
    CBitmap bmp; bmp.CreateCompatibleBitmap(&dc, w, h);
    CBitmap* oldBmp = mem.SelectObject(&bmp);

    const COLORREF kBg     = RGB(28, 28, 32);
    const COLORREF kEntity = RGB(70, 130, 220);
    const COLORREF kStat   = RGB(220, 90, 180);
    const COLORREF kBorder = RGB(200, 200, 210);
    const COLORREF kText   = RGB(240, 240, 245);
    const COLORREF kPlay   = RGB(255, 80, 80);

    mem.FillSolidRect(&rc, kBg);

    const std::vector<CameraFrame>* cams = m_owner ? m_owner->GetCameras() : nullptr;

    if (cams && !cams->empty())
    {
        const int boxTop = 2, boxBot = h - 2;
        mem.SetBkMode(TRANSPARENT);
        // Compact GUI font (not the dialog's large 12pt) so the strip stays short.
        CFont* oldFont = mem.SelectObject(CFont::FromHandle(
            (HFONT)::GetStockObject(DEFAULT_GUI_FONT)));

        // One metrics query for the whole strip: the box text is two stacked
        // lines (label over duration) and DrawText won't lay that out for us.
        TEXTMETRIC tm{};
        mem.GetTextMetrics(&tm);
        const int lineH = (int)tm.tmHeight;

        // Durations are reported as the wall-clock seconds the view is actually
        // up, i.e. the scenario span divided by the preview playback rate — at
        // 10x a 16.9 s frame is only on screen for about 2 s.
        const double spd = m_owner ? m_owner->GetPlaySpeed() : 1.0;

        for (size_t i = 0; i < cams->size(); ++i)
        {
            const CameraFrame& c = (*cams)[i];
            int xb = (int)(TimeToX(c.beginSecond, w) + 0.5);
            int xe = (int)(TimeToX(c.endSecond,   w) + 0.5);
            if (xe <= xb) xe = xb + 1;
            CRect box(xb, boxTop, xe, boxBot);

            const COLORREF base = (c.kind == CameraKind::Stationary) ? kStat : kEntity;
            // Slight per-index lightness alternation so equal-kind neighbors read apart.
            const double alpha = (i % 2 == 0) ? 0.55 : 0.42;
            mem.FillSolidRect(&box, Blend(kBg, base, alpha));

            // Border.
            CBrush borderBrush(kBorder);
            mem.FrameRect(&box, &borderBrush);

            // Text, clipped inside the box with a small inset: the label on top
            // and the duration under it. Two lines only when the strip is tall
            // enough for both — otherwise fall back to the single centred label
            // rather than clipping one of them.
            CRect textRc = box; textRc.DeflateRect(4, 0);
            if (textRc.Width() > 6)
            {
                mem.SetTextColor(kText);
                CString label(c.label.c_str());
                const UINT kFlags = DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;

                if (lineH > 0 && textRc.Height() >= 2 * lineH)
                {
                    // Centre the pair as a block, then stack label over duration.
                    const int top = textRc.top + (textRc.Height() - 2 * lineH) / 2;
                    CRect r1(textRc.left, top,         textRc.right, top + lineH);
                    CRect r2(textRc.left, top + lineH, textRc.right, top + 2 * lineH);

                    mem.DrawText(label, &r1, kFlags | DT_TOP);

                    const double span = c.endSecond - c.beginSecond;
                    mem.DrawText(FormatDurationCompact((span > 0.0 ? span : 0.0) / spd),
                                 &r2, kFlags | DT_TOP);
                }
                else
                {
                    mem.DrawText(label, &textRc, kFlags | DT_VCENTER);
                }
            }
        }
        mem.SelectObject(oldFont);

        // Playhead.
        if (m_owner)
        {
            const int px = (int)(TimeToX(m_owner->GetPreviewTime(), w) + 0.5);
            CPen pen(PS_SOLID, 1, kPlay);
            CPen* oldPen = mem.SelectObject(&pen);
            mem.MoveTo(px, 0); mem.LineTo(px, h);
            mem.SelectObject(oldPen);
        }
    }
    else
    {
        // Empty schedule hint.
        mem.SetBkMode(TRANSPARENT);
        mem.SetTextColor(RGB(150, 150, 158));
        CFont* oldFont = mem.SelectObject(CFont::FromHandle(
            (HFONT)::GetStockObject(DEFAULT_GUI_FONT)));
        CRect tr = rc;
        mem.DrawText(_T("No cameras — use \"New Cam\" or the Add row above"),
                     &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        mem.SelectObject(oldFont);
    }

    dc.BitBlt(0, 0, w, h, &mem, 0, 0, SRCCOPY);
    mem.SelectObject(oldBmp);
}

//
// OnLButtonDown — capture and record the frame/zone under the cursor plus a
//   snapshot of all frame times, so the following moves apply cleanly.
//
void CCameraTimeline::OnLButtonDown(UINT, CPoint pt)
{
    CRect rc; GetClientRect(&rc);
    Zone zone = Zone::None;
    const int frame = HitTest(pt.x, rc.Width(), zone);

    m_capturing  = true;
    m_dragging   = false;
    m_dragFrame  = frame;
    m_dragZone   = zone;
    m_dragStartX = pt.x;

    m_snapshot.clear();
    if (const std::vector<CameraFrame>* cams = m_owner ? m_owner->GetCameras() : nullptr)
        for (const CameraFrame& c : *cams)
            m_snapshot.emplace_back(c.beginSecond, c.endSecond);

    SetCapture();
}

//
// OnMouseMove — once past the drag threshold, apply a move (interior body) or a
//   resize (edge) by shifting the affected shared boundary, keeping the tiling.
//
void CCameraTimeline::OnMouseMove(UINT, CPoint pt)
{
    if (!m_capturing || !m_owner) return;

    if (!m_dragging && abs(pt.x - m_dragStartX) < kDragSlopPx)
        return;                       // still might be a click

    std::vector<CameraFrame>* cams = m_owner->MutableCameras();
    if (!cams || m_dragFrame < 0 || (size_t)m_dragFrame >= cams->size() ||
        m_snapshot.size() != cams->size())
        return;

    CRect rc; GetClientRect(&rc);
    const int w = rc.Width();
    double D = m_owner->GetPreviewDuration();
    if (D <= 0.0) D = 1.0;
    const double dt = XToTime(pt.x, w) - XToTime(m_dragStartX, w);
    const size_t i = (size_t)m_dragFrame;
    const size_t n = m_snapshot.size();
    const double b0 = m_snapshot[i].first;    // this box's begin/end at drag start
    const double e0 = m_snapshot[i].second;

    // Boxes are begin-sorted at drag start and only box `i` moves. A RESIZE drag
    // clamps against the fixed neighbors so an edge never eats into an adjacent
    // box. A BODY drag does NOT: the box is free to slide over/past its neighbors
    // so the operator can reorder the schedule (carry a later box in front of an
    // earlier one). The overlap that creates while dragging is resolved on drop —
    // OnLButtonUp -> OnTimelineEdited -> NormalizeCameraSchedule re-sorts by start
    // time and shoves the boxes apart, so the drop order becomes the new order.
    const double leftLimit  = (i > 0)     ? m_snapshot[i - 1].second : 0.0;
    const double rightLimit = (i + 1 < n) ? m_snapshot[i + 1].first  : D;

    if (m_dragZone == Zone::ResizeL)
    {
        double b = b0 + dt;
        b = std::max(0.0, std::max(leftLimit, b));   // don't cross the left neighbor
        b = std::min(b, e0 - kMinFrameSec);
        (*cams)[i].beginSecond = b;
        m_dragging = true;
    }
    else if (m_dragZone == Zone::ResizeR)
    {
        double e = e0 + dt;
        e = std::min(D, std::min(rightLimit, e));    // don't cross the right neighbor
        e = std::max(e, b0 + kMinFrameSec);
        (*cams)[i].endSecond = e;
        m_dragging = true;
    }
    else if (m_dragZone == Zone::Body)
    {
        // Free horizontal move across the whole timeline; neighbors are not a
        // barrier (that's what lets boxes be reordered). Keep only the whole box
        // within [0, D]; the drop de-overlaps and re-sorts.
        const double width = e0 - b0;
        double hi = D - width;
        if (hi < 0.0) hi = 0.0;
        double b = std::max(0.0, std::min(hi, b0 + dt));
        (*cams)[i].beginSecond = b;
        (*cams)[i].endSecond   = b + width;
        m_dragging = true;
    }

    if (m_dragging)
        m_owner->OnTimelineFrameEditing();   // live refresh (canvas + this strip)
}

//
// OnLButtonUp — commit a drag (normalize + mark dirty) or, if the pointer never
//   moved far, treat it as a scrub to the clicked time.
//
void CCameraTimeline::OnLButtonUp(UINT, CPoint pt)
{
    if (!m_capturing) return;
    m_capturing = false;
    ReleaseCapture();

    if (m_dragging)
    {
        // A move can carry the box past its neighbors — reorder the schedule to
        // match where it was dropped before the owner normalizes (sort + de-overlap).
        if (m_dragZone == Zone::Body) ReindexDraggedFrame();
        if (m_owner) m_owner->OnTimelineEdited();
    }
    else if (m_owner)
    {
        CRect rc; GetClientRect(&rc);
        m_owner->ScrubToTime(XToTime(pt.x, rc.Width()));
    }

    m_dragging  = false;
    m_dragFrame = -1;
    m_dragZone  = Zone::None;
    m_snapshot.clear();
}

//
// ReindexDraggedFrame — move the just-dragged frame to the vector slot implied by
//   its new start time. Target index = count of OTHER frames that start strictly
//   earlier, so equal-start ties place the moved frame FIRST among them (lets a
//   full left drag reach index 0). NormalizeCameraSchedule then stable-sorts by
//   start and de-overlaps, preserving this order for the tie group.
//
void CCameraTimeline::ReindexDraggedFrame()
{
    std::vector<CameraFrame>* cams = m_owner ? m_owner->MutableCameras() : nullptr;
    if (!cams || m_dragFrame < 0 || (size_t)m_dragFrame >= cams->size()) return;

    const size_t i = (size_t)m_dragFrame;
    const double b = (*cams)[i].beginSecond;

    size_t target = 0;
    for (size_t k = 0; k < cams->size(); ++k)
        if (k != i && (*cams)[k].beginSecond < b) ++target;

    if (target == i) return;   // already in place
    CameraFrame moved = std::move((*cams)[i]);
    cams->erase(cams->begin() + i);
    cams->insert(cams->begin() + target, std::move(moved));
}

//
// OnRButtonUp — right-click a camera box to pop its Edit + Delete menu. This is
//   the only route to an Entity camera's settings after creation: Entity frames
//   have no canvas box, so the canvas menu cannot reach them.
//
void CCameraTimeline::OnRButtonUp(UINT, CPoint pt)
{
    if (!m_owner) return;
    CRect rc; GetClientRect(&rc);
    Zone zone = Zone::None;
    const int frame = HitTest(pt.x, rc.Width(), zone);
    if (frame < 0) return;   // not on a box

    const std::vector<CameraFrame>* cams = m_owner->GetCameras();
    if (!cams || static_cast<size_t>(frame) >= cams->size()) return;

    // Note there is deliberately NO CameraFrame& held here. TrackPopupMenu runs its
    // own message loop and "Edit Camera..." opens a modal on top of it, so a
    // reference into Scenario::cameras could not safely survive either. Only the
    // index travels, and EditCameraViaDialog re-validates it.
    enum { kEditCamera = 1, kDeleteCamera };

    CMenu menu;
    menu.CreatePopupMenu();
    menu.AppendMenu(MF_STRING, kEditCamera,   _T("Edit Camera..."));
    menu.AppendMenu(MF_SEPARATOR, 0, static_cast<LPCTSTR>(nullptr));
    menu.AppendMenu(MF_STRING, kDeleteCamera, _T("Delete Camera"));

    CPoint screen = pt;
    ClientToScreen(&screen);
    SetForegroundWindow();   // so the menu dismisses correctly on click-away
    const UINT cmd = menu.TrackPopupMenu(
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON, screen.x, screen.y, this);

    if (cmd == kEditCamera)
        m_owner->EditCameraViaDialog(static_cast<size_t>(frame));
    else if (cmd == kDeleteCamera)
        m_owner->DeleteCamera(static_cast<size_t>(frame));
}

//
// OnSetCursor — show a horizontal-resize cursor over an editable edge, a move
//   cursor over an interior body, else the default arrow.
//
BOOL CCameraTimeline::OnSetCursor(CWnd*, UINT, UINT)
{
    CPoint pt; ::GetCursorPos(&pt); ScreenToClient(&pt);
    CRect rc; GetClientRect(&rc);
    Zone zone = Zone::None;
    const int frame = HitTest(pt.x, rc.Width(), zone);

    (void)frame;
    if (zone == Zone::ResizeL || zone == Zone::ResizeR)
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEWE));   // resize either edge
    else if (zone == Zone::Body)
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));  // move any box
    else
        ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
    return TRUE;
}
