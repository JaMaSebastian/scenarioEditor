//=============================================================================
//  PreviewCanvas.cpp
//-----------------------------------------------------------------------------
//  Implements CPreviewCanvas: the Direct2D rendering of the scenario preview
//  (map backdrop, terrain overlay, legend, courses/trails, entity dots and
//  edit handles) and all mouse handling — pan/zoom, entity and ellipse/line
//  handle dragging, rubber-band group selection, boundary painting, and the
//  right-click context menus that drive the owning CPreviewPage.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-25
//=============================================================================
#include "pch.h"
#include "PreviewCanvas.h"
#include "PreviewPage.h"
#include "Scenario.h"        // CameraTransition / CameraFrame (camera RMB menu)
#include "Direct2DContext.h"
#include "CoordTransforms.h"
#include "ScenarioEditor.h"   // theApp.SettingsPath() -> tile cache dir
#include "../log.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr float kEntityDotRadiusPx = 5.0f;
    constexpr float kAnchorDotRadiusPx = 6.0f;  // pink Line end-anchor drag handle
    constexpr float kOrientationLenPx  = 18.0f;
    constexpr float kLabelOffsetPx     = 9.0f;
    constexpr float kCameraBoxHalfPx   = 7.0f;  // pink camera vantage square (half-extent)

    //
    // MakeRgb — build a Direct2D color from a 0xRRGGBB packed int (+ optional alpha).
    //
    D2D1_COLOR_F MakeRgb(uint32_t rgb, float a = 1.0f)
    {
        const float r = ((rgb >> 16) & 0xFF) / 255.0f;
        const float g = ((rgb >>  8) & 0xFF) / 255.0f;
        const float b = ( rgb        & 0xFF) / 255.0f;
        return D2D1::ColorF(r, g, b, a);
    }
}

BEGIN_MESSAGE_MAP(CPreviewCanvas, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_SIZE()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_MOUSEWHEEL()
    ON_WM_RBUTTONDOWN()
    ON_WM_RBUTTONUP()
    ON_WM_NCHITTEST()
    ON_WM_DESTROY()
    ON_MESSAGE(WM_APP_TILE_READY, &CPreviewCanvas::OnTileReady)
END_MESSAGE_MAP()

// We're subclassing a STATIC control, which by default returns HTTRANSPARENT
// from its hit test — that causes Windows to deliver WM_LBUTTONDOWN /
// WM_MOUSEWHEEL etc. to the parent dialog instead of to us. Override the
// hit test to claim the client area so the pan / zoom handlers receive
// their messages.
LRESULT CPreviewCanvas::OnNcHitTest(CPoint /*pt*/)
{
    return HTCLIENT;
}

//
// OnLButtonDown — begin the appropriate left-button action for what's under the
//   cursor, in priority order: "Set Start" pick, line end-anchor drag, ellipse
//   focus-handle drag, ellipse shape-handle drag, entity drag, else a view pan.
//
void CPreviewCanvas::OnLButtonDown(UINT nFlags, CPoint pt)
{
    SetFocus();           // so the wheel comes to us if the user clicks first

    // "Set Start" pick mode: hit-test the click against ellipse orbits here,
    // in the same projection we draw with, then record the start instead of
    // starting a pan.
    if (m_owner && m_owner->IsStartPickArmed() && PickStartAt(pt))
    {
        CWnd::OnLButtonDown(nFlags, pt);
        return;
    }

    // Line end-anchor drag: grabbing the pink destination dot moves the course's
    // end. Takes priority over entity drag / pan (the dot sits on top).
    if (m_owner && !m_owner->IsStartPickArmed())
    {
        const int a = HitTestLineAnchor(pt);
        if (a >= 0)
        {
            const PreviewRenderState& s = m_owner->GetRenderState();
            m_draggingAnchor   = true;
            m_dragAnchorEntity = static_cast<int>(s.lineAnchors[a].entityIdx);
            m_dragAnchorSeg    = static_cast<int>(s.lineAnchors[a].segIdx);
            SetCapture();
            ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
            CWnd::OnLButtonDown(nFlags, pt);
            return;
        }
    }

    // Ellipse focus-handle drag (sits on top of the entity dot).
    if (m_owner && !m_owner->IsStartPickArmed())
    {
        const int fh = HitTestFocus(pt);
        if (fh >= 0)
        {
            const PreviewRenderState& s = m_owner->GetRenderState();
            m_draggingFocus   = true;
            m_dragFocusEntity = static_cast<int>(s.focusHandles[fh].entityIdx);
            m_dragFocusSeg    = static_cast<int>(s.focusHandles[fh].segIdx);
            m_dragFocusIdx    = s.focusHandles[fh].focusIdx;
            SetCapture();
            ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
            CWnd::OnLButtonDown(nFlags, pt);
            return;
        }
    }

    // Ellipse shape-handle drag (orange dot on the orbit curve). Re-sizes the
    // orbit. Sits at the orbit's start, so it takes priority over the entity dot
    // (which, for a stand-alone orbit, is in the same spot).
    if (m_owner && !m_owner->IsStartPickArmed())
    {
        const int sh = HitTestEllipseShape(pt);
        if (sh >= 0)
        {
            const PreviewRenderState& s = m_owner->GetRenderState();
            m_draggingShape   = true;
            m_dragShapeEntity = static_cast<int>(s.ellipses[sh].entityIdx);
            m_dragShapeSeg    = static_cast<int>(s.ellipses[sh].segIdx);
            SetCapture();
            ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
            CWnd::OnLButtonDown(nFlags, pt);
            return;
        }
    }

    // Stationary-camera box drag (pink square). Sits on top and is always
    // draggable (not gated on the t=0 entity-drag rule).
    if (m_owner && !m_owner->IsStartPickArmed())
    {
        const int c = HitTestCamera(pt);
        if (c >= 0)
        {
            const PreviewRenderState& s = m_owner->GetRenderState();
            m_draggingCamera  = true;
            m_dragCameraFrame = static_cast<int>(s.cameras[c].frameIdx);
            SetCapture();
            ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
            CWnd::OnLButtonDown(nFlags, pt);
            return;
        }
    }

    // Entity drag: when the preview is stopped at t=0, grabbing a dot relocates
    // its start location instead of panning the view.
    if (m_owner && !m_owner->IsStartPickArmed() && m_owner->IsEntityDragAllowed())
    {
        const int hit = HitTestEntity(pt);
        if (hit >= 0)
        {
            m_draggingEntity = true;
            m_dragEntityIdx  = hit;
            SetCapture();
            ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
            CWnd::OnLButtonDown(nFlags, pt);
            return;
        }
    }

    // Level-zone box drag (lowest priority — below entities/cameras/handles so a
    // dot on top of a zone still wins). Grab anywhere inside a visible zone box and
    // move the whole rectangle; the grab offset keeps it from jumping.
    if (m_owner && !m_owner->IsStartPickArmed())
    {
        const int zi = HitTestZone(pt);
        if (zi >= 0)
        {
            const PreviewRenderState& s = m_owner->GetRenderState();
            double e = 0.0, n = 0.0;
            if (UnprojectToEnu(pt, e, n) &&
                static_cast<size_t>(zi) < s.levelZones.size())
            {
                const PreviewLevelZone& z = s.levelZones[static_cast<size_t>(zi)];
                const double cE = 0.5 * (z.eastMinM  + z.eastMaxM);
                const double cN = 0.5 * (z.northMinM + z.northMaxM);
                m_draggingZone = true;
                m_dragZoneIdx  = zi;
                m_zoneGrabOffE = e - cE;   // grab point relative to zone center
                m_zoneGrabOffN = n - cN;
                SetCapture();
                ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
                CWnd::OnLButtonDown(nFlags, pt);
                return;
            }
        }
    }

    m_dragging   = true;
    m_lastDragPt = pt;
    SetCapture();
    CWnd::OnLButtonDown(nFlags, pt);
}

//
// OnLButtonUp — finish whichever left-drag is active: commit the final ENU
//   position to the owner (focus/shape/anchor/entity drag) or just end a pan,
//   then release capture.
//
void CPreviewCanvas::OnLButtonUp(UINT nFlags, CPoint pt)
{
    if (m_draggingFocus)
    {
        double e = 0.0, n = 0.0;
        if (m_owner && m_dragFocusEntity >= 0 && UnprojectToEnu(pt, e, n))
            m_owner->DragEllipseFocus(static_cast<size_t>(m_dragFocusEntity),
                                      static_cast<size_t>(m_dragFocusSeg), m_dragFocusIdx, e, n);
        m_draggingFocus   = false;
        m_dragFocusEntity = -1;
        m_dragFocusSeg    = -1;
        m_dragFocusIdx    = -1;
        ReleaseCapture();
        if (m_owner) m_owner->EndLineEndDrag();
    }
    else if (m_draggingShape)
    {
        double e = 0.0, n = 0.0;
        if (m_owner && m_dragShapeEntity >= 0 && UnprojectToEnu(pt, e, n))
            m_owner->DragEllipseShapeHandle(static_cast<size_t>(m_dragShapeEntity),
                                            static_cast<size_t>(m_dragShapeSeg), e, n);
        m_draggingShape   = false;
        m_dragShapeEntity = -1;
        m_dragShapeSeg    = -1;
        ReleaseCapture();
        if (m_owner) m_owner->EndLineEndDrag();
    }
    else if (m_draggingAnchor)
    {
        double e = 0.0, n = 0.0;
        if (m_owner && m_dragAnchorEntity >= 0 && UnprojectToEnu(pt, e, n))
            m_owner->DragLineEnd(static_cast<size_t>(m_dragAnchorEntity),
                                 static_cast<size_t>(m_dragAnchorSeg), e, n);
        m_draggingAnchor   = false;
        m_dragAnchorEntity = -1;
        m_dragAnchorSeg    = -1;
        ReleaseCapture();
        if (m_owner) m_owner->EndLineEndDrag();
    }
    else if (m_draggingEntity)
    {
        // Final position, then commit (reload other tabs + mark dirty).
        double e = 0.0, n = 0.0;
        if (m_owner && m_dragEntityIdx >= 0 && UnprojectToEnu(pt, e, n))
            m_owner->DragEntityTo(static_cast<size_t>(m_dragEntityIdx), e, n);
        m_draggingEntity = false;
        m_dragEntityIdx  = -1;
        ReleaseCapture();
        if (m_owner) m_owner->EndEntityDrag();
    }
    else if (m_draggingCamera)
    {
        double e = 0.0, n = 0.0;
        if (m_owner && m_dragCameraFrame >= 0 && UnprojectToEnu(pt, e, n))
            m_owner->DragCameraTo(static_cast<size_t>(m_dragCameraFrame), e, n);
        m_draggingCamera  = false;
        m_dragCameraFrame = -1;
        ReleaseCapture();
        if (m_owner) m_owner->EndCameraDrag();
    }
    else if (m_draggingZone)
    {
        double e = 0.0, n = 0.0;
        if (m_owner && m_dragZoneIdx >= 0 && UnprojectToEnu(pt, e, n))
            m_owner->DragZoneTo(static_cast<size_t>(m_dragZoneIdx),
                                e - m_zoneGrabOffE, n - m_zoneGrabOffN);
        m_draggingZone = false;
        m_dragZoneIdx  = -1;
        ReleaseCapture();
        if (m_owner) m_owner->EndZoneDrag();
    }
    else if (m_dragging)
    {
        m_dragging = false;
        ReleaseCapture();
    }
    CWnd::OnLButtonUp(nFlags, pt);
}

//
// OnMouseMove — live update of whatever gesture is in progress: boundary/rubber-
//   band box, focus/shape/anchor/entity drag (report the new ENU to the owner),
//   or panning the view by the pixel delta.
//
void CPreviewCanvas::OnMouseMove(UINT nFlags, CPoint pt)
{
    if (m_boundaryDragActive)
    {
        m_boundaryCur = pt;
        Invalidate(FALSE);
        CWnd::OnMouseMove(nFlags, pt);
        return;
    }
    if (m_rbActive)
    {
        m_rbCur = pt;
        const int dxp = pt.x - m_rbStart.x, dyp = pt.y - m_rbStart.y;
        if (dxp * dxp + dyp * dyp > 9) m_rbMoved = true;
        Invalidate(FALSE);
        CWnd::OnMouseMove(nFlags, pt);
        return;
    }
    if (m_draggingFocus && m_owner && m_dragFocusEntity >= 0)
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
        double e = 0.0, n = 0.0;
        if (UnprojectToEnu(pt, e, n))
            m_owner->DragEllipseFocus(static_cast<size_t>(m_dragFocusEntity),
                                      static_cast<size_t>(m_dragFocusSeg), m_dragFocusIdx, e, n);
    }
    else if (m_draggingShape && m_owner && m_dragShapeEntity >= 0)
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
        double e = 0.0, n = 0.0;
        if (UnprojectToEnu(pt, e, n))
            m_owner->DragEllipseShapeHandle(static_cast<size_t>(m_dragShapeEntity),
                                            static_cast<size_t>(m_dragShapeSeg), e, n);
    }
    else if (m_draggingAnchor && m_owner && m_dragAnchorEntity >= 0)
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
        double e = 0.0, n = 0.0;
        if (UnprojectToEnu(pt, e, n))
            m_owner->DragLineEnd(static_cast<size_t>(m_dragAnchorEntity),
                                 static_cast<size_t>(m_dragAnchorSeg), e, n);
    }
    else if (m_draggingEntity && m_owner && m_dragEntityIdx >= 0)
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
        double e = 0.0, n = 0.0;
        if (UnprojectToEnu(pt, e, n))
            m_owner->DragEntityTo(static_cast<size_t>(m_dragEntityIdx), e, n);
    }
    else if (m_draggingCamera && m_owner && m_dragCameraFrame >= 0)
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
        double e = 0.0, n = 0.0;
        if (UnprojectToEnu(pt, e, n))
            m_owner->DragCameraTo(static_cast<size_t>(m_dragCameraFrame), e, n);
    }
    else if (m_draggingZone && m_owner && m_dragZoneIdx >= 0)
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
        double e = 0.0, n = 0.0;
        if (UnprojectToEnu(pt, e, n))
            m_owner->DragZoneTo(static_cast<size_t>(m_dragZoneIdx),
                                e - m_zoneGrabOffE, n - m_zoneGrabOffN);
    }
    else if (m_dragging && m_owner)
    {
        const int dx = pt.x - m_lastDragPt.x;
        const int dy = pt.y - m_lastDragPt.y;
        m_lastDragPt = pt;
        if (dx != 0 || dy != 0) m_owner->PanByPixels(dx, dy);
    }
    CWnd::OnMouseMove(nFlags, pt);
}

