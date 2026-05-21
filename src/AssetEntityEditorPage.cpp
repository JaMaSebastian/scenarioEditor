#include "pch.h"
#include "AssetEntityEditorPage.h"
#include "CoordTransforms.h"
#include "EntityTypeCatalog.h"
#include "Scenario.h"
#include "ScenarioWorker.h"   // WM_APP_MARK_DIRTY
#include "../log.h"

namespace
{
    // Walk up to the top-level frame and post the dirty notification. The
    // page lives inside the tab control, so GetParent() is the tab; we want
    // the dialog above that.
    void NotifyDirty(CWnd* page)
    {
        if (!page) return;
        if (CWnd* top = page->GetTopLevelParent())
            top->PostMessage(WM_APP_MARK_DIRTY, 0, 0);
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
        { IDC_EDIT_ENTITY_LAT,          _T("Initial latitude in decimal degrees (used when coord mode = Lat/Lon/Alt)."), false },
        { IDC_EDIT_ENTITY_LON,          _T("Initial longitude in decimal degrees."), false },
        { IDC_EDIT_ENTITY_ALT,          _T("Initial altitude in meters above MSL."), false },
        { IDC_EDIT_ENTITY_LOCAL_X,      _T("Initial local X in meters (used when coord mode = Local)."), false },
        { IDC_EDIT_ENTITY_LOCAL_Y,      _T("Initial local Y in meters."), false },
        { IDC_EDIT_ENTITY_LOCAL_Z,      _T("Initial local Z in meters."), false },
        { IDC_EDIT_ENTITY_ECEF_X,       _T("Initial ECEF X in meters (used when coord mode = ECEF)."), false },
        { IDC_EDIT_ENTITY_ECEF_Y,       _T("Initial ECEF Y in meters."), false },
        { IDC_EDIT_ENTITY_ECEF_Z,       _T("Initial ECEF Z in meters."), false },
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
    ON_BN_CLICKED   (IDC_BTN_ADD_ASSET,         &CAssetEntityEditorPage::OnAddAsset)
    ON_BN_CLICKED   (IDC_BTN_DELETE_ASSET,      &CAssetEntityEditorPage::OnDeleteAsset)
    ON_BN_CLICKED   (IDC_BTN_DUPLICATE_ASSET,   &CAssetEntityEditorPage::OnDuplicateAsset)
    ON_BN_CLICKED   (IDC_BTN_MOVE_ASSET,        &CAssetEntityEditorPage::OnMoveAsset)
    ON_NOTIFY       (TVN_SELCHANGED, IDC_TREE_ASSETS,
                                                &CAssetEntityEditorPage::OnAssetTreeSelChanged)
END_MESSAGE_MAP()

void CAssetEntityEditorPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

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

void CAssetEntityEditorPage::CommitToActiveEntity()
{
    if (!m_scenario) return;
    auto& ents = m_scenario->entities;
    if (m_selectedEntityIdx < 0 || m_selectedEntityIdx >= static_cast<int>(ents.size()))
        return;
    WriteTo(ents[m_selectedEntityIdx],
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM);
}

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
}

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
    m_scenario->entities.push_back(std::move(fresh));
    m_selectedEntityIdx = static_cast<int>(m_scenario->entities.size()) - 1;
    RefreshAssetTree();
    LoadActiveEntity();
    NotifyDirty(this);
}

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
        cb->SetCurSel(0); // Lat/Lon/Alt — most user-friendly default per §15
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
    SetDlgItemText(IDC_EDIT_ENTITY_LAT, _T("37.7749"));
    SetDlgItemText(IDC_EDIT_ENTITY_LON, _T("-122.4194"));
    SetDlgItemText(IDC_EDIT_ENTITY_ALT, _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_HEADING, _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_PITCH,   _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_ROLL,    _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_LOCAL_X, _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_LOCAL_Y, _T("0.0"));
    SetDlgItemText(IDC_EDIT_ENTITY_LOCAL_Z, _T("0.0"));
    // ECEF echo of the LLA default — populated so the ECEF combo mode is
    // also pre-seeded with something sensible, even though the canonical
    // value gets overwritten by WriteTo() unless the user picks ECEF mode.
    SetDlgItemText(IDC_EDIT_ENTITY_ECEF_X, _T("-2706174.85"));
    SetDlgItemText(IDC_EDIT_ENTITY_ECEF_Y, _T("-4261059.49"));
    SetDlgItemText(IDC_EDIT_ENTITY_ECEF_Z, _T("3885725.49"));

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

void CAssetEntityEditorPage::RepopulateDomains()
{
    if (!m_catalog) return;
    const uint8_t kindId = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_KIND, 1) & 0xFF);
    FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_DOMAIN),
              m_catalog->Domains(kindId), /*defaultDomain*/ 2);
}

void CAssetEntityEditorPage::RepopulateCategories()
{
    if (!m_catalog) return;
    const uint8_t kindId   = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_KIND,   1) & 0xFF);
    const uint8_t domainId = static_cast<uint8_t>(ComboNumericValue(IDC_COMBO_ENTITY_DOMAIN, 2) & 0xFF);
    FillCombo((CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_CATEGORY),
              m_catalog->Categories(kindId, domainId), /*defaultCategory*/ 1);
}

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

