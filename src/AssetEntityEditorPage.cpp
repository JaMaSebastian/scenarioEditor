//=============================================================================
//  AssetEntityEditorPage.cpp
//-----------------------------------------------------------------------------
//  Implements CAssetEntityEditorPage: the asset tree (add/delete/duplicate/
//  move/validate), the catalog-backed cascading DIS type combos, and the
//  bidirectional bind between the detail controls and the Entity model
//  (ReadFrom / WriteTo), including position reprojection across coord modes.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "AssetEntityEditorPage.h"
#include "CatalogEditorDialog.h"
#include "CoordTransforms.h"
#include "EntityTypeCatalog.h"
#include "FormatUtil.h"
#include "Scenario.h"
#include "ScenarioEditor.h"   // theApp
#include "ScenarioWorker.h"   // WM_APP_MARK_DIRTY
#include "Validator.h"
#include "../log.h"

#include <cmath>

namespace
{
    // Walk up to the top-level frame and post the dirty notification. The
    // page lives inside the tab control, so GetParent() is the tab; we want
    // the dialog above that.
    void NotifyDirty(CWnd* page)
    {
        if (!page) return;
        // SendMessage (synchronous) — see MotionPathEditorPage.cpp's
        // matching comment. PostMessage would defer the MarkDirty call
        // past the m_suppressDirty window in RefreshUiFromScenario, so
        // every load would flip the dialog to "modified".
        if (CWnd* top = page->GetTopLevelParent())
            top->SendMessage(WM_APP_MARK_DIRTY, 0, 0);
    }
}

#include <cstdlib>
#include <cwchar>
#include <utility>

namespace
{
    const FFieldHelp kFields[] = {
        { IDC_BTN_ADD_ASSET,            _T("Add a new asset / entity to the scenario."), false },
        { IDC_BTN_DELETE_ASSET,         _T("Delete the selected asset / entity."), false },
        { IDC_BTN_DUPLICATE_ASSET,      _T("Duplicate the selected asset / entity."), false },
        { IDC_BTN_MOVE_ASSET,           _T("Reparent / reorder the selected asset within the tree."), false },
        { IDC_BTN_VALIDATE_ASSET,       _T("Run validation on the selected asset only."), false },

        { IDC_CHK_ENTITY_ENABLED,       _T("Include this entity in scenario output. Unchecked entities are skipped."), false },
        { IDC_EDIT_ENTITY_NAME,         _T("Human-readable entity name (editor use only)."), true },
        { IDC_EDIT_ENTITY_DESCRIPTION,  _T("Free-text description of this entity."), false },
        { IDC_EDIT_ENTITY_MARKING,      _T("DIS Marking Text (max 11 ASCII chars)."), false },
        { IDC_COMBO_ENTITY_FORCE_ID,    _T("DIS Force ID: Other, Friendly, Opposing, or Neutral."), true },
        { IDC_COMBO_ENTITY_KIND,        _T("DIS Entity Type - Kind (Platform, Munition, Life Form, etc.). Pick from catalog or type a raw SISO-REF-010 ID."), true },
        { IDC_COMBO_ENTITY_DOMAIN,      _T("DIS Entity Type - Domain (Land, Air, Surface, Subsurface, Space). Filtered by selected Kind."), true },
        { IDC_COMBO_ENTITY_COUNTRY,     _T("DIS Entity Type - Country (SISO-REF-010). Pick from catalog or type a raw numeric code."), true },
        { IDC_COMBO_ENTITY_CATEGORY,    _T("DIS Entity Type - Category. Filtered by Kind + Domain."), true },
        { IDC_COMBO_ENTITY_SUBCATEGORY, _T("DIS Entity Type - Subcategory. Filtered by Kind + Domain + Category."), false },
        { IDC_EDIT_ENTITY_SPECIFIC,     _T("DIS Entity Type - Specific (free numeric)."), false },
        { IDC_EDIT_ENTITY_EXTRA,        _T("DIS Entity Type - Extra (free numeric)."), false },

        { IDC_EDIT_ENTITY_SITE_ID,      _T("DIS Site ID for this entity. Range 1-65534."), true },
        { IDC_EDIT_ENTITY_APP_ID,       _T("DIS Application ID for this entity. Range 1-65534."), true },
        { IDC_EDIT_ENTITY_ID,           _T("DIS Entity ID (must be unique within Site+App). Range 1-65534."), true },

        { IDC_COMBO_ENTITY_COORD_MODE,  _T("Coordinate system for this entity's initial position."), true },
        { IDC_EDIT_ENTITY_LAT,          _T("Initial position component A — meaning depends on Initial Coord Mode: Lat (deg) / Local X (m) / ECEF X (m)."), false },
        { IDC_EDIT_ENTITY_LON,          _T("Initial position component B — Lon (deg) / Local Y (m) / ECEF Y (m)."), false },
        { IDC_EDIT_ENTITY_ALT,          _T("Initial position component C — Alt (m above MSL) / Local Z (m) / ECEF Z (m)."), false },
        // Position edits use a single dynamic row; help text below covers
        // all three modes via IDC_EDIT_ENTITY_LAT/LON/ALT (the labels
        // relabel at runtime based on Initial Coord Mode).

        // Physical Model override radios + per-entity speed multiplier.
        { IDC_RADIO_PHYS_INHERIT,    _T("Inherit the scenario's Physical Model Default (Scenario Setup tab)."), false },
        { IDC_RADIO_PHYS_E_IGNORE,   _T("Skip envelope checks for this entity, even when the scenario default says Validate/Limit."), false },
        { IDC_RADIO_PHYS_E_VALIDATE, _T("Validator warns when this entity's motion exceeds its airframe envelope."), false },
        { IDC_RADIO_PHYS_E_LIMIT,    _T("Sampler clamps this entity's motion to airframe envelope at runtime (Phase 2)."), false },
        { IDC_CHK_E_SPEED_MULT,      _T("Override the scenario-wide speed multiplier for this entity only."), false },
        { IDC_EDIT_E_SPEED_MULT,     _T("Per-entity speed multiplier (active when the override checkbox is on)."), false },
        { IDC_EDIT_ENTITY_HEADING,      _T("Initial heading in degrees (0 = north, clockwise)."), false },
        { IDC_EDIT_ENTITY_PITCH,        _T("Initial pitch in degrees."), false },
        { IDC_EDIT_ENTITY_ROLL,         _T("Initial roll in degrees."), false },
        { IDC_EDIT_ENTITY_INITIAL_SPEED,_T("Initial linear speed in meters per second."), false },

        { IDC_EDIT_ENTITY_BEGIN_TIME,   _T("Scenario time (s) when the entity becomes active."), true },
        { IDC_EDIT_ENTITY_END_TIME,     _T("Scenario time (s) when the entity becomes inactive."), false },
        { IDC_EDIT_ENTITY_UPDATE_RATE,  _T("Per-entity update rate in Hz. Leave blank to use scenario default."), false },
    };

