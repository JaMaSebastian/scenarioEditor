//=============================================================================
//  EntityCameraDialog.cpp
//-----------------------------------------------------------------------------
//  Implements CEntityCameraDialog: fills the Source/Camera/Target/Transition
//  dropdowns, re-derives the Camera row and the gimbal-limits toggle whenever
//  the source changes, gates the zoom row off its two checkboxes, and captures
//  the operator's selections on OK. Preset list comes from theApp.CameraPresets()
//  via CameraPresetCombo; the target list is the scenario's entities plus a
//  "(focus on source)" / "(no target)" entry. Source "(none)" means a
//  source-less Stationary camera: no camera type, no preset, no envelope.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#include "pch.h"
#include "EntityCameraDialog.h"
#include "ScenarioEditor.h"        // theApp + CameraPresets()
#include "CameraPresetCatalog.h"
#include "CameraTargetExtents.h"   // does the chosen target have catalogued dimensions?
#include "CameraPresetCombo.h"    // shared preset-combo fill + kRowNotSelectable

#include <string>
#include <algorithm>
#include <cwchar>

BEGIN_MESSAGE_MAP(CEntityCameraDialog, CDialogEx)
    ON_CBN_SELCHANGE(IDC_COMBO_ECAM_SOURCE, &CEntityCameraDialog::OnSourceChange)
    ON_BN_CLICKED(IDC_CHK_ECAM_ZOOM,       &CEntityCameraDialog::OnZoomToggle)
    ON_BN_CLICKED(IDC_CHK_ECAM_DYNZOOM,    &CEntityCameraDialog::OnZoomToggle)
    ON_BN_CLICKED(IDC_CHK_ECAM_GIMBAL, &CEntityCameraDialog::OnLimitsToggle)
    ON_CBN_SELCHANGE(IDC_COMBO_ECAM_PRESET, &CEntityCameraDialog::OnPresetChange)
    ON_CBN_SELCHANGE(IDC_COMBO_ECAM_TARGET, &CEntityCameraDialog::OnTargetChange)
    ON_BN_CLICKED(IDC_CHK_ECAM_ENV_DEF,     &CEntityCameraDialog::OnEnvelopeDefineToggle)
    // Every angle edit feeds the span readouts, which are the only thing that makes
    // a wrapping yaw/roll arc legible while it is being typed.
    ON_EN_CHANGE(IDC_EDIT_ECAM_MINPITCH,    &CEntityCameraDialog::OnEnvelopeEdit)
    ON_EN_CHANGE(IDC_EDIT_ECAM_MAXPITCH,    &CEntityCameraDialog::OnEnvelopeEdit)
    ON_EN_CHANGE(IDC_EDIT_ECAM_MINYAW,      &CEntityCameraDialog::OnEnvelopeEdit)
    ON_EN_CHANGE(IDC_EDIT_ECAM_MAXYAW,      &CEntityCameraDialog::OnEnvelopeEdit)
    ON_EN_CHANGE(IDC_EDIT_ECAM_MINROLL,     &CEntityCameraDialog::OnEnvelopeEdit)
    ON_EN_CHANGE(IDC_EDIT_ECAM_MAXROLL,     &CEntityCameraDialog::OnEnvelopeEdit)
END_MESSAGE_MAP()

namespace
{
    // Read a numeric edit STRICTLY: wcstod alone reports 0 for "abc", which would
    // silently accept garbage as a legal angle. Requires the whole (trimmed) text
    // to parse.
    bool ParseNum(const CString& text, double& out)
    {
        CString s = text; s.Trim();
        if (s.IsEmpty()) return false;
        wchar_t* end = nullptr;
        const double d = std::wcstod(s, &end);
        if (!end || *end != L'\0') return false;
        out = d;
        return true;
    }

    // Arc swept from lo to hi, increasing. Mirrors CameraViewStore::ArcSpanDegrees:
    // deliberately NOT normalised to <360, so a full circle reports 360 rather than 0.
    double ArcSpanDeg(double lo, double hi)
    {
        double s = hi - lo;
        if (s < 0.0) s += 360.0;
        return s;
    }
}

//
// SetEditContext — seed from an existing frame and switch to Edit mode.
//
//   Everything lands in the same members OnOK commits, so OnInitDialog needs no
//   parallel "edit" path: it always seeds from the members, whose defaults ARE the
//   New-camera values. Only the caption/button text and the two one-shot latches
//   below are conditional.
//
//   The preset is captured by NAME. CameraPresetCombo's item data is an index into
//   theApp.CameraPresets(), which is not stable across a Cameras.ini reload, and a
//   borrowed preset does not sit where the "first match for this entity" rule would
//   put it.
//
void CEntityCameraDialog::SetEditContext(const Scenario* scn, const CameraFrame& f)
{
    m_scenario = scn;
    m_editing  = true;

    m_sourceId          = (f.kind == CameraKind::Entity) ? f.sourceEntityId : 0;
    m_targetId          = f.targetEntityId;
    m_transition        = f.transition;
    m_transitionSeconds = f.transitionSeconds;
    m_label             = f.label;

    m_vantEastM  = f.vantEastM;
    m_vantNorthM = f.vantNorthM;
    m_vantUpM    = f.vantUpM;

    m_zoomEnabled = f.zoomEnabled;
    m_zoomFovDeg  = f.zoomFovDeg;
    m_dynamicZoom = f.dynamicZoom;
    m_zoomFillPct = f.zoomFillPct;

    m_limitsEnabled = f.limitsEnabled;

    m_seedSourceId      = m_sourceId;
    m_seedPresetType    = f.presetType;
    m_seedPresetAngle   = f.presetAngle;
    m_presetSeedPending = true;
    m_limitsSeedPending = true;
}

