#include "pch.h"
#include "DeployPage.h"
#include "ScenarioEditor.h"   // theApp.Settings() / SettingsPath()
#include "SettingsIO.h"

#include <algorithm>          // std::min (NOMINMAX is set project-wide)
#include <uxtheme.h>          // SetWindowTheme
#pragma comment(lib, "uxtheme.lib")

namespace
{
    const COLORREF kBlack = RGB(0, 0, 0);
    const COLORREF kWhite = RGB(255, 255, 255);
    const COLORREF kRowSel = RGB(38, 79, 120);   // visible blue row highlight

    // Status glyph colors.
    const COLORREF kOpenBlue   = RGB(120, 190, 255);
    const COLORREF kUseGreen   = RGB(50, 200, 80);
    const COLORREF kUnavailRed = RGB(220, 60, 60);

    // Action-button base colors (Deploy, Stop, Go, Pause, Release).
    const COLORREF kBtnDeploy  = RGB(40, 90, 170);
    const COLORREF kBtnStop    = RGB(170, 40, 40);
    const COLORREF kBtnGo       = RGB(40, 150, 60);
    const COLORREF kBtnPause    = RGB(180, 130, 30);
    const COLORREF kBtnRelease  = RGB(90, 90, 110);

    const int kRowHeight = 60;   // double height
    const int kFirstBtnCol = 6;   // columns 6..10 are the action buttons
    const int kNumBtns = 5;

    // Column headers only — widths are NOT hardcoded. They come from
    // settings.ini (the user's hand-set, persisted widths); a plain uniform
    // width is used only the very first time, before any are saved.
    const TCHAR* const kColumnTitles[] = {
        _T(""),               // 0: status icon
        _T("Status"),         // 1: status word
        _T("Resource"),       // 2: name
        _T("Contact"),        // 3: contact
        _T("Reserved Start"), // 4
        _T("Reserved End"),   // 5
        _T("Deploy"),         // 6
        _T("Stop"),           // 7
        _T("Go"),             // 8
        _T("Pause"),          // 9
        _T("Release"),        // 10
    };
    const int kNumColumns = _countof(kColumnTitles);
    const int kFallbackColWidth = 120;   // first-run only; user resizes from here
}

BEGIN_MESSAGE_MAP(CDeployPage, CHelpAwarePage)
    ON_WM_SIZE()
    ON_WM_SHOWWINDOW()
    ON_WM_DESTROY()
    ON_WM_CTLCOLOR()
    ON_WM_DRAWITEM()
    ON_NOTIFY(NM_CLICK, IDC_LIST_DEPLOY, &CDeployPage::OnListClick)
END_MESSAGE_MAP()

void CDeployPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = nullptr;
    outCount = 0;
}

const TCHAR* CDeployPage::StatusWord(Status s)
{
    switch (s)
    {
        case Status::Open:        return _T("Open");
        case Status::InUse:       return _T("In use");
        case Status::Unavailable: return _T("Unavailable");
    }
    return _T("");
}

BOOL CDeployPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    // Dark theme (same pattern as the Run tab): black page background + white
    // text, edit/other controls handled in OnCtlColor.
    m_blackBrush.CreateSolidBrush(kBlack);
    m_whiteBrush.CreateSolidBrush(kWhite);
    SetBackgroundColor(kBlack);

    m_list.SubclassDlgItem(IDC_LIST_DEPLOY, this);

    // Force the owner-draw row height with a 1 x kRowHeight dummy image list.
    m_rowHeight.Create(1, kRowHeight, ILC_COLOR, 1, 1);
    m_list.SetImageList(&m_rowHeight, LVSIL_SMALL);

    m_list.SetExtendedStyle(m_list.GetExtendedStyle() |
                            LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
    m_list.SetBkColor(kBlack);
    m_list.SetTextBkColor(kBlack);
    m_list.SetTextColor(kWhite);

    // Apply persisted widths if we have one per column; otherwise fall back to
    // a uniform starting width the user can resize (which then persists).
    const std::vector<int>& saved = theApp.Settings().deployColumnWidths;
    const bool haveSaved = (saved.size() == static_cast<size_t>(kNumColumns));
    for (int i = 0; i < kNumColumns; ++i)
    {
        int w = haveSaved ? saved[i] : kFallbackColWidth;
        if (w < 10) w = 10;   // guard against a zero/negative persisted value
        m_list.InsertColumn(i, kColumnTitles[i], LVCFMT_LEFT, w);
    }

    SeedRows();
    LayoutList();
    return TRUE;
}