    // Format a catalog entry as "<name> (<id>)" so the user always sees
    // the numeric ID alongside the friendly label.
    CString FormatEntry(const CatalogEntry& e)
    {
        CString s;
        s.Format(_T("%S (%u)"), e.name.c_str(), static_cast<unsigned>(e.id));
        return s;
    }

    // Populate a combo from a catalog vector. Items carry their numeric ID
    // in item-data so GetItemData(idx) recovers the wire value.
    void FillCombo(CComboBox* cb, const std::vector<CatalogEntry>& entries,
                   uint16_t selectIfPresent)
    {
        if (!cb) return;
        cb->ResetContent();
        int matchedIndex = -1;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const CatalogEntry& e = entries[i];
            const int idx = cb->AddString(FormatEntry(e));
            if (idx >= 0)
                cb->SetItemData(idx, static_cast<DWORD_PTR>(e.id));
            if (e.id == selectIfPresent)
                matchedIndex = idx;
        }
        if (matchedIndex >= 0)
        {
            cb->SetCurSel(matchedIndex);
        }
        else
        {
            // Not in catalog (user-typed numeric or no catalog loaded).
            // Leave the typed text intact, but if there isn't any, show
            // the raw number so the field never reads back as 0.
            CString existing;
            cb->GetWindowText(existing);
            if (existing.IsEmpty())
            {
                CString numeric;
                numeric.Format(_T("%u"), static_cast<unsigned>(selectIfPresent));
                cb->SetWindowText(numeric);
            }
        }
    }
}

BEGIN_MESSAGE_MAP(CAssetEntityEditorPage, CHelpAwarePage)
    ON_CBN_SELCHANGE(IDC_COMBO_ENTITY_KIND,     &CAssetEntityEditorPage::OnKindChanged)
    ON_CBN_SELCHANGE(IDC_COMBO_ENTITY_DOMAIN,   &CAssetEntityEditorPage::OnDomainChanged)
    ON_CBN_SELCHANGE(IDC_COMBO_ENTITY_CATEGORY, &CAssetEntityEditorPage::OnCategoryChanged)
    ON_CBN_SELCHANGE(IDC_COMBO_ENTITY_COORD_MODE,
                                                &CAssetEntityEditorPage::OnEntityCoordModeChanged)
    ON_BN_CLICKED   (IDC_RADIO_PHYS_INHERIT,    &CAssetEntityEditorPage::OnEntityPhysOverrideRadio)
    ON_BN_CLICKED   (IDC_RADIO_PHYS_E_IGNORE,   &CAssetEntityEditorPage::OnEntityPhysOverrideRadio)
    ON_BN_CLICKED   (IDC_RADIO_PHYS_E_VALIDATE, &CAssetEntityEditorPage::OnEntityPhysOverrideRadio)
    ON_BN_CLICKED   (IDC_RADIO_PHYS_E_LIMIT,    &CAssetEntityEditorPage::OnEntityPhysOverrideRadio)
    ON_BN_CLICKED   (IDC_CHK_E_SPEED_MULT,      &CAssetEntityEditorPage::OnEntitySpeedMultToggle)
    ON_EN_CHANGE    (IDC_EDIT_ENTITY_NAME,      &CAssetEntityEditorPage::OnEntityNameChanged)
    ON_EN_KILLFOCUS (IDC_EDIT_ENTITY_INITIAL_SPEED,
                                                &CAssetEntityEditorPage::OnInitialSpeedKillFocus)
    ON_BN_CLICKED   (IDC_BTN_EDIT_CATALOG,      &CAssetEntityEditorPage::OnEditCatalog)
    ON_BN_CLICKED   (IDC_BTN_ADD_ASSET,         &CAssetEntityEditorPage::OnAddAsset)
    ON_BN_CLICKED   (IDC_BTN_DELETE_ASSET,      &CAssetEntityEditorPage::OnDeleteAsset)
    ON_BN_CLICKED   (IDC_BTN_DUPLICATE_ASSET,   &CAssetEntityEditorPage::OnDuplicateAsset)
    ON_BN_CLICKED   (IDC_BTN_MOVE_ASSET,        &CAssetEntityEditorPage::OnMoveAsset)
    ON_BN_CLICKED   (IDC_BTN_VALIDATE_ASSET,    &CAssetEntityEditorPage::OnValidateAsset)
    ON_NOTIFY       (TVN_SELCHANGED, IDC_TREE_ASSETS,
                                                &CAssetEntityEditorPage::OnAssetTreeSelChanged)
END_MESSAGE_MAP()

//
// GetFieldHelpTable — hands the base HelpAwarePage this page's field-help table.
//
void CAssetEntityEditorPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

//
// RefreshAssetTree — rebuilds the tree items from m_scenario->entities and
// reselects m_selectedEntityIdx (clamped). Suppresses selection notifications
// during the rebuild so it doesn't recurse into OnAssetTreeSelChanged.
//
void CAssetEntityEditorPage::RefreshAssetTree()
{
    if (!::IsWindow(GetSafeHwnd()) || !m_scenario) return;
    CTreeCtrl* tree = (CTreeCtrl*)GetDlgItem(IDC_TREE_ASSETS);
    if (!tree) return;

    m_suppressTreeNotify = true;
    tree->DeleteAllItems();

    const auto& ents = m_scenario->entities;
    if (ents.empty()) { m_suppressTreeNotify = false; return; }
    if (m_selectedEntityIdx < 0) m_selectedEntityIdx = 0;
    if (m_selectedEntityIdx >= static_cast<int>(ents.size()))
        m_selectedEntityIdx = static_cast<int>(ents.size()) - 1;

    HTREEITEM selected = nullptr;
    for (size_t i = 0; i < ents.size(); ++i)
    {
        CString label;
        if (ents[i].name.empty())
            label.Format(_T("Entity %u"), static_cast<unsigned>(ents[i].entityId));
        else
            label.Format(_T("%S  (id=%u)"), ents[i].name.c_str(),
                         static_cast<unsigned>(ents[i].entityId));
        HTREEITEM it = tree->InsertItem(label, TVI_ROOT, TVI_LAST);
        tree->SetItemData(it, static_cast<DWORD_PTR>(i));
        if (static_cast<int>(i) == m_selectedEntityIdx) selected = it;
    }
    if (selected) tree->SelectItem(selected);
    m_suppressTreeNotify = false;
}

