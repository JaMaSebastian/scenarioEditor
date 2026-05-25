#pragma once

#include "pch.h"

#include <cstdint>
#include <string>
#include <vector>

class CPreviewPage;

struct PreviewEntityPose
{
    double      enuE = 0.0;
    double      enuN = 0.0;
    double      enuU = 0.0;
    double      headingDeg = 0.0;
    uint8_t     forceId = 1;
    std::string name;
};

struct PreviewRenderState
{
    std::vector<PreviewEntityPose>             poses;
    std::vector<std::vector<D2D1_POINT_2F>>    paths;   // per entity, ENU meters
    std::vector<std::vector<D2D1_POINT_2F>>    trails;  // per entity, ENU meters

    double  zoomMetersPerPx = 1.0;
    double  centerEnuE      = 0.0;
    double  centerEnuN      = 0.0;
    double  previewTimeSec  = 0.0;
    double  durationSec     = 0.0;

    bool    showLabels      = true;
    bool    showTrails      = true;
    bool    showPaths       = true;
    bool    showOrientation = false;
};

class CPreviewCanvas : public CWnd
{
public:
    void SetOwner(CPreviewPage* owner) { m_owner = owner; }

protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint pt);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint pt);
    afx_msg void OnMouseMove(UINT nFlags, CPoint pt);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    afx_msg LRESULT OnNcHitTest(CPoint pt);
    DECLARE_MESSAGE_MAP()

private:
    bool EnsureResources();
    void DiscardDeviceResources();
    void Render(const PreviewRenderState& state);
    D2D1_POINT_2F ProjectEnu(double e, double n, const PreviewRenderState& s,
                             float canvasW, float canvasH) const;
    ID2D1SolidColorBrush* BrushForForce(uint8_t forceId);

    CPreviewPage*                                 m_owner = nullptr;

    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushGrid;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushLabel;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushForce[4]; // Other/Friendly/Opposing/Neutral
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushOutline;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle>      m_dashedStroke;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     m_textFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     m_hudTextFormat;

    // Left-drag pan state.
    bool    m_dragging   = false;
    CPoint  m_lastDragPt;
};
