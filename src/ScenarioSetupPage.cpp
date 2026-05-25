#include "pch.h"
#include "ScenarioSetupPage.h"
#include "FormatUtil.h"
#include "Scenario.h"

#include <cstdlib>

namespace
{
    const FFieldHelp kFields[] = {
        { IDC_EDIT_SCENARIO_NAME,         _T("Display name of the scenario."), true },
        { IDC_EDIT_SCENARIO_DESCRIPTION,  _T("Free-text description of the scenario's purpose."), false },
        { IDC_RADIO_DIS_V6,               _T("Emit PDUs using IEEE 1278.1-1995 (DIS v6) layout."), true },
        { IDC_RADIO_DIS_V7,               _T("Emit PDUs using IEEE 1278.1-2012 (DIS v7) layout. Default."), true },
        { IDC_EDIT_EXERCISE_ID,           _T("DIS Exercise ID. Range 1-255."), true },
        { IDC_EDIT_SITE_ID,               _T("DIS Site ID for this application. Range 1-65534."), true },
        { IDC_EDIT_APPLICATION_ID,        _T("DIS Application ID for this exercise. Range 1-65534."), true },
        { IDC_DATETIME_SCENARIO_START,    _T("Wall-clock start time of the scenario."), true },
        { IDC_EDIT_DURATION_SECONDS,      _T("Total scenario length in seconds."), true },
        { IDC_EDIT_DEFAULT_UPDATE_RATE,   _T("Default entity update rate in Hz (used when an entity has no override)."), true },
        { IDC_COMBO_DEFAULT_COORD_MODE,   _T("Coordinate system used by default for new entities (Geodetic / Local / ECEF)."), true },
        { IDC_EDIT_ORIGIN_LAT,            _T("Scenario origin latitude in decimal degrees. Used to anchor local coordinates."), false },
        { IDC_EDIT_ORIGIN_LON,            _T("Scenario origin longitude in decimal degrees."), false },
        { IDC_EDIT_ORIGIN_ALT,            _T("Scenario origin altitude in meters above MSL."), false },
        { IDC_BTN_USE_ENTITY_AS_ORIGIN,   _T("Copy the currently selected entity's position into the scenario origin fields."), false },
        { IDC_BTN_CLEAR_ORIGIN,           _T("Clear the scenario origin (lat / lon / alt)."), false },
        { IDC_CHK_VALIDATE_BEFORE_SEND,   _T("Run scenario validation before sending PDUs over the network."), false },
        { IDC_CHK_VALIDATE_BEFORE_RECORD, _T("Run scenario validation before starting a recording."), false },
        { IDC_CHK_VALIDATE_BEFORE_REPLAY, _T("Run scenario validation before replaying a recording."), false },
        { IDC_RADIO_PHYS_IGNORE,   _T("Don't check authored motion against airframe physical envelope. Default."), false },
        { IDC_RADIO_PHYS_VALIDATE, _T("Validator warns when motion exceeds airframe envelope (max speed, ceiling, G, etc.); wire output unchanged."), false },
        { IDC_RADIO_PHYS_LIMIT,    _T("Sampler clamps motion to airframe envelope at runtime (Phase 2 — wired up in a follow-on)."), false },
        { IDC_CHK_SPEED_MULT,      _T("Apply a scenario-wide speed multiplier to all entities (per-entity overrides apply)."), false },
        { IDC_EDIT_SPEED_MULT,     _T("Scenario-wide speed multiplier. 1.0 = no change; >1 faster; <1 slower."), false },
    };
}

BEGIN_MESSAGE_MAP(CScenarioSetupPage, CHelpAwarePage)
    ON_CBN_SELCHANGE(IDC_COMBO_DEFAULT_COORD_MODE,
                     &CScenarioSetupPage::OnDefaultCoordModeChanged)
    ON_BN_CLICKED(IDC_RADIO_PHYS_IGNORE,   &CScenarioSetupPage::OnPhysModeRadio)
    ON_BN_CLICKED(IDC_RADIO_PHYS_VALIDATE, &CScenarioSetupPage::OnPhysModeRadio)
    ON_BN_CLICKED(IDC_RADIO_PHYS_LIMIT,    &CScenarioSetupPage::OnPhysModeRadio)
    ON_BN_CLICKED(IDC_CHK_SPEED_MULT,      &CScenarioSetupPage::OnSpeedMultiplierToggle)
END_MESSAGE_MAP()

void CScenarioSetupPage::OnDefaultCoordModeChanged()
{
    // Live-commit the default coord mode so Asset.OnAddAsset and
    // Motion.OnAddSegment see the latest value when they read
    // scenario->defaultCoordMode (without waiting for Save/Validate to
    // flush WriteTo).
    if (!m_scenario) return;
    if (const CComboBox* cb = (const CComboBox*)GetDlgItem(IDC_COMBO_DEFAULT_COORD_MODE))
    {
        switch (cb->GetCurSel()) {
            case 0: m_scenario->defaultCoordMode = CoordMode::LatLonAlt; break;
            case 1: m_scenario->defaultCoordMode = CoordMode::Local;     break;
            case 2: m_scenario->defaultCoordMode = CoordMode::ECEF;      break;
        }
    }
}

