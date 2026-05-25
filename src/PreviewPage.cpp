#include "pch.h"
#include "PreviewPage.h"
#include "Scenario.h"
#include "MotionSampler.h"
#include "CoordTransforms.h"
#include "../log.h"

#include <algorithm>
#include <cmath>

namespace
{
    const FFieldHelp kFields[] = {
        { IDC_BTN_PREVIEW_START,       _T("Start the preview animation."), false },
        { IDC_BTN_PREVIEW_PAUSE,       _T("Pause the preview animation."), false },
        { IDC_BTN_PREVIEW_RESUME,      _T("Resume the paused preview animation."), false },
        { IDC_BTN_PREVIEW_STOP,        _T("Stop the preview and reset to time zero."), false },
        { IDC_BTN_PREVIEW_ZOOM_IN,     _T("Zoom the preview canvas in."), false },
        { IDC_BTN_PREVIEW_ZOOM_OUT,    _T("Zoom the preview canvas out."), false },
        { IDC_BTN_PREVIEW_FIT,         _T("Fit the entire scenario in the preview canvas."), false },
        { IDC_CHK_PREVIEW_LABELS,      _T("Show entity labels in the preview."), false },
        { IDC_CHK_PREVIEW_TRAILS,      _T("Show motion trails behind moving entities."), false },
        { IDC_CHK_PREVIEW_PATHS,       _T("Show configured motion paths."), false },
        { IDC_CHK_PREVIEW_ORIENTATION, _T("Show heading vectors on each entity."), false },
    };

    // Sample N evenly-spaced points along [0, duration] for FitScenario
    // and the per-entity motion-path cache.
    constexpr size_t kFitSampleCount  = 32;
    // (path sampling now lives per-segment inside RebuildPathsCache —
    // see kLinePts / kEllipsePts there)
}

BEGIN_MESSAGE_MAP(CPreviewPage, CHelpAwarePage)
    ON_WM_DESTROY()
    ON_WM_TIMER()
    ON_WM_HSCROLL()
    ON_WM_SHOWWINDOW()
    ON_BN_CLICKED(IDC_BTN_PREVIEW_START,       &CPreviewPage::OnStart)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_PAUSE,       &CPreviewPage::OnPause)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_RESUME,      &CPreviewPage::OnResume)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_STOP,        &CPreviewPage::OnStop)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_ZOOM_IN,     &CPreviewPage::OnZoomIn)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_ZOOM_OUT,    &CPreviewPage::OnZoomOut)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_FIT,         &CPreviewPage::OnFit)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_LABELS,      &CPreviewPage::OnLabelsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_TRAILS,      &CPreviewPage::OnTrailsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_PATHS,       &CPreviewPage::OnPathsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_ORIENTATION, &CPreviewPage::OnOrientationToggle)
END_MESSAGE_MAP()

void CPreviewPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

BOOL CPreviewPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    LOG("PreviewPage::OnInitDialog: entered");

    m_canvas.SetOwner(this);   // pointer-only; safe to set before subclass
    BOOL subOk = m_canvas.SubclassDlgItem(IDC_STATIC_PREVIEW_CANVAS, this);
    {
        char buf[256];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage: SubclassDlgItem result=%d, canvasHwnd=%p, scenario=%p",
                  (int)subOk, (void*)m_canvas.GetSafeHwnd(), (void*)m_scenario);
        LOG(buf);
    }

    CheckDlgButton(IDC_CHK_PREVIEW_LABELS,      m_showLabels      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_TRAILS,      m_showTrails      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_PATHS,       m_showPaths       ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_ORIENTATION, m_showOrientation ? BST_CHECKED : BST_UNCHECKED);

    if (CSliderCtrl* s = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME))
    {
        const int maxTick = (m_scenario && m_scenario->durationSeconds > 0.0)
            ? static_cast<int>(m_scenario->durationSeconds * 10.0) : 3000;
        s->SetRange(0, maxTick, TRUE);
        s->SetPos(0);
    }

    RebuildPathsCache();
    FitScenario();
    RebuildRenderState();

    return TRUE;
}

void CPreviewPage::OnDestroy()
{
    StopTimer();
    CHelpAwarePage::OnDestroy();
}

