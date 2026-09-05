//=============================================================================
//  MotionPathEditorPage.cpp
//-----------------------------------------------------------------------------
//  Implements CMotionPathEditorPage: the Motion Path Editor tab. Manages the
//  entity selector, the segment timeline list, and the Line/Ellipse geometry
//  fields, converting between Lat-Lon-Alt / Local / ECEF frames and coupling
//  Line speed to segment End Time. Edits the shared Scenario in place.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "MotionPathEditorPage.h"
#include "Scenario.h"
#include "ScenarioEditor.h"   // theApp
#include "ScenarioWorker.h"   // WM_APP_MARK_DIRTY
#include "Validator.h"
#include "CoordTransforms.h"
#include "MotionSampler.h"
#include "SpeedSeed.h"        // SeedCruiseSpeedMps — catalog cruise / 10 m/s default
#include "FormatUtil.h"

#include <cmath>
#include <cstdlib>

namespace
{
    //
    // NotifyDirty — relay WM_APP_MARK_DIRTY up to the top-level dialog so it
    // flags the scenario as modified. See the inline note on SendMessage.
    //
    void NotifyDirty(CWnd* page)
    {
        if (!page) return;
        // SendMessage (not PostMessage) so the top-level dialog's
        // MarkDirty fires SYNCHRONOUSLY while m_suppressDirty is still
        // set during RefreshUiFromScenario. PostMessage would queue the
        // message and process it AFTER the suppress flag was cleared,
        // flipping the dialog to "modified" on every load.
        if (CWnd* top = page->GetTopLevelParent())
            top->SendMessage(WM_APP_MARK_DIRTY, 0, 0);
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

    //
    // ReadDoubleText — parse the numeric text of dialog item `id`, returning
    // `fallback` when the field is empty or not a valid number.
    //
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

    //
    // WriteDoubleText — set dialog item `id` to a trimmed string form of `v`.
    //
    void WriteDoubleText(CWnd& wnd, UINT id, double v)
    {
        wnd.SetDlgItemText(id, FormatDoubleTrim(v));
    }

    // Approximate straight-line distance (m) between the segment's start
    // and end positions, used to convert authored Line Speed into a
    // matching endSecond. Honors coordMode; for ECEF / Local we use the
    // raw deltas; for LatLonAlt we apply the standard mid-latitude
    // E/N scaling. Returns 0 if endpoints coincide.
    double ApproxLineDistanceMeters(const MotionSegment& s)
    {
        double dE = 0.0, dN = 0.0, dU = 0.0;
        switch (s.coordMode) {
            case CoordMode::Local:
                dE = s.endLocalX - s.startLocalX;
                dN = s.endLocalY - s.startLocalY;
                dU = s.endLocalZ - s.startLocalZ;
                break;
            case CoordMode::ECEF:
                dE = s.endEcefX - s.startEcefX;
                dN = s.endEcefY - s.startEcefY;
                dU = s.endEcefZ - s.startEcefZ;
                break;
            case CoordMode::LatLonAlt:
            default: {
                constexpr double kPi = 3.14159265358979323846;
                const double midLatRad = 0.5 * (s.startLat + s.endLat) * (kPi / 180.0);
                dE = (s.endLon - s.startLon) * 111320.0 * std::cos(midLatRad);
                dN = (s.endLat - s.startLat) * 111320.0;
                dU = s.endAlt - s.startAlt;
                break;
            }
        }
        return std::sqrt(dE * dE + dN * dN + dU * dU);
    }

}

BEGIN_MESSAGE_MAP(CMotionPathEditorPage, CHelpAwarePage)
    ON_WM_SHOWWINDOW()
    ON_CBN_SELCHANGE(IDC_COMBO_MOTION_ENTITY,       &CMotionPathEditorPage::OnEntityChanged)
    ON_CBN_SELCHANGE(IDC_COMBO_MOTION_TYPE,         &CMotionPathEditorPage::OnSegmentTypeChanged)
    ON_CBN_SELCHANGE(IDC_COMBO_SEGMENT_COORD_MODE,  &CMotionPathEditorPage::OnSegmentCoordModeChanged)
    ON_EN_CHANGE    (IDC_EDIT_SEGMENT_START_TIME,   &CMotionPathEditorPage::OnSegmentTimingChanged)
    ON_EN_CHANGE    (IDC_EDIT_SEGMENT_END_TIME,     &CMotionPathEditorPage::OnSegmentTimingChanged)
    ON_EN_KILLFOCUS (IDC_EDIT_SPEED,       &CMotionPathEditorPage::OnEllipseSpeedKillFocus)
    ON_EN_KILLFOCUS (IDC_EDIT_LINE_SPEED,           &CMotionPathEditorPage::OnLineSpeedKillFocus)
    ON_BN_CLICKED   (IDC_BTN_ADD_SEGMENT,      &CMotionPathEditorPage::OnAddSegment)
    ON_BN_CLICKED   (IDC_BTN_DELETE_SEGMENT,   &CMotionPathEditorPage::OnDeleteSegment)
    ON_BN_CLICKED   (IDC_BTN_DUPLICATE_SEGMENT,&CMotionPathEditorPage::OnDuplicateSegment)
    ON_BN_CLICKED   (IDC_BTN_SEGMENT_MOVE_UP,  &CMotionPathEditorPage::OnMoveSegmentUp)
    ON_BN_CLICKED   (IDC_BTN_SEGMENT_MOVE_DOWN,&CMotionPathEditorPage::OnMoveSegmentDown)
    ON_BN_CLICKED   (IDC_BTN_VALIDATE_SEGMENT, &CMotionPathEditorPage::OnValidateSegment)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST_MOTION_SEGMENTS,
                                               &CMotionPathEditorPage::OnSegmentListSelChanged)
END_MESSAGE_MAP()

//
// GetFieldHelpTable — expose this page's field-help table to the base class.
//
void CMotionPathEditorPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