//
// OnInitDialog — fill the Source/Target/Transition dropdowns, then let
//   RefreshForSource() derive everything that depends on the chosen source.
//
BOOL CEntityCameraDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // Source: "(none)" (id 0) followed by every entity. (none) is a real choice,
    // not a placeholder — it builds the entity-less Stationary camera the Preview
    // tab's "New Cam..." button used to create directly.
    if (CComboBox* src = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_SOURCE))
    {
        src->ResetContent();
        const int z = src->AddString(_T("(none — stationary camera)"));
        src->SetItemData(z, 0);
        int sel = 0;
        if (m_scenario)
        {
            for (const Entity& e : m_scenario->entities)
            {
                const std::string& nm = e.marking.empty() ? e.name : e.marking;
                CString label;
                label.Format(_T("%s [%u]"),
                             (LPCTSTR)CString(CA2W(nm.c_str())),
                             static_cast<unsigned>(e.entityId));
                const int ix = src->AddString(label);
                src->SetItemData(ix, e.entityId);
                if (e.entityId == m_sourceId) sel = ix;
            }
        }
        src->SetCurSel(sel);
    }

    // Target: row 0 (id 0) followed by every entity. Row 0's wording depends on the
    // source — "(focus on source)" vs "(no target)" — so RefreshForSource sets it.
    if (CComboBox* tgt = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_TARGET))
    {
        tgt->ResetContent();
        const int z = tgt->AddString(_T("(focus on source)"));
        tgt->SetItemData(z, 0);
        if (m_scenario)
        {
            for (const Entity& e : m_scenario->entities)
            {
                const std::string& nm = e.marking.empty() ? e.name : e.marking;
                CString label;
                label.Format(_T("%s [%u]"),
                             (LPCTSTR)CString(CA2W(nm.c_str())),
                             static_cast<unsigned>(e.entityId));
                const int ix = tgt->AddString(label);
                tgt->SetItemData(ix, e.entityId);
            }
        }
        // Re-select the seeded target by id. m_targetId is 0 for a new camera, which
        // lands on row 0 — identical to the old hardcoded SetCurSel(0).
        int sel = 0;
        for (int i = 0; i < tgt->GetCount(); ++i)
        {
            if (static_cast<uint16_t>(tgt->GetItemData(i)) == m_targetId) { sel = i; break; }
        }
        tgt->SetCurSel(sel);
    }

    // Transition: Crossfade/Blend (index 0) or Hard cut (index 1). Filled BEFORE the
    // Refresh cascade below, so a helper that ever needs to read this row finds it
    // populated rather than empty.
    if (CComboBox* trn = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_TRANS))
    {
        trn->ResetContent();
        trn->AddString(_T("Crossfade / Blend"));
        trn->AddString(_T("Hard cut"));
        trn->SetCurSel(m_transition == CameraTransition::HardCut ? 1 : 0);
    }

    // Zoom: seeded from the members, whose defaults are off/off — so a new camera
    // still behaves exactly as cameras did before the feature existed. The FOV edit
    // starts at the chosen preset's own FOV rather than a magic number, so turning
    // zoom on and pressing OK is a no-op until the operator actually changes it.
    CheckDlgButton(IDC_CHK_ECAM_ZOOM,    m_zoomEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_ECAM_DYNZOOM, m_dynamicZoom ? BST_CHECKED : BST_UNCHECKED);
    {
        CString s;
        s.Format(_T("%g"), m_zoomFillPct);
        SetDlgItemText(IDC_EDIT_ECAM_FILL, s);
        s.Format(_T("%g"), m_zoomFovDeg);
        SetDlgItemText(IDC_EDIT_ECAM_FOV, s);
    }

    // The shot fields the dialog gained as an editor.
    SetDlgItemText(IDC_EDIT_ECAM_LABEL, CString(CA2W(m_label.c_str())));
    {
        CString s;
        s.Format(_T("%g"), m_transitionSeconds);
        SetDlgItemText(IDC_EDIT_ECAM_TRANSSEC, s);
        s.Format(_T("%g"), m_vantEastM);  SetDlgItemText(IDC_EDIT_ECAM_VANT_E, s);
        s.Format(_T("%g"), m_vantNorthM); SetDlgItemText(IDC_EDIT_ECAM_VANT_N, s);
        s.Format(_T("%g"), m_vantUpM);    SetDlgItemText(IDC_EDIT_ECAM_VANT_U, s);
    }

    // Which Cameras.ini copies a save will actually reach. Shown up front rather
    // than in a modal afterwards, so an unset DISBrowser folder is known BEFORE the
    // operator spends effort on an envelope.
    RefreshEnvelopePathNote();

    // Fills the preset combo, seeds the FOV from it, loads the envelope, and gates
    // the limits toggle — all of it depends on the source, so it lives in one place.
    RefreshForSource();
    RefreshTargetSize();
    UpdateZoomFields();

    if (m_editing)
    {
        SetWindowText(_T("Edit Camera"));
        SetDlgItemText(IDOK, _T("Save"));
    }

    return TRUE;
}

