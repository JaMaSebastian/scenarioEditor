#pragma once

#include "pch.h"
#include "Resource.h"
#include "Scenario.h"
#include "ScenarioWorker.h"
#include "UdpSender.h"
#include "ScenarioSetupPage.h"
#include "AssetEntityEditorPage.h"
#include "MotionPathEditorPage.h"
#include "OutputPlaybackPage.h"
#include "PreviewPage.h"

class CScenarioEditorDialog : public CDialogEx
{
public:
    enum { IDD = IDD_SCENARIO_EDITOR_DIALOG };

    CScenarioEditorDialog(CWnd* pParent = nullptr);

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* pMsg) override;

    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnPlaceholderCommand(UINT nID);
    afx_msg void OnFileExit();
    afx_msg void OnHelpAbout();
    afx_msg void OnLoopToggle();
    afx_msg void OnUpdateLoopToggle(CCmdUI* pCmdUI);
    afx_msg void OnHelpToggle();
    afx_msg void OnUpdateHelpToggle(CCmdUI* pCmdUI);
    afx_msg void OnPlaybackStart();
    afx_msg void OnPlaybackPause();
    afx_msg void OnPlaybackResume();
    afx_msg void OnPlaybackStop();
    afx_msg void OnReplayFile();
    afx_msg void OnOpenRecording();
    afx_msg LRESULT OnPlaybackStatusMessage(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnPlaybackProgressMessage(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnPlaybackErrorMessage(WPARAM wParam, LPARAM lParam);
    afx_msg void OnDestroy();
    afx_msg void OnFileNew();
    afx_msg void OnFileOpen();
    afx_msg void OnFileSave();
    afx_msg void OnFileSaveAs();
    afx_msg void OnScenarioValidate();
    afx_msg LRESULT OnMarkDirtyMessage(WPARAM wParam, LPARAM lParam);
    afx_msg void OnClose();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnKbAddAsset();
    afx_msg void OnKbDeleteAsset();
    afx_msg void OnKbDuplicateAsset();
    afx_msg void OnKbMoveAsset();
    afx_msg void OnKbAddSegment();
    afx_msg void OnKbDeleteSegment();
    afx_msg void OnKbDuplicateSegment();
    void OnCancel() override;

    DECLARE_MESSAGE_MAP()

private:
    void CreatePages();
    void CreateToolBar();
    void LayoutChildren();
    void ShowPage(int index);
    void SeedStatusBar();
    void PropagateHelpToggle();
    void UpdateStatusPduCount();
    void RefreshUiFromScenario();
    void UpdateTitle();
    void CaptureUiIntoScenario();
    void Revalidate();
    bool DoSaveTo(const CString& iniPath);

    // Block J — dirty tracking + Save/Discard/Cancel prompt
    void MarkDirty();
    void ClearDirty();
    // Returns true if the caller may proceed (Save succeeded, or Discard);
    // returns false if the user picked Cancel.
    bool MaybePromptSaveOnDiscard();

    bool                    m_isDirty = false;
    bool                    m_suppressDirty = false;
    CMFCToolBar             m_toolBar;
    CTabCtrl                m_tabCtrl;
    CStatusBar              m_statusBar;
    CFont                   m_uiFont;          // 12pt font for the status bar so it matches the bigger dialog text
    HACCEL                  m_hAccel = nullptr;
    int                     m_activePage = -1;
    bool                    m_loopChecked = false;
    bool                    m_helpChecked = false;
    Scenario                m_scenario;
    UdpSender               m_udp;            // unused once worker owns sending
    unsigned                m_pduCount = 0;
    CString                 m_currentScenarioPath; // empty until first Save/Open
    ScenarioWorker          m_worker;

    CScenarioSetupPage      m_pageSetup;
    CAssetEntityEditorPage  m_pageAssets;
    CMotionPathEditorPage   m_pageMotion;
    COutputPlaybackPage     m_pageOutput;
    CPreviewPage            m_pagePreview;
    CDialogEx*              m_pages[5] = { nullptr };
};
