#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"
#include "PreviewCanvas.h"
#include "CesiumView.h"

#include <set>

struct Scenario;
struct Entity;
struct MotionSegment;

class CPreviewPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_PREVIEW_PAGE };
    CPreviewPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

    void SetScenario(Scenario* scenario) { m_scenario = scenario; }

    // Canvas reads this every paint to project entities onto the screen.
    const PreviewRenderState& GetRenderState() const { return m_state; }

    // "Set Start" pick mode. The canvas hit-tests the click and calls
    // ApplyStartPick with the chosen ellipse + snapped ENU point.
    bool IsStartPickArmed() const { return m_startPickArmed; }
    void ApplyStartPick(size_t entityIdx, size_t segIdx, double enuE, double enuN);

    // Drag-to-set-start. Entities are only draggable while the preview sits
    // stopped at t=0 (where the dot genuinely is the start location).
    bool IsEntityDragAllowed() const
    {
        return m_runState == PreviewState::Idle && m_previewTimeSec == 0.0;
    }
    // Live-mutate entity `idx`'s start location to scenario-origin ENU (e,n)
    // while keeping its current altitude, then rebuild the preview. Called per
    // mouse-move during a drag.
    void DragEntityTo(size_t idx, double enuE, double enuN);
    // Commit a finished drag: reload the other tabs from the model and flag the
    // scenario modified (so it persists into the .ini on Save).
    void EndEntityDrag();

    // Right-click context menu action: clone entity `idx`, give it a unique
    // EntityID and a "-N"-suffixed name, then refresh the preview + other tabs.
    void DuplicateEntity(size_t idx);

    // Right-click context menu action: prompt for a new label and rename entity `idx`.
    void RenameEntity(size_t idx);

    // Right-click context menu action: remove entity `idx` (keeps at least one).
    void DeleteEntity(size_t idx);

    // Plot > Line: give entity `idx` a straight-line course at its type's cruise
    // speed, on its conveyance plane (domain), with a draggable end-anchor.
    void PlotLineCourse(size_t idx);

    // Plot > Ellipse: give entity `idx` an orbit (clockwise/CCW) snapped to it,
    // with two draggable focus handles.
    void PlotEllipseCourse(size_t idx, bool clockwise);

    // Plot > Entity Ellipse: like PlotEllipseCourse, but the orbit's center
    // follows another entity (prompted by name) as it moves through the
    // scenario. Collapsing the two foci makes it a circle around the target.
    void PlotEntityEllipse(size_t idx, bool clockwise);
    // Drag an ellipse focus; the orbit re-forms to keep the entity on it.
    void DragEllipseFocus(size_t entityIdx, size_t segIdx, int focusIdx, double e, double n);

    // Drag the orbit's shape handle (the orange dot on the curve at its start) to
    // (e,n): the ellipse re-sizes to pass through that point and starts there.
    // If a Line precedes the orbit, its end follows so the path stays connected.
    void DragEllipseShapeHandle(size_t entityIdx, size_t segIdx, double e, double n);

    // Drag the pink end-anchor of entity `entityIdx`'s Line segment `segIdx` to
    // (enuE,enuN); EndLineEndDrag commits (reload tabs + mark dirty) on release.
    void DragLineEnd(size_t entityIdx, size_t segIdx, double enuE, double enuN);
    void EndLineEndDrag();

    // Right-click a course line > "Add Line": append a new Line leg starting where
    // the entity's last segment ends, continuing its direction, with a pink anchor.
    void AddLineToEnd(size_t entityIdx);

    // Right-click a course line > "Add Ellipse": append an orbit (CW/CCW) attached
    // to the entity's last segment end, with draggable foci.
    void AddEllipseToEnd(size_t entityIdx, bool clockwise);

    // Right-click a course line > "Add Entity Ellipse": append a follow-orbit
    // (CW/CCW) attached to the entity's last segment end, centered on a target
    // entity (prompted by name) that the orbit tracks as it moves.
    void AddEntityEllipseToEnd(size_t entityIdx, bool clockwise);

    // Right-click > "Set Duration...": show/change how long entity `idx`'s whole
    // motion path takes (rescales segment times + speeds to fit the new duration).
    void SetPathDuration(size_t idx);

    // Right-click > "Set Delay...": hold entity `idx` at its start for N seconds
    // before it begins moving, by shifting all its motion segments' time windows
    // forward so the first leg starts at t=N.
    void SetEntityDelay(size_t idx);

    // Group selection (right-drag rubber-band) + group commands.
    void SetSelectedEntities(const std::vector<size_t>& idxs);
    void ClearSelection();
    void SetGroupDuration();   // prompt once, apply to all selected entities
    void RefreshGroupSpeed();  // re-pull catalog cruise speed for all selected entities
    void RefreshEntitySpeed(size_t idx);  // re-pull catalog cruise speed for one entity

    // Pan / zoom inputs from the canvas (left-drag + scroll wheel).
    // PanByPixels shifts the view by an integer-pixel delta. ZoomAtPixel
    // anchors the zoom on a specific screen point so the world point
    // under the cursor stays put.
    void PanByPixels(int dxPx, int dyPx);
    void ZoomAtPixel(double factor, int cursorXPx, int cursorYPx);

protected:
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
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
    afx_msg void OnTerrainToggle();
    afx_msg void OnLegendToggle();
    afx_msg void OnSetStart();
    afx_msg void OnSpeedChange();
    afx_msg void OnDestTimeToggle();
    afx_msg void OnMapLayerChange();
    afx_msg void OnMapPlaceChange();
    afx_msg void OnMapAddPlace();
    afx_msg void OnMapDelPlace();
    afx_msg void OnMapCapturePlace();
    afx_msg void OnMapMoveEntitiesToggle();
    afx_msg void OnPropertiesItemChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDIS);
    DECLARE_MESSAGE_MAP()

