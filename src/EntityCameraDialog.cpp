//=============================================================================
//  EntityCameraDialog.cpp
//-----------------------------------------------------------------------------
//  Implements CEntityCameraDialog: populates the Camera-preset, Target and
//  Transition dropdowns (source is fixed to the clicked entity), gates the zoom
//  row off its two checkboxes, and captures the operator's selections on OK.
//  Preset list comes from theApp.CameraPresets(); the target list is the
//  scenario's entities plus a "(focus on source)" entry.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-14
//=============================================================================
#include "pch.h"
#include "EntityCameraDialog.h"
#include "ScenarioEditor.h"        // theApp + CameraPresets()
#include "CameraPresetCatalog.h"
#include "CameraTargetExtents.h"   // does the chosen target have catalogued dimensions?

#include <string>

BEGIN_MESSAGE_MAP(CEntityCameraDialog, CDialogEx)
    ON_BN_CLICKED(IDC_CHK_ECAM_ZOOM,       &CEntityCameraDialog::OnZoomToggle)
    ON_BN_CLICKED(IDC_CHK_ECAM_DYNZOOM,    &CEntityCameraDialog::OnZoomToggle)
    ON_BN_CLICKED(IDC_CHK_ECAM_GIMBAL, &CEntityCameraDialog::OnLimitsToggle)
    ON_CBN_SELCHANGE(IDC_COMBO_ECAM_PRESET, &CEntityCameraDialog::OnPresetChange)
    ON_CBN_SELCHANGE(IDC_COMBO_ECAM_TARGET, &CEntityCameraDialog::OnTargetChange)
END_MESSAGE_MAP()

//
// OnInitDialog — show the fixed source and fill the three dropdowns.
//
BOOL CEntityCameraDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    if (CWnd* s = GetDlgItem(IDC_STATIC_ECAM_SOURCE))
        s->SetWindowText(m_sourceLabel);

    // Camera presets (config\Cameras.ini): item-data = index into the catalog.
    if (CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_PRESET))
    {
        pre->ResetContent();
        const auto& presets = theApp.CameraPresets().Presets();
        for (size_t i = 0; i < presets.size(); ++i)
        {
            const int ix = pre->AddString(CString(CA2W(presets[i].DisplayLabel().c_str())));
            pre->SetItemData(ix, static_cast<DWORD_PTR>(i));
        }
        if (pre->GetCount() > 0) pre->SetCurSel(0);
    }

    // Target: "(focus on source)" (id 0) followed by every entity.
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
        tgt->SetCurSel(0);
    }

    // Zoom: off by default, so a camera added without touching this row behaves
    // exactly as cameras did before the feature existed. The FOV edit starts at
    // the chosen preset's own FOV rather than a magic number, so turning zoom on
    // and pressing OK is a no-op until the operator actually changes it.
    CheckDlgButton(IDC_CHK_ECAM_ZOOM,    BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_ECAM_DYNZOOM, BST_UNCHECKED);
    SeedFovFromPreset();
    {
        CString s;
        s.Format(_T("%g"), m_zoomFillPct);
        SetDlgItemText(IDC_EDIT_ECAM_FILL, s);
    }
    RefreshTargetSize();
    UpdateZoomFields();
    RefreshLimitsAvailability();

    // Transition: Crossfade/Blend (index 0) or Hard cut (index 1).
    if (CComboBox* trn = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_TRANS))
    {
        trn->ResetContent();
        trn->AddString(_T("Crossfade / Blend"));
        trn->AddString(_T("Hard cut"));
        trn->SetCurSel(0);
    }

    return TRUE;
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
    // A different camera means a different (or absent) envelope.
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
        probe.kind           = CameraKind::Entity;
        probe.sourceEntityId = m_sourceId;
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
    const CameraPreset* p = nullptr;

    CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_ECAM_PRESET);
    if (pre && pre->GetCurSel() >= 0)
    {
        const auto& presets = theApp.CameraPresets().Presets();
        const size_t ix = static_cast<size_t>(pre->GetItemData(pre->GetCurSel()));
        if (ix < presets.size()) p = &presets[ix];
    }

    m_presetHasLimits = (p != nullptr) && p->LimitsAvailable();

    // Name the reason on the control itself. The fix — authoring an envelope in
    // DISBrowser's hanger Settings tab — isn't guessable from a grayed checkbox.
    SetDlgItemText(IDC_CHK_ECAM_GIMBAL,
                   m_presetHasLimits ? _T("Gimbal limits")
                                     : _T("Gimbal limits (none defined for this camera)"));
    if (CWnd* w = GetDlgItem(IDC_CHK_ECAM_GIMBAL)) w->EnableWindow(m_presetHasLimits);

    // Default on when the camera has an envelope: an author who took the trouble to
    // define one almost always wants it enforced on new shots. Existing plays are
    // untouched — they carry their own saved flag.
    CheckDlgButton(IDC_CHK_ECAM_GIMBAL, m_presetHasLimits ? BST_CHECKED : BST_UNCHECKED);
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

    if (!pre || pre->GetCurSel() < 0)
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

    // The m_presetHasLimits guard stops a check made against one preset from surviving a
    // switch to another that has no envelope at all.
    m_limitsEnabled = m_presetHasLimits
                   && (IsDlgButtonChecked(IDC_CHK_ECAM_GIMBAL) == BST_CHECKED);

    m_presetIndex = static_cast<size_t>(pre->GetItemData(pre->GetCurSel()));
    m_targetId    = (tgt && tgt->GetCurSel() >= 0)
                    ? static_cast<uint16_t>(tgt->GetItemData(tgt->GetCurSel())) : 0;
    m_transition  = (trn && trn->GetCurSel() == 1)
                    ? CameraTransition::HardCut : CameraTransition::CrossfadeBlend;

    CDialogEx::OnOK();
}