//
// OnInitDialog — populate the Type, Coord Mode, and Direction combos and set
// up the segment timeline list-control columns.
//
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
        // Initial selection mirrors the scenario's default coord mode so
        // the combo lands on Local before any segment is loaded.
        int defaultIdx = 0;
        if (m_scenario) {
            switch (m_scenario->defaultCoordMode) {
                case CoordMode::LatLonAlt: defaultIdx = 0; break;
                case CoordMode::Local:     defaultIdx = 1; break;
                case CoordMode::ECEF:      defaultIdx = 2; break;
            }
        }
        cb->SetCurSel(defaultIdx);
    }

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_DIR))
    {
        cb->AddString(_T("Clockwise"));
        cb->AddString(_T("CounterClockwise"));
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

//
// ActiveEntity — return the entity at m_entityIdx, or nullptr if out of range.
//
Entity* CMotionPathEditorPage::ActiveEntity()
{
    if (!m_scenario) return nullptr;
    if (m_entityIdx < 0 || m_entityIdx >= (int)m_scenario->entities.size()) return nullptr;
    return &m_scenario->entities[m_entityIdx];
}

//
// ActiveSegment — return the segment at m_segmentIdx of the active entity, or
// nullptr if no entity/segment is selected.
//
MotionSegment* CMotionPathEditorPage::ActiveSegment()
{
    Entity* e = ActiveEntity();
    if (!e) return nullptr;
    if (m_segmentIdx < 0 || m_segmentIdx >= (int)e->motionSegments.size()) return nullptr;
    return &e->motionSegments[m_segmentIdx];
}

//
// Refresh — rebuild the entity combo and segment list, then load the active
// segment. No-op if the window isn't created yet.
//
void CMotionPathEditorPage::Refresh()
{
    if (!::IsWindow(GetSafeHwnd())) return;
    RefreshEntityCombo();
    RefreshSegmentList();
    LoadActiveSegment();
}

//
// RefreshEntityCombo — repopulate the entity selector with "name (id=N)"
// labels; keeps the model index in each item's data. Events suppressed.
//
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

//
// RefreshSegmentList — rebuild the timeline list rows (index, enabled, start,
// end, type, description) for the active entity and restore the selection.
//
void CMotionPathEditorPage::RefreshSegmentList()
{
    CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_MOTION_SEGMENTS);
    if (!lv) return;
    m_suppressEvents = true;
    lv->DeleteAllItems();

    Entity* e = ActiveEntity();
    if (e)
    {
        // Auto-select the first segment if the user hasn't picked one yet
        // (or the previous selection is stale). Without this, the Type-
        // Specific group stays hidden until the user clicks a row, which
        // is surprising when an entity has exactly one segment.
        const int count = static_cast<int>(e->motionSegments.size());
        if (m_segmentIdx < 0 || m_segmentIdx >= count)
            m_segmentIdx = (count > 0) ? 0 : -1;

        for (size_t i = 0; i < e->motionSegments.size(); ++i)
        {
            const MotionSegment& s = e->motionSegments[i];
            CString idx; idx.Format(_T("%zu"), i + 1);
            const int row = lv->InsertItem((int)i, idx);
            lv->SetItemText(row, 1, s.enabled ? _T("Yes") : _T("No"));
            lv->SetItemText(row, 2, FormatDoubleTrim(s.startSecond));
            lv->SetItemText(row, 3, FormatDoubleTrim(s.endSecond));
            const TCHAR* tn = _T("Stationary");
            switch (s.type) {
                case MotionType::Stationary: tn = _T("Stationary"); break;
                case MotionType::StopHold:   tn = _T("StopHold");   break;
                case MotionType::Line:       tn = _T("Line");       break;
                case MotionType::Ellipse:    tn = _T("Ellipse");    break;
            }
            if (s.type == MotionType::Line && s.accelerateFromStop)
                tn = _T("Line (Take-off)");
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

//
// LoadActiveSegment — copy the active segment's enabled/timing/type/coord/
// description plus the Line and Ellipse geometry into the controls, then set
// type-specific visibility. Events suppressed during the load.
//
void CMotionPathEditorPage::LoadActiveSegment()
{
    if (!::IsWindow(GetSafeHwnd())) return;
    m_suppressEvents = true;

    MotionSegment* s = ActiveSegment();
    const bool have = (s != nullptr);

    CheckDlgButton(IDC_CHK_SEGMENT_ENABLED, have && s->enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_SEGMENT_TAKEOFF,
                   have && s->accelerateFromStop ? BST_CHECKED : BST_UNCHECKED);
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

    if (have) {
        LoadEllipseFromSegment(*s);
        LoadLineFromSegment(*s);
    }
    SetTypeSpecificVisibility();
    m_suppressEvents = false;
}

//
// CommitActiveSegmentFromUi — write enabled/timing/type/take-off/coord/
// description back into the active segment, then commit either the Ellipse or
// the Line/Stationary/StopHold geometry per the committed type.
//
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
    // Take-off is only meaningful for a Line; the checkbox is hidden otherwise.
    // Evaluate after the type is committed above so a type change clears it.
    s->accelerateFromStop = (s->type == MotionType::Line) &&
                            (IsDlgButtonChecked(IDC_CHK_SEGMENT_TAKEOFF) == BST_CHECKED);
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

    if (s->type == MotionType::Ellipse)
        CommitEllipseToSegment(*s);
    else
        CommitLineToSegment(*s);   // covers Line, Stationary, StopHold
}

//
// SetTypeSpecificVisibility — show/hide the Ellipse group, the Line/Stationary/
// StopHold start group, and the Line-only end+speed+take-off controls for the
// current type, and relabel the coordinate fields to match the coord mode.
//
void CMotionPathEditorPage::SetTypeSpecificVisibility()
{
    MotionSegment* s = ActiveSegment();
    const MotionType type = s ? s->type : MotionType::Stationary;
    const bool ellipse    = (s && type == MotionType::Ellipse);
    const bool line       = (s && type == MotionType::Line);
    const bool startOnly  = (s && (type == MotionType::Stationary ||
                                   type == MotionType::StopHold));
    const bool showLine   = (line || startOnly);

    // ----- Ellipse group (two-foci + length form) -----
    const int ellipseIds[] = {
        IDC_GROUP_ELLIPSE_PARAMS,
        IDC_LBL_F1, IDC_LBL_F2,
        IDC_LBL_LEN, IDC_LBL_BRG, IDC_LBL_SPD, IDC_LBL_DIR,
        IDC_EDIT_F1_A, IDC_EDIT_F1_B, IDC_EDIT_F1_C,
        IDC_EDIT_F2_A, IDC_EDIT_F2_B, IDC_EDIT_F2_C,
        IDC_EDIT_LEN, IDC_EDIT_BEARING, IDC_EDIT_SPEED,
        IDC_COMBO_DIR,
        IDC_LBL_ELLIPSE_START,
        IDC_EDIT_ELLIPSE_START_A, IDC_EDIT_ELLIPSE_START_B, IDC_EDIT_ELLIPSE_START_C,
        IDC_LBL_ELLIPSE_HINT,
    };
    for (int id : ellipseIds)
        if (CWnd* w = GetDlgItem(id))
            w->ShowWindow(ellipse ? SW_SHOW : SW_HIDE);

    // ----- Line / Stationary / StopHold group -----
    const int lineStartIds[] = {
        IDC_GROUP_LINE_PARAMS,
        IDC_LBL_LINE_START_POS,
        IDC_EDIT_LINE_START_A, IDC_EDIT_LINE_START_B, IDC_EDIT_LINE_START_C,
        IDC_EDIT_LINE_START_H, IDC_EDIT_LINE_START_P, IDC_EDIT_LINE_START_R,
        IDC_LBL_LINE_START_H_TAG, IDC_LBL_LINE_START_P_TAG, IDC_LBL_LINE_START_R_TAG,
        IDC_LBL_LINE_HINT,
    };
    for (int id : lineStartIds)
        if (CWnd* w = GetDlgItem(id))
            w->ShowWindow(showLine ? SW_SHOW : SW_HIDE);

    const int lineEndIds[] = {
        IDC_LBL_LINE_END_POS,
        IDC_EDIT_LINE_END_A, IDC_EDIT_LINE_END_B, IDC_EDIT_LINE_END_C,
        IDC_EDIT_LINE_END_H, IDC_EDIT_LINE_END_P, IDC_EDIT_LINE_END_R,
        IDC_LBL_LINE_END_H_TAG, IDC_LBL_LINE_END_P_TAG, IDC_LBL_LINE_END_R_TAG,
        // Speed field — only meaningful when there IS an end point (i.e.
        // when the entity is actually traveling). Stationary / StopHold
        // hide it together with the End row.
        IDC_LBL_LINE_SPEED, IDC_EDIT_LINE_SPEED,
        // Take-off is a Line-only attribute (accelerate from a stop to Speed).
        IDC_CHK_SEGMENT_TAKEOFF,
    };
    for (int id : lineEndIds)
        if (CWnd* w = GetDlgItem(id))
            w->ShowWindow(line ? SW_SHOW : SW_HIDE);

    // Relabel the Ellipse F1/F2 labels + the Line Start/End labels based
    // on the segment's coord mode.
    if (ellipse)
    {
        const TCHAR* f1Txt = _T("Focus 1 (Lat/Lon/Alt):");
        const TCHAR* f2Txt = _T("Focus 2 (Lat/Lon/Alt):");
        const TCHAR* stTxt = _T("Start (Lat/Lon/Alt):");
        switch (s->coordMode) {
            case CoordMode::Local:
                f1Txt = _T("Focus 1 (X/Y/Z m):");
                f2Txt = _T("Focus 2 (X/Y/Z m):");
                stTxt = _T("Start (X/Y/Z m):");
                break;
            case CoordMode::ECEF:
                f1Txt = _T("Focus 1 (ECEF X/Y/Z):");
                f2Txt = _T("Focus 2 (ECEF X/Y/Z):");
                stTxt = _T("Start (ECEF X/Y/Z):");
                break;
            case CoordMode::LatLonAlt: default: break;
        }
        SetDlgItemText(IDC_LBL_F1, f1Txt);
        SetDlgItemText(IDC_LBL_F2, f2Txt);
        SetDlgItemText(IDC_LBL_ELLIPSE_START, stTxt);
    }
    if (showLine)
    {
        const TCHAR* startTxt = _T("Start Lat / Lon / Alt:");
        const TCHAR* endTxt   = _T("End   Lat / Lon / Alt:");
        switch (s->coordMode) {
            case CoordMode::Local:
                startTxt = _T("Start X / Y / Z (m):");
                endTxt   = _T("End   X / Y / Z (m):");
                break;
            case CoordMode::ECEF:
                startTxt = _T("Start ECEF X / Y / Z:");
                endTxt   = _T("End   ECEF X / Y / Z:");
                break;
            case CoordMode::LatLonAlt: default: break;
        }
        SetDlgItemText(IDC_LBL_LINE_START_POS, startTxt);
        SetDlgItemText(IDC_LBL_LINE_END_POS,   endTxt);
    }
}

// Helper: copy a focus triple from the segment into 3 doubles based on
// the segment's coord mode. Used by both Load and the coord-mode-flip
// conversion.
// Local-mode Z is STORED as height above the flat tangent plane at the scenario origin,
// but the edit boxes PRESENT it as altitude above the ellipsoid - the thing an author
// means by "0". The two differ by the plane rise, ~1 m per 1.4 km of range, so a ship
// authored as "1" twelve km out was really at 12.4 m and visibly flew. These convert at
// the UI boundary only: nothing downstream of the edit boxes changes meaning, and files
// from older builds keep working (they simply display their true altitude).
// No-op in the other coord modes, where the third component is already an altitude
// (LatLonAlt) or an ECEF axis.
static double LocalZToDisplay(CoordMode mode, const Scenario* scn, double e, double nn, double up)
{
    if (mode != CoordMode::Local || !scn) return up;
    return CoordTransforms::LocalUpToGeodeticAlt(e, nn, up,
               scn->originLatDeg, scn->originLonDeg, scn->originAltM);
}
static double LocalZFromDisplay(CoordMode mode, const Scenario* scn, double e, double nn, double altM)
{
    if (mode != CoordMode::Local || !scn) return altM;
    return CoordTransforms::GeodeticAltToLocalUp(e, nn, altM,
               scn->originLatDeg, scn->originLonDeg, scn->originAltM);
}

static void GetFocusTriple(const MotionSegment& s, int which,
                           double& a, double& b, double& c)
{
    switch (s.coordMode)
    {
        case CoordMode::Local:
            a = (which == 1) ? s.f1LocalX : s.f2LocalX;
            b = (which == 1) ? s.f1LocalY : s.f2LocalY;
            c = (which == 1) ? s.f1LocalZ : s.f2LocalZ;
            break;
        case CoordMode::ECEF:
            a = (which == 1) ? s.f1EcefX : s.f2EcefX;
            b = (which == 1) ? s.f1EcefY : s.f2EcefY;
            c = (which == 1) ? s.f1EcefZ : s.f2EcefZ;
            break;
        case CoordMode::LatLonAlt:
        default:
            a = (which == 1) ? s.f1Lat : s.f2Lat;
            b = (which == 1) ? s.f1Lon : s.f2Lon;
            c = (which == 1) ? s.f1Alt : s.f2Alt;
            break;
    }
}

//
// SetFocusTriple — inverse of GetFocusTriple: store 3 doubles into focus 1 or 2
// of the segment's active-frame fields per s.coordMode.
//
static void SetFocusTriple(MotionSegment& s, int which,
                           double a, double b, double c)
{
    switch (s.coordMode)
    {
        case CoordMode::Local:
            if (which == 1) { s.f1LocalX = a; s.f1LocalY = b; s.f1LocalZ = c; }
            else            { s.f2LocalX = a; s.f2LocalY = b; s.f2LocalZ = c; }
            break;
        case CoordMode::ECEF:
            if (which == 1) { s.f1EcefX = a; s.f1EcefY = b; s.f1EcefZ = c; }
            else            { s.f2EcefX = a; s.f2EcefY = b; s.f2EcefZ = c; }
            break;
        case CoordMode::LatLonAlt:
        default:
            if (which == 1) { s.f1Lat = a; s.f1Lon = b; s.f1Alt = c; }
            else            { s.f2Lat = a; s.f2Lon = b; s.f2Alt = c; }
            break;
    }
}

//
// LoadEllipseFromSegment — write the two foci, length, bearing, speed, and
// direction into the Ellipse controls, plus the read-only computed start
// position (sampler pose at geometric u=0) shown in the segment's coord mode.
//
void CMotionPathEditorPage::LoadEllipseFromSegment(const MotionSegment& s)
{
    double a, b, c;
    GetFocusTriple(s, 1, a, b, c);
    WriteDoubleText(*this, IDC_EDIT_F1_A, a);
    WriteDoubleText(*this, IDC_EDIT_F1_B, b);
    WriteDoubleText(*this, IDC_EDIT_F1_C, LocalZToDisplay(s.coordMode, m_scenario, a, b, c));
    GetFocusTriple(s, 2, a, b, c);
    WriteDoubleText(*this, IDC_EDIT_F2_A, a);
    WriteDoubleText(*this, IDC_EDIT_F2_B, b);
    WriteDoubleText(*this, IDC_EDIT_F2_C, LocalZToDisplay(s.coordMode, m_scenario, a, b, c));

    WriteDoubleText(*this, IDC_EDIT_LEN,     s.lengthMeters);
    WriteDoubleText(*this, IDC_EDIT_BEARING, s.startBearingDeg);
    WriteDoubleText(*this, IDC_EDIT_SPEED,   s.speedMps);

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_DIR))
        cb->SetCurSel(s.direction == EllipseDirection::Clockwise ? 0 : 1);

    // Computed start position (read-only). The exact start is what the sampler
    // produces at geometric u=0 (theta0 from startBearingDeg). Show it in the
    // segment's coordMode so it matches the focus fields. Set on the Preview tab.
    double sa = 0.0, sb = 0.0, sc = 0.0;
    if (m_scenario)
    {
        const SampledPose p = MotionSampler::EvaluateSegment(
            s, 0.0, m_scenario, /*geometricMode*/ true);
        switch (s.coordMode)
        {
        case CoordMode::Local:
        {
        //    double up;
            CoordTransforms::EcefToLocalEnuDeg(
                p.ecefX, p.ecefY, p.ecefZ,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
                sa, sb, sc);   // East/North/Up → X/Y/Z
            // Match the editable boxes above: the Z column is an ALTITUDE, so a path
            // authored at sea level reads back 0 here instead of the plane rise.
            { double gLat = 0.0, gLon = 0.0;
              CoordTransforms::EcefToGeodeticDeg(p.ecefX, p.ecefY, p.ecefZ, gLat, gLon, sc); }
            break;
        }
        case CoordMode::ECEF:
            sa = p.ecefX; sb = p.ecefY; sc = p.ecefZ;
            break;
        case CoordMode::LatLonAlt:
        default:
            CoordTransforms::EcefToGeodeticDeg(p.ecefX, p.ecefY, p.ecefZ, sa, sb, sc);
            break;
        }
    }
    WriteDoubleText(*this, IDC_EDIT_ELLIPSE_START_A, sa);
    WriteDoubleText(*this, IDC_EDIT_ELLIPSE_START_B, sb);
    WriteDoubleText(*this, IDC_EDIT_ELLIPSE_START_C, sc);
}

