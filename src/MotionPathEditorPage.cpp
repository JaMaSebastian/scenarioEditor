#include "pch.h"
#include "MotionPathEditorPage.h"
#include "Scenario.h"
#include "ScenarioWorker.h"   // WM_APP_MARK_DIRTY

#include <cstdlib>

namespace
{
    void NotifyDirty(CWnd* page)
    {
        if (!page) return;
        if (CWnd* top = page->GetTopLevelParent())
            top->PostMessage(WM_APP_MARK_DIRTY, 0, 0);
    }
}

namespace
{
    const FFieldHelp kFields[] = {
        { IDC_COMBO_MOTION_ENTITY,       _T("Select which entity's motion path you are editing."), true },

        { IDC_BTN_ADD_SEGMENT,           _T("Add a new motion segment after the selected row."), false },
        { IDC_BTN_DELETE_SEGMENT,        _T("Delete the selected motion segment."), false },
        { IDC_BTN_DUPLICATE_SEGMENT,     _T("Duplicate the selected motion segment."), false },
        { IDC_BTN_SEGMENT_MOVE_UP,       _T("Move the selected segment earlier in the timeline."), false },
        { IDC_BTN_SEGMENT_MOVE_DOWN,     _T("Move the selected segment later in the timeline."), false },
        { IDC_BTN_VALIDATE_SEGMENT,      _T("Validate only the selected motion segment."), false },

        { IDC_CHK_SEGMENT_ENABLED,       _T("Include this segment when generating the entity's motion."), false },
        { IDC_EDIT_SEGMENT_START_TIME,   _T("Scenario time (s) when this segment begins."), true },
        { IDC_EDIT_SEGMENT_END_TIME,     _T("Scenario time (s) when this segment ends."), true },
        { IDC_COMBO_MOTION_TYPE,         _T("Segment type: Stationary, Stop/Hold, Line, or Ellipse."), true },
        { IDC_COMBO_SEGMENT_COORD_MODE,  _T("Coordinate system used by this segment's geometry fields."), true },
        { IDC_EDIT_SEGMENT_DESCRIPTION,  _T("Optional description for this motion segment."), false },

        { IDC_BTN_IMPORT_CSV,            _T("Import waypoints from a CSV file."), false },
        { IDC_BTN_EXPORT_CSV,            _T("Export the current waypoints to a CSV file."), false },
    };

    double ReadDoubleText(const CWnd& wnd, UINT id, double fallback)
    {
        CString text;
        wnd.GetDlgItemText(id, text);
        if (text.IsEmpty()) return fallback;
        CT2A ascii(text);
        char* end = nullptr;
        const double v = std::strtod(ascii.m_psz, &end);
        return (end == ascii.m_psz) ? fallback : v;
    }

    void WriteDoubleText(CWnd& wnd, UINT id, double v)
    {
        CString buf; buf.Format(_T("%.6f"), v); wnd.SetDlgItemText(id, buf);
    }
}

BEGIN_MESSAGE_MAP(CMotionPathEditorPage, CHelpAwarePage)
    ON_CBN_SELCHANGE(IDC_COMBO_MOTION_ENTITY, &CMotionPathEditorPage::OnEntityChanged)
    ON_CBN_SELCHANGE(IDC_COMBO_MOTION_TYPE,   &CMotionPathEditorPage::OnSegmentTypeChanged)
    ON_BN_CLICKED   (IDC_BTN_ADD_SEGMENT,      &CMotionPathEditorPage::OnAddSegment)
    ON_BN_CLICKED   (IDC_BTN_DELETE_SEGMENT,   &CMotionPathEditorPage::OnDeleteSegment)
    ON_BN_CLICKED   (IDC_BTN_DUPLICATE_SEGMENT,&CMotionPathEditorPage::OnDuplicateSegment)
    ON_BN_CLICKED   (IDC_BTN_SEGMENT_MOVE_UP,  &CMotionPathEditorPage::OnMoveSegmentUp)
    ON_BN_CLICKED   (IDC_BTN_SEGMENT_MOVE_DOWN,&CMotionPathEditorPage::OnMoveSegmentDown)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST_MOTION_SEGMENTS,
                                               &CMotionPathEditorPage::OnSegmentListSelChanged)
