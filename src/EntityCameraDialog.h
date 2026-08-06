//=============================================================================
//  EntityCameraDialog.h
//-----------------------------------------------------------------------------
//  Declares CEntityCameraDialog, the modal launched from an entity dot's
//  right-click "New Camera..." item. The source entity is fixed (the clicked
//  dot); the dialog lets the operator choose which Cameras.ini preset to mount,
//  which entity to track (or focus on the source), the zoom applied while this
//  camera is live, and the transition used when cutting to it. On OK the caller
//  reads the selections back and builds the CameraFrame.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "Scenario.h"

#include <cstdint>

// Modal "New Entity Camera" dialog. Caller sets the context before DoModal();
// on IDOK the getters return the chosen preset / target / transition.
class CEntityCameraDialog : public CDialogEx
{
public:
    enum { IDD = IDD_ENTITY_CAMERA };
    CEntityCameraDialog(CWnd* pParent = nullptr) : CDialogEx(IDD, pParent) {}

    // Fixed source (the clicked entity), the scenario (for the target list),
    // and a human label shown for the source. Set before DoModal().
    void SetContext(const Scenario* scn, uint16_t sourceEntityId, const CString& sourceLabel)
    { m_scenario = scn; m_sourceId = sourceEntityId; m_sourceLabel = sourceLabel; }

    // Results — valid after IDOK.
    size_t           PresetIndex()    const { return m_presetIndex; }
    uint16_t         TargetEntityId() const { return m_targetId; }
    CameraTransition Transition()     const { return m_transition; }
    bool             ZoomEnabled()    const { return m_zoomEnabled; }
    double           ZoomFovDeg()     const { return m_zoomFovDeg; }
    bool             DynamicZoom()    const { return m_dynamicZoom; }
    double           ZoomFillPct()    const { return m_zoomFillPct; }
    bool             LimitsEnabled()  const { return m_limitsEnabled; }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnZoomToggle();     // either zoom checkbox -> re-gate the fields
    afx_msg void OnLimitsToggle();   // gimbal-limits checkbox
    afx_msg void OnPresetChange();   // re-seed the FOV edit from the new preset
    afx_msg void OnTargetChange();   // new target -> new (or no) catalogued size
    DECLARE_MESSAGE_MAP()

private:
    // Enable/disable the zoom edits to match the two checkboxes.
    void UpdateZoomFields();
    // Re-resolve whether the currently selected target has catalogued dimensions.
    // Dynamic zoom has nothing to fit to without them, so it stays grayed.
    void RefreshTargetSize();
    // Put the selected preset's own FOV in the FOV edit (used until the operator
    // turns zoom on and starts editing it themselves).
    void SeedFovFromPreset();
    // Re-resolve whether the selected preset defines and enables a gimbal envelope.
    // Without one there is nothing to enforce, so the toggle grays — and says why,
    // since a silently disabled control reads as a bug. Gates on the PRESET, not the
    // target, so it belongs on preset change rather than target change. Defaults the
    // toggle on when available, so new work picks limits up without extra clicks.
    void RefreshLimitsAvailability();

    const Scenario* m_scenario   = nullptr;
    uint16_t        m_sourceId   = 0;
    CString         m_sourceLabel;

    size_t           m_presetIndex = 0;
    uint16_t         m_targetId    = 0;
    CameraTransition m_transition  = CameraTransition::CrossfadeBlend;

    bool             m_zoomEnabled = false;
    double           m_zoomFovDeg  = 90.0;
    bool             m_dynamicZoom = false;
    double           m_zoomFillPct = 85.0;

    bool             m_limitsEnabled = false;

    bool             m_targetHasSize   = false;  // catalog has dimensions for the target
    bool             m_presetHasLimits = false;  // preset defines AND enables an envelope
};