void CPreviewPage::OnShowWindow(BOOL bShow, UINT nStatus)
{
    CHelpAwarePage::OnShowWindow(bShow, nStatus);
    if (bShow && m_scenario)
    {
        // Re-fit whenever the preview becomes visible — covers the "load a
        // different scenario then switch tabs" case without forcing the user
        // to click Fit explicitly. Slider range may also need updating.
        if (CSliderCtrl* s = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME))
        {
            const int maxTick = (m_scenario->durationSeconds > 0.0)
                ? static_cast<int>(m_scenario->durationSeconds * 10.0) : 3000;
            s->SetRange(0, maxTick, TRUE);
        }
        RebuildPathsCache();
        FitScenario();
        RebuildRenderState();
        if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    }
}

void CPreviewPage::StartTimer()
{
    if (m_timerActive) return;
    SetTimer(kAnimTimerId, 33, nullptr);    // ~30 Hz
    m_timerActive = true;
}

void CPreviewPage::StopTimer()
{
    if (!m_timerActive) return;
    KillTimer(kAnimTimerId);
    m_timerActive = false;
}

void CPreviewPage::OnStart()
{
    if (!m_scenario || m_scenario->durationSeconds <= 0.0) return;
    if (m_runState == PreviewState::Playing) return;
    if (m_runState == PreviewState::Idle)
    {
        m_previewTimeSec = 0.0;
        m_trails.assign(m_scenario->entities.size(), {});
    }
    m_runState   = PreviewState::Playing;
    m_lastTickMs = GetTickCount64();
    StartTimer();
}

void CPreviewPage::OnPause()
{
    if (m_runState != PreviewState::Playing) return;
    m_runState = PreviewState::Paused;
    StopTimer();
}

void CPreviewPage::OnResume()
{
    if (m_runState != PreviewState::Paused) return;
    m_runState   = PreviewState::Playing;
    m_lastTickMs = GetTickCount64();
    StartTimer();
}

