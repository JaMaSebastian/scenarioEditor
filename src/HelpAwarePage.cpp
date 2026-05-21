#include "pch.h"
#include "HelpAwarePage.h"

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

BOOL CHelpAwarePage::PreTranslateMessage(MSG* pMsg)
{
    if (m_tooltipReady)
        m_helpTooltip.RelayEvent(pMsg);
    return CDialogEx::PreTranslateMessage(pMsg);
}

void CHelpAwarePage::SetHelpActive(bool active)
{
    if (m_tooltipReady)
        m_helpTooltip.Activate(active ? TRUE : FALSE);
}

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