//
// CommitToActiveEntity — writes the current control values into the selected
// entity, using the scenario origin for local/geodetic conversions.
//
void CAssetEntityEditorPage::CommitToActiveEntity()
{
    if (!m_scenario) return;
    auto& ents = m_scenario->entities;
    if (m_selectedEntityIdx < 0 || m_selectedEntityIdx >= static_cast<int>(ents.size()))
        return;
    WriteTo(ents[m_selectedEntityIdx],
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM);
}

//
// LoadActiveEntity — loads the selected entity (index clamped) into the controls.
//
void CAssetEntityEditorPage::LoadActiveEntity()
{
    if (!m_scenario) return;
    auto& ents = m_scenario->entities;
    if (ents.empty()) return;
    if (m_selectedEntityIdx < 0) m_selectedEntityIdx = 0;
    if (m_selectedEntityIdx >= static_cast<int>(ents.size()))
        m_selectedEntityIdx = static_cast<int>(ents.size()) - 1;
    ReadFrom(ents[m_selectedEntityIdx]);
}

//
// OnAssetTreeSelChanged — tree selection changed: commit edits to the
// previously selected entity, switch to the new one, and refresh labels.
//
void CAssetEntityEditorPage::OnAssetTreeSelChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    if (m_suppressTreeNotify || !m_scenario) return;

    LPNMTREEVIEW nm = (LPNMTREEVIEW)pNMHDR;
    const int prev = m_selectedEntityIdx;
    const int next = static_cast<int>(nm->itemNew.lParam);
    if (prev == next) return;

    // Commit edits to the previously selected entity, then switch.
    if (prev >= 0 && prev < static_cast<int>(m_scenario->entities.size()))
        CommitToActiveEntity();
    m_selectedEntityIdx = next;
    LoadActiveEntity();
    // Refresh tree labels so the just-committed entity's rename is visible.
    RefreshAssetTree();
}

//
// OnAddAsset — appends a new entity (next free entityId, scenario default coord
// mode), selects it, and marks the dialog dirty.
//
void CAssetEntityEditorPage::OnAddAsset()
{
    if (!m_scenario) return;
    CommitToActiveEntity();

    Entity fresh;
    uint16_t next = 1;
    for (const auto& e : m_scenario->entities)
        if (e.entityId >= next) next = static_cast<uint16_t>(e.entityId + 1);
    fresh.entityId = next;
    fresh.name     = "New Entity";
    // New entities inherit the scenario's default coord mode so the
    // Asset page comes up authoring in whatever frame the user picked on
    // Scenario Setup (typically Local X/Y/Z).
    fresh.initialCoordMode = m_scenario->defaultCoordMode;
    m_scenario->entities.push_back(std::move(fresh));
    m_selectedEntityIdx = static_cast<int>(m_scenario->entities.size()) - 1;
    RefreshAssetTree();
    LoadActiveEntity();
    NotifyDirty(this);
}

//
// OnDeleteAsset — removes the selected entity (always keeps at least one),
// reselects a valid index, and marks dirty.
//
void CAssetEntityEditorPage::OnDeleteAsset()
{
    if (!m_scenario) return;
    if (m_scenario->entities.size() <= 1) return; // keep at least one
    if (m_selectedEntityIdx < 0 ||
        m_selectedEntityIdx >= static_cast<int>(m_scenario->entities.size()))
        return;

    m_scenario->entities.erase(m_scenario->entities.begin() + m_selectedEntityIdx);
    if (m_selectedEntityIdx >= static_cast<int>(m_scenario->entities.size()))
        m_selectedEntityIdx = static_cast<int>(m_scenario->entities.size()) - 1;
    RefreshAssetTree();
    LoadActiveEntity();
    NotifyDirty(this);
}

//
// OnDuplicateAsset — copies the selected entity with a fresh entityId and a
// " (copy)" name suffix, selects the copy, and marks dirty.
//
void CAssetEntityEditorPage::OnDuplicateAsset()
{
    if (!m_scenario) return;
    if (m_selectedEntityIdx < 0 ||
        m_selectedEntityIdx >= static_cast<int>(m_scenario->entities.size()))
        return;
    CommitToActiveEntity();

    Entity copy = m_scenario->entities[m_selectedEntityIdx];
    uint16_t next = 1;
    for (const auto& e : m_scenario->entities)
        if (e.entityId >= next) next = static_cast<uint16_t>(e.entityId + 1);
    copy.entityId = next;
    if (!copy.name.empty()) copy.name += " (copy)";
    m_scenario->entities.push_back(std::move(copy));
    m_selectedEntityIdx = static_cast<int>(m_scenario->entities.size()) - 1;
    RefreshAssetTree();
    LoadActiveEntity();
    NotifyDirty(this);
}

//
// OnMoveAsset — reorders the selected entity (see inline note on the scheme).
//
void CAssetEntityEditorPage::OnMoveAsset()
{
    // v2: simple "shift selection down by one position" reorder. Wraps to top.
    if (!m_scenario || m_scenario->entities.size() < 2) return;
    if (m_selectedEntityIdx < 0 ||
        m_selectedEntityIdx >= static_cast<int>(m_scenario->entities.size()))
        return;
    const int n = static_cast<int>(m_scenario->entities.size());
    const int dst = (m_selectedEntityIdx + 1) % n;
    std::swap(m_scenario->entities[m_selectedEntityIdx], m_scenario->entities[dst]);
    m_selectedEntityIdx = dst;
    RefreshAssetTree();
    LoadActiveEntity();
    NotifyDirty(this);
}

