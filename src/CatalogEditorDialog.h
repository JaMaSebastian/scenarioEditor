//=============================================================================
//  CatalogEditorDialog.h
//-----------------------------------------------------------------------------
//  Declares CCatalogEditorDialog, the modal editor for EntityTypeCatalog.ini.
//  Presents the catalog as a tree plus a detail panel and per-Subcategory
//  attribute grid, supports add/delete of nodes and (sibling-propagating)
//  attributes, and commits edits to disk and the live catalog on Save.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-25
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "EntityTypeCatalog.h"

// Modal Catalog Editor. Edits a deep copy of the live catalog; on Save
// writes back to disk AND copies the edited state into the live
// instance. On Cancel, nothing is touched.
class CCatalogEditorDialog : public CDialogEx
{
public:
    enum { IDD = IDD_CATALOG_EDITOR };

    // live = pointer to the application's EntityTypeCatalog. The dialog
    // takes a deep copy on construction and commits back on Save.
    // path = absolute path to EntityTypeCatalog.ini on disk.
    CCatalogEditorDialog(EntityTypeCatalog* live, std::wstring path,
                         CWnd* pParent = nullptr);

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnTreeSelChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnAttributeRowChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnAttributeDblClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnAddNode();
    afx_msg void OnDeleteNode();
    afx_msg void OnAddAttribute();
    afx_msg void OnDeleteAttribute();
    afx_msg void OnSave();
    afx_msg void OnSaveClose();
    afx_msg void OnNameChanged();
    void OnCancel() override;
    DECLARE_MESSAGE_MAP()

private:
    // ----- Node type tag (packed into tree-item lParam) -----
    enum class NodeType : uint8_t { Root, Countries, Kind, Domain, Country, Category, Subcategory };

    struct NodeRef {
        NodeType type = NodeType::Root;
        uint8_t  k = 0, d = 0, c = 0;
        uint16_t id = 0;   // id of the leaf entry (kind id / domain id / country id / category id / subcat id)
    };

    static DWORD_PTR Pack(const NodeRef& r);
    static NodeRef   Unpack(DWORD_PTR p);

    // Find the live entry corresponding to a NodeRef (returns nullptr
    // for Root / Countries header / missing).
    CatalogEntry* FindEntry(const NodeRef& r);

    // Rebuild the entire tree from m_edit. Preserves the selection if
    // the previously-selected node still exists.
    void RebuildTree(NodeRef preserveSel);

    // Refresh detail panel + attribute grid for the current selection.
    void RefreshDetail();
    void RefreshAttributeGrid(const CatalogEntry* sub, const CatalogEntry* parentCat);
    void SetColumnWidths25_75();
    void UpdateHintFor(const CString& attributeName);

    // ----- Members -----
    EntityTypeCatalog*  m_live = nullptr;   // app's catalog (committed on save)
    std::wstring        m_path;             // absolute path to EntityTypeCatalog.ini
    EntityTypeCatalog   m_edit;             // editable deep copy
    NodeRef             m_sel;              // currently-selected node
    bool                m_suppressEvents = false;
};