//
// OnMouseWheel — zoom toward/away from the cursor. Forward = zoom in; 1.25x per
//   detent, applied about the client-space pixel under the pointer.
//
BOOL CPreviewCanvas::OnMouseWheel(UINT /*nFlags*/, short zDelta, CPoint screenPt)
{
    if (!m_owner) return FALSE;
    // Wheel forward (positive zDelta) = zoom IN (fewer meters per pixel).
    // 120 ticks per detent → 1.25x per detent feels right.
    const int detents = zDelta / WHEEL_DELTA;
    if (detents == 0) return TRUE;
    const int steps = (detents < 0) ? -detents : detents;
    double factor = 1.0;
    for (int i = 0; i < steps; ++i) factor *= 1.25;
    if (detents > 0) factor = 1.0 / factor;   // forward → divide

    // screenPt is in *screen* coords; OnMouseWheel doesn't pre-translate.
    CPoint client = screenPt;
    ScreenToClient(&client);
    m_owner->ZoomAtPixel(factor, client.x, client.y);
    return TRUE;
}

//
// OnRButtonDown — start a right-drag gesture: boundary paint (if armed), else a
//   rubber-band group selection when pressing on empty space. On an object the
//   menu is deferred to OnRButtonUp.
//
void CPreviewCanvas::OnRButtonDown(UINT nFlags, CPoint pt)
{
    SetFocus();

    // "Boundary" paint mode: the SAME right-drag gesture that mass-selects entities instead marks
    // the 3D-terrain box (no entity selection, no context menu). Armed via the Preview Boundary button.
    // Foliage-Area paint uses the SAME gesture; the two are mutually exclusive
    // at the page, so only one can be armed at a time.
    if (m_owner && (m_owner->IsBoundaryArmed() || m_owner->IsFoliageAreaArmed()))
    {
        m_boundaryDragActive = true;
        m_boundaryStart = pt;
        m_boundaryCur   = pt;
        SetCapture();
        ::SetCursor(::LoadCursor(nullptr, IDC_CROSS));
        CWnd::OnRButtonDown(nFlags, pt);
        return;
    }

    // Right-press on empty space begins a rubber-band group selection. On a
    // camera box, an entity dot, or a course line, do nothing here -- OnRButtonUp
    // shows the relevant context menu. (Cameras must be included or a right-press
    // on a camera arms the rubber-band and swallows its menu.)
    const bool onObject = (HitTestCamera(pt) >= 0) ||
                          (HitTestEntity(pt) >= 0) || (HitTestLineSegment(pt) >= 0);
    if (!onObject)
    {
        m_rbActive = true;
        m_rbMoved  = false;
        m_rbStart  = pt;
        m_rbCur    = pt;
        SetCapture();
    }
    CWnd::OnRButtonDown(nFlags, pt);
}

//
// OnRButtonUp — resolve the right-button gesture: commit a boundary box (→ lat/lon
//   corners to the owner), finish/clear a rubber-band selection, or pop the
//   appropriate context menu (course-line, group, or single-entity) and dispatch
//   the chosen command to the owner.
//
//
// ShowFoliageAreaMenu — right-click actions on a painted foliage area.
//   Returns true if a menu was shown (and the click consumed). Deliberately only
//   claims a right-click with NO drag: a right-DRAG across a forest must still
//   rubber-band select the entities inside it, which is why this is called from
//   the no-move path rather than from OnRButtonDown's object test. Areas are
//   large, and treating them as objects would kill group-select over any forest.
//
bool CPreviewCanvas::ShowFoliageAreaMenu(CPoint pt)
{
    const int folHit = HitTestFoliageArea(pt);
    if (folHit < 0 || !m_owner) return false;
    if (HitTestCamera(pt) >= 0 || HitTestEntity(pt) >= 0) return false;

    const PreviewRenderState& s = m_owner->GetRenderState();
    enum { kFolCount = 7100, kFolDelete, kFolDeleteAll };

    CString countItem;
    countItem.Format(_T("Set Tree Count (%lld)..."),
             s.foliageAreas[static_cast<size_t>(folHit)].treeCount);

    CMenu menu;
    menu.CreatePopupMenu();
    menu.AppendMenu(MF_STRING, kFolCount, countItem);
    menu.AppendMenu(MF_STRING, kFolDelete, _T("Delete Foliage Area"));
    if (s.foliageAreas.size() > 1)
    {
        menu.AppendMenu(MF_SEPARATOR, 0, static_cast<LPCTSTR>(nullptr));
        CString allItem;
        allItem.Format(_T("Delete All %d Foliage Areas"),
               static_cast<int>(s.foliageAreas.size()));
        menu.AppendMenu(MF_STRING, kFolDeleteAll, allItem);
    }

    CPoint sp = pt;
    ClientToScreen(&sp);
    SetForegroundWindow();
    const UINT cmd = menu.TrackPopupMenu(
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON, sp.x, sp.y, this);

    if      (cmd == kFolCount)     m_owner->SetFoliageAreaCount(static_cast<size_t>(folHit));
    else if (cmd == kFolDelete)    m_owner->DeleteFoliageArea(static_cast<size_t>(folHit));
    else if (cmd == kFolDeleteAll) m_owner->DeleteAllFoliageAreas();
    return true;
}

