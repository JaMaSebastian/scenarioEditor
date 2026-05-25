#pragma once

#include "pch.h"
#include "Resource.h"
#include "EntityTypeCatalog.h"

#include <cstdint>
#include <string>

// Functional secondary "Add Catalog Node" dialog. The caller sets the
// target catalog + parent-tree context before DoModal(); on IDOK the
// new node is already inserted into the catalog and NewlyAdded()
// returns its identity (so the parent dialog can rebuild + select).
class CCatalogAddDialog : public CDialogEx
{
public:
    enum { IDD = IDD_CATALOG_ADD };
    CCatalogAddDialog(CWnd* pParent = nullptr) : CDialogEx(IDD, pParent) {}

    // The catalog we're modifying (parent dialog's edit copy).
    void SetCatalog(EntityTypeCatalog* cat) { m_catalog = cat; }

    // Parent tree context. type is CCatalogEditorDialog::NodeType cast
    // to int (we avoid the header dependency). k/d/c are the parent
    // address; id is the selected node's id (used as the default
    // category/subcategory parent when picking a child type).
    void SetParentContext(int parentType, uint8_t k, uint8_t d, uint8_t c, uint16_t selId)
    {
        m_parentType = parentType;
        m_k = k; m_d = d; m_c = c; m_selId = selId;
    }

    // After IDOK, identifies the row that was just inserted.
    struct AddedRef {
        int      type = 0;   // matches CCatalogEditorDialog::NodeType
        uint8_t  k = 0, d = 0, c = 0;
        uint16_t id = 0;
    };
    AddedRef NewlyAdded() const { return m_added; }

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnTypeChanged();
    void OnOK() override;
    DECLARE_MESSAGE_MAP()

private:
    enum AddType { TYPE_KIND = 0, TYPE_DOMAIN, TYPE_CATEGORY,
                   TYPE_SUBCATEGORY, TYPE_COUNTRY };

    // Compute the next available ID for the currently selected Type,
    // given the parent context. Returns 1 if no siblings exist.
    uint16_t AutoIdFor(int addType) const;

    // Build the "Under: X > Y > Z" breadcrumb for the current Type.
    CString  BreadcrumbFor(int addType) const;

    // Map current tree selection -> default Add type (so the common
    // case is one click + Enter).
    int      DefaultTypeFromContext() const;

    // Convenience: parent context resolution.
    // Picks the parent IDs (k, d, c) for inserting a child of the
    // current Type, considering the currently-selected tree node.
    void ResolveParent(int addType, uint8_t& outK, uint8_t& outD, uint8_t& outC) const;

    EntityTypeCatalog* m_catalog   = nullptr;
    int                m_parentType = 0;
    uint8_t            m_k = 0, m_d = 0, m_c = 0;
    uint16_t           m_selId     = 0;
    AddedRef           m_added;
};
