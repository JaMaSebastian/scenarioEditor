//=============================================================================
//  OutputPlaybackPage.cpp
//-----------------------------------------------------------------------------
//  Implements COutputPlaybackPage: the dark-themed Run tab. Sets up field help,
//  binds the OutputConfig controls (ReadFrom/WriteTo), relays transport buttons
//  to the main dialog, and writes DISBrowser's Config\Startup.ini from the
//  chosen level/basemap and the scenario's origin/terrain bounds.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "OutputPlaybackPage.h"
#include "Scenario.h"
#include "ScenarioEditor.h"   // theApp.Settings()
#include "SettingsIO.h"
#include "TerrainEndpoint.h"
#include "StartupIniWriter.h"

#include <cstdlib>
#include <shlobj.h>           // SHBrowseForFolder — pick the DISBrowser project folder
#include <uxtheme.h>          // SetWindowTheme — declassic checkboxes/radios/groupboxes
#pragma comment(lib, "uxtheme.lib")

namespace
{
    const FFieldHelp kFields[] = {
        { IDC_RADIO_MODE_UDP_UNICAST,   _T("Send PDUs to a single host via UDP."), true },
        { IDC_RADIO_MODE_UDP_MULTICAST, _T("Send PDUs to a multicast group via UDP."), true },
        { IDC_RADIO_MODE_TCP,           _T("Send PDUs over a direct TCP connection - reliable, ordered, one peer."), true },
        { IDC_RADIO_MODE_FILE_RECORDING,_T("Write PDUs to a .disrec recording file instead of the network."), true },
        { IDC_RADIO_MODE_PREVIEW_ONLY,  _T("Render in the Preview tab only - no network traffic."), true },

        { IDC_EDIT_UDP_UNICAST_IP,      _T("Destination IPv4 address for UDP unicast output."), true },
        { IDC_EDIT_UDP_UNICAST_PORT,    _T("Destination UDP port for unicast output."), true },
        { IDC_EDIT_UDP_MULTICAST_GROUP, _T("IPv4 multicast group address (224.0.0.0/4)."), true },
        { IDC_EDIT_UDP_MULTICAST_PORT,  _T("Destination port for multicast output."), true },
        { IDC_EDIT_UDP_MULTICAST_TTL,   _T("Multicast TTL (hops). 1 = LAN-only."), false },
        { IDC_EDIT_UDP_MULTICAST_IFACE, _T("Local IPv4 of the outgoing interface, or 0.0.0.0 for OS default."), false },
        { IDC_CHK_UDP_MULTICAST_LOOP,   _T("Deliver our own multicast PDUs back to local sockets (useful for self-testing)."), false },
        { IDC_RADIO_TCP_CLIENT,         _T("The editor connects OUT to the visualizer's DIS TCP port (Remote host / Remote port). The visualizer must be listening first."), true },
        { IDC_RADIO_TCP_SERVER,         _T("The editor listens on Listen port and the visualizer connects IN to it. Start the editor's run first."), true },
        { IDC_EDIT_TCP_HOST,            _T("Client mode: IPv4 address of the receiver (DISBrowser's host)."), true },
        { IDC_EDIT_TCP_PORT,            _T("Client mode: receiver's TCP port. Must match DISBrowser's [DIS] TcpListenPort."), true },
        { IDC_EDIT_TCP_LISTEN_PORT,     _T("Server mode: local TCP port to accept a receiver on."), false },
        { IDC_EDIT_TCP_TIMEOUT,         _T("How long to wait for the connection (client) or a client (server), in ms."), false },
        { IDC_CHK_TCP_RECONNECT,        _T("Client mode: re-dial once if the connection drops mid-run."), false },
        { IDC_EDIT_CAM_CHANNEL_HOST,    _T("IPv4 address of the machine running the visualizer; camera cuts and poses are streamed there during a run. Leave blank for no camera direction."), true },
        { IDC_EDIT_CAM_CHANNEL_PORT,    _T("Visualizer camera director TCP port. Must match [CameraChannel] Port in its DefaultGame.ini (3011)."), false },

        { IDC_EDIT_RECORDING_PATH,      _T(".disrec file path that the recorder will write to."), false },
        { IDC_BTN_RECORDING_BROWSE,     _T("Pick a recording output path."), false },
        { IDC_EDIT_REPLAY_PATH,         _T(".disrec file path that the replayer will read from."), false },
        { IDC_BTN_REPLAY_BROWSE,        _T("Pick a recording to replay."), false },

        { IDC_BTN_PLAYBACK_START,       _T("Start playback / streaming."), false },
        { IDC_BTN_PLAYBACK_PAUSE,       _T("Pause playback."), false },
        { IDC_BTN_PLAYBACK_RESUME,      _T("Resume a paused playback."), false },
        { IDC_BTN_PLAYBACK_STOP,        _T("Stop playback and reset the scenario clock."), false },
        { IDC_CHK_PLAYBACK_LOOP,        _T("When the scenario or recording ends, restart from the beginning."), false },
        { IDC_COMBO_PLAYBACK_SPEED,     _T("Playback speed multiplier."), false },

        { IDC_BTN_ATTRIBUTES,           _T("Open the Attributes notebook to edit the scenario setup, assets/entities, and motion paths."), false },

        { IDC_UNREAL_LEVEL_COMBO,       _T("Which DISBrowser Unreal level to load at startup. Only 'Generic' uses the origin/basemap below."), false },
        { IDC_UNREAL_BASEMAP_COMBO,     _T("Ground for the Generic level: ESRI Satellite, OpenTopoMap Topographic, self-hosted Cesium 3D terrain (needs Scripts\\serve_terrain.cmd running), or none."), false },
        { IDC_EDIT_UNREAL_PROJECT,      _T("Root folder of the DISBrowser Unreal project (the one containing Config\\)."), false },
        { IDC_BTN_UNREAL_PROJECT_BROWSE,_T("Pick the DISBrowser project folder."), false },
        { IDC_BTN_CONFIGURE_UNREAL,     _T("Write Config\\Startup.ini so DISBrowser loads this level, origin, and basemap next time it starts."), false },
        { IDC_CHK_UNREAL_DYNAMIC_TILES, _T("Generic level only: stream basemap tiles in around the camera as you fly (no void past the initial grid) and cache them for future sessions."), false },
    };

