#include "pch.h"
#include "CatalogAddDialog.h"

namespace
{
    // Mirrors CCatalogEditorDialog::NodeType (avoid the header dep).
    enum class TreeNodeType : uint8_t
    { Root, Countries, Kind, Domain, Country, Category, Subcategory };

    std::string Narrow(const CString& s)
    {
        if (s.IsEmpty()) return {};
        CT2A a(s);
        return a.m_psz ? std::string(a.m_psz) : std::string();
    }

    uint16_t MaxIdPlusOne(const std::vector<CatalogEntry>& v)
    {
        uint16_t mx = 0;
        for (const auto& e : v) if (e.id > mx) mx = e.id;
        return static_cast<uint16_t>(mx + 1);
    }
}

BEGIN_MESSAGE_MAP(CCatalogAddDialog, CDialogEx)
    ON_CBN_SELCHANGE(IDC_COMBO_ADD_TYPE, &CCatalogAddDialog::OnTypeChanged)
END_MESSAGE_MAP()

int CCatalogAddDialog::DefaultTypeFromContext() const
{
    switch (static_cast<TreeNodeType>(m_parentType))
    {
        case TreeNodeType::Kind:        return TYPE_DOMAIN;
        case TreeNodeType::Domain:      return TYPE_CATEGORY;
        case TreeNodeType::Category:    return TYPE_SUBCATEGORY;
        case TreeNodeType::Subcategory: return TYPE_SUBCATEGORY;
        case TreeNodeType::Country:     return TYPE_COUNTRY;
        case TreeNodeType::Countries:   return TYPE_COUNTRY;
        default:                        return TYPE_KIND;
    }
}

void CCatalogAddDialog::ResolveParent(int addType, uint8_t& outK,
                                      uint8_t& outD, uint8_t& outC) const
{
    outK = outD = outC = 0;
    // Walk up from the selection. m_k/m_d/m_c plus m_selId combined
    // identify the selected node; for child-of-X we want the X's
    // address.
    const auto sel = static_cast<TreeNodeType>(m_parentType);
    switch (addType)
    {
        case TYPE_DOMAIN:
            if (sel == TreeNodeType::Kind)
                outK = static_cast<uint8_t>(m_selId);
            else if (sel == TreeNodeType::Domain || sel == TreeNodeType::Category ||
                     sel == TreeNodeType::Subcategory)
                outK = m_k;
            break;
        case TYPE_CATEGORY:
            if (sel == TreeNodeType::Domain) {
                outK = m_k;
                outD = static_cast<uint8_t>(m_selId);
            }
            else if (sel == TreeNodeType::Category || sel == TreeNodeType::Subcategory) {
                outK = m_k; outD = m_d;
            }
            break;
        case TYPE_SUBCATEGORY:
            if (sel == TreeNodeType::Category) {
                outK = m_k; outD = m_d;
                outC = static_cast<uint8_t>(m_selId);
            }
            else if (sel == TreeNodeType::Subcategory) {
                outK = m_k; outD = m_d; outC = m_c;
            }
            break;
        default:
            break;
    }
}

uint16_t CCatalogAddDialog::AutoIdFor(int addType) const
{
    if (!m_catalog) return 1;
    uint8_t k = 0, d = 0, c = 0;
    ResolveParent(addType, k, d, c);
    switch (addType)
    {
        case TYPE_KIND:
            return MaxIdPlusOne(m_catalog->MutableKinds());
        case TYPE_DOMAIN:
            return MaxIdPlusOne(m_catalog->MutableDomains()[k]);
        case TYPE_CATEGORY:
            return MaxIdPlusOne(m_catalog->MutableCategories()[{ k, d }]);
        case TYPE_SUBCATEGORY:
            return MaxIdPlusOne(m_catalog->MutableSubcategories()[{ k, d, c }]);
        case TYPE_COUNTRY:
            return MaxIdPlusOne(m_catalog->MutableCountries());
    }
    return 1;
}

