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

    CAttributesDialog(Scenario* scenario,
                      const EntityTypeCatalog* catalog,
                      bool helpActive,
                      int  startTab,
                      CWnd* pParent = nullptr);

    // True if any hosted page reported an edit (WM_APP_MARK_DIRTY).
    bool WasModified() const { return m_modified; }

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* pMsg) override;
    void OnOK() override;
    void OnCancel() override;

    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg LRESULT OnMarkDirtyMessage(WPARAM wParam, LPARAM lParam);
    afx_msg void OnKbAddAsset();
    afx_msg void OnKbDeleteAsset();
    afx_msg void OnKbDuplicateAsset();
    afx_msg void OnKbMoveAsset();
    afx_msg void OnKbAddSegment();
    afx_msg void OnKbDeleteSegment();
    afx_msg void OnKbDuplicateSegment();

    DECLARE_MESSAGE_MAP()

private:
    void CreatePages();
    void LayoutChildren();
    void ShowPage(int index);
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