//
// CommitEllipseToSegment — read the two foci, length, bearing, speed, and
// direction controls back into the segment.
//
void CMotionPathEditorPage::CommitEllipseToSegment(MotionSegment& s)
{
    {
        const double f1a = ReadDoubleText(*this, IDC_EDIT_F1_A, 0.0);
        const double f1b = ReadDoubleText(*this, IDC_EDIT_F1_B, 0.0);
        const double f1c = ReadDoubleText(*this, IDC_EDIT_F1_C, 0.0);
        SetFocusTriple(s, 1, f1a, f1b,
                       LocalZFromDisplay(s.coordMode, m_scenario, f1a, f1b, f1c));
        const double f2a = ReadDoubleText(*this, IDC_EDIT_F2_A, 0.0);
        const double f2b = ReadDoubleText(*this, IDC_EDIT_F2_B, 0.0);
        const double f2c = ReadDoubleText(*this, IDC_EDIT_F2_C, 0.0);
        SetFocusTriple(s, 2, f2a, f2b,
                       LocalZFromDisplay(s.coordMode, m_scenario, f2a, f2b, f2c));
    }

    s.lengthMeters    = ReadDoubleText(*this, IDC_EDIT_LEN,     s.lengthMeters);
    s.startBearingDeg = ReadDoubleText(*this, IDC_EDIT_BEARING, s.startBearingDeg);
    s.speedMps        = ReadDoubleText(*this, IDC_EDIT_SPEED,   s.speedMps);

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_DIR))
        s.direction = (cb->GetCurSel() == 0) ? EllipseDirection::Clockwise
                                             : EllipseDirection::CounterClockwise;
}

