//=============================================================================
//  HelpAwarePage.h
//-----------------------------------------------------------------------------
//  Declares CHelpAwarePage, a CDialogEx base class that turns a per-field help
//  table into hover tooltips on the dialog's controls, plus the FFieldHelp
//  descriptor struct that subclasses supply.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include "pch.h"

//-----------------------------------------------------------------------------
// FFieldHelp — one control's help entry
//   Maps a dialog control ID to its tooltip description and whether the field
//   is required (which appends a "(Required)" suffix).
//-----------------------------------------------------------------------------
struct FFieldHelp
{
    UINT         controlId;
    const TCHAR* description;
    bool         required;
};

//-----------------------------------------------------------------------------
// CHelpAwarePage — dialog base with field-level tooltip help
//   Builds a CToolTipCtrl from the subclass-provided FFieldHelp table on init
//   and can toggle those tooltips on/off; subclasses implement GetFieldHelpTable.
//-----------------------------------------------------------------------------
class CHelpAwarePage : public CDialogEx
{
public:
    CHelpAwarePage(UINT idd, CWnd* pParent) : CDialogEx(idd, pParent) {}

    // Enable/disable the field tooltips at runtime.
    void SetHelpActive(bool active);

protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* pMsg) override;

    // Subclass hook: hand back the static help table (array + element count).
    virtual void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const = 0;

private:
    // Add a tooltip tool for each help-table entry whose control exists.
    void RegisterHelpEntries();

    CToolTipCtrl m_helpTooltip;
    bool         m_tooltipReady = false;
};
