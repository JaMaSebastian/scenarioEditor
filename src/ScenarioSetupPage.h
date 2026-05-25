#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

struct Scenario;

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
    afx_msg void OnSpeedMultiplierToggle();
    DECLARE_MESSAGE_MAP()

private:
    Scenario* m_scenario = nullptr;
};