void CDeployPage::SeedRows()
{
    auto add = [this](Status s, const TCHAR* name, const TCHAR* contact,
                      const TCHAR* start, const TCHAR* end)
    {
        DeployRow r;
        r.status  = s;
        r.name    = name;
        r.contact = contact;
        r.start   = start;
        r.end     = end;
        m_rows.push_back(r);
    };

    add(Status::Open,        _T("NODA AI Server#1"), _T(""),          _T(""),                 _T(""));
    add(Status::InUse,       _T("NODA AI Server#2"), _T("A. Rivera"), _T("2026-07-10 08:00"), _T("2026-07-10 12:00"));
    add(Status::Unavailable, _T("NODA AI Server#3"), _T("M. Chen"),   _T("2026-07-10 06:00"), _T("2026-07-11 06:00"));
    add(Status::Open,        _T("NODA AI Server#4"), _T(""),          _T(""),                 _T(""));
    add(Status::InUse,       _T("NODA AI Server#5"), _T("T. Okafor"), _T("2026-07-10 09:30"), _T("2026-07-10 17:00"));
    add(Status::Open,        _T("NODA AI Server#6"), _T(""),          _T(""),                 _T(""));
    add(Status::Unavailable, _T("NODA AI Server#7"), _T("R. Patel"),  _T("2026-07-09 22:00"), _T("2026-07-10 22:00"));

    // One list item per row; the text is drawn in OnDrawItem, so the item text
    // itself only needs to exist (subitem count follows the columns).
    m_list.DeleteAllItems();
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
        m_list.InsertItem(i, _T(""));
}

void CDeployPage::LayoutList()
{
    if (!::IsWindow(GetSafeHwnd()) || !::IsWindow(m_list.GetSafeHwnd())) return;
    CRect rc;
    GetClientRect(&rc);
    m_list.MoveWindow(rc.left + 4, rc.top + 4, rc.Width() - 8, rc.Height() - 8);
}

void CDeployPage::OnSize(UINT nType, int cx, int cy)
{
    CHelpAwarePage::OnSize(nType, cx, cy);
    LayoutList();
}

void CDeployPage::SaveColumnWidths()
{
    if (!::IsWindow(m_list.GetSafeHwnd())) return;
    if (m_list.GetHeaderCtrl() == nullptr ||
        m_list.GetHeaderCtrl()->GetItemCount() != kNumColumns) return;

    std::vector<int> widths;
    widths.reserve(kNumColumns);
    for (int i = 0; i < kNumColumns; ++i)
        widths.push_back(m_list.GetColumnWidth(i));

    // Only write if something actually changed, to avoid needless disk churn.
    if (theApp.Settings().deployColumnWidths == widths) return;
    theApp.Settings().deployColumnWidths = std::move(widths);
    SettingsIO::Save(theApp.Settings(), theApp.SettingsPath());
}

void CDeployPage::OnShowWindow(BOOL bShow, UINT nStatus)
{
    CHelpAwarePage::OnShowWindow(bShow, nStatus);
    // Persist the user's column widths whenever they navigate away from the tab.
    if (!bShow)
        SaveColumnWidths();
}

