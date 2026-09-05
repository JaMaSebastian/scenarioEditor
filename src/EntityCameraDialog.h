//=============================================================================
//  EntityCameraDialog.h
//-----------------------------------------------------------------------------
//  Declares CEntityCameraDialog, the modal behind every camera-authoring path:
//  the Preview tab's "New Cam..." button, an entity dot's right-click
//  "New Camera...", and "Edit Camera..." on a camera box (timeline strip or
//  canvas). The creation paths differ only in which source the dropdown opens on
//  — (none) for the button, the clicked entity for the right-click — so the same
//  dialog builds both kinds of camera.
//
//  SetEditContext() switches it to Edit mode: every control is seeded from an
//  existing CameraFrame, the caption becomes "Edit Camera" and OK becomes "Save".
//  Seeding writes the SAME members OnOK commits and the getters return, so the
//  two modes cannot drift — a fresh camera is simply one seeded with defaults.
//
//  The right-hand "Gimbal envelope" group is a different SCOPE from everything
//  else here. Every other control edits this one shot; the envelope belongs to
//  the mounted PRESET and lives in Cameras.ini, so changing it affects every shot
//  mounting that preset, in this scenario and any other. The caller writes it via
//  CameraPresetIO; this dialog only collects it, and only reports it as changed
//  when the operator actually touched it.
//
//  Source (none) means the camera has no source entity: an entity-less
//  Stationary camera sitting at a fixed vantage. Such a camera has no camera
//  TYPE — there is only one camera, with no back/front/left/right — so the
//  Camera row and the preset-derived gimbal limits do not apply and are grayed.
//  It may still track a target entity, and zoom/transition still apply.
//
//  For an entity source the dialog picks which Cameras.ini preset to mount,
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
#include "CameraPresetCombo.h"   // shared preset-combo fill + kRowNotSelectable
#include "CameraPresetIO.h"      // GimbalEnvelope (the preset-level angles)

#include <cstdint>
#include <string>

// Modal "New Camera" dialog. Caller sets the context before DoModal(); on IDOK
// the getters return the chosen source / preset / target / transition.
class CEntityCameraDialog : public CDialogEx
{
public:
    enum { IDD = IDD_ENTITY_CAMERA };
    CEntityCameraDialog(CWnd* pParent = nullptr) : CDialogEx(IDD, pParent) {}

    // The scenario (for the source and target lists) and the source the Source
    // dropdown should open on — 0 selects "(none)", i.e. a Stationary camera.
    // Set before DoModal().
    void SetContext(const Scenario* scn, uint16_t preselectSourceId)
    { m_scenario = scn; m_sourceId = preselectSourceId; }

    // Edit mode: seed every control from an existing frame. Pass a COPY, not a
    // reference into Scenario::cameras — the caller's index can be invalidated
    // while the modal is up, so nothing here may alias the live vector.
    void SetEditContext(const Scenario* scn, const CameraFrame& f);

    // Results — valid after IDOK.
    // 0 means the operator chose "(none)": build a Stationary camera, and ignore
    // PresetIndex()/LimitsEnabled(), which do not apply to one.
    uint16_t         SourceEntityId() const { return m_sourceId; }
    size_t           PresetIndex()    const { return m_presetIndex; }
    uint16_t         TargetEntityId() const { return m_targetId; }
    CameraTransition Transition()     const { return m_transition; }
    bool             ZoomEnabled()    const { return m_zoomEnabled; }
    double           ZoomFovDeg()     const { return m_zoomFovDeg; }
    bool             DynamicZoom()    const { return m_dynamicZoom; }
    double           ZoomFillPct()    const { return m_zoomFillPct; }
    bool             LimitsEnabled()  const { return m_limitsEnabled; }

    // The chosen preset by NAME as well as by index. The caller needs this because
    // writing an envelope reloads the catalog, which invalidates PresetIndex();
    // reading the strings first means the update path never has to re-index.
    const std::string& PresetType()  const { return m_presetType; }
    const std::string& PresetAngle() const { return m_presetAngle; }

    // Shot fields the dialog gained when it became an editor. Vantage is ENU
    // metres from the scenario origin and applies only to a Stationary camera.
    double             TransitionSeconds() const { return m_transitionSeconds; }
    const std::string& Label()             const { return m_label; }
    double             VantEastM()         const { return m_vantEastM; }
    double             VantNorthM()        const { return m_vantNorthM; }
    double             VantUpM()           const { return m_vantUpM; }

