#pragma once

#include "pch.h"

struct FFieldHelp
{
    UINT         controlId;
    const TCHAR* description;
    bool         required;
};

class CHelpAwarePage : public CDialogEx
{
public:
    CHelpAwarePage(UINT idd, CWnd* pParent) : CDialogEx(idd, pParent) {}

    void SetHelpActive(bool active);

protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* pMsg) override;

    virtual void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const = 0;

private:
    void RegisterHelpEntries();

    CToolTipCtrl m_helpTooltip;
    bool         m_tooltipReady = false;
};
