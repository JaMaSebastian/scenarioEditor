#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

#include <cstdint>

struct Entity;
struct Scenario;
class  EntityTypeCatalog;
enum class CoordMode : uint8_t;   // full definition in Scenario.h

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

    afx_msg void OnKindChanged();
    afx_msg void OnDomainChanged();
    afx_msg void OnCategoryChanged();
    afx_msg void OnEntityCoordModeChanged();
    afx_msg void OnEntityPhysOverrideRadio();
    afx_msg void OnEntitySpeedMultToggle();
    afx_msg void OnEntityNameChanged();
    afx_msg void OnEditCatalog();
    afx_msg void OnInitialSpeedKillFocus();
    afx_msg void OnAddAsset();
    afx_msg void OnDeleteAsset();
    afx_msg void OnDuplicateAsset();
    afx_msg void OnMoveAsset();
    afx_msg void OnValidateAsset();
    afx_msg void OnAssetTreeSelChanged(NMHDR* pNMHDR, LRESULT* pResult);

    DECLARE_MESSAGE_MAP()

private:
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