//
// SelectedSourceId — the source currently on screen (0 = "(none)"). Read from the
//   combo rather than m_sourceId so the gating helpers stay correct while the
//   operator is still changing things; m_sourceId is only committed in OnOK.
//
uint16_t CEntityCameraDialog::SelectedSourceId() const
{
    const CComboBox* src = (const CComboBox*)GetDlgItem(IDC_COMBO_ECAM_SOURCE);
    if (!src) return 0;
    const int sel = src->GetCurSel();
    if (sel < 0) return 0;
    return static_cast<uint16_t>(src->GetItemData(sel));
}

//
// RefreshForSource — re-derive everything the source decides.
//
//   With no source the camera is a fixed point in space, not a mount: it has no
//   camera TYPE (one camera, no back/front/left/right) and therefore no preset and
//   no preset-derived gimbal envelope. Both rows gray, and both say why — the same
//   convention CameraZoomMenu::Build uses, because a silently disabled control
//   reads as a bug.
//
//   Target row 0 is id 0 either way, but it means different things: "focus on the
//   source" for an entity camera, plain "no target" for a stationary one.
//
void CEntityCameraDialog::RefreshForSource()
{
    const uint16_t srcId  = SelectedSourceId();
    const bool     mounted = (srcId != 0);

    if (CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_PRESET))
    {
        if (mounted)
        {
            const std::string typeKey = m_scenario
                ? CameraPresetCombo::TypeKeyForEntityId(*m_scenario, srcId)
                : std::string();
            const int firstMatch = CameraPresetCombo::Populate(*pre, typeKey);

            // An edited frame re-selects the preset it actually mounts — including
            // one deliberately borrowed from another airframe, which lives below the
            // separator and would otherwise be silently re-pointed at firstMatch.
            int want = -1;
            if (m_presetSeedPending && srcId == m_seedSourceId)
                want = FindPresetRow(*pre, m_seedPresetType, m_seedPresetAngle);

            pre->SetCurSel(want >= 0 ? want : (firstMatch >= 0 ? firstMatch : 0));

            // The seed belongs to the source it came from. Once the operator moves to
            // a different entity, the New-camera "that entity's own first view" rule
            // takes over.
            if (srcId != m_seedSourceId) m_presetSeedPending = false;
        }
        else
        {
            CameraPresetCombo::PopulateNoSource(*pre);
        }
        pre->EnableWindow(mounted);
    }

    // Vantage is a Stationary camera's whole position; a mounted camera rides its
    // entity, so the row grays.
    UpdateVantageFields();

    // Relabel target row 0 in place, keeping its id-0 item data and the current
    // selection. Deleting/reinserting would silently drop a chosen target.
    if (CComboBox* tgt = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_TARGET))
    {
        const int sel = tgt->GetCurSel();
        if (tgt->GetCount() > 0)
        {
            tgt->DeleteString(0);
            tgt->InsertString(0, mounted ? _T("(focus on source)") : _T("(no target)"));
            tgt->SetItemData(0, 0);
            tgt->SetCurSel(sel >= 0 ? sel : 0);
        }
    }

    // FOV follows the preset while zoom is off; with no preset it keeps whatever
    // it had, which is the 90-degree default. An edited frame that already has zoom
    // on carries an operator-chosen FOV, and that must survive init.
    if (mounted && !(m_presetSeedPending && m_zoomEnabled)) SeedFovFromPreset();

    // The mount changed, so the envelope on offer did too.
    LoadEnvelopeFromPreset();
    RefreshLimitsAvailability();
}

//
// FindPresetRow — see header.
//
int CEntityCameraDialog::FindPresetRow(const CComboBox& c,
                                       const std::string& type,
                                       const std::string& angle) const
{
    if (type.empty()) return -1;

    const auto& presets = theApp.CameraPresets().Presets();
    for (int i = 0; i < c.GetCount(); ++i)
    {
        const DWORD_PTR d = c.GetItemData(i);
        if (d == CameraPresetCombo::kRowNotSelectable) continue;   // separator / notice
        const size_t ix = static_cast<size_t>(d);
        if (ix < presets.size() && presets[ix].type == type && presets[ix].angle == angle)
            return i;
    }
    return -1;
}

//
// UpdateVantageFields — the vantage row applies only to a source-less Stationary
//   camera. Naming the reason on the label beats three silently grayed edits.
//
void CEntityCameraDialog::UpdateVantageFields()
{
    const bool stationary = (SelectedSourceId() == 0);

    SetDlgItemText(IDC_LBL_ECAM_VANT,
                   stationary ? _T("Vantage:") : _T("Vantage: (rides source)"));

    const int ids[] = { IDC_EDIT_ECAM_VANT_E, IDC_EDIT_ECAM_VANT_N, IDC_EDIT_ECAM_VANT_U };
    for (int id : ids)
        if (CWnd* w = GetDlgItem(id)) w->EnableWindow(stationary);
    if (CWnd* w = GetDlgItem(IDC_LBL_ECAM_VANT)) w->EnableWindow(stationary);
}

