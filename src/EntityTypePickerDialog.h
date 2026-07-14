//=============================================================================
//  EntityTypePickerDialog.h
//-----------------------------------------------------------------------------
//  Declares CEntityTypePickerDialog, a read-only catalog tree picker used by the
//  Preview tab's "Change Entity" command. Displays the Kind/Domain/Category/
//  Subcategory hierarchy and returns the chosen Subcategory leaf; it never
//  mutates or saves the catalog.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-13
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"

#include <cstdint>

class EntityTypeCatalog;

// Read-only catalog tree picker used by the Preview tab's "Change Entity"
// command. Shows the Kind -> Domain -> Category -> Subcategory hierarchy from
// the (const) EntityTypeCatalog; OK is enabled only when a Subcategory leaf is
// selected. This dialog NEVER mutates or saves the catalog — it just returns the
// chosen (kind, domain, category, subcategory). (The full CatalogEditorDialog is
// a heavyweight editor that writes to disk and is intentionally not reused here.)
class CEntityTypePickerDialog : public CDialogEx
{
public:
    enum { IDD = IDD_ENTITY_TYPE_PICKER };
    CEntityTypePickerDialog(const EntityTypeCatalog* catalog,
                            uint8_t curKind, uint8_t curDomain,
                            uint8_t curCategory, uint8_t curSubcategory,
                            CWnd* parent = nullptr);

    bool HasSelection() const { return m_hasSel; }
    void GetSelection(uint8_t& k, uint8_t& d, uint8_t& c, uint8_t& sub) const
    {
        k = m_selK; d = m_selD; c = m_selC; sub = m_selSub;
    }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnTreeSelChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTreeDblClick(NMHDR* pNMHDR, LRESULT* pResult);
    DECLARE_MESSAGE_MAP()

private:
    struct Node { bool leaf; uint8_t k, d, c, sub; };
    static DWORD_PTR Pack(bool leaf, uint8_t k, uint8_t d, uint8_t c, uint8_t sub);
    static Node      Unpack(DWORD_PTR p);

    void BuildTree();           // populate the tree; select/expand the current leaf
    void UpdateOkEnabled();     // OK enabled only when a subcategory leaf is selected

    const EntityTypeCatalog* m_catalog = nullptr;
    uint8_t m_curK, m_curD, m_curC, m_curSub;   // preselect target

    bool    m_hasSel = false;
    uint8_t m_selK = 0, m_selD = 0, m_selC = 0, m_selSub = 0;
};
