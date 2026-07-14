//=============================================================================
//  AttributesDialog.h
//-----------------------------------------------------------------------------
//  Declares CAttributesDialog, the modal notebook that hosts the three
//  scenario-authoring pages (Scenario Setup, Asset / Entity Editor, Motion
//  Path Editor) in a tab control. Edits the shared Scenario in place and
//  reports whether any hosted page marked the document dirty.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-10
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "ScenarioSetupPage.h"
#include "AssetEntityEditorPage.h"
#include "MotionPathEditorPage.h"

struct Scenario;
class  EntityTypeCatalog;

// Modal notebook that hosts the three scenario-authoring pages (Scenario
// Setup, Asset / Entity Editor, Motion Path Editor). These used to live as
// tabs on the main window; they were moved here so the main window can focus
// on Output / Playback + Preview. Launched from the Output / Playback page's
// "Attributes..." button (see CScenarioEditorDialog::OnOpenAttributes).
//
// The dialog edits the shared Scenario in place. Pending control edits are
// flushed on close (Close / OK); the owner then refreshes Preview + the
// validation panes and marks the document dirty if WasModified() is true.
class CAttributesDialog : public CDialogEx
{
public:
    enum { IDD = IDD_ATTRIBUTES_DIALOG };

    //
    // CAttributesDialog — construct over the shared scenario + catalog; carry
    // the toolbar help state and the tab index to open on.
    //
    CAttributesDialog(Scenario* scenario,
                      const EntityTypeCatalog* catalog,
                      bool helpActive,
                      int  startTab,
                      CWnd* pParent = nullptr);

    // True if any hosted page reported an edit (WM_APP_MARK_DIRTY).
    bool WasModified() const { return m_modified; }

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    //
    // OnInitDialog — insert the three tabs, create + populate the hosted pages
    // from the scenario, and reveal the requested start tab.
    //
    BOOL OnInitDialog() override;
    //
    // PreTranslateMessage — route keyboard accelerators to the dialog's
    // command handlers.
    //
    BOOL PreTranslateMessage(MSG* pMsg) override;
    //
    // OnOK — flush in-progress edits into the scenario, then close (Close/OK).
    //
    void OnOK() override;
    //
    // OnCancel — Escape also flushes edits (pages mutate the scenario in place,
    // so there is nothing to discard).
    //
    void OnCancel() override;

    //
    // OnSize — re-lay out the tab control, Close button, and active page.
    //
    afx_msg void OnSize(UINT nType, int cx, int cy);
    //
    // OnTabSelChange — show the page for the newly selected tab.
    //
    afx_msg void OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult);
    //
    // OnMarkDirtyMessage — WM_APP_MARK_DIRTY handler; sets m_modified unless
    // dirty tracking is suppressed during initial population.
    //
    afx_msg LRESULT OnMarkDirtyMessage(WPARAM wParam, LPARAM lParam);
    //
    // OnKb* — keyboard-shortcut handlers: switch to the relevant page and
    // synthesize the corresponding toolbar button click.
    //
    afx_msg void OnKbAddAsset();
    afx_msg void OnKbDeleteAsset();
    afx_msg void OnKbDuplicateAsset();
    afx_msg void OnKbMoveAsset();
    afx_msg void OnKbAddSegment();
    afx_msg void OnKbDeleteSegment();
    afx_msg void OnKbDuplicateSegment();

    DECLARE_MESSAGE_MAP()

private:
    //
    // CreatePages — create the three hosted pages (hidden), wiring each to the
    // shared scenario and catalog.
    //
    void CreatePages();
    //
    // LayoutChildren — size the tab control, pin the Close button, and fit the
    // active page to the tab's content area.
    //
    void LayoutChildren();
    //
    // ShowPage — hide the current page and show the one at `index`, resized to
    // the tab content area, and sync the tab selection.
    //
    void ShowPage(int index);
    //
    // CaptureIntoScenario — flush all three pages' in-progress edits back into
    // the shared scenario.
    //
    void CaptureIntoScenario();

    CTabCtrl                m_tabCtrl;
    CScenarioSetupPage      m_pageSetup;
    CAssetEntityEditorPage  m_pageAssets;
    CMotionPathEditorPage   m_pageMotion;
    CDialogEx*              m_pages[3] = { nullptr };
    int                     m_activePage = -1;

    Scenario*                m_scenario  = nullptr;
    const EntityTypeCatalog*  m_catalog  = nullptr;
    bool                     m_helpActive = false;
    int                      m_startTab   = 0;
    bool                     m_modified   = false;
    // Gates the WM_APP_MARK_DIRTY handler off while the pages are being
    // populated in OnInitDialog (ReadFrom fires EN_CHANGE on every edit).
    bool                     m_suppressDirty = false;
    HACCEL                   m_hAccel     = nullptr;
};
