#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"
#include "PreviewCanvas.h"

struct Scenario;

class CPreviewPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_PREVIEW_PAGE };
    CPreviewPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

    void SetScenario(const Scenario* scenario) { m_scenario = scenario; }

    // Canvas reads this every paint to project entities onto the screen.
    const PreviewRenderState& GetRenderState() const { return m_state; }

protected:
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;
    afx_msg void OnDestroy();
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnHScroll(UINT code, UINT pos, CScrollBar* sb);
    afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
    afx_msg void OnStart();
    afx_msg void OnPause();
    afx_msg void OnResume();
    afx_msg void OnStop();
    afx_msg void OnZoomIn();
    afx_msg void OnZoomOut();
    afx_msg void OnFit();
    afx_msg void OnLabelsToggle();
    afx_msg void OnTrailsToggle();
    afx_msg void OnPathsToggle();
    afx_msg void OnOrientationToggle();
    DECLARE_MESSAGE_MAP()

private:
    enum class PreviewState { Idle, Playing, Paused };

    void RebuildRenderState();
    void RebuildPathsCache();
    void FitScenario();
    void UpdateSliderFromTime();
    void StartTimer();
    void StopTimer();

    const Scenario*      m_scenario       = nullptr;
    CPreviewCanvas       m_canvas;
    PreviewState         m_runState       = PreviewState::Idle;
    double               m_previewTimeSec = 0.0;
    ULONGLONG            m_lastTickMs     = 0;
    bool                 m_timerActive    = false;
    bool                 m_suppressScroll = false;

    double               m_zoomMetersPerPx = 1.0;
    double               m_centerEnuE      = 0.0;
    double               m_centerEnuN      = 0.0;

    bool                 m_showLabels      = true;
    bool                 m_showTrails      = true;
    bool                 m_showPaths       = true;
    bool                 m_showOrientation = false;

    PreviewRenderState                                m_state;
    std::vector<std::vector<D2D1_POINT_2F>>           m_pathsCache;   // per entity, ENU points
    std::vector<std::vector<D2D1_POINT_2F>>           m_trails;       // per entity, ENU ring buf
    double                                            m_lastTrailTime = -1.0;

    static constexpr UINT_PTR kAnimTimerId    = 0x101;
    static constexpr size_t   kTrailMaxPoints = 180;
};