void CDeployPage::OnDestroy()
{
    // Catch the app-close case (user resized then quit without switching tabs).
    SaveColumnWidths();
    CHelpAwarePage::OnDestroy();
}

HBRUSH CDeployPage::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    if (!m_blackBrush.GetSafeHandle() || !m_whiteBrush.GetSafeHandle())
        return CHelpAwarePage::OnCtlColor(pDC, pWnd, nCtlColor);

    switch (nCtlColor)
    {
        case CTLCOLOR_DLG:
        case CTLCOLOR_STATIC:
            pDC->SetBkMode(TRANSPARENT);
            pDC->SetTextColor(kWhite);
            pDC->SetBkColor(kBlack);
            return static_cast<HBRUSH>(m_blackBrush);
        default:
            return CHelpAwarePage::OnCtlColor(pDC, pWnd, nCtlColor);
    }
}

// ---------- per-row state machine ----------

bool CDeployPage::BtnEnabled(const DeployRow& r, Btn b) const
{
    switch (b)
    {
        case Btn::Deploy:  return r.status == Status::Open;
        case Btn::Go:      return r.ownedByUs && !r.running;                 // ready or resume-from-pause
        case Btn::Pause:   return r.ownedByUs && r.running;
        case Btn::Stop:    return r.ownedByUs && (r.running || r.paused);
        case Btn::Release: return r.ownedByUs && !r.running && !r.paused;
    }
    return false;
}

void CDeployPage::ApplyBtn(DeployRow& r, Btn b)
{
    switch (b)
    {
        case Btn::Deploy:
            r.status = Status::InUse; r.ownedByUs = true;
            r.running = false; r.paused = false;
            r.contact = _T("(you)");
            break;
        case Btn::Go:      r.running = true;  r.paused = false; break;
        case Btn::Pause:   r.running = false; r.paused = true;  break;
        case Btn::Stop:    r.running = false; r.paused = false; break;
        case Btn::Release:
            r.status = Status::Open; r.ownedByUs = false;
            r.running = false; r.paused = false;
            r.contact.Empty(); r.start.Empty(); r.end.Empty();
            break;
    }
}

// ---------- owner draw ----------

void CDeployPage::DrawStatusIcon(CDC& dc, const CRect& rc, Status s) const
{
    // Bigger, bolder glyphs. Size scales with the (now taller) row. Geometric
    // pens with round joins/caps keep the thick strokes looking solid.
    const int d = std::min(30, std::min(rc.Width() - 8, rc.Height() - 12));
    const CPoint c(rc.left + rc.Width() / 2, rc.top + rc.Height() / 2);

    switch (s)
    {
        case Status::Open:   // light-blue hollow circle
        {
            LOGBRUSH lb = { BS_SOLID, kOpenBlue, 0 };
            CPen pen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND, 4, &lb);
            CPen* op = dc.SelectObject(&pen);
            CBrush* ob = static_cast<CBrush*>(dc.SelectStockObject(NULL_BRUSH));
            dc.Ellipse(c.x - d / 2, c.y - d / 2, c.x + d / 2, c.y + d / 2);
            dc.SelectObject(ob);
            dc.SelectObject(op);
            break;
        }
        case Status::InUse:  // green check mark
        {
            LOGBRUSH lb = { BS_SOLID, kUseGreen, 0 };
            CPen pen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND, 5, &lb);
            CPen* op = dc.SelectObject(&pen);
            const int h = d / 2;
            dc.MoveTo(c.x - h,       c.y + h / 4);
            dc.LineTo(c.x - h / 4,   c.y + h);
            dc.LineTo(c.x + h,       c.y - h);
            dc.SelectObject(op);
            break;
        }
        case Status::Unavailable:  // red dash
        {
            const int halfH = 4;   // ~8px thick bar
            CRect bar(c.x - d / 2, c.y - halfH, c.x + d / 2, c.y + halfH);
            dc.FillSolidRect(&bar, kUnavailRed);
            break;
        }
    }
}

