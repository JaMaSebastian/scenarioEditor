//=============================================================================
//  ScenarioSetupPage.h
//-----------------------------------------------------------------------------
//  Declares CScenarioSetupPage, the "Scenario Setup" tab of the Attributes
//  notebook. Edits scenario-wide settings: name/description, DIS version and
//  exercise/site/application IDs, origin lat/lon/alt, default coord mode,
//  duration/update rate, and physical-model defaults.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

struct Scenario;

//-----------------------------------------------------------------------------
// CScenarioSetupPage — dialog page for scenario-level attributes.
//   Marshals its controls to/from the shared Scenario via WriteTo/ReadFrom,
//   and live-commits a few fields (coord mode, physical model) as edited.
//-----------------------------------------------------------------------------
class CScenarioSetupPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_SCENARIO_SETUP_PAGE };
    CScenarioSetupPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

    // Copy the page's edit-control values into the given Scenario, including
    // exercise / site / application IDs, DIS protocol version, origin
    // lat/lon/alt, and default coord mode.
    void WriteTo(Scenario& scenario) const;

    // Inverse of WriteTo: populate this page's controls from a loaded
    // Scenario (called by File > Open).
    void ReadFrom(const Scenario& scenario);

    // Pointer to the live scenario; required so the Default Coord Mode
    // combo can commit IMMEDIATELY (other pages read scenario->defaultCoordMode
    // when adding new entities / segments and need the latest value).
    void SetScenario(Scenario* scenario) { m_scenario = scenario; }

protected:
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;
    afx_msg void OnDefaultCoordModeChanged();
    afx_msg void OnPhysModeRadio();
    afx_msg void OnClearOrigin();   // blank the origin fields -> provisional/uncommitted
    afx_msg void OnSpeedMultiplierToggle();
    DECLARE_MESSAGE_MAP()

private:
    Scenario* m_scenario = nullptr;
};