    const TCHAR* const kUnrealLevels[]  = { _T("Generic"), _T("Beach"), _T("Forest"), _T("Main"), _T("Hanger"), _T("GodView") };
    const TCHAR* const kUnrealBasemaps[] = { _T("Satellite"), _T("Topographic"), _T("Cesium 3D (self-hosted)"), _T("None") };

    // Select the combo item whose text equals `want` (case-insensitive); else index 0.
    void SelectByText(CComboBox* cb, const CString& want)
    {
        if (!cb) return;
        for (int i = 0; i < cb->GetCount(); ++i)
        {
            CString item; cb->GetLBText(i, item);
            if (item.CompareNoCase(want) == 0) { cb->SetCurSel(i); return; }
        }
        cb->SetCurSel(0);
    }
}

BEGIN_MESSAGE_MAP(COutputPlaybackPage, CHelpAwarePage)
    ON_BN_CLICKED(IDC_BTN_RECORDING_BROWSE, &COutputPlaybackPage::OnBrowseRecording)
    ON_BN_CLICKED(IDC_BTN_REPLAY_BROWSE,    &COutputPlaybackPage::OnBrowseReplay)
    ON_BN_CLICKED(IDC_BTN_PLAYBACK_START,   &COutputPlaybackPage::OnLocalPlaybackStart)
    ON_BN_CLICKED(IDC_BTN_PLAYBACK_PAUSE,   &COutputPlaybackPage::OnLocalPlaybackPause)
    ON_BN_CLICKED(IDC_BTN_PLAYBACK_RESUME,  &COutputPlaybackPage::OnLocalPlaybackResume)
    ON_BN_CLICKED(IDC_BTN_PLAYBACK_STOP,    &COutputPlaybackPage::OnLocalPlaybackStop)
    ON_BN_CLICKED(IDC_BTN_ATTRIBUTES,       &COutputPlaybackPage::OnAttributes)
    ON_BN_CLICKED(IDC_BTN_CONFIGURE_UNREAL, &COutputPlaybackPage::OnConfigureUnreal)
    ON_BN_CLICKED(IDC_BTN_UNREAL_PROJECT_BROWSE, &COutputPlaybackPage::OnBrowseUnrealProject)
    ON_WM_CTLCOLOR()
