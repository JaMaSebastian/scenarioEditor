#include "pch.h"
#include "CatalogEditorDialog.h"
#include "CatalogAddDialog.h"
#include "FormatUtil.h"
#include "../log.h"

#include <algorithm>
#include <cmath>

BEGIN_MESSAGE_MAP(CCatalogEditorDialog, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_ADD_NODE,         &CCatalogEditorDialog::OnAddNode)
    ON_BN_CLICKED(IDC_BTN_DELETE_NODE,      &CCatalogEditorDialog::OnDeleteNode)
    ON_BN_CLICKED(IDC_BTN_ADD_ATTRIBUTE,    &CCatalogEditorDialog::OnAddAttribute)
    ON_BN_CLICKED(IDC_BTN_DELETE_ATTRIBUTE, &CCatalogEditorDialog::OnDeleteAttribute)
    ON_BN_CLICKED(IDC_BTN_SAVE_CATALOG,     &CCatalogEditorDialog::OnSave)
    ON_BN_CLICKED(IDC_BTN_SAVE_CLOSE_CATALOG,&CCatalogEditorDialog::OnSaveClose)
    ON_EN_CHANGE(IDC_EDIT_CATALOG_NAME,     &CCatalogEditorDialog::OnNameChanged)
    ON_NOTIFY(TVN_SELCHANGED, IDC_TREE_CATALOG,
              &CCatalogEditorDialog::OnTreeSelChanged)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST_ATTRIBUTES,
              &CCatalogEditorDialog::OnAttributeRowChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_LIST_ATTRIBUTES,
              &CCatalogEditorDialog::OnAttributeDblClick)
END_MESSAGE_MAP()

// =========================================================================
// Hint text table — keys correspond to the well-known attribute names the
// Validator reads. Anything not listed yields an empty hint.
// =========================================================================

namespace
{
    struct AttrHint { const TCHAR* key; const TCHAR* hint; };
    const AttrHint kHints[] = {
        { _T("MaxSpeedMetersPerSecond"),         _T("Speed (m/s). Accepts Mach: e.g. \"Mach 1.2\" or \"M 0.8\" (343 m/s @ ISA sea level).") },
        { _T("CruiseSpeedMetersPerSecond"),      _T("Speed (m/s). Accepts Mach.") },
        { _T("StallSpeedMetersPerSecond"),       _T("Speed (m/s). Accepts Mach.") },
        { _T("NeverExceedSpeedMetersPerSecond"), _T("Speed (m/s). Accepts Mach.") },
        { _T("MaxTaxiSpeedMetersPerSecond"),     _T("Speed (m/s). Accepts Mach.") },
        { _T("MaxClimbRateMetersPerSecond"),     _T("Rate (m/s). ft/min input coming in a future version.") },
        { _T("MaxDescentRateMetersPerSecond"),   _T("Rate (m/s). ft/min input coming in a future version.") },
        { _T("NormalClimbRateMetersPerSecond"),  _T("Rate (m/s).") },
        { _T("NormalDescentRateMetersPerSecond"),_T("Rate (m/s).") },
        { _T("ServiceCeilingMeters"),            _T("Altitude (m). Feet input coming in a future version.") },
        { _T("MaxAltitudeMeters"),               _T("Altitude (m).") },
        { _T("MinAltitudeMeters"),               _T("Altitude (m).") },
        { _T("MaxBankAngleDeg"),                 _T("Angle (deg).") },
        { _T("NormalBankAngleDeg"),              _T("Angle (deg).") },
        { _T("MaxTurnRateDegPerSec"),            _T("Rate (deg/s).") },
        { _T("MaxPitchAngleDeg"),                _T("Angle (deg).") },
        { _T("MaxRollRateDegPerSec"),            _T("Rate (deg/s).") },
        { _T("MaxG"),                            _T("Load factor (g, unitless).") },
        { _T("MaxAccelMetersPerSecond2"),        _T("Acceleration (m/s\xB2).") },
        { _T("MaxDecelMetersPerSecond2"),        _T("Deceleration (m/s\xB2).") },
        { _T("LengthMeters"),                    _T("Dimension (m).") },
        { _T("WingspanMeters"),                  _T("Dimension (m).") },
        { _T("HeightMeters"),                    _T("Dimension (m).") },
    };
    const TCHAR* HintFor(const CString& key)
    {
        for (const auto& h : kHints) if (key == h.key) return h.hint;
        return _T("");
    }