//
// OnValidateAsset — commits pending edits, runs the full Validator, filters the
// report to issues about this entity's id, and shows a per-entity summary box.
//
void CAssetEntityEditorPage::OnValidateAsset()
{
    if (!m_scenario) return;
    if (m_selectedEntityIdx < 0 ||
        m_selectedEntityIdx >= static_cast<int>(m_scenario->entities.size()))
    {
        AfxMessageBox(_T("No asset is selected."), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Commit pending edits into the model so validation sees the latest UI.
    CommitToActiveEntity();

    // Run the full validator and filter to issues that mention this entity.
    const Entity& e = m_scenario->entities[m_selectedEntityIdx];
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "Entity %u ",
                  static_cast<unsigned>(e.entityId));

    const Validator::Report rep = (m_catalog != nullptr)
        ? Validator::Validate(*m_scenario, *m_catalog)
        : Validator::Validate(*m_scenario);
    int err = 0, warn = 0, info = 0;
    CString body;
    auto sevLabel = [](Validator::Severity s) -> const TCHAR* {
        switch (s) {
            case Validator::Severity::Error:   return _T("ERROR");
            case Validator::Severity::Warning: return _T("WARN ");
            default:                           return _T("INFO ");
        }
    };
    for (const auto& i : rep.issues)
    {
        // Match either an entity-level subject ("Entity N (name)") or a
        // motion-level subject ("Entity N / Motion M") belonging to this id.
        if (i.subject.rfind(prefix, 0) != 0) continue;
        switch (i.severity) {
            case Validator::Severity::Error:   ++err;  break;
            case Validator::Severity::Warning: ++warn; break;
            case Validator::Severity::Info:    ++info; break;
        }
        CString line;
        line.Format(_T("[%s] %s — %s\n"),
                    sevLabel(i.severity),
                    CString(CA2T(i.subject.c_str())).GetString(),
                    CString(CA2T(i.message.c_str())).GetString());
        body += line;
    }

    CString header;
    header.Format(_T("Entity %u  —  Errors: %d   Warnings: %d   Info: %d\n\n"),
                  static_cast<unsigned>(e.entityId), err, warn, info);
    if (body.IsEmpty()) body = _T("(No issues found.)");

    AfxMessageBox(header + body,
                  MB_OK | (err > 0 ? MB_ICONWARNING : MB_ICONINFORMATION));
}

//
// OnInitDialog — seeds the static combos (Force ID, Coord Mode), the wire-field
// defaults, and the catalog-backed cascading combos so a fresh page is
// well-formed before any entity is selected.
//
BOOL CAssetEntityEditorPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_FORCE_ID))
    {
        cb->AddString(_T("Other"));
        cb->AddString(_T("Friendly"));
        cb->AddString(_T("Opposing"));
        cb->AddString(_T("Neutral"));
        cb->SetCurSel(1); // Friendly default
    }

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_COORD_MODE))
    {
        cb->AddString(_T("Lat/Lon/Alt"));
        cb->AddString(_T("Local X/Y/Z"));
        cb->AddString(_T("ECEF"));
        cb->SetCurSel(1); // Local X/Y/Z — matches Entity::initialCoordMode default
    }

    // Seed the wire-relevant fields so the first PDU is well-formed even if
    // the user clicks Playback > Start without touching anything.
    SetDlgItemInt(IDC_EDIT_ENTITY_SITE_ID, 1, FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_APP_ID,  1, FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_ID,      1, FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_SPECIFIC,0, FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_EXTRA,   0, FALSE);
    SetDlgItemText(IDC_EDIT_ENTITY_MARKING, _T("SCN-001"));

    // Defaults: San Francisco at sea level, matching the scenario origin
    // seeded by ScenarioSetupPage. With origin == entity position, the
    // Local-ENU offsets are (0, 0, 0).
    // Seed the three position edits with the default-mode Lat/Lon/Alt
    // values; ReadFrom will overwrite once an entity is loaded, but a
    // fresh dialog (before any selection) shows sensible numbers.
    SetDlgItemText(IDC_EDIT_ENTITY_LAT, _T("37.7749"));
    SetDlgItemText(IDC_EDIT_ENTITY_LON, _T("-122.4194"));
    SetDlgItemText(IDC_EDIT_ENTITY_ALT, _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_HEADING, _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_PITCH,   _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_ROLL,    _T("0.0"));

    // Populate catalog-backed combos. Defaults pre-select Platform (1) /
    // Air (2) / USA (225) / Category 1 / Subcategory 1, matching the
    // Entity defaults in Scenario.h.
    if (m_catalog)
    {
        FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_KIND),
                  m_catalog->Kinds(), /*defaultKind*/ 1);
        RepopulateDomains();
        FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_COUNTRY),
                  m_catalog->Countries(), /*defaultCountry*/ 225);
        RepopulateCategories();
        RepopulateSubcategories();
    }
    else
    {
        // No catalog loaded — keep dropdowns empty per §7.5, but seed text
        // with raw IDs so the PDU is still well-formed.
        SetDlgItemText(IDC_COMBO_ENTITY_KIND,        _T("1"));
        SetDlgItemText(IDC_COMBO_ENTITY_DOMAIN,      _T("2"));
        SetDlgItemText(IDC_COMBO_ENTITY_COUNTRY,     _T("225"));
        SetDlgItemText(IDC_COMBO_ENTITY_CATEGORY,    _T("1"));
        SetDlgItemText(IDC_COMBO_ENTITY_SUBCATEGORY, _T("1"));
    }

 //   LOG("DIAGNOSTIC: posting BN_CLICKED for IDC_BTN_ADD_ASSET from asset page Init");
 //   PostMessage(WM_COMMAND,
 //       MAKEWPARAM(IDC_BTN_ADD_ASSET, BN_CLICKED),
 //       reinterpret_cast<LPARAM>(GetDlgItem(IDC_BTN_ADD_ASSET)->GetSafeHwnd()));

    return TRUE;
}

//
// ComboNumericValue — resolves a catalog combo to its numeric wire ID: the
// selected item's item-data, else a parse of typed text, else fallback.
//
int CAssetEntityEditorPage::ComboNumericValue(UINT comboId, int fallback) const
{
    const CComboBox* cb = (const CComboBox*)GetDlgItem(comboId);
    if (!cb) return fallback;
    const int sel = cb->GetCurSel();
    if (sel != CB_ERR)
        return static_cast<int>(cb->GetItemData(sel));

    CString text;
    cb->GetWindowText(text);
    if (text.IsEmpty()) return fallback;
    CT2A ascii(text);
    wchar_t wbuf[64];
    swprintf_s(wbuf, L"%S", ascii.m_psz);
    wchar_t* end = nullptr;
    const long v = std::wcstol(wbuf, &end, 10);
    return (end == wbuf) ? fallback : static_cast<int>(v);
}