END_MESSAGE_MAP()

//
// GetFieldHelpTable — hands the base HelpAwarePage this page's field-help table.
//
void COutputPlaybackPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

//
// OnInitDialog — applies the dark theme (declassics themed check/radio/group
// controls so white text shows), seeds the output-mode, playback-speed, and
// DISBrowser (Unreal) controls from saved settings and OutputConfig defaults.
//
BOOL COutputPlaybackPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    // Dark theme for the Run tab. Paint the page background black (used when
    // erasing) and prepare the brushes OnCtlColor hands back per control.
    m_blackBrush.CreateSolidBrush(RGB(0, 0, 0));
    m_whiteBrush.CreateSolidBrush(RGB(255, 255, 255));
    SetBackgroundColor(RGB(0, 0, 0));

    // Themed checkboxes / radios / group boxes ignore the text color set in
    // OnCtlColor, so their labels would stay dark (invisible on black). Drop
    // their visual style to classic drawing so the white text takes effect.
    for (CWnd* c = GetWindow(GW_CHILD); c; c = c->GetNextWindow(GW_HWNDNEXT))
    {
        TCHAR cls[32] = { 0 };
        ::GetClassName(c->GetSafeHwnd(), cls, _countof(cls));
        if (_tcsicmp(cls, _T("Button")) == 0)
        {
            const DWORD t = c->GetStyle() & BS_TYPEMASK;
            if (t == BS_CHECKBOX || t == BS_AUTOCHECKBOX ||
                t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON ||
                t == BS_3STATE || t == BS_AUTO3STATE || t == BS_GROUPBOX)
            {
                ::SetWindowTheme(c->GetSafeHwnd(), L"", L"");
            }
        }
    }

    CheckDlgButton(IDC_RADIO_MODE_UDP_UNICAST, BST_CHECKED);

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_PLAYBACK_SPEED))
    {
        const TCHAR* speeds[] = { _T("0.25x"), _T("0.5x"), _T("1x"), _T("2x"), _T("10x") };
        for (const auto* s : speeds)
            cb->AddString(s);
        cb->SetCurSel(2);
    }

    // Populate the DISBrowser (Unreal) configuration controls from saved settings.
    const ::Settings& st = theApp.Settings();
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_UNREAL_LEVEL_COMBO))
    {
        for (const auto* s : kUnrealLevels) cb->AddString(s);
        SelectByText(cb, CString(st.unrealTargetLevel.c_str()));
    }
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_UNREAL_BASEMAP_COMBO))
    {
        for (const auto* s : kUnrealBasemaps) cb->AddString(s);
        SelectByText(cb, CString(st.unrealBasemap.c_str()));
    }
    SetDlgItemText(IDC_EDIT_UNREAL_PROJECT, ResolveProjectDir());
    CheckDlgButton(IDC_CHK_UNREAL_DYNAMIC_TILES, st.unrealDynamicTiles ? BST_CHECKED : BST_UNCHECKED);

    // Seed defaults from a fresh OutputConfig.
    OutputConfig defaults;
    ReadFrom(defaults);
    return TRUE;
}

//
// ResolveProjectDir — returns the saved DISBrowser project dir, else a sibling
// "DISBrowser" folder derived from the running exe's path.
//
CString COutputPlaybackPage::ResolveProjectDir() const
{
    const ::Settings& st = theApp.Settings();
    if (!st.disBrowserProjectDir.empty())
        return CString(st.disBrowserProjectDir.c_str());

    // Default: sibling "DISBrowser" folder next to the ScenarioEditor exe's grandparent.
    TCHAR exe[MAX_PATH] = { 0 };
    ::GetModuleFileName(nullptr, exe, _countof(exe));
    CString path(exe);
    int slash = path.ReverseFind(_T('\\'));
    if (slash > 0) path = path.Left(slash);            // strip exe name
    // exe typically lives in ScenarioEditor\build*\<cfg>\ — hop up to the common parent, then DISBrowser.
    return path + _T("\\..\\..\\..\\DISBrowser");
}

