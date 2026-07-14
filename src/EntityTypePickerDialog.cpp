//=============================================================================
//  EntityTypePickerDialog.cpp
//-----------------------------------------------------------------------------
//  Implements CEntityTypePickerDialog: building the read-only catalog tree,
//  preselecting the current entity type, packing/unpacking node identity into
//  tree lParams, enabling OK only for Subcategory leaves, and returning the
//  chosen (kind, domain, category, subcategory).
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-13
//=============================================================================
#include "pch.h"
#include "EntityTypePickerDialog.h"
#include "EntityTypeCatalog.h"

BEGIN_MESSAGE_MAP(CEntityTypePickerDialog, CDialogEx)
    ON_NOTIFY(TVN_SELCHANGED, IDC_TREE_ENTITY_TYPE, &CEntityTypePickerDialog::OnTreeSelChanged)
    ON_NOTIFY(NM_DBLCLK,      IDC_TREE_ENTITY_TYPE, &CEntityTypePickerDialog::OnTreeDblClick)
END_MESSAGE_MAP()

//
// CEntityTypePickerDialog::CEntityTypePickerDialog — store the (const) catalog
//   and the current entity type to preselect when the tree is built.
//
CEntityTypePickerDialog::CEntityTypePickerDialog(const EntityTypeCatalog* catalog,
                                                 uint8_t curKind, uint8_t curDomain,
                                                 uint8_t curCategory, uint8_t curSubcategory,
                                                 CWnd* parent)
    : CDialogEx(IDD, parent), m_catalog(catalog),
      m_curK(curKind), m_curD(curDomain), m_curC(curCategory), m_curSub(curSubcategory)
{
}

// lParam pack: bit32 = isLeaf, bits 24-31 = kind, 16-23 = domain, 8-15 = category, 0-7 = subcat.
DWORD_PTR CEntityTypePickerDialog::Pack(bool leaf, uint8_t k, uint8_t d, uint8_t c, uint8_t sub)
{
    return (static_cast<DWORD_PTR>(leaf ? 1 : 0) << 32) |
           (static_cast<DWORD_PTR>(k)   << 24) |
           (static_cast<DWORD_PTR>(d)   << 16) |
           (static_cast<DWORD_PTR>(c)   << 8)  |
            static_cast<DWORD_PTR>(sub);
}

//
// CEntityTypePickerDialog::Unpack — inverse of Pack; decode a tree lParam into
//   a Node (leaf flag + k/d/c/sub).
//
CEntityTypePickerDialog::Node CEntityTypePickerDialog::Unpack(DWORD_PTR p)
{
    Node n;
    n.leaf = ((p >> 32) & 0x1) != 0;
    n.k    = static_cast<uint8_t>((p >> 24) & 0xFF);
    n.d    = static_cast<uint8_t>((p >> 16) & 0xFF);
    n.c    = static_cast<uint8_t>((p >> 8)  & 0xFF);
    n.sub  = static_cast<uint8_t>( p        & 0xFF);
    return n;
}

//
// CEntityTypePickerDialog::OnInitDialog — build the tree and set the initial
//   OK-button enabled state.
//
BOOL CEntityTypePickerDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    BuildTree();
    UpdateOkEnabled();
    return TRUE;
}