//
// RepopulateDomains — refills the Domain combo with the catalog domains valid
// for the currently selected Kind.
//
void CAssetEntityEditorPage::RepopulateDomains()
{
    if (!m_catalog) return;
    const uint8_t kindId = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_KIND, 1) & 0xFF);
    FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_DOMAIN),
              m_catalog->Domains(kindId), /*defaultDomain*/ 2);
}

//
// RepopulateCategories — refills the Category combo for the current Kind+Domain.
//
void CAssetEntityEditorPage::RepopulateCategories()
{
    if (!m_catalog) return;
    const uint8_t kindId   = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_KIND,   1) & 0xFF);
    const uint8_t domainId = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_DOMAIN, 2) & 0xFF);
    FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_CATEGORY),
              m_catalog->Categories(kindId, domainId), /*defaultCategory*/ 1);
}

//
// RepopulateSubcategories — refills the Subcategory combo for the current
// Kind+Domain+Category.
//
void CAssetEntityEditorPage::RepopulateSubcategories()
{
    if (!m_catalog) return;
    const uint8_t kindId     = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_KIND,     1) & 0xFF);
    const uint8_t domainId   = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_DOMAIN,   2) & 0xFF);
    const uint8_t categoryId = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_CATEGORY, 1) & 0xFF);
    FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_SUBCATEGORY),
              m_catalog->Subcategories(kindId, domainId, categoryId),
              /*defaultSubcategory*/ 1);
}

//
// OnKindChanged — Kind changed; re-cascade Domain, Category, and Subcategory.
//
void CAssetEntityEditorPage::OnKindChanged()
{
    RepopulateDomains();
    RepopulateCategories();
    RepopulateSubcategories();
}

//
// OnDomainChanged — Domain changed; re-cascade Category and Subcategory.
//
void CAssetEntityEditorPage::OnDomainChanged()
{
    RepopulateCategories();
    RepopulateSubcategories();
}

//
// OnCategoryChanged — Category changed; re-cascade Subcategory.
//
void CAssetEntityEditorPage::OnCategoryChanged()
{
    RepopulateSubcategories();
}

namespace { double ReadDoubleText(const CWnd&, UINT, double); }   // defined below

//
// OnEditCatalog — opens the catalog editor dialog and unconditionally refreshes
// the cascading combos afterward (a Save may have added/renamed entries).
//
void CAssetEntityEditorPage::OnEditCatalog()
{
    // Edit a deep copy of the live catalog; on Save the dialog rewrites
    // the INI and swaps the live instance. Cascading combos refresh
    // here so any add/delete/rename is visible immediately.
    CCatalogEditorDialog dlg(&theApp.MutableCatalog(),
                             theApp.CatalogPath(),
                             this);
    dlg.DoModal();

    // The dialog commits on every Save (not just Save&Close), and we
    // can't tell from IDOK/IDCANCEL whether a save happened — just
    // always refresh.
    if (m_catalog)
    {
        FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_KIND),
                  m_catalog->Kinds(),
                  static_cast<uint16_t>(ComboNumericValue(IDC_COMBO_ENTITY_KIND, 1) & 0xFF));
        RepopulateDomains();
        FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_COUNTRY),
                  m_catalog->Countries(),
                  static_cast<uint16_t>(ComboNumericValue(IDC_COMBO_ENTITY_COUNTRY, 225)));
        RepopulateCategories();
        RepopulateSubcategories();
    }
}

//
// OnInitialSpeedKillFocus — on focus loss, parses the initial-speed field
// (accepts unit suffixes) and rewrites it as a trimmed m/s value.
//
void CAssetEntityEditorPage::OnInitialSpeedKillFocus()
{
    CString t;
    GetDlgItemText(IDC_EDIT_ENTITY_INITIAL_SPEED, t);
    if (t.IsEmpty()) return;
    const double mps = ParseSpeedMps(t);
    if (!std::isnan(mps))
        SetDlgItemText(IDC_EDIT_ENTITY_INITIAL_SPEED, FormatDoubleTrim(mps));
}

//
// OnEntityNameChanged — per-keystroke: pushes the edited name into the model and
// updates the selected tree item's label (cheap, no full rebuild).
//
void CAssetEntityEditorPage::OnEntityNameChanged()
{
    if (m_suppressTreeNotify) return;
    if (!m_scenario) return;
    if (m_selectedEntityIdx < 0 ||
        m_selectedEntityIdx >= (int)m_scenario->entities.size())
        return;

    // Live-update the data model AND the tree label so the user sees the
    // rename happen as they type. EN_CHANGE fires per-keystroke; we keep
    // the work cheap (CTreeCtrl::SetItemText on one item, no rebuild).
    Entity& e = m_scenario->entities[m_selectedEntityIdx];
    CString t;
    GetDlgItemText(IDC_EDIT_ENTITY_NAME, t);
    CT2A a(t); e.name = a.m_psz ? a.m_psz : "";

    if (CTreeCtrl* tree = (CTreeCtrl*)GetDlgItem(IDC_TREE_ASSETS))
    {
        HTREEITEM hit = tree->GetSelectedItem();
        if (hit)
        {
            CString label;
            if (e.name.empty())
                label.Format(_T("Entity %u"), static_cast<unsigned>(e.entityId));
            else
                label.Format(_T("%S  (id=%u)"), e.name.c_str(),
                             static_cast<unsigned>(e.entityId));
            tree->SetItemText(hit, label);
        }
    }
    NotifyDirty(this);
}

