//=============================================================================
//  HelpAwarePage.cpp
//-----------------------------------------------------------------------------
//  Implements CHelpAwarePage: creates the tooltip control, relays mouse
//  messages to it so tooltips appear, toggles help on/off, and registers a
//  tooltip for each control listed in the subclass's FFieldHelp table.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "HelpAwarePage.h"

//
// OnInitDialog — create the tooltip control (wide, no prefix), start it
//   deactivated, and register the field help entries.
//
BOOL CHelpAwarePage::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    if (m_helpTooltip.Create(this, TTS_ALWAYSTIP | TTS_NOPREFIX))
    {
        m_helpTooltip.SetMaxTipWidth(360);
        m_helpTooltip.Activate(FALSE);
        m_tooltipReady = true;
        RegisterHelpEntries();
    }

    return TRUE;
}

//
// PreTranslateMessage — forward messages to the tooltip control so it can show
//   tips, then let the base class handle them normally.
//
BOOL CHelpAwarePage::PreTranslateMessage(MSG* pMsg)
{
    if (m_tooltipReady)
        m_helpTooltip.RelayEvent(pMsg);
    return CDialogEx::PreTranslateMessage(pMsg);
}

//
// SetHelpActive — activate or deactivate the tooltip control (no-op until ready).
//
void CHelpAwarePage::SetHelpActive(bool active)
{
    if (m_tooltipReady)
        m_helpTooltip.Activate(active ? TRUE : FALSE);
}

//
// RegisterHelpEntries — pull the subclass's help table and add a tooltip for
//   each entry whose control exists, appending "(Required)" to required fields.
//
void CHelpAwarePage::RegisterHelpEntries()
{
    const FFieldHelp* table = nullptr;
    size_t count = 0;
    GetFieldHelpTable(table, count);
    if (!table || count == 0)
        return;

    for (size_t i = 0; i < count; ++i)
    {
        const FFieldHelp& f = table[i];
        CWnd* pCtrl = GetDlgItem(f.controlId);
        if (!pCtrl)
            continue;

        CString text(f.description);
        if (f.required)
            text += _T("  (Required)");

        m_helpTooltip.AddTool(pCtrl, text);
    }
}