//
// SeedEllipseDefaultsIfBlank — when the foci/length/speed look unset, seed the
// scenario's default coord mode, place both foci at the origin, use a 1 km
// diameter, and pull speed from the entity's airframe cruise (else 100 m/s).
//
void CMotionPathEditorPage::SeedEllipseDefaultsIfBlank(MotionSegment& s)
{
    // Inherit the scenario's default coord mode if both foci look unset
    // across all three frames.
    auto fociBlank = [](const MotionSegment& s) {
        return s.f1Lat == 0.0 && s.f1Lon == 0.0 && s.f1Alt == 0.0 &&
               s.f2Lat == 0.0 && s.f2Lon == 0.0 && s.f2Alt == 0.0 &&
               s.f1LocalX == 0.0 && s.f1LocalY == 0.0 && s.f1LocalZ == 0.0 &&
               s.f2LocalX == 0.0 && s.f2LocalY == 0.0 && s.f2LocalZ == 0.0 &&
               s.f1EcefX == 0.0 && s.f1EcefY == 0.0 && s.f1EcefZ == 0.0 &&
               s.f2EcefX == 0.0 && s.f2EcefY == 0.0 && s.f2EcefZ == 0.0;
    };
    const bool blank = fociBlank(s);
    if (m_scenario && blank)
    {
        s.coordMode = m_scenario->defaultCoordMode;
        if (s.coordMode == CoordMode::Local)
        {
            s.f1LocalX = s.f2LocalX = 0.0;
            s.f1LocalY = s.f2LocalY = 0.0;
            s.f1LocalZ = s.f2LocalZ = 100.0;
        }
        else
        {
            s.f1Lat = s.f2Lat = m_scenario->originLatDeg;
            s.f1Lon = s.f2Lon = m_scenario->originLonDeg;
            s.f1Alt = s.f2Alt = m_scenario->originAltM;
        }
    }

    // Default length: 1 km diameter (radius 500 m) for a sensibly-sized
    // circle at the scenario origin.
    if (s.lengthMeters <= 0.0) s.lengthMeters = 1000.0;

    // Speed from airframe cruise; if the type isn't catalogued, default to
    // 10 m/s and log an error (see SeedCruiseSpeedMps).
    if (s.speedMps <= 0.0)
        s.speedMps = SeedCruiseSpeedMps(ActiveEntity());

    // startBearingDeg defaults to 0 (North) — fine, no override.
}

