//=============================================================================
//  FoliageDialog.cpp
//-----------------------------------------------------------------------------
//  Implements CFoliageDialog: seeds the tree-type checkboxes, palm sub-kind
//  radios, and rendering-backend radios from the incoming FoliageConfig; keeps
//  the palm radios enabled only while "Palm" is checked; and reads the controls
//  back into m_config on Save (IDOK).
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-25
//=============================================================================
#include "pch.h"
#include "FoliageDialog.h"

BEGIN_MESSAGE_MAP(CFoliageDialog, CDialogEx)
    ON_BN_CLICKED(IDC_CHK_FOLIAGE_PALM, &CFoliageDialog::OnPalmToggle)
END_MESSAGE_MAP()

//
// OnInitDialog — reflect the current FoliageConfig into the controls.
//
BOOL CFoliageDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    CheckDlgButton(IDC_CHK_FOLIAGE_OAK,      m_config.oak      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_FOLIAGE_BIGTREES, m_config.bigTrees ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_FOLIAGE_PALM,     m_config.palm     ? BST_CHECKED : BST_UNCHECKED);

    // Palm sub-kind (one-of-three).
    CheckRadioButton(IDC_RADIO_FOLIAGE_PALM_ALL, IDC_RADIO_FOLIAGE_PALM_STRAIGHT,
                     m_config.palmKind == PalmKind::Tall     ? IDC_RADIO_FOLIAGE_PALM_TALL
                   : m_config.palmKind == PalmKind::Straight ? IDC_RADIO_FOLIAGE_PALM_STRAIGHT
                                                             : IDC_RADIO_FOLIAGE_PALM_ALL);

    // Rendering backend (one-of-three).
    CheckRadioButton(IDC_RADIO_FOLIAGE_INENGINE, IDC_RADIO_FOLIAGE_BLENDERGIS,
                     m_config.renderMode == FoliageRenderMode::I3dm       ? IDC_RADIO_FOLIAGE_I3DM
                   : m_config.renderMode == FoliageRenderMode::BlenderGIS ? IDC_RADIO_FOLIAGE_BLENDERGIS
                                                                          : IDC_RADIO_FOLIAGE_INENGINE);

    UpdatePalmRadios();
    return TRUE;
}

//
// OnPalmToggle — the Palm checkbox changed: gate the sub-kind radios.
//
void CFoliageDialog::OnPalmToggle()
{
    UpdatePalmRadios();
}

//
// UpdatePalmRadios — the All/Tall/Straight radios are only meaningful while Palm
//   is checked, so enable/disable them to match.
//
void CFoliageDialog::UpdatePalmRadios()
{
    const BOOL on = (IsDlgButtonChecked(IDC_CHK_FOLIAGE_PALM) == BST_CHECKED);
    for (int id = IDC_RADIO_FOLIAGE_PALM_ALL; id <= IDC_RADIO_FOLIAGE_PALM_STRAIGHT; ++id)
        if (CWnd* w = GetDlgItem(id)) w->EnableWindow(on);
}

//
// OnOK — "Save": read the controls into m_config, then close with IDOK. Only the
//   in-memory config is updated here; the caller stores it on the scenario and it
//   reaches scenario.ini on the next scenario save.
//
void CFoliageDialog::OnOK()
{
    m_config.oak      = (IsDlgButtonChecked(IDC_CHK_FOLIAGE_OAK)      == BST_CHECKED);
    m_config.bigTrees = (IsDlgButtonChecked(IDC_CHK_FOLIAGE_BIGTREES) == BST_CHECKED);
    m_config.palm     = (IsDlgButtonChecked(IDC_CHK_FOLIAGE_PALM)     == BST_CHECKED);

    m_config.palmKind =
        (IsDlgButtonChecked(IDC_RADIO_FOLIAGE_PALM_TALL)     == BST_CHECKED) ? PalmKind::Tall
      : (IsDlgButtonChecked(IDC_RADIO_FOLIAGE_PALM_STRAIGHT) == BST_CHECKED) ? PalmKind::Straight
                                                                            : PalmKind::All;

    m_config.renderMode =
        (IsDlgButtonChecked(IDC_RADIO_FOLIAGE_I3DM)       == BST_CHECKED) ? FoliageRenderMode::I3dm
      : (IsDlgButtonChecked(IDC_RADIO_FOLIAGE_BLENDERGIS) == BST_CHECKED) ? FoliageRenderMode::BlenderGIS
                                                                          : FoliageRenderMode::InEngine;

    CDialogEx::OnOK();
}