    // ----- The mounted preset's gimbal envelope (Cameras.ini, shared) -----
    // EnvelopeChanged() is false unless the operator actually altered something,
    // so an edit that only renames a camera never rewrites Cameras.ini — and never
    // fires the "this affects N other shots" confirmation.
    bool   EnvelopeChanged() const { return m_envChanged; }
    bool   EnvelopeDefined() const { return m_env.defined; }
    bool   EnvelopeEnabled() const { return m_env.enabled; }
    double MinPitch() const { return m_env.minPitch; }
    double MaxPitch() const { return m_env.maxPitch; }
    double MinYaw()   const { return m_env.minYaw;   }
    double MaxYaw()   const { return m_env.maxYaw;   }
    double MinRoll()  const { return m_env.minRoll;  }
    double MaxRoll()  const { return m_env.maxRoll;  }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnSourceChange();   // new source -> new preset list / gating
    afx_msg void OnZoomToggle();     // either zoom checkbox -> re-gate the fields
    afx_msg void OnLimitsToggle();   // gimbal-limits checkbox
    afx_msg void OnPresetChange();   // re-seed the FOV edit from the new preset
    afx_msg void OnTargetChange();   // new target -> new (or no) catalogued size
    afx_msg void OnEnvelopeDefineToggle();  // define/clear the preset's envelope
    afx_msg void OnEnvelopeEdit();          // any angle edit -> recompute the spans
    DECLARE_MESSAGE_MAP()

private:
    // The source currently selected in the combo (0 = "(none)"). Read live rather
    // than cached, so the gating helpers agree with what is on screen mid-dialog.
    uint16_t SelectedSourceId() const;

    // Re-fill and re-gate everything that depends on the source: the preset combo,
    // the gimbal-limits toggle, and the wording of the target list's row 0.
    void RefreshForSource();

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
    // A source-less camera has no mount at all, so it never has limits.
    void RefreshLimitsAvailability();

    // Find the combo row carrying the catalog entry named by type/angle, or -1.
    // Matching by NAME rather than by index is what lets an edited frame keep a
    // deliberately borrowed preset: Populate lists the source's own type first and
    // everything else below a separator, so the "first match" rule would silently
    // re-point a camera mounting another airframe's calibration.
    int  FindPresetRow(const CComboBox& c,
                       const std::string& type, const std::string& angle) const;

    // Load the selected preset's envelope into m_env and the six edits, and gray
    // the group when there is no mount to hang it on. Called whenever the preset
    // changes — the envelope belongs to the preset, so uncommitted edits are
    // deliberately discarded rather than carried onto a different camera.
    void LoadEnvelopeFromPreset();
    // Push m_env into the controls, gate them on "defined", and recompute spans.
    void UpdateEnvelopeFields();
    // Recompute the three arc-span readouts from what is currently typed.
    void RefreshEnvelopeSpans();
    // Show which Cameras.ini copies a save will reach (and warn when DISBrowser's
    // copy is not among them) BEFORE the operator commits.
    void RefreshEnvelopePathNote();
    // Enable/disable + gray the vantage row: it applies only to a source-less
    // Stationary camera.
    void UpdateVantageFields();

    const Scenario* m_scenario   = nullptr;
    uint16_t        m_sourceId   = 0;      // 0 = "(none)" -> Stationary camera

    size_t           m_presetIndex = 0;
    std::string      m_presetType, m_presetAngle;
    uint16_t         m_targetId    = 0;
    CameraTransition m_transition  = CameraTransition::CrossfadeBlend;

    bool             m_zoomEnabled = false;
    double           m_zoomFovDeg  = 90.0;
    bool             m_dynamicZoom = false;
    double           m_zoomFillPct = 85.0;

    bool             m_limitsEnabled = false;

    // Shot fields. Defaults match CameraFrame's, so a New camera seeded from these
    // members lands exactly where it did before the dialog could edit them.
    double           m_transitionSeconds = 0.5;
    std::string      m_label;
    double           m_vantEastM = 0.0, m_vantNorthM = 0.0, m_vantUpM = 0.0;

    // The mounted preset's envelope, as loaded and as edited.
    CameraPresetIO::GimbalEnvelope m_env;
    CameraPresetIO::GimbalEnvelope m_envLoaded;   // to diff against on OK
    bool             m_envChanged = false;

    // ----- Edit-mode seeding -----
    // m_editing only drives the caption/button text and the two one-shot latches;
    // everything else seeds unconditionally from the members above, whose defaults
    // are already the New-camera values.
    bool             m_editing = false;
    uint16_t         m_seedSourceId = 0;              // source the preset seed belongs to
    std::string      m_seedPresetType, m_seedPresetAngle;
    bool             m_presetSeedPending = false;     // consume once, then act like New
    bool             m_limitsSeedPending = false;

    bool             m_targetHasSize   = false;  // catalog has dimensions for the target
    bool             m_presetHasLimits = false;  // an envelope is defined for this mount
};