//
// LoadLineFromSegment — write the start and end position triples (per coord
// mode), the start/end heading-pitch-roll, and the Line speed into controls.
//
void CMotionPathEditorPage::LoadLineFromSegment(const MotionSegment& s)
{
    // Bind the three Start position edits + the three End position edits
    // to the active-frame triple per s.coordMode.
    auto loadTriple = [this](const MotionSegment& s, bool isEnd) {
        const UINT idA = isEnd ? IDC_EDIT_LINE_END_A : IDC_EDIT_LINE_START_A;
        const UINT idB = isEnd ? IDC_EDIT_LINE_END_B : IDC_EDIT_LINE_START_B;
        const UINT idC = isEnd ? IDC_EDIT_LINE_END_C : IDC_EDIT_LINE_START_C;
        double a = 0, b = 0, c = 0;
        switch (s.coordMode) {
            case CoordMode::Local:
                a = isEnd ? s.endLocalX : s.startLocalX;
                b = isEnd ? s.endLocalY : s.startLocalY;
                c = isEnd ? s.endLocalZ : s.startLocalZ;
                break;
            case CoordMode::ECEF:
                a = isEnd ? s.endEcefX : s.startEcefX;
                b = isEnd ? s.endEcefY : s.startEcefY;
                c = isEnd ? s.endEcefZ : s.startEcefZ;
                break;
            case CoordMode::LatLonAlt: default:
                a = isEnd ? s.endLat : s.startLat;
                b = isEnd ? s.endLon : s.startLon;
                c = isEnd ? s.endAlt : s.startAlt;
                break;
        }
        WriteDoubleText(*this, idA, a);
        WriteDoubleText(*this, idB, b);
        WriteDoubleText(*this, idC, LocalZToDisplay(s.coordMode, m_scenario, a, b, c));
    };
    loadTriple(s, false);
    loadTriple(s, true);

    WriteDoubleText(*this, IDC_EDIT_LINE_START_H, s.startHeadingDeg);
    WriteDoubleText(*this, IDC_EDIT_LINE_START_P, s.startPitchDeg);
    WriteDoubleText(*this, IDC_EDIT_LINE_START_R, s.startRollDeg);
    WriteDoubleText(*this, IDC_EDIT_LINE_END_H,   s.endHeadingDeg);
    WriteDoubleText(*this, IDC_EDIT_LINE_END_P,   s.endPitchDeg);
    WriteDoubleText(*this, IDC_EDIT_LINE_END_R,   s.endRollDeg);

    // Line speed shares the same s.speedMps field as Ellipse — the
    // sampler picks the right interpretation per segment type.
    WriteDoubleText(*this, IDC_EDIT_LINE_SPEED, s.speedMps);
}

//
// CommitLineToSegment — read the start (and, for Line, end) position triples
// and attitudes back into the segment. Only reads the End row and speed for a
// Line; deliberately does NOT re-derive End Time (see inline note).
//
void CMotionPathEditorPage::CommitLineToSegment(MotionSegment& s)
{
    auto commitTriple = [this](MotionSegment& s, bool isEnd) {
        const UINT idA = isEnd ? IDC_EDIT_LINE_END_A : IDC_EDIT_LINE_START_A;
        const UINT idB = isEnd ? IDC_EDIT_LINE_END_B : IDC_EDIT_LINE_START_B;
        const UINT idC = isEnd ? IDC_EDIT_LINE_END_C : IDC_EDIT_LINE_START_C;
        const double a    = ReadDoubleText(*this, idA, 0.0);
        const double b    = ReadDoubleText(*this, idB, 0.0);
        const double cIn  = ReadDoubleText(*this, idC, 0.0);
        const double c    = LocalZFromDisplay(s.coordMode, m_scenario, a, b, cIn);
        switch (s.coordMode) {
            case CoordMode::Local:
                if (isEnd) { s.endLocalX = a; s.endLocalY = b; s.endLocalZ = c; }
                else       { s.startLocalX = a; s.startLocalY = b; s.startLocalZ = c; }
                break;
            case CoordMode::ECEF:
                if (isEnd) { s.endEcefX = a; s.endEcefY = b; s.endEcefZ = c; }
                else       { s.startEcefX = a; s.startEcefY = b; s.startEcefZ = c; }
                break;
            case CoordMode::LatLonAlt: default:
                if (isEnd) { s.endLat = a; s.endLon = b; s.endAlt = c; }
                else       { s.startLat = a; s.startLon = b; s.startAlt = c; }
                break;
        }
    };
    commitTriple(s, false);
    // End row is only authored when type == Line; for Stationary / StopHold
    // it's hidden and we don't want to zero out endLat etc. by reading
    // empty controls.
    if (s.type == MotionType::Line)
        commitTriple(s, true);

    // The user edited exactly one rep (the coordMode rep shown in the row);
    // fan it out to the other two so the segment stays self-consistent and the
    // Preview (which draws from Local) reflects a Lat/Lon- or ECEF-mode edit.
    if (m_scenario)
    {
        MotionSampler::SyncSegmentEndpoint(s, /*isEnd*/false, s.coordMode,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM);
        if (s.type == MotionType::Line)
            MotionSampler::SyncSegmentEndpoint(s, /*isEnd*/true, s.coordMode,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM);
    }

    s.startHeadingDeg = ReadDoubleText(*this, IDC_EDIT_LINE_START_H, s.startHeadingDeg);
    s.startPitchDeg   = ReadDoubleText(*this, IDC_EDIT_LINE_START_P, s.startPitchDeg);
    s.startRollDeg    = ReadDoubleText(*this, IDC_EDIT_LINE_START_R, s.startRollDeg);
    if (s.type == MotionType::Line)
    {
        s.endHeadingDeg = ReadDoubleText(*this, IDC_EDIT_LINE_END_H, s.endHeadingDeg);
        s.endPitchDeg   = ReadDoubleText(*this, IDC_EDIT_LINE_END_P, s.endPitchDeg);
        s.endRollDeg    = ReadDoubleText(*this, IDC_EDIT_LINE_END_R, s.endRollDeg);
        // Line uses speedMps. Read it back, but do NOT re-derive endSecond
        // here. This function runs on routine commits (entity / segment
        // switching, list refresh, etc.) where the speed field still
        // holds the previously-loaded value — recomputing endSecond from
        // it would silently clobber a user-authored End Time. The
        // speed↔endTime coupling now lives in OnLineSpeedKillFocus so it
        // only fires when the user actually edits the Speed field.
        const double newSpeed = ReadDoubleText(*this, IDC_EDIT_LINE_SPEED, 0.0);
        if (newSpeed > 0.0)
            s.speedMps = newSpeed;
    }
}