CString CCatalogAddDialog::BreadcrumbFor(int addType) const
{
    if (!m_catalog) return _T("");
    uint8_t k = 0, d = 0, c = 0;
    ResolveParent(addType, k, d, c);

    CString out;
    auto findName = [](const std::vector<CatalogEntry>& v, uint16_t id) -> CString {
        for (const auto& e : v) if (e.id == id) return CString(e.name.c_str());
        return _T("");
    };

    switch (addType)
    {
        case TYPE_KIND:
            return _T("(top-level Kind)");
        case TYPE_COUNTRY:
            return _T("(top-level Country)");
        case TYPE_DOMAIN: {
            if (k == 0) { out = _T("(no parent Kind selected — pick a Kind in the tree first)"); break; }
            CString kn = findName(m_catalog->MutableKinds(), k);
            out.Format(_T("Under: Kind.%u  %s"), k, kn.GetString());
            break;
        }
        case TYPE_CATEGORY: {
            if (k == 0 || d == 0) { out = _T("(no parent Domain selected)"); break; }
            CString kn = findName(m_catalog->MutableKinds(), k);
            CString dn = findName(m_catalog->MutableDomains()[k], d);
            out.Format(_T("Under: %s > %s"), kn.GetString(), dn.GetString());
            break;
        }
        case TYPE_SUBCATEGORY: {
            if (k == 0 || d == 0 || c == 0) { out = _T("(no parent Category selected)"); break; }
            CString kn = findName(m_catalog->MutableKinds(), k);
            CString dn = findName(m_catalog->MutableDomains()[k], d);
            CString cn = findName(m_catalog->MutableCategories()[{ k, d }], c);
            out.Format(_T("Under: %s > %s > %s"),
                       kn.GetString(), dn.GetString(), cn.GetString());
            break;
        }
    }
    return out;
}

BOOL CCatalogAddDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ADD_TYPE))
    {
        cb->AddString(_T("Kind"));
        cb->AddString(_T("Domain"));
        cb->AddString(_T("Category"));
        cb->AddString(_T("Subcategory"));
        cb->AddString(_T("Country"));
        cb->SetCurSel(DefaultTypeFromContext());
    }

    // Seed ID + breadcrumb from the current combo selection.
    OnTypeChanged();
    SetDlgItemText(IDC_EDIT_ADD_NAME, _T(""));
    // Focus on Name so common path is type-and-Enter.
    if (CEdit* e = (CEdit*)GetDlgItem(IDC_EDIT_ADD_NAME)) {
        e->SetFocus();
        return FALSE;
    }
    return TRUE;
}

void CCatalogAddDialog::OnTypeChanged()
{
    int idx = 0;
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ADD_TYPE))
        idx = cb->GetCurSel();
    if (idx < 0) idx = 0;

    CString id; id.Format(_T("%u"), AutoIdFor(idx));
    SetDlgItemText(IDC_EDIT_ADD_ID, id);
    SetDlgItemText(IDC_LBL_ADD_BREADCRUMB, BreadcrumbFor(idx));
}