END_MESSAGE_MAP()

void CMotionPathEditorPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

BOOL CMotionPathEditorPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MOTION_TYPE))
    {
        cb->AddString(_T("Stationary"));
        cb->AddString(_T("StopHold"));
        cb->AddString(_T("Line"));
        cb->AddString(_T("Ellipse"));
        cb->SetCurSel(0);
    }

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_SEGMENT_COORD_MODE))
    {
        cb->AddString(_T("Lat/Lon/Alt"));
        cb->AddString(_T("Local X/Y/Z"));
        cb->AddString(_T("ECEF"));
        cb->SetCurSel(0);
    }

    if (CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_MOTION_SEGMENTS))
    {
        lv->SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        struct Col { const TCHAR* name; int width; };
        const Col cols[] = {
            { _T("#"),           24  },
            { _T("Enabled"),     50  },
            { _T("Start"),       60  },
            { _T("End"),         60  },
            { _T("Type"),        80  },
            { _T("Description"), 200 },
        };
        for (int i = 0; i < (int)(sizeof(cols)/sizeof(cols[0])); ++i)
            lv->InsertColumn(i, cols[i].name, LVCFMT_LEFT, cols[i].width);
    }

    return TRUE;
}

Entity* CMotionPathEditorPage::ActiveEntity()
{
    if (!m_scenario) return nullptr;
    if (m_entityIdx < 0 || m_entityIdx >= (int)m_scenario->entities.size()) return nullptr;
    return &m_scenario->entities[m_entityIdx];
}

MotionSegment* CMotionPathEditorPage::ActiveSegment()
{
    Entity* e = ActiveEntity();
    if (!e) return nullptr;
    if (m_segmentIdx < 0 || m_segmentIdx >= (int)e->motionSegments.size()) return nullptr;
    return &e->motionSegments[m_segmentIdx];
}

void CMotionPathEditorPage::Refresh()
{
    if (!::IsWindow(GetSafeHwnd())) return;
    RefreshEntityCombo();
    RefreshSegmentList();
    LoadActiveSegment();
}

void CMotionPathEditorPage::RefreshEntityCombo()
{
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MOTION_ENTITY);
    if (!cb || !m_scenario) return;
    m_suppressEvents = true;
    cb->ResetContent();
    for (size_t i = 0; i < m_scenario->entities.size(); ++i) {
        const Entity& e = m_scenario->entities[i];
        CString label;
        label.Format(_T("%S (id=%u)"),
                     e.name.empty() ? "(unnamed)" : e.name.c_str(),
                     static_cast<unsigned>(e.entityId));
        const int idx = cb->AddString(label);
        cb->SetItemData(idx, static_cast<DWORD_PTR>(i));
    }
    if (m_entityIdx < 0 || m_entityIdx >= (int)m_scenario->entities.size())
        m_entityIdx = 0;
    cb->SetCurSel(m_entityIdx);
    m_suppressEvents = false;
}