//
// SeedLineDefaultsIfBlank — anchor a blank start at the scenario origin (or
// local 0,0,100); for a Line also give a ~1 km end and a cruise/100 m/s speed,
// then re-derive End Time so the seeded start/end/speed are self-consistent.
//
void CMotionPathEditorPage::SeedLineDefaultsIfBlank(MotionSegment& s)
{
    if (!m_scenario) return;

    // If all start position fields are 0 across all three frames, anchor
    // them at the scenario origin (LatLonAlt mode) or at local (0,0,100)
    // (Local mode). This makes new Line segments visible immediately.
    const bool startBlank =
        (s.startLat == 0.0 && s.startLon == 0.0 && s.startAlt == 0.0 &&
         s.startLocalX == 0.0 && s.startLocalY == 0.0 && s.startLocalZ == 0.0 &&
         s.startEcefX == 0.0 && s.startEcefY == 0.0 && s.startEcefZ == 0.0);
    if (startBlank)
    {
        if (s.coordMode == CoordMode::Local)
        {
            s.startLocalX = 0.0;
            s.startLocalY = 0.0;
            s.startLocalZ = 100.0;
        }
        else
        {
            s.startLat = m_scenario->originLatDeg;
            s.startLon = m_scenario->originLonDeg;
            s.startAlt = m_scenario->originAltM;
        }
    }

    // End defaults: same as start plus a small offset, so the line has a
    // visible length. Only matters for Line type.
    if (s.type == MotionType::Line)
    {
        const bool endBlank =
            (s.endLat == 0.0 && s.endLon == 0.0 && s.endAlt == 0.0 &&
             s.endLocalX == 0.0 && s.endLocalY == 0.0 && s.endLocalZ == 0.0 &&
             s.endEcefX == 0.0 && s.endEcefY == 0.0 && s.endEcefZ == 0.0);
        if (endBlank)
        {
            if (s.coordMode == CoordMode::Local)
            {
                s.endLocalX = 1000.0;        // 1 km east
                s.endLocalY = 0.0;
                s.endLocalZ = s.startLocalZ;
            }
            else
            {
                s.endLat = m_scenario->originLatDeg;
                s.endLon = m_scenario->originLonDeg + 0.01;   // ~1 km east at mid-lats
                s.endAlt = m_scenario->originAltM;
            }
        }

        // Seed Line speed from the active entity's airframe cruise speed (same
        // lookup the Ellipse path uses). If a profile isn't catalogued, default
        // to 10 m/s and log an error (see SeedCruiseSpeedMps).
        if (s.speedMps <= 0.0)
            s.speedMps = SeedCruiseSpeedMps(ActiveEntity());

        // Re-derive endSecond to match the seeded speed across the seeded
        // start→end distance, so the user sees a self-consistent setup.
        const double dist = ApproxLineDistanceMeters(s);
        if (dist > 0.0 && s.speedMps > 0.0)
            s.endSecond = s.startSecond + dist / s.speedMps;
    }
}

//
// OnShowWindow — when the page becomes visible, Refresh() so entity/segment
// edits made on other tabs are reflected without reloading the file.
//
void CMotionPathEditorPage::OnShowWindow(BOOL bShow, UINT nStatus)
{
    CHelpAwarePage::OnShowWindow(bShow, nStatus);
    if (!bShow || !m_scenario) return;
    // The Asset tab can add/rename/delete entities behind our back. Rebuild
    // the entity combo + segment list every time we come into view so the
    // user sees the current scenario without having to reload the file.
    Refresh();
}

//
// OnEntityChanged — commit the current segment, switch m_entityIdx to the
// combo selection, clear the segment selection, and reload the list.
//
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

//
// OnEllipseSpeedKillFocus — parse the Ellipse speed field (Mach/mph/km/h) and
// rewrite it as plain m/s.
//
void CMotionPathEditorPage::OnEllipseSpeedKillFocus()
{
    if (m_suppressEvents) return;
    CString t;
    GetDlgItemText(IDC_EDIT_SPEED, t);
    if (t.IsEmpty()) return;
    const double mps = ParseSpeedMps(t);
    if (!std::isnan(mps))
        SetDlgItemText(IDC_EDIT_SPEED, FormatDoubleTrim(mps));
}

//
// OnLineSpeedKillFocus — normalize the Line speed to m/s and, since this is a
// real user edit, slide End Time so distance = speed x duration holds, updating
// both the End Time field and the timeline list cell.
//
void CMotionPathEditorPage::OnLineSpeedKillFocus()
{
    if (m_suppressEvents) return;
    CString t;
    GetDlgItemText(IDC_EDIT_LINE_SPEED, t);
    if (t.IsEmpty()) return;
    const double mps = ParseSpeedMps(t);
    if (std::isnan(mps)) return;

    // Re-format the field back as plain m/s (handles Mach / mph / km/h).
    SetDlgItemText(IDC_EDIT_LINE_SPEED, FormatDoubleTrim(mps));

    // Speed→EndTime coupling: when the user actively edits Speed, slide
    // End Time so distance = speed × duration stays self-consistent. We
    // only do this on the kill-focus event (a real user edit), NOT on
    // routine commits, so a user-authored End Time is preserved when
    // they bounce between entities / segments.
    MotionSegment* s = ActiveSegment();
    if (!s || s->type != MotionType::Line || mps <= 0.0) return;
    s->speedMps = mps;
    const double dist = ApproxLineDistanceMeters(*s);
    if (dist <= 0.0) return;
    s->endSecond = s->startSecond + dist / mps;
    SetDlgItemText(IDC_EDIT_SEGMENT_END_TIME, FormatDoubleTrim(s->endSecond));
    if (CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_MOTION_SEGMENTS))
        if (m_segmentIdx >= 0)
            lv->SetItemText(m_segmentIdx, 3, FormatDoubleTrim(s->endSecond));
}