void CCatalogAddDialog::OnOK()
{
    if (!m_catalog) { CDialogEx::OnOK(); return; }

    int idx = 0;
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_ADD_TYPE))
        idx = cb->GetCurSel();
    if (idx < 0) idx = 0;

    CString idText, nameText;
    GetDlgItemText(IDC_EDIT_ADD_ID,   idText);
    GetDlgItemText(IDC_EDIT_ADD_NAME, nameText);
    nameText.Trim();

    if (nameText.IsEmpty()) {
        AfxMessageBox(_T("Please enter a name."), MB_OK | MB_ICONINFORMATION);
        if (CWnd* e = GetDlgItem(IDC_EDIT_ADD_NAME)) e->SetFocus();
        return;
    }
    // Parse + validate ID.
    long idVal = _ttol(idText);
    if (idVal <= 0 || idVal > 65535) {
        AfxMessageBox(_T("ID must be between 1 and 65535."),
                      MB_OK | MB_ICONWARNING);
        if (CWnd* e = GetDlgItem(IDC_EDIT_ADD_ID)) e->SetFocus();
        return;
    }
    const uint16_t id = static_cast<uint16_t>(idVal);
    const std::string name = Narrow(nameText);

    // Resolve parent + check sibling uniqueness, then insert.
    uint8_t k = 0, d = 0, c = 0;
    ResolveParent(idx, k, d, c);

    auto idExists = [](const std::vector<CatalogEntry>& v, uint16_t id) {
        for (const auto& e : v) if (e.id == id) return true;
        return false;
    };

    switch (idx)
    {
        case TYPE_KIND: {
            if (idExists(m_catalog->MutableKinds(), id)) {
                AfxMessageBox(_T("A Kind with that ID already exists."), MB_OK | MB_ICONWARNING);
                return;
            }
            CatalogEntry e; e.id = id; e.name = name;
            m_catalog->MutableKinds().push_back(std::move(e));
            m_added = { static_cast<int>(TreeNodeType::Kind), k, d, c, id };
            break;
        }
        case TYPE_DOMAIN: {
            if (k == 0) {
                AfxMessageBox(_T("Select a Kind in the tree first."),
                              MB_OK | MB_ICONWARNING);
                return;
            }
            auto& v = m_catalog->MutableDomains()[k];
            if (idExists(v, id)) {
                AfxMessageBox(_T("A Domain with that ID already exists under this Kind."),
                              MB_OK | MB_ICONWARNING);
                return;
            }
            CatalogEntry e; e.id = id; e.name = name;
            v.push_back(std::move(e));
            m_added = { static_cast<int>(TreeNodeType::Domain), k, d, c, id };
            break;
        }
        case TYPE_CATEGORY: {
            if (k == 0 || d == 0) {
                AfxMessageBox(_T("Select a Domain in the tree first."),
                              MB_OK | MB_ICONWARNING);
                return;
            }
            auto& v = m_catalog->MutableCategories()[{ k, d }];
            if (idExists(v, id)) {
                AfxMessageBox(_T("A Category with that ID already exists under this Domain."),
                              MB_OK | MB_ICONWARNING);
                return;
            }
            CatalogEntry e; e.id = id; e.name = name;
            v.push_back(std::move(e));
            m_added = { static_cast<int>(TreeNodeType::Category), k, d, c, id };
            break;
        }
        case TYPE_SUBCATEGORY: {
            if (k == 0 || d == 0 || c == 0) {
                AfxMessageBox(_T("Select a Category in the tree first."),
                              MB_OK | MB_ICONWARNING);
                return;
            }
            auto& v = m_catalog->MutableSubcategories()[{ k, d, c }];
            if (idExists(v, id)) {
                AfxMessageBox(_T("A Subcategory with that ID already exists under this Category."),
                              MB_OK | MB_ICONWARNING);
                return;
            }
            CatalogEntry e; e.id = id; e.name = name;
            // Seed empty attribute values for every attribute in the
            // parent Category's schema, so the grid populates immediately.
            auto catIt = m_catalog->MutableCategories().find({ k, d });
            if (catIt != m_catalog->MutableCategories().end())
                for (const auto& ce : catIt->second)
                    if (ce.id == c) {
                        for (const auto& an : ce.attributeNames)
                            e.attributeValues[an] = "";
                        break;
                    }
            v.push_back(std::move(e));
            m_added = { static_cast<int>(TreeNodeType::Subcategory), k, d, c, id };
            break;
        }
        case TYPE_COUNTRY: {
            if (idExists(m_catalog->MutableCountries(), id)) {
                AfxMessageBox(_T("A Country with that ID already exists."),
                              MB_OK | MB_ICONWARNING);
                return;
            }
            CatalogEntry e; e.id = id; e.name = name;
            m_catalog->MutableCountries().push_back(std::move(e));
            m_added = { static_cast<int>(TreeNodeType::Country), k, d, c, id };
            break;
        }
    }

    CDialogEx::OnOK();
}