//
// OnSourceChange — a different source means a different camera list, a different
//   envelope, and different wording on the target row.
//
void CEntityCameraDialog::OnSourceChange()
{
    // Clear the per-shot seed BEFORE the refresh cascade reads it. (The preset seed
    // is left alone: RefreshForSource itself decides whether the new source is still
    // the one the seed belongs to.)
    m_limitsSeedPending = false;

    RefreshForSource();
    // The probe frame's kind changes with the source, so the catalogued-size answer
    // (and the Dynamic toggle that depends on it) can change too.
    RefreshTargetSize();
    UpdateZoomFields();
}

//
// OnZoomToggle — either checkbox changed; re-gate the two edits.
//
void CEntityCameraDialog::OnZoomToggle()
{
    UpdateZoomFields();
}

//
// OnPresetChange — track the preset's FOV while zoom is still off. Once zoom is
//   enabled the value is the operator's, so leave it alone.
//
void CEntityCameraDialog::OnPresetChange()
{
    if (IsDlgButtonChecked(IDC_CHK_ECAM_ZOOM) != BST_CHECKED)
        SeedFovFromPreset();

    // A deliberate preset switch ends the edit-mode seeding: from here the
    // default-on rule applies, as it would for a brand-new camera.
    m_limitsSeedPending = false;
    m_presetSeedPending = false;

    // A different camera means a different (or absent) envelope.
    LoadEnvelopeFromPreset();
    RefreshLimitsAvailability();
}

//
// OnLimitsToggle — nothing to re-gate; the checkbox is the whole control. Present so
//   the click is handled rather than falling through to the dialog default.
//
void CEntityCameraDialog::OnLimitsToggle()
{
}

//
// OnTargetChange — the tracked entity changed, so the catalogued size did too.
//
void CEntityCameraDialog::OnTargetChange()
{
    RefreshTargetSize();
    UpdateZoomFields();
}

//
// RefreshTargetSize — ask the catalog whether the selected target has dimensions,
//   and say so on the Dynamic checkbox. Building a throwaway CameraFrame keeps the
//   "which entity does this frame actually look at" rule in one place
//   (CameraTargetExtents) instead of duplicating it here.
//
void CEntityCameraDialog::RefreshTargetSize()
{
    m_targetHasSize = false;

    if (m_scenario)
    {
        CameraFrame probe;
        // A source-less camera is Stationary, and CameraTargetExtents keys off the
        // kind: an Entity frame with no explicit target falls back to its source,
        // a Stationary one has nothing to fall back to.
        const uint16_t srcId = SelectedSourceId();
        probe.kind           = srcId ? CameraKind::Entity : CameraKind::Stationary;
        probe.sourceEntityId = srcId;
        probe.targetEntityId = 0;
        if (CComboBox* tgt = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_TARGET))
        {
            if (tgt->GetCurSel() >= 0)
                probe.targetEntityId = static_cast<uint16_t>(tgt->GetItemData(tgt->GetCurSel()));
        }

        double l = 0.0, w = 0.0, h = 0.0;
        m_targetHasSize = CameraTargetExtents::Resolve(*m_scenario, probe, l, w, h);
    }

    // Name the reason on the control itself — a checkbox that is simply disabled
    // reads as broken, and "add dimensions to EntityTypeCatalog.ini" isn't a
    // guessable fix.
    SetDlgItemText(IDC_CHK_ECAM_DYNZOOM,
                   m_targetHasSize ? _T("Dynamic (fit target)")
                                   : _T("Dynamic (no catalog size)"));

    // A target swap can invalidate an already-ticked Dynamic box.
    if (!m_targetHasSize) CheckDlgButton(IDC_CHK_ECAM_DYNZOOM, BST_UNCHECKED);
}

//
// UpdateZoomFields — Dynamic needs zoom on AND a catalogued target size to fit to;
//   the FOV edit only applies to a static override, and Fill % only to a dynamic
//   fit. Gating all three keeps it obvious which number is actually in play.
//
void CEntityCameraDialog::UpdateZoomFields()
{
    const BOOL zoom = (IsDlgButtonChecked(IDC_CHK_ECAM_ZOOM)    == BST_CHECKED);
    const BOOL dyn  = (IsDlgButtonChecked(IDC_CHK_ECAM_DYNZOOM) == BST_CHECKED);

    if (CWnd* w = GetDlgItem(IDC_CHK_ECAM_DYNZOOM)) w->EnableWindow(zoom && m_targetHasSize);
    if (CWnd* w = GetDlgItem(IDC_EDIT_ECAM_FOV))    w->EnableWindow(zoom && !dyn);
    if (CWnd* w = GetDlgItem(IDC_EDIT_ECAM_FILL))   w->EnableWindow(zoom &&  dyn);
}