//
// OnEntityPhysOverrideRadio — maps the checked physical-model radio to the
// active entity's physicalModelOverride and marks dirty.
//
void CAssetEntityEditorPage::OnEntityPhysOverrideRadio()
{
    if (!m_scenario) return;
    if (m_selectedEntityIdx < 0 ||
        m_selectedEntityIdx >= (int)m_scenario->entities.size())
        return;
    Entity& entity = m_scenario->entities[m_selectedEntityIdx];

    if (IsDlgButtonChecked(IDC_RADIO_PHYS_E_VALIDATE) == BST_CHECKED)
        entity.physicalModelOverride = PhysicalModelOverride::Validate;
    else if (IsDlgButtonChecked(IDC_RADIO_PHYS_E_LIMIT) == BST_CHECKED)
        entity.physicalModelOverride = PhysicalModelOverride::Limit;
    else if (IsDlgButtonChecked(IDC_RADIO_PHYS_E_IGNORE) == BST_CHECKED)
        entity.physicalModelOverride = PhysicalModelOverride::Ignore;
    else
        entity.physicalModelOverride = PhysicalModelOverride::Inherit;
    NotifyDirty(this);
}

//
// OnEntitySpeedMultToggle — mirrors the speed-multiplier override checkbox into
// the active entity and marks dirty.
//
void CAssetEntityEditorPage::OnEntitySpeedMultToggle()
{
    if (!m_scenario) return;
    if (m_selectedEntityIdx < 0 ||
        m_selectedEntityIdx >= (int)m_scenario->entities.size())
        return;
    Entity& entity = m_scenario->entities[m_selectedEntityIdx];
    entity.overrideSpeedMultiplier =
        IsDlgButtonChecked(IDC_CHK_E_SPEED_MULT) == BST_CHECKED;
    NotifyDirty(this);
}