void CPreviewCanvas::OnRButtonUp(UINT nFlags, CPoint pt)
{
    // Finish a "Boundary" paint drag: convert the box → ENU → lat/lon corners → the page (which
    // records it + disarms so the yellow rectangle disappears). No context menu, no entity selection.
    if (m_boundaryDragActive)
    {
        m_boundaryDragActive = false;
        ReleaseCapture();
        double e0 = 0, n0 = 0, e1 = 0, n1 = 0;
        if (m_owner && UnprojectToEnu(m_boundaryStart, e0, n0) && UnprojectToEnu(m_boundaryCur, e1, n1))
        {
            const double eMin = (e0 < e1) ? e0 : e1, eMax = (e0 < e1) ? e1 : e0;
            const double nMin = (n0 < n1) ? n0 : n1, nMax = (n0 < n1) ? n1 : n0;
            // Ignore a click / hairline box (< 1 m on a side).
            if ((eMax - eMin) > 1.0 && (nMax - nMin) > 1.0)
            {
                const PreviewRenderState& s = m_owner->GetRenderState();
                auto enuToLatLon = [&](double e, double n, double& lat, double& lon)
                {
                    double X, Y, Z, alt;
                    CoordTransforms::LocalEnuToEcefDeg(e, n, 0.0,
                        s.originLatDeg, s.originLonDeg, s.originAltM, X, Y, Z);
                    CoordTransforms::EcefToGeodeticDeg(X, Y, Z, lat, lon, alt);
                };
                // Take the lat/lon bounding box over all 4 ENU corners (robust to any origin skew).
                const double corners[4][2] = { {eMin, nMin}, {eMin, nMax}, {eMax, nMin}, {eMax, nMax} };
                double latMin = 1e18, latMax = -1e18, lonMin = 1e18, lonMax = -1e18;
                for (int i = 0; i < 4; ++i)
                {
                    double la = 0, lo = 0;
                    enuToLatLon(corners[i][0], corners[i][1], la, lo);
                    if (la < latMin) latMin = la;  if (la > latMax) latMax = la;
                    if (lo < lonMin) lonMin = lo;  if (lo > lonMax) lonMax = lo;
                }
                if (m_owner->IsFoliageAreaArmed())
                    m_owner->OnFoliageAreaPainted(latMin, latMax, lonMin, lonMax);
                else
                    m_owner->OnBoundaryPainted(latMin, latMax, lonMin, lonMax);
            }
        }
        return;
    }

    // Finish a rubber-band group selection (drag = select dots inside the box; a
    // plain empty right-click with no drag = clear the current group).
    if (m_rbActive)
    {
        m_rbActive = false;
        ReleaseCapture();
        if (m_rbMoved && m_owner && m_rt)
        {
            const PreviewRenderState& s = m_owner->GetRenderState();
            const D2D1_SIZE_F dip = m_rt->GetSize();
            const D2D1_POINT_2F a = ClientToDip(m_rbStart);
            const D2D1_POINT_2F b = ClientToDip(m_rbCur);
            const float x0 = (a.x < b.x) ? a.x : b.x, x1 = (a.x < b.x) ? b.x : a.x;
            const float y0 = (a.y < b.y) ? a.y : b.y, y1 = (a.y < b.y) ? b.y : a.y;
            std::vector<size_t> picks;
            for (size_t i = 0; i < s.poses.size(); ++i)
            {
                if (!s.poses[i].enabled) continue;
                const D2D1_POINT_2F p =
                    ProjectEnu(s.poses[i].enuE, s.poses[i].enuN, s, dip.width, dip.height);
                if (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1)
                    picks.push_back(i);
            }
            m_owner->SetSelectedEntities(picks);
        }
        else if (m_owner)
        {
            // No drag: a click inside a painted foliage area opens its menu
            // instead of clearing the selection.
            if (ShowFoliageAreaMenu(pt)) { m_rbMoved = false; return; }
            m_owner->ClearSelection();
        }
        m_rbMoved = false;
        return;
    }

    // Camera box right-click (sits on top of entities): Edit + Delete. Target,
    // transition and zoom used to be three submenus here; they are all fields of
    // the one Edit Camera modal now, which is also the only place the gimbal
    // envelope can be authored.
    {
        const int camHit = HitTestCamera(pt);
        if (camHit >= 0 && m_owner)
        {
            const PreviewRenderState& s = m_owner->GetRenderState();
            const size_t frameIdx = s.cameras[camHit].frameIdx;

            const std::vector<CameraFrame>* cams = m_owner->GetCameras();
            if (!cams || frameIdx >= cams->size()) return;

            enum { kCamEdit = 1, kCamDelete };

            CMenu menu;
            menu.CreatePopupMenu();
            menu.AppendMenu(MF_STRING, kCamEdit,   _T("Edit Camera..."));
            menu.AppendMenu(MF_SEPARATOR, 0, static_cast<LPCTSTR>(nullptr));
            menu.AppendMenu(MF_STRING, kCamDelete, _T("Delete Camera"));

            CPoint sp = pt;
            ClientToScreen(&sp);
            SetForegroundWindow();
            const UINT cmd = menu.TrackPopupMenu(
                TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON, sp.x, sp.y, this);

            if (cmd == kCamEdit)
            {
                m_owner->EditCameraViaDialog(frameIdx);
            }
            else if (cmd == kCamDelete)
            {
                m_owner->DeleteCamera(frameIdx);
            }
            return;
        }
    }

    const int idx = HitTestEntity(pt);
    if (idx < 0)
    {
        // Not on an entity dot — maybe on a course line: offer to extend it.
        const int lineEnt = HitTestLineSegment(pt);
        if (lineEnt >= 0)
        {
            // Same shape as the entity-dot Plot menu: Line, Ellipse (CW/CCW), and
            // Entity Ellipse (CW/CCW) all nest inside a "Plot" submenu.
            enum { kAddLine = 1, kAddEllipseCW, kAddEllipseCCW,
                   kAddEntEllipseCW, kAddEntEllipseCCW, kSetDur };
            CMenu ell;
            ell.CreatePopupMenu();
            ell.AppendMenu(MF_STRING, kAddEllipseCW,  _T("Clockwise"));
            ell.AppendMenu(MF_STRING, kAddEllipseCCW, _T("Counter-Clockwise"));
            CMenu entEll;
            entEll.CreatePopupMenu();
            entEll.AppendMenu(MF_STRING, kAddEntEllipseCW,  _T("Clockwise"));
            entEll.AppendMenu(MF_STRING, kAddEntEllipseCCW, _T("Counter-Clockwise"));
            CMenu plot;
            plot.CreatePopupMenu();
            plot.AppendMenu(MF_STRING, kAddLine, _T("Line"));
            plot.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(ell.GetSafeHmenu()), _T("Ellipse"));
            plot.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(entEll.GetSafeHmenu()), _T("Entity Ellipse"));
            CMenu lm;
            lm.CreatePopupMenu();
            lm.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(plot.GetSafeHmenu()), _T("Plot"));
            lm.AppendMenu(MF_STRING, kSetDur,  _T("Set Duration..."));
            CPoint sp = pt;
            ClientToScreen(&sp);
            SetForegroundWindow();
            const UINT c = lm.TrackPopupMenu(
                TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON, sp.x, sp.y, this);
            ell.Detach();     // owned by `plot` now
            entEll.Detach();  // owned by `plot` now
            plot.Detach();    // owned by `lm` now
            if (m_owner)
            {
                if (c == kAddLine)               m_owner->AddLineToEnd(static_cast<size_t>(lineEnt));
                else if (c == kAddEllipseCW)     m_owner->AddEllipseToEnd(static_cast<size_t>(lineEnt), true);
                else if (c == kAddEllipseCCW)    m_owner->AddEllipseToEnd(static_cast<size_t>(lineEnt), false);
                else if (c == kAddEntEllipseCW)  m_owner->AddEntityEllipseToEnd(static_cast<size_t>(lineEnt), true);
                else if (c == kAddEntEllipseCCW) m_owner->AddEntityEllipseToEnd(static_cast<size_t>(lineEnt), false);
                else if (c == kSetDur)           m_owner->SetPathDuration(static_cast<size_t>(lineEnt));
            }
            return;
        }
        CWnd::OnRButtonUp(nFlags, pt);
        return;
    }

    // If the clicked entity is part of the current group selection, offer group
    // commands that apply to ALL selected entities.
    if (m_owner)
    {
        const PreviewRenderState& gs = m_owner->GetRenderState();
        const bool thisSelected =
            (static_cast<size_t>(idx) < gs.selected.size() && gs.selected[idx]);
        if (thisSelected)
        {
            enum { kGroupDur = 1, kGroupSpeed };
            CMenu gm;
            gm.CreatePopupMenu();
            gm.AppendMenu(MF_STRING, kGroupDur,   _T("Set Duration (selected)..."));
            gm.AppendMenu(MF_STRING, kGroupSpeed, _T("Refresh Speed"));
            CPoint gp = pt;
            ClientToScreen(&gp);
            SetForegroundWindow();
            const UINT gc = gm.TrackPopupMenu(
                TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON, gp.x, gp.y, this);
            if (gc == kGroupDur)        m_owner->SetGroupDuration();
            else if (gc == kGroupSpeed) m_owner->RefreshGroupSpeed();
            return;
        }
    }

    // Build the context menu in code (the app has no .rc menu resources). IDs
    // are local; TPM_RETURNCMD hands the chosen id straight back to us.
    enum { kPlotLine = 1, kEllipseCW, kEllipseCCW, kDuplicate, kRename, kDelete,
           kSetDuration, kRefreshSpeed, kSetDelay, kEntEllipseCW, kEntEllipseCCW,
           kChangeEntity, kTakeoffLine, kNewCamera, kDeleteCamera };

    CMenu ell;
    ell.CreatePopupMenu();
    ell.AppendMenu(MF_STRING, kEllipseCW,  _T("Clockwise"));
    ell.AppendMenu(MF_STRING, kEllipseCCW, _T("Counter-Clockwise"));

    CMenu entEll;
    entEll.CreatePopupMenu();
    entEll.AppendMenu(MF_STRING, kEntEllipseCW,  _T("Clockwise"));
    entEll.AppendMenu(MF_STRING, kEntEllipseCCW, _T("Counter-Clockwise"));

    CMenu plot;
    plot.CreatePopupMenu();
    plot.AppendMenu(MF_STRING, kPlotLine, _T("Line"));
    // Take-off line (accelerate from a stop to cruise) — fixed-wing aircraft only.
    if (m_owner && m_owner->CanTakeoff(static_cast<size_t>(idx)))
        plot.AppendMenu(MF_STRING, kTakeoffLine, _T("Take-off Line"));
    plot.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(ell.GetSafeHmenu()), _T("Ellipse"));
    plot.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(entEll.GetSafeHmenu()), _T("Entity Ellipse"));

    CMenu menu;
    menu.CreatePopupMenu();
    menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(plot.GetSafeHmenu()), _T("Plot"));
    // "New Camera...": opens the modal to pick source / preset / target / transition.
    // Never grayed for a missing catalog — the dialog's source can be set to "(none)",
    // which builds a Stationary camera and needs no presets at all.
    menu.AppendMenu(MF_STRING, kNewCamera, _T("New Camera..."));
    // "Delete Camera": remove the camera(s) mounted on this entity (grayed when
    // the entity has none). Each deleted camera drops its timeline box too.
    const bool hasEntityCamera =
        m_owner && m_owner->EntityCameraCount(static_cast<size_t>(idx)) > 0;
    menu.AppendMenu(hasEntityCamera ? MF_STRING : (MF_STRING | MF_GRAYED),
                    kDeleteCamera, _T("Delete Camera"));
    menu.AppendMenu(MF_STRING, kSetDuration,  _T("Set Duration..."));
    menu.AppendMenu(MF_STRING, kSetDelay,     _T("Set Delay..."));
    menu.AppendMenu(MF_STRING, kRefreshSpeed, _T("Refresh Speed"));
    menu.AppendMenu(MF_STRING, kDuplicate, _T("Duplicate"));
    menu.AppendMenu(MF_STRING, kRename,       _T("Rename"));
    menu.AppendMenu(MF_STRING, kChangeEntity, _T("Change Entity..."));
    menu.AppendMenu(MF_STRING, kDelete,       _T("Delete"));
    ell.Detach();    // owned by `plot` now
    entEll.Detach(); // owned by `plot` now
    plot.Detach();   // owned by `menu` now (avoid double-destroy)

    CPoint screen = pt;
    ClientToScreen(&screen);
    SetForegroundWindow();   // so the menu dismisses correctly on click-away
    const UINT cmd = menu.TrackPopupMenu(
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON, screen.x, screen.y, this);

    switch (cmd)
    {
    case kPlotLine:
        if (m_owner) m_owner->PlotLineCourse(static_cast<size_t>(idx));
        break;
    case kTakeoffLine:
        if (m_owner) m_owner->PlotTakeoffLine(static_cast<size_t>(idx));
        break;
    case kEllipseCW:
        if (m_owner) m_owner->PlotEllipseCourse(static_cast<size_t>(idx), true);
        break;
    case kEllipseCCW:
        if (m_owner) m_owner->PlotEllipseCourse(static_cast<size_t>(idx), false);
        break;
    case kEntEllipseCW:
        if (m_owner) m_owner->PlotEntityEllipse(static_cast<size_t>(idx), true);
        break;
    case kEntEllipseCCW:
        if (m_owner) m_owner->PlotEntityEllipse(static_cast<size_t>(idx), false);
        break;
    case kSetDuration:
        if (m_owner) m_owner->SetPathDuration(static_cast<size_t>(idx));
        break;
    case kSetDelay:
        if (m_owner) m_owner->SetEntityDelay(static_cast<size_t>(idx));
        break;
    case kRefreshSpeed:
        if (m_owner) m_owner->RefreshEntitySpeed(static_cast<size_t>(idx));
        break;
    case kDuplicate:
        if (m_owner) m_owner->DuplicateEntity(static_cast<size_t>(idx));
        break;
    case kRename:
        if (m_owner) m_owner->RenameEntity(static_cast<size_t>(idx));
        break;
    case kChangeEntity:
        if (m_owner) m_owner->ChangeEntity(static_cast<size_t>(idx));
        break;
    case kNewCamera:
        if (m_owner) m_owner->NewEntityCameraViaDialog(static_cast<size_t>(idx));
        break;
    case kDeleteCamera:
        if (m_owner) m_owner->DeleteCamerasForEntity(static_cast<size_t>(idx));
        break;
    case kDelete:
        if (m_owner) m_owner->DeleteEntity(static_cast<size_t>(idx));
        break;
    default:
        break;
    }
}