    std::string Narrow(const CString& s)
    {
        if (s.IsEmpty()) return {};
        CT2A a(s);
        return a.m_psz ? std::string(a.m_psz) : std::string();
    }
}

// =========================================================================
// Tiny inline modal for prompting a single name string.
// =========================================================================

class CPromptDialog : public CDialogEx
{
public:
    enum { IDD = IDD_PROMPT_NAME };
    CPromptDialog(const CString& caption, const CString& prompt,
                  const CString& initial, CWnd* parent)
        : CDialogEx(IDD, parent), m_caption(caption), m_prompt(prompt), m_value(initial)
    {}
    const CString& Value() const { return m_value; }

protected:
    BOOL OnInitDialog() override
    {
        CDialogEx::OnInitDialog();
        SetWindowText(m_caption);
        SetDlgItemText(IDC_LBL_PROMPT,  m_prompt);
        SetDlgItemText(IDC_EDIT_PROMPT, m_value);
        if (CEdit* e = (CEdit*)GetDlgItem(IDC_EDIT_PROMPT))
        {
            e->SetSel(0, -1);
            e->SetFocus();
            return FALSE;  // tell MFC we set focus ourselves
        }
        return TRUE;
    }
    void OnOK() override
    {
        GetDlgItemText(IDC_EDIT_PROMPT, m_value);
        CDialogEx::OnOK();
    }
    CString m_caption, m_prompt, m_value;
};

// =========================================================================
// NodeRef <-> tree lParam packing
// =========================================================================
//   bits  0..7   = type
//   bits  8..15  = k
//   bits 16..23  = d
//   bits 24..31  = c
//   bits 32..47  = id    (uint16)

DWORD_PTR CCatalogEditorDialog::Pack(const NodeRef& r)
{
    return (static_cast<DWORD_PTR>(r.type) << 0) |
           (static_cast<DWORD_PTR>(r.k)    << 8) |
           (static_cast<DWORD_PTR>(r.d)    << 16) |
           (static_cast<DWORD_PTR>(r.c)    << 24) |
           (static_cast<DWORD_PTR>(r.id)   << 32);
}
CCatalogEditorDialog::NodeRef CCatalogEditorDialog::Unpack(DWORD_PTR p)
{
    NodeRef r;
    r.type = static_cast<NodeType>((p >> 0)  & 0xFF);
    r.k    = static_cast<uint8_t>( (p >> 8)  & 0xFF);
    r.d    = static_cast<uint8_t>( (p >> 16) & 0xFF);
    r.c    = static_cast<uint8_t>( (p >> 24) & 0xFF);
    r.id   = static_cast<uint16_t>((p >> 32) & 0xFFFF);
    return r;
}

// =========================================================================

CCatalogEditorDialog::CCatalogEditorDialog(EntityTypeCatalog* live, std::wstring path,
                                           CWnd* parent)
    : CDialogEx(IDD, parent), m_live(live), m_path(std::move(path))
{
    if (m_live) m_edit = *m_live;   // deep copy
}

BOOL CCatalogEditorDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // Attribute grid setup — 25 / 75 column split.
    if (CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_ATTRIBUTES))
    {
        lv->SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        lv->InsertColumn(0, _T("Attribute"), LVCFMT_LEFT, 100);
        lv->InsertColumn(1, _T("Value"),     LVCFMT_LEFT, 300);
        SetColumnWidths25_75();
    }

    SetDlgItemText(IDC_LBL_CATALOG_TYPE, _T("(no selection)"));
    SetDlgItemText(IDC_EDIT_CATALOG_ID,  _T(""));
    SetDlgItemText(IDC_EDIT_CATALOG_NAME,_T(""));
    SetDlgItemText(IDC_LBL_CATALOG_HINT, _T(""));
    if (CWnd* idCtl = GetDlgItem(IDC_EDIT_CATALOG_ID))
        idCtl->EnableWindow(FALSE);   // ID read-only (rename via Delete + Add)

    NodeRef none{};
    RebuildTree(none);

    return TRUE;
}