//
// OnEntityCoordModeChanged — coord-mode combo flipped; reprojects the displayed
// position through ECEF so the new-mode numbers describe the same point (see
// inline steps), then relabels the row and marks dirty.
//
void CAssetEntityEditorPage::OnEntityCoordModeChanged()
{
    // The user just flipped the Initial Coord Mode combo. Pivot the
    // currently-displayed position triple through ECEF so the new-mode
    // numbers describe the same physical point.
    if (!m_scenario) return;
    if (m_selectedEntityIdx < 0 ||
        m_selectedEntityIdx >= (int)m_scenario->entities.size())
        return;
    Entity& entity = m_scenario->entities[m_selectedEntityIdx];

    const CoordMode oldMode = entity.initialCoordMode;

    CoordMode newMode = oldMode;
    if (const CComboBox* cb = (const CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_COORD_MODE))
    {
        switch (cb->GetCurSel()) {
            case 0: newMode = CoordMode::LatLonAlt; break;
            case 1: newMode = CoordMode::Local;     break;
            case 2: newMode = CoordMode::ECEF;      break;
        }
    }
    if (newMode == oldMode) {
        // Edge case: combo says same mode — just relabel for safety.
        RelabelPositionRow(newMode);
        WriteActivePositionFields(entity, newMode);
        return;
    }

    // Step 1: commit what the user sees into the OLD-mode fields so we
    // have a fixed point in space.
    const double posA = ReadDoubleText(*this, IDC_EDIT_ENTITY_LAT, 0.0);
    const double posB = ReadDoubleText(*this, IDC_EDIT_ENTITY_LON, 0.0);
    const double posC = ReadDoubleText(*this, IDC_EDIT_ENTITY_ALT, 0.0);
    switch (oldMode) {
        case CoordMode::LatLonAlt: entity.lat = posA; entity.lon = posB; entity.alt = posC; break;
        case CoordMode::Local:     entity.localX = posA; entity.localY = posB; entity.localZ = posC; break;
        case CoordMode::ECEF:      entity.ecefX = posA; entity.ecefY = posB; entity.ecefZ = posC; break;
    }

    // Step 2: convert OLD-mode → ECEF.
    double ecefX = 0, ecefY = 0, ecefZ = 0;
    switch (oldMode) {
        case CoordMode::LatLonAlt:
            CoordTransforms::GeodeticToEcefDeg(entity.lat, entity.lon, entity.alt,
                                               ecefX, ecefY, ecefZ);
            break;
        case CoordMode::Local:
            CoordTransforms::LocalEnuToEcefDeg(entity.localX, entity.localY, entity.localZ,
                                               m_scenario->originLatDeg,
                                               m_scenario->originLonDeg,
                                               m_scenario->originAltM,
                                               ecefX, ecefY, ecefZ);
            break;
        case CoordMode::ECEF:
            ecefX = entity.ecefX; ecefY = entity.ecefY; ecefZ = entity.ecefZ;
            break;
    }
    // Step 3: ECEF → NEW mode (and persist ECEF too so the wire is correct).
    entity.ecefX = ecefX; entity.ecefY = ecefY; entity.ecefZ = ecefZ;
    switch (newMode) {
        case CoordMode::LatLonAlt:
            CoordTransforms::EcefToGeodeticDeg(ecefX, ecefY, ecefZ,
                                               entity.lat, entity.lon, entity.alt);
            break;
        case CoordMode::Local:
            CoordTransforms::EcefToLocalEnuDeg(ecefX, ecefY, ecefZ,
                                               m_scenario->originLatDeg,
                                               m_scenario->originLonDeg,
                                               m_scenario->originAltM,
                                               entity.localX, entity.localY, entity.localZ);
            break;
        case CoordMode::ECEF:
            // already stored above
            break;
    }
    entity.initialCoordMode = newMode;

    RelabelPositionRow(newMode);
    WriteActivePositionFields(entity, newMode);
    NotifyDirty(this);
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

//
// WriteTo — pulls every editable control value into `entity`, converting the
// position (per the selected coord mode) and H/P/R into ECEF/DIS wire fields
// using the given scenario origin. Leaves fields unchanged when input is blank.
//
void CAssetEntityEditorPage::WriteTo(Entity& entity,
                                     double originLatDeg, double originLonDeg,
                                     double originAltM) const
{
    if (!::IsWindow(GetSafeHwnd()))
        return;

    BOOL ok = FALSE;
    const UINT site = GetDlgItemInt(IDC_EDIT_ENTITY_SITE_ID, &ok, FALSE);
    if (ok) entity.siteId        = static_cast<uint16_t>(site & 0xFFFF);
    const UINT app = GetDlgItemInt(IDC_EDIT_ENTITY_APP_ID, &ok, FALSE);
    if (ok) entity.applicationId = static_cast<uint16_t>(app & 0xFFFF);
    const UINT eid = GetDlgItemInt(IDC_EDIT_ENTITY_ID, &ok, FALSE);
    if (ok) entity.entityId      = static_cast<uint16_t>(eid & 0xFFFF);

    if (const CComboBox* cb = (const CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_FORCE_ID))
    {
        const int idx = cb->GetCurSel();
        if (idx >= 0 && idx <= 3)
            entity.forceId = static_cast<uint8_t>(idx);
    }

    entity.kind        = static_cast<uint8_t> (ComboNumericValue(IDC_COMBO_ENTITY_KIND,         entity.kind)        & 0xFF);
    entity.domain      = static_cast<uint8_t> (ComboNumericValue(IDC_COMBO_ENTITY_DOMAIN,       entity.domain)      & 0xFF);
    entity.country     = static_cast<uint16_t>(ComboNumericValue(IDC_COMBO_ENTITY_COUNTRY,      entity.country)     & 0xFFFF);
    entity.category    = static_cast<uint8_t> (ComboNumericValue(IDC_COMBO_ENTITY_CATEGORY,     entity.category)    & 0xFF);
    entity.subcategory = static_cast<uint8_t> (ComboNumericValue(IDC_COMBO_ENTITY_SUBCATEGORY,  entity.subcategory) & 0xFF);

    const UINT spec = GetDlgItemInt(IDC_EDIT_ENTITY_SPECIFIC, &ok, FALSE);
    if (ok) entity.specific = static_cast<uint8_t>(spec & 0xFF);
    const UINT extra = GetDlgItemInt(IDC_EDIT_ENTITY_EXTRA, &ok, FALSE);
    if (ok) entity.extra    = static_cast<uint8_t>(extra & 0xFF);

    // Read the three position edits — interpretation depends on the
    // currently-selected Initial Coord Mode (combo is the single source of
    // truth for which frame the edits represent).
    if (const CComboBox* cb = (const CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_COORD_MODE))
    {
        const int idx = cb->GetCurSel();
        if      (idx == 0) entity.initialCoordMode = CoordMode::LatLonAlt;
        else if (idx == 1) entity.initialCoordMode = CoordMode::Local;
        else if (idx == 2) entity.initialCoordMode = CoordMode::ECEF;
    }
    const double posA = ReadDoubleText(*this, IDC_EDIT_ENTITY_LAT, 0.0);
    const double posB = ReadDoubleText(*this, IDC_EDIT_ENTITY_LON, 0.0);
    const double posC = ReadDoubleText(*this, IDC_EDIT_ENTITY_ALT, 0.0);
    switch (entity.initialCoordMode)
    {
        case CoordMode::LatLonAlt:
            entity.lat = posA; entity.lon = posB; entity.alt = posC;
            CoordTransforms::GeodeticToEcefDeg(entity.lat, entity.lon, entity.alt,
                                               entity.ecefX, entity.ecefY, entity.ecefZ);
            break;
        case CoordMode::Local:
            entity.localX = posA; entity.localY = posB; entity.localZ = posC;
            CoordTransforms::LocalEnuToEcefDeg(entity.localX, entity.localY, entity.localZ,
                                               originLatDeg, originLonDeg, originAltM,
                                               entity.ecefX, entity.ecefY, entity.ecefZ);
            break;
        case CoordMode::ECEF:
            entity.ecefX = posA; entity.ecefY = posB; entity.ecefZ = posC;
            break;
    }

    // Read aviation H/P/R inputs and convert to DIS wire orientation
    // (psi, theta, phi) via the local-NED → ECEF rotation at origin
    // (§7.3.1 / §14.5.2). PduBuilder reads entity.psi/theta/phi unchanged.
    entity.headingDeg = ReadDoubleText(*this, IDC_EDIT_ENTITY_HEADING, entity.headingDeg);
    entity.pitchDeg   = ReadDoubleText(*this, IDC_EDIT_ENTITY_PITCH,   entity.pitchDeg);
    entity.rollDeg    = ReadDoubleText(*this, IDC_EDIT_ENTITY_ROLL,    entity.rollDeg);
    {
        double psi = 0.0, theta = 0.0, phi = 0.0;
        CoordTransforms::LocalHprToEcefPsiThetaPhi(entity.headingDeg, entity.pitchDeg, entity.rollDeg,
                                                   originLatDeg, originLonDeg,
                                                   psi, theta, phi);
        entity.psi   = static_cast<float>(psi);
        entity.theta = static_cast<float>(theta);
        entity.phi   = static_cast<float>(phi);
    }

    CString marking;
    GetDlgItemText(IDC_EDIT_ENTITY_MARKING, marking);
    if (!marking.IsEmpty())
    {
        CT2A ascii(marking);
        entity.marking = ascii.m_psz;
        if (entity.marking.size() > 11)
            entity.marking.resize(11);
    }

    // General-section fields that were decorative until now: Enabled
    // checkbox, Name, Description, and Timing block (begin / end / rate).
    entity.enabled = IsDlgButtonChecked(IDC_CHK_ENTITY_ENABLED) == BST_CHECKED;
    {
        CString t;
        GetDlgItemText(IDC_EDIT_ENTITY_NAME, t);
        CT2A a(t); entity.name = a.m_psz ? a.m_psz : "";
    }
    {
        CString t;
        GetDlgItemText(IDC_EDIT_ENTITY_DESCRIPTION, t);
        CT2A a(t); entity.description = a.m_psz ? a.m_psz : "";
    }
    entity.beginSecond     = ReadDoubleText(*this, IDC_EDIT_ENTITY_BEGIN_TIME,    entity.beginSecond);
    entity.endSecond       = ReadDoubleText(*this, IDC_EDIT_ENTITY_END_TIME,      entity.endSecond);
    entity.updateRateHz    = ReadDoubleText(*this, IDC_EDIT_ENTITY_UPDATE_RATE,   entity.updateRateHz);
    entity.initialSpeedMps = ReadDoubleText(*this, IDC_EDIT_ENTITY_INITIAL_SPEED, entity.initialSpeedMps);

    // Physical Model override (radios) + speed-multiplier override.
    if (IsDlgButtonChecked(IDC_RADIO_PHYS_E_VALIDATE) == BST_CHECKED)
        entity.physicalModelOverride = PhysicalModelOverride::Validate;
    else if (IsDlgButtonChecked(IDC_RADIO_PHYS_E_LIMIT) == BST_CHECKED)
        entity.physicalModelOverride = PhysicalModelOverride::Limit;
    else if (IsDlgButtonChecked(IDC_RADIO_PHYS_E_IGNORE) == BST_CHECKED)
        entity.physicalModelOverride = PhysicalModelOverride::Ignore;
    else
        entity.physicalModelOverride = PhysicalModelOverride::Inherit;
    entity.overrideSpeedMultiplier =
        IsDlgButtonChecked(IDC_CHK_E_SPEED_MULT) == BST_CHECKED;
    entity.speedMultiplier =
        ReadDoubleText(*this, IDC_EDIT_E_SPEED_MULT, entity.speedMultiplier);
}

namespace
{
    // Select a combo's item whose itemData matches `id`. If no match, drop
    // a raw "%u" string into the edit portion so the displayed value still
    // reflects the model.
    void SelectComboById(CComboBox* cb, uint16_t id)
    {
        if (!cb) return;
        const int count = cb->GetCount();
        for (int i = 0; i < count; ++i)
        {
            if (static_cast<uint16_t>(cb->GetItemData(i)) == id) {
                cb->SetCurSel(i);
                return;
            }
        }
        CString text;
        text.Format(_T("%u"), static_cast<unsigned>(id));
        cb->SetWindowText(text);
    }
}

//
// ReadFrom — pushes an entity's fields onto the controls: identity, timing,
// catalog combos (selected by ID in cascade order), coord mode + position row,
// and the physical-model / speed-multiplier options.
//
void CAssetEntityEditorPage::ReadFrom(const Entity& entity)
{
    if (!::IsWindow(GetSafeHwnd())) return;

    SetDlgItemInt(IDC_EDIT_ENTITY_SITE_ID,  entity.siteId,        FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_APP_ID,   entity.applicationId, FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_ID,       entity.entityId,      FALSE);

    SetDlgItemInt(IDC_EDIT_ENTITY_SPECIFIC, entity.specific,      FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_EXTRA,    entity.extra,         FALSE);

    SetDlgItemText(IDC_EDIT_ENTITY_MARKING, CA2T(entity.marking.c_str()));

    // General section (Enabled / Name / Description) + Timing.
    CheckDlgButton(IDC_CHK_ENTITY_ENABLED,
                   entity.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemText(IDC_EDIT_ENTITY_NAME,        CA2T(entity.name.c_str()));
    SetDlgItemText(IDC_EDIT_ENTITY_DESCRIPTION, CA2T(entity.description.c_str()));
    SetDlgItemText(IDC_EDIT_ENTITY_BEGIN_TIME,    FormatDoubleTrim(entity.beginSecond));
    SetDlgItemText(IDC_EDIT_ENTITY_END_TIME,      FormatDoubleTrim(entity.endSecond));
    SetDlgItemText(IDC_EDIT_ENTITY_UPDATE_RATE,   FormatDoubleTrim(entity.updateRateHz));
    SetDlgItemText(IDC_EDIT_ENTITY_INITIAL_SPEED, FormatDoubleTrim(entity.initialSpeedMps));

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_FORCE_ID))
        cb->SetCurSel(entity.forceId <= 3 ? entity.forceId : 0);

    // Catalog combos: select by ID. Order matters — Kind first so Domain/
    // Category/Subcategory have the correct cascade context, then re-fill.
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_KIND))
        SelectComboById(cb, entity.kind);
    RepopulateDomains();
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_DOMAIN))
        SelectComboById(cb, entity.domain);
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_COUNTRY))
        SelectComboById(cb, entity.country);
    RepopulateCategories();
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_CATEGORY))
        SelectComboById(cb, entity.category);
    RepopulateSubcategories();
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_SUBCATEGORY))
        SelectComboById(cb, entity.subcategory);

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_COORD_MODE))
    {
        int idx = 0;
        switch (entity.initialCoordMode) {
            case CoordMode::LatLonAlt: idx = 0; break;
            case CoordMode::Local:     idx = 1; break;
            case CoordMode::ECEF:      idx = 2; break;
        }
        cb->SetCurSel(idx);
    }

    RelabelPositionRow(entity.initialCoordMode);
    WriteActivePositionFields(entity, entity.initialCoordMode);

    // Physical model radios + speed-multiplier override.
    CheckDlgButton(IDC_RADIO_PHYS_INHERIT,
                   entity.physicalModelOverride == PhysicalModelOverride::Inherit  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_PHYS_E_IGNORE,
                   entity.physicalModelOverride == PhysicalModelOverride::Ignore   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_PHYS_E_VALIDATE,
                   entity.physicalModelOverride == PhysicalModelOverride::Validate ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_PHYS_E_LIMIT,
                   entity.physicalModelOverride == PhysicalModelOverride::Limit    ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_E_SPEED_MULT,
                   entity.overrideSpeedMultiplier ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemText(IDC_EDIT_E_SPEED_MULT, FormatDoubleTrim(entity.speedMultiplier));
}

