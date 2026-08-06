//=============================================================================
//  OutputPlaybackPage.h
//-----------------------------------------------------------------------------
//  Declares COutputPlaybackPage, the dark-themed "Run" tab. It edits the
//  OutputConfig (network/file output mode, playback speed/loop), relays the
//  playback transport buttons to the main dialog, and configures the DISBrowser
//  Unreal side by writing Config\Startup.ini (level / origin / basemap).
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include "pch.h"
#include "Resource.h"
#include "HelpAwarePage.h"

struct OutputConfig;
struct Scenario;

//-----------------------------------------------------------------------------
// COutputPlaybackPage — "Run" property page.
//   Binds the OutputConfig to its controls (ReadFrom/WriteTo), forwards the
//   Start/Pause/Resume/Stop and Attributes commands up to the main dialog, and
//   writes the DISBrowser Startup.ini from the current scenario origin/bounds.
//-----------------------------------------------------------------------------
class COutputPlaybackPage : public CHelpAwarePage
{
public:
    enum { IDD = IDD_OUTPUT_PLAYBACK_PAGE };
    COutputPlaybackPage(CWnd* pParent = nullptr) : CHelpAwarePage(IDD, pParent) {}

    void SetScenario(Scenario* scenario) { m_scenario = scenario; }

    // The full path of the currently-open scenario.ini (owned by the main dialog).
    // "Configure Unreal" derives this scenario's <scenario>-camera.ini from it and
    // writes that path into DISBrowser's Startup.ini. Empty until first Save/Open.
    void SetScenarioPath(const CString& path) { m_scenarioPath = path; }

    //
    // WriteTo / ReadFrom — bind the page's controls to an OutputConfig (pull
    // control values out / push config values in).
    //
    void WriteTo(OutputConfig& out) const;
    void ReadFrom(const OutputConfig& out);

protected:
    //
    // OnInitDialog — applies the dark theme and seeds the mode/speed/Unreal
    // controls from saved settings and OutputConfig defaults.
    //
    BOOL OnInitDialog() override;
    void GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const override;

    //
    // OnCtlColor — dark-theme brushes: white-on-black statics, white edit boxes.
    //
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    //
    // OnBrowseRecording / OnBrowseReplay — file pickers for the .disrec paths.
    //
    afx_msg void OnBrowseRecording();
    afx_msg void OnBrowseReplay();
    //
    // OnLocalPlayback* / OnAttributes — relay the transport and Attributes
    // commands up to the main dialog (which owns the live scenario/catalog).
    //
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
    CString   m_scenarioPath;   // full path of the open scenario.ini (from the dialog)

    // Run tab is a dark theme: black page background + white label text, with
    // edit boxes kept white-on-black. Brushes are handed back from OnCtlColor.
    CBrush    m_blackBrush;
    CBrush    m_whiteBrush;
};