//
// OnSegmentTimingChanged — push the start/end time edits into the segment and
// update only the timeline list cells (avoids clobbering in-progress typing).
//
void CMotionPathEditorPage::OnSegmentTimingChanged()
{
    if (m_suppressEvents) return;
    MotionSegment* s = ActiveSegment();
    if (!s) return;
    // Read just the two timing edits and push to the active segment, then
    // refresh ONLY the timeline list cells (avoid a full Load that would
    // overwrite the user's in-progress typing in the same edit).
    s->startSecond = ReadDoubleText(*this, IDC_EDIT_SEGMENT_START_TIME, s->startSecond);
    s->endSecond   = ReadDoubleText(*this, IDC_EDIT_SEGMENT_END_TIME,   s->endSecond);

    if (CListCtrl* lv = (CListCtrl*)GetDlgItem(IDC_LIST_MOTION_SEGMENTS))
    {
        if (m_segmentIdx >= 0)
        {
            lv->SetItemText(m_segmentIdx, 2, FormatDoubleTrim(s->startSecond));
            lv->SetItemText(m_segmentIdx, 3, FormatDoubleTrim(s->endSecond));
        }
    }
    NotifyDirty(this);
}

//
// OnSegmentCoordModeChanged — the user picked a new coordinate frame; commit
// current values, then convert the segment's foci (Ellipse) or start/end
// (Line/Stationary/StopHold) through ECEF into the new frame and reload the UI.
//
void CMotionPathEditorPage::OnSegmentCoordModeChanged()
{
    if (m_suppressEvents) return;
    MotionSegment* s = ActiveSegment();
    if (!s) return;

    // Read the OLD mode (what the segment currently stores).
    const CoordMode oldMode = s->coordMode;

    // Read the NEW mode from the combo.
    CoordMode newMode = oldMode;
    if (const CComboBox* cb = (const CComboBox*)GetDlgItem(IDC_COMBO_SEGMENT_COORD_MODE))
    {
        switch (cb->GetCurSel()) {
            case 0: newMode = CoordMode::LatLonAlt; break;
            case 1: newMode = CoordMode::Local;     break;
            case 2: newMode = CoordMode::ECEF;      break;
        }
    }
    if (newMode == oldMode) return;   // nothing to do

    // Commit currently-displayed UI values into the old triple so we have a
    // fixed-point in space to translate from.
    if (s->type == MotionType::Ellipse)
        CommitEllipseToSegment(*s);
    else
        CommitLineToSegment(*s);

    auto toEcef = [&](double a, double b, double c,
                      double& ex, double& ey, double& ez)
    {
        switch (oldMode) {
            case CoordMode::LatLonAlt:
                CoordTransforms::GeodeticToEcefDeg(a, b, c, ex, ey, ez);
                break;
            case CoordMode::Local:
                if (m_scenario)
                    CoordTransforms::LocalEnuToEcefDeg(
                        a, b, c,
                        m_scenario->originLatDeg, m_scenario->originLonDeg,
                        m_scenario->originAltM, ex, ey, ez);
                break;
            case CoordMode::ECEF:
                ex = a; ey = b; ez = c;
                break;
        }
    };
    auto fromEcef = [&](double ex, double ey, double ez,
                        double& outA, double& outB, double& outC)
    {
        switch (newMode) {
            case CoordMode::LatLonAlt:
                CoordTransforms::EcefToGeodeticDeg(ex, ey, ez, outA, outB, outC);
                break;
            case CoordMode::Local:
                if (m_scenario)
                    CoordTransforms::EcefToLocalEnuDeg(
                        ex, ey, ez,
                        m_scenario->originLatDeg, m_scenario->originLonDeg,
                        m_scenario->originAltM, outA, outB, outC);
                break;
            case CoordMode::ECEF:
                outA = ex; outB = ey; outC = ez;
                break;
        }
    };

    // Line / Stationary / StopHold use start (and Line uses end too).
    auto getOldLine = [&](double& a, double& b, double& c, bool isEnd) {
        switch (oldMode) {
            case CoordMode::LatLonAlt:
                a = isEnd ? s->endLat   : s->startLat;
                b = isEnd ? s->endLon   : s->startLon;
                c = isEnd ? s->endAlt   : s->startAlt;
                break;
            case CoordMode::Local:
                a = isEnd ? s->endLocalX : s->startLocalX;
                b = isEnd ? s->endLocalY : s->startLocalY;
                c = isEnd ? s->endLocalZ : s->startLocalZ;
                break;
            case CoordMode::ECEF:
                a = isEnd ? s->endEcefX : s->startEcefX;
                b = isEnd ? s->endEcefY : s->startEcefY;
                c = isEnd ? s->endEcefZ : s->startEcefZ;
                break;
        }
    };
    auto setNewLine = [&](double a, double b, double c, bool isEnd) {
        switch (newMode) {
            case CoordMode::LatLonAlt:
                if (isEnd) { s->endLat = a; s->endLon = b; s->endAlt = c; }
                else       { s->startLat = a; s->startLon = b; s->startAlt = c; }
                break;
            case CoordMode::Local:
                if (isEnd) { s->endLocalX = a; s->endLocalY = b; s->endLocalZ = c; }
                else       { s->startLocalX = a; s->startLocalY = b; s->startLocalZ = c; }
                break;
            case CoordMode::ECEF:
                if (isEnd) { s->endEcefX = a; s->endEcefY = b; s->endEcefZ = c; }
                else       { s->startEcefX = a; s->startEcefY = b; s->startEcefZ = c; }
                break;
        }
    };

    // Ellipse: convert BOTH foci. Line: convert start + end. Stationary/
    // StopHold: convert start only.
    const bool hasEllipse = (s->type == MotionType::Ellipse);
    const bool hasStart   = (s->type == MotionType::Line ||
                             s->type == MotionType::Stationary ||
                             s->type == MotionType::StopHold);
    const bool hasEnd     = (s->type == MotionType::Line);

    if (hasEllipse) {
        // Pivot each focus through ECEF using the GetFocusTriple/
        // SetFocusTriple helpers (which read from the OLD coordMode). We
        // temporarily flip s->coordMode for the get-then-set sequence.
        const CoordMode storedMode = s->coordMode;
        s->coordMode = oldMode;
        double a, b, c;
        double ex, ey, ez, na, nb, nc;
        for (int which : { 1, 2 }) {
            GetFocusTriple(*s, which, a, b, c);
            toEcef(a, b, c, ex, ey, ez);
            fromEcef(ex, ey, ez, na, nb, nc);
            s->coordMode = newMode;
            SetFocusTriple(*s, which, na, nb, nc);
            s->coordMode = oldMode;
        }
        s->coordMode = storedMode;   // overwritten below
    }
    if (hasStart) {
        double a, b, c, ex, ey, ez, na, nb, nc;
        getOldLine(a, b, c, /*isEnd=*/false);
        toEcef(a, b, c, ex, ey, ez);
        fromEcef(ex, ey, ez, na, nb, nc);
        setNewLine(na, nb, nc, /*isEnd=*/false);
    }
    if (hasEnd) {
        double a, b, c, ex, ey, ez, na, nb, nc;
        getOldLine(a, b, c, /*isEnd=*/true);
        toEcef(a, b, c, ex, ey, ez);
        fromEcef(ex, ey, ez, na, nb, nc);
        setNewLine(na, nb, nc, /*isEnd=*/true);
    }

    s->coordMode = newMode;

    // Refresh UI: relabel + repopulate from new triple.
    m_suppressEvents = true;
    LoadEllipseFromSegment(*s);
    LoadLineFromSegment(*s);
    SetTypeSpecificVisibility();
    m_suppressEvents = false;

    NotifyDirty(this);
}