void CCatalogEditorDialog::SetColumnWidths25_75()
{
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_ATTRIBUTES);
    if (!lv) return;
    CRect rc; lv->GetClientRect(&rc);
    const int totalW = rc.Width() - ::GetSystemMetrics(SM_CXVSCROLL) - 4;
    if (totalW < 100) return;
    const int attrW = totalW / 4;
    lv->SetColumnWidth(0, attrW);
    lv->SetColumnWidth(1, totalW - attrW);
}

void CCatalogEditorDialog::UpdateHintFor(const CString& name)
{
    SetDlgItemText(IDC_LBL_CATALOG_HINT, HintFor(name));
}

// -------------------------------------------------------------------------
// Tree rebuild
// -------------------------------------------------------------------------

void CCatalogEditorDialog::RebuildTree(NodeRef preserveSel)
{
    CTreeCtrl* tree = (CTreeCtrl*)GetDlgItem(IDC_TREE_CATALOG);
    if (!tree) return;
    m_suppressEvents = true;
    tree->DeleteAllItems();

    HTREEITEM selected = nullptr;

    HTREEITEM root = tree->InsertItem(_T("Catalog"));
    tree->SetItemData(root, Pack(NodeRef{ NodeType::Root, 0, 0, 0, 0 }));
    if (preserveSel.type == NodeType::Root) selected = root;

    auto& kinds = m_edit.MutableKinds();
    std::sort(kinds.begin(), kinds.end(),
              [](const CatalogEntry& a, const CatalogEntry& b){ return a.id < b.id; });

    for (auto& kind : kinds)
    {
        CString lbl;
        lbl.Format(_T("Kind.%u  %s"), kind.id, CString(kind.name.c_str()).GetString());
        HTREEITEM hKind = tree->InsertItem(lbl, root);
        NodeRef rKind{ NodeType::Kind, static_cast<uint8_t>(kind.id), 0, 0, kind.id };
        tree->SetItemData(hKind, Pack(rKind));
        if (preserveSel.type == NodeType::Kind && preserveSel.id == kind.id) selected = hKind;

        auto& doms = m_edit.MutableDomains()[static_cast<uint8_t>(kind.id)];
        std::sort(doms.begin(), doms.end(),
                  [](const CatalogEntry& a, const CatalogEntry& b){ return a.id < b.id; });
        for (auto& dom : doms)
        {
            CString d;
            d.Format(_T("Domain.%u.%u  %s"), kind.id, dom.id,
                     CString(dom.name.c_str()).GetString());
            HTREEITEM hDom = tree->InsertItem(d, hKind);
            NodeRef rDom{ NodeType::Domain, static_cast<uint8_t>(kind.id),
                          static_cast<uint8_t>(dom.id), 0, dom.id };
            tree->SetItemData(hDom, Pack(rDom));
            if (preserveSel.type == NodeType::Domain &&
                preserveSel.k == kind.id && preserveSel.id == dom.id) selected = hDom;

            auto catIt = m_edit.MutableCategories().find(
                { static_cast<uint8_t>(kind.id), static_cast<uint8_t>(dom.id) });
            if (catIt == m_edit.MutableCategories().end()) continue;
            auto& cats = catIt->second;
            std::sort(cats.begin(), cats.end(),
                      [](const CatalogEntry& a, const CatalogEntry& b){ return a.id < b.id; });
            for (auto& cat : cats)
            {
                CString cl;
                cl.Format(_T("Category.%u.%u.%u  %s"), kind.id, dom.id, cat.id,
                          CString(cat.name.c_str()).GetString());
                HTREEITEM hCat = tree->InsertItem(cl, hDom);
                NodeRef rCat{ NodeType::Category, static_cast<uint8_t>(kind.id),
                              static_cast<uint8_t>(dom.id), static_cast<uint8_t>(cat.id),
                              cat.id };
                tree->SetItemData(hCat, Pack(rCat));
                if (preserveSel.type == NodeType::Category &&
                    preserveSel.k == kind.id && preserveSel.d == dom.id &&
                    preserveSel.id == cat.id) selected = hCat;

                auto subIt = m_edit.MutableSubcategories().find(
                    { static_cast<uint8_t>(kind.id),
                      static_cast<uint8_t>(dom.id),
                      static_cast<uint8_t>(cat.id) });
                if (subIt == m_edit.MutableSubcategories().end()) continue;
                auto& subs = subIt->second;
                std::sort(subs.begin(), subs.end(),
                          [](const CatalogEntry& a, const CatalogEntry& b){ return a.id < b.id; });
                for (auto& sub : subs)
                {
                    CString sl;
                    sl.Format(_T("Subcat.%u.%u.%u.%u  %s"),
                              kind.id, dom.id, cat.id, sub.id,
                              CString(sub.name.c_str()).GetString());
                    HTREEITEM hSub = tree->InsertItem(sl, hCat);
                    NodeRef rSub{ NodeType::Subcategory,
                                  static_cast<uint8_t>(kind.id),
                                  static_cast<uint8_t>(dom.id),
                                  static_cast<uint8_t>(cat.id),
                                  sub.id };
                    tree->SetItemData(hSub, Pack(rSub));
                    if (preserveSel.type == NodeType::Subcategory &&
                        preserveSel.k == kind.id && preserveSel.d == dom.id &&
                        preserveSel.c == cat.id && preserveSel.id == sub.id)
                        selected = hSub;
                }
            }
        }
    }

    // Countries branch.
    HTREEITEM hCountries = tree->InsertItem(_T("Countries"), root);
    tree->SetItemData(hCountries, Pack(NodeRef{ NodeType::Countries, 0, 0, 0, 0 }));
    if (preserveSel.type == NodeType::Countries) selected = hCountries;

    auto& countries = m_edit.MutableCountries();
    std::sort(countries.begin(), countries.end(),
              [](const CatalogEntry& a, const CatalogEntry& b){ return a.id < b.id; });
    for (auto& co : countries)
    {
        CString cl;
        cl.Format(_T("%u  %s"), co.id, CString(co.name.c_str()).GetString());
        HTREEITEM hCo = tree->InsertItem(cl, hCountries);
        NodeRef rCo{ NodeType::Country, 0, 0, 0, co.id };
        tree->SetItemData(hCo, Pack(rCo));
        if (preserveSel.type == NodeType::Country && preserveSel.id == co.id) selected = hCo;
    }

    tree->Expand(root, TVE_EXPAND);
    m_suppressEvents = false;
    if (selected) tree->SelectItem(selected);
}