//
// OnCtlColor — dark theme: dialog/static text white-on-black, edit boxes and
// combo drop-lists black-on-white; falls back to base until brushes are built.
//
HBRUSH COutputPlaybackPage::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    // WM_CTLCOLOR can arrive while the controls are being created, before
    // OnInitDialog builds the brushes — fall back to the default until ready.
    if (!m_blackBrush.GetSafeHandle() || !m_whiteBrush.GetSafeHandle())
        return CHelpAwarePage::OnCtlColor(pDC, pWnd, nCtlColor);

    switch (nCtlColor)
    {
        case CTLCOLOR_DLG:
            return static_cast<HBRUSH>(m_blackBrush);

        // Static text, group-box captions, and the (declassic'd) checkbox /
        // radio labels: white text on a black background.
        case CTLCOLOR_STATIC:
            pDC->SetBkMode(TRANSPARENT);
            pDC->SetTextColor(RGB(255, 255, 255));
            pDC->SetBkColor(RGB(0, 0, 0));
            return static_cast<HBRUSH>(m_blackBrush);

        // Edit boxes (and the combo drop-down list) stay white with black text.
        case CTLCOLOR_EDIT:
        case CTLCOLOR_LISTBOX:
            pDC->SetTextColor(RGB(0, 0, 0));
            pDC->SetBkColor(RGB(255, 255, 255));
            return static_cast<HBRUSH>(m_whiteBrush);

        default:
            return CHelpAwarePage::OnCtlColor(pDC, pWnd, nCtlColor);
    }
}

//
// ReadFrom — pushes an OutputConfig onto the controls: output-mode radios, the
// UDP/multicast + recording/replay fields, and playback speed/loop.
//
void COutputPlaybackPage::ReadFrom(const OutputConfig& o)
{
    if (!::IsWindow(GetSafeHwnd())) return;
    CheckDlgButton(IDC_RADIO_MODE_UDP_UNICAST,    o.mode == OutputMode::UdpUnicast    ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_MODE_UDP_MULTICAST,  o.mode == OutputMode::UdpMulticast  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_MODE_TCP,            o.mode == OutputMode::Tcp           ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_MODE_FILE_RECORDING, o.mode == OutputMode::FileRecording ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_RADIO_MODE_PREVIEW_ONLY,   o.mode == OutputMode::PreviewOnly   ? BST_CHECKED : BST_UNCHECKED);

    SetDlgItemText(IDC_EDIT_UDP_UNICAST_IP,    CA2T(o.unicastIp.c_str()));
    SetDlgItemInt (IDC_EDIT_UDP_UNICAST_PORT,  o.unicastPort, FALSE);
    SetDlgItemText(IDC_EDIT_UDP_MULTICAST_GROUP, CA2T(o.multicastGroup.c_str()));
    SetDlgItemInt (IDC_EDIT_UDP_MULTICAST_PORT,  o.multicastPort, FALSE);
    SetDlgItemInt (IDC_EDIT_UDP_MULTICAST_TTL,   o.multicastTtl,  FALSE);
    SetDlgItemText(IDC_EDIT_UDP_MULTICAST_IFACE, CA2T(o.multicastInterface.c_str()));
    CheckDlgButton(IDC_CHK_UDP_MULTICAST_LOOP, o.multicastLoopback ? BST_CHECKED : BST_UNCHECKED);

    CheckDlgButton(IDC_RADIO_TCP_CLIENT, o.tcpListen ? BST_UNCHECKED : BST_CHECKED);
    CheckDlgButton(IDC_RADIO_TCP_SERVER, o.tcpListen ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemText(IDC_EDIT_TCP_HOST,        CA2T(o.tcpRemoteHost.c_str()));
    SetDlgItemInt (IDC_EDIT_TCP_PORT,        o.tcpRemotePort, FALSE);
    SetDlgItemInt (IDC_EDIT_TCP_LISTEN_PORT, o.tcpListenPort, FALSE);
    SetDlgItemInt (IDC_EDIT_TCP_TIMEOUT,     o.tcpTimeoutMs,  FALSE);
    CheckDlgButton(IDC_CHK_TCP_RECONNECT, o.tcpReconnect ? BST_CHECKED : BST_UNCHECKED);

    // A disabled channel shows as a blank address: that is the one control.
    SetDlgItemText(IDC_EDIT_CAM_CHANNEL_HOST,
                   o.cameraChannelEnabled ? CA2T(o.cameraChannelHost.c_str()) : _T(""));
    SetDlgItemInt (IDC_EDIT_CAM_CHANNEL_PORT, o.cameraChannelPort, FALSE);

    SetDlgItemText(IDC_EDIT_RECORDING_PATH, CA2T(o.recordingPath.c_str()));
    SetDlgItemText(IDC_EDIT_REPLAY_PATH,    CA2T(o.replayPath.c_str()));

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_PLAYBACK_SPEED))
    {
        int idx = 2; // default 1.0x
        if      (o.playbackSpeed <= 0.30) idx = 0;
        else if (o.playbackSpeed <= 0.75) idx = 1;
        else if (o.playbackSpeed <= 1.50) idx = 2;
        else if (o.playbackSpeed <= 5.00) idx = 3;
        else                              idx = 4;
        cb->SetCurSel(idx);
    }
    CheckDlgButton(IDC_CHK_PLAYBACK_LOOP, o.loopEnabled ? BST_CHECKED : BST_UNCHECKED);
}