//
// SeedFovFromPreset — copy the selected Cameras.ini preset's FOV into the edit.
//
void CEntityCameraDialog::SeedFovFromPreset()
{
    double fov = m_zoomFovDeg;

    // Only ever called for a mounted source; a source-less camera has no preset to
    // read a FOV from and keeps the 90-degree default.
    CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_PRESET);
    if (pre && pre->GetCurSel() >= 0)
    {
        const auto& presets = theApp.CameraPresets().Presets();
        const size_t ix = static_cast<size_t>(pre->GetItemData(pre->GetCurSel()));
        if (ix < presets.size() && presets[ix].fov > 0.0) fov = presets[ix].fov;
    }

    CString s;
    s.Format(_T("%g"), fov);
    SetDlgItemText(IDC_EDIT_ECAM_FOV, s);
}

//
// RefreshLimitsAvailability — see header.
//
void CEntityCameraDialog::RefreshLimitsAvailability()
{
    // A source-less camera has no mount, so there is no envelope to enforce no
    // matter what the (disabled) preset row happens to show.
    const bool mounted = (SelectedSourceId() != 0);

    // NOTE: this deliberately does NOT reload the envelope from the preset. It is
    // called after a Define-envelope click as well as after a preset change, and a
    // reload there would immediately overwrite the click it is reacting to.
    // LoadEnvelopeFromPreset() is called from the two places the PRESET changes.

    // The per-shot toggle gates on whether an envelope EXISTS FOR THIS MOUNT right
    // now — read from the live "Define envelope" box, not from the catalog. Deriving
    // it from the catalog would leave this checkbox grayed for the rest of the
    // session in exactly the case that matters most: the operator defining an
    // envelope here for the first time, before anything has been written to disk.
    m_presetHasLimits = mounted && m_env.defined;

    // Name the reason on the control itself. "Define one in the group at right" is
    // not guessable from a silently grayed checkbox.
    SetDlgItemText(IDC_CHK_ECAM_GIMBAL,
                   m_presetHasLimits
                       ? _T("Gimbal limits (this shot)")
                       : (mounted ? _T("Gimbal limits (no envelope defined yet)")
                                  : _T("Gimbal limits (stationary camera has no mount)")));
    if (CWnd* w = GetDlgItem(IDC_CHK_ECAM_GIMBAL)) w->EnableWindow(m_presetHasLimits);

    if (m_limitsSeedPending)
    {
        // An edited frame carries its own saved opt-in; honour it once, then behave
        // like a new camera for any later preset change.
        CheckDlgButton(IDC_CHK_ECAM_GIMBAL,
                       (m_presetHasLimits && m_limitsEnabled) ? BST_CHECKED : BST_UNCHECKED);
        m_limitsSeedPending = false;
    }
    else
    {
        // Default on when the camera has an envelope: an author who took the trouble
        // to define one almost always wants it enforced.
        CheckDlgButton(IDC_CHK_ECAM_GIMBAL, m_presetHasLimits ? BST_CHECKED : BST_UNCHECKED);
    }
}

//
// LoadEnvelopeFromPreset — see header.
//
//   Switching preset DISCARDS uncommitted envelope edits, deliberately: the
//   envelope belongs to the preset, so carrying half-typed angles onto a different
//   camera would attribute them to the wrong mount. The group caption names its
//   owner so this is visible rather than mysterious.
//
void CEntityCameraDialog::LoadEnvelopeFromPreset()
{
    const bool mounted = (SelectedSourceId() != 0);
    const CameraPreset* p = nullptr;

    CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_PRESET);
    if (mounted && pre && pre->GetCurSel() >= 0)
    {
        const DWORD_PTR d = pre->GetItemData(pre->GetCurSel());
        if (d != CameraPresetCombo::kRowNotSelectable)
        {
            const auto& presets = theApp.CameraPresets().Presets();
            const size_t ix = static_cast<size_t>(d);
            if (ix < presets.size()) p = &presets[ix];
        }
    }

    CameraPresetIO::GimbalEnvelope env;   // permissive defaults
    if (p && p->hasLimits)
    {
        env.defined  = true;
        env.enabled  = p->limitsEnabled;
        env.minPitch = p->minPitch; env.maxPitch = p->maxPitch;
        env.minYaw   = p->minYaw;   env.maxYaw   = p->maxYaw;
        env.minRoll  = p->minRoll;  env.maxRoll  = p->maxRoll;
    }

    m_env       = env;
    m_envLoaded = env;   // the baseline EnvelopeChanged() diffs against

    // Name the owning preset on the group box, so "shared by every shot using this
    // preset" has something concrete attached to it.
    CString caption(_T("Gimbal envelope"));
    if (p) caption.Format(_T("Gimbal envelope — %s"),
                          (LPCTSTR)CString(CA2W(p->DisplayLabel().c_str())));
    SetDlgItemText(IDC_GRP_ECAM_ENV, caption);

    UpdateEnvelopeFields();
}