//
// RelabelPositionRow — sets the position-row label text to match the coord mode.
//
void CAssetEntityEditorPage::RelabelPositionRow(CoordMode mode)
{
    const TCHAR* text = _T("Lat / Lon / Alt:");
    switch (mode) {
        case CoordMode::Local: text = _T("Local X / Y / Z (m):"); break;
        case CoordMode::ECEF:  text = _T("ECEF X / Y / Z (m):");  break;
        case CoordMode::LatLonAlt: default: break;
    }
    SetDlgItemText(IDC_LBL_ENTITY_POSITION, text);
}

//
// WriteActivePositionFields — writes the entity's position triple for the given
// coord mode into the three position edit controls (trimmed formatting).
//
void CAssetEntityEditorPage::WriteActivePositionFields(const Entity& entity, CoordMode mode)
{
    double a = 0.0, b = 0.0, c = 0.0;
    switch (mode) {
        case CoordMode::LatLonAlt: a = entity.lat;   b = entity.lon;   c = entity.alt;   break;
        case CoordMode::Local:     a = entity.localX; b = entity.localY; c = entity.localZ; break;
        case CoordMode::ECEF:      a = entity.ecefX; b = entity.ecefY; c = entity.ecefZ; break;
    }
    SetDlgItemText(IDC_EDIT_ENTITY_LAT, FormatDoubleTrim(a));
    SetDlgItemText(IDC_EDIT_ENTITY_LON, FormatDoubleTrim(b));
    SetDlgItemText(IDC_EDIT_ENTITY_ALT, FormatDoubleTrim(c));
}