//
// WriteTo — pulls the control values into an OutputConfig: output mode, the
// UDP/multicast + recording/replay fields, and playback speed/loop. Blank text
// fields leave the corresponding config values unchanged.
//
void COutputPlaybackPage::WriteTo(OutputConfig& o) const
{
    if (!::IsWindow(GetSafeHwnd())) return;

    if      (IsDlgButtonChecked(IDC_RADIO_MODE_UDP_UNICAST))    o.mode = OutputMode::UdpUnicast;
    else if (IsDlgButtonChecked(IDC_RADIO_MODE_UDP_MULTICAST))  o.mode = OutputMode::UdpMulticast;
    else if (IsDlgButtonChecked(IDC_RADIO_MODE_TCP))            o.mode = OutputMode::Tcp;
    else if (IsDlgButtonChecked(IDC_RADIO_MODE_FILE_RECORDING)) o.mode = OutputMode::FileRecording;
    else if (IsDlgButtonChecked(IDC_RADIO_MODE_PREVIEW_ONLY))   o.mode = OutputMode::PreviewOnly;

    CString text;
    GetDlgItemText(IDC_EDIT_UDP_UNICAST_IP, text);
    if (!text.IsEmpty()) { CT2A a(text); o.unicastIp = a.m_psz; }
    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(IDC_EDIT_UDP_UNICAST_PORT, &ok, FALSE);
    if (ok) o.unicastPort = static_cast<uint16_t>(v & 0xFFFF);

    GetDlgItemText(IDC_EDIT_UDP_MULTICAST_GROUP, text);
    if (!text.IsEmpty()) { CT2A a(text); o.multicastGroup = a.m_psz; }
    v = GetDlgItemInt(IDC_EDIT_UDP_MULTICAST_PORT, &ok, FALSE);
    if (ok) o.multicastPort = static_cast<uint16_t>(v & 0xFFFF);
    v = GetDlgItemInt(IDC_EDIT_UDP_MULTICAST_TTL, &ok, FALSE);
    if (ok) o.multicastTtl = static_cast<int>(v);
    GetDlgItemText(IDC_EDIT_UDP_MULTICAST_IFACE, text);
    if (!text.IsEmpty()) { CT2A a(text); o.multicastInterface = a.m_psz; }
    o.multicastLoopback = IsDlgButtonChecked(IDC_CHK_UDP_MULTICAST_LOOP) == BST_CHECKED;

    o.tcpListen = IsDlgButtonChecked(IDC_RADIO_TCP_SERVER) == BST_CHECKED;
    GetDlgItemText(IDC_EDIT_TCP_HOST, text);
    if (!text.IsEmpty()) { CT2A a(text); o.tcpRemoteHost = a.m_psz; }
    v = GetDlgItemInt(IDC_EDIT_TCP_PORT, &ok, FALSE);
    if (ok) o.tcpRemotePort = static_cast<uint16_t>(v & 0xFFFF);
    v = GetDlgItemInt(IDC_EDIT_TCP_LISTEN_PORT, &ok, FALSE);
    if (ok) o.tcpListenPort = static_cast<uint16_t>(v & 0xFFFF);
    v = GetDlgItemInt(IDC_EDIT_TCP_TIMEOUT, &ok, FALSE);
    if (ok) o.tcpTimeoutMs = static_cast<int>(v);
    o.tcpReconnect = IsDlgButtonChecked(IDC_CHK_TCP_RECONNECT) == BST_CHECKED;

    GetDlgItemText(IDC_EDIT_CAM_CHANNEL_HOST, text);
    text.Trim();
    o.cameraChannelEnabled = !text.IsEmpty();       // blank address = off
    if (!text.IsEmpty()) { CT2A a(text); o.cameraChannelHost = a.m_psz; }
    v = GetDlgItemInt(IDC_EDIT_CAM_CHANNEL_PORT, &ok, FALSE);
    if (ok) o.cameraChannelPort = static_cast<uint16_t>(v & 0xFFFF);

    GetDlgItemText(IDC_EDIT_RECORDING_PATH, text);
    { CT2A a(text); o.recordingPath = a.m_psz; }
    GetDlgItemText(IDC_EDIT_REPLAY_PATH, text);
    { CT2A a(text); o.replayPath = a.m_psz; }

    if (const CComboBox* cb = (const CComboBox*)GetDlgItem(IDC_COMBO_PLAYBACK_SPEED))
    {
        const double speeds[] = { 0.25, 0.5, 1.0, 2.0, 10.0 };
        const int idx = cb->GetCurSel();
        if (idx >= 0 && idx < 5) o.playbackSpeed = speeds[idx];
    }
    o.loopEnabled = IsDlgButtonChecked(IDC_CHK_PLAYBACK_LOOP) == BST_CHECKED;
}