// -------------------------------------------------------------------------
// Selection handling
// -------------------------------------------------------------------------

CatalogEntry* CCatalogEditorDialog::FindEntry(const NodeRef& r)
{
    switch (r.type)
    {
        case NodeType::Kind:
            for (auto& k : m_edit.MutableKinds())
                if (k.id == r.id) return &k;
            break;
        case NodeType::Domain:
            for (auto& d : m_edit.MutableDomains()[r.k])
                if (d.id == r.id) return &d;
            break;
        case NodeType::Category: {
            auto it = m_edit.MutableCategories().find({ r.k, r.d });
            if (it == m_edit.MutableCategories().end()) break;
            for (auto& c : it->second)
                if (c.id == r.id) return &c;
            break;
        }
        case NodeType::Subcategory: {
            auto it = m_edit.MutableSubcategories().find(
                std::make_tuple(r.k, r.d, r.c));
            if (it == m_edit.MutableSubcategories().end()) break;
            for (auto& s : it->second)
                if (s.id == r.id) return &s;
            break;
        }
        case NodeType::Country:
            for (auto& c : m_edit.MutableCountries())
                if (c.id == r.id) return &c;
            break;
        default: break;
    }
    return nullptr;
}

void CCatalogEditorDialog::OnTreeSelChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    if (m_suppressEvents) return;
    LPNMTREEVIEW nm = (LPNMTREEVIEW)pNMHDR;
    if (!nm->itemNew.hItem) return;
    m_sel = Unpack(static_cast<DWORD_PTR>(nm->itemNew.lParam));
    RefreshDetail();
}

