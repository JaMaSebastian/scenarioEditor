#include "pch.h"
#include "PlaysPage.h"

#include <algorithm>   // std::max (NOMINMAX is set project-wide)

namespace
{
    // Layout metrics (pixels). The page lays its children out by hand, matching
    // the pixel-based layout the rest of the app uses. The accordion fills the
    // tab from the top-left (no outer margin); each category header is only as
    // wide as its text plus a left/right pad, and 3x a normal button's height.
    constexpr int kOuterX    = 0;
    constexpr int kOuterY    = 0;
    constexpr int kHeaderH   = 78;   // 3x a normal ~26px button
    constexpr int kItemH     = 72;   // 3x a normal ~24px button
    constexpr int kGap       = 4;
    constexpr int kPanelGap  = 10;   // extra space between category panels
    constexpr int kIndent    = 24;
    constexpr int kHeaderPad = 24;   // text margin inside a header button (each side)
    constexpr int kItemPad   = 16;   // text margin inside an item button (each side)
    constexpr int kColGap    = 6;    // gap between accordion and scrollbar
    constexpr int kBlurbGap  = 16;   // gap between scrollbar and blurb region
    constexpr int kBlurbTop  = 14;   // top inset of the blurb text

    // Encoding-proof expand/collapse indicators. (Unicode triangles rendered as
    // raw source bytes get mangled under the codepage-1252 source charset, which
    // is what produced the earlier "garbage characters" before the name.)
    const wchar_t* const kGlyphExpanded  = L"-";
    const wchar_t* const kGlyphCollapsed = L"+";

    HWND MakeButton(HWND parent, UINT id, const CString& text, DWORD extraStyle)
    {
        return ::CreateWindowEx(
            0, _T("BUTTON"), text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
            0, 0, 0, 0,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
            AfxGetInstanceHandle(), nullptr);
    }
}

BEGIN_MESSAGE_MAP(CPlaysPage, CHelpAwarePage)
    ON_WM_SIZE()
    ON_WM_PAINT()
    ON_WM_VSCROLL()
    ON_WM_MOUSEWHEEL()
    ON_WM_DRAWITEM()
    ON_COMMAND_RANGE(IDC_PLAYS_HEADER_FIRST, IDC_PLAYS_HEADER_LAST, &CPlaysPage::OnHeaderClicked)
    ON_COMMAND_RANGE(IDC_PLAYS_ITEM_FIRST,   IDC_PLAYS_ITEM_LAST,   &CPlaysPage::OnItemClicked)
END_MESSAGE_MAP()

void CPlaysPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = nullptr;
    outCount = 0;
}

BOOL CPlaysPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    // Bold, slightly larger font for the selected play's name in the client area.
    LOGFONT lf = { 0 };
    if (GetFont()) GetFont()->GetLogFont(&lf);
    LOGFONT lft = lf;
    lft.lfWeight = FW_BOLD;
    lft.lfHeight = static_cast<LONG>(lf.lfHeight * 1.25);
    m_titleFont.CreateFontIndirect(&lft);

    m_scroll.Create(WS_CHILD | SBS_VERT, CRect(0, 0, 0, 0), this, IDC_PLAYS_SCROLLBAR);

    BuildAccordion();
    Relayout();
    return TRUE;
}