//
// CEntityTypePickerDialog::BuildTree — populate the tree from the catalog
//   (Kind > Domain > Category > Subcategory), tagging each item with its packed
//   identity, then expand to and select the leaf matching the current type.
//
void CEntityTypePickerDialog::BuildTree()
{
    CTreeCtrl* tree = (CTreeCtrl*)GetDlgItem(IDC_TREE_ENTITY_TYPE);
    if (!tree || !m_catalog) return;

    tree->DeleteAllItems();
    HTREEITEM selLeaf = nullptr;

    for (const CatalogEntry& kind : m_catalog->Kinds())
    {
        const uint8_t k = static_cast<uint8_t>(kind.id);
        CString kl; kl.Format(_T("%u  %s"), kind.id, CString(kind.name.c_str()).GetString());
        HTREEITEM hKind = tree->InsertItem(kl);
        tree->SetItemData(hKind, Pack(false, k, 0, 0, 0));

        for (const CatalogEntry& dom : m_catalog->Domains(k))
        {
            const uint8_t d = static_cast<uint8_t>(dom.id);
            CString dl; dl.Format(_T("%u  %s"), dom.id, CString(dom.name.c_str()).GetString());
            HTREEITEM hDom = tree->InsertItem(dl, hKind);
            tree->SetItemData(hDom, Pack(false, k, d, 0, 0));

            for (const CatalogEntry& cat : m_catalog->Categories(k, d))
            {
                const uint8_t c = static_cast<uint8_t>(cat.id);
                CString cl; cl.Format(_T("%u  %s"), cat.id, CString(cat.name.c_str()).GetString());
                HTREEITEM hCat = tree->InsertItem(cl, hDom);
                tree->SetItemData(hCat, Pack(false, k, d, c, 0));

                for (const CatalogEntry& sc : m_catalog->Subcategories(k, d, c))
                {
                    const uint8_t sub = static_cast<uint8_t>(sc.id);
                    CString sl; sl.Format(_T("%u  %s"), sc.id, CString(sc.name.c_str()).GetString());
                    HTREEITEM hSub = tree->InsertItem(sl, hCat);
                    tree->SetItemData(hSub, Pack(true, k, d, c, sub));

                    if (k == m_curK && d == m_curD && c == m_curC && sub == m_curSub)
                        selLeaf = hSub;
                }
            }
        }
    }

    if (selLeaf)
    {
        tree->EnsureVisible(selLeaf);   // expands ancestors so the leaf is visible
        tree->SelectItem(selLeaf);
    }
}

//
// CEntityTypePickerDialog::UpdateOkEnabled — enable the OK button only when the
//   current tree selection is a Subcategory leaf.
//
void CEntityTypePickerDialog::UpdateOkEnabled()
{
    CTreeCtrl* tree = (CTreeCtrl*)GetDlgItem(IDC_TREE_ENTITY_TYPE);
    bool leaf = false;
    if (tree)
        if (HTREEITEM h = tree->GetSelectedItem())
            leaf = Unpack(tree->GetItemData(h)).leaf;
    if (CWnd* ok = GetDlgItem(IDOK)) ok->EnableWindow(leaf ? TRUE : FALSE);
}

//
// CEntityTypePickerDialog::OnTreeSelChanged — selection handler; re-evaluate the
//   OK-button enabled state.
//
void CEntityTypePickerDialog::OnTreeSelChanged(NMHDR*, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    UpdateOkEnabled();
}

//
// CEntityTypePickerDialog::OnTreeDblClick — double-click accepts the dialog when
//   the selected node is a Subcategory leaf.
//
void CEntityTypePickerDialog::OnTreeDblClick(NMHDR*, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    // Double-clicking a subcategory leaf accepts it.
    CTreeCtrl* tree = (CTreeCtrl*)GetDlgItem(IDC_TREE_ENTITY_TYPE);
    if (tree)
        if (HTREEITEM h = tree->GetSelectedItem())
            if (Unpack(tree->GetItemData(h)).leaf)
                OnOK();
}

//
// CEntityTypePickerDialog::OnOK — record the selected Subcategory leaf's
//   (k,d,c,sub) into the result fields and close; ignores non-leaf selections.
//
void CEntityTypePickerDialog::OnOK()
{
    CTreeCtrl* tree = (CTreeCtrl*)GetDlgItem(IDC_TREE_ENTITY_TYPE);
    if (!tree) return;
    HTREEITEM h = tree->GetSelectedItem();
    if (!h) return;
    const Node n = Unpack(tree->GetItemData(h));
    if (!n.leaf) return;   // only subcategory leaves are valid choices

    m_hasSel = true;
    m_selK = n.k; m_selD = n.d; m_selC = n.c; m_selSub = n.sub;
    CDialogEx::OnOK();
}