//
// OnSegmentTypeChanged — commit, seed defaults for the newly selected type
// (Ellipse vs Line/Stationary/StopHold), reload its fields, refresh the list,
// and update type-specific visibility.
//
void CMotionPathEditorPage::OnSegmentTypeChanged()
{
    if (m_suppressEvents) return;
    CommitActiveSegmentFromUi();
    // The user just switched to a new type — if it's Ellipse and the
    // segment's parameters are blank (radius=0 etc.), seed defaults so
    // the orbit is visible without further input. Then refresh the UI.
    if (MotionSegment* s = ActiveSegment())
    {
        if (s->type == MotionType::Ellipse)
        {
            SeedEllipseDefaultsIfBlank(*s);
            LoadEllipseFromSegment(*s);
            NotifyDirty(this);
        }
        else
        {
            // Line / Stationary / StopHold all use the Start row; Line
            // additionally uses the End row.
            SeedLineDefaultsIfBlank(*s);
            LoadLineFromSegment(*s);
            NotifyDirty(this);
        }
    }
    RefreshSegmentList();
    SetTypeSpecificVisibility();
}

//
// OnSegmentListSelChanged — on a new timeline-row selection, commit the
// outgoing segment, update m_segmentIdx, and load the newly selected segment.
//
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

//
// OnAddSegment — append a new Stationary segment starting where the last one
// ended (30 s long), inheriting the scenario's default coord mode; select it.
//
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
    // Inherit the scenario's default coord mode so the segment's "Coord Mode"
    // combo lands on Local X/Y/Z when the user picked Local on Scenario Setup.
    if (m_scenario)
        s.coordMode = m_scenario->defaultCoordMode;
    e->motionSegments.push_back(std::move(s));
    m_segmentIdx  = static_cast<int>(e->motionSegments.size()) - 1;
    RefreshSegmentList();
    LoadActiveSegment();
    NotifyDirty(this);
}

//
// OnDeleteSegment — erase the selected segment and clamp the selection.
//
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

//
// OnDuplicateSegment — insert a copy of the selected segment right after it,
// shifting its time window forward by its own duration; select the copy.
//
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

//
// OnMoveSegmentUp — swap the selected segment with the previous one.
//
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

//
// OnMoveSegmentDown — swap the selected segment with the next one.
//
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

//
// OnValidateSegment — run the full scenario validator, filter the report to the
// selected "Entity N / Motion M" subject, and show its issues in a message box.
//
void CMotionPathEditorPage::OnValidateSegment()
{
    if (!m_scenario) return;
    Entity* e = ActiveEntity();
    if (!e)
    {
        AfxMessageBox(_T("No entity is selected."), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (m_segmentIdx < 0 ||
        m_segmentIdx >= static_cast<int>(e->motionSegments.size()))
    {
        AfxMessageBox(_T("No motion segment is selected."), MB_OK | MB_ICONINFORMATION);
        return;
    }

    CommitActiveSegmentFromUi();

    // Validator emits motion subjects of the form "Entity N / Motion M"
    // where M is 1-based. Filter the full report to just this segment.
    char prefix[80];
    std::snprintf(prefix, sizeof(prefix), "Entity %u / Motion %d",
                  static_cast<unsigned>(e->entityId),
                  m_segmentIdx + 1);

    const Validator::Report rep = Validator::Validate(*m_scenario, theApp.Catalog());
    int err = 0, warn = 0, info = 0;
    CString body;
    auto sevLabel = [](Validator::Severity s) -> const TCHAR* {
        switch (s) {
            case Validator::Severity::Error:   return _T("ERROR");
            case Validator::Severity::Warning: return _T("WARN ");
            default:                           return _T("INFO ");
        }
    };
    for (const auto& i : rep.issues)
    {
        if (i.subject.rfind(prefix, 0) != 0) continue;
        switch (i.severity) {
            case Validator::Severity::Error:   ++err;  break;
            case Validator::Severity::Warning: ++warn; break;
            case Validator::Severity::Info:    ++info; break;
        }
        CString line;
        line.Format(_T("[%s] %s — %s\n"),
                    sevLabel(i.severity),
                    CString(CA2T(i.subject.c_str())).GetString(),
                    CString(CA2T(i.message.c_str())).GetString());
        body += line;
    }

    CString header;
    header.Format(_T("Entity %u / Motion %d  —  Errors: %d   Warnings: %d   Info: %d\n\n"),
                  static_cast<unsigned>(e->entityId), m_segmentIdx + 1,
                  err, warn, info);
    if (body.IsEmpty()) body = _T("(No issues found.)");

    AfxMessageBox(header + body,
                  MB_OK | (err > 0 ? MB_ICONWARNING : MB_ICONINFORMATION));
}