void CScenarioSetupPage::OnPhysModeRadio()
{
    if (!m_scenario) return;
    if (IsDlgButtonChecked(IDC_RADIO_PHYS_VALIDATE) == BST_CHECKED)
        m_scenario->defaultPhysicalModel = PhysicalModelMode::Validate;
    else if (IsDlgButtonChecked(IDC_RADIO_PHYS_LIMIT) == BST_CHECKED)
        m_scenario->defaultPhysicalModel = PhysicalModelMode::Limit;
    else
        m_scenario->defaultPhysicalModel = PhysicalModelMode::Ignore;
}

void CScenarioSetupPage::OnSpeedMultiplierToggle()
{
    if (!m_scenario) return;
    m_scenario->speedMultiplierEnabled =
        IsDlgButtonChecked(IDC_CHK_SPEED_MULT) == BST_CHECKED;
}

void CScenarioSetupPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

BOOL CScenarioSetupPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_DEFAULT_COORD_MODE))
    {
        cb->AddString(_T("Lat/Lon/Alt"));
        cb->AddString(_T("Local X/Y/Z"));
        cb->AddString(_T("ECEF"));
        cb->SetCurSel(1); // Local X/Y/Z — matches Scenario::defaultCoordMode
    }

    CheckDlgButton(IDC_RADIO_DIS_V7, BST_CHECKED);
    CheckDlgButton(IDC_RADIO_DIS_V6, BST_UNCHECKED);

    // Seed the IDs with the scenario defaults so users see "1 / 1 / 1"
    // instead of empty boxes (which read back as 0 and form invalid PDUs).
    SetDlgItemInt(IDC_EDIT_EXERCISE_ID,    1, FALSE);
    SetDlgItemInt(IDC_EDIT_SITE_ID,        1, FALSE);
    SetDlgItemInt(IDC_EDIT_APPLICATION_ID, 1, FALSE);

    // Seed the scenario origin: San Francisco at sea level on WGS-84. This
    // matches the Asset page's Lat/Lon/Alt default, so Local-mode entities
    // start at (0,0,0) and produce the same ECEF as the LatLonAlt default.
    SetDlgItemText(IDC_EDIT_ORIGIN_LAT, _T("37.7749"));
    SetDlgItemText(IDC_EDIT_ORIGIN_LON, _T("-122.4194"));
    SetDlgItemText(IDC_EDIT_ORIGIN_ALT, _T("0.0"));

    // Physical Model Defaults — default to Ignore + multiplier=1.0 disabled.
    CheckDlgButton(IDC_RADIO_PHYS_IGNORE,   BST_CHECKED);
    CheckDlgButton(IDC_RADIO_PHYS_VALIDATE, BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_PHYS_LIMIT,    BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_SPEED_MULT,      BST_UNCHECKED);
    SetDlgItemText(IDC_EDIT_SPEED_MULT, _T("1.0"));

    return TRUE;
}

namespace
{
    double ReadDoubleText(const CWnd& wnd, UINT id, double fallback)
    {
        CString text;
        wnd.GetDlgItemText(id, text);
        if (text.IsEmpty())
            return fallback;
        CT2A ascii(text);
        char* end = nullptr;
        const double v = std::strtod(ascii.m_psz, &end);
        if (end == ascii.m_psz)
            return fallback;
        return v;
    }
}

