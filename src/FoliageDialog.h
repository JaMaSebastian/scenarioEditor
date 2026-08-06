//=============================================================================
//  FoliageDialog.h
//-----------------------------------------------------------------------------
//  Declares CFoliageDialog, the modal launched from the Preview tab "Foliage"
//  button. It captures which tree packs to place (Oak / Big Trees / Palm, with a
//  palm sub-kind) and which rendering backend to experiment with (In Engine /
//  i3dm / BlenderGIS). It only records the selection into a FoliageConfig; the
//  caller stores that on the scenario (persisted to scenario.ini [Foliage]).
//  Nothing is generated/rendered here.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-25
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "Scenario.h"   // FoliageConfig / PalmKind / FoliageRenderMode

// Modal "Foliage" dialog. Caller sets the current config before DoModal(); on
// IDOK (the "Save" button) Config() returns the edited selection.
class CFoliageDialog : public CDialogEx
{
public:
    enum { IDD = IDD_FOLIAGE };
    CFoliageDialog(CWnd* pParent = nullptr) : CDialogEx(IDD, pParent) {}

    // Seed the dialog from an existing selection (call before DoModal()).
    void SetConfig(const FoliageConfig& cfg) { m_config = cfg; }
    // Edited selection — valid after IDOK.
    const FoliageConfig& Config() const { return m_config; }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnPalmToggle();   // enable/disable the palm sub-kind radios
    DECLARE_MESSAGE_MAP()

private:
    // Enable the All/Tall/Straight radios only while "Palm" is checked.
    void UpdatePalmRadios();

    FoliageConfig m_config;
};