//
// UpdateEnvelopeFields — push m_env into the controls and gate them.
//
void CEntityCameraDialog::UpdateEnvelopeFields()
{
    const bool mounted = (SelectedSourceId() != 0);

    CheckDlgButton(IDC_CHK_ECAM_ENV_DEF, m_env.defined ? BST_CHECKED : BST_UNCHECKED);
    if (CWnd* w = GetDlgItem(IDC_CHK_ECAM_ENV_DEF)) w->EnableWindow(mounted);

    const struct { int id; double v; } rows[] = {
        { IDC_EDIT_ECAM_MINPITCH, m_env.minPitch }, { IDC_EDIT_ECAM_MAXPITCH, m_env.maxPitch },
        { IDC_EDIT_ECAM_MINYAW,   m_env.minYaw   }, { IDC_EDIT_ECAM_MAXYAW,   m_env.maxYaw   },
        { IDC_EDIT_ECAM_MINROLL,  m_env.minRoll  }, { IDC_EDIT_ECAM_MAXROLL,  m_env.maxRoll  },
    };
    for (const auto& r : rows)
    {
        CString s; s.Format(_T("%g"), r.v);
        SetDlgItemText(r.id, s);
        if (CWnd* w = GetDlgItem(r.id)) w->EnableWindow(mounted && m_env.defined);
    }

    RefreshEnvelopeSpans();
}

//
// RefreshEnvelopeSpans — the readouts that make a wrapping arc legible.
//
//   Pitch is an interval, so it reports a plain range. Yaw and roll are arcs swept
//   Min->Max increasing: a reversed pair is LEGAL and describes the complement cone
//   wrapping through +/-180, so it reports the real swept angle plus "(wraps)".
//   Without this the only feedback for a reversed pair would be its absence, and a
//   deliberate wrap would look indistinguishable from a typo.
//
void CEntityCameraDialog::RefreshEnvelopeSpans()
{
    const struct { int loId, hiId, spanId; bool arc; } rows[] = {
        { IDC_EDIT_ECAM_MINPITCH, IDC_EDIT_ECAM_MAXPITCH, IDC_LBL_ECAM_PITCH_SPAN, false },
        { IDC_EDIT_ECAM_MINYAW,   IDC_EDIT_ECAM_MAXYAW,   IDC_LBL_ECAM_YAW_SPAN,   true  },
        { IDC_EDIT_ECAM_MINROLL,  IDC_EDIT_ECAM_MAXROLL,  IDC_LBL_ECAM_ROLL_SPAN,  true  },
    };

    for (const auto& r : rows)
    {
        CString lo, hi, out;
        GetDlgItemText(r.loId, lo);
        GetDlgItemText(r.hiId, hi);

        double a = 0.0, b = 0.0;
        if (!m_env.defined)                      out = _T("");
        else if (!ParseNum(lo, a) || !ParseNum(hi, b)) out = _T("—");
        else if (!r.arc)
        {
            // Interval: show it the way round it will actually be stored.
            out.Format(_T("range %g\xB0"), (b >= a) ? (b - a) : (a - b));
        }
        else if (a == b)                         out = _T("axis locked");
        else
        {
            const double span = ArcSpanDeg(a, b);
            if (span >= 359.999)                 out = _T("full circle");
            else if (b < a)                      out.Format(_T("arc %g\xB0 (wraps)"), span);
            else                                 out.Format(_T("arc %g\xB0"), span);
        }
        SetDlgItemText(r.spanId, out);
    }
}

//
// RefreshEnvelopePathNote — say where a save will land before it happens.
//
//   The envelope only reaches playback through DISBrowser's own copy of
//   Cameras.ini. When that copy can't be found, the operator needs to know BEFORE
//   authoring six angles, not in a modal afterwards.
//
void CEntityCameraDialog::RefreshEnvelopePathNote()
{
    const std::wstring dbPath =
        CameraPresetIO::DisBrowserCamerasIniPath(theApp.DisBrowserProjectDir());

    CString note;
    if (dbPath.empty())
    {
        note = _T("Saving writes the editor's config\\Cameras.ini only — ")
               _T("DISBrowser's copy was not found, so playback will not see it. ")
               _T("Set the DISBrowser project folder on the Run tab.");
    }
    else
    {
        // DISBrowser rewrites this file wholesale from a map it loads at HUD start,
        // so a save made while it is running is silently lost on its next save.
        note = _T("Saving writes both the editor's and DISBrowser's Cameras.ini. ")
               _T("Close DISBrowser (or reload its hanger) first, or it will ")
               _T("overwrite this on its next save.");
    }
    SetDlgItemText(IDC_LBL_ECAM_ENV_PATH, note);
}

//
// OnEnvelopeDefineToggle — defining or clearing the envelope re-gates the six
//   edits AND the per-shot toggle, which cannot be offered without an envelope.
//
void CEntityCameraDialog::OnEnvelopeDefineToggle()
{
    m_env.defined = (IsDlgButtonChecked(IDC_CHK_ECAM_ENV_DEF) == BST_CHECKED);
    UpdateEnvelopeFields();
    RefreshLimitsAvailability();
}

//
// OnEnvelopeEdit — an angle changed; only the readouts need to keep up. The values
//   themselves are parsed and validated in OnOK, so a half-typed "-" never trips an
//   error mid-keystroke.
//
void CEntityCameraDialog::OnEnvelopeEdit()
{
    RefreshEnvelopeSpans();
}