private:
    enum class PreviewState { Idle, Playing, Paused };

    void RebuildRenderState();
    void RebuildPathsCache();
    // Map/location UI helpers.
    void PopulatePlacesCombo(int selectIdx = -1);   // fill from theApp.Settings()
    // Re-anchor the map/scenario origin to (lat,lon). When flyHeightM >= 0 and the
    // Cesium layer is active, the globe camera flies to that eye altitude (the
    // location's captured viewing height); otherwise the height is derived from
    // the current 2D zoom.
    void ApplyMapPlace(double lat, double lon, double flyHeightM = -1.0);
    void RefreshMapView();                           // rebuild + repaint after a map change
    // Cesium 3D globe helpers.
    void ActivateCesium(bool on);                    // show the globe vs the 2D canvas
    void PushCesiumEntities();                       // send current entity positions to the globe
    void FlyCesiumToOrigin();                         // aim the globe camera at the scenario origin
    double CurrentViewAltitude();                     // eye-height framing the current 2D view (m)
    void RefreshDestTimeList();   // fill the "Show Dest Time" grid from current motion
    // Time-axis length for the preview: the larger of the authored scenario
    // duration and the latest enabled segment end across all entities, so the
    // time bar/playback span the longest entity path even when entity durations
    // exceed Scenario.DurationSeconds.
    double EffectivePreviewDuration() const;
    void   UpdateSliderRange();   // resize the time bar to EffectivePreviewDuration()
    // Resolve an ellipse segment's focus (focus2 ? f2 : f1) to scenario-origin
    // ENU east/north, honoring the segment's coordMode.
    void FocusToEnu(const MotionSegment& s, bool focus2,
                    double& outE, double& outN) const;
    // Index into m_scenario->entities of the entity whose entityId == id, or -1.
    int  FindEntityIndexById(int entityId) const;
    // Prompt for a follow-target entity by name (Entity Ellipse). Returns its
    // index on OK with a valid, non-self match; false on cancel/empty/not-found
    // /self (message boxes shown as appropriate). `selfIdx` is the orbiter.
    bool PromptForTargetEntity(size_t selfIdx, size_t& outTargetIdx);
    // Current preview-ENU offset of a follow-ellipse's moving center relative to
    // its stored foci midpoint: (target current pos - stored midpoint). Returns
    // false when the segment isn't a follow ellipse or the target is missing.
    bool FollowCenterOffset(const MotionSegment& s, double& outDE, double& outDN) const;
    // Append ONE revolution of a follow-ellipse to m_state.paths[entityIdx],
    // re-centered on the target's position at the current preview time, so only
    // the current loop is drawn (and it updates as the entity/target move).
    void AppendFollowEllipsePath(size_t entityIdx, const MotionSegment& s);
    // DragEntityTo helpers: rewrite an entity's start location (all coord
    // representations) from scenario-origin ENU (east, north, up).
    void SetEntityInitialEnu(Entity& e, double east, double north, double up) const;
    void SetSegmentStartEnu(MotionSegment& s, double east, double north, double up) const;
    void SetEllipseStartBearing(size_t entityIdx, size_t segIdx,
                                MotionSegment& s, double east, double north) const;
    void FitScenario();
    // Pickable ellipse orbits, rebuilt with the paths cache; copied into the
    // render state so the canvas can hit-test them. Defined in PreviewCanvas.h.
    void UpdateSliderFromTime();
    void StartTimer();
    void StopTimer();

    // Dark theme (matches the Run / Deploy tabs): black page background, white
    // label text; brushes handed back from OnCtlColor.
    CBrush               m_blackBrush;
    CBrush               m_whiteBrush;

    Scenario*            m_scenario       = nullptr;
    CPreviewCanvas       m_canvas;
    CesiumView           m_cesium;        // 3D globe (shown when map layer = Cesium)
    PreviewState         m_runState       = PreviewState::Idle;
    double               m_previewTimeSec = 0.0;
    double               m_playSpeed      = 10.0;  // preview playback speed multiplier
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
    bool                 m_showTerrain     = true;   // [Level] overlay toggle
    MapLayer             m_mapLayer        = MapLayer::None;  // map backdrop layer
    bool                 m_mapOnline       = true;   // allow tile downloads
    bool                 m_moveEntitiesWithMap = false; // relocate the scene with a
                                                        // Location change (vs. leave
                                                        // entities world-fixed); off by default
    bool                 m_showLegend      = false;  // size/direction legend overlay
    bool                 m_showDestTime    = false;  // destination-time grid
    bool                 m_suppressPropsRefresh = false;  // guard: don't repopulate the
                                                          // grid while handling its own
                                                          // selection notification

    bool                 m_startPickArmed  = false;  // armed by Set Start; shows green "?"
    std::vector<PreviewEllipse> m_ellipseTargets;
    std::set<size_t>            m_selected;       // group-selected entity indices
    std::set<size_t>            m_refreshFailed;  // selected entities w/ no catalog cruise (red ring)

    PreviewRenderState                                m_state;
    std::vector<std::vector<D2D1_POINT_2F>>           m_pathsCache;   // per entity, ENU points
    std::vector<std::vector<D2D1_POINT_2F>>           m_trails;       // per entity, ENU ring buf
    double                                            m_lastTrailTime = -1.0;

    static constexpr UINT_PTR kAnimTimerId    = 0x101;
    static constexpr size_t   kTrailMaxPoints = 180;
};