void CMotionPathEditorPage::RefreshSegmentList()
{
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_MOTION_SEGMENTS);
    if (!lv) return;
    m_suppressEvents = true;
    lv->DeleteAllItems();

    Entity* e = ActiveEntity();
    if (e)
    {
        for (size_t i = 0; i < e->motionSegments.size(); ++i)
        {
            const MotionSegment& s = e->motionSegments[i];
            CString idx; idx.Format(_T("%zu"), i + 1);
            const int row = lv->InsertItem((int)i, idx);
            lv->SetItemText(row, 1, s.enabled ? _T("Yes") : _T("No"));
            CString tmp;
            tmp.Format(_T("%.2f"), s.startSecond); lv->SetItemText(row, 2, tmp);
            tmp.Format(_T("%.2f"), s.endSecond);   lv->SetItemText(row, 3, tmp);
            const TCHAR* tn = _T("Stationary");
            switch (s.type) {
                case MotionType::Stationary: tn = _T("Stationary"); break;
                case MotionType::StopHold:   tn = _T("StopHold");   break;
                case MotionType::Line:       tn = _T("Line");       break;
                case MotionType::Ellipse:    tn = _T("Ellipse");    break;
            }
            lv->SetItemText(row, 4, tn);
            lv->SetItemText(row, 5, CA2T(s.description.c_str()));
        }
        if (m_segmentIdx >= 0 && m_segmentIdx < (int)e->motionSegments.size())
        {
            lv->SetItemState(m_segmentIdx, LVIS_SELECTED | LVIS_FOCUSED,
                             LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    m_suppressEvents = false;
}

void CMotionPathEditorPage::LoadActiveSegment()
{
    if (!::IsWindow(GetSafeHwnd())) return;
    m_suppressEvents = true;

    MotionSegment* s = ActiveSegment();
    const bool have = (s != nullptr);

    CheckDlgButton(IDC_CHK_SEGMENT_ENABLED, have && s->enabled ? BST_CHECKED : BST_UNCHECKED);
    WriteDoubleText(*this, IDC_EDIT_SEGMENT_START_TIME, have ? s->startSecond : 0.0);
    WriteDoubleText(*this, IDC_EDIT_SEGMENT_END_TIME,   have ? s->endSecond   : 0.0);

    int typeIdx = 0;
    if (have) switch (s->type) {
        case MotionType::Stationary: typeIdx = 0; break;
        case MotionType::StopHold:   typeIdx = 1; break;
        case MotionType::Line:       typeIdx = 2; break;
        case MotionType::Ellipse:    typeIdx = 3; break;
    }
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MOTION_TYPE)) cb->SetCurSel(typeIdx);

    int coordIdx = 0;
    if (have) switch (s->coordMode) {
        case CoordMode::LatLonAlt: coordIdx = 0; break;
        case CoordMode::Local:     coordIdx = 1; break;
        case CoordMode::ECEF:      coordIdx = 2; break;
    }
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_SEGMENT_COORD_MODE)) cb->SetCurSel(coordIdx);

    SetDlgItemText(IDC_EDIT_SEGMENT_DESCRIPTION,
                   have ? CString(CA2T(s->description.c_str())) : CString());

    SetTypeSpecificVisibility();
    m_suppressEvents = false;
}

void CMotionPathEditorPage::CommitActiveSegmentFromUi()
{
    MotionSegment* s = ActiveSegment();
    if (!s) return;
    s->enabled     = IsDlgButtonChecked(IDC_CHK_SEGMENT_ENABLED) == BST_CHECKED;
    s->startSecond = ReadDoubleText(*this, IDC_EDIT_SEGMENT_START_TIME, s->startSecond);
    s->endSecond   = ReadDoubleText(*this, IDC_EDIT_SEGMENT_END_TIME,   s->endSecond);

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MOTION_TYPE)) {
        switch (cb->GetCurSel()) {
            case 0: s->type = MotionType::Stationary; break;
            case 1: s->type = MotionType::StopHold;   break;
            case 2: s->type = MotionType::Line;       break;
            case 3: s->type = MotionType::Ellipse;    break;
        }
    }
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_SEGMENT_COORD_MODE)) {
        switch (cb->GetCurSel()) {
            case 0: s->coordMode = CoordMode::LatLonAlt; break;
            case 1: s->coordMode = CoordMode::Local;     break;
            case 2: s->coordMode = CoordMode::ECEF;      break;
        }
    }
    CString text;
    GetDlgItemText(IDC_EDIT_SEGMENT_DESCRIPTION, text);
    CT2A ascii(text);
    s->description = ascii.m_psz;
}

void CMotionPathEditorPage::SetTypeSpecificVisibility()
{
    // The .rc template has a placeholder groupbox for per-type controls;
    // until per-type subfields are authored, show/hide is a no-op. The
    // Type combo + Start/End/CoordMode/Description still suffice to
    // round-trip Stationary/StopHold/Line metadata. Ellipse-specific
    // numeric fields are still authored via the INI for this slice; the
    // UI subfields land in a follow-up.
}

