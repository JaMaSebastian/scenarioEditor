#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

struct OutputConfig;
struct Scenario;

class COutputPlaybackPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_OUTPUT_PLAYBACK_PAGE };
    COutputPlaybackPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

    void SetScenario(Scenario* scenario) { m_scenario = scenario; }

    void WriteTo(OutputConfig& out) const;
    void ReadFrom(const OutputConfig& out);

protected:
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;

    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnBrowseRecording();
    afx_msg void OnBrowseReplay();
    afx_msg void OnLocalPlaybackStart();
    afx_msg void OnLocalPlaybackPause();
    afx_msg void OnLocalPlaybackResume();
    afx_msg void OnLocalPlaybackStop();
    afx_msg void OnAttributes();
    afx_msg void OnConfigureUnreal();      // write Config\Startup.ini for DISBrowser
    afx_msg void OnBrowseUnrealProject();  // pick the DISBrowser project folder

    DECLARE_MESSAGE_MAP()

private:
    // Resolves the DISBrowser project dir: the saved setting, else "..\DISBrowser" next to the exe.
    CString ResolveProjectDir() const;

private:
    Scenario* m_scenario = nullptr;

    // Run tab is a dark theme: black page background + white label text, with
    // edit boxes kept white-on-black. Brushes are handed back from OnCtlColor.
    CBrush    m_blackBrush;
    CBrush    m_whiteBrush;
};
