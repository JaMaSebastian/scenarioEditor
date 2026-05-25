#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

struct Scenario;
struct Entity;
struct MotionSegment;

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
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;

    afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
    afx_msg void OnEntityChanged();
    afx_msg void OnSegmentTypeChanged();
    afx_msg void OnSegmentCoordModeChanged();
    afx_msg void OnSegmentTimingChanged();
    afx_msg void OnEllipseSpeedKillFocus();
    afx_msg void OnLineSpeedKillFocus();
    afx_msg void OnAddSegment();
    afx_msg void OnDeleteSegment();
    afx_msg void OnDuplicateSegment();
    afx_msg void OnMoveSegmentUp();
    afx_msg void OnMoveSegmentDown();
    afx_msg void OnValidateSegment();
    afx_msg void OnSegmentListSelChanged(NMHDR* pNMHDR, LRESULT* pResult);

    DECLARE_MESSAGE_MAP()

private:
    void RefreshEntityCombo();
    void RefreshSegmentList();
    void LoadActiveSegment();
    void CommitActiveSegmentFromUi();
    void SetTypeSpecificVisibility();
    void LoadEllipseFromSegment(const MotionSegment& s);
    void CommitEllipseToSegment(MotionSegment& s);
    void SeedEllipseDefaultsIfBlank(MotionSegment& s);
    void LoadLineFromSegment(const MotionSegment& s);
    void CommitLineToSegment(MotionSegment& s);
    void SeedLineDefaultsIfBlank(MotionSegment& s);

    Entity*       ActiveEntity();
    MotionSegment* ActiveSegment();

    Scenario* m_scenario      = nullptr;
    int       m_entityIdx     = 0;
    int       m_segmentIdx    = -1;   // -1 = nothing selected
    bool      m_suppressEvents = false;
};