void CCatalogEditorDialog::RefreshDetail()
{
    m_suppressEvents = true;
    CatalogEntry* entry = FindEntry(m_sel);

    const TCHAR* typeText = _T("(no selection)");
    switch (m_sel.type)
    {
        case NodeType::Root:        typeText = _T("Catalog (root)"); break;
        case NodeType::Countries:   typeText = _T("Countries (header)"); break;
        case NodeType::Kind:        typeText = _T("Kind"); break;
        case NodeType::Domain:      typeText = _T("Domain"); break;
        case NodeType::Country:     typeText = _T("Country"); break;
        case NodeType::Category:    typeText = _T("Category"); break;
        case NodeType::Subcategory: typeText = _T("Subcategory"); break;
    }
    SetDlgItemText(IDC_LBL_CATALOG_TYPE, typeText);

    if (entry)
    {
        CString idText; idText.Format(_T("%u"), entry->id);
        SetDlgItemText(IDC_EDIT_CATALOG_ID,   idText);
        SetDlgItemText(IDC_EDIT_CATALOG_NAME, CString(entry->name.c_str()));
    }
    else
    {
        SetDlgItemText(IDC_EDIT_CATALOG_ID,   _T(""));
        SetDlgItemText(IDC_EDIT_CATALOG_NAME, _T(""));
    }

    // Attribute grid: populate for Subcategory selections (using its
    // parent Category's schema as the ordering source). For all other
    // selections, clear the grid.
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_ATTRIBUTES);
    if (lv) lv->DeleteAllItems();

    if (m_sel.type == NodeType::Subcategory && entry)
    {
        // Find parent Category for the attribute order.
        const CatalogEntry* parentCat = nullptr;
        auto it = m_edit.MutableCategories().find({ m_sel.k, m_sel.d });
        if (it != m_edit.MutableCategories().end())
            for (auto& ce : it->second)
                if (ce.id == m_sel.c) { parentCat = &ce; break; }
        RefreshAttributeGrid(entry, parentCat);
    }

    SetDlgItemText(IDC_LBL_CATALOG_HINT, _T(""));
    m_suppressEvents = false;
}

void CCatalogEditorDialog::RefreshAttributeGrid(const CatalogEntry* sub,
                                                const CatalogEntry* parentCat)
{
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_ATTRIBUTES);
    if (!lv || !sub) return;
    lv->DeleteAllItems();
    if (parentCat)
    {
        for (size_t i = 0; i < parentCat->attributeNames.size(); ++i)
        {
            const std::string& key = parentCat->attributeNames[i];
            const int row = lv->InsertItem(static_cast<int>(i),
                                            CString(key.c_str()));
            auto v = sub->attributeValues.find(key);
            const std::string val = (v == sub->attributeValues.end()) ? "" : v->second;
            lv->SetItemText(row, 1, CString(val.c_str()));
        }
    }
    else
    {
        // No parent schema; fall back to whatever attributes the subcat
        // happens to have (map order).
        int i = 0;
        for (const auto& kv : sub->attributeValues)
        {
            const int row = lv->InsertItem(i++, CString(kv.first.c_str()));
            lv->SetItemText(row, 1, CString(kv.second.c_str()));
        }
    }
}

void CCatalogEditorDialog::OnAttributeRowChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    LPNMLISTVIEW nm = (LPNMLISTVIEW)pNMHDR;
    if (!(nm->uChanged & LVIF_STATE)) return;
    if (!(nm->uNewState & LVIS_SELECTED)) return;
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_ATTRIBUTES);
    if (!lv) return;
    UpdateHintFor(lv->GetItemText(nm->iItem, 0));
}

