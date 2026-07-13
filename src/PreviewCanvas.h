#pragma once

#include "pch.h"
#include "MapTileService.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

class CPreviewPage;

struct PreviewEntityPose
{
    double      enuE = 0.0;
    double      enuN = 0.0;
    double      enuU = 0.0;
    double      headingDeg = 0.0;
    uint8_t     forceId = 1;
    bool        enabled = true;   // disabled entities are not pickable/draggable
    std::string name;
};

// One pickable ellipse orbit: its sampled ENU polyline plus center (foci
// midpoint, ENU). Used by the canvas to hit-test "Set Start" clicks in the
// exact same projection it draws with (so picking is DPI-correct).
struct PreviewEllipse
{
    size_t                      entityIdx = 0;
    size_t                      segIdx    = 0;
    std::vector<D2D1_POINT_2F>  enuPts;            // scenario-origin ENU
    double                      centerE   = 0.0;
    double                      centerN   = 0.0;
};

// Draggable end-anchor ("pink dot") for a Line motion segment — the course
// destination the user drags into place. ENU = scenario-origin East/North.
struct PreviewLineAnchor
{
    size_t entityIdx = 0;
    size_t segIdx    = 0;
    double enuE      = 0.0;
    double enuN      = 0.0;
};

// Draggable focus handle of an ellipse orbit (two per Local Ellipse segment).
struct PreviewFocusHandle
{
    size_t entityIdx = 0;
    size_t segIdx    = 0;
    int    focusIdx  = 0;   // 0 = F1, 1 = F2
    double enuE      = 0.0;
    double enuN      = 0.0;
};

// Axis-aligned terrain footprint to paint under the entities, in scenario-
// origin ENU metres (East = X, North = Y). Mirrors Scenario.h's LevelRect,
// flattened for the render state.
struct PreviewLevelRect
{
    bool   enabled   = false;
    double eastMinM  = 0.0;
    double eastMaxM  = 0.0;
    double northMinM = 0.0;
    double northMaxM = 0.0;
};

// A named, tinted sub-region (forest, exclusion box, …) to outline and label
// over the terrain. Flattened mirror of Scenario.h's LevelZone.
struct PreviewLevelZone
{
    bool        enabled   = false;
    std::string name;
    double      eastMinM  = 0.0;
    double      eastMaxM  = 0.0;
    double      northMinM = 0.0;
    double      northMaxM = 0.0;
    uint32_t    colorRgb  = 0x2E7D32;  // 0xRRGGBB fill/outline tint
};

struct PreviewRenderState
{
    std::vector<PreviewEntityPose>             poses;
    std::vector<std::vector<D2D1_POINT_2F>>    paths;   // per entity, ENU meters
    std::vector<std::vector<D2D1_POINT_2F>>    trails;  // per entity, ENU meters
    std::vector<D2D1_POINT_2F>                 ellipseStarts; // ENU start point per ellipse segment
    std::vector<PreviewEllipse>                ellipses;      // pickable orbits (hit-testing)
    std::vector<PreviewLineAnchor>             lineAnchors;   // draggable Line end "pink dots"
    std::vector<bool>                          selected;      // group-selection flag per entity
    std::vector<bool>                          selectError;   // refresh-failed -> draw ring red
    std::vector<PreviewFocusHandle>            focusHandles;  // draggable ellipse foci (orange dots)

    double  zoomMetersPerPx = 1.0;
    double  centerEnuE      = 0.0;
    double  centerEnuN      = 0.0;
    double  previewTimeSec  = 0.0;
    double  durationSec     = 0.0;

    bool    showLabels      = true;
    bool    showTrails      = true;
    bool    showPaths       = true;
    bool    showOrientation = false;
    bool    showLegend      = false;  // size/direction legend overlay

    // Map backdrop: georeferenced raster tiles drawn under everything. Anchored
    // to the scenario origin so it lines up with the ENU entity positions.
    MapLayer mapLayer     = MapLayer::None;
    bool     mapOnline    = true;
    double   originLatDeg = 0.0;
    double   originLonDeg = 0.0;
    double   originAltM   = 0.0;