void CAssetEntityEditorPage::OnKindChanged()
{
    RepopulateDomains();
    RepopulateCategories();
    RepopulateSubcategories();
}

void CAssetEntityEditorPage::OnDomainChanged()
{
    RepopulateCategories();
    RepopulateSubcategories();
}

void CAssetEntityEditorPage::OnCategoryChanged()
{
    RepopulateSubcategories();
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

    // Read all three position frames as the user typed them. The active
    // one (per Coord Mode combo) becomes the source of truth for ECEF.
    entity.lat    = ReadDoubleText(*this, IDC_EDIT_ENTITY_LAT,    entity.lat);
    entity.lon    = ReadDoubleText(*this, IDC_EDIT_ENTITY_LON,    entity.lon);
    entity.alt    = ReadDoubleText(*this, IDC_EDIT_ENTITY_ALT,    entity.alt);
    entity.localX = ReadDoubleText(*this, IDC_EDIT_ENTITY_LOCAL_X, entity.localX);
    entity.localY = ReadDoubleText(*this, IDC_EDIT_ENTITY_LOCAL_Y, entity.localY);
    entity.localZ = ReadDoubleText(*this, IDC_EDIT_ENTITY_LOCAL_Z, entity.localZ);
    entity.ecefX  = ReadDoubleText(*this, IDC_EDIT_ENTITY_ECEF_X,  entity.ecefX);
    entity.ecefY  = ReadDoubleText(*this, IDC_EDIT_ENTITY_ECEF_Y,  entity.ecefY);
    entity.ecefZ  = ReadDoubleText(*this, IDC_EDIT_ENTITY_ECEF_Z,  entity.ecefZ);

    if (const CComboBox* cb = (const CComboBox*)GetDlgItem(IDC_COMBO_ENTITY_COORD_MODE))
    {
        const int idx = cb->GetCurSel();
        if      (idx == 0) entity.initialCoordMode = CoordMode::LatLonAlt;
        else if (idx == 1) entity.initialCoordMode = CoordMode::Local;
        else if (idx == 2) entity.initialCoordMode = CoordMode::ECEF;
    }

    // Normalize to ECEF (§15.6 — wire frame). Whichever frame the user
    // picked becomes authoritative; we overwrite entity.ecefX/Y/Z so the
    // PduBuilder is unchanged.
    switch (entity.initialCoordMode)
    {
        case CoordMode::LatLonAlt:
            CoordTransforms::GeodeticToEcefDeg(entity.lat, entity.lon, entity.alt,
                                               entity.ecefX, entity.ecefY, entity.ecefZ);
            break;
        case CoordMode::Local:
            CoordTransforms::LocalEnuToEcefDeg(entity.localX, entity.localY, entity.localZ,
                                               originLatDeg, originLonDeg, originAltM,
                                               entity.ecefX, entity.ecefY, entity.ecefZ);
            break;
        case CoordMode::ECEF:
            // entity.ecefX/Y/Z already read above; nothing to do.
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

void CAssetEntityEditorPage::ReadFrom(const Entity& entity)
{
    if (!::IsWindow(GetSafeHwnd())) return;

    SetDlgItemInt(IDC_EDIT_ENTITY_SITE_ID,  entity.siteId,        FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_APP_ID,   entity.applicationId, FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_ID,       entity.entityId,      FALSE);

    SetDlgItemInt(IDC_EDIT_ENTITY_SPECIFIC, entity.specific,      FALSE);
    SetDlgItemInt(IDC_EDIT_ENTITY_EXTRA,    entity.extra,         FALSE);

    SetDlgItemText(IDC_EDIT_ENTITY_MARKING, CA2T(entity.marking.c_str()));

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

    CString buf;
    buf.Format(_T("%.6f"),  entity.lat);    SetDlgItemText(IDC_EDIT_ENTITY_LAT, buf);
    buf.Format(_T("%.6f"),  entity.lon);    SetDlgItemText(IDC_EDIT_ENTITY_LON, buf);
    buf.Format(_T("%.3f"),  entity.alt);    SetDlgItemText(IDC_EDIT_ENTITY_ALT, buf);
    buf.Format(_T("%.3f"),  entity.localX); SetDlgItemText(IDC_EDIT_ENTITY_LOCAL_X, buf);
    buf.Format(_T("%.3f"),  entity.localY); SetDlgItemText(IDC_EDIT_ENTITY_LOCAL_Y, buf);
    buf.Format(_T("%.3f"),  entity.localZ); SetDlgItemText(IDC_EDIT_ENTITY_LOCAL_Z, buf);
    buf.Format(_T("%.2f"),  entity.ecefX);  SetDlgItemText(IDC_EDIT_ENTITY_ECEF_X, buf);
    buf.Format(_T("%.2f"),  entity.ecefY);  SetDlgItemText(IDC_EDIT_ENTITY_ECEF_Y, buf);
    buf.Format(_T("%.2f"),  entity.ecefZ);  SetDlgItemText(IDC_EDIT_ENTITY_ECEF_Z, buf);

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
}