void CPlaysPage::BuildAccordion()
{
    // The play catalog: category -> (play name, plain-English meaning). Add
    // categories / plays here — layout, command routing, and the client-area
    // blurb all pick them up automatically.
    m_categories.clear();

    m_categories.push_back({ _T("Defensive & Security"), {
        { _T("Area Defense"),
          _T("Hold terrain and deny the enemy access to a specific area.") },
        { _T("Mobile Defense"),
          _T("Allow or shape the enemy's movement, then defeat them with a decisive counterattack.") },
        { _T("Retrograde"),
          _T("Move away from the enemy in an organized way. This can include delay, withdrawal, or retirement.") },
        { _T("Screen"),
          _T("Provides early warning. It observes, reports, and may harass, but avoids becoming decisively engaged.") },
        { _T("Guard"),
          _T("Protects the main force by fighting to gain time, prevent surprise, and stop enemy observation/fire against the main body.") },
        { _T("Cover"),
          _T("A stronger, more independent security force that operates farther away and can fight to develop the situation before the enemy reaches the main body.") },
        { _T("Area Security"),
          _T("Protects a specific area, route, facility, population, or activity.") },
    } });

    m_categories.push_back({ _T("Offensive"), {
        { _T("Movement to Contact"),
          _T("Move forward to find and make contact with the enemy when the enemy situation is unclear.") },
        { _T("Attack"),
          _T("Strike the enemy to destroy/defeat forces or seize terrain.") },
        { _T("Exploitation"),
          _T("Follow up a successful attack to break the enemy deeper and prevent recovery.") },
        { _T("Pursuit"),
          _T("Chase and destroy or cut off an enemy force that is trying to escape.") },
    } });

    m_categories.push_back({ _T("Intelligence, Surveillance & Reconnaissance (ISR)"), {
        { _T("Route Reconnaissance"),
          _T("Examine a specific road, trail, waterway, bridge route, or movement path. Used to answer: Can we move through here? Is it blocked? Is it defended?") },
        { _T("Zone Reconnaissance"),
          _T("Search an entire zone or corridor from one boundary to another. Used when the commander needs a broad picture of terrain, enemy, obstacles, and routes.") },
        { _T("Area Reconnaissance"),
          _T("Examine a specific place, such as a town, bridge, hill, airfield, landing zone, port, or suspected enemy position.") },
        { _T("Reconnaissance in Force"),
          _T("A stronger combat operation meant to make the enemy react, revealing their strength, location, weapons, or intentions. This can involve fighting.") },
        { _T("Special Reconnaissance"),
          _T("Reconnaissance by special operations forces, often deep, covert, or politically sensitive. Used where normal forces may not be able to go.") },
        { _T("Surveillance"),
          _T("More passive or continuous observation over time.") },
        { _T("Screening"),
          _T("Watching an area to give early warning and protect friendly forces.") },
        { _T("Scouting"),
          _T("Informal or small-unit term for reconnaissance.") },
        { _T("Patrol"),
          _T("A unit sent out to gather information, secure an area, or fight if needed.") },
    } });

    const HWND parent = GetSafeHwnd();
    HFONT font = static_cast<HFONT>(GetFont() ? GetFont()->GetSafeHandle() : nullptr);

    UINT headerId = IDC_PLAYS_HEADER_FIRST;
    UINT itemId   = IDC_PLAYS_ITEM_FIRST;

    for (Category& cat : m_categories)
    {
        // Header is owner-drawn so it can paint a black background with white
        // text (standard push buttons don't allow recoloring).
        cat.headerId = headerId++;
        HWND h = MakeButton(parent, cat.headerId, cat.name, BS_OWNERDRAW);
        if (font) ::SendMessage(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        UpdateHeaderText(cat);

        for (const Play& item : cat.items)
        {
            const UINT id = itemId++;
            cat.itemIds.push_back(id);
            HWND ih = MakeButton(parent, id, item.name, BS_PUSHBUTTON | BS_LEFT);
            if (font) ::SendMessage(ih, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }
}

void CPlaysPage::UpdateHeaderText(const Category& cat)
{
    if (CWnd* h = GetDlgItem(cat.headerId))
    {
        CString text;
        text.Format(_T("%s  %s"),
                    cat.expanded ? kGlyphExpanded : kGlyphCollapsed,
                    cat.name.GetString());
        h->SetWindowText(text);
    }
}

const CPlaysPage::Play* CPlaysPage::FindPlay(UINT itemId) const
{
    for (const Category& cat : m_categories)
    {
        for (size_t i = 0; i < cat.itemIds.size(); ++i)
            if (cat.itemIds[i] == itemId)
                return &cat.items[i];
    }
    return nullptr;
}

void CPlaysPage::Relayout()
{
    // WM_SIZE is dispatched during dialog creation, before OnInitDialog has
    // built the accordion or created the scrollbar — bail until those exist so
    // we don't SetWindowPos / ShowWindow an uncreated control (asserts in MFC).
    if (!::IsWindow(GetSafeHwnd()) || !::IsWindow(m_scroll.GetSafeHwnd())) return;

    // Measure header / item text against the dialog font so each button is only
    // as wide as its label plus an internal pad.
    CClientDC dc(this);
    CFont* oldFont = dc.SelectObject(GetFont());

    CRect rc;
    GetClientRect(&rc);
    m_viewportH = rc.Height();

    // Pass 1: compute each button's unscrolled slot, the widest right edge
    // (over ALL buttons, so the column width stays stable as panels collapse),
    // and the total content height (only visible rows count).
    struct Slot { CWnd* w; int x, y, cw, ch; bool show; };
    std::vector<Slot> slots;
    int maxRight = 0;
    int y = kOuterY;

    for (const Category& cat : m_categories)
    {
        if (CWnd* h = GetDlgItem(cat.headerId))
        {
            CString text; h->GetWindowText(text);
            const int w = dc.GetTextExtent(text).cx + 2 * kHeaderPad;
            maxRight = std::max(maxRight, kOuterX + w);
            slots.push_back({ h, kOuterX, y, w, kHeaderH, true });
        }
        y += kHeaderH + kGap;

        for (UINT id : cat.itemIds)
        {
            CWnd* item = GetDlgItem(id);
            if (!item) continue;
            CString text; item->GetWindowText(text);
            const int w = dc.GetTextExtent(text).cx + 2 * kItemPad;
            maxRight = std::max(maxRight, kOuterX + kIndent + w);
            if (cat.expanded)
            {
                slots.push_back({ item, kOuterX + kIndent, y, w, kItemH, true });
                y += kItemH + kGap;
            }
            else
            {
                slots.push_back({ item, 0, 0, 0, 0, false });
            }
        }
        y += kPanelGap;
    }

    dc.SelectObject(oldFont);

    const int contentH = y;
    m_maxScroll = std::max(0, contentH - m_viewportH);
    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
    if (m_scrollY < 0)           m_scrollY = 0;

    // Pass 2: apply positions (offset by the scroll amount; the page edges clip
    // rows that scroll above the top / below the bottom).
    for (const Slot& s : slots)
    {
        if (!s.show) { s.w->ShowWindow(SW_HIDE); continue; }
        s.w->SetWindowPos(nullptr, s.x, s.y - m_scrollY, s.cw, s.ch,
                          SWP_NOZORDER | SWP_NOACTIVATE);
        s.w->ShowWindow(SW_SHOW);
    }

    // Scrollbar: just right of the accordion column, only when content overflows.
    const int vsw = ::GetSystemMetrics(SM_CXVSCROLL);
    const int sbx = maxRight + kColGap;
    if (m_maxScroll > 0)
    {
        m_scroll.MoveWindow(sbx, 0, vsw, m_viewportH);
        SCROLLINFO si = { sizeof(si) };
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin  = 0;
        si.nMax  = contentH - 1;
        si.nPage = m_viewportH;
        si.nPos  = m_scrollY;
        m_scroll.SetScrollInfo(&si, TRUE);
        m_scroll.ShowWindow(SW_SHOW);
    }
    else
    {
        m_scroll.ShowWindow(SW_HIDE);
    }

    m_blurbLeft = maxRight + kColGap + vsw + kBlurbGap;
}

CRect CPlaysPage::BlurbRect() const
{
    CRect rc;
    GetClientRect(&rc);
    return CRect(m_blurbLeft, kBlurbTop, rc.right - kBlurbGap, rc.bottom - kBlurbTop);
}

void CPlaysPage::OnPaint()
{
    CPaintDC dc(this);
    if (m_selName.IsEmpty()) return;

    CRect area = BlurbRect();
    if (area.Width() < 60) return;

    dc.SetBkMode(TRANSPARENT);

    // Title (bold), centered near the top of the client area.
    CFont* oldFont = dc.SelectObject(&m_titleFont);
    CRect calc(area);
    dc.DrawText(m_selName, calc, DT_WORDBREAK | DT_CENTER | DT_CALCRECT);
    CRect titleRc(area.left, area.top, area.right, area.top + calc.Height());
    dc.DrawText(m_selName, titleRc, DT_CENTER | DT_TOP | DT_WORDBREAK);
    dc.SelectObject(oldFont);

    // Plain-English meaning below the title.
    CRect textRc(area.left, titleRc.bottom + 10, area.right, area.bottom);
    dc.SelectObject(GetFont());
    dc.DrawText(m_selBlurb, textRc, DT_CENTER | DT_TOP | DT_WORDBREAK);
}

void CPlaysPage::OnSize(UINT nType, int cx, int cy)
{
    CHelpAwarePage::OnSize(nType, cx, cy);
    Relayout();
    Invalidate();   // blurb region moved / resized — repaint it
}

void CPlaysPage::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
    if (pScrollBar == &m_scroll)
    {
        int pos = m_scrollY;
        switch (nSBCode)
        {
            case SB_LINEUP:        pos -= 24; break;
            case SB_LINEDOWN:      pos += 24; break;
            case SB_PAGEUP:        pos -= m_viewportH; break;
            case SB_PAGEDOWN:      pos += m_viewportH; break;
            case SB_TOP:           pos = 0; break;
            case SB_BOTTOM:        pos = m_maxScroll; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = static_cast<int>(nPos); break;
            default: break;
        }
        if (pos < 0)           pos = 0;
        if (pos > m_maxScroll) pos = m_maxScroll;
        if (pos != m_scrollY)
        {
            m_scrollY = pos;
            Relayout();
        }
        return;
    }
    CHelpAwarePage::OnVScroll(nSBCode, nPos, pScrollBar);
}

BOOL CPlaysPage::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
    if (m_maxScroll > 0)
    {
        int pos = m_scrollY - (zDelta / WHEEL_DELTA) * 48;
        if (pos < 0)           pos = 0;
        if (pos > m_maxScroll) pos = m_maxScroll;
        if (pos != m_scrollY)
        {
            m_scrollY = pos;
            Relayout();
        }
        return TRUE;
    }
    return CHelpAwarePage::OnMouseWheel(nFlags, zDelta, pt);
}

void CPlaysPage::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT dis)
{
    // Header buttons: black background, white left-aligned text.
    if (dis && dis->CtlType == ODT_BUTTON &&
        nIDCtl >= IDC_PLAYS_HEADER_FIRST && nIDCtl <= IDC_PLAYS_HEADER_LAST)
    {
        CDC dc;
        dc.Attach(dis->hDC);
        const CRect rc(dis->rcItem);
        dc.FillSolidRect(rc, RGB(0, 0, 0));                 // black button

        CFont* oldFont = dc.SelectObject(GetFont());
        const int oldBk = dc.SetBkMode(TRANSPARENT);
        const COLORREF oldTx = dc.SetTextColor(RGB(255, 255, 255));  // white text

        TCHAR buf[128] = { 0 };
        ::GetWindowText(dis->hwndItem, buf, _countof(buf));
        CRect tr(rc);
        tr.left += kHeaderPad;
        dc.DrawText(buf, -1, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        dc.SetTextColor(oldTx);
        dc.SetBkMode(oldBk);
        dc.SelectObject(oldFont);
        dc.Detach();
        return;
    }
    CHelpAwarePage::OnDrawItem(nIDCtl, dis);
}

void CPlaysPage::OnHeaderClicked(UINT nID)
{
    for (Category& cat : m_categories)
    {
        if (cat.headerId == nID)
        {
            cat.expanded = !cat.expanded;
            UpdateHeaderText(cat);
            Relayout();
            break;
        }
    }
}

void CPlaysPage::OnItemClicked(UINT nID)
{
    if (const Play* play = FindPlay(nID))
    {
        m_selName  = play->name;
        m_selBlurb = play->blurb;
        InvalidateRect(BlurbRect(), TRUE);   // repaint just the client-area text
    }
}