    // Background terrain overlay (off unless the scenario carries a [Level]).
    bool             showLevel = true;
    PreviewLevelRect levelLand;
    PreviewLevelRect levelOcean;
    std::vector<PreviewLevelZone> levelZones;   // named sub-regions, drawn over terrain
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
    afx_msg void OnRButtonDown(UINT nFlags, CPoint pt);
    afx_msg void OnRButtonUp(UINT nFlags, CPoint pt);
    afx_msg LRESULT OnNcHitTest(CPoint pt);
    afx_msg void OnDestroy();
    afx_msg LRESULT OnTileReady(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()

private:
    bool EnsureResources();
    void DiscardDeviceResources();
    void Render(const PreviewRenderState& state);
    // Draw the georeferenced map backdrop (visible tiles) behind everything.
    void DrawMap(const PreviewRenderState& state, float w, float h);
    // Device bitmap for a tile, created lazily from the tile service's pixels
    // (device-independent). Returns nullptr while the tile is still loading.
    ID2D1Bitmap* TileBitmap(MapLayer layer, int z, int x, int y);
    D2D1_POINT_2F ProjectEnu(double e, double n, const PreviewRenderState& s,
                             float canvasW, float canvasH) const;
    ID2D1SolidColorBrush* BrushForForce(uint8_t forceId);
    // "Set Start" hit-test: pxPt is a physical-pixel client click. Returns true
    // if it landed within range of an ellipse orbit (and applied the pick).
    bool PickStartAt(CPoint pxPt);

    // Inverse of ProjectEnu: map a physical-pixel client point to scenario-
    // origin ENU east/north (same DIP handling as PickStartAt).
    bool UnprojectToEnu(CPoint pxPt, double& outE, double& outN) const;
    // Nearest entity dot to a physical-pixel client point, or -1 if none is
    // within the pick radius. Skips disabled entities.
    int  HitTestEntity(CPoint pxPt) const;
    // Nearest Line end-anchor ("pink dot") to a physical-pixel point, or -1.
    int  HitTestLineAnchor(CPoint pxPt) const;
    // Entity whose drawn course (path polyline) is under a physical-pixel point,
    // or -1. Used to right-click a line and extend it.
    int  HitTestLineSegment(CPoint pxPt) const;
    // Nearest ellipse focus handle to a physical-pixel point, or -1.
    int  HitTestFocus(CPoint pxPt) const;
    // Nearest ellipse shape handle (orange dot on the curve at the orbit start)
    // to a physical-pixel point, or -1. Returns an index into state.ellipses.
    int  HitTestEllipseShape(CPoint pxPt) const;
    // Convert a physical-pixel client point to DIPs (the space ProjectEnu draws in).
    D2D1_POINT_2F ClientToDip(CPoint px) const;

    CPreviewPage*                                 m_owner = nullptr;

    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushGrid;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushLabel;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushForce[4]; // Other/Friendly/Opposing/Neutral
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushOutline;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushStart;    // ellipse start markers
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushLand;     // terrain footprint fill
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushOcean;    // water footprint fill
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushLevelEdge;// terrain outline
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushZone;     // per-zone fill/outline (color set at draw)
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushLegendStar; // pink "*" when a legend blurb can't fit on one line
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushSelect;     // group-selection ring + rubber-band box
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushSelectErr;  // red ring: Refresh Speed found no catalog cruise
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_brushFocus;      // ellipse focus handles (orange)
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle>      m_dashedStroke;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     m_textFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     m_hudTextFormat;

    // Left-drag pan state.
    bool    m_dragging   = false;
    CPoint  m_lastDragPt;

    // Entity-drag state ("grab a dot and move its start location"). Active only
    // when the owner reports the preview is stopped at t=0.
    bool    m_draggingEntity = false;
    int     m_dragEntityIdx  = -1;

    // Line end-anchor ("pink dot") drag state — the entity+segment whose Line
    // end is being dragged (stable identifiers; survive render-state rebuilds).
    bool    m_draggingAnchor   = false;
    int     m_dragAnchorEntity = -1;
    int     m_dragAnchorSeg    = -1;

    // Right-drag rubber-band group selection.
    bool    m_rbActive = false;
    bool    m_rbMoved  = false;
    CPoint  m_rbStart;
    CPoint  m_rbCur;

    // Left-drag "Boundary" paint (armed via the Preview tab Boundary button): a yellow
    // rectangle marking the 3D-terrain box; on release its corners → lat/lon → the page.
    bool    m_boundaryDragActive = false;
    CPoint  m_boundaryStart;
    CPoint  m_boundaryCur;

    // Ellipse focus-handle drag.
    bool    m_draggingFocus   = false;
    int     m_dragFocusEntity = -1;
    int     m_dragFocusSeg    = -1;
    int     m_dragFocusIdx    = -1;

    // Ellipse shape-handle drag (orange dot on the curve; re-sizes the orbit).
    bool    m_draggingShape   = false;
    int     m_dragShapeEntity = -1;
    int     m_dragShapeSeg    = -1;

    // Map backdrop: async tile service (device-independent pixels) + a device
    // bitmap cache keyed by packed (layer,z,x,y), FIFO-evicted.
    MapTileService                                                   m_tiles;
    bool                                                             m_tilesInit = false;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID2D1Bitmap>> m_tileBitmaps;
    std::deque<uint64_t>                                             m_tileBmpOrder;
};
