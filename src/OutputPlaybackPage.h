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

    afx_msg void OnBrowseRecording();
    afx_msg void OnBrowseReplay();
    afx_msg void OnLocalPlaybackStart();
    afx_msg void OnLocalPlaybackPause();
    afx_msg void OnLocalPlaybackResume();
    afx_msg void OnLocalPlaybackStop();

    DECLARE_MESSAGE_MAP()

private:
    Scenario* m_scenario = nullptr;
};