//
// OnOK — capture the selections. Refuse to close if there are no presets to pick
//   or if an enabled zoom field is out of range.
//
void CEntityCameraDialog::OnOK()
{
    CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_PRESET);
    CComboBox* tgt = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_TARGET);
    CComboBox* trn = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_TRANS);

    // Commit the source first: everything below branches on whether this camera
    // is mounted on an entity at all.
    m_sourceId = SelectedSourceId();
    const bool mounted = (m_sourceId != 0);

    if (mounted && (!pre || pre->GetCurSel() < 0))
    {
        AfxMessageBox(_T("No camera presets are available (config\\Cameras.ini)."));
        return;   // keep the dialog open
    }

    m_zoomEnabled = (IsDlgButtonChecked(IDC_CHK_ECAM_ZOOM)    == BST_CHECKED);
    m_dynamicZoom = (IsDlgButtonChecked(IDC_CHK_ECAM_DYNZOOM) == BST_CHECKED);

    // Only validate the field that is actually in play — a stale value behind a
    // disabled edit shouldn't be able to block OK.
    CString txt;
    if (m_zoomEnabled && !m_dynamicZoom)
    {
        GetDlgItemText(IDC_EDIT_ECAM_FOV, txt);
        const double fov = _tstof(txt);
        if (fov < kZoomFovMinDeg || fov > kZoomFovMaxDeg)
        {
            CString msg;
            msg.Format(_T("Field of view must be between %g and %g degrees."),
                       kZoomFovMinDeg, kZoomFovMaxDeg);
            AfxMessageBox(msg);
            return;
        }
        m_zoomFovDeg = fov;
    }
    if (m_zoomEnabled && m_dynamicZoom)
    {
        GetDlgItemText(IDC_EDIT_ECAM_FILL, txt);
        const double fill = _tstof(txt);
        if (fill < kZoomFillMinPct || fill > kZoomFillMaxPct)
        {
            CString msg;
            msg.Format(_T("Fill %% must be between %g and %g."),
                       kZoomFillMinPct, kZoomFillMaxPct);
            AfxMessageBox(msg);
            return;
        }
        m_zoomFillPct = fill;
    }

    if (mounted)
    {
        // The m_presetHasLimits guard stops a check made against one preset from surviving
        // a switch to another that has no envelope at all.
        m_limitsEnabled = m_presetHasLimits
                       && (IsDlgButtonChecked(IDC_CHK_ECAM_GIMBAL) == BST_CHECKED);

        // The separator / "no views" rows are not presets. Refuse rather than index the
        // catalog with the sentinel, which would run off the end of the vector.
        const DWORD_PTR pdata = pre->GetItemData(pre->GetCurSel());
        if (pdata == CameraPresetCombo::kRowNotSelectable)
        {
            AfxMessageBox(_T("Pick a camera view for this entity.\n\n")
                          _T("If it has none yet, author them in DISBrowser's hanger, or ")
                          _T("choose one under \"other types\" to deliberately borrow ")
                          _T("another airframe's calibration."));
            return;   // keep the dialog open
        }
        m_presetIndex = static_cast<size_t>(pdata);

        // Capture the preset by NAME too — the caller reads these instead of the
        // index, which a Cameras.ini reload would invalidate.
        const auto& presets = theApp.CameraPresets().Presets();
        if (m_presetIndex < presets.size())
        {
            m_presetType  = presets[m_presetIndex].type;
            m_presetAngle = presets[m_presetIndex].angle;
        }
    }
    else
    {
        // No source: a Stationary camera is a point in space, not a mount, so it
        // carries neither a Cameras.ini preset nor a gimbal envelope.
        m_presetIndex   = 0;
        m_limitsEnabled = false;
        m_presetType.clear();
        m_presetAngle.clear();
    }
    m_targetId    = (tgt && tgt->GetCurSel() >= 0)
                    ? static_cast<uint16_t>(tgt->GetItemData(tgt->GetCurSel())) : 0;
    m_transition  = (trn && trn->GetCurSel() == 1)
                    ? CameraTransition::HardCut : CameraTransition::CrossfadeBlend;

    // ---- Shot fields the dialog gained as an editor ----
    {
        CString s;
        GetDlgItemText(IDC_EDIT_ECAM_TRANSSEC, s);
        double secs = m_transitionSeconds;
        if (!ParseNum(s, secs) || secs < kTransitionMinSec || secs > kTransitionMaxSec)
        {
            CString msg;
            msg.Format(_T("Transition seconds must be between %g and %g."),
                       kTransitionMinSec, kTransitionMaxSec);
            AfxMessageBox(msg);
            return;   // keep the dialog open
        }
        m_transitionSeconds = secs;

        GetDlgItemText(IDC_EDIT_ECAM_LABEL, s);
        s.Trim();
        m_label = (LPCSTR)CW2A(s, CP_UTF8);

        // Vantage only applies to a Stationary camera; behind a grayed row the text
        // is stale, so a mounted camera must not be able to fail validation on it.
        if (!mounted)
        {
            const struct { int id; double* dst; const wchar_t* name; } rows[] = {
                { IDC_EDIT_ECAM_VANT_E, &m_vantEastM,  L"East"  },
                { IDC_EDIT_ECAM_VANT_N, &m_vantNorthM, L"North" },
                { IDC_EDIT_ECAM_VANT_U, &m_vantUpM,    L"Up"    },
            };
            for (const auto& r : rows)
            {
                GetDlgItemText(r.id, s);
                double v = 0.0;
                if (!ParseNum(s, v))
                {
                    CString msg;
                    msg.Format(_T("Vantage %s must be a number (metres from the ")
                               _T("scenario origin)."), r.name);
                    AfxMessageBox(msg);
                    return;
                }
                *r.dst = v;
            }
        }
    }

    // ---- The mounted preset's gimbal envelope ----
    // Only validated when it is actually defined: stale text behind an unchecked
    // envelope must never be able to block Save, the same rule the zoom edits follow.
    m_env.defined = mounted && (IsDlgButtonChecked(IDC_CHK_ECAM_ENV_DEF) == BST_CHECKED);

    if (m_env.defined)
    {
        const struct { int loId, hiId; double* lo; double* hi;
                       const wchar_t* name; bool arc; } axes[] = {
            { IDC_EDIT_ECAM_MINPITCH, IDC_EDIT_ECAM_MAXPITCH,
              &m_env.minPitch, &m_env.maxPitch, L"Pitch", false },
            { IDC_EDIT_ECAM_MINYAW,   IDC_EDIT_ECAM_MAXYAW,
              &m_env.minYaw,   &m_env.maxYaw,   L"Yaw",   true  },
            { IDC_EDIT_ECAM_MINROLL,  IDC_EDIT_ECAM_MAXROLL,
              &m_env.minRoll,  &m_env.maxRoll,  L"Roll",  true  },
        };

        for (const auto& ax : axes)
        {
            CString loT, hiT;
            GetDlgItemText(ax.loId, loT);
            GetDlgItemText(ax.hiId, hiT);

            double lo = 0.0, hi = 0.0;
            if (!ParseNum(loT, lo) || !ParseNum(hiT, hi))
            {
                CString msg;
                msg.Format(_T("%s Min and Max must both be numbers."), ax.name);
                AfxMessageBox(msg);
                return;
            }

            const double bound = ax.arc ? kGimbalYawRollMaxDeg : kGimbalPitchMaxDeg;
            if (lo < -bound || lo > bound || hi < -bound || hi > bound)
            {
                CString msg;
                msg.Format(_T("%s must be between %g and %g degrees."),
                           ax.name, -bound, bound);
                AfxMessageBox(msg);
                return;
            }

            if (!ax.arc && lo > hi)
            {
                // Pitch is a plain interval, so a reversed pair is a typo. Swap it
                // silently — exactly what DISBrowser does when it loads the file.
                std::swap(lo, hi);
            }
            else if (ax.arc && lo == hi)
            {
                // Legal, but drastic enough to be worth confirming rather than
                // refusing: it pins the axis solid.
                CString msg;
                msg.Format(_T("%s Min and Max are equal, which locks that axis ")
                           _T("solid.\n\nContinue?"), ax.name);
                if (AfxMessageBox(msg, MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return;
            }
            // An arc with lo > hi is NOT swapped: it is the legal envelope that
            // wraps through +/-180, and reordering it would silently invert the
            // camera's travel range.

            *ax.lo = lo;
            *ax.hi = hi;
        }
    }

    // Did the operator actually touch the envelope? Only then does the caller
    // rewrite Cameras.ini — so renaming a camera never fires the shared-scope
    // confirmation, and never risks the write at all.
    m_envChanged = (m_env.defined  != m_envLoaded.defined)
                || (m_env.defined && (m_env.enabled  != m_envLoaded.enabled  ||
                                      m_env.minPitch != m_envLoaded.minPitch ||
                                      m_env.maxPitch != m_envLoaded.maxPitch ||
                                      m_env.minYaw   != m_envLoaded.minYaw   ||
                                      m_env.maxYaw   != m_envLoaded.maxYaw   ||
                                      m_env.minRoll  != m_envLoaded.minRoll  ||
                                      m_env.maxRoll  != m_envLoaded.maxRoll));

    if (m_envChanged)
    {
        // Naming the preset and counting the affected shots is what stops this from
        // reading like a per-shot setting. It is the most surprising thing the
        // dialog does, so it is never silent.
        int shots = 0;
        if (m_scenario)
        {
            for (const CameraFrame& c : m_scenario->cameras)
                if (c.kind == CameraKind::Entity &&
                    c.presetType == m_presetType && c.presetAngle == m_presetAngle)
                    ++shots;
        }

        CString msg;
        msg.Format(_T("This gimbal envelope belongs to the camera preset \"%s / %s\", ")
                   _T("not to this shot.\n\n")
                   _T("Changing it affects %d camera shot%s in this scenario, plus any ")
                   _T("shot in another scenario that mounts the same preset.\n\n")
                   _T("Save it to Cameras.ini?"),
                   (LPCTSTR)CString(CA2W(m_presetType.c_str())),
                   (LPCTSTR)CString(CA2W(m_presetAngle.c_str())),
                   shots, (shots == 1) ? _T("") : _T("s"));
        if (AfxMessageBox(msg, MB_YESNO | MB_ICONWARNING) != IDYES)
            return;   // keep the dialog open
    }

    CDialogEx::OnOK();
}