void CScenarioSetupPage::WriteTo(Scenario& scenario) const
{
    if (!::IsWindow(GetSafeHwnd()))
        return;

    BOOL ok = FALSE;
    const UINT ex   = GetDlgItemInt(IDC_EDIT_EXERCISE_ID,    &ok, FALSE);
    if (ok) scenario.exerciseId    = static_cast<uint8_t>(ex & 0xFF);
    const UINT site = GetDlgItemInt(IDC_EDIT_SITE_ID,        &ok, FALSE);
    if (ok) scenario.siteId        = static_cast<uint16_t>(site & 0xFFFF);
    const UINT app  = GetDlgItemInt(IDC_EDIT_APPLICATION_ID, &ok, FALSE);
    if (ok) scenario.applicationId = static_cast<uint16_t>(app & 0xFFFF);

    scenario.protocolVersion = IsDlgButtonChecked(IDC_RADIO_DIS_V6) ? 6 : 7;

    // Origin drives Local-ENU ↔ ECEF conversion on the Asset page.
    scenario.originLatDeg = ReadDoubleText(*this, IDC_EDIT_ORIGIN_LAT, scenario.originLatDeg);
    scenario.originLonDeg = ReadDoubleText(*this, IDC_EDIT_ORIGIN_LON, scenario.originLonDeg);
    scenario.originAltM   = ReadDoubleText(*this, IDC_EDIT_ORIGIN_ALT, scenario.originAltM);

    // Default coord mode combo: 0=Lat/Lon/Alt, 1=Local, 2=ECEF.
    if (const CComboBox* cb = (const CComboBox*)GetDlgItem(IDC_COMBO_DEFAULT_COORD_MODE))
    {
        const int idx = cb->GetCurSel();
        if      (idx == 0) scenario.defaultCoordMode = CoordMode::LatLonAlt;
        else if (idx == 1) scenario.defaultCoordMode = CoordMode::Local;
        else if (idx == 2) scenario.defaultCoordMode = CoordMode::ECEF;
    }

    // Name + description are surfaced on this page; capture them.
    CString text;
    GetDlgItemText(IDC_EDIT_SCENARIO_NAME, text);
    if (!text.IsEmpty()) { CT2A ascii(text); scenario.name = ascii.m_psz; }
    GetDlgItemText(IDC_EDIT_SCENARIO_DESCRIPTION, text);
    CT2A descAscii(text);
    scenario.description = descAscii.m_psz;

    // Duration + default update rate.
    scenario.durationSeconds      = ReadDoubleText(*this, IDC_EDIT_DURATION_SECONDS,       scenario.durationSeconds);
    scenario.defaultUpdateRateHz  = ReadDoubleText(*this, IDC_EDIT_DEFAULT_UPDATE_RATE,    scenario.defaultUpdateRateHz);

    // Physical Model Defaults — radios + speed multiplier.
    if (IsDlgButtonChecked(IDC_RADIO_PHYS_VALIDATE) == BST_CHECKED)
        scenario.defaultPhysicalModel = PhysicalModelMode::Validate;
    else if (IsDlgButtonChecked(IDC_RADIO_PHYS_LIMIT) == BST_CHECKED)
        scenario.defaultPhysicalModel = PhysicalModelMode::Limit;
    else
        scenario.defaultPhysicalModel = PhysicalModelMode::Ignore;
    scenario.speedMultiplierEnabled =
        IsDlgButtonChecked(IDC_CHK_SPEED_MULT) == BST_CHECKED;
    scenario.speedMultiplier =
        ReadDoubleText(*this, IDC_EDIT_SPEED_MULT, scenario.speedMultiplier);
}

void CScenarioSetupPage::ReadFrom(const Scenario& scenario)
{
    if (!::IsWindow(GetSafeHwnd())) return;

    SetDlgItemText(IDC_EDIT_SCENARIO_NAME,        CA2T(scenario.name.c_str()));
    SetDlgItemText(IDC_EDIT_SCENARIO_DESCRIPTION, CA2T(scenario.description.c_str()));

    CheckDlgButton(IDC_RADIO_DIS_V6, scenario.protocolVersion == 6 ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_DIS_V7, scenario.protocolVersion == 7 ? BST_CHECKED : BST_UNCHECKED);

    SetDlgItemInt(IDC_EDIT_EXERCISE_ID,    scenario.exerciseId,    FALSE);
    SetDlgItemInt(IDC_EDIT_SITE_ID,        scenario.siteId,        FALSE);
    SetDlgItemInt(IDC_EDIT_APPLICATION_ID, scenario.applicationId, FALSE);

    SetDlgItemText(IDC_EDIT_ORIGIN_LAT, FormatDoubleTrim(scenario.originLatDeg));
    SetDlgItemText(IDC_EDIT_ORIGIN_LON, FormatDoubleTrim(scenario.originLonDeg));
    SetDlgItemText(IDC_EDIT_ORIGIN_ALT, FormatDoubleTrim(scenario.originAltM));

    SetDlgItemText(IDC_EDIT_DURATION_SECONDS,     FormatDoubleTrim(scenario.durationSeconds));
    SetDlgItemText(IDC_EDIT_DEFAULT_UPDATE_RATE,  FormatDoubleTrim(scenario.defaultUpdateRateHz));

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_DEFAULT_COORD_MODE))
    {
        int idx = 0;
        switch (scenario.defaultCoordMode) {
            case CoordMode::LatLonAlt: idx = 0; break;
            case CoordMode::Local:     idx = 1; break;
            case CoordMode::ECEF:      idx = 2; break;
        }
        cb->SetCurSel(idx);
    }

    // Physical Model Defaults.
    CheckDlgButton(IDC_RADIO_PHYS_IGNORE,
                   scenario.defaultPhysicalModel == PhysicalModelMode::Ignore   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_PHYS_VALIDATE,
                   scenario.defaultPhysicalModel == PhysicalModelMode::Validate ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_PHYS_LIMIT,
                   scenario.defaultPhysicalModel == PhysicalModelMode::Limit    ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_SPEED_MULT,
                   scenario.speedMultiplierEnabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemText(IDC_EDIT_SPEED_MULT, FormatDoubleTrim(scenario.speedMultiplier));
}
