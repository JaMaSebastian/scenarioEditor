//=============================================================================
//  PlaysPage.cpp
//-----------------------------------------------------------------------------
//  Implements CPlaysPage: builds the accordion of owner-drawn category headers
//  and subcategory buttons from theApp.Plays(), lays them out by hand with
//  scroll support, paints the selected play's name/definition, draws the gear
//  button, and routes header/item clicks (expand/collapse, load/run scenario).
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-10
//=============================================================================
#include "pch.h"
#include "PlaysPage.h"
#include "ScenarioEditor.h"        // theApp.Plays()/MutablePlays()/PlaysPath()
#include "ScenarioEditorDialog.h"  // CScenarioEditorDialog::LoadScenarioForPlay
#include "PlaysEditorDialog.h"     // CPlaysEditorDialog (opened by the gear button)
#include "../log.h"

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

    // Settings gear button, pinned to the top-right corner of the client area
    // (Plays-tab-only — it configures this tab's catalog). Margin keeps it off
    // the top / right edges.
    constexpr int kGearSize   = 96;  // 3x the original 32px
    constexpr int kGearMargin = 12;

    // Encoding-proof expand/collapse indicators. (Unicode triangles rendered as
    // raw source bytes get mangled under the codepage-1252 source charset, which
    // is what produced the earlier "garbage characters" before the name.)
    const wchar_t* const kGlyphExpanded  = L"-";
    const wchar_t* const kGlyphCollapsed = L"+";

    //
    // MakeButton — create a child BUTTON (zero-sized; positioned later in
    //   Relayout) with the given id, caption, and extra style bits.
    //
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
    ON_BN_CLICKED(IDC_BTN_GEAR,              &CPlaysPage::OnEditPlays)
END_MESSAGE_MAP()

//
// CPlaysPage::GetFieldHelpTable — no per-field help on this page (returns empty).
//
void CPlaysPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = nullptr;
    outCount = 0;
}

//
// CPlaysPage::OnInitDialog — create the title font, scrollbar, gear button, and
//   its tooltip, then build the accordion and lay everything out.
//
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

    // Settings gear (top-right of the client area). Owner-drawn so it can paint
    // the PNG icon with alpha; positioned in Relayout.
    LoadGearImage();
    m_gearBtn.Create(_T("Edit Plays"),
                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                     CRect(0, 0, 0, 0), this, IDC_BTN_GEAR);

    if (m_toolTip.Create(this))
    {
        m_toolTip.AddTool(&m_gearBtn,
            _T("Configure Plays\n")
            _T("Add, edit, delete, and reorder the play categories and their ")
            _T("subcategories; edit each subcategory's definition; and associate ")
            _T("a scenario .ini with a subcategory (loaded — and run, if \"Run ")
            _T("Scenario\" is set — when its button is pressed)."));
        m_toolTip.SetMaxTipWidth(360);
        m_toolTip.SetDelayTime(TTDT_AUTOPOP, 20000);
        m_toolTip.Activate(TRUE);
    }

    BuildAccordion();
    Relayout();
    return TRUE;
}

//
// CPlaysPage::PreTranslateMessage — relay messages to the tooltip control so
//   the gear button's hover help works.
//
BOOL CPlaysPage::PreTranslateMessage(MSG* pMsg)
{
    if (m_toolTip.GetSafeHwnd())
        m_toolTip.RelayEvent(pMsg);
    return CHelpAwarePage::PreTranslateMessage(pMsg);
}

//
// CPlaysPage::LoadGearImage — decode the embedded gear PNG (RCDATA) into
//   m_gearImg via a memory stream; logs and leaves the image null on failure.
//
void CPlaysPage::LoadGearImage()
{
    // Decode the gear PNG from the embedded RCDATA resource into a CImage
    // (CImage auto-initializes GDI+ and keeps the alpha channel).
    HINSTANCE hInst = AfxGetResourceHandle();
    HRSRC hRes = ::FindResource(hInst, MAKEINTRESOURCE(IDB_GEAR_PNG), RT_RCDATA);
    if (!hRes) { LOG("PlaysPage::LoadGearImage: gear resource not found"); return; }

    const DWORD size = ::SizeofResource(hInst, hRes);
    HGLOBAL hResData = ::LoadResource(hInst, hRes);
    const void* pData = hResData ? ::LockResource(hResData) : nullptr;
    if (!pData || size == 0) { LOG("PlaysPage::LoadGearImage: lock failed"); return; }

    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return;
    if (void* pMem = ::GlobalLock(hMem))
    {
        memcpy(pMem, pData, size);
        ::GlobalUnlock(hMem);

        IStream* pStream = nullptr;
        if (SUCCEEDED(::CreateStreamOnHGlobal(hMem, TRUE /*own+free*/, &pStream)))
        {
            if (FAILED(m_gearImg.Load(pStream)))
                LOG("PlaysPage::LoadGearImage: CImage::Load failed");
            pStream->Release();
            return;
        }
    }
    ::GlobalFree(hMem);
}