void CDeployPage::DrawButton(CDC& dc, const CRect& rc, const CString& label,
                             COLORREF base, bool enabled) const
{
    // Center a fixed-height button in the (tall) cell rather than filling it.
    CRect b(rc);
    b.DeflateRect(3, 0);
    const int bh = std::min(28, b.Height() - 8);
    const int cy = b.top + b.Height() / 2;
    b.top    = cy - bh / 2;
    b.bottom = cy + bh / 2;

    const COLORREF fill = enabled ? base : RGB(45, 45, 45);
    const COLORREF text = enabled ? kWhite : RGB(110, 110, 110);
    dc.FillSolidRect(&b, fill);

    CBrush frame(enabled ? RGB(20, 20, 20) : RGB(70, 70, 70));
    dc.FrameRect(&b, &frame);

    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(text);
    dc.DrawText(label, b, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void CDeployPage::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT dis)
{
    if (nIDCtl != IDC_LIST_DEPLOY || !dis || dis->CtlType != ODT_LISTVIEW)
    {
        CHelpAwarePage::OnDrawItem(nIDCtl, dis);
        return;
    }

    const int row = static_cast<int>(dis->itemID);
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    const DeployRow& r = m_rows[row];

    CDC dc;
    dc.Attach(dis->hDC);

    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    dc.FillSolidRect(&dis->rcItem, selected ? kRowSel : kBlack);

    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(kWhite);

    const int top = dis->rcItem.top;
    const int bot = dis->rcItem.bottom;
    int x = dis->rcItem.left;   // scroll-adjusted row origin

    const TCHAR* const btnLabels[kNumBtns] = { _T("Deploy"), _T("Stop"), _T("Go"), _T("Pause"), _T("Release") };
    const COLORREF     btnColors[kNumBtns] = { kBtnDeploy, kBtnStop, kBtnGo, kBtnPause, kBtnRelease };

    for (int col = 0; col < kNumColumns; ++col)
    {
        const int w = m_list.GetColumnWidth(col);
        CRect cell(x, top, x + w, bot);
        x += w;

        if (col == 0)
        {
            DrawStatusIcon(dc, cell, r.status);
        }
        else if (col < kFirstBtnCol)
        {
            CString s;
            switch (col)
            {
                case 1: s = StatusWord(r.status); break;
                case 2: s = r.name;    break;
                case 3: s = r.contact; break;
                case 4: s = r.start;   break;
                case 5: s = r.end;     break;
            }
            CRect tr(cell); tr.left += 6; tr.right -= 4;
            dc.DrawText(s, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        else
        {
            const int bi = col - kFirstBtnCol;  // 0..4 == Btn order
            DrawButton(dc, cell, btnLabels[bi], btnColors[bi],
                       BtnEnabled(r, static_cast<Btn>(bi)));
        }
    }

    dc.Detach();
}

int CDeployPage::ButtonHit(int row, CPoint pt) const
{
    for (int bi = 0; bi < kNumBtns; ++bi)
    {
        CRect rc;
        if (m_list.GetSubItemRect(row, kFirstBtnCol + bi, LVIR_BOUNDS, rc) &&
            rc.PtInRect(pt))
            return bi;
    }
    return -1;
}

void CDeployPage::OnListClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    if (pResult) *pResult = 0;
    const NMITEMACTIVATE* ia = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);

    const CPoint pt = ia->ptAction;
    LVHITTESTINFO ht = { 0 };
    ht.pt = pt;
    const int row = m_list.SubItemHitTest(&ht);
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;

    const int bi = ButtonHit(row, pt);
    if (bi < 0) return;

    const Btn b = static_cast<Btn>(bi);
    if (!BtnEnabled(m_rows[row], b)) return;

    ApplyBtn(m_rows[row], b);
    m_list.RedrawItems(row, row);
    m_list.UpdateWindow();
}