void CCatalogEditorDialog::OnAttributeDblClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    if (m_sel.type != NodeType::Subcategory) return;
    LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)pNMHDR;
    if (ia->iItem < 0) return;
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_ATTRIBUTES);
    if (!lv) return;

    const CString attr = lv->GetItemText(ia->iItem, 0);
    const CString oldVal = lv->GetItemText(ia->iItem, 1);

    // Speed (m/s) attributes accept "Mach 1.2" / "M 0.8" — same parser
    // as the asset / motion-path editors. We apply the conversion when
    // the attribute key looks like a speed field (contains "Speed" AND
    // the m/s unit suffix).
    CString promptText;
    promptText.Format(_T("Value for %s:"), attr.GetString());
    CPromptDialog dlg(_T("Edit Attribute"), promptText, oldVal, this);
    if (dlg.DoModal() != IDOK) return;
    CString newVal = dlg.Value();
    newVal.Trim();

    const bool isSpeedField = (attr.Find(_T("Speed")) >= 0) &&
                              (attr.Find(_T("MetersPerSecond")) >= 0);
    if (isSpeedField && !newVal.IsEmpty()) {
        const double mps = ParseSpeedMps(newVal);
        if (!std::isnan(mps)) newVal = FormatDoubleTrim(mps);
    }

    // Commit to the Subcategory's attributeValues map.
    CatalogEntry* sub = FindEntry(m_sel);
    if (!sub) return;
    sub->attributeValues[Narrow(attr)] = Narrow(newVal);
    lv->SetItemText(ia->iItem, 1, newVal);
    UpdateHintFor(attr);
}

// -------------------------------------------------------------------------
// Name editing (commit on every change to the in-memory copy)
// -------------------------------------------------------------------------

void CCatalogEditorDialog::OnNameChanged()
{
    if (m_suppressEvents) return;
    CatalogEntry* entry = FindEntry(m_sel);
    if (!entry) return;
    CString t; GetDlgItemText(IDC_EDIT_CATALOG_NAME, t);
    entry->name = Narrow(t);

    // Refresh the tree label for this node.
    CTreeCtrl* tree = (CTreeCtrl*)GetDlgItem(IDC_TREE_CATALOG);
    if (!tree) return;
    HTREEITEM hit = tree->GetSelectedItem();
    if (!hit) return;
    CString lbl;
    switch (m_sel.type) {
        case NodeType::Kind:        lbl.Format(_T("Kind.%u  %s"),     entry->id, t.GetString()); break;
        case NodeType::Domain:      lbl.Format(_T("Domain.%u.%u  %s"),    m_sel.k, entry->id, t.GetString()); break;
        case NodeType::Category:    lbl.Format(_T("Category.%u.%u.%u  %s"), m_sel.k, m_sel.d, entry->id, t.GetString()); break;
        case NodeType::Subcategory: lbl.Format(_T("Subcat.%u.%u.%u.%u  %s"), m_sel.k, m_sel.d, m_sel.c, entry->id, t.GetString()); break;
        case NodeType::Country:     lbl.Format(_T("%u  %s"), entry->id, t.GetString()); break;
        default: return;
    }
    tree->SetItemText(hit, lbl);
}

// -------------------------------------------------------------------------
// Add node — delegates to CCatalogAddDialog
// -------------------------------------------------------------------------

void CCatalogEditorDialog::OnAddNode()
{
    CCatalogAddDialog dlg(this);
    // Wire up live data for the secondary dialog.
    dlg.SetCatalog(&m_edit);
    dlg.SetParentContext(static_cast<int>(m_sel.type),
                         m_sel.k, m_sel.d, m_sel.c, m_sel.id);
    if (dlg.DoModal() != IDOK) return;

    // The secondary dialog has inserted the new node into m_edit directly
    // (see CCatalogAddDialog::OnOK). Rebuild the tree to reflect it.
    const auto a = dlg.NewlyAdded();
    NodeRef justAdded{};
    justAdded.type = static_cast<NodeType>(a.type);
    justAdded.k    = a.k;
    justAdded.d    = a.d;
    justAdded.c    = a.c;
    justAdded.id   = a.id;
    RebuildTree(justAdded);
}

