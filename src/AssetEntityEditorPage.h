//=============================================================================
//  AssetEntityEditorPage.h
//-----------------------------------------------------------------------------
//  Declares CAssetEntityEditorPage, the "Assets / Entities" property page. It
//  presents a tree of the scenario's entities and a detail panel for editing
//  the selected entity's DIS type (catalog-backed cascading combos), identity,
//  initial position/orientation, timing, and per-entity physical-model options.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

#include <cstdint>

struct Entity;
struct Scenario;
class  EntityTypeCatalog;
enum class CoordMode : uint8_t;   // full definition in Scenario.h

//-----------------------------------------------------------------------------
// CAssetEntityEditorPage — property page for editing scenario entities.
//   Owns the asset tree + detail controls; edits one entity at a time against
//   a non-owning live Scenario pointer, using a non-owning EntityTypeCatalog to
//   drive the DIS Kind/Domain/Category/Subcategory dropdowns.
//-----------------------------------------------------------------------------
class CAssetEntityEditorPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_ASSET_ENTITY_EDITOR_PAGE };
    CAssetEntityEditorPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

    // Catalog is loaded by the application at startup and handed in here
    // before Create() so OnInitDialog can populate the dropdowns.
    void SetCatalog(const EntityTypeCatalog* catalog) { m_catalog = catalog; }

    // The page edits one entity at a time, identified by m_selectedEntityIdx.
    // The owning dialog gives the page a (non-owning) pointer to the live
    // Scenario so the tree and detail panel can both work against the same
    // model. Caller is responsible for keeping the pointer alive.
    void SetScenario(Scenario* scenario) { m_scenario = scenario; }

    int  SelectedIndex() const { return m_selectedEntityIdx; }

    // Pull values for the wire fields V2 cares about from this page's
    // controls into entity. (Caller pulls the right entity from the model.)
    void WriteTo(Entity& entity,
                 double originLatDeg, double originLonDeg, double originAltM) const;

    // Inverse of WriteTo: push an Entity's fields onto this page's controls.
    void ReadFrom(const Entity& entity);

    // Rebuild the asset tree from the current Scenario and re-select the
    // selected index (clamped to the new range).
    void RefreshAssetTree();

protected:
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;

    //
    // Cascading DIS type combos: reselecting a higher level re-filters the
    // levels below it (Kind -> Domain -> Category -> Subcategory).
    //
    afx_msg void OnKindChanged();
    afx_msg void OnDomainChanged();
    afx_msg void OnCategoryChanged();
    //
    // OnEntityCoordModeChanged — user switched the Initial Coord Mode combo;
    // reprojects the displayed position triple into the new frame.
    //
    afx_msg void OnEntityCoordModeChanged();
    //
    // OnEntityPhysOverrideRadio — commits the selected physical-model override
    // radio into the active entity.
    //
    afx_msg void OnEntityPhysOverrideRadio();
    //
    // OnEntitySpeedMultToggle — commits the per-entity speed-multiplier
    // override checkbox state.
    //
    afx_msg void OnEntitySpeedMultToggle();
    //
    // OnEntityNameChanged — live-updates model + tree label as the name is typed.
    //
    afx_msg void OnEntityNameChanged();
    //
    // OnEditCatalog — opens the catalog editor dialog, then refreshes combos.
    //
    afx_msg void OnEditCatalog();
    //
    // OnInitialSpeedKillFocus — normalizes the initial-speed field (parses units).
    //
    afx_msg void OnInitialSpeedKillFocus();
    //
    // Asset-tree toolbar actions: add / delete / duplicate / reorder / validate
    // the selected entity.
    //
    afx_msg void OnAddAsset();
    afx_msg void OnDeleteAsset();
    afx_msg void OnDuplicateAsset();
    afx_msg void OnMoveAsset();
    afx_msg void OnValidateAsset();
    //
    // OnAssetTreeSelChanged — commits the previous entity, then loads the newly
    // selected one into the detail controls.
    //
    afx_msg void OnAssetTreeSelChanged(NMHDR* pNMHDR, LRESULT* pResult);

    DECLARE_MESSAGE_MAP()

private:
    //
    // ComboNumericValue — returns the numeric wire ID for a catalog combo:
    // the selected item's item-data, else a parse of typed text, else fallback.
    //
    int  ComboNumericValue(UINT comboId, int fallback) const;

    void RepopulateDomains();
    void RepopulateCategories();
    void RepopulateSubcategories();

    // Commit current control values into entities[m_selectedEntityIdx].
    // Origin args read from m_scenario; safe to call when no scenario is set.
    void CommitToActiveEntity();

    // Load entities[m_selectedEntityIdx] into the controls.
    void LoadActiveEntity();

    // Update the dynamic position label text to match the given coord mode,
    // and write the appropriate triple from `entity` into the three position
    // edit controls.
    void RelabelPositionRow(CoordMode mode);
    void WriteActivePositionFields(const Entity& entity, CoordMode mode);

    const EntityTypeCatalog* m_catalog          = nullptr;
    Scenario*                m_scenario         = nullptr;
    int                      m_selectedEntityIdx = 0;
    bool                     m_suppressTreeNotify = false;
};
