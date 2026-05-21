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

protected:
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;
    DECLARE_MESSAGE_MAP()
};