// -------------------------------------------------------------------------
// Delete node
// -------------------------------------------------------------------------

void CCatalogEditorDialog::OnDeleteNode()
{
    if (m_sel.type == NodeType::Root || m_sel.type == NodeType::Countries)
    {
        AfxMessageBox(_T("Pick a Kind / Domain / Category / Subcategory / Country to delete."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    CString prompt;
    prompt.Format(_T("Delete this node and all descendants?\n\nThis cannot be undone (until you click Cancel on the dialog)."));
    if (AfxMessageBox(prompt, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

    switch (m_sel.type)
    {
        case NodeType::Kind:
        {
            auto& v = m_edit.MutableKinds();
            v.erase(std::remove_if(v.begin(), v.end(),
                [this](const CatalogEntry& e){ return e.id == m_sel.id; }), v.end());
            // Cascade: drop all children of this kind.
            m_edit.MutableDomains().erase(static_cast<uint8_t>(m_sel.id));
            for (auto it = m_edit.MutableCategories().begin(); it != m_edit.MutableCategories().end(); )
                if (it->first.first == m_sel.id) it = m_edit.MutableCategories().erase(it); else ++it;
            for (auto it = m_edit.MutableSubcategories().begin(); it != m_edit.MutableSubcategories().end(); )
                if (std::get<0>(it->first) == m_sel.id) it = m_edit.MutableSubcategories().erase(it); else ++it;
            break;
        }
        case NodeType::Domain:
        {
            auto& v = m_edit.MutableDomains()[m_sel.k];
            v.erase(std::remove_if(v.begin(), v.end(),
                [this](const CatalogEntry& e){ return e.id == m_sel.id; }), v.end());
            for (auto it = m_edit.MutableCategories().begin(); it != m_edit.MutableCategories().end(); )
                if (it->first.first == m_sel.k && it->first.second == m_sel.id)
                    it = m_edit.MutableCategories().erase(it); else ++it;
            for (auto it = m_edit.MutableSubcategories().begin(); it != m_edit.MutableSubcategories().end(); )
                if (std::get<0>(it->first) == m_sel.k && std::get<1>(it->first) == m_sel.id)
                    it = m_edit.MutableSubcategories().erase(it); else ++it;
            break;
        }
        case NodeType::Category:
        {
            auto& v = m_edit.MutableCategories()[{ m_sel.k, m_sel.d }];
            v.erase(std::remove_if(v.begin(), v.end(),
                [this](const CatalogEntry& e){ return e.id == m_sel.id; }), v.end());
            m_edit.MutableSubcategories().erase(
                std::make_tuple(m_sel.k, m_sel.d, static_cast<uint8_t>(m_sel.id)));
            break;
        }
        case NodeType::Subcategory:
        {
            auto& v = m_edit.MutableSubcategories()[{ m_sel.k, m_sel.d, m_sel.c }];
            v.erase(std::remove_if(v.begin(), v.end(),
                [this](const CatalogEntry& e){ return e.id == m_sel.id; }), v.end());
            break;
        }
        case NodeType::Country:
        {
            auto& v = m_edit.MutableCountries();
            v.erase(std::remove_if(v.begin(), v.end(),
                [this](const CatalogEntry& e){ return e.id == m_sel.id; }), v.end());
            break;
        }
        default: break;
    }

    NodeRef none{};
    RebuildTree(none);
}

// -------------------------------------------------------------------------
// Add Attribute (sibling-propagating)
// -------------------------------------------------------------------------

void CCatalogEditorDialog::OnAddAttribute()
{
    // Resolve the parent Category for the current selection. We allow
    // adding attributes when a Category or any of its Subcategories is
    // selected.
    uint8_t k = m_sel.k, d = m_sel.d, c = m_sel.c;
    if (m_sel.type == NodeType::Category) c = static_cast<uint8_t>(m_sel.id);
    else if (m_sel.type != NodeType::Subcategory) {
        AfxMessageBox(_T("Select a Category or one of its Subcategories first."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto catIt = m_edit.MutableCategories().find({ k, d });
    if (catIt == m_edit.MutableCategories().end()) return;
    CatalogEntry* cat = nullptr;
    for (auto& ce : catIt->second) if (ce.id == c) { cat = &ce; break; }
    if (!cat) return;

    CPromptDialog prompt(_T("Add Attribute"),
                         _T("New attribute name (applies to all sibling Subcategories):"),
                         _T(""), this);
    if (prompt.DoModal() != IDOK) return;
    CString name = prompt.Value();
    name.Trim();
    if (name.IsEmpty()) return;
    const std::string n = Narrow(name);
    if (n == "Name") {
        AfxMessageBox(_T("\"Name\" is reserved."), MB_OK | MB_ICONWARNING);
        return;
    }
    for (const auto& existing : cat->attributeNames)
        if (existing == n) {
            AfxMessageBox(_T("Attribute already exists on this Category."),
                          MB_OK | MB_ICONINFORMATION);
            return;
        }

    cat->attributeNames.push_back(n);
    // Propagate empty value to all sibling subcategories.
    auto subIt = m_edit.MutableSubcategories().find({ k, d, c });
    if (subIt != m_edit.MutableSubcategories().end())
        for (auto& sub : subIt->second)
            if (sub.attributeValues.find(n) == sub.attributeValues.end())
                sub.attributeValues[n] = "";

    RefreshDetail();
}

// -------------------------------------------------------------------------
// Delete Attribute (sibling-propagating)
// -------------------------------------------------------------------------

void CCatalogEditorDialog::OnDeleteAttribute()
{
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_ATTRIBUTES);
    if (!lv) return;
    const int row = lv->GetNextItem(-1, LVNI_SELECTED);
    if (row < 0) {
        AfxMessageBox(_T("Select an attribute row first."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const CString attrName = lv->GetItemText(row, 0);

    // Same parent-Category resolution as OnAddAttribute.
    uint8_t k = m_sel.k, d = m_sel.d, c = m_sel.c;
    if (m_sel.type == NodeType::Category) c = static_cast<uint8_t>(m_sel.id);
    else if (m_sel.type != NodeType::Subcategory) return;

    CString msg;
    msg.Format(_T("Delete attribute \"%s\" from this Category?\n\nIt will be removed from every Subcategory under this Category."),
               attrName.GetString());
    if (AfxMessageBox(msg, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

    const std::string n = Narrow(attrName);
    auto catIt = m_edit.MutableCategories().find({ k, d });
    if (catIt != m_edit.MutableCategories().end())
        for (auto& ce : catIt->second)
            if (ce.id == c) {
                auto& v = ce.attributeNames;
                v.erase(std::remove(v.begin(), v.end(), n), v.end());
            }
    auto subIt = m_edit.MutableSubcategories().find({ k, d, c });
    if (subIt != m_edit.MutableSubcategories().end())
        for (auto& sub : subIt->second)
            sub.attributeValues.erase(n);

    RefreshDetail();
}

// -------------------------------------------------------------------------
// Save / Cancel
// -------------------------------------------------------------------------

void CCatalogEditorDialog::OnSave()
{
    if (!m_live) return;
    if (!m_edit.SaveToIni(m_path)) {
        AfxMessageBox(_T("Failed to save EntityTypeCatalog.ini. See error.log."),
                      MB_OK | MB_ICONERROR);
        return;
    }
    *m_live = m_edit;
    sprintf_s(szError, sizeof(szError),
              "Catalog saved to %S", m_path.c_str());
    LOG(szError);
}

void CCatalogEditorDialog::OnSaveClose()
{
    OnSave();
    EndDialog(IDOK);
}

void CCatalogEditorDialog::OnCancel()
{
    // Discard m_edit (deep copy never written back).
    CDialogEx::OnCancel();
}
