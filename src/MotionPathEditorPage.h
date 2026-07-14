//=============================================================================
//  MotionPathEditorPage.h
//-----------------------------------------------------------------------------
//  Declares CMotionPathEditorPage, the dialog page for authoring an entity's
//  ordered list of motion segments (Stationary, Stop/Hold, Line, Ellipse).
//  Provides per-segment geometry/timing editing across Lat-Lon-Alt, Local, and
//  ECEF coordinate frames, backed directly by the shared Scenario model.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

struct Scenario;
struct Entity;
struct MotionSegment;

//-----------------------------------------------------------------------------
// CMotionPathEditorPage — Motion Path Editor tab of the Attributes dialog
//   Edits the selected entity's motion segments in place on the shared
//   Scenario. Handles the entity selector combo, the segment timeline list,
//   and the type-specific geometry fields (Line vs Ellipse), including
//   coordinate-frame conversion when the segment's coord mode changes.
//-----------------------------------------------------------------------------
class CMotionPathEditorPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_MOTION_PATH_EDITOR_PAGE };
    CMotionPathEditorPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

    void SetScenario(Scenario* scenario) { m_scenario = scenario; }

    // Rebuild the entity selector combo + segment list from the model.
    void Refresh();

    // Flush the in-progress UI edits into the active segment. Called from
    // the main dialog's CaptureUiIntoScenario() before Save / Validate /
    // Playback Start so pending edits aren't lost.
    void CommitPendingEdits() { CommitActiveSegmentFromUi(); }

protected:
    //
    // OnInitDialog — populate the type/coord/direction combos and the segment
    // timeline list-control columns.
    //
    BOOL OnInitDialog() override;
    //
    // GetFieldHelpTable — hand the base class this page's field-help entries.
    //
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;

    //
    // OnShowWindow — rebuild the entity combo + segment list each time the page
    // is shown (the Asset tab may have changed entities behind our back).
    //
    afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
    //
    // OnEntityChanged — commit the current segment, switch the active entity,
    // and reload its segment list.
    //
    afx_msg void OnEntityChanged();
    //
    // OnSegmentTypeChanged — commit, seed defaults for the newly chosen type,
    // and refresh the type-specific fields.
    //
    afx_msg void OnSegmentTypeChanged();
    //
    // OnSegmentCoordModeChanged — convert the segment's geometry (foci / start /
    // end) from the old frame to the newly selected coordinate mode via ECEF.
    //
    afx_msg void OnSegmentCoordModeChanged();
    //
    // OnSegmentTimingChanged — push the start/end time edits into the segment
    // and update just the timeline list cells (no full reload).
    //
    afx_msg void OnSegmentTimingChanged();
    //
    // OnEllipseSpeedKillFocus — normalize the Ellipse speed field to plain m/s
    // (parses Mach / mph / km/h).
    //
    afx_msg void OnEllipseSpeedKillFocus();
    //
    // OnLineSpeedKillFocus — normalize the Line speed field and, on a real user
    // edit, re-derive End Time so distance = speed x duration stays consistent.
    //
    afx_msg void OnLineSpeedKillFocus();
    //
    // OnAddSegment — append a new Stationary segment after the current one.
    //
    afx_msg void OnAddSegment();
    //
    // OnDeleteSegment — remove the selected segment.
    //
    afx_msg void OnDeleteSegment();
    //
    // OnDuplicateSegment — clone the selected segment, shifting its time window.
    //
    afx_msg void OnDuplicateSegment();
    //
    // OnMoveSegmentUp — swap the selected segment with the one before it.
    //
    afx_msg void OnMoveSegmentUp();
    //
    // OnMoveSegmentDown — swap the selected segment with the one after it.
    //
    afx_msg void OnMoveSegmentDown();
    //
    // OnValidateSegment — run the full validator and show only the issues that
    // belong to the selected entity/segment.
    //
    afx_msg void OnValidateSegment();
    //
    // OnSegmentListSelChanged — commit the outgoing segment and load the newly
    // selected timeline row.
    //
    afx_msg void OnSegmentListSelChanged(NMHDR* pNMHDR, LRESULT* pResult);

    DECLARE_MESSAGE_MAP()

private:
    //
    // RefreshEntityCombo — rebuild the entity selector from the scenario.
    //
    void RefreshEntityCombo();
    //
    // RefreshSegmentList — rebuild the timeline list for the active entity and
    // restore/auto-select a row.
    //
    void RefreshSegmentList();
    //
    // LoadActiveSegment — populate all controls from the active segment.
    //
    void LoadActiveSegment();
    //
    // CommitActiveSegmentFromUi — write all control values back into the active
    // segment (type, timing, coord mode, and type-specific geometry).
    //
    void CommitActiveSegmentFromUi();
    //
    // SetTypeSpecificVisibility — show/hide and relabel the Ellipse vs
    // Line/Stationary/StopHold field groups for the current segment type/frame.
    //
    void SetTypeSpecificVisibility();
    //
    // LoadEllipseFromSegment — fill the Ellipse fields (foci, length, bearing,
    // speed, direction) and the computed read-only start position.
    //
    void LoadEllipseFromSegment(const MotionSegment& s);
    //
    // CommitEllipseToSegment — read the Ellipse fields back into the segment.
    //
    void CommitEllipseToSegment(MotionSegment& s);
    //
    // SeedEllipseDefaultsIfBlank — supply sensible defaults (foci at origin,
    // 1 km circle, cruise speed) when the Ellipse params look unset.
    //
    void SeedEllipseDefaultsIfBlank(MotionSegment& s);
    //
    // LoadLineFromSegment — fill the Line/Stationary/StopHold start+end
    // position and attitude fields plus the Line speed.
    //
    void LoadLineFromSegment(const MotionSegment& s);
    //
    // CommitLineToSegment — read the Line fields back; only touches the End row
    // and speed when the segment is a Line.
    //
    void CommitLineToSegment(MotionSegment& s);
    //
    // SeedLineDefaultsIfBlank — anchor a blank start at the origin and give a
    // Line a visible end/speed, re-deriving End Time to match.
    //
    void SeedLineDefaultsIfBlank(MotionSegment& s);

    //
    // ActiveEntity — the currently selected entity, or nullptr.
    //
    Entity*       ActiveEntity();
    //
    // ActiveSegment — the currently selected motion segment, or nullptr.
    //
    MotionSegment* ActiveSegment();

    Scenario* m_scenario      = nullptr;
    int       m_entityIdx     = 0;
    int       m_segmentIdx    = -1;   // -1 = nothing selected
    bool      m_suppressEvents = false;
};
