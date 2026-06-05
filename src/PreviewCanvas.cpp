#include "pch.h"
#include "PreviewCanvas.h"
#include "PreviewPage.h"
#include "Direct2DContext.h"
#include "../log.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr float kEntityDotRadiusPx = 5.0f;
    constexpr float kOrientationLenPx  = 18.0f;
    constexpr float kLabelOffsetPx     = 9.0f;

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
    ON_WM_NCHITTEST()
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

    m_dragging   = true;
    m_lastDragPt = pt;
    SetCapture();
    CWnd::OnLButtonDown(nFlags, pt);
}

void CPreviewCanvas::OnLButtonUp(UINT nFlags, CPoint pt)
{
    if (m_dragging)
    {
        m_dragging = false;
        ReleaseCapture();
    }
    CWnd::OnLButtonUp(nFlags, pt);
}

void CPreviewCanvas::OnMouseMove(UINT nFlags, CPoint pt)
{
    if (m_dragging && m_owner)
    {
        const int dx = pt.x - m_lastDragPt.x;
        const int dy = pt.y - m_lastDragPt.y;
        m_lastDragPt = pt;
        if (dx != 0 || dy != 0) m_owner->PanByPixels(dx, dy);
    }
    CWnd::OnMouseMove(nFlags, pt);
}

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

BOOL CPreviewCanvas::OnEraseBkgnd(CDC*)
{
    // D2D paints the full client area; suppress GDI erase to avoid flicker.
    return TRUE;
}

void CPreviewCanvas::OnSize(UINT nType, int cx, int cy)
{
    CWnd::OnSize(nType, cx, cy);
    if (m_rt)
        m_rt->Resize(D2D1::SizeU(static_cast<UINT32>(cx), static_cast<UINT32>(cy)));
}

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

    m_rt->CreateSolidColorBrush(MakeRgb(0xF8F8F8), &m_brushBg);
    m_rt->CreateSolidColorBrush(MakeRgb(0xCCCCCC), &m_brushGrid);
    m_rt->CreateSolidColorBrush(MakeRgb(0x222222), &m_brushLabel);
    m_rt->CreateSolidColorBrush(MakeRgb(0xFFFFFF), &m_brushOutline);
    m_rt->CreateSolidColorBrush(MakeRgb(0x00A000), &m_brushStart);   // green

    // Force-ID palette: 0 Other, 1 Friendly, 2 Opposing, 3 Neutral
    m_rt->CreateSolidColorBrush(MakeRgb(0x808080), &m_brushForce[0]);
    m_rt->CreateSolidColorBrush(MakeRgb(0x1E78D2), &m_brushForce[1]);
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

    return true;
}

void CPreviewCanvas::DiscardDeviceResources()
{
    m_rt.Reset();
    m_brushBg.Reset();
    m_brushGrid.Reset();
    m_brushLabel.Reset();
    m_brushOutline.Reset();
    m_brushStart.Reset();
    for (auto& b : m_brushForce) b.Reset();
}

ID2D1SolidColorBrush* CPreviewCanvas::BrushForForce(uint8_t forceId)
{
    if (forceId >= 4) forceId = 0;
    return m_brushForce[forceId].Get();
}

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

void CPreviewCanvas::Render(const PreviewRenderState& state)
{
    if (!m_rt) return;

    const D2D1_SIZE_F rtSize = m_rt->GetSize();
    const float w = rtSize.width;
    const float h = rtSize.height;

    m_rt->BeginDraw();
    m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    m_rt->Clear(MakeRgb(0xF8F8F8));

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
            const float oldA = b->GetOpacity();
            b->SetOpacity(0.35f);
            D2D1_POINT_2F prev = ProjectEnu(path[0].x, path[0].y, state, w, h);
            for (size_t k = 1; k < path.size(); ++k)
            {
                D2D1_POINT_2F cur = ProjectEnu(path[k].x, path[k].y, state, w, h);
                m_rt->DrawLine(prev, cur, b, 1.2f, m_dashedStroke.Get());
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

    HRESULT hr = m_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
}