// The Output tab's playback buttons mirror the toolbar; they fire the
// same WM_COMMAND IDs to the main dialog, which already handles them.
namespace
{
    void RelayToMain(CWnd* page, UINT cmdId)
    {
        if (CWnd* top = page->GetTopLevelParent())
            top->PostMessage(WM_COMMAND, cmdId, 0);
    }
}

void COutputPlaybackPage::OnLocalPlaybackStart()  { RelayToMain(this, ID_PLAYBACK_START); }
void COutputPlaybackPage::OnLocalPlaybackPause()  { RelayToMain(this, ID_PLAYBACK_PAUSE); }
void COutputPlaybackPage::OnLocalPlaybackResume() { RelayToMain(this, ID_PLAYBACK_RESUME); }
void COutputPlaybackPage::OnLocalPlaybackStop()   { RelayToMain(this, ID_PLAYBACK_STOP); }

// The Attributes notebook is owned by the main dialog (it holds the live
// Scenario + catalog), so relay the open request up rather than launching it
// from the page.
void COutputPlaybackPage::OnAttributes()          { RelayToMain(this, ID_TOOLS_OPEN_ATTRIBUTES); }

//
// OnBrowseRecording — Save dialog for the .disrec recording output path.
//
void COutputPlaybackPage::OnBrowseRecording()
{
    CFileDialog dlg(/*open*/FALSE, _T("disrec"), nullptr,
                    OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
                    _T("DIS Recording (*.disrec)|*.disrec|All Files (*.*)|*.*||"),
                    this);
    if (dlg.DoModal() == IDOK)
        SetDlgItemText(IDC_EDIT_RECORDING_PATH, dlg.GetPathName());
}

//
// OnBrowseReplay — Open dialog for choosing a .disrec recording to replay.
//
void COutputPlaybackPage::OnBrowseReplay()
{
    CFileDialog dlg(/*open*/TRUE, _T("disrec"), nullptr,
                    OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
                    _T("DIS Recording (*.disrec)|*.disrec|All Files (*.*)|*.*||"),
                    this);
    if (dlg.DoModal() == IDOK)
        SetDlgItemText(IDC_EDIT_REPLAY_PATH, dlg.GetPathName());
}

namespace
{
    // BrowseCallbackProc: preselect the folder passed via lpData.
    int CALLBACK BrowseInitProc(HWND hwnd, UINT msg, LPARAM /*lp*/, LPARAM data)
    {
        if (msg == BFFM_INITIALIZED && data)
            ::SendMessage(hwnd, BFFM_SETSELECTION, TRUE, data);
        return 0;
    }
}