//
// OnEraseBkgnd — suppress GDI background erase (D2D repaints everything).
//
BOOL CPreviewCanvas::OnEraseBkgnd(CDC*)
{
    // D2D paints the full client area; suppress GDI erase to avoid flicker.
    return TRUE;
}

//
// OnSize — resize the Direct2D render target to match the new client area.
//
void CPreviewCanvas::OnSize(UINT nType, int cx, int cy)
{
    CWnd::OnSize(nType, cx, cy);
    if (m_rt)
        m_rt->Resize(D2D1::SizeU(static_cast<UINT32>(cx), static_cast<UINT32>(cy)));
}

//
// EnsureResources — lazily create the HWND render target, all solid brushes,
//   the dashed stroke, the two DWrite text formats, and (once) the map tile
//   service. No-op if already created; returns false on failure.
//
bool CPreviewCanvas::EnsureResources()
{
    if (m_rt) return true;
    ID2D1Factory* factory = Direct2DContext::Factory();
    if (!factory || !GetSafeHwnd()) return false;

    CRect rc;
    GetClientRect(&rc);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(std::max<LONG>(rc.Width(), 1)),
        static_cast<UINT32>(std::max<LONG>(rc.Height(), 1)));

    HRESULT hr = factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(GetSafeHwnd(), size),
        &m_rt);
    {
        char buf[200];
        sprintf_s(buf, sizeof(buf),
                  "PreviewCanvas::EnsureResources: CreateHwndRT hr=0x%08lX size=%ux%u hwnd=%p",
                  (long)hr, (unsigned)size.width, (unsigned)size.height, (void*)GetSafeHwnd());
        LOG(buf);
    }
    if (FAILED(hr) || !m_rt) return false;

    m_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    m_rt->CreateSolidColorBrush(MakeRgb(0x000000), &m_brushBg);      // dark theme
    m_rt->CreateSolidColorBrush(MakeRgb(0x444444), &m_brushGrid);    // subtle on black
    m_rt->CreateSolidColorBrush(MakeRgb(0xEAEAEA), &m_brushLabel);   // light text on black
    m_rt->CreateSolidColorBrush(MakeRgb(0xFFFFFF), &m_brushOutline);
    m_rt->CreateSolidColorBrush(MakeRgb(0x00A000), &m_brushStart);   // green
    m_rt->CreateSolidColorBrush(MakeRgb(0xD9C9A3), &m_brushLand);    // sand
    m_rt->CreateSolidColorBrush(MakeRgb(0x9FC4DA), &m_brushOcean);   // water blue
    m_rt->CreateSolidColorBrush(MakeRgb(0x6F86A0), &m_brushLevelEdge); // muted outline
    m_rt->CreateSolidColorBrush(MakeRgb(0x2E7D32), &m_brushZone);    // per-zone (color set at draw)
    m_rt->CreateSolidColorBrush(MakeRgb(0xFF2D9B), &m_brushLegendStar); // hot pink "*" placeholder
    m_rt->CreateSolidColorBrush(MakeRgb(0xFFD000), &m_brushSelect);     // selection ring / rubber-band box
    m_rt->CreateSolidColorBrush(MakeRgb(0xE02020), &m_brushSelectErr);  // red ring (Refresh Speed: no catalog cruise)
    m_rt->CreateSolidColorBrush(MakeRgb(0xFF8C00), &m_brushFocus);      // ellipse focus handles (orange)

    // Force-ID palette: 0 Other, 1 Friendly, 2 Opposing, 3 Neutral
    m_rt->CreateSolidColorBrush(MakeRgb(0x808080), &m_brushForce[0]);
    m_rt->CreateSolidColorBrush(MakeRgb(0x78BEFF), &m_brushForce[1]);   // light blue
    m_rt->CreateSolidColorBrush(MakeRgb(0xD23A2A), &m_brushForce[2]);
    m_rt->CreateSolidColorBrush(MakeRgb(0xE0B020), &m_brushForce[3]);

    const float dashes[] = { 4.0f, 3.0f };
    factory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
            D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
        dashes, ARRAYSIZE(dashes),
        &m_dashedStroke);

    if (IDWriteFactory* dw = Direct2DContext::DWrite())
    {
        dw->CreateTextFormat(L"Segoe UI", nullptr,
                             DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &m_textFormat);
        if (m_textFormat)
        {
            m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        dw->CreateTextFormat(L"Segoe UI", nullptr,
                             DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL,
                             13.0f, L"en-us", &m_hudTextFormat);
        if (m_hudTextFormat)
            m_hudTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // One-time map tile service setup: cache dir next to settings.ini, and this
    // window as the async tile-ready notification target.
    if (!m_tilesInit)
    {
        std::wstring dir = theApp.SettingsPath();
        const size_t slash = dir.find_last_of(L"\\/");
        dir = (slash != std::wstring::npos) ? dir.substr(0, slash) : std::wstring(L".");
        dir += L"\\maptiles";
        m_tiles.SetCacheDir(dir);
        m_tiles.SetNotifyWindow(GetSafeHwnd());
        m_tilesInit = true;
    }

    return true;
}

//
// DiscardDeviceResources — release the render target, brushes and cached tile
//   bitmaps after a lost/recreated device; next paint rebuilds them.
//
void CPreviewCanvas::DiscardDeviceResources()
{
    m_rt.Reset();
    m_brushBg.Reset();
    m_brushGrid.Reset();
    m_brushLabel.Reset();
    m_brushOutline.Reset();
    m_brushStart.Reset();
    m_brushLand.Reset();
    m_brushOcean.Reset();
    m_brushLevelEdge.Reset();
    m_brushZone.Reset();
    m_brushLegendStar.Reset();
    m_brushSelect.Reset();
    m_brushSelectErr.Reset();
    m_brushFocus.Reset();
    for (auto& b : m_brushForce) b.Reset();
    m_tileBitmaps.clear();     // device bitmaps die with the render target
    m_tileBmpOrder.clear();
}

//
// BrushForForce — return the palette brush for a force id, clamping unknown ids
//   to 0 (Other).
//
ID2D1SolidColorBrush* CPreviewCanvas::BrushForForce(uint8_t forceId)
{
    if (forceId >= 4) forceId = 0;
    return m_brushForce[forceId].Get();
}

//
// ProjectEnu — map a scenario-origin ENU point (metres) to a DIP screen point
//   using the state's zoom (metres/px) and center. East→+x, North→−y (screen Y
//   grows down). The one projection all drawing and hit-testing share.
//
D2D1_POINT_2F CPreviewCanvas::ProjectEnu(double e, double n,
                                         const PreviewRenderState& s,
                                         float canvasW, float canvasH) const
{
    // ENU East = +X right, North = +Y up. Screen Y grows down.
    const double pxPerMeter = (s.zoomMetersPerPx > 0.0) ? (1.0 / s.zoomMetersPerPx) : 1.0;
    const float sx = canvasW * 0.5f + static_cast<float>((e - s.centerEnuE) * pxPerMeter);
    const float sy = canvasH * 0.5f - static_cast<float>((n - s.centerEnuN) * pxPerMeter);
    return D2D1::Point2F(sx, sy);
}

//
// OnDestroy — stop the tile worker thread before the HWND is torn down.
//
void CPreviewCanvas::OnDestroy()
{
    m_tiles.Stop();   // join the worker before our HWND goes away
    CWnd::OnDestroy();
}

//
// OnTileReady — WM_APP_TILE_READY handler: a tile finished downloading, so
//   invalidate to repaint and pick it up.
//
LRESULT CPreviewCanvas::OnTileReady(WPARAM, LPARAM)
{
    // A background tile finished loading: repaint so it gets picked up.
    if (::IsWindow(GetSafeHwnd())) Invalidate(FALSE);
    return 0;
}

//
// TileBitmap — get (or lazily create) the device bitmap for a map tile from the
//   tile service's decoded pixels, cached by packed (layer,z,x,y) and FIFO-
//   evicted. Returns nullptr while the tile is still loading.
//
ID2D1Bitmap* CPreviewCanvas::TileBitmap(MapLayer layer, int z, int x, int y)
{
    if (!m_rt) return nullptr;
    const uint64_t key = ((uint64_t)(uint32_t)(int)layer << 61)
                       | ((uint64_t)(uint32_t)z << 56)
                       | ((uint64_t)(uint32_t)x << 28)
                       | ((uint64_t)(uint32_t)y);

    auto it = m_tileBitmaps.find(key);
    if (it != m_tileBitmaps.end()) return it->second.Get();

    // Not yet a device bitmap: ask the service for decoded pixels.
    std::shared_ptr<const MapTilePixels> px = m_tiles.GetTile(layer, z, x, y);
    if (!px || px->width <= 0 || px->height <= 0 || px->bgra.empty()) return nullptr;

    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap> bmp;
    const HRESULT hr = m_rt->CreateBitmap(
        D2D1::SizeU((UINT32)px->width, (UINT32)px->height),
        px->bgra.data(), (UINT32)(px->width * 4), props, &bmp);
    if (FAILED(hr) || !bmp) return nullptr;

    // Insert + FIFO-evict to keep the device bitmap cache bounded.
    m_tileBitmaps.emplace(key, bmp);
    m_tileBmpOrder.push_back(key);
    constexpr size_t kMaxTileBitmaps = 256;
    while (m_tileBmpOrder.size() > kMaxTileBitmaps)
    {
        const uint64_t old = m_tileBmpOrder.front();
        m_tileBmpOrder.pop_front();
        if (old != key) m_tileBitmaps.erase(old);
    }
    return bmp.Get();
}

//
// DrawMap — draw the georeferenced raster-tile backdrop under everything. Works
//   out the visible ENU rectangle, converts it to a lat/lon box through the
//   scenario origin, picks the Web-Mercator zoom matching the current m/px, then
//   places each visible tile via ProjectEnu and paints the provider attribution.
//
void CPreviewCanvas::DrawMap(const PreviewRenderState& s, float w, float h)
{
    if (s.mapLayer == MapLayer::None || s.mapLayer == MapLayer::Cesium ||
        !m_rt || w <= 0.0f || h <= 0.0f) return;
    m_tiles.SetOnline(s.mapOnline);

    const double mpp = (s.zoomMetersPerPx > 0.0) ? s.zoomMetersPerPx : 1.0;

    // ENU <-> geodetic through the scenario origin (so the map lines up with the
    // ENU-drawn entities). ProjectEnu then places each tile in screen space.
    auto enuToGeo = [&](double e, double n, double& lat, double& lon)
    {
        double X, Y, Z, alt;
        CoordTransforms::LocalEnuToEcefDeg(e, n, 0.0,
            s.originLatDeg, s.originLonDeg, s.originAltM, X, Y, Z);
        CoordTransforms::EcefToGeodeticDeg(X, Y, Z, lat, lon, alt);
    };
    auto geoToEnu = [&](double lat, double lon, double& e, double& n)
    {
        double X, Y, Z, up;
        CoordTransforms::GeodeticToEcefDeg(lat, lon, 0.0, X, Y, Z);
        CoordTransforms::EcefToLocalEnuDeg(X, Y, Z,
            s.originLatDeg, s.originLonDeg, s.originAltM, e, n, up);
    };

    // Visible ENU rectangle and its lat/lon bounding box.
    const double halfE = 0.5 * w * mpp, halfN = 0.5 * h * mpp;
    const double eMin = s.centerEnuE - halfE, eMax = s.centerEnuE + halfE;
    const double nMin = s.centerEnuN - halfN, nMax = s.centerEnuN + halfN;

    double latMin = 90.0, latMax = -90.0, lonMin = 180.0, lonMax = -180.0;
    const double corners[4][2] = { {eMin, nMin}, {eMin, nMax}, {eMax, nMin}, {eMax, nMax} };
    for (const auto& c : corners)
    {
        double la, lo; enuToGeo(c[0], c[1], la, lo);
        latMin = std::min(latMin, la); latMax = std::max(latMax, la);
        lonMin = std::min(lonMin, lo); lonMax = std::max(lonMax, lo);
    }
    double clat, clon; enuToGeo(s.centerEnuE, s.centerEnuN, clat, clon);

    // Web Mercator is undefined past ~85 deg; nothing to draw there.
    if (clat > 85.0 || clat < -85.0) return;
    latMax = std::min(latMax, 85.05); latMin = std::max(latMin, -85.05);

    // Pick the zoom whose tile resolution ~ matches the current metres/pixel.
    const double cosLat = std::max(0.02, std::cos(clat * 3.14159265358979323846 / 180.0));
    int z = (int)std::lround(std::log2(156543.03392804097 * cosLat / mpp));
    z = std::max(MapTileService::MinZoom(s.mapLayer),
                 std::min(MapTileService::MaxZoom(s.mapLayer), z));

    const int nTiles = 1 << z;
    int xMin = (int)std::floor(MapTileService::TileXAtLon(lonMin, z));
    int xMax = (int)std::floor(MapTileService::TileXAtLon(lonMax, z));
    int yMin = (int)std::floor(MapTileService::TileYAtLat(latMax, z));  // y grows south
    int yMax = (int)std::floor(MapTileService::TileYAtLat(latMin, z));
    xMin = std::max(0, xMin); xMax = std::min(nTiles - 1, xMax);
    yMin = std::max(0, yMin); yMax = std::min(nTiles - 1, yMax);

    // Safety cap: never iterate an unbounded grid (bad origin / degenerate view).
    if ((int64_t)(xMax - xMin + 1) * (yMax - yMin + 1) > 600) return;

    // Each tile is drawn into the axis-aligned bounding box of its FOUR projected
    // corners (not just the two diagonal ones). Projecting Web-Mercator tiles through
    // the ENU tangent plane rotates them away from the origin meridian; a 2-corner
    // rect then leaves black gaps/stagger between tiles. Using the full 4-corner bbox
    // makes neighbouring tiles overlap on their shared edge instead of gapping — no
    // black seams — while keeping the simple (transform-free) DrawBitmap path.
    for (int ty = yMin; ty <= yMax; ++ty)
    {
        for (int tx = xMin; tx <= xMax; ++tx)
        {
            ID2D1Bitmap* bmp = TileBitmap(s.mapLayer, z, tx, ty);
            if (!bmp) continue;

            const double lonW = MapTileService::LonAtTileX(tx,     z);
            const double lonE = MapTileService::LonAtTileX(tx + 1, z);
            const double latN = MapTileService::LatAtTileY(ty,     z);
            const double latS = MapTileService::LatAtTileY(ty + 1, z);

            const double geo[4][2] = { {latN, lonW}, {latN, lonE}, {latS, lonW}, {latS, lonE} };
            float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
            for (const auto& g : geo)
            {
                double e, n; geoToEnu(g[0], g[1], e, n);
                const D2D1_POINT_2F p = ProjectEnu(e, n, s, w, h);
                minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
            }
            const D2D1_RECT_F dst = D2D1::RectF(minx, miny, maxx, maxy);
            m_rt->DrawBitmap(bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

    // Attribution (required by the tile providers), bottom-left HUD.
    if (m_hudTextFormat && m_brushLabel)
    {
        const wchar_t* attr = MapTileService::Attribution(s.mapLayer);
        if (attr && *attr)
        {
            // Top-left, clear of the bottom-left time/zoom HUD.
            const D2D1_RECT_F tr = D2D1::RectF(6.0f, 3.0f, w - 6.0f, 19.0f);
            m_rt->DrawTextW(attr, (UINT32)wcslen(attr), m_hudTextFormat.Get(), tr,
                            m_brushLabel.Get());
        }
    }
}

//
// PickStartAt — "Set Start" pick: find the ellipse orbit whose polyline is
//   closest to a physical-pixel click (within a snap radius), and if found tell
//   the owner to set that entity/segment's start at the picked ENU point.
//   Returns true if a pick was applied.
//
bool CPreviewCanvas::PickStartAt(CPoint pxPt)
{
    if (!m_owner || !m_rt) return false;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (s.ellipses.empty()) return false;

    // Project in DIP space (what ProjectEnu/Render use), and convert the
    // physical-pixel click into DIPs so the two match on scaled displays.
    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return false;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / scaleX;
    const double clickY = pxPt.y / scaleY;

    // Snap radius: 50 physical px expressed in DIPs.
    const double hitDip = 50.0 / ((scaleX > 0.0) ? scaleX : 1.0);
    double bestDist2 = hitDip * hitDip;

    const PreviewEllipse* bestEll = nullptr;
    double bestE = 0.0, bestN = 0.0;

    for (const PreviewEllipse& el : s.ellipses)
    {
        if (el.enuPts.size() < 2) continue;
        D2D1_POINT_2F a = ProjectEnu(el.enuPts[0].x, el.enuPts[0].y, s, dip.width, dip.height);
        for (size_t k = 1; k < el.enuPts.size(); ++k)
        {
            const D2D1_POINT_2F b =
                ProjectEnu(el.enuPts[k].x, el.enuPts[k].y, s, dip.width, dip.height);

            // Closest point on segment (a,b) to the DIP click.
            const double abx = b.x - a.x, aby = b.y - a.y;
            const double apx = clickX - a.x, apy = clickY - a.y;
            const double len2 = abx * abx + aby * aby;
            double u = (len2 > 0.0) ? (apx * abx + apy * aby) / len2 : 0.0;
            if (u < 0.0) u = 0.0; else if (u > 1.0) u = 1.0;
            const double cx = a.x + u * abx, cy = a.y + u * aby;
            const double dx = cx - clickX, dy = cy - clickY;
            const double d2 = dx * dx + dy * dy;
            if (d2 < bestDist2)
            {
                bestDist2 = d2;
                bestEll   = &el;
                bestE = el.enuPts[k - 1].x + u * (el.enuPts[k].x - el.enuPts[k - 1].x);
                bestN = el.enuPts[k - 1].y + u * (el.enuPts[k].y - el.enuPts[k - 1].y);
            }
            a = b;
        }
    }

    if (!bestEll) return false;   // nothing within snap range
    m_owner->ApplyStartPick(bestEll->entityIdx, bestEll->segIdx, bestE, bestN);
    return true;
}

//
// UnprojectToEnu — inverse of ProjectEnu: convert a physical-pixel client point
//   to scenario-origin ENU east/north (via DIPs, so it's DPI-correct). Returns
//   false if there's no render target/owner yet.
//
bool CPreviewCanvas::UnprojectToEnu(CPoint pxPt, double& outE, double& outN) const
{
    if (!m_owner || !m_rt) return false;
    const PreviewRenderState& s = m_owner->GetRenderState();

    // Convert the physical-pixel click into DIPs (ProjectEnu works in DIPs).
    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return false;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    // Invert ProjectEnu: sx = w/2 + (e-cE)/mpp ; sy = h/2 - (n-cN)/mpp.
    const double mpp = (s.zoomMetersPerPx > 0.0) ? s.zoomMetersPerPx : 1.0;
    outE = s.centerEnuE + (clickX - dip.width  * 0.5) * mpp;
    outN = s.centerEnuN - (clickY - dip.height * 0.5) * mpp;
    return true;
}

//
// HitTestEntity — index of the nearest enabled entity dot to a physical-pixel
//   point within the pick radius, or -1 if none.
//
int CPreviewCanvas::HitTestEntity(CPoint pxPt) const
{
    if (!m_owner || !m_rt) return -1;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (s.poses.empty()) return -1;

    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return -1;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    // Pick radius: the dot radius plus a few DIPs of slack so small dots are
    // easy to grab.
    const double hitDip = kEntityDotRadiusPx + 5.0;
    double bestDist2 = hitDip * hitDip;
    int best = -1;

    for (size_t i = 0; i < s.poses.size(); ++i)
    {
        if (!s.poses[i].enabled) continue;
        const D2D1_POINT_2F p =
            ProjectEnu(s.poses[i].enuE, s.poses[i].enuN, s, dip.width, dip.height);
        const double dx = p.x - clickX, dy = p.y - clickY;
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestDist2) { bestDist2 = d2; best = static_cast<int>(i); }
    }
    return best;
}

//
// HitTestZone — index into state.levelZones of the topmost enabled zone box whose
//   rectangle contains a physical-pixel point, or -1. Only hits when zones are
//   visible (showZones). Later zones are drawn on top, so iterate in reverse.
//
//
// HitTestFoliageArea — topmost painted foliage rectangle under a click, or -1.
//   Reverse order so the most recently painted area wins, matching both the draw
//   order and the bake's "first painted owns the overlap" rule read backwards:
//   the one you see on top is the one you act on.
//
int CPreviewCanvas::HitTestFoliageArea(CPoint pxPt) const
{
    if (!m_owner || !m_rt) return -1;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (s.foliageAreas.empty()) return -1;

    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return -1;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    for (int i = static_cast<int>(s.foliageAreas.size()) - 1; i >= 0; --i)
    {
        const PreviewFoliageArea& a = s.foliageAreas[static_cast<size_t>(i)];
        const D2D1_POINT_2F tl = ProjectEnu(a.eastMinM, a.northMaxM, s, dip.width, dip.height);
        const D2D1_POINT_2F br = ProjectEnu(a.eastMaxM, a.northMinM, s, dip.width, dip.height);
        const double left   = std::min(tl.x, br.x), right  = std::max(tl.x, br.x);
        const double top    = std::min(tl.y, br.y), bottom = std::max(tl.y, br.y);
        if (clickX >= left && clickX <= right && clickY >= top && clickY <= bottom)
            return i;
    }
    return -1;
}

int CPreviewCanvas::HitTestZone(CPoint pxPt) const
{
    if (!m_owner || !m_rt) return -1;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (!s.showZones || s.levelZones.empty()) return -1;

    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return -1;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    for (int i = static_cast<int>(s.levelZones.size()) - 1; i >= 0; --i)
    {
        const PreviewLevelZone& z = s.levelZones[static_cast<size_t>(i)];
        if (!z.enabled) continue;
        const D2D1_POINT_2F tl = ProjectEnu(z.eastMinM, z.northMaxM, s, dip.width, dip.height);
        const D2D1_POINT_2F br = ProjectEnu(z.eastMaxM, z.northMinM, s, dip.width, dip.height);
        const double left   = std::min(tl.x, br.x), right  = std::max(tl.x, br.x);
        const double top    = std::min(tl.y, br.y), bottom = std::max(tl.y, br.y);
        if (clickX >= left && clickX <= right && clickY >= top && clickY <= bottom)
            return i;
    }
    return -1;
}

//
// HitTestCamera — index into state.cameras of the Stationary camera box whose
//   square contains a physical-pixel point, or -1. Entity cameras aren't picked
//   (they track their source and can't be dragged). Topmost (last) wins.
//
int CPreviewCanvas::HitTestCamera(CPoint pxPt) const
{
    if (!m_owner || !m_rt) return -1;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (s.cameras.empty()) return -1;

    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return -1;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    const double half = kCameraBoxHalfPx + 3.0;   // a little grab slack
    for (size_t i = s.cameras.size(); i-- > 0; )   // topmost first
    {
        if (!s.cameras[i].stationary) continue;
        const D2D1_POINT_2F p =
            ProjectEnu(s.cameras[i].enuE, s.cameras[i].enuN, s, dip.width, dip.height);
        if (std::fabs(p.x - clickX) <= half && std::fabs(p.y - clickY) <= half)
            return static_cast<int>(i);
    }
    return -1;
}

//
// HitTestLineAnchor — index into state.lineAnchors of the nearest Line end-anchor
//   ("pink dot") to a physical-pixel point within the pick radius, or -1.
//
int CPreviewCanvas::HitTestLineAnchor(CPoint pxPt) const
{
    if (!m_owner || !m_rt) return -1;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (s.lineAnchors.empty()) return -1;

    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return -1;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    const double hitDip = kAnchorDotRadiusPx + 6.0;
    double bestDist2 = hitDip * hitDip;
    int best = -1;
    for (size_t i = 0; i < s.lineAnchors.size(); ++i)
    {
        const D2D1_POINT_2F p =
            ProjectEnu(s.lineAnchors[i].enuE, s.lineAnchors[i].enuN, s, dip.width, dip.height);
        const double dx = p.x - clickX, dy = p.y - clickY;
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestDist2) { bestDist2 = d2; best = static_cast<int>(i); }
    }
    return best;
}

//
// HitTestLineSegment — entity index whose drawn course polyline passes under a
//   physical-pixel point (point-to-segment distance within slack), or -1. Used
//   to right-click a course line and extend it.
//
int CPreviewCanvas::HitTestLineSegment(CPoint pxPt) const
{
    if (!m_owner || !m_rt) return -1;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (s.paths.empty()) return -1;

    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return -1;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    const double thresh = 6.0;   // DIPs of slack around the line
    double best = thresh;
    int bestEnt = -1;
    for (size_t i = 0; i < s.paths.size(); ++i)
    {
        const auto& poly = s.paths[i];
        for (size_t k = 1; k < poly.size(); ++k)
        {
            const D2D1_POINT_2F a = ProjectEnu(poly[k - 1].x, poly[k - 1].y, s, dip.width, dip.height);
            const D2D1_POINT_2F b = ProjectEnu(poly[k].x,     poly[k].y,     s, dip.width, dip.height);
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double len2 = dx * dx + dy * dy;
            double t = (len2 > 0.0) ? ((clickX - a.x) * dx + (clickY - a.y) * dy) / len2 : 0.0;
            t = (t < 0.0) ? 0.0 : (t > 1.0 ? 1.0 : t);
            const double qx = a.x + t * dx, qy = a.y + t * dy;
            const double ex = clickX - qx, ey = clickY - qy;
            const double d = std::sqrt(ex * ex + ey * ey);
            if (d < best) { best = d; bestEnt = static_cast<int>(i); }
        }
    }
    return bestEnt;
}

//
// HitTestFocus — index into state.focusHandles of the nearest ellipse focus
//   handle to a physical-pixel point within the pick radius, or -1.
//
int CPreviewCanvas::HitTestFocus(CPoint pxPt) const
{
    if (!m_owner || !m_rt) return -1;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (s.focusHandles.empty()) return -1;

    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return -1;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    const double hitDip = kAnchorDotRadiusPx + 6.0;
    double bestDist2 = hitDip * hitDip;
    int best = -1;
    for (size_t i = 0; i < s.focusHandles.size(); ++i)
    {
        const D2D1_POINT_2F p =
            ProjectEnu(s.focusHandles[i].enuE, s.focusHandles[i].enuN, s, dip.width, dip.height);
        const double dx = p.x - clickX, dy = p.y - clickY;
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestDist2) { bestDist2 = d2; best = static_cast<int>(i); }
    }
    return best;
}

//
// HitTestEllipseShape — index into state.ellipses of the orbit whose shape
//   handle (the dot on the curve at the orbit start) is nearest a physical-pixel
//   point within the pick radius, or -1.
//
int CPreviewCanvas::HitTestEllipseShape(CPoint pxPt) const
{
    if (!m_owner || !m_rt) return -1;
    const PreviewRenderState& s = m_owner->GetRenderState();
    if (s.ellipses.empty()) return -1;

    const D2D1_SIZE_F dip = m_rt->GetSize();
    if (dip.width <= 0.0f || dip.height <= 0.0f) return -1;
    CRect rc; GetClientRect(&rc);
    const double scaleX = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double scaleY = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    const double clickX = pxPt.x / ((scaleX > 0.0) ? scaleX : 1.0);
    const double clickY = pxPt.y / ((scaleY > 0.0) ? scaleY : 1.0);

    const double hitDip = kAnchorDotRadiusPx + 6.0;
    double bestDist2 = hitDip * hitDip;
    int best = -1;
    for (size_t i = 0; i < s.ellipses.size(); ++i)
    {
        const PreviewEllipse& el = s.ellipses[i];
        if (el.enuPts.empty()) continue;           // handle sits at the orbit start
        const D2D1_POINT_2F p =
            ProjectEnu(el.enuPts[0].x, el.enuPts[0].y, s, dip.width, dip.height);
        const double dx = p.x - clickX, dy = p.y - clickY;
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestDist2) { bestDist2 = d2; best = static_cast<int>(i); }
    }
    return best;
}