//
// CPlaysPage::OnEditPlays — gear button: open the modal Plays editor, then
//   reload the accordion from the (possibly edited) catalog.
//
void CPlaysPage::OnEditPlays()
{
    // Modal editor operates on a deep copy of theApp's Plays catalog and
    // commits on Save; rebuild the accordion regardless of how it closed.
    CPlaysEditorDialog dlg(&theApp.MutablePlays(), theApp.PlaysPath(), this);
    dlg.DoModal();
    ReloadCatalog();
}

//
// CPlaysPage::BuildAccordion — mirror theApp.Plays() into m_categories and
//   create the owner-drawn header + push-button item windows (clamped to the
//   IDC_PLAYS_HEADER_*/ITEM_* id ranges).
//
void CPlaysPage::BuildAccordion()
{
    // The play catalog is data-driven from theApp.Plays() (persisted to
    // plays.ini, edited via the gear-button editor). Layout, command routing,
    // and the client-area blurb all pick these up automatically. The
    // IDC_PLAYS_HEADER_* / IDC_PLAYS_ITEM_* id ranges cap us at 10 categories
    // and 40 subcategories total; anything beyond is dropped (the editor
    // enforces the same caps, so this is just defensive).
    m_categories.clear();

    const PlaysCatalog& plays = theApp.Plays();
    for (const PlayCategory& src : plays.categories)
    {
        if (m_categories.size() >= (IDC_PLAYS_HEADER_LAST - IDC_PLAYS_HEADER_FIRST + 1))
            break;
        Category cat;
        cat.name = src.name;
        for (const PlaySub& sub : src.subs)
            cat.items.push_back({ sub.name, sub.blurb, sub.scenarioPath });
        m_categories.push_back(std::move(cat));
    }

    const HWND parent = GetSafeHwnd();
    HFONT font = static_cast<HFONT>(GetFont() ? GetFont()->GetSafeHandle() : nullptr);

    UINT headerId = IDC_PLAYS_HEADER_FIRST;
    UINT itemId   = IDC_PLAYS_ITEM_FIRST;

    for (Category& cat : m_categories)
    {
        if (itemId > IDC_PLAYS_ITEM_LAST) break;
        // Header is owner-drawn so it can paint a black background with white
        // text (standard push buttons don't allow recoloring).
        cat.headerId = headerId++;
        HWND h = MakeButton(parent, cat.headerId, cat.name, BS_OWNERDRAW);
        if (font) ::SendMessage(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        UpdateHeaderText(cat);

        for (const Play& item : cat.items)
        {
            if (itemId > IDC_PLAYS_ITEM_LAST) break;
            const UINT id = itemId++;
            cat.itemIds.push_back(id);
            HWND ih = MakeButton(parent, id, item.name, BS_PUSHBUTTON | BS_LEFT);
            if (font) ::SendMessage(ih, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }
}

//
// CPlaysPage::ReloadCatalog — destroy the existing accordion button windows,
//   clear the selection blurb, then rebuild and relayout; called after the
//   Plays editor closes.
//
void CPlaysPage::ReloadCatalog()
{
    // Destroy the currently-created header / item button windows, then rebuild
    // the accordion from the (possibly edited) catalog. Called after the Plays
    // editor closes.
    for (const Category& cat : m_categories)
    {
        if (CWnd* h = GetDlgItem(cat.headerId)) h->DestroyWindow();
        for (UINT id : cat.itemIds)
            if (CWnd* it = GetDlgItem(id)) it->DestroyWindow();
    }
    m_categories.clear();

    // The selection ids are gone; clear the client-area blurb too.
    m_selName.Empty();
    m_selBlurb.Empty();

    BuildAccordion();
    Relayout();
    Invalidate();
}

//
// CPlaysPage::UpdateHeaderText — set a category header's caption with the
//   expand/collapse glyph reflecting its current state.
//
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

//
// CPlaysPage::FindPlay — locate the Play backing a subcategory button id, or
//   null if none matches.
//
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

//
// CPlaysPage::Relayout — position every accordion button (sized to its text,
//   honoring collapse and the scroll offset), size/show the scrollbar when the
//   content overflows, and pin the gear button + blurb region. Safe to call
//   before the accordion/scrollbar exist.
//
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

    // Pin the settings gear to the top-right corner of the client area, with a
    // margin off the top and right edges; keep it above the accordion buttons
    // (highest z-order) and push the blurb text below it.
    if (::IsWindow(m_gearBtn.GetSafeHwnd()))
    {
        const int gx = rc.right - kGearSize - kGearMargin;
        const int gy = kGearMargin;
        m_gearBtn.SetWindowPos(&CWnd::wndTop, gx > 0 ? gx : kGearMargin, gy,
                               kGearSize, kGearSize, SWP_NOACTIVATE);
        m_blurbTop = gy + kGearSize + 12;
    }
    else
    {
        m_blurbTop = kBlurbTop;
    }
}

//
// CPlaysPage::BlurbRect — client-area rectangle (right of the accordion +
//   scrollbar) where the selected play's name/definition is drawn.
//
CRect CPlaysPage::BlurbRect() const
{
    CRect rc;
    GetClientRect(&rc);
    return CRect(m_blurbLeft, m_blurbTop, rc.right - kBlurbGap, rc.bottom - kBlurbTop);
}

//
// CPlaysPage::OnPaint — draw the selected play's name (bold) and its
//   plain-English definition, centered in the blurb region.
//
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

//
// CPlaysPage::OnSize — relayout and repaint when the page is resized.
//
void CPlaysPage::OnSize(UINT nType, int cx, int cy)
{
    CHelpAwarePage::OnSize(nType, cx, cy);
    Relayout();
    Invalidate();   // blurb region moved / resized — repaint it
}

//
// CPlaysPage::OnVScroll — handle the accordion scrollbar (line/page/thumb),
//   clamp the offset, and relayout; defers other scrollbars to the base.
//
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

//
// CPlaysPage::OnMouseWheel — scroll the accordion by the wheel delta when the
//   content overflows; otherwise passes through to the base.
//
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

//
// CPlaysPage::OnDrawItem — owner-draw the gear button (transparent PNG, with a
//   pressed state) and the category headers (black background, white
//   left-aligned text); other controls fall through to the base.
//
void CPlaysPage::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT dis)
{
    // Settings gear: transparent PNG over the page background.
    if (dis && dis->CtlType == ODT_BUTTON && nIDCtl == IDC_BTN_GEAR)
    {
        CDC dc;
        dc.Attach(dis->hDC);
        const CRect rc(dis->rcItem);
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

        // Fill with the page background so the icon's transparent areas blend in.
        dc.FillSolidRect(rc, ::GetSysColor(COLOR_3DFACE));
        if (pressed)
            dc.DrawEdge(const_cast<LPRECT>(&dis->rcItem), BDR_SUNKENOUTER, BF_RECT);

        if (!m_gearImg.IsNull())
        {
            CRect dst(rc);
            dst.DeflateRect(2, 2);
            if (pressed) dst.OffsetRect(1, 1);
            const int oldMode = dc.SetStretchBltMode(HALFTONE);
            m_gearImg.Draw(dc.GetSafeHdc(), dst);   // alpha-blended for 32bpp PNG
            dc.SetStretchBltMode(oldMode);
        }
        else
        {
            dc.SetBkMode(TRANSPARENT);
            dc.DrawText(_T("Plays"), const_cast<LPRECT>(&dis->rcItem),
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        dc.Detach();
        return;
    }

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

//
// CPlaysPage::OnHeaderClicked — toggle the clicked category's expanded state,
//   refresh its glyph, and relayout.
//
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

//
// CPlaysPage::OnItemClicked — show the clicked subcategory's name/definition in
//   the blurb pane and, if it has an associated scenario .ini, load (and
//   optionally run) it via the main dialog.
//
void CPlaysPage::OnItemClicked(UINT nID)
{
    const Play* play = FindPlay(nID);
    if (!play) return;

    m_selName  = play->name;
    m_selBlurb = play->blurb;
    InvalidateRect(BlurbRect(), TRUE);   // repaint just the client-area text

    // If this subcategory has an associated scenario .ini, load it (and run it
    // when the editor's "Run Scenario" option is set).
    if (m_main && !play->scenarioPath.IsEmpty())
        m_main->LoadScenarioForPlay(play->scenarioPath, theApp.Plays().runScenario);
}