void CMotionPathEditorPage::OnEntityChanged()
{
    if (m_suppressEvents) return;
    CommitActiveSegmentFromUi();
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MOTION_ENTITY))
        m_entityIdx = static_cast<int>(cb->GetItemData(cb->GetCurSel()));
    m_segmentIdx = -1;
    RefreshSegmentList();
    LoadActiveSegment();
}

void CMotionPathEditorPage::OnSegmentTypeChanged()
{
    if (m_suppressEvents) return;
    CommitActiveSegmentFromUi();
    RefreshSegmentList();
    SetTypeSpecificVisibility();
}

void CMotionPathEditorPage::OnSegmentListSelChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    if (m_suppressEvents) return;
    LPNMLISTVIEW nm = (LPNMLISTVIEW)pNMHDR;
    if (!(nm->uChanged & LVIF_STATE)) return;
    if (!(nm->uNewState & LVIS_SELECTED)) return;
    if (m_segmentIdx == nm->iItem) return;
    CommitActiveSegmentFromUi();
    m_segmentIdx = nm->iItem;
    LoadActiveSegment();
}

void CMotionPathEditorPage::OnAddSegment()
{
    Entity* e = ActiveEntity();
    if (!e) return;
    CommitActiveSegmentFromUi();
    MotionSegment s;
    s.type = MotionType::Stationary;
    s.startSecond = e->motionSegments.empty() ? 0.0
                                              : e->motionSegments.back().endSecond;
    s.endSecond   = s.startSecond + 30.0;
    e->motionSegments.push_back(std::move(s));
    m_segmentIdx  = static_cast<int>(e->motionSegments.size()) - 1;
    RefreshSegmentList();
    LoadActiveSegment();
    NotifyDirty(this);
}

void CMotionPathEditorPage::OnDeleteSegment()
{
    Entity* e = ActiveEntity();
    if (!e) return;
    if (m_segmentIdx < 0 || m_segmentIdx >= (int)e->motionSegments.size()) return;
    e->motionSegments.erase(e->motionSegments.begin() + m_segmentIdx);
    if (m_segmentIdx >= (int)e->motionSegments.size())
        m_segmentIdx = (int)e->motionSegments.size() - 1;
    RefreshSegmentList();
    LoadActiveSegment();
    NotifyDirty(this);
}

void CMotionPathEditorPage::OnDuplicateSegment()
{
    Entity* e = ActiveEntity();
    if (!e) return;
    if (m_segmentIdx < 0 || m_segmentIdx >= (int)e->motionSegments.size()) return;
    CommitActiveSegmentFromUi();
    MotionSegment copy = e->motionSegments[m_segmentIdx];
    const double width = copy.endSecond - copy.startSecond;
    copy.startSecond  = copy.endSecond;
    copy.endSecond    = copy.startSecond + (width > 0 ? width : 30.0);
    e->motionSegments.insert(e->motionSegments.begin() + m_segmentIdx + 1, std::move(copy));
    ++m_segmentIdx;
    RefreshSegmentList();
    LoadActiveSegment();
    NotifyDirty(this);
}

void CMotionPathEditorPage::OnMoveSegmentUp()
{
    Entity* e = ActiveEntity();
    if (!e || m_segmentIdx <= 0) return;
    CommitActiveSegmentFromUi();
    std::swap(e->motionSegments[m_segmentIdx], e->motionSegments[m_segmentIdx - 1]);
    --m_segmentIdx;
    RefreshSegmentList();
    LoadActiveSegment();
    NotifyDirty(this);
}

void CMotionPathEditorPage::OnMoveSegmentDown()
{
    Entity* e = ActiveEntity();
    if (!e) return;
    if (m_segmentIdx < 0 || m_segmentIdx >= (int)e->motionSegments.size() - 1) return;
    CommitActiveSegmentFromUi();
    std::swap(e->motionSegments[m_segmentIdx], e->motionSegments[m_segmentIdx + 1]);
    ++m_segmentIdx;
    RefreshSegmentList();
    LoadActiveSegment();
    NotifyDirty(this);
}