//
// ClientToDip — convert a physical-pixel client point to DIPs (the space
//   ProjectEnu and the render target draw in), so picking matches on scaled displays.
//
D2D1_POINT_2F CPreviewCanvas::ClientToDip(CPoint px) const
{
    if (!m_rt) return D2D1::Point2F(static_cast<float>(px.x), static_cast<float>(px.y));
    const D2D1_SIZE_F dip = m_rt->GetSize();
    CRect rc; GetClientRect(&rc);
    const double sx = (rc.Width()  > 0) ? rc.Width()  / dip.width  : 1.0;
    const double sy = (rc.Height() > 0) ? rc.Height() / dip.height : 1.0;
    return D2D1::Point2F(static_cast<float>(px.x / ((sx > 0.0) ? sx : 1.0)),
                         static_cast<float>(px.y / ((sy > 0.0) ? sy : 1.0)));
}

//
// OnPaint — WM_PAINT handler: ensure device resources exist, then Render the
//   owner's current render state (first few paints are logged for diagnostics).
//
void CPreviewCanvas::OnPaint()
{
    CPaintDC dc(this);   // validates the update region; the HDC itself is unused.
    static int s_paintCount = 0;
    const bool logThis = (s_paintCount++ < 3);
    if (logThis)
    {
        CRect rc; GetClientRect(&rc);
        char buf[200];
        sprintf_s(buf, sizeof(buf),
                  "PreviewCanvas::OnPaint #%d: client=%ldx%ld owner=%p",
                  s_paintCount, (long)rc.Width(), (long)rc.Height(), (void*)m_owner);
        LOG(buf);
    }
    if (!EnsureResources())
    {
        if (logThis) LOG("PreviewCanvas: EnsureResources FAILED");
        return;
    }
    if (!m_owner) return;

    const PreviewRenderState& state = m_owner->GetRenderState();
    Render(state);
}

