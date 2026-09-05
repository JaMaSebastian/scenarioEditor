//=============================================================================
//  CameraTimeline.h
//-----------------------------------------------------------------------------
//  Declares CCameraTimeline, the owner-drawn strip beneath the Preview canvas
//  that visualizes the scenario's camera schedule as translucent boxes along a
//  time axis, each showing its camera label over the wall-clock seconds that
//  view is up at the current preview speed. Boxes tile [0, duration]; the user
//  drags a box body to move it (interior frames) and drags a box edge to resize
//  it — edits move the shared boundary so the contiguous tiling is preserved. A
//  click scrubs the playback time. The control reads/edits its data through its
//  CPreviewPage owner; it holds no scenario state of its own.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#pragma once

#include <afxwin.h>
#include <vector>
#include <utility>

class CPreviewPage;

//-----------------------------------------------------------------------------
// CCameraTimeline — GDI double-buffered timeline strip (subclassed onto a
//   static placeholder). Delegates all model access/mutation to its owner.
//-----------------------------------------------------------------------------
class CCameraTimeline : public CWnd
{
public:
    void SetOwner(CPreviewPage* owner) { m_owner = owner; }

    // ----- Time scale (zoom) + horizontal scroll -----
    // The strip no longer fits the whole duration to its width; instead a fixed
    // on-screen reference distance represents `scaleSeconds` of time, so a box of
    // a given duration grows/shrinks as the scale changes (1s -> 100ms = 10x
    // wider). Content wider than the client is panned by the owner's scroll bar.
    static constexpr double kRefPx = 80.0;   // pixels that represent one scale unit

    void   SetTimeScale(double scaleSeconds)          // seconds per kRefPx pixels
    {
        if (scaleSeconds > 1e-9) m_scaleSeconds = scaleSeconds;
    }
    double GetTimeScale()  const { return m_scaleSeconds; }
    double PxPerSecond()   const { return kRefPx / m_scaleSeconds; }

    void   SetScrollOffset(int px) { m_scrollPx = px < 0 ? 0 : px; }
    int    GetScrollOffset() const { return m_scrollPx; }

    // Full pixel width the schedule needs at the current scale (0..duration).
    int    ContentWidthPx() const;

    // Seconds currently visible across the strip's drawable width at this scale
    // (i.e. how much time the control shows right now). 0 if not yet realized.
    double VisibleSpanSeconds() const;

protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint pt);
    afx_msg void OnMouseMove(UINT nFlags, CPoint pt);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint pt);
    afx_msg void OnRButtonUp(UINT nFlags, CPoint pt);   // "Delete Camera" popup
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg LRESULT OnNcHitTest(CPoint pt);   // HTCLIENT so the static gets the mouse
    DECLARE_MESSAGE_MAP()

private:
    enum class Zone { None, Body, ResizeL, ResizeR };

    // Map time <-> pixel x over the current client width.
    double TimeToX(double t, int w) const;
    double XToTime(double x, int w) const;

    // Which frame (and where within it) is under a client-x pixel.
    int  HitTest(int px, int w, Zone& outZone) const;

    // After a body (move) drag, slide the dragged frame to the vector position
    // implied by its new start time, so a box carried left/right of its neighbors
    // reorders the schedule. Ties resolve toward the moved box so a full left drag
    // can reach index 0 (become the first frame). No-op for resize drags.
    void ReindexDraggedFrame();

    CPreviewPage* m_owner = nullptr;

    // Zoom + pan state.
    double m_scaleSeconds = 1.0;   // seconds represented by kRefPx pixels
    int    m_scrollPx     = 0;     // horizontal scroll offset (pixels)

    // Drag state.
    bool  m_capturing = false;   // mouse captured since LButtonDown
    bool  m_dragging  = false;   // moved far enough to count as a drag (vs click)
    int   m_dragFrame = -1;
    Zone  m_dragZone  = Zone::None;
    int   m_dragStartX = 0;
    // Snapshot of every frame's [begin,end] at drag start, so incremental
    // application never drifts.
    std::vector<std::pair<double, double>> m_snapshot;
};