//
// OnBrowseUnrealProject — folder picker (preseeded with the current value) for
// the DISBrowser Unreal project root; writes the choice back to the edit box.
//
void COutputPlaybackPage::OnBrowseUnrealProject()
{
    CString initial;
    GetDlgItemText(IDC_EDIT_UNREAL_PROJECT, initial);

    TCHAR display[MAX_PATH] = { 0 };
    BROWSEINFO bi = { 0 };
    bi.hwndOwner = GetSafeHwnd();
    bi.pszDisplayName = display;
    bi.lpszTitle = _T("Select the DISBrowser Unreal project folder (contains Config\\)");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = BrowseInitProc;
    bi.lParam = (LPARAM)(LPCTSTR)initial;

    LPITEMIDLIST pidl = ::SHBrowseForFolder(&bi);
    if (!pidl) return;

    TCHAR path[MAX_PATH] = { 0 };
    if (::SHGetPathFromIDList(pidl, path))
        SetDlgItemText(IDC_EDIT_UNREAL_PROJECT, path);
    ::CoTaskMemFree(pidl);
}

//
// OnConfigureUnreal — persists the level/basemap/project picks, then writes
// DISBrowser's Config\Startup.ini (origin + terrain bounds from the scenario)
// via StartupIniWriter and reports success/failure to the user.
//
void COutputPlaybackPage::OnConfigureUnreal()
{
    // The origin isn't committed until the scenario is saved; don't hand DISBrowser a
    // provisional (default-view) origin — it would drop the level at the wrong place.
    if (m_scenario && !m_scenario->originSet)
    {
        AfxMessageBox(_T("Save the scenario first to set its origin, then Configure Unreal."),
                      MB_OK | MB_ICONWARNING);
        return;
    }

    // Gather the picks.
    CString level, basemap, projectDir;
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_UNREAL_LEVEL_COMBO))
        cb->GetLBText(cb->GetCurSel() < 0 ? 0 : cb->GetCurSel(), level);
    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_UNREAL_BASEMAP_COMBO))
        cb->GetLBText(cb->GetCurSel() < 0 ? 0 : cb->GetCurSel(), basemap);
    GetDlgItemText(IDC_EDIT_UNREAL_PROJECT, projectDir);
    projectDir.Trim();
    const bool dynamicTiles = IsDlgButtonChecked(IDC_CHK_UNREAL_DYNAMIC_TILES) == BST_CHECKED;

    if (projectDir.IsEmpty())
    {
        AfxMessageBox(_T("Set the DISBrowser project folder first."), MB_ICONWARNING);
        return;
    }

    // Guard against a silent "no ground" handoff: the Preview tab has its own Map combo
    // (the 2D/3D preview layer) which does NOT feed Startup.ini — only THIS tab's basemap
    // combo does. Writing Map=None for a scenario with a painted 3D-terrain boundary is
    // almost always the two combos being conflated, and DISBrowser then renders no
    // landscape at all. Confirm before writing it.
    if (basemap.CompareNoCase(_T("None")) == 0 &&
        m_scenario && m_scenario->terrainBoundsValid)
    {
        if (AfxMessageBox(
                _T("The Unreal basemap on THIS tab is set to \"None\" — DISBrowser will render ")
                _T("no landscape, even though this scenario has a painted 3D-terrain boundary.\n\n")
                _T("(The Preview tab's Map combo only changes the preview, not Unreal.)\n\n")
                _T("Write Startup.ini with no ground layer anyway?"),
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        {
            return;
        }
    }

    // Persist the choices so they survive across sessions (dialog saves settings on exit).
    ::Settings& st = theApp.Settings();
    { CT2A a(level);      st.unrealTargetLevel   = a.m_psz; }
    { CT2A a(basemap);    st.unrealBasemap       = a.m_psz; }
    { CT2A a(projectDir); st.disBrowserProjectDir = a.m_psz; }
    st.unrealDynamicTiles = dynamicTiles;

    double lat = 0.0, lon = 0.0, alt = 0.0;
    bool   boundsValid = false;
    double latMin = 0.0, latMax = 0.0, lonMin = 0.0, lonMax = 0.0;
    if (m_scenario)
    {
        lat = m_scenario->originLatDeg;
        lon = m_scenario->originLonDeg;
        alt = m_scenario->originAltM;
        boundsValid = m_scenario->terrainBoundsValid;
        latMin = m_scenario->terrainLatMinDeg;
        latMax = m_scenario->terrainLatMaxDeg;
        lonMin = m_scenario->terrainLonMinDeg;
        lonMax = m_scenario->terrainLonMaxDeg;
    }

    // Camera schedule: the [Camera.N] sections now live inside scenario.ini, so
    // point DISBrowser's director at the scenario.ini itself (its config reader
    // keeps only [Camera.N] sections and ignores the rest). Hand it over only when
    // the scenario is saved AND actually has cameras; otherwise pass an empty path
    // so Startup.ini clears any stale schedule and DISBrowser runs without a track.
    CString cameraAbs;
    bool cameraConfigured = false;
    if (!m_scenarioPath.IsEmpty() && m_scenario && !m_scenario->cameras.empty() &&
        ::GetFileAttributes(m_scenarioPath) != INVALID_FILE_ATTRIBUTES)
    {
        cameraAbs = m_scenarioPath;
        cameraConfigured = true;
    }

    // Foliage selection -> [Foliage]. DISBrowser scatters the chosen trees across the
    // terrain box, so it needs a built 3D terrain to sit on; warn if there's foliage
    // but no boundary, and still write the rest.
    StartupIniWriter::FoliageHandoff foliage;
    if (m_scenario)
    {
        const FoliageConfig& fo = m_scenario->foliage;
        foliage.oak      = fo.oak;
        foliage.bigTrees = fo.bigTrees;
        foliage.palm     = fo.palm;
        foliage.palmKind = (fo.palmKind == PalmKind::Tall)     ? "Tall"
                         : (fo.palmKind == PalmKind::Straight) ? "Straight" : "All";
        foliage.renderMode = (fo.renderMode == FoliageRenderMode::I3dm)       ? "i3dm"
                           : (fo.renderMode == FoliageRenderMode::BlenderGIS) ? "BlenderGIS" : "InEngine";
        if ((fo.oak || fo.bigTrees || fo.palm) && !boundsValid)
            AfxMessageBox(_T("Foliage is selected but this scenario has no 3D terrain boundary.\n")
                          _T("Paint a Boundary and Build 3D Terrain first — foliage needs terrain to sit on."),
                          MB_OK | MB_ICONWARNING);
    }

    const TerrainEndpoint terrainEp = ResolveTerrainEndpoint(st);

    std::wstring result;
    const bool ok = StartupIniWriter::Write(
        std::wstring(CT2W(projectDir)),
        st.unrealTargetLevel, st.unrealBasemap, dynamicTiles,
        lat, lon, alt,
        boundsValid, latMin, latMax, lonMin, lonMax,
        std::wstring(CT2W(cameraAbs)), foliage,
        // Where DISBrowser fetches tiles from. Remote and Service both put the
        // server on another host/port, so this cannot be assumed to be
        // localhost:8088. Resolved through the SHARED resolver rather than
        // re-derived here -- this used to be an inline copy that defaulted to
        // "localhost" while PreviewPage's copy defaulted to "127.0.0.1".
        terrainEp.host,
        terrainEp.port,
        result);

    if (ok)
    {
        CString msg;
        if (level.CompareNoCase(_T("Generic")) == 0)
            msg.Format(_T("Wrote:\n%s\n\nDISBrowser will load the Generic level with origin ")
                       _T("(%.6f, %.6f, %.1f m) and the %s basemap at startup.\nDynamic tile streaming: %s."),
                       result.c_str(), lat, lon, alt, (LPCTSTR)basemap,
                       dynamicTiles ? _T("ON") : _T("off"));
        else
            msg.Format(_T("Wrote:\n%s\n\nDISBrowser will load the %s level at startup ")
                       _T("(origin/basemap unchanged for non-Generic levels)."),
                       result.c_str(), (LPCTSTR)level);

        // Camera track status (applies to every level).
        if (cameraConfigured)
        {
            CString cam;
            cam.Format(_T("\n\nCamera track: %s\n(DISBrowser's director will play this schedule.)"),
                       (LPCTSTR)cameraAbs);
            msg += cam;
        }
        else
        {
            msg += _T("\n\nCamera track: none — save the scenario (with at least one camera) ")
                   _T("first, then Configure Unreal again to wire its camera schedule.");
        }
        AfxMessageBox(msg, MB_ICONINFORMATION);
    }
    else
    {
        AfxMessageBox((_T("Could not write Startup.ini:\n") + CString(result.c_str())), MB_ICONERROR);
    }
}