//
// Render — draw one full frame in back-to-front order: map backdrop, terrain
//   overlay + zones, legend, crosshair, courses, trails, orbit markers and edit
//   handles, entity dots/orientation/labels, HUD, and any active drag box. On a
//   lost target it discards device resources so the next paint rebuilds them.
//
void CPreviewCanvas::Render(const PreviewRenderState& state)
{
    if (!m_rt) return;

    const D2D1_SIZE_F rtSize = m_rt->GetSize();
    const float w = rtSize.width;
    const float h = rtSize.height;

    m_rt->BeginDraw();
    m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    m_rt->Clear(MakeRgb(0x000000));

    // Map backdrop (georeferenced raster tiles), under everything else.
    DrawMap(state, w, h);

    // Terrain overlay (under the grid + entities). Ocean first so land covers
    // the strip where they overlap, matching the real shoreline.
    if (state.showLevel)
    {
        auto drawRect = [&](const PreviewLevelRect& r, ID2D1SolidColorBrush* fill)
        {
            if (!r.enabled || !fill) return;
            // Projection is axis-aligned (East→+x, North→−y), so two opposite
            // corners give the screen rectangle.
            const D2D1_POINT_2F tl = ProjectEnu(r.eastMinM, r.northMaxM, state, w, h);
            const D2D1_POINT_2F br = ProjectEnu(r.eastMaxM, r.northMinM, state, w, h);
            const D2D1_RECT_F rc = D2D1::RectF(std::min(tl.x, br.x), std::min(tl.y, br.y),
                                               std::max(tl.x, br.x), std::max(tl.y, br.y));
            const float oldA = fill->GetOpacity();
            fill->SetOpacity(0.55f);
            m_rt->FillRectangle(rc, fill);
            fill->SetOpacity(oldA);
            if (m_brushLevelEdge) m_rt->DrawRectangle(rc, m_brushLevelEdge.Get(), 1.0f);
        };
        drawRect(state.levelOcean, m_brushOcean.Get());
        drawRect(state.levelLand,  m_brushLand.Get());
    }

    // Named sub-regions (forests, exclusion boxes, …) over the terrain: tinted
    // fill + solid outline + centered label, one reusable brush recolored per
    // zone. Gated SEPARATELY from the land/ocean footprint (state.showZones vs.
    // state.showLevel) so the big terrain box can be hidden — e.g. to clear a
    // runway that entities take off through — while a zone like the Palm Forest
    // stays visible.
    if (state.showZones)
    {
        for (const PreviewLevelZone& z : state.levelZones)
        {
            if (!z.enabled || !m_brushZone) continue;
            const D2D1_POINT_2F tl = ProjectEnu(z.eastMinM, z.northMaxM, state, w, h);
            const D2D1_POINT_2F br = ProjectEnu(z.eastMaxM, z.northMinM, state, w, h);
            const D2D1_RECT_F rc = D2D1::RectF(std::min(tl.x, br.x), std::min(tl.y, br.y),
                                               std::max(tl.x, br.x), std::max(tl.y, br.y));
            m_brushZone->SetColor(MakeRgb(z.colorRgb));
            m_brushZone->SetOpacity(0.40f);
            m_rt->FillRectangle(rc, m_brushZone.Get());
            m_brushZone->SetOpacity(1.0f);
            m_rt->DrawRectangle(rc, m_brushZone.Get(), 1.5f);

            if (!z.name.empty() && m_textFormat && m_brushLabel)
            {
                const CA2W wname(z.name.c_str());
                const wchar_t* txt = wname.m_psz ? wname.m_psz : L"";
                const D2D1_RECT_F tr = D2D1::RectF(rc.left + 4.0f, rc.top + 3.0f,
                                                   rc.right - 4.0f, rc.bottom - 3.0f);
                m_rt->DrawTextW(txt, static_cast<UINT32>(wcslen(txt)),
                               m_textFormat.Get(), tr, m_brushLabel.Get());
            }
        }
    }

    // ---- Painted foliage areas: forest-green boxes labelled with their tree
    //      count. Drawn as painted, overlaps included -- the bake decides who
    //      owns overlapped ground, and hiding the overlap here would make the
    //      count in each label look wrong. ----
    if (m_brushZone && !state.foliageAreas.empty())
    {
        for (const PreviewFoliageArea& a : state.foliageAreas)
        {
            const D2D1_POINT_2F tl = ProjectEnu(a.eastMinM, a.northMaxM, state, w, h);
            const D2D1_POINT_2F br = ProjectEnu(a.eastMaxM, a.northMinM, state, w, h);
            const D2D1_RECT_F rc = D2D1::RectF(std::min(tl.x, br.x), std::min(tl.y, br.y),
                                               std::max(tl.x, br.x), std::max(tl.y, br.y));
            m_brushZone->SetColor(D2D1::ColorF(0.18f, 0.55f, 0.22f));
            m_brushZone->SetOpacity(0.22f);
            m_rt->FillRectangle(rc, m_brushZone.Get());
            m_brushZone->SetOpacity(1.0f);
            m_rt->DrawRectangle(rc, m_brushZone.Get(), 1.5f);

            if (m_textFormat && m_brushLabel)
            {
                wchar_t txt[64];
                swprintf_s(txt, L"%lld trees", a.treeCount);
                const D2D1_RECT_F tr = D2D1::RectF(rc.left + 4.0f, rc.top + 3.0f,
                                                   rc.right - 4.0f, rc.bottom - 3.0f);
                m_rt->DrawTextW(txt, static_cast<UINT32>(wcslen(txt)),
                                m_textFormat.Get(), tr, m_brushLabel.Get());
            }
        }
    }

    // ---- Legend overlay: per-area edge distances + N/S/E/W, zoom-aware fit
    //      (a blurb that can't fit on one line becomes a pink "*"). ----
    if (state.showLegend && m_textFormat && m_brushLabel && m_brushLegendStar)
    {
        IDWriteFactory* dw = Direct2DContext::DWrite();
        auto measureW = [&](const wchar_t* s) -> float
        {
            if (!dw) return 0.0f;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dw->CreateTextLayout(s, (UINT32)wcslen(s), m_textFormat.Get(),
                                            1.0e6f, 1.0e6f, &layout)) || !layout)
                return 0.0f;
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            return tm.width;
        };
        // Draw `s` centered at (cx,cy) on one line, or a pink "*" if it can't fit `allotted` px.
        auto drawBlurb = [&](const wchar_t* s, float cx, float cy, float allotted)
        {
            const bool fits = measureW(s) <= allotted;
            const wchar_t* out = fits ? s : L"*";
            ID2D1SolidColorBrush* br = fits ? m_brushLabel.Get() : m_brushLegendStar.Get();
            const D2D1_RECT_F rc = D2D1::RectF(cx - 200.0f, cy - 9.0f, cx + 200.0f, cy + 9.0f);
            m_rt->DrawTextW(out, (UINT32)wcslen(out), m_textFormat.Get(), rc, br);
        };
        auto fmtDist = [](double meters, wchar_t* out, size_t cch)
        {
            if (meters < 500.0) swprintf_s(out, cch, L"%.0f m", meters);
            else                swprintf_s(out, cch, L"%.2f km", meters / 1000.0);
        };

        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        // Per-area edge sizes (top edge = E-W width, left side = N-S height),
        // only while the terrain overlay they annotate is visible.
        if (state.showLevel)
        {
            auto areaLegend = [&](const PreviewLevelRect& r)
            {
                if (!r.enabled) return;
                const D2D1_POINT_2F tl  = ProjectEnu(r.eastMinM, r.northMaxM, state, w, h);
                const D2D1_POINT_2F br2 = ProjectEnu(r.eastMaxM, r.northMinM, state, w, h);
                const float left = std::min(tl.x, br2.x), right = std::max(tl.x, br2.x);
                const float top  = std::min(tl.y, br2.y), bot   = std::max(tl.y, br2.y);
                wchar_t buf[32];
                fmtDist(r.eastMaxM - r.eastMinM, buf, 32);
                drawBlurb(buf, (left + right) * 0.5f, top - 10.0f, right - left);
                fmtDist(r.northMaxM - r.northMinM, buf, 32);
                drawBlurb(buf, left - 26.0f, (top + bot) * 0.5f, bot - top);
            };
            areaLegend(state.levelOcean);
            areaLegend(state.levelLand);
            for (const PreviewLevelZone& z : state.levelZones)
            {
                PreviewLevelRect rr;
                rr.enabled   = z.enabled;
                rr.eastMinM  = z.eastMinM;  rr.eastMaxM  = z.eastMaxM;
                rr.northMinM = z.northMinM; rr.northMaxM = z.northMaxM;
                areaLegend(rr);
            }
        }

        // Compass labels at the canvas edges (north-up map).
        drawBlurb(L"North", w * 0.5f,  12.0f,     w);
        drawBlurb(L"South", w * 0.5f,  h - 12.0f, w);
        drawBlurb(L"East",  w - 30.0f, h * 0.5f,  h);
        drawBlurb(L"West",  30.0f,     h * 0.5f,  h);

        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); // restore for zone labels
    }

    // Crosshair at canvas center (ENU origin in view space if center is 0,0).
    m_rt->DrawLine(D2D1::Point2F(0, h * 0.5f),
                   D2D1::Point2F(w, h * 0.5f),
                   m_brushGrid.Get(), 1.0f, m_dashedStroke.Get());
    m_rt->DrawLine(D2D1::Point2F(w * 0.5f, 0),
                   D2D1::Point2F(w * 0.5f, h),
                   m_brushGrid.Get(), 1.0f, m_dashedStroke.Get());

    const size_t n = state.poses.size();

    // Paths (under everything).
    if (state.showPaths)
    {
        for (size_t i = 0; i < n && i < state.paths.size(); ++i)
        {
            const auto& path = state.paths[i];
            if (path.size() < 2) continue;
            ID2D1SolidColorBrush* b = BrushForForce(state.poses[i].forceId);
            if (!b) continue;
            // Group-selected entities get a brighter, thicker path line.
            const bool sel = (i < state.selected.size() && state.selected[i]);
            const float oldA = b->GetOpacity();
            b->SetOpacity(sel ? 0.95f : 0.35f);
            const float lineW = sel ? 2.6f : 1.2f;
            D2D1_POINT_2F prev = ProjectEnu(path[0].x, path[0].y, state, w, h);
            for (size_t k = 1; k < path.size(); ++k)
            {
                D2D1_POINT_2F cur = ProjectEnu(path[k].x, path[k].y, state, w, h);
                m_rt->DrawLine(prev, cur, b, lineW, m_dashedStroke.Get());
                prev = cur;
            }
            b->SetOpacity(oldA);
        }
    }

    // Trails (under dots but above paths).
    if (state.showTrails)
    {
        for (size_t i = 0; i < n && i < state.trails.size(); ++i)
        {
            const auto& trail = state.trails[i];
            if (trail.size() < 2) continue;
            ID2D1SolidColorBrush* b = BrushForForce(state.poses[i].forceId);
            if (!b) continue;
            const float oldA = b->GetOpacity();
            const size_t segs = trail.size() - 1;
            D2D1_POINT_2F prev = ProjectEnu(trail[0].x, trail[0].y, state, w, h);
            for (size_t k = 1; k < trail.size(); ++k)
            {
                const float t = static_cast<float>(k) / static_cast<float>(segs);
                b->SetOpacity(0.10f + 0.80f * t);
                D2D1_POINT_2F cur = ProjectEnu(trail[k].x, trail[k].y, state, w, h);
                m_rt->DrawLine(prev, cur, b, 2.0f);
                prev = cur;
            }
            b->SetOpacity(oldA);
        }
    }

    // Ellipse start markers: a filled green dot where each orbit begins.
    if (state.showPaths && m_brushStart)
    {
        for (const D2D1_POINT_2F& startEnu : state.ellipseStarts)
        {
            const D2D1_POINT_2F sp = ProjectEnu(startEnu.x, startEnu.y, state, w, h);
            D2D1_ELLIPSE dot = D2D1::Ellipse(sp, 5.0f, 5.0f);
            m_rt->FillEllipse(dot, m_brushStart.Get());
            m_rt->DrawEllipse(dot, m_brushOutline.Get(), 1.5f);  // white rim for contrast
        }
    }

    // Line course end-anchors: draggable pink destination dots.
    if (state.showPaths && m_brushLegendStar)
    {
        for (const PreviewLineAnchor& a : state.lineAnchors)
        {
            const D2D1_POINT_2F sp = ProjectEnu(a.enuE, a.enuN, state, w, h);
            D2D1_ELLIPSE dot = D2D1::Ellipse(sp, kAnchorDotRadiusPx, kAnchorDotRadiusPx);
            m_rt->FillEllipse(dot, m_brushLegendStar.Get());
            m_rt->DrawEllipse(dot, m_brushOutline.Get(), 1.5f);  // white rim
        }
    }

    // Ellipse focus handles: a thin major-axis line between each orbit's two
    // foci + draggable orange dots.
    if (state.showPaths && m_brushFocus)
    {
        for (size_t i = 0; i + 1 < state.focusHandles.size(); ++i)
        {
            const PreviewFocusHandle& a = state.focusHandles[i];
            const PreviewFocusHandle& b = state.focusHandles[i + 1];
            if (a.entityIdx == b.entityIdx && a.segIdx == b.segIdx &&
                a.focusIdx == 0 && b.focusIdx == 1)
                m_rt->DrawLine(ProjectEnu(a.enuE, a.enuN, state, w, h),
                               ProjectEnu(b.enuE, b.enuN, state, w, h),
                               m_brushFocus.Get(), 1.0f);
        }
        for (const PreviewFocusHandle& f : state.focusHandles)
        {
            const D2D1_POINT_2F sp = ProjectEnu(f.enuE, f.enuN, state, w, h);
            D2D1_ELLIPSE dot = D2D1::Ellipse(sp, kAnchorDotRadiusPx, kAnchorDotRadiusPx);
            m_rt->FillEllipse(dot, m_brushFocus.Get());
            m_rt->DrawEllipse(dot, m_brushOutline.Get(), 1.5f);  // white rim
        }

        // Ellipse shape handle: a draggable dot sitting ON the orbit curve at its
        // start. Same orange as the foci; grabbing it re-sizes the orbit. Drawn
        // over the green start marker so it doubles as the start indicator.
        for (const PreviewEllipse& el : state.ellipses)
        {
            if (el.enuPts.empty()) continue;
            const D2D1_POINT_2F sp =
                ProjectEnu(el.enuPts[0].x, el.enuPts[0].y, state, w, h);
            D2D1_ELLIPSE dot = D2D1::Ellipse(sp, kAnchorDotRadiusPx, kAnchorDotRadiusPx);
            m_rt->FillEllipse(dot, m_brushFocus.Get());
            m_rt->DrawEllipse(dot, m_brushOutline.Get(), 1.5f);  // white rim
        }
    }

    // Entity dots + orientation + labels.
    std::vector<D2D1_POINT_2F> dotScreen(n);
    for (size_t i = 0; i < n; ++i)
    {
        const auto& p = state.poses[i];
        const D2D1_POINT_2F sp = ProjectEnu(p.enuE, p.enuN, state, w, h);
        dotScreen[i] = sp;
        ID2D1SolidColorBrush* b = BrushForForce(p.forceId);
        if (!b) continue;

        // Dot
        D2D1_ELLIPSE dot = D2D1::Ellipse(sp, kEntityDotRadiusPx, kEntityDotRadiusPx);
        m_rt->FillEllipse(dot, b);
        m_rt->DrawEllipse(dot, m_brushOutline.Get(), 1.5f);

        // Group-selection highlight ring (red if a Refresh Speed found no catalog cruise).
        if (i < state.selected.size() && state.selected[i])
        {
            ID2D1SolidColorBrush* rb =
                (i < state.selectError.size() && state.selectError[i]) ? m_brushSelectErr.Get()
                                                                       : m_brushSelect.Get();
            if (rb)
            {
                D2D1_ELLIPSE ring = D2D1::Ellipse(sp, kEntityDotRadiusPx + 4.0f, kEntityDotRadiusPx + 4.0f);
                m_rt->DrawEllipse(ring, rb, 2.0f);
            }
        }

        // Orientation arrow: heading 0 = North = up, clockwise from above.
        if (state.showOrientation)
        {
            const double hRad = p.headingDeg * 3.14159265358979323846 / 180.0;
            const float dx = static_cast<float>(std::sin(hRad)) * kOrientationLenPx;
            const float dy = static_cast<float>(-std::cos(hRad)) * kOrientationLenPx;
            m_rt->DrawLine(sp,
                           D2D1::Point2F(sp.x + dx, sp.y + dy),
                           b, 2.0f);
        }
    }

    // End-point highlight for group-selected entities: mark and ring the last
    // point of the path (same look as the entity dot's selection ring) and
    // label it "<entity name>" / "end point" on two lines.
    for (size_t i = 0; i < n; ++i)
    {
        if (!(i < state.selected.size() && state.selected[i])) continue;
        if (i >= state.paths.size() || state.paths[i].size() < 2) continue;

        const D2D1_POINT_2F endEnu = state.paths[i].back();
        const D2D1_POINT_2F sp = ProjectEnu(endEnu.x, endEnu.y, state, w, h);

        if (ID2D1SolidColorBrush* b = BrushForForce(state.poses[i].forceId))
        {
            D2D1_ELLIPSE dot = D2D1::Ellipse(sp, kEntityDotRadiusPx, kEntityDotRadiusPx);
            m_rt->FillEllipse(dot, b);
            m_rt->DrawEllipse(dot, m_brushOutline.Get(), 1.5f);
        }

        ID2D1SolidColorBrush* rb =
            (i < state.selectError.size() && state.selectError[i]) ? m_brushSelectErr.Get()
                                                                   : m_brushSelect.Get();
        if (rb)
        {
            D2D1_ELLIPSE ring = D2D1::Ellipse(sp, kEntityDotRadiusPx + 4.0f, kEntityDotRadiusPx + 4.0f);
            m_rt->DrawEllipse(ring, rb, 2.0f);
        }

        // Two-line label: entity name, then "end point".
        if (state.showLabels && m_textFormat)
        {
            const CA2W wname(state.poses[i].name.c_str());
            std::wstring txt = (wname.m_psz ? wname.m_psz : L"");
            txt += L"\nend point";
            D2D1_RECT_F r = D2D1::RectF(sp.x + kLabelOffsetPx, sp.y - 8.0f,
                                        sp.x + kLabelOffsetPx + 160.0f, sp.y + 30.0f);
            m_rt->DrawTextW(txt.c_str(), static_cast<UINT32>(txt.size()),
                            m_textFormat.Get(), r, m_brushLabel.Get());
        }
    }

    // Labels with simple pairwise repulsion to reduce overlap.
    if (state.showLabels && m_textFormat)
    {
        struct Label { std::wstring text; float x, y; float w, h; };
        std::vector<Label> labels(n);
        for (size_t i = 0; i < n; ++i)
        {
            const CA2W wname(state.poses[i].name.c_str());
            labels[i].text = wname.m_psz ? wname.m_psz : L"";
            labels[i].x = dotScreen[i].x + kLabelOffsetPx;
            labels[i].y = dotScreen[i].y - 8.0f;
            labels[i].w = 8.0f * static_cast<float>(labels[i].text.size()) + 6.0f;
            labels[i].h = 16.0f;
        }
        for (int pass = 0; pass < 4; ++pass)
        {
            bool moved = false;
            for (size_t i = 0; i < labels.size(); ++i)
                for (size_t j = i + 1; j < labels.size(); ++j)
                {
                    auto& a = labels[i];
                    auto& b = labels[j];
                    const float ox = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
                    const float oy = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
                    if (ox > 0 && oy > 0)
                    {
                        const float push = (oy * 0.5f) + 1.0f;
                        if (a.y < b.y) { a.y -= push; b.y += push; }
                        else           { a.y += push; b.y -= push; }
                        moved = true;
                    }
                }
            if (!moved) break;
        }
        for (const auto& L : labels)
        {
            if (L.text.empty()) continue;
            D2D1_RECT_F r = D2D1::RectF(L.x, L.y, L.x + L.w, L.y + L.h);
            m_rt->DrawTextW(L.text.c_str(),
                           static_cast<UINT32>(L.text.size()),
                           m_textFormat.Get(), r, m_brushLabel.Get());
        }
    }

    // Camera vantage boxes (pink squares). Stationary cameras are solid; Entity
    // cameras (pinned to their source) are drawn as a hollow outline so they read
    // as "attached, not free". Labels sit just above each box.
    if (m_brushLegendStar)
    {
        for (const PreviewCameraBox& cam : state.cameras)
        {
            const D2D1_POINT_2F sp = ProjectEnu(cam.enuE, cam.enuN, state, w, h);
            const D2D1_RECT_F box = D2D1::RectF(sp.x - kCameraBoxHalfPx, sp.y - kCameraBoxHalfPx,
                                                sp.x + kCameraBoxHalfPx, sp.y + kCameraBoxHalfPx);
            if (cam.stationary)
            {
                m_rt->FillRectangle(box, m_brushLegendStar.Get());
                m_rt->DrawRectangle(box, m_brushOutline.Get(), 1.5f);
            }
            else
            {
                m_rt->DrawRectangle(box, m_brushLegendStar.Get(), 2.0f);
            }

            if (state.showLabels && m_textFormat && !cam.label.empty())
            {
                const CA2W wlabel(cam.label.c_str());
                std::wstring txt = wlabel.m_psz ? wlabel.m_psz : L"";
                D2D1_RECT_F r = D2D1::RectF(sp.x + kCameraBoxHalfPx + 2.0f, sp.y - 8.0f,
                                            sp.x + kCameraBoxHalfPx + 180.0f, sp.y + 10.0f);
                m_rt->DrawTextW(txt.c_str(), static_cast<UINT32>(txt.size()),
                                m_textFormat.Get(), r, m_brushLegendStar.Get());
            }
        }
    }

    // HUD: bottom-left time + zoom readout.
    if (m_hudTextFormat)
    {
        wchar_t buf[128];
        swprintf_s(buf, L"t = %.1fs / %.0fs    zoom = %.2f m/px",
                   state.previewTimeSec, state.durationSec, state.zoomMetersPerPx);
        D2D1_RECT_F r = D2D1::RectF(8.0f, h - 22.0f, w - 8.0f, h - 4.0f);
        m_rt->DrawTextW(buf, static_cast<UINT32>(wcslen(buf)),
                       m_hudTextFormat.Get(), r, m_brushLabel.Get());
    }

    // DIAGNOSTIC: selected entity's ENU + distance-from-origin, just above the
    // time/zoom line. Drawn in the red "error" brush so it stands out as a probe.
    if (m_hudTextFormat && !state.selectedReadout.empty())
    {
        ID2D1Brush* rb = m_brushSelectErr ? m_brushSelectErr.Get() : m_brushLabel.Get();
        D2D1_RECT_F r = D2D1::RectF(8.0f, h - 40.0f, w - 8.0f, h - 22.0f);
        m_rt->DrawTextW(state.selectedReadout.c_str(),
                        static_cast<UINT32>(state.selectedReadout.size()),
                        m_hudTextFormat.Get(), r, rb);
    }

    // Rubber-band group-selection box (right-drag).
    if (m_rbActive && m_rbMoved && m_brushSelect)
    {
        const D2D1_POINT_2F a = ClientToDip(m_rbStart);
        const D2D1_POINT_2F b = ClientToDip(m_rbCur);
        const D2D1_RECT_F box = D2D1::RectF((a.x < b.x ? a.x : b.x), (a.y < b.y ? a.y : b.y),
                                            (a.x < b.x ? b.x : a.x), (a.y < b.y ? b.y : a.y));
        m_rt->DrawRectangle(box, m_brushSelect.Get(), 1.0f, m_dashedStroke.Get());
    }

    // "Boundary" paint box (left-drag while armed): the yellow 3D-terrain boundary rectangle.
    // Drawn thicker than the selection box so it reads as the terrain footprint being marked.
    if (m_boundaryDragActive && m_brushSelect)
    {
        const D2D1_POINT_2F a = ClientToDip(m_boundaryStart);
        const D2D1_POINT_2F b = ClientToDip(m_boundaryCur);
        const D2D1_RECT_F box = D2D1::RectF((a.x < b.x ? a.x : b.x), (a.y < b.y ? a.y : b.y),
                                            (a.x < b.x ? b.x : a.x), (a.y < b.y ? b.y : a.y));
        m_rt->DrawRectangle(box, m_brushSelect.Get(), 2.5f, m_dashedStroke.Get());
    }

    // "Show Origin" marker: a big red X at the scenario origin (ENU 0,0). Drawn
    // pixel-sized (not metres) so it stays clearly visible whether zoomed far out
    // or all the way in, and drawn last so it sits on top of everything.
    if (state.showOrigin && m_brushSelectErr)
    {
        const D2D1_POINT_2F o = ProjectEnu(0.0, 0.0, state, w, h);
        const float r = 18.0f;   // half-arm length in pixels
        const float sw = 3.0f;   // stroke width
        m_rt->DrawLine(D2D1::Point2F(o.x - r, o.y - r),
                       D2D1::Point2F(o.x + r, o.y + r), m_brushSelectErr.Get(), sw);
        m_rt->DrawLine(D2D1::Point2F(o.x - r, o.y + r),
                       D2D1::Point2F(o.x + r, o.y - r), m_brushSelectErr.Get(), sw);
        // Hollow ring so the exact origin point reads clearly at any zoom.
        m_rt->DrawEllipse(D2D1::Ellipse(o, r + 3.0f, r + 3.0f), m_brushSelectErr.Get(), 1.5f);
    }

    HRESULT hr = m_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
}
