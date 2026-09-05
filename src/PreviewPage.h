//=============================================================================
//  PreviewPage.h
//-----------------------------------------------------------------------------
//  Declares CPreviewPage, the ScenarioEditor "Preview" tab: a top-down 2D
//  canvas (with an optional Cesium 3D globe) that animates a scenario over
//  time and lets the operator author entities and their motion paths --
//  drag start points, plot line/ellipse/take-off/follow courses, paint a
//  terrain boundary, relocate the map origin, and drive playback.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"
#include "PreviewCanvas.h"
#include "CameraTimeline.h"
#include "CesiumView.h"
#include "ProcessSupervisor.h"
#include "TerrainServiceClient.h"
#include "HangerCameraTypeMap.h"   // entity tuple -> Cameras.ini type key

#include <deque>
#include <set>
#include <string>
#include <vector>

struct Scenario;
struct Entity;
struct MotionSegment;
struct CameraFrame;
class  CEntityCameraDialog;
enum class CameraTransition : uint8_t;

//-----------------------------------------------------------------------------
// CPreviewPage — the "Preview" property page (dialog tab)
//   Owns the 2D preview canvas and the Cesium 3D view, holds the animation
//   clock/zoom/pan state, and mutates the shared Scenario in response to
//   canvas gestures and toolbar/menu commands. Rebuilds a PreviewRenderState
//   that the canvas reads each paint.
//-----------------------------------------------------------------------------
class CPreviewPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_PREVIEW_PAGE };
    CPreviewPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent)
    {
        // Both supervisors post WM_APP_PROC_EXIT to this page; the tag says which.
        m_proc.SetTag(kTagBuild);
        m_server.SetTag(kTagServer);
    }

    void SetScenario(Scenario* scenario) { m_scenario = scenario; }

    // A different scenario was loaded (or File > New reset the model): re-seed
    // the four view drop-downs from its [Preview] section. Called by the host
    // dialog's RefreshUiFromScenario; safe before the page has a window.
    void OnScenarioLoaded();

    // Canvas reads this every paint to project entities onto the screen.
    const PreviewRenderState& GetRenderState() const { return m_state; }

    // "Set Start" pick mode. The canvas hit-tests the click and calls
    // ApplyStartPick with the chosen ellipse + snapped ENU point.
    bool IsStartPickArmed() const { return m_startPickArmed; }
    void ApplyStartPick(size_t entityIdx, size_t segIdx, double enuE, double enuN);

    // "Boundary" paint mode. While armed the canvas draws a yellow drag-rectangle;
    // on release it converts the box corners to lat/lon and calls OnBoundaryPainted,
    // which records the 3D-terrain box on the scenario and disarms (the rectangle vanishes).
    bool IsBoundaryArmed() const { return m_boundaryArmed; }
    void OnBoundaryPainted(double latMinDeg, double latMaxDeg, double lonMinDeg, double lonMaxDeg);

    // "Foliage Area" paint mode. Same right-drag gesture as Boundary, but each
    // drag APPENDS a rectangle instead of replacing one, because foliage is a
    // list of areas rather than a single box.
    bool IsFoliageAreaArmed() const { return m_foliageAreaArmed; }
    void OnFoliageAreaPainted(double latMinDeg, double latMaxDeg, double lonMinDeg, double lonMaxDeg);
    // Canvas right-click actions on a painted area.
    void SetFoliageAreaCount(size_t idx);
    void DeleteFoliageArea(size_t idx);
    void DeleteAllFoliageAreas();

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

    // ----- Camera system (Preview-tab authoring) -----
    // Canvas drag of a Stationary camera box: live-move camera frame `frameIdx`'s
    // vantage to scenario-origin ENU (e,n); EndCameraDrag commits (mark dirty).
    void DragCameraTo(size_t frameIdx, double enuE, double enuN);
    void EndCameraDrag();
    // Level-zone box drag: recenter zone `idx` on ENU (centerE, centerN), keeping
    // its size; EndZoneDrag commits (reload tabs + mark dirty).
    void DragZoneTo(size_t idx, double centerE, double centerN);
    void EndZoneDrag();
    // Camera-box RMB actions (canvas box and timeline box alike). Everything a
    // camera carries is now edited in one modal — see EditCameraViaDialog — so
    // there are no per-field setters here any more.
    void DeleteCamera(size_t frameIdx);
    // Re-resolve every frame's cached target dimensions from the entity catalog.
    void RefreshCameraTargetExtents();

    // Entity-dot "Delete Camera": count the Entity cameras mounted on the entity
    // at `entityIdx`, and remove them all (each drops its timeline box too, since
    // the schedule and the boxes are the same list).
    size_t EntityCameraCount(size_t entityIdx) const;
    void   DeleteCamerasForEntity(size_t entityIdx);

    // Entity-dot "New Camera..." path: pop the New Camera modal with the clicked
    // entity preselected as the source. The button path (OnNewCamera) opens the
    // same modal with no source, and both finish in CreateCameraFromDialog.
    void NewEntityCameraViaDialog(size_t entityIdx);

    // Shared tail of both creation paths: build an Entity camera, or (source 0) a
    // source-less Stationary one, from an accepted New Camera dialog.
    void CreateCameraFromDialog(const CEntityCameraDialog& dlg);

    // Camera-box "Edit Camera...": the same modal, seeded from the clicked frame.
    // Takes an index, but treats it as a hint — see UpdateCameraFromDialog.
    void EditCameraViaDialog(size_t frameIdx);

    // Apply an accepted Edit Camera dialog onto an existing frame. `before` is the
    // pre-modal snapshot: it re-locates the frame if the vector was reordered while
    // the modal was up, and decides whether the label was ever customised.
    void UpdateCameraFromDialog(size_t frameIdx, const CEntityCameraDialog& dlg,
                                const CameraFrame& before);

    // Write the dialog's gimbal envelope to both copies of Cameras.ini and reload
    // the catalog. No-op unless the operator actually changed it. MUST be called
    // after dlg.PresetIndex() has been consumed — the reload invalidates it.
    void ApplyEnvelopeFromDialog(const CEntityCameraDialog& dlg);

    // "<entity> / <type> / <angle>" — the name an Entity camera gets when nobody
    // has renamed it. Shared by the create and edit paths so the edit path can tell
    // an untouched auto-name from one the operator typed.
    std::string AutoEntityCameraLabel(uint16_t srcEntityId,
                                      const std::string& presetType,
                                      const std::string& presetAngle) const;

    void AddEntityCameraFrame(size_t entityIdx, size_t presetIndex,
                              uint16_t targetEntityId, CameraTransition transition,
                              bool zoomEnabled = false, double zoomFovDeg = 90.0,
                              bool dynamicZoom = false, double zoomFillPct = 85.0,
                              bool limitsEnabled = false,
                              double transitionSeconds = 0.5,
                              const std::string& label = std::string());
    // (kept for the timeline/other callers)
    size_t  CameraPresetCount() const;
    CString CameraPresetLabel(size_t i) const;

    // Camera-timeline interface (read + edit the schedule; keep it contiguous).
    const std::vector<CameraFrame>* GetCameras() const;
    std::vector<CameraFrame>*       MutableCameras();
    double GetPreviewDuration() const { return EffectivePreviewDuration(); }
    double GetPreviewTime()     const { return m_previewTimeSec; }
    // Preview playback rate from the "Speed:" combo. The timeline divides each
    // camera's scenario span by this to show how long the view is really up on
    // screen. Guarded so a bad combo state can never divide by zero.
    double GetPlaySpeed()       const { return m_playSpeed > 1e-9 ? m_playSpeed : 1.0; }
    void   ScrubToTime(double t);            // set the playback time from a timeline click
    void   OnTimelineFrameEditing();         // live refresh during a box move/resize
    void   OnTimelineEdited();               // commit a box move/resize (normalize + dirty)

    // Right-click context menu action: clone entity `idx`, give it a unique
    // EntityID and a "-N"-suffixed name, then refresh the preview + other tabs.
    void DuplicateEntity(size_t idx);

    // Right-click context menu action: prompt for a new label and rename entity `idx`.
    void RenameEntity(size_t idx);

    // Right-click context menu action: open the catalog tree picker and re-type
    // entity `idx` to the chosen subcategory, re-deriving its speed/attributes
    // and updating its label to the new type's name.
    void ChangeEntity(size_t idx);

    // Right-click context menu action: remove entity `idx` (keeps at least one).
    void DeleteEntity(size_t idx);

    // Plot > Line: give entity `idx` a straight-line course at its type's cruise
    // speed, on its conveyance plane (domain), with a draggable end-anchor.
    void PlotLineCourse(size_t idx);

    // Plot > Take-off Line: like PlotLineCourse, but the entity starts STOPPED and
    // accelerates to cruise by the end of the line (constant acceleration). Only
    // meaningful for fixed-wing aircraft (Platform/Air) — see CanTakeoff.
    void PlotTakeoffLine(size_t idx);

    // True when entity `idx` is a Platform (kind 1) in the Air domain (2), i.e.
    // eligible for a take-off line. Drives whether the context menu offers it.
    bool CanTakeoff(size_t idx) const;

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

    // After editing one leg's timing, re-glue every following enabled segment so
    // the whole course stays time-contiguous (no frozen inter-segment gap).
    void RechainFollowingTimes(Entity& e, size_t fromIdx);

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
    afx_msg void OnNewEntity();   // "New" button: drop a default octopus drone
    afx_msg void OnNewCamera();   // "New Cam..." button: New Camera modal, no source
    // Lowest-numbered "Camera N" label not already used by an existing frame, so
    // deleting then re-adding stationary cameras never produces duplicate names.
    std::string NextStationaryCameraLabel() const;
    afx_msg void OnAddCamera();   // "Add" button: create an entity camera from the dropdowns
    afx_msg void OnLabelsToggle();
    afx_msg void OnTrailsToggle();
    afx_msg void OnPathsToggle();
    afx_msg void OnOrientationToggle();
    afx_msg void OnTerrainToggle();
    afx_msg void OnZonesToggle();

    // The view drop-downs (Map / Location / Speed / Scale) and the "Show" check
    // boxes belong to the SCENARIO, not the user, so they round-trip through
    // scenario.ini's [Preview] section rather than settings.ini:
    //   PersistPreviewView           — controls -> m_scenario->preview, + mark dirty.
    //   ApplyPreviewViewFromScenario — m_scenario->preview -> controls + state.
    // Apply never marks dirty and never re-applies the Location's coordinates:
    // the origin in [Origin] is authoritative, the label only re-selects the row.
    void PersistPreviewView();
    void ApplyPreviewViewFromScenario();

    // The "Show" boxes are BS_OWNERDRAW (see DrawCheckBox). A BS_OWNERDRAW button
    // neither toggles itself on a click nor stores a check state — BM_SETCHECK is
    // documented as having no effect on one — so the m_show* members ARE the
    // state, and these three are how it is read, written and drawn:
    //   ToggleCheck   — flip `flag`, repaint the box, persist to the scenario.
    //   RepaintCheck  — redraw after the caller has set the member (restore path).
    //   CheckBoxState — the member behind a control id, for the painter.
    void ToggleCheck(int ctrlId, bool& flag);
    void RepaintCheck(int ctrlId);
    bool CheckBoxState(int ctrlId) const;

    // Paint one owner-draw check box: a box drawn at kCheckScale x the size
    // Windows would use, plus the control's label, in the page's dark theme.
    void DrawCheckBox(LPDRAWITEMSTRUCT dis);
    afx_msg void OnLegendToggle();
    afx_msg void OnShowOriginToggle();   // toggle: recenter view on ENU(0,0) + draw a red X there
    afx_msg void OnGotoOrigin();         // recenter view on ENU(0,0) at the current zoom/height
    afx_msg void OnSetStart();
    afx_msg void OnBoundary();
    afx_msg void OnFoliage();   // "Foliage" button: pick tree types + rendering backend
    afx_msg void OnBuildTerrain();
    afx_msg void OnBuildFoliage();   // "Build i3dm Foliage": bake the streamed i3dm tree tileset
    afx_msg void OnPurgeTerrain();
    afx_msg void OnFoliageArea();
    afx_msg void OnStartTerrainServer();
    afx_msg void OnStopTerrainServer();
    afx_msg void OnSpeedChange();
    afx_msg void OnCamScaleChange();   // time-scale dropdown for the camera strip
    afx_msg void OnDestTimeToggle();
    afx_msg void OnMapLayerChange();
    afx_msg void OnMapPlaceChange();
    afx_msg void OnMapAddPlace();
    afx_msg void OnMapDelPlace();
    afx_msg void OnMapCapturePlace();
    afx_msg void OnMapMoveEntitiesToggle();
    afx_msg void OnPropertiesItemChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDIS);
    // Supervised-child plumbing: one line of piped stdout/stderr, and the
    // process exit with its real code (see ProcessSupervisor.h).
    afx_msg LRESULT OnProcOutput(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnProcExit(WPARAM wParam, LPARAM lParam);
    // Same two-message shape as the supervisor, but sourced from the terrain
    // service's HTTP job API instead of a child process's pipe.
    afx_msg LRESULT OnTerrainJobLine(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnTerrainJobDone(WPARAM wParam, LPARAM lParam);
    afx_msg void OnCamEntityChanged();   // repoint the preset list at the new entity
    DECLARE_MESSAGE_MAP()

private:
    enum class PreviewState { Idle, Playing, Paused };

    // Recompute m_state (poses/paths/trails/overlays) from the model at the
    // current preview time, then push entities to the globe.
    void RebuildRenderState();
    // Rebuild the cached per-entity motion-path polylines + pickable ellipse
    // targets (call after any edit that changes geometry).
    void RebuildPathsCache();
    // Camera helpers.
    void PopulateCameraCombos();   // fill the entity/preset/target/transition dropdowns
    // Refill the preset dropdown for whichever entity is selected: that entity's own
    // camera type first (and selected), everything else below a separator. Call on any
    // change to the entity combo, or the author can mount another airframe's calibration
    // without noticing — an F22 preset on a Hornet resolves and applies perfectly happily.
    void RefreshCameraPresetCombo();
    // Camera type key ("F-18", "GARC", ...) for the entity currently selected in
    // IDC_COMBO_CAM_ENTITY, or "" when none is selected or the hanger map is unavailable.
    std::string SelectedCameraEntityTypeKey() const;
    void AddCameraFrame(CameraFrame&& f);   // split the schedule at the scrub time
    void NormalizeCameraSchedule();         // clamp, sort, and de-overlap the schedule
    // Sync the camera-strip horizontal scroll bar (range/page/pos) to the current
    // time scale + duration; disable it when the whole schedule fits on screen.
    void UpdateCameraScrollBar();
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
    // Current map view center as geodetic lat/lon + eye altitude: the Cesium
    // globe's look-at point when the 3D layer is active, else the 2D canvas center
    // resolved against the origin. Falls back to the scenario origin when nothing
    // better is available. Used to prefill the Add/Capture Location dialog.
    void CurrentViewLatLonAlt(double& lat, double& lon, double& alt);
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
    // Auto-zoom/pan so the whole scenario (and any terrain overlay) fits the canvas.
    void FitScenario();
    // Self-hosted Cesium terrain server (WSL python3 serve.py on :8088) controls.
    // Resolve DISBrowser\Scripts\<fileName> from the configured project dir (Run tab).
    CString ResolveDisBrowserScript(const CString& fileName) const;
    // Probe/refresh whether the terrain tile server is answering on localhost:8088,
    // then enable/disable the Boundary + Build 3D Terrain + Stop buttons and repaint
    // the Start button's green "running" check accordingly.
    void RefreshTerrainServerUi();
    // Pickable ellipse orbits, rebuilt with the paths cache; copied into the
    // render state so the canvas can hit-test them. Defined in PreviewCanvas.h.
    // Push m_previewTimeSec onto the time slider (guarded against re-entrant scroll).
    void UpdateSliderFromTime();
    // Start / stop the ~30 Hz animation timer.
    void StartTimer();
    void StopTimer();

    // Dark theme (matches the Run / Deploy tabs): black page background, white
    // label text; brushes handed back from OnCtlColor.
    CBrush               m_blackBrush;
    CBrush               m_whiteBrush;

    Scenario*            m_scenario       = nullptr;
    CPreviewCanvas       m_canvas;
    CCameraTimeline      m_timeline;      // camera schedule strip under the canvas
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
    bool                 m_showTerrain     = true;   // [Level] land/ocean footprint toggle
    bool                 m_showZones       = true;   // [Level] named-zone (e.g. Palm Forest) toggle
    MapLayer             m_mapLayer        = MapLayer::None;  // map backdrop layer
    bool                 m_mapOnline       = true;   // allow tile downloads
    bool                 m_moveEntitiesWithMap = false; // relocate the scene with a
                                                        // Location change (vs. leave
                                                        // entities world-fixed); off by default
    bool                 m_showLegend      = false;  // size/direction legend overlay
    bool                 m_showOrigin      = false;  // toggle: mark scenario origin (ENU 0,0) with a red X
    bool                 m_showDestTime    = false;  // destination-time grid

    // Entity drag-drop diagnostic: DragEntityTo caches the last drop here so
    // EndEntityDrag can log the drop ENU vs. the resulting streamed t=0 pose.
    int                  m_dragDiagIdx     = -1;
    double               m_dragDiagEnuE    = 0.0;
    double               m_dragDiagEnuN    = 0.0;
    bool                 m_suppressPropsRefresh = false;  // guard: don't repopulate the
                                                          // grid while handling its own
                                                          // selection notification

    bool                 m_startPickArmed  = false;  // armed by Set Start; shows green "?"
    bool                 m_boundaryArmed   = false;  // armed by Boundary; shows green check, paints box
    bool                 m_foliageAreaArmed = false; // armed by Foliage Area; each drag appends a rect
    bool                 m_terrainServerUp = false;  // localhost:8088 answering; drives the Start check + gating

    // --- supervised external work (replaces the old fire-and-forget ShellExecutes) ---
    ProcessSupervisor    m_proc;          // the running build/purge/publish, if any
    ProcessSupervisor    m_server;        // TerrainServer.exe; deliberately NOT killed on exit
    CString              m_jobLabel;      // "Build 3D Terrain" etc., for status + error titles
    std::deque<CString>  m_jobTail;       // last N output lines, shown if the job fails
    std::deque<CString>  m_serverTail;    // same, for the long-running server
    // Tags distinguish the two supervisors in WM_APP_PROC_EXIT (see ProcessSupervisor.h).
    static const WORD    kTagBuild  = 1;
    static const WORD    kTagServer = 2;
    bool                 m_publishPending = false;  // run the tile publish once tiling succeeds

    // --- Mode=Service: the work happens in a container we do not own ---
    TerrainServiceClient m_svc;           // submits + polls; no child process involved
    bool                 m_jobActive = false;   // an HTTP job is in flight
    CString              m_svcHealth;     // last /api/v1/health status ("healthy"/"degraded")
    int                  m_healthTick = 0; // divides the 1.5s probe down to a ~5s health call
    void  StartServiceJob(const CString& label, const std::string& jsonBody);
    bool  ServiceMode() const;

    void  StartSupervised(const CString& label, const CString& commandLine,
                          const CString& workingDir);
    void  SetJobStatus(const CString& text);
    // One failure path for both job sources, so the modal a user sees is
    // identical whether the work ran locally or in the container.
    void  FailJob(const CString& label, const CString& detail);
    void  UpdateBuildButtons();           // disable build buttons while a job is in flight
    CString TerrainTilesDir() const;      // NTFS folder TerrainServer.exe serves from
    // Single source of truth for "which server, where" across the three
    // [Terrain] Mode values — probe target, Startup.ini URL and status text all
    // read these rather than assuming localhost:8088.
    CStringA       TerrainHost() const;
    unsigned short TerrainPort() const;
    CString        TerrainModeName() const;
    CString WslPathOf(const CString& winPath) const;   // C:\x -> /mnt/c/x
    std::vector<PreviewEllipse> m_ellipseTargets;
    std::set<size_t>            m_selected;       // group-selected entity indices
    std::set<size_t>            m_refreshFailed;  // selected entities w/ no catalog cruise (red ring)

    PreviewRenderState                                m_state;
    std::vector<std::vector<D2D1_POINT_2F>>           m_pathsCache;   // per entity, ENU points
    std::vector<std::vector<D2D1_POINT_2F>>           m_trails;       // per entity, ENU ring buf
    double                                            m_lastTrailTime = -1.0;

    static constexpr UINT_PTR kAnimTimerId    = 0x101;
    static constexpr UINT_PTR kServerPollTimerId = 0x102;   // polls localhost:8088 for the terrain server
    static constexpr size_t   kTrailMaxPoints = 180;
};