void CPreviewPage::OnStop()
{
    StopTimer();
    m_runState       = PreviewState::Idle;
    m_previewTimeSec = 0.0;
    m_trails.clear();
    UpdateSliderFromTime();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnTimer(UINT_PTR id)
{
    if (id != kAnimTimerId) { CHelpAwarePage::OnTimer(id); return; }
    if (!m_scenario || m_runState != PreviewState::Playing) return;

    const ULONGLONG now = GetTickCount64();
    const double dt = (now - m_lastTickMs) / 1000.0;
    m_lastTickMs = now;

    m_previewTimeSec += dt;
    const double dur = m_scenario->durationSeconds;
    if (dur > 0.0 && m_previewTimeSec >= dur)
    {
        // End of scenario — clamp to duration, stop animating, return to
        // Idle so the next Start restarts from t=0.
        m_previewTimeSec = dur;
        StopTimer();
        m_runState = PreviewState::Idle;
    }

    UpdateSliderFromTime();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnHScroll(UINT code, UINT pos, CScrollBar* sb)
{
    if (m_suppressScroll) { CHelpAwarePage::OnHScroll(code, pos, sb); return; }
    CSliderCtrl* slider = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME);
    if (sb && slider && sb->GetSafeHwnd() == slider->GetSafeHwnd())
    {
        const int p = slider->GetPos();
        m_previewTimeSec = p / 10.0;
        m_trails.clear();   // scrubbing invalidates trail history
        RebuildRenderState();
        if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
        return;
    }
    CHelpAwarePage::OnHScroll(code, pos, sb);
}

void CPreviewPage::OnZoomIn()
{
    m_zoomMetersPerPx /= 1.25;
    if (m_zoomMetersPerPx < 0.001) m_zoomMetersPerPx = 0.001;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnZoomOut()
{
    m_zoomMetersPerPx *= 1.25;
    if (m_zoomMetersPerPx > 1e9) m_zoomMetersPerPx = 1e9;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnFit()
{
    FitScenario();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::PanByPixels(int dxPx, int dyPx)
{
    // Screen +x = East (right); screen +y = South (down), so subtract dy
    // to convert into ENU North (which is screen-up).
    m_centerEnuE -= dxPx * m_zoomMetersPerPx;
    m_centerEnuN += dyPx * m_zoomMetersPerPx;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::ZoomAtPixel(double factor, int cursorXPx, int cursorYPx)
{
    if (factor <= 0.0) return;
    if (!m_canvas.GetSafeHwnd())
    {
        m_zoomMetersPerPx *= factor;
        if (m_zoomMetersPerPx < 0.001) m_zoomMetersPerPx = 0.001;
        if (m_zoomMetersPerPx > 1e9)   m_zoomMetersPerPx = 1e9;
        RebuildRenderState();
        return;
    }

    // World point currently under the cursor (canvas coords -> ENU).
    CRect rc; m_canvas.GetClientRect(&rc);
    const double w = std::max<LONG>(rc.Width(),  1);
    const double h = std::max<LONG>(rc.Height(), 1);
    const double oldZoom = m_zoomMetersPerPx;
    const double worldE = m_centerEnuE + (cursorXPx - w * 0.5) * oldZoom;
    const double worldN = m_centerEnuN - (cursorYPx - h * 0.5) * oldZoom;

    // New zoom, clamped.
    double newZoom = oldZoom * factor;
    if (newZoom < 0.001) newZoom = 0.001;
    if (newZoom > 1e9)   newZoom = 1e9;
    m_zoomMetersPerPx = newZoom;

    // Shift center so the same world point ends up back under the cursor.
    m_centerEnuE = worldE - (cursorXPx - w * 0.5) * newZoom;
    m_centerEnuN = worldN + (cursorYPx - h * 0.5) * newZoom;

    RebuildRenderState();
    m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnLabelsToggle()
{
    m_showLabels = IsDlgButtonChecked(IDC_CHK_PREVIEW_LABELS) == BST_CHECKED;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnTrailsToggle()
{
    m_showTrails = IsDlgButtonChecked(IDC_CHK_PREVIEW_TRAILS) == BST_CHECKED;
    if (!m_showTrails) m_trails.clear();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnPathsToggle()
{
    m_showPaths = IsDlgButtonChecked(IDC_CHK_PREVIEW_PATHS) == BST_CHECKED;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnOrientationToggle()
{
    m_showOrientation = IsDlgButtonChecked(IDC_CHK_PREVIEW_ORIENTATION) == BST_CHECKED;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::UpdateSliderFromTime()
{
    if (CSliderCtrl* s = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME))
    {
        m_suppressScroll = true;
        s->SetPos(static_cast<int>(m_previewTimeSec * 10.0 + 0.5));
        m_suppressScroll = false;
    }
}

void CPreviewPage::RebuildPathsCache()
{
    m_pathsCache.clear();
    if (!m_scenario) return;

    const size_t n = m_scenario->entities.size();
    m_pathsCache.resize(n);

    // Per-segment-type sample counts. Ellipse needs the most points so the
    // curve looks smooth; Line is a straight chord so 2 points suffice
    // (extra points are harmless). Stationary / StopHold contribute one
    // anchor point (the entity's resting position).
    constexpr size_t kLinePts       = 2;
    constexpr size_t kEllipsePts    = 96;
    constexpr size_t kStaticPts     = 1;

    for (size_t i = 0; i < n; ++i)
    {
        const Entity& e = m_scenario->entities[i];
        if (!e.enabled || e.motionSegments.empty()) continue;

        auto pushEnu = [&](const SampledPose& pose) {
            double east, north, up;
            CoordTransforms::EcefToLocalEnuDeg(
                pose.ecefX, pose.ecefY, pose.ecefZ,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
                east, north, up);
            m_pathsCache[i].push_back(D2D1::Point2F(
                static_cast<float>(east), static_cast<float>(north)));
        };

        for (const MotionSegment& s : e.motionSegments)
        {
            if (!s.enabled) continue;

            size_t nPts = kStaticPts;
            if (s.type == MotionType::Line)    nPts = kLinePts;
            if (s.type == MotionType::Ellipse) nPts = kEllipsePts;

            for (size_t k = 0; k < nPts; ++k)
            {
                const double u = (nPts <= 1)
                                  ? 0.0
                                  : static_cast<double>(k) / static_cast<double>(nPts - 1);
                // geometricMode=true: u ∈ [0,1] traces one full cycle of
                // the segment's shape (line start→end, or ONE ellipse
                // revolution), regardless of how many real orbits the
                // segment's time window contains. This kills the chord-
                // overlay "thick ring" artifact for many-orbit ellipses.
                const SampledPose pose = MotionSampler::EvaluateSegment(
                    s, u, m_scenario, /*geometricMode*/ true);
                pushEnu(pose);
            }
        }
    }
}

void CPreviewPage::FitScenario()
{
    if (!m_scenario || m_scenario->entities.empty())
    {
        m_zoomMetersPerPx = 1.0;
        m_centerEnuE = m_centerEnuN = 0.0;
        return;
    }

    double minE =  1e18, minN =  1e18;
    double maxE = -1e18, maxN = -1e18;
    const double dur = std::max(1.0, m_scenario->durationSeconds);

    for (const Entity& e : m_scenario->entities)
    {
        if (!e.enabled) continue;
        for (size_t k = 0; k < kFitSampleCount; ++k)
        {
            const double t = dur * static_cast<double>(k)
                           / static_cast<double>(kFitSampleCount - 1);
            const SampledPose pose = MotionSampler::SamplePose(e, *m_scenario, t);
            double east, north, up;
            CoordTransforms::EcefToLocalEnuDeg(
                pose.ecefX, pose.ecefY, pose.ecefZ,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
                east, north, up);
            if (east < minE) minE = east;
            if (east > maxE) maxE = east;
            if (north < minN) minN = north;
            if (north > maxN) maxN = north;
        }
    }

    if (minE > maxE || minN > maxN)
    {
        m_zoomMetersPerPx = 1.0;
        m_centerEnuE = m_centerEnuN = 0.0;
        return;
    }

    // 10% padding around the bounding box; ensure non-zero extent.
    double boxW = std::max(1.0, (maxE - minE)) * 1.10;
    double boxH = std::max(1.0, (maxN - minN)) * 1.10;
    m_centerEnuE = 0.5 * (minE + maxE);
    m_centerEnuN = 0.5 * (minN + maxN);

    CRect rc;
    if (m_canvas.GetSafeHwnd()) m_canvas.GetClientRect(&rc);
    const double w = std::max<LONG>(rc.Width(),  600);
    const double h = std::max<LONG>(rc.Height(), 400);

    m_zoomMetersPerPx = std::max(boxW / w, boxH / h);
    if (m_zoomMetersPerPx < 0.01) m_zoomMetersPerPx = 0.01;

    {
        char buf[400];
        sprintf_s(buf, sizeof(buf),
                  "FitScenario: origin=(%.4f,%.4f,%.1f) bbox E=[%.1f..%.1f] N=[%.1f..%.1f] "
                  "centerENU=(%.1f,%.1f) zoom=%.3f m/px canvas=%.0fx%.0f",
                  m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
                  minE, maxE, minN, maxN,
                  m_centerEnuE, m_centerEnuN, m_zoomMetersPerPx, w, h);
        LOG(buf);
    }
}

void CPreviewPage::RebuildRenderState()
{
    m_state.poses.clear();
    m_state.paths.clear();
    m_state.trails.clear();

    m_state.zoomMetersPerPx = m_zoomMetersPerPx;
    m_state.centerEnuE      = m_centerEnuE;
    m_state.centerEnuN      = m_centerEnuN;
    m_state.previewTimeSec  = m_previewTimeSec;
    m_state.durationSec     = m_scenario ? m_scenario->durationSeconds : 0.0;
    m_state.showLabels      = m_showLabels;
    m_state.showTrails      = m_showTrails;
    m_state.showPaths       = m_showPaths;
    m_state.showOrientation = m_showOrientation;

    if (!m_scenario) return;

    const size_t n = m_scenario->entities.size();
    if (m_trails.size() != n) m_trails.assign(n, {});

    const bool advanceTrail = (m_runState == PreviewState::Playing)
                            && (m_lastTrailTime < 0.0
                                || std::fabs(m_previewTimeSec - m_lastTrailTime) > 0.0);

    for (size_t i = 0; i < n; ++i)
    {
        const Entity& e = m_scenario->entities[i];
        if (!e.enabled) { m_state.poses.push_back({}); continue; }

        const SampledPose pose = MotionSampler::SamplePose(e, *m_scenario, m_previewTimeSec);
        double east, north, up;
        CoordTransforms::EcefToLocalEnuDeg(
            pose.ecefX, pose.ecefY, pose.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            east, north, up);

        PreviewEntityPose ep;
        ep.enuE       = east;
        ep.enuN       = north;
        ep.enuU       = up;
        ep.headingDeg = pose.headingDeg;
        ep.forceId    = e.forceId;
        ep.name       = e.marking.empty() ? e.name : e.marking;
        m_state.poses.push_back(std::move(ep));

        if (advanceTrail)
        {
            auto& tr = m_trails[i];
            tr.push_back(D2D1::Point2F(static_cast<float>(east),
                                       static_cast<float>(north)));
            if (tr.size() > kTrailMaxPoints)
                tr.erase(tr.begin(), tr.begin() + (tr.size() - kTrailMaxPoints));
        }
    }

    m_lastTrailTime = m_previewTimeSec;

    m_state.paths  = m_pathsCache;
    m_state.trails = m_trails;
}
