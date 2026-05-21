#include "pch.h"
#include "OutputPlaybackPage.h"
#include "Scenario.h"

#include <cstdlib>

namespace
{
    const FFieldHelp kFields[] = {
        { IDC_RADIO_MODE_UDP_UNICAST,   _T("Send PDUs to a single host via UDP."), true },
        { IDC_RADIO_MODE_UDP_MULTICAST, _T("Send PDUs to a multicast group via UDP."), true },
        { IDC_RADIO_MODE_TCP,           _T("Send PDUs over a direct TCP connection. (Deferred in V2 per spec §23.2.)"), true },
        { IDC_RADIO_MODE_FILE_RECORDING,_T("Write PDUs to a .disrec recording file instead of the network."), true },
        { IDC_RADIO_MODE_PREVIEW_ONLY,  _T("Render in the Preview tab only - no network traffic."), true },

        { IDC_EDIT_UDP_UNICAST_IP,      _T("Destination IPv4 address for UDP unicast output."), true },
        { IDC_EDIT_UDP_UNICAST_PORT,    _T("Destination UDP port for unicast output."), true },
        { IDC_EDIT_UDP_MULTICAST_GROUP, _T("IPv4 multicast group address (224.0.0.0/4)."), true },
        { IDC_EDIT_UDP_MULTICAST_PORT,  _T("Destination port for multicast output."), true },
        { IDC_EDIT_UDP_MULTICAST_TTL,   _T("Multicast TTL (hops). 1 = LAN-only."), false },
        { IDC_EDIT_UDP_MULTICAST_IFACE, _T("Local IPv4 of the outgoing interface, or 0.0.0.0 for OS default."), false },
        { IDC_CHK_UDP_MULTICAST_LOOP,   _T("Deliver our own multicast PDUs back to local sockets (useful for self-testing)."), false },

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
    };
}

BEGIN_MESSAGE_MAP(COutputPlaybackPage, CHelpAwarePage)
    ON_BN_CLICKED(IDC_BTN_RECORDING_BROWSE, &COutputPlaybackPage::OnBrowseRecording)
    ON_BN_CLICKED(IDC_BTN_REPLAY_BROWSE,    &COutputPlaybackPage::OnBrowseReplay)
    ON_BN_CLICKED(IDC_BTN_PLAYBACK_START,   &COutputPlaybackPage::OnLocalPlaybackStart)
    ON_BN_CLICKED(IDC_BTN_PLAYBACK_PAUSE,   &COutputPlaybackPage::OnLocalPlaybackPause)
    ON_BN_CLICKED(IDC_BTN_PLAYBACK_RESUME,  &COutputPlaybackPage::OnLocalPlaybackResume)
    ON_BN_CLICKED(IDC_BTN_PLAYBACK_STOP,    &COutputPlaybackPage::OnLocalPlaybackStop)
END_MESSAGE_MAP()

void COutputPlaybackPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

BOOL COutputPlaybackPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    CheckDlgButton(IDC_RADIO_MODE_UDP_UNICAST, BST_CHECKED);

    if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_PLAYBACK_SPEED))
    {
        const TCHAR* speeds[] = { _T("0.25x"), _T("0.5x"), _T("1x"), _T("2x"), _T("10x") };
        for (const auto* s : speeds)
            cb->AddString(s);
        cb->SetCurSel(2);
    }

    // Seed defaults from a fresh OutputConfig.
    OutputConfig defaults;
    ReadFrom(defaults);
    return TRUE;
}

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

void COutputPlaybackPage::OnBrowseRecording()
{
    CFileDialog dlg(/*open*/FALSE, _T("disrec"), nullptr,
                    OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
                    _T("DIS Recording (*.disrec)|*.disrec|All Files (*.*)|*.*||"),
                    this);
    if (dlg.DoModal() == IDOK)
        SetDlgItemText(IDC_EDIT_RECORDING_PATH, dlg.GetPathName());
}

void COutputPlaybackPage::OnBrowseReplay()
{
    CFileDialog dlg(/*open*/TRUE, _T("disrec"), nullptr,
                    OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
                    _T("DIS Recording (*.disrec)|*.disrec|All Files (*.*)|*.*||"),
                    this);
    if (dlg.DoModal() == IDOK)
        SetDlgItemText(IDC_EDIT_REPLAY_PATH, dlg.GetPathName());
}
