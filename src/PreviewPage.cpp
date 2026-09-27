//=============================================================================
//  PreviewPage.cpp
//-----------------------------------------------------------------------------
//  Implements CPreviewPage, the ScenarioEditor "Preview" tab. Drives timed
//  playback of a scenario, projects entities onto a 2D canvas / Cesium globe,
//  and implements the authoring gestures (drag start, plot line/ellipse/
//  take-off/follow courses, set duration/delay, group ops, boundary paint,
//  map relocation) plus the self-hosted terrain-server controls.
//  Free helpers (name indexing, path rescale, ellipse snap, TCP probe) and
//  the small modal dialogs live in an anonymous namespace up top.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "PreviewPage.h"
#include "TerrainEndpoint.h"
#include "Scenario.h"
#include "MotionSampler.h"
#include "CoordTransforms.h"
#include "ScenarioWorker.h"   // WM_APP_REFRESH_UI / WM_APP_MARK_DIRTY
#include "ScenarioEditor.h"   // theApp.Catalog() — per-type cruise speed
#include "EntityCameraDialog.h"
#include "FoliageDialog.h"          // "Foliage" button dialog
#include "EntityTypeCatalog.h"      // CatalogEntry / AirframeProfile
#include "SpeedSeed.h"              // SeedCruiseSpeedMps — catalog cruise / 10 m/s default
#include "CameraTargetExtents.h"    // cache the framed entity's catalogued size
#include "CameraPresetCombo.h"    // shared preset-combo fill + kRowNotSelectable
#include "EntityTypePickerDialog.h" // "Change Entity" catalog tree picker
#include "../log.h"
#include <shlobj.h>      // SHGetFolderPath — %LOCALAPPDATA% for the published tiles
#include <shellapi.h>    // SHFileOperation — drop the published copy on purge

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <uxtheme.h>          // SetWindowTheme — declassic checkboxes for white labels
#pragma comment(lib, "uxtheme.lib")
#include <shellapi.h>         // ShellExecute — launch the DISBrowser re-tile script
#include <utility>            // std::swap
#include <winsock2.h>         // probe localhost:8088 to detect the terrain tile server
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace
{
    // True if something is listening on <host>:<port> (WSL2 forwards localhost),
    // used to detect the self-hosted Cesium terrain server. Non-blocking connect
    // with a short timeout so the UI poll never stalls.
    //
    // Resolution is via getaddrinfo, NOT InetPtonA: InetPtonA parses dotted-quad
    // literals only, so any HOSTNAME (an EC2 DNS name, "localhost", a container
    // alias) left sin_addr as 0.0.0.0 and the probe reported DOWN forever, with
    // no error anywhere -- indistinguishable from a dead server. AF_UNSPEC so an
    // AAAA-only name still resolves; every returned address is tried in turn.
    bool ProbeTcpPort(const char* host, unsigned short port, int timeoutMs)
    {
        static bool wsaUp = false;
        if (!wsaUp)
        {
            WSADATA wsa{};
            if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
            wsaUp = true;   // process-lifetime; cleaned up at exit
        }

        char portStr[16] = { 0 };
        sprintf_s(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* res = nullptr;
        if (::getaddrinfo(host, portStr, &hints, &res) != 0 || res == nullptr)
            return false;

        bool ok = false;
        for (addrinfo* ai = res; ai != nullptr && !ok; ai = ai->ai_next)
        {
            SOCKET s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == INVALID_SOCKET) continue;

            u_long nonBlocking = 1;
            ::ioctlsocket(s, FIONBIO, &nonBlocking);

            if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
                ok = true;
            else if (::WSAGetLastError() == WSAEWOULDBLOCK)
            {
                fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
                timeval tv{};
                tv.tv_sec  = timeoutMs / 1000;
                tv.tv_usec = (timeoutMs % 1000) * 1000;
                if (::select(0, nullptr, &wf, nullptr, &tv) > 0 && FD_ISSET(s, &wf))
                {
                    int err = 0; int len = sizeof(err);
                    if (::getsockopt(s, SOL_SOCKET, SO_ERROR,
                                     reinterpret_cast<char*>(&err), &len) == 0 && err == 0)
                        ok = true;
                }
            }

            ::closesocket(s);
        }

        ::freeaddrinfo(res);
        return ok;
    }
    const FFieldHelp kFields[] = {
        { IDC_BTN_PREVIEW_START,       _T("Start the preview animation."), false },
        { IDC_BTN_PREVIEW_PAUSE,       _T("Pause the preview animation."), false },
        { IDC_BTN_PREVIEW_RESUME,      _T("Resume the paused preview animation."), false },
        { IDC_BTN_PREVIEW_STOP,        _T("Stop the preview and reset to time zero."), false },
        { IDC_BTN_PREVIEW_ZOOM_IN,     _T("Zoom the preview canvas in."), false },
        { IDC_BTN_PREVIEW_ZOOM_OUT,    _T("Zoom the preview canvas out."), false },
        { IDC_BTN_PREVIEW_FIT,         _T("Fit the entire scenario in the preview canvas."), false },
        { IDC_CHK_PREVIEW_LABELS,      _T("Show entity labels in the preview."), false },
        { IDC_CHK_PREVIEW_TRAILS,      _T("Show motion trails behind moving entities."), false },
        { IDC_CHK_PREVIEW_PATHS,       _T("Show configured motion paths."), false },
        { IDC_CHK_PREVIEW_ORIENTATION, _T("Show heading vectors on each entity."), false },
        { IDC_CHK_PREVIEW_TERRAIN,     _T("Show the level's land/ocean footprint (the big terrain box)."), false },
        { IDC_CHK_PREVIEW_ZONES,       _T("Show the level's named zones (e.g. Palm Forest) — independent of the terrain box."), false },
        { IDC_CHK_PREVIEW_LEGEND,      _T("Show size (per-area edge distances) and N/S/E/W direction labels."), false },
        { IDC_BTN_PREVIEW_SET_START,   _T("Click, then click an ellipse to set where that entity starts."), false },
        { IDC_BTN_PREVIEW_SHOW_ORIGIN, _T("Draw a red X at the scenario origin (ENU 0,0) and center the view on it."), false },
        { IDC_BTN_PREVIEW_GOTO_ORIGIN, _T("Move the camera over the scenario origin, keeping the current height/zoom."), false },
    };

    // Split a display name into base + trailing "-<digits>" index.
    // "Garc-3" -> ("Garc", 3); "Garc" -> ("Garc", 0).
    void SplitNameIndex(const std::string& nm, std::string& base, int& idx)
    {
        base = nm;
        idx  = 0;
        const size_t dash = nm.find_last_of('-');
        if (dash != std::string::npos && dash > 0 && dash + 1 < nm.size())
        {
            bool allDigit = true;
            for (size_t i = dash + 1; i < nm.size(); ++i)
                if (nm[i] < '0' || nm[i] > '9') { allDigit = false; break; }
            if (allDigit)
            {
                int v = 0;
                for (size_t i = dash + 1; i < nm.size(); ++i) v = v * 10 + (nm[i] - '0');
                idx  = v;
                base = nm.substr(0, dash);
            }
        }
    }

    // The label shown for an entity (preview uses marking when present, else name).
    std::string EntityDisplayName(const Entity& e)
    {
        return e.marking.empty() ? e.name : e.marking;
    }

    // Next "base-N" label for a duplicate of `srcLabel`, not colliding with any
    // existing entity's display name (Garc -> Garc-1, Garc-1 -> Garc-2, ...).
    std::string MakeDuplicateName(const Scenario& sc, const std::string& srcLabel)
    {
        std::string base; int srcIdx;
        SplitNameIndex(srcLabel, base, srcIdx);
        int maxIdx = 0;
        for (const Entity& e : sc.entities)
        {
            std::string b; int i;
            SplitNameIndex(EntityDisplayName(e), b, i);
            if (b == base && i > maxIdx) maxIdx = i;
        }
        return base + "-" + std::to_string(maxIdx + 1);
    }

    // Tiny modal text prompt (reuses the IDD_PROMPT_NAME dialog template, same
    // pattern as CatalogEditorDialog's prompt).
    class CPromptDialog : public CDialogEx
    {
    public:
        enum { IDD = IDD_PROMPT_NAME };
        CPromptDialog(const CString& caption, const CString& prompt,
                      const CString& initial, CWnd* parent)
            : CDialogEx(IDD, parent), m_caption(caption), m_prompt(prompt), m_value(initial)
        {}
        const CString& Value() const { return m_value; }
    protected:
        BOOL OnInitDialog() override
        {
            CDialogEx::OnInitDialog();
            SetWindowText(m_caption);
            SetDlgItemText(IDC_LBL_PROMPT,  m_prompt);
            SetDlgItemText(IDC_EDIT_PROMPT, m_value);
            if (CEdit* e = (CEdit*)GetDlgItem(IDC_EDIT_PROMPT))
            {
                e->SetSel(0, -1);
                e->SetFocus();
                return FALSE;  // we set focus ourselves
            }
            return TRUE;
        }
        void OnOK() override
        {
            GetDlgItemText(IDC_EDIT_PROMPT, m_value);
            CDialogEx::OnOK();
        }
        CString m_caption, m_prompt, m_value;
    };

    // Modal picker for the Entity Ellipse target: a drop-down of candidate entity
    // labels (the caller supplies the list, already excluding the orbiter). The
    // selected 0-based item index is read back via Selection().
    class CEntityPickerDialog : public CDialogEx
    {
    public:
        enum { IDD = IDD_ENTITY_PICKER };
        CEntityPickerDialog(const std::vector<CString>& items, CWnd* parent)
            : CDialogEx(IDD, parent), m_items(items) {}
        int Selection() const { return m_sel; }
    protected:
        BOOL OnInitDialog() override
        {
            CDialogEx::OnInitDialog();
            if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_TARGET_ENTITY))
            {
                for (const CString& s : m_items) cb->AddString(s);
                cb->SetCurSel(m_items.empty() ? -1 : 0);
                cb->SetFocus();
                return FALSE;  // we set focus ourselves
            }
            return TRUE;
        }
        void OnOK() override
        {
            if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_TARGET_ENTITY))
                m_sel = cb->GetCurSel();
            CDialogEx::OnOK();
        }
        std::vector<CString> m_items;
        int m_sel = -1;
    };

    // Modal "Terminate Explosion": the source entity (read-only), a Target Entity
    // drop-down, when the impact happens (auto = intercept at cruise speed, or a
    // typed time) and where on the target (forward / right / up metres in the
    // target's own frame). The caller supplies the candidate targets, already
    // excluding the source, plus a size line per candidate.
    class CTerminateExplosionDialog : public CDialogEx
    {
    public:
        enum { IDD = IDD_TERMINATE_EXPLOSION };
        CTerminateExplosionDialog(const CString& source,
                                  const std::vector<CString>& targets,
                                  const std::vector<CString>& targetSizes, CWnd* parent)
            : CDialogEx(IDD, parent), m_source(source), m_targets(targets),
              m_sizes(targetSizes) {}

        // In: initial values. Out: accepted values.
        int    m_sel       = 0;       // index into targets
        bool   m_autoTime  = true;
        double m_timeSec   = 0.0;
        double m_minTime   = 0.0;     // the leg's start: an impact must come after it
        double m_forwardM  = 0.0;
        double m_rightM    = 0.0;
        double m_upM       = 0.0;
        // New explosions: the Up box follows the chosen target's suggested aim
        // height (on a ship's hull rather than its waterline). Empty = never.
        std::vector<double> m_upDefaults;

    protected:
        BOOL OnInitDialog() override
        {
            CDialogEx::OnInitDialog();
            SetDlgItemText(IDC_EDIT_TERM_SOURCE, m_source);
            if (CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_TERM_TARGET))
            {
                for (const CString& s : m_targets) cb->AddString(s);
                cb->SetCurSel(m_targets.empty() ? -1 : m_sel);
            }
            CheckDlgButton(IDC_CHK_TERM_AUTO_TIME, m_autoTime ? BST_CHECKED : BST_UNCHECKED);
            CString s;
            s.Format(_T("%.2f"), m_timeSec);  SetDlgItemText(IDC_EDIT_TERM_TIME, s);
            s.Format(_T("%.1f"), m_forwardM); SetDlgItemText(IDC_EDIT_TERM_FORWARD, s);
            s.Format(_T("%.1f"), m_rightM);   SetDlgItemText(IDC_EDIT_TERM_RIGHT, s);
            s.Format(_T("%.1f"), m_upM);      SetDlgItemText(IDC_EDIT_TERM_UP, s);
            SyncControls();
            SeedUpFromTarget();
            if (CWnd* cb = GetDlgItem(IDC_COMBO_TERM_TARGET)) { cb->SetFocus(); return FALSE; }
            return TRUE;
        }
        BOOL OnCommand(WPARAM wParam, LPARAM lParam) override
        {
            const UINT id = LOWORD(wParam), code = HIWORD(wParam);
            if ((id == IDC_COMBO_TERM_TARGET && code == CBN_SELCHANGE) ||
                (id == IDC_CHK_TERM_AUTO_TIME && code == BN_CLICKED))
                SyncControls();
            if (id == IDC_COMBO_TERM_TARGET && code == CBN_SELCHANGE)
                SeedUpFromTarget();
            return CDialogEx::OnCommand(wParam, lParam);
        }
        // Size line for the chosen target; time box only when not automatic.
        void SyncControls()
        {
            const CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_TERM_TARGET);
            const int sel = cb ? cb->GetCurSel() : -1;
            SetDlgItemText(IDC_LBL_TERM_TARGET_SIZE,
                           (sel >= 0 && sel < static_cast<int>(m_sizes.size())) ? m_sizes[sel] : CString());
            if (CWnd* t = GetDlgItem(IDC_EDIT_TERM_TIME))
                t->EnableWindow(IsDlgButtonChecked(IDC_CHK_TERM_AUTO_TIME) != BST_CHECKED);
        }
        // Put the chosen target's suggested aim height in the Up box.
        void SeedUpFromTarget()
        {
            const CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_TERM_TARGET);
            const int sel = cb ? cb->GetCurSel() : -1;
            if (sel < 0 || sel >= static_cast<int>(m_upDefaults.size())) return;
            CString s;
            s.Format(_T("%.1f"), m_upDefaults[static_cast<size_t>(sel)]);
            SetDlgItemText(IDC_EDIT_TERM_UP, s);
        }
        void OnOK() override
        {
            const CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_TERM_TARGET);
            m_sel = cb ? cb->GetCurSel() : -1;
            if (m_sel < 0) { AfxMessageBox(_T("Choose a target entity.")); return; }
            m_autoTime = IsDlgButtonChecked(IDC_CHK_TERM_AUTO_TIME) == BST_CHECKED;
            CString s;
            GetDlgItemText(IDC_EDIT_TERM_TIME, s);    m_timeSec  = _tstof(s);
            GetDlgItemText(IDC_EDIT_TERM_FORWARD, s); m_forwardM = _tstof(s);
            GetDlgItemText(IDC_EDIT_TERM_RIGHT, s);   m_rightM   = _tstof(s);
            GetDlgItemText(IDC_EDIT_TERM_UP, s);      m_upM      = _tstof(s);
            if (!m_autoTime && m_timeSec <= m_minTime)
            {
                CString msg;
                msg.Format(_T("The impact time must be after the leg starts (%.2f s)."), m_minTime);
                AfxMessageBox(msg);
                return;
            }
            CDialogEx::OnOK();
        }
        CString              m_source;
        std::vector<CString> m_targets;
        std::vector<CString> m_sizes;
    };

    // Modal "Add Location": label + latitude + longitude + altitude. The numeric
    // fields are prefilled from the captured/current view so the common case is
    // one keystroke (the label). Altitude is the eye/viewing height in metres.
    class CAddPlaceDialog : public CDialogEx
    {
    public:
        enum { IDD = IDD_ADD_PLACE };
        CAddPlaceDialog(double lat, double lon, double alt, CWnd* parent)
            : CDialogEx(IDD, parent), m_lat(lat), m_lon(lon), m_alt(alt) {}
        CString m_label;
        double  m_lat = 0.0;
        double  m_lon = 0.0;
        double  m_alt = 0.0;
    protected:
        BOOL OnInitDialog() override
        {
            CDialogEx::OnInitDialog();
            CString s;
            s.Format(_T("%.6f"), m_lat); SetDlgItemText(IDC_EDIT_PLACE_LAT, s);
            s.Format(_T("%.6f"), m_lon); SetDlgItemText(IDC_EDIT_PLACE_LON, s);
            s.Format(_T("%.1f"), m_alt); SetDlgItemText(IDC_EDIT_PLACE_ALT, s);
            if (CEdit* e = (CEdit*)GetDlgItem(IDC_EDIT_PLACE_LABEL)) { e->SetFocus(); return FALSE; }
            return TRUE;
        }
        void OnOK() override
        {
            GetDlgItemText(IDC_EDIT_PLACE_LABEL, m_label);
            m_label.Trim();
            if (m_label.IsEmpty()) { AfxMessageBox(_T("Enter a label for the location.")); return; }
            CString la, lo, al;
            GetDlgItemText(IDC_EDIT_PLACE_LAT, la);
            GetDlgItemText(IDC_EDIT_PLACE_LON, lo);
            GetDlgItemText(IDC_EDIT_PLACE_ALT, al);
            m_lat = _tstof(la);
            m_lon = _tstof(lo);
            m_alt = _tstof(al);
            if (m_lat < -90.0 || m_lat > 90.0 || m_lon < -180.0 || m_lon > 180.0)
            {
                AfxMessageBox(_T("Latitude must be -90..90 and longitude -180..180."));
                return;
            }
            CDialogEx::OnOK();
        }
    };

    double SegmentGeoLength(const MotionSegment& s, const Scenario* scn);  // fwd

    // Make an entity's whole motion path span `D` seconds WITHOUT altering speed
    // (speed is the catalog physics). Each moving leg keeps its natural duration
    // (length / speed; one orbit for an ellipse) and holds keep their dwell; the
    // legs are packed contiguously. Any time left over (D beyond the natural path
    // length) is handed to the FINAL leg: a terminal ellipse simply orbits for
    // longer, a terminal line loiters at its destination. This avoids stretching
    // earlier legs into long loiters that would push a trailing orbit out of the
    // playback window. Never compresses below the natural travel time. No-op if
    // there are no enabled segments or D <= 0.
    void RescalePathDuration(Entity& e, double D, const Scenario* scn)
    {
        if (D <= 0.0) return;
        std::vector<size_t> segs;
        for (size_t k = 0; k < e.motionSegments.size(); ++k)
            if (e.motionSegments[k].enabled) segs.push_back(k);
        if (segs.empty()) return;

        const double t0 = e.motionSegments[segs.front()].startSecond;
        double t = t0;
        for (size_t k : segs)
        {
            MotionSegment& s = e.motionSegments[k];
            double dur;
            if ((s.type == MotionType::Line || s.type == MotionType::Ellipse) &&
                s.speedMps > 0.0)
            {
                const double len = SegmentGeoLength(s, scn);
                dur = (len > 0.0) ? len / s.speedMps : 0.0;
            }
            else
            {
                dur = s.endSecond - s.startSecond;   // hold: keep dwell time
                if (dur < 0.0) dur = 0.0;
            }
            s.startSecond = t;
            s.endSecond   = t + dur;
            t += dur;
        }

        // D is a TIME BUDGET, not a speed command. Speed stays authoritative
        // (the sampler advances Line/Ellipse legs at speedMps and clamps travel
        // to the geometry), so when D is shorter than the path's natural
        // traversal the entity simply stops partway -- we never speed it up to
        // make it fit, and we never refuse the duration because the path is too
        // long. Set the overall window to exactly [t0, t0+D]: truncate the leg
        // the boundary lands in and collapse any later legs to a zero-length
        // window so they are never reached.
        const double target = t0 + D;
        bool boundaryPassed = false;
        for (size_t k : segs)
        {
            MotionSegment& s = e.motionSegments[k];
            if (boundaryPassed) { s.startSecond = target; s.endSecond = target; continue; }
            if (s.endSecond >= target) { s.endSecond = target; boundaryPassed = true; }
        }

        // When D outruns the whole path, hand the slack to the final leg so a
        // trailing orbit / hold fills the remaining time.
        MotionSegment& last = e.motionSegments[segs.back()];
        if (target > last.endSecond) last.endSecond = target;
    }

    // Geometric length of one segment (one cycle for an ellipse), in metres,
    // by sampling start->end and summing ENU chord distances.
    double SegmentGeoLength(const MotionSegment& s, const Scenario* scn)
    {
        if (!scn) return 0.0;
        const int N = 24;
        double len = 0.0, pe = 0.0, pn = 0.0, pu = 0.0;
        for (int i = 0; i <= N; ++i)
        {
            const double u = static_cast<double>(i) / static_cast<double>(N);
            const SampledPose p = MotionSampler::EvaluateSegment(s, u, scn, true);
            double ee, nn, uu;
            CoordTransforms::EcefToLocalEnuDeg(
                p.ecefX, p.ecefY, p.ecefZ,
                scn->originLatDeg, scn->originLonDeg, scn->originAltM, ee, nn, uu);
            if (i > 0)
            {
                const double dx = ee - pe, dy = nn - pn, dz = uu - pu;
                len += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            pe = ee; pn = nn; pu = uu;
        }
        return len;
    }

    // Make ellipse `s` (Local coordMode) pass through point P at its start: set
    // the string length L=|P-F1|+|P-F2| and startBearing = compass(P - midpoint).
    void SnapEllipse(MotionSegment& s, double pE, double pN)
    {
        const double f1e = s.f1LocalX, f1n = s.f1LocalY;
        const double f2e = s.f2LocalX, f2n = s.f2LocalY;
        const double d1 = std::sqrt((pE - f1e) * (pE - f1e) + (pN - f1n) * (pN - f1n));
        const double d2 = std::sqrt((pE - f2e) * (pE - f2e) + (pN - f2n) * (pN - f2n));
        s.lengthMeters = d1 + d2;
        const double cE = 0.5 * (f1e + f2e), cN = 0.5 * (f1n + f2n);
        double brg = std::atan2(pE - cE, pN - cN) * (180.0 / 3.14159265358979323846);
        if (brg < 0.0) brg += 360.0;
        s.startBearingDeg = brg;
    }

    // Refresh an entity's path to the catalog cruise speed: set each MOVING
    // (Line/Ellipse) segment's speedMps to `cruise`. The segment TIME WINDOWS
    // (durations) are deliberately LEFT UNTOUCHED -- a leg's allotted duration is
    // set independently (via Set Duration) and represents how long the entity is
    // budgeted on that leg, which may be shorter, equal to, or longer than the
    // path's natural traversal time. Refresh Speed only re-pulls the physics
    // (catalog speed); it must not overwrite the authored duration.
    void RefreshPathSpeed(Entity& e, double cruise, const Scenario* /*scn*/)
    {
        if (cruise <= 0.0) return;
        for (MotionSegment& s : e.motionSegments)
        {
            if (!s.enabled) continue;
            if (s.type == MotionType::Line || s.type == MotionType::Ellipse)
                s.speedMps = cruise;
        }
    }

    // Preview playback-rate choices. One table drives the combo's strings, the
    // multiplier a selection means, and the reverse lookup that restores a saved
    // PlaySpeed - so the labels and the numbers cannot drift apart. Large
    // multipliers exist for wide-area scenarios where a single lap/leg spans
    // tens of km.
    struct PlaySpeedChoice { const wchar_t* label; double mul; };
    constexpr PlaySpeedChoice kPlaySpeeds[] = {
        { L"1x",       1.0 },
        { L"2x",       2.0 },
        { L"5x",       5.0 },
        { L"10x",     10.0 },
        { L"30x",     30.0 },
        { L"60x",     60.0 },
        { L"120x",   120.0 },
        { L"300x",   300.0 },
        { L"600x",   600.0 },
        { L"1200x", 1200.0 },
    };
    constexpr int kPlaySpeedDefault = 3;   // "10x" - fast enough that motion reads

    // Camera-strip time-scale choices (seconds represented by kRefPx pixels).
    // Smaller value => more pixels/second => wider boxes. Order matches the combo.
    struct CamScale { const wchar_t* label; double seconds; };
    constexpr CamScale kCamScales[] = {
        { L"10 ms",  0.01 },
        { L"100 ms", 0.10 },
        { L"1 s",    1.00 },
        { L"2 s",    2.00 },
        { L"5 s",    5.00 },
        { L"10 s",  10.00 },
        { L"100 s", 100.00 },
    };
    constexpr int kCamScaleDefault = 2;   // "1 s"

    // How much bigger than the system glyph the owner-draw "Show" check boxes are
    // drawn. Windows sizes a check box for an 8pt dialog font; this page is 12pt,
    // so the stock 13px box reads as a speck next to its own label. 2x lands it at
    // roughly the height of the text beside it.
    constexpr int    kCheckScale     = 2;
    // The logical (96-DPI) side of a stock Windows check box glyph, which the
    // system does not expose as a metric.
    constexpr int    kCheckBaseLogicalPx = 13;
    // Gap between the box and its label, as a fraction of the box size.
    constexpr double kCheckLabelGap  = 0.45;

    // Every owner-draw check box on this page, in one place so the DrawItem
    // dispatch and the resource file cannot drift apart.
    constexpr int kCheckBoxIds[] = {
        IDC_CHK_PREVIEW_LABELS,
        IDC_CHK_PREVIEW_TRAILS,
        IDC_CHK_PREVIEW_PATHS,
        IDC_CHK_PREVIEW_ORIENTATION,
        IDC_CHK_PREVIEW_TERRAIN,
        IDC_CHK_PREVIEW_ZONES,
        IDC_CHK_PREVIEW_LEGEND,
        IDC_CHK_PREVIEW_DESTTIME,
        IDC_CHK_MAP_MOVE_ENTITIES,
    };
    bool IsPreviewCheckBox(int id)
    {
        for (int i : kCheckBoxIds) if (i == id) return true;
        return false;
    }

    // Sample N evenly-spaced points along [0, duration] for FitScenario
    // and the per-entity motion-path cache.
    constexpr size_t kFitSampleCount  = 32;
    // FitScenario ignores entity positions farther than this from the origin.
    // A flat ENU fit only makes sense near the origin; a > 5000 km offset means
    // the throwaway default entity (origin still at 0,0) or a scenario whose
    // origin was relocated away from its entities — fitting to it just zooms into
    // a phantom point and blanks the map. Those fall through to the origin-framed
    // fallback so the satellite/map backdrop stays visible.
    constexpr double kFitMaxOffsetM   = 5.0e6;   // 5000 km
    // Smallest extent FitScenario will frame: a lone stationary entity is shown
    // with this much map around it rather than zoomed to a single dot.
    constexpr double kFitMinExtentM   = 5000.0;  // 5 km
    // Default zoom when there's nothing meaningful to fit (empty/fresh scenario, or
    // all entities skipped as far-flung): a comfortable ~10 km-across view of the
    // origin so the satellite/map backdrop is visible and navigable.
    constexpr double kFreshViewMetersPerPx = 8.0;
    // (path sampling now lives per-segment inside RebuildPathsCache —
    // see kLinePts / kEllipsePts there)
}

BEGIN_MESSAGE_MAP(CPreviewPage, CHelpAwarePage)
    ON_WM_CTLCOLOR()
    ON_WM_DESTROY()
    ON_WM_TIMER()
    ON_WM_HSCROLL()
    ON_WM_SHOWWINDOW()
    ON_CBN_SELCHANGE(IDC_COMBO_CAM_ENTITY,     &CPreviewPage::OnCamEntityChanged)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_START,       &CPreviewPage::OnStart)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_PAUSE,       &CPreviewPage::OnPause)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_RESUME,      &CPreviewPage::OnResume)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_STOP,        &CPreviewPage::OnStop)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_ZOOM_IN,     &CPreviewPage::OnZoomIn)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_ZOOM_OUT,    &CPreviewPage::OnZoomOut)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_FIT,         &CPreviewPage::OnFit)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_NEW,         &CPreviewPage::OnNewEntity)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_NEW_CAMERA,  &CPreviewPage::OnNewCamera)
    ON_BN_CLICKED(IDC_BTN_CAM_ADD,             &CPreviewPage::OnAddCamera)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_LABELS,      &CPreviewPage::OnLabelsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_TRAILS,      &CPreviewPage::OnTrailsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_PATHS,       &CPreviewPage::OnPathsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_ORIENTATION, &CPreviewPage::OnOrientationToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_TERRAIN,     &CPreviewPage::OnTerrainToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_ZONES,       &CPreviewPage::OnZonesToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_LEGEND,      &CPreviewPage::OnLegendToggle)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_SHOW_ORIGIN, &CPreviewPage::OnShowOriginToggle)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_GOTO_ORIGIN, &CPreviewPage::OnGotoOrigin)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_SET_START,   &CPreviewPage::OnSetStart)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_BOUNDARY,    &CPreviewPage::OnBoundary)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_FOLIAGE,     &CPreviewPage::OnFoliage)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_BUILD_TERRAIN, &CPreviewPage::OnBuildTerrain)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_BUILD_FOLIAGE, &CPreviewPage::OnBuildFoliage)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_PURGE_TERRAIN, &CPreviewPage::OnPurgeTerrain)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_FOLIAGE_AREA,  &CPreviewPage::OnFoliageArea)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_TERRAIN_START, &CPreviewPage::OnStartTerrainServer)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_TERRAIN_STOP,  &CPreviewPage::OnStopTerrainServer)
    ON_CBN_SELCHANGE(IDC_COMBO_PREVIEW_SPEED,  &CPreviewPage::OnSpeedChange)
    ON_CBN_SELCHANGE(IDC_COMBO_CAM_SCALE,      &CPreviewPage::OnCamScaleChange)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_DESTTIME,    &CPreviewPage::OnDestTimeToggle)
    ON_CBN_SELCHANGE(IDC_COMBO_MAP_LAYER,      &CPreviewPage::OnMapLayerChange)
    ON_CBN_SELCHANGE(IDC_COMBO_MAP_PLACE,      &CPreviewPage::OnMapPlaceChange)
    ON_BN_CLICKED(IDC_BTN_MAP_ADDPLACE,        &CPreviewPage::OnMapAddPlace)
    ON_BN_CLICKED(IDC_BTN_MAP_DELPLACE,        &CPreviewPage::OnMapDelPlace)
    ON_BN_CLICKED(IDC_BTN_MAP_CAPTURE,         &CPreviewPage::OnMapCapturePlace)
    ON_BN_CLICKED(IDC_CHK_MAP_MOVE_ENTITIES,   &CPreviewPage::OnMapMoveEntitiesToggle)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST_PREVIEW_DESTTIME, &CPreviewPage::OnPropertiesItemChanged)
    ON_WM_DRAWITEM()
    ON_MESSAGE(WM_APP_PROC_OUTPUT, &CPreviewPage::OnProcOutput)
    ON_MESSAGE(WM_APP_PROC_EXIT,   &CPreviewPage::OnProcExit)
    ON_MESSAGE(WM_APP_TERRAIN_JOB_LINE, &CPreviewPage::OnTerrainJobLine)
    ON_MESSAGE(WM_APP_TERRAIN_JOB_DONE, &CPreviewPage::OnTerrainJobDone)
END_MESSAGE_MAP()

//
// GetFieldHelpTable — hand the base help system this page's per-control help table.
//
void CPreviewPage::GetFieldHelpTable(const FFieldHelp*& outArray, size_t& outCount) const
{
    outArray = kFields;
    outCount = sizeof(kFields) / sizeof(kFields[0]);
}

//
// OnCtlColor — paint the dark theme: white text on black for the page,
//   static labels, declassic'd checkboxes, and combo drop-down lists.
//
HBRUSH CPreviewPage::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    if (!m_blackBrush.GetSafeHandle() || !m_whiteBrush.GetSafeHandle())
        return CHelpAwarePage::OnCtlColor(pDC, pWnd, nCtlColor);

    switch (nCtlColor)
    {
        case CTLCOLOR_DLG:
        case CTLCOLOR_STATIC:   // labels ("Speed:", "Time:") + declassic'd checkboxes
            pDC->SetBkMode(TRANSPARENT);
            pDC->SetTextColor(RGB(255, 255, 255));
            pDC->SetBkColor(RGB(0, 0, 0));
            return static_cast<HBRUSH>(m_blackBrush);
        case CTLCOLOR_LISTBOX:  // combo drop-down list
            pDC->SetTextColor(RGB(255, 255, 255));
            pDC->SetBkColor(RGB(0, 0, 0));
            return static_cast<HBRUSH>(m_blackBrush);
        default:
            return CHelpAwarePage::OnCtlColor(pDC, pWnd, nCtlColor);
    }
}

//
// OnInitDialog — one-time page setup: dark theme, subclass the canvas, seed the
//   checkboxes / speed & map-layer combos / places list / dest-time grid, size
//   the time slider, build the first render state, and start the server poll.
//
BOOL CPreviewPage::OnInitDialog()
{
    CHelpAwarePage::OnInitDialog();

    LOG("PreviewPage::OnInitDialog: entered");

    // Dark theme (matches the Run / Deploy tabs). Black page background + white
    // label text; declassic the checkboxes so their labels honor the text color.
    m_blackBrush.CreateSolidBrush(RGB(0, 0, 0));
    m_whiteBrush.CreateSolidBrush(RGB(255, 255, 255));
    SetBackgroundColor(RGB(0, 0, 0));
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

    m_canvas.SetOwner(this);   // pointer-only; safe to set before subclass
    BOOL subOk = m_canvas.SubclassDlgItem(IDC_STATIC_PREVIEW_CANVAS, this);
    {
        char buf[256];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage: SubclassDlgItem result=%d, canvasHwnd=%p, scenario=%p",
                  (int)subOk, (void*)m_canvas.GetSafeHwnd(), (void*)m_scenario);
        LOG(buf);
    }

    // Camera timeline strip (below the canvas). Same subclass pattern as the canvas.
    m_timeline.SetOwner(this);
    m_timeline.SubclassDlgItem(IDC_CAMERA_TIMELINE, this);

    // The "Show" check boxes are seeded from the scenario's [Preview] section by
    // ApplyPreviewViewFromScenario near the end of this function — they are
    // per-scenario state now, not per-user, so there is nothing to restore here.

    if (CComboBox* spd = (CComboBox*)GetDlgItem(IDC_COMBO_PREVIEW_SPEED))
    {
        spd->ResetContent();
        for (const PlaySpeedChoice& c : kPlaySpeeds) spd->AddString(c.label);
        spd->SetCurSel(kPlaySpeedDefault);   // overridden below by the scenario's [Preview]
    }

    if (CListCtrl* dt = (CListCtrl*)GetDlgItem(IDC_LIST_PREVIEW_DESTTIME))
    {
        dt->SetExtendedStyle(dt->GetExtendedStyle() | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        dt->SetBkColor(RGB(0, 0, 0));       // dark theme to match the page
        dt->SetTextBkColor(RGB(0, 0, 0));
        dt->SetTextColor(RGB(255, 255, 255));
        // Duration  = the entity's scheduled path window (last end - first start).
        // Path Time = geometric traversal time (sum of segment length / speed).
        // Cruise    = the entity type's catalog CruiseSpeedMetersPerSecond.
        dt->InsertColumn(0, _T("Entity"),         LVCFMT_LEFT,   10);
        dt->InsertColumn(1, _T("Duration (s)"),   LVCFMT_RIGHT,  10);
        dt->InsertColumn(2, _T("Path Time (s)"),  LVCFMT_RIGHT,  10);
        dt->InsertColumn(3, _T("Cruise (m/sec)"), LVCFMT_RIGHT,  10);
        dt->InsertColumn(4, _T("Infinite"),       LVCFMT_CENTER, 10);

        // Size columns to fill the control's real client width. Fixed raw-pixel
        // widths render too narrow on high-DPI displays (headers truncate to
        // "E...", "D..."); proportional sizing stays readable at any DPI. Reserve
        // the vertical scrollbar width so a full list doesn't force a horizontal
        // scrollbar. Last column takes the remainder so the widths sum exactly.
        CRect rc; dt->GetClientRect(&rc);
        const int avail = rc.Width() - ::GetSystemMetrics(SM_CXVSCROLL) - 4;
        if (avail > 0)
        {
            const int pct[4] = { 28, 18, 18, 22 };   // Entity, Duration, Path, Cruise
            int used = 0;
            for (int c = 0; c < 4; ++c)
            {
                const int cw = MulDiv(avail, pct[c], 100);
                dt->SetColumnWidth(c, cw);
                used += cw;
            }
            dt->SetColumnWidth(4, avail - used);     // Infinite = remainder
        }
        dt->ShowWindow(SW_HIDE);   // revealed when the Show > "Properties" box is checked
    }

    // Map backdrop layer selector.
    if (CComboBox* mc = (CComboBox*)GetDlgItem(IDC_COMBO_MAP_LAYER))
    {
        mc->ResetContent();
        mc->AddString(_T("None"));
        mc->AddString(_T("Satellite"));
        mc->AddString(_T("Topographic"));
        mc->AddString(_T("Cesium (3D)"));
        mc->SetCurSel(static_cast<int>(m_mapLayer));   // MapLayer enum == index
    }

    // Location drop-down: seed a default the first time so the list isn't empty.
    if (theApp.Settings().mapPlaces.empty())
    {
        MapPlace hb; hb.label = "Hurlburt Field, Florida";
        hb.lat = 30.4275; hb.lon = -86.6893;
        theApp.Settings().mapPlaces.push_back(hb);
        SettingsIO::Save(theApp.Settings(), theApp.SettingsPath());
    }
    PopulatePlacesCombo();

    // Camera-strip time-scale dropdown: seed the choices and apply the default
    // scale to the timeline so boxes render at a known zoom.
    if (CComboBox* sc = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_SCALE))
    {
        sc->ResetContent();
        for (const CamScale& s : kCamScales) sc->AddString(s.label);
        sc->SetCurSel(kCamScaleDefault);
        m_timeline.SetTimeScale(kCamScales[kCamScaleDefault].seconds);
    }

    // Camera dropdowns (entity / preset / target / transition) + normalize any
    // cameras loaded with the scenario so the schedule tiles the timeline.
    PopulateCameraCombos();
    NormalizeCameraSchedule();
    RefreshCameraTargetExtents();   // entity types may have changed since authoring
    UpdateCameraScrollBar();

    UpdateSliderRange();
    if (CSliderCtrl* s = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME))
        s->SetPos(0);

    RebuildPathsCache();
    FitScenario();
    RebuildRenderState();

    // Restore this scenario's saved view drop-downs. Applied AFTER FitScenario so
    // a scenario saved on the Cesium layer opens the globe framed on the fitted
    // view rather than on the pre-fit default zoom.
    ApplyPreviewViewFromScenario();

    // Detect whether the self-hosted terrain server is already running (it may have
    // been left up from a previous run — we never kill it on our own exit), then poll
    // periodically so the Start check + Boundary/Build gating track it live.
    RefreshTerrainServerUi();
    SetTimer(kServerPollTimerId, 1500, nullptr);

    return TRUE;
}

void CPreviewPage::OnDestroy()
{
    StopTimer();
    KillTimer(kServerPollTimerId);
    // NOTE: intentionally do NOT stop the terrain server here — closing the editor
    // leaves the tile server running (per design), so a live DISBrowser keeps its terrain.
    CHelpAwarePage::OnDestroy();
}

void CPreviewPage::OnShowWindow(BOOL bShow, UINT nStatus)
{
    CHelpAwarePage::OnShowWindow(bShow, nStatus);
    if (bShow && m_scenario)
    {
        // Re-fit whenever the preview becomes visible — covers the "load a
        // different scenario then switch tabs" case without forcing the user
        // to click Fit explicitly. Slider range may also need updating.
        UpdateSliderRange();
        PopulateCameraCombos();   // entity list may have changed on other tabs
        RebuildPathsCache();
        FitScenario();
        RebuildRenderState();
        UpdateCameraScrollBar();  // duration may have changed on another tab
        if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
        if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    }
}

//
// OnMapLayerChange — map-layer combo changed: update m_mapLayer, show/hide the
//   Cesium globe vs the 2D canvas, and repaint.
//
void CPreviewPage::OnMapLayerChange()
{
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MAP_LAYER);
    if (!cb) return;
    const int sel = cb->GetCurSel();
    m_mapLayer = (sel <= 0) ? MapLayer::None : static_cast<MapLayer>(sel);
    ActivateCesium(m_mapLayer == MapLayer::Cesium);
    PersistPreviewView();
    RefreshMapView();
}

namespace
{
    // Force-ID palette mirrored from the canvas (Other/Friendly/Opposing/Neutral).
    unsigned int ForceColorRgb(uint8_t forceId)
    {
        switch (forceId)
        {
        case 1:  return 0x78BEFF;   // Friendly (light blue)
        case 2:  return 0xD23A2A;   // Opposing
        case 3:  return 0xE0B020;   // Neutral
        default: return 0x808080;   // Other
        }
    }
}

void CPreviewPage::ActivateCesium(bool on)
{
    if (on)
    {
        // Place the globe over the same rect as the 2D canvas, hide the canvas.
        CRect wr;
        if (m_canvas.GetSafeHwnd()) { m_canvas.GetWindowRect(&wr); ScreenToClient(&wr); }
        else wr = CRect(0, 0, 400, 300);

        if (!m_cesium.GetSafeHwnd()) m_cesium.Create(this, wr);
        else                         m_cesium.SetBounds(wr);
        m_cesium.ShowView(true);
        if (m_canvas.GetSafeHwnd()) m_canvas.ShowWindow(SW_HIDE);

        // Route a globe-painted boundary box back into the same handler the 2D canvas uses.
        m_cesium.SetBoundsCallback([this](double laMn, double laMx, double loMn, double loMx)
        {
            OnBoundaryPainted(laMn, laMx, loMn, loMx);
        });
        // Carry the current armed state onto the globe so the gesture is identical either way.
        m_cesium.SetBoundaryMode(m_boundaryArmed);

        FlyCesiumToOrigin();
        PushCesiumEntities();
    }
    else
    {
        if (m_cesium.GetSafeHwnd()) m_cesium.ShowView(false);
        if (m_canvas.GetSafeHwnd()) m_canvas.ShowWindow(SW_SHOW);
    }
}

double CPreviewPage::CurrentViewAltitude()
{
    // Eye/viewing altitude that frames the current 2D view: the ground span
    // visible in the canvas, clamped to something sensible. Used to seed a
    // location's altitude and to open the globe at the same framing.
    double canvasH = 400.0;
    if (m_canvas.GetSafeHwnd()) { CRect rc; m_canvas.GetClientRect(&rc); canvasH = std::max<LONG>(rc.Height(), 200); }
    return std::max(3000.0, m_zoomMetersPerPx * canvasH * 1.3);
}

void CPreviewPage::FlyCesiumToOrigin()
{
    if (m_mapLayer != MapLayer::Cesium || !m_cesium.GetSafeHwnd() || !m_scenario) return;

    // Camera height ~ the ground span currently visible in the 2D view, so the
    // globe opens framed on the same area.
    m_cesium.FlyTo(m_scenario->originLatDeg, m_scenario->originLonDeg, CurrentViewAltitude());
}

//
// PushCesiumEntities — convert each current entity pose (ENU) to lat/lon/alt and
//   send it, colored by force, to the 3D globe. No-op unless Cesium is active.
//
void CPreviewPage::PushCesiumEntities()
{
    if (m_mapLayer != MapLayer::Cesium || !m_cesium.GetSafeHwnd() || !m_scenario) return;

    std::vector<CesiumEntityPt> pts;
    pts.reserve(m_state.poses.size());
    for (const PreviewEntityPose& p : m_state.poses)
    {
        if (!p.enabled || p.exploded) continue;
        double X, Y, Z, lat, lon, alt;
        CoordTransforms::LocalEnuToEcefDeg(p.enuE, p.enuN, p.enuU,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM, X, Y, Z);
        CoordTransforms::EcefToGeodeticDeg(X, Y, Z, lat, lon, alt);

        CesiumEntityPt e;
        e.name     = p.name;
        e.lat      = lat;
        e.lon      = lon;
        e.altM     = alt;
        e.colorRgb = ForceColorRgb(p.forceId);
        pts.push_back(std::move(e));
    }
    m_cesium.SetEntities(pts);
}

//
// OnMapPlaceChange — a saved Location was picked: relocate the map/scenario
//   origin to that place (flying the globe to its captured height if any).
//
void CPreviewPage::OnMapPlaceChange()
{
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MAP_PLACE);
    if (!cb) return;
    const int sel = cb->GetCurSel();
    const std::vector<MapPlace>& places = theApp.Settings().mapPlaces;
    if (sel < 0 || sel >= static_cast<int>(places.size())) return;
    const MapPlace& mp = places[static_cast<size_t>(sel)];
    PersistPreviewView();   // remember WHICH location this scenario is authored against
    ApplyMapPlace(mp.lat, mp.lon, mp.alt > 0.0 ? mp.alt : -1.0);
}

//
// CurrentViewLatLonAlt — resolve where the map view is currently centered to
//   geodetic lat/lon + eye altitude. Cesium (3D): the globe's reported look-at
//   point. 2D layers: the canvas center (m_centerEnuE/N) resolved against the
//   origin, with the 2D eye-height from zoom. Falls back to the scenario origin
//   when no scenario exists or the globe hasn't reported its position yet.
//
void CPreviewPage::CurrentViewLatLonAlt(double& lat, double& lon, double& alt)
{
    lat = m_scenario ? m_scenario->originLatDeg : 0.0;
    lon = m_scenario ? m_scenario->originLonDeg : 0.0;
    alt = CurrentViewAltitude();   // eye/viewing height (2D framing)

    if (m_mapLayer == MapLayer::Cesium)
    {
        // Globe center + eye altitude (where the user flew); leave the origin
        // fallback in place if it hasn't reported yet.
        m_cesium.GetLookAt(lat, lon, alt);
    }
    else if (m_scenario)
    {
        // m_centerEnuE/N are the ENU metres (East/North) from the origin at the
        // middle of the view; resolve them back to geodetic. Altitude stays the
        // 2D eye-height computed above.
        double X = 0.0, Y = 0.0, Z = 0.0;
        CoordTransforms::LocalEnuToEcefDeg(
            m_centerEnuE, m_centerEnuN, 0.0,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            X, Y, Z);
        double ga = 0.0;
        CoordTransforms::EcefToGeodeticDeg(X, Y, Z, lat, lon, ga);
    }
}

//
// OnMapAddPlace — "Add Location": prompt (prefilled from the current globe/canvas
//   view), append the new MapPlace, persist Settings, then relocate the origin there.
//
void CPreviewPage::OnMapAddPlace()
{
    double lat = 0.0, lon = 0.0, alt = 0.0;
    CurrentViewLatLonAlt(lat, lon, alt);
    CAddPlaceDialog dlg(lat, lon, alt, this);
    if (dlg.DoModal() != IDOK) return;

    MapPlace p;
    p.label = std::string(CStringA(dlg.m_label).GetString());
    p.lat   = dlg.m_lat;
    p.lon   = dlg.m_lon;
    p.alt   = dlg.m_alt;
    theApp.Settings().mapPlaces.push_back(p);
    SettingsIO::Save(theApp.Settings(), theApp.SettingsPath());

    PopulatePlacesCombo(static_cast<int>(theApp.Settings().mapPlaces.size()) - 1);
    PersistPreviewView();
    ApplyMapPlace(p.lat, p.lon, p.alt > 0.0 ? p.alt : -1.0);
}

//
// OnMapDelPlace — remove the selected saved Location, persist, and reselect a
//   neighboring entry.
//
void CPreviewPage::OnMapDelPlace()
{
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MAP_PLACE);
    if (!cb) return;
    const int sel = cb->GetCurSel();
    std::vector<MapPlace>& places = theApp.Settings().mapPlaces;
    if (sel < 0 || sel >= static_cast<int>(places.size())) return;

    places.erase(places.begin() + sel);
    SettingsIO::Save(theApp.Settings(), theApp.SettingsPath());
    PopulatePlacesCombo(places.empty() ? -1
                        : std::min(sel, static_cast<int>(places.size()) - 1));
    // Deleting the place the scenario named leaves the combo on a different row
    // (or none); record whatever it now shows so the .ini can't keep pointing at
    // a location this machine no longer has.
    PersistPreviewView();
}

void CPreviewPage::OnMapCapturePlace()
{
    // "Current location" = wherever the map view is currently centered (globe
    // look-at in 3D, canvas center in 2D). Seeds the Add Place dialog so the
    // captured lat/lon can be named and saved, then relocates the scenario origin
    // to it (see ApplyMapPlace below). Capturing MUST move the origin explicitly:
    // PopulatePlacesCombo selects the new row programmatically, which does NOT fire
    // CBN_SELCHANGE, so OnMapPlaceChange never runs — without ApplyMapPlace the
    // origin would silently stay put (and every "Configure Unreal" would keep
    // writing the OLD origin, so DISBrowser spawns at the previous spot no matter
    // which location you just captured).
    double lat = 0.0, lon = 0.0, alt = 0.0;
    CurrentViewLatLonAlt(lat, lon, alt);

    CAddPlaceDialog dlg(lat, lon, alt, this);
    if (dlg.DoModal() != IDOK) return;

    MapPlace p;
    p.label = std::string(CStringA(dlg.m_label).GetString());
    p.lat   = dlg.m_lat;
    p.lon   = dlg.m_lon;
    p.alt   = dlg.m_alt;
    theApp.Settings().mapPlaces.push_back(p);
    SettingsIO::Save(theApp.Settings(), theApp.SettingsPath());
    PopulatePlacesCombo(static_cast<int>(theApp.Settings().mapPlaces.size()) - 1);
    PersistPreviewView();
    // Move the scenario origin to the captured spot so it becomes the active
    // location (what "Configure Unreal" hands DISBrowser). Programmatic combo
    // selection above doesn't fire OnMapPlaceChange, so relocate explicitly.
    ApplyMapPlace(p.lat, p.lon, p.alt > 0.0 ? p.alt : -1.0);
}

//
// OnMapMoveEntitiesToggle — remember whether a Location change should drag the
//   entities along (vs. leaving them world-fixed).
//
void CPreviewPage::OnMapMoveEntitiesToggle()
{
    ToggleCheck(IDC_CHK_MAP_MOVE_ENTITIES, m_moveEntitiesWithMap);
}

//
// PopulatePlacesCombo — refill the Location drop-down from saved settings,
//   optionally selecting row `selectIdx`.
//
void CPreviewPage::PopulatePlacesCombo(int selectIdx)
{
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_COMBO_MAP_PLACE);
    if (!cb) return;
    cb->ResetContent();
    const std::vector<MapPlace>& places = theApp.Settings().mapPlaces;
    for (const MapPlace& p : places)
        cb->AddString(CString(p.label.c_str()));
    if (selectIdx >= 0 && selectIdx < static_cast<int>(places.size()))
        cb->SetCurSel(selectIdx);
}

//
// RefreshMapView — rebuild the render state and repaint the canvas after a map change.
//
void CPreviewPage::RefreshMapView()
{
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// ApplyMapPlace — re-anchor the scenario origin to (lat,lon). Depending on the
//   "move entities" toggle, either drag the scene with the origin or keep every
//   entity world-fixed (re-expressing Local geometry against the new origin).
//   Rebuilds caches, repaints, and flies the globe camera. Marks the model dirty.
//
void CPreviewPage::ApplyMapPlace(double lat, double lon, double flyHeightM)
{
    if (!m_scenario) { RefreshMapView(); return; }

    const double oldLat = m_scenario->originLatDeg;
    const double oldLon = m_scenario->originLonDeg;
    const double oldAlt = m_scenario->originAltM;

    if (m_moveEntitiesWithMap)
    {
        // MOVE: relocate the scenario origin to (lat,lon) but PRESERVE each
        // entity's ENU layout, so the scene stays put on screen while its
        // geographic anchor -- and thus the map backdrop -- moves to the chosen
        // location. Local-mode motion segments follow the origin automatically
        // (the sampler resolves them from local+origin each tick).
        std::vector<std::array<double, 3>> enu(m_scenario->entities.size());
        for (size_t i = 0; i < m_scenario->entities.size(); ++i)
        {
            const Entity& e = m_scenario->entities[i];
            double ee = 0.0, nn = 0.0, uu = 0.0;
            CoordTransforms::EcefToLocalEnuDeg(e.ecefX, e.ecefY, e.ecefZ,
                                               oldLat, oldLon, oldAlt, ee, nn, uu);
            enu[i] = { ee, nn, uu };
        }

        m_scenario->originLatDeg = lat;
        m_scenario->originLonDeg = lon;

        for (size_t i = 0; i < m_scenario->entities.size(); ++i)
            SetEntityInitialEnu(m_scenario->entities[i], enu[i][0], enu[i][1], enu[i][2]);
    }
    else
    {
        // KEEP WORLD: only the map backdrop and globe camera relocate; every
        // entity stays fixed on the Earth. The origin move would otherwise drag
        // Local-mode geometry with it (the sampler resolves it from local+origin),
        // so re-express each Local coordinate against the new origin from its
        // current world ECEF -- leaving world position unchanged. ECEF/LatLon-mode
        // geometry is already world-fixed and needs no adjustment.
        const double newAlt = oldAlt;   // altitude of the origin is not changed

        // Re-express one Local ENU point (metres from origin) so its world
        // position is preserved across the origin move.
        auto freezeLocal = [&](double& lx, double& ly, double& lz)
        {
            double X = 0.0, Y = 0.0, Z = 0.0;
            CoordTransforms::LocalEnuToEcefDeg(lx, ly, lz, oldLat, oldLon, oldAlt, X, Y, Z);
            double e = 0.0, n = 0.0, u = 0.0;
            CoordTransforms::EcefToLocalEnuDeg(X, Y, Z, lat, lon, newAlt, e, n, u);
            lx = e; ly = n; lz = u;
        };

        for (Entity& ent : m_scenario->entities)
            for (MotionSegment& s : ent.motionSegments)
            {
                if (s.coordMode != CoordMode::Local) continue;
                freezeLocal(s.startLocalX, s.startLocalY, s.startLocalZ);
                freezeLocal(s.endLocalX,   s.endLocalY,   s.endLocalZ);
                if (s.type == MotionType::Ellipse)
                {
                    freezeLocal(s.f1LocalX, s.f1LocalY, s.f1LocalZ);
                    freezeLocal(s.f2LocalX, s.f2LocalY, s.f2LocalZ);
                }
            }

        m_scenario->originLatDeg = lat;
        m_scenario->originLonDeg = lon;

        // Entity initial poses are world-fixed by their (unchanged) ECEF; just
        // resync their Local siblings so the Asset tab reflects the new origin.
        for (Entity& ent : m_scenario->entities)
        {
            double e = 0.0, n = 0.0, u = 0.0;
            CoordTransforms::EcefToLocalEnuDeg(ent.ecefX, ent.ecefY, ent.ecefZ,
                                               lat, lon, newAlt, e, n, u);
            ent.localX = e; ent.localY = n; ent.localZ = u;
        }
    }

    // Re-center the 2D view on the new origin, exactly as OnGotoOrigin does.
    // m_centerEnuE/N are metres from the ORIGIN, so leaving them alone across an
    // origin move re-applies the old offset against the new anchor and the view
    // jumps the same distance a second time. Picking a place that the view was
    // already looking at therefore landed at 2*place - origin: choosing
    // Taiwan-Video (22.64N,120.26E) from a Taipei origin (25.03N,121.35E) put the
    // canvas at 20.25N,119.16E, ~290 km out in the South China Sea, where Esri
    // has no imagery above z13 and every tile reads "Map data not yet available".
    // The zoom (metres/pixel) is deliberately kept -- only the center moves.
    m_centerEnuE = 0.0;   // origin is ENU (0,0)
    m_centerEnuN = 0.0;

    RebuildPathsCache();
    RefreshMapView();
    // Re-aim the globe camera: to the location's captured eye altitude when we
    // have one, else framed on the current 2D view (FlyCesiumToOrigin).
    if (flyHeightM >= 0.0 && m_mapLayer == MapLayer::Cesium && m_cesium.GetSafeHwnd())
        m_cesium.FlyTo(lat, lon, flyHeightM);
    else
        FlyCesiumToOrigin();
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);

    // Sanity check: in KEEP-WORLD mode ("Move entities" off) the entities stay fixed
    // on the Earth while the origin jumps to the new place. If that leaves them far
    // from the origin, the flat map/preview projection breaks (entities render on the
    // far side of the globe). Warn so it isn't a surprise — enabling "Move entities"
    // avoids it. (In MOVE mode the entities follow the origin, so this never fires.)
    // Only when the origin is committed (a saved/loaded scenario): while the origin
    // is provisional the operator is just navigating the default view, so relocating
    // must stay silent.
    if (m_scenario->originSet && !m_moveEntitiesWithMap)
    {
        constexpr double kDisconnectM = 500000.0;   // 500 km — see kOriginDisconnectMeters
        double best = HUGE_VAL;
        for (const Entity& ent : m_scenario->entities)
            if (ent.enabled) best = std::min(best, std::hypot(ent.localX, ent.localY));
        if (best > kDisconnectM && best < HUGE_VAL)
        {
            CString msg;
            msg.Format(_T("The origin moved, but the scenario's entities stayed fixed on the ")
                       _T("Earth and are now about %.0f km away — the map/preview will look wrong.")
                       _T("\n\nEnable \"Move entities\" before relocating to bring them along, or ")
                       _T("pick a Location near the entities."), best / 1000.0);
            AfxMessageBox(msg, MB_OK | MB_ICONWARNING);
        }
    }
}

//
// EffectivePreviewDuration — playback span in seconds: the larger of the authored
//   scenario duration and the latest enabled segment end (falls back to 300s).
//
double CPreviewPage::EffectivePreviewDuration() const
{
    double dur = (m_scenario && m_scenario->durationSeconds > 0.0)
               ? m_scenario->durationSeconds : 0.0;
    if (m_scenario)
        for (const Entity& e : m_scenario->entities)
        {
            if (!e.enabled) continue;
            for (const MotionSegment& s : e.motionSegments)
                if (s.enabled && s.endSecond > dur) dur = s.endSecond;
        }
    if (dur <= 0.0) dur = 300.0;
    return dur;
}

//
// UpdateSliderRange — size the time slider to the effective duration (0.1 s ticks).
//
void CPreviewPage::UpdateSliderRange()
{
    if (CSliderCtrl* s = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME))
        s->SetRange(0, static_cast<int>(EffectivePreviewDuration() * 10.0), TRUE);
    // The camera schedule's right edge tracks the duration; keep it clamped and
    // repaint the strip. Duration drives the strip's pixel width, so resync the
    // horizontal scroll bar too.
    NormalizeCameraSchedule();
    UpdateCameraScrollBar();
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
}

//
// StartTimer — begin the ~30 Hz animation timer (idempotent).
//
void CPreviewPage::StartTimer()
{
    if (m_timerActive) return;
    SetTimer(kAnimTimerId, 33, nullptr);    // ~30 Hz
    m_timerActive = true;
}

//
// StopTimer — stop the animation timer (idempotent).
//
void CPreviewPage::StopTimer()
{
    if (!m_timerActive) return;
    KillTimer(kAnimTimerId);
    m_timerActive = false;
}

//
// OnStart — begin playback: from Idle reset to t=0 and clear trails, then run
//   the animation timer.
//
void CPreviewPage::OnStart()
{
    if (!m_scenario || m_scenario->durationSeconds <= 0.0) return;
    if (m_runState == PreviewState::Playing) return;
    if (m_runState == PreviewState::Idle)
    {
        m_previewTimeSec = 0.0;
        m_trails.assign(m_scenario->entities.size(), {});
    }
    m_runState   = PreviewState::Playing;
    m_lastTickMs = GetTickCount64();
    StartTimer();
}

//
// OnPause — pause playback while keeping the current time.
//
void CPreviewPage::OnPause()
{
    if (m_runState != PreviewState::Playing) return;
    m_runState = PreviewState::Paused;
    StopTimer();
}

//
// OnResume — resume from Paused, re-basing the tick clock so time doesn't jump.
//
void CPreviewPage::OnResume()
{
    if (m_runState != PreviewState::Paused) return;
    m_runState   = PreviewState::Playing;
    m_lastTickMs = GetTickCount64();
    StartTimer();
}

//
// OnStop — stop playback and reset to t=0, clearing trails and repainting.
//
void CPreviewPage::OnStop()
{
    StopTimer();
    m_runState       = PreviewState::Idle;
    m_previewTimeSec = 0.0;
    m_trails.clear();
    UpdateSliderFromTime();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnSpeedChange()
{
    if (CComboBox* spd = (CComboBox*)GetDlgItem(IDC_COMBO_PREVIEW_SPEED))
    {
        const int sel = spd->GetCurSel();
        if (sel >= 0 && sel < static_cast<int>(_countof(kPlaySpeeds)))
            m_playSpeed = kPlaySpeeds[sel].mul;
    }
    PersistPreviewView();

    // The camera boxes label themselves with a speed-adjusted duration, so the
    // strip has to repaint even though nothing about the schedule changed.
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
}

//
// OnTimer — timer dispatch: the server-poll timer refreshes terrain UI; the
//   animation timer advances preview time by dt*speed, clamps/stops at the end,
//   and repaints.
//
void CPreviewPage::OnTimer(UINT_PTR id)
{
    if (id == kServerPollTimerId) { RefreshTerrainServerUi(); return; }
    if (id != kAnimTimerId) { CHelpAwarePage::OnTimer(id); return; }
    if (!m_scenario || m_runState != PreviewState::Playing) return;

    const ULONGLONG now = GetTickCount64();
    const double dt = (now - m_lastTickMs) / 1000.0;
    m_lastTickMs = now;

    m_previewTimeSec += dt * m_playSpeed;
    const double dur = EffectivePreviewDuration();
    if (dur > 0.0 && m_previewTimeSec >= dur)
    {
        // End of scenario — clamp to duration, stop animating, return to
        // Idle so the next Start restarts from t=0.
        m_previewTimeSec = dur;
        StopTimer();
        m_runState = PreviewState::Idle;
    }

    UpdateSliderFromTime();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);  // move the playhead
}

//
// OnHScroll — time-slider scrub: set preview time from the slider position,
//   discard trail history, and repaint.
//
void CPreviewPage::OnHScroll(UINT code, UINT pos, CScrollBar* sb)
{
    if (m_suppressScroll) { CHelpAwarePage::OnHScroll(code, pos, sb); return; }

    // Camera-strip horizontal scroll bar: pan the (zoomed) timeline view.
    CScrollBar* hbar = (CScrollBar*)GetDlgItem(IDC_CAMERA_HSCROLL);
    if (sb && hbar && sb->GetSafeHwnd() == hbar->GetSafeHwnd())
    {
        SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_ALL;
        hbar->GetScrollInfo(&si);
        int p = si.nPos;
        const int line = 20;
        switch (code)
        {
        case SB_LINELEFT:    p -= line;            break;
        case SB_LINERIGHT:   p += line;            break;
        case SB_PAGELEFT:    p -= (int)si.nPage;   break;
        case SB_PAGERIGHT:   p += (int)si.nPage;   break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: p = si.nTrackPos;   break;
        case SB_LEFT:        p = si.nMin;          break;
        case SB_RIGHT:       p = si.nMax;          break;
        default: break;
        }
        const int maxPos = std::max(0, (int)si.nMax - (int)si.nPage + 1);
        if (p < 0) p = 0;
        if (p > maxPos) p = maxPos;
        hbar->SetScrollPos(p, TRUE);
        m_timeline.SetScrollOffset(p);
        if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
        return;
    }

    CSliderCtrl* slider = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME);
    if (sb && slider && sb->GetSafeHwnd() == slider->GetSafeHwnd())
    {
        const int p = slider->GetPos();
        m_previewTimeSec = p / 10.0;
        m_trails.clear();   // scrubbing invalidates trail history
        RebuildRenderState();
        if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
        if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);  // move the playhead
        return;
    }
    CHelpAwarePage::OnHScroll(code, pos, sb);
}

//
// OnZoomIn — zoom the canvas in one step (about the view center).
//
void CPreviewPage::OnZoomIn()
{
    m_zoomMetersPerPx /= 1.25;
    if (m_zoomMetersPerPx < 0.001) m_zoomMetersPerPx = 0.001;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnZoomOut — zoom the canvas out one step (about the view center).
//
void CPreviewPage::OnZoomOut()
{
    m_zoomMetersPerPx *= 1.25;
    if (m_zoomMetersPerPx > 1e9) m_zoomMetersPerPx = 1e9;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnFit — re-fit the whole scenario to the canvas and repaint.
//
void CPreviewPage::OnFit()
{
    FitScenario();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::PanByPixels(int dxPx, int dyPx)
{
    // Screen +x = East (right); screen +y = South (down), so subtract dy
    // to convert into ENU North (which is screen-up).
    m_centerEnuE -= dxPx * m_zoomMetersPerPx;
    m_centerEnuN += dyPx * m_zoomMetersPerPx;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::ZoomAtPixel(double factor, int cursorXPx, int cursorYPx)
{
    if (factor <= 0.0) return;
    if (!m_canvas.GetSafeHwnd())
    {
        m_zoomMetersPerPx *= factor;
        if (m_zoomMetersPerPx < 0.001) m_zoomMetersPerPx = 0.001;
        if (m_zoomMetersPerPx > 1e9)   m_zoomMetersPerPx = 1e9;
        RebuildRenderState();
        return;
    }

    // World point currently under the cursor (canvas coords -> ENU).
    CRect rc; m_canvas.GetClientRect(&rc);
    const double w = std::max<LONG>(rc.Width(),  1);
    const double h = std::max<LONG>(rc.Height(), 1);
    const double oldZoom = m_zoomMetersPerPx;
    const double worldE = m_centerEnuE + (cursorXPx - w * 0.5) * oldZoom;
    const double worldN = m_centerEnuN - (cursorYPx - h * 0.5) * oldZoom;

    // New zoom, clamped.
    double newZoom = oldZoom * factor;
    if (newZoom < 0.001) newZoom = 0.001;
    if (newZoom > 1e9)   newZoom = 1e9;
    m_zoomMetersPerPx = newZoom;

    // Shift center so the same world point ends up back under the cursor.
    m_centerEnuE = worldE - (cursorXPx - w * 0.5) * newZoom;
    m_centerEnuN = worldN + (cursorYPx - h * 0.5) * newZoom;

    RebuildRenderState();
    m_canvas.Invalidate(FALSE);
}

//
// OnLabelsToggle — toggle entity labels and repaint.
//
void CPreviewPage::OnLabelsToggle()
{
    ToggleCheck(IDC_CHK_PREVIEW_LABELS, m_showLabels);
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnTrailsToggle — toggle motion trails (clearing history when off) and repaint.
//
void CPreviewPage::OnTrailsToggle()
{
    ToggleCheck(IDC_CHK_PREVIEW_TRAILS, m_showTrails);
    if (!m_showTrails) m_trails.clear();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnPathsToggle — toggle drawing of configured motion paths and repaint.
//
void CPreviewPage::OnPathsToggle()
{
    ToggleCheck(IDC_CHK_PREVIEW_PATHS, m_showPaths);
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnOrientationToggle — toggle per-entity heading vectors and repaint.
//
void CPreviewPage::OnOrientationToggle()
{
    ToggleCheck(IDC_CHK_PREVIEW_ORIENTATION, m_showOrientation);
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnTerrainToggle — toggle the level land/ocean footprint overlay and repaint.
//
void CPreviewPage::OnTerrainToggle()
{
    ToggleCheck(IDC_CHK_PREVIEW_TERRAIN, m_showTerrain);
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// ToggleCheck — flip one owner-draw check box. A BS_OWNERDRAW button reports the
//   click but neither toggles nor stores a check state of its own (BM_SETCHECK is
//   documented as having no effect on one), so the member is the only state: flip
//   it, repaint the box, and write the new value into the scenario's [Preview]
//   section.
//
void CPreviewPage::ToggleCheck(int ctrlId, bool& flag)
{
    flag = !flag;
    RepaintCheck(ctrlId);
    PersistPreviewView();
}

//
// RepaintCheck — redraw one owner-draw check box. There is no state to push onto
//   the control: DrawCheckBox reads the m_show* member through CheckBoxState, so
//   setting the member and calling this is the whole update.
//
void CPreviewPage::RepaintCheck(int ctrlId)
{
    if (CWnd* b = GetDlgItem(ctrlId)) b->Invalidate();
}

//
// CheckBoxState — the member behind one owner-draw check box. This is the single
//   place the control ids and the m_show* flags are tied together; the painter
//   goes through it rather than asking the control, which would always answer
//   "unchecked".
//
bool CPreviewPage::CheckBoxState(int ctrlId) const
{
    switch (ctrlId)
    {
        case IDC_CHK_PREVIEW_LABELS:      return m_showLabels;
        case IDC_CHK_PREVIEW_TRAILS:      return m_showTrails;
        case IDC_CHK_PREVIEW_PATHS:       return m_showPaths;
        case IDC_CHK_PREVIEW_ORIENTATION: return m_showOrientation;
        case IDC_CHK_PREVIEW_TERRAIN:     return m_showTerrain;
        case IDC_CHK_PREVIEW_ZONES:       return m_showZones;
        case IDC_CHK_PREVIEW_LEGEND:      return m_showLegend;
        case IDC_CHK_PREVIEW_DESTTIME:    return m_showDestTime;
        case IDC_CHK_MAP_MOVE_ENTITIES:   return m_moveEntitiesWithMap;
        default:                          return false;
    }
}

// PreviewView::mapLayer is a plain int so Scenario.h stays free of the MFC
// headers that declare MapLayer; the two must keep the same numbering.
static_assert(static_cast<int>(MapLayer::None)        == 0, "PreviewView::mapLayer mapping");
static_assert(static_cast<int>(MapLayer::Satellite)   == 1, "PreviewView::mapLayer mapping");
static_assert(static_cast<int>(MapLayer::Topographic) == 2, "PreviewView::mapLayer mapping");
static_assert(static_cast<int>(MapLayer::Cesium)      == 3, "PreviewView::mapLayer mapping");

//
// PersistPreviewView — snapshot the whole Preview view state (the Map /
//   Location / Speed / Scale drop-downs plus every "Show" check box) into the
//   scenario's [Preview] section and flag the document modified, so it is saved
//   with the scenario rather than lost on close. This lives in scenario.ini, not
//   settings.ini, because it describes how a PARTICULAR scenario is best viewed:
//   a coastal play wants satellite imagery and its own location, a long transit
//   wants 300x, a scenario with a painted level wants Terrain and Zones drawn,
//   and a 200-entity scenario is unreadable with Labels and Trails on.
//
void CPreviewPage::PersistPreviewView()
{
    if (!m_scenario) return;
    PreviewView& pv = m_scenario->preview;

    pv.mapLayer        = static_cast<int>(m_mapLayer);
    pv.playSpeed       = m_playSpeed;
    pv.camScaleSeconds = m_timeline.GetTimeScale();

    pv.showLabels          = m_showLabels;
    pv.showTrails          = m_showTrails;
    pv.showPaths           = m_showPaths;
    pv.showOrientation     = m_showOrientation;
    pv.showTerrain         = m_showTerrain;
    pv.showZones           = m_showZones;
    pv.showLegend          = m_showLegend;
    pv.showProperties      = m_showDestTime;
    pv.moveEntitiesWithMap = m_moveEntitiesWithMap;

    // The Location is stored by LABEL, not by row index: the saved-places list is
    // per-user (settings.ini), so an index would point at a different place — or
    // off the end — on another machine or after an Add/Delete.
    pv.mapPlace.clear();
    if (CComboBox* pc = (CComboBox*)GetDlgItem(IDC_COMBO_MAP_PLACE))
    {
        const int sel = pc->GetCurSel();
        if (sel >= 0)
        {
            CString label;
            pc->GetLBText(sel, label);
            pv.mapPlace = std::string(CStringA(label).GetString());
        }
    }

    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_MARK_DIRTY, 0, 0);
}

//
// ApplyPreviewViewFromScenario — the reverse of PersistPreviewView: seed the four
//   drop-downs (and the state behind them) from the scenario's [Preview] section.
//   Deliberately does NOT mark the document dirty, and does NOT re-apply the saved
//   Location's coordinates — [Origin] is the authority on where the scenario sits,
//   so the label only re-selects the row. Speed/Scale snap to the nearest offered
//   choice, which keeps a hand-edited .ini value usable instead of ignored.
//
void CPreviewPage::ApplyPreviewViewFromScenario()
{
    if (!m_scenario) return;
    const PreviewView& pv = m_scenario->preview;

    // --- "Show" check boxes ---
    m_showLabels          = pv.showLabels;
    m_showTrails          = pv.showTrails;
    m_showPaths           = pv.showPaths;
    m_showOrientation     = pv.showOrientation;
    m_showTerrain         = pv.showTerrain;
    m_showZones           = pv.showZones;
    m_showLegend          = pv.showLegend;
    m_showDestTime        = pv.showProperties;
    m_moveEntitiesWithMap = pv.moveEntitiesWithMap;

    RepaintCheck(IDC_CHK_PREVIEW_LABELS);
    RepaintCheck(IDC_CHK_PREVIEW_TRAILS);
    RepaintCheck(IDC_CHK_PREVIEW_PATHS);
    RepaintCheck(IDC_CHK_PREVIEW_ORIENTATION);
    RepaintCheck(IDC_CHK_PREVIEW_TERRAIN);
    RepaintCheck(IDC_CHK_PREVIEW_ZONES);
    RepaintCheck(IDC_CHK_PREVIEW_LEGEND);
    RepaintCheck(IDC_CHK_PREVIEW_DESTTIME);
    RepaintCheck(IDC_CHK_MAP_MOVE_ENTITIES);

    // Trails are history, not geometry: a scenario saved with them off must not
    // come up showing the previous scenario's tails.
    if (!m_showTrails) m_trails.clear();

    // The Properties grid is a real control, so its visibility follows its box.
    if (CWnd* lc = GetDlgItem(IDC_LIST_PREVIEW_DESTTIME))
        lc->ShowWindow(m_showDestTime ? SW_SHOW : SW_HIDE);
    if (m_showDestTime) RefreshDestTimeList();

    // --- Map backdrop layer ---
    int layer = pv.mapLayer;
    if (layer < 0 || layer > static_cast<int>(MapLayer::Cesium)) layer = 0;
    m_mapLayer = static_cast<MapLayer>(layer);
    if (CComboBox* mc = (CComboBox*)GetDlgItem(IDC_COMBO_MAP_LAYER))
        mc->SetCurSel(layer);
    ActivateCesium(m_mapLayer == MapLayer::Cesium);

    // --- Location: select the matching saved place, if this machine has it ---
    if (CComboBox* pc = (CComboBox*)GetDlgItem(IDC_COMBO_MAP_PLACE))
    {
        int sel = -1;
        const std::vector<MapPlace>& places = theApp.Settings().mapPlaces;
        for (size_t i = 0; i < places.size(); ++i)
        {
            if (places[i].label == pv.mapPlace) { sel = static_cast<int>(i); break; }
        }
        pc->SetCurSel(sel);   // -1 clears it: unknown label => no row highlighted
    }

    // --- Preview speed: nearest offered multiplier ---
    {
        int best = kPlaySpeedDefault;
        double bestErr = -1.0;
        for (int i = 0; i < static_cast<int>(_countof(kPlaySpeeds)); ++i)
        {
            const double err = std::fabs(kPlaySpeeds[i].mul - pv.playSpeed);
            if (bestErr < 0.0 || err < bestErr) { bestErr = err; best = i; }
        }
        m_playSpeed = kPlaySpeeds[best].mul;
        if (CComboBox* spd = (CComboBox*)GetDlgItem(IDC_COMBO_PREVIEW_SPEED))
            spd->SetCurSel(best);
    }

    // --- Camera-strip time scale: nearest offered scale ---
    {
        int best = kCamScaleDefault;
        double bestErr = -1.0;
        for (int i = 0; i < static_cast<int>(_countof(kCamScales)); ++i)
        {
            const double err = std::fabs(kCamScales[i].seconds - pv.camScaleSeconds);
            if (bestErr < 0.0 || err < bestErr) { bestErr = err; best = i; }
        }
        if (CComboBox* sc = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_SCALE))
            sc->SetCurSel(best);
        m_timeline.SetTimeScale(kCamScales[best].seconds);
        m_timeline.SetScrollOffset(0);
        UpdateCameraScrollBar();
    }
}

//
// OnScenarioLoaded — a different scenario replaced the model (File > Open / New,
//   or a Play). Re-seed the view drop-downs from its [Preview] section and
//   repaint. No-op before the page has a window: OnInitDialog applies the same
//   settings itself once it does.
//
void CPreviewPage::OnScenarioLoaded()
{
    if (!GetSafeHwnd()) return;

    // Same order as OnInitDialog: fit the new scenario first, so restoring the
    // Cesium layer opens the globe framed on this scenario rather than on the
    // previous one's zoom. (OnShowWindow re-fits when the tab is next shown.)
    RebuildPathsCache();
    FitScenario();
    ApplyPreviewViewFromScenario();
    RefreshMapView();
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
}

//
// OnScenarioEdited — the same model, edited in place. Everything derived from
//   it is rebuilt (paths, camera combos, slider range, render state) but the
//   view is left exactly where the operator has it. Plot > Line used to come
//   through OnScenarioLoaded, whose FitScenario re-framed the whole scenario on
//   every edit: the 2 km starter course vanished at that zoom and the operator
//   lost the entity they were working on.
//
void CPreviewPage::OnScenarioEdited()
{
    if (!GetSafeHwnd()) return;
    RebuildPathsCache();
    PopulateCameraCombos();
    UpdateSliderRange();
    UpdateCameraScrollBar();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd())   m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
}

//
// OnZonesToggle — toggle the named level zones (e.g. Palm Forest) independently
//   of the land/ocean footprint, so the big terrain box can be hidden while a
//   zone stays visible. Repaints.
//
void CPreviewPage::OnZonesToggle()
{
    ToggleCheck(IDC_CHK_PREVIEW_ZONES, m_showZones);
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnLegendToggle — toggle the size/direction legend overlay and repaint.
//
void CPreviewPage::OnLegendToggle()
{
    ToggleCheck(IDC_CHK_PREVIEW_LEGEND, m_showLegend);
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::OnShowOriginToggle()
{
    // Owner-draw toggle (not a checkbox): flip our own bool, repaint the button's
    // bold green check, and when turning ON re-center the view on the scenario
    // origin (ENU 0,0) so the red X marker drawn by the canvas is in view.
    m_showOrigin = !m_showOrigin;
    if (CWnd* btn = GetDlgItem(IDC_BTN_PREVIEW_SHOW_ORIGIN))
        btn->Invalidate();   // repaint owner-draw to show/hide the green check
    if (m_showOrigin)
    {
        m_centerEnuE = 0.0;   // origin is ENU (0,0); recenter the view on it
        m_centerEnuN = 0.0;
    }
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnGotoOrigin — move the camera over the scenario origin without changing how
//   high it looks from: the 2D view keeps its zoom (metres/pixel) and the 3D
//   globe keeps its current eye altitude; only the center moves. Unlike
//   "Show Origin" this is a pure navigation action — it doesn't toggle the
//   red-X marker.
//
void CPreviewPage::OnGotoOrigin()
{
    m_centerEnuE = 0.0;   // origin is ENU (0,0)
    m_centerEnuN = 0.0;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);

    if (m_mapLayer == MapLayer::Cesium && m_cesium.GetSafeHwnd() && m_scenario)
    {
        // Keep the globe's reported eye altitude; if it hasn't reported a
        // position yet, fall back to the 2D view's equivalent eye height.
        double lat = 0.0, lon = 0.0, alt = 0.0;
        if (!m_cesium.GetLookAt(lat, lon, alt)) alt = CurrentViewAltitude();
        m_cesium.FlyTo(m_scenario->originLatDeg, m_scenario->originLonDeg, alt);
    }
}

void CPreviewPage::OnSetStart()
{
    // Toggle pick mode. While armed, the green "?" shows and the next click on
    // an ellipse records that entity's start; clicking the button again cancels.
    m_startPickArmed = !m_startPickArmed;
    if (CWnd* btn = GetDlgItem(IDC_BTN_PREVIEW_SET_START))
        btn->Invalidate();   // repaint owner-draw to show/hide the armed "?"
    if (m_startPickArmed && m_canvas.GetSafeHwnd())
        ::SetCursor(::LoadCursor(nullptr, IDC_CROSS));
}

void CPreviewPage::OnBoundary()
{
    // Toggle boundary-paint mode. While armed the button shows a bold green check and the
    // canvas turns a left-drag into a yellow terrain-boundary rectangle (see PreviewCanvas).
    m_boundaryArmed = !m_boundaryArmed;
    if (m_boundaryArmed)
    {
        m_startPickArmed   = false;   // the paint modes are mutually exclusive
        m_foliageAreaArmed = false;
        if (CWnd* f = GetDlgItem(IDC_BTN_PREVIEW_FOLIAGE_AREA)) f->Invalidate();
    }
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_BOUNDARY))  b->Invalidate();
    if (CWnd* s = GetDlgItem(IDC_BTN_PREVIEW_SET_START)) s->Invalidate();
    if (m_canvas.GetSafeHwnd())
    {
        m_canvas.Invalidate(FALSE);
        if (m_boundaryArmed) ::SetCursor(::LoadCursor(nullptr, IDC_CROSS));
    }
    // Same gesture works on the 3D globe: arm/disarm right-drag rectangle paint there too.
    if (m_mapLayer == MapLayer::Cesium && m_cesium.GetSafeHwnd())
        m_cesium.SetBoundaryMode(m_boundaryArmed);
}

//
// OnFoliage — "Foliage" button: open the modal tree-type / rendering-backend
//   picker seeded from the scenario's current selection. On Save, store the edited
//   selection back on the scenario and mark it dirty so it persists to
//   scenario.ini ([Foliage]) on the next save. This only records the choice — no
//   foliage is generated or rendered here.
//
void CPreviewPage::OnFoliage()
{
    if (!m_scenario) return;

    CFoliageDialog dlg(this);
    dlg.SetConfig(m_scenario->foliage);
    if (dlg.DoModal() != IDOK) return;

    m_scenario->foliage = dlg.Config();
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_MARK_DIRTY, 0, 0);
}

//
// OnFoliageArea — toggle "Foliage Area" paint mode.
//   Mutually exclusive with the Boundary and Set-Start tools: they all claim the
//   same drag gesture, and two armed at once would be ambiguous.
//
void CPreviewPage::OnFoliageArea()
{
    m_foliageAreaArmed = !m_foliageAreaArmed;
    if (m_foliageAreaArmed)
    {
        m_boundaryArmed  = false;
        m_startPickArmed = false;
    }
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_FOLIAGE_AREA)) b->Invalidate();
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_BOUNDARY))     b->Invalidate();
    if (CWnd* s = GetDlgItem(IDC_BTN_PREVIEW_SET_START))    s->Invalidate();
    if (m_canvas.GetSafeHwnd())
    {
        m_canvas.Invalidate(FALSE);
        if (m_foliageAreaArmed) ::SetCursor(::LoadCursor(nullptr, IDC_CROSS));
    }
    if (m_mapLayer == MapLayer::Cesium && m_cesium.GetSafeHwnd())
        m_cesium.SetBoundaryMode(m_foliageAreaArmed);

    if (m_foliageAreaArmed && m_scenario)
    {
        CString msg;
        msg.Format(_T("Foliage Area armed: right-drag a rectangle (%d painted)"),
                   static_cast<int>(m_scenario->foliageAreas.size()));
        SetJobStatus(msg);
    }
}

//
// OnFoliageAreaPainted — a drag finished: APPEND a foliage rectangle and ask how
//   many trees it carries. Unlike the terrain boundary this does not replace the
//   previous box, and it stays armed so several areas can be painted in a row.
//
void CPreviewPage::OnFoliageAreaPainted(double latMinDeg, double latMaxDeg,
                                        double lonMinDeg, double lonMaxDeg)
{
    if (!m_scenario) return;

    if (latMinDeg > latMaxDeg) std::swap(latMinDeg, latMaxDeg);
    if (lonMinDeg > lonMaxDeg) std::swap(lonMinDeg, lonMaxDeg);

    // Seed the prompt from the previous area so painting a series of similar
    // patches does not mean retyping the number every time.
    long long seed = 20000;
    if (!m_scenario->foliageAreas.empty())
        seed = m_scenario->foliageAreas.back().treeCount;

    CString initial;
    initial.Format(_T("%lld"), seed);
    CPromptDialog dlg(_T("Foliage Area"), _T("Trees in this area:"), initial, this);
    if (dlg.DoModal() != IDOK) return;      // cancel = do not add the area

    const long long count = _ttoi64(dlg.Value());
    if (count <= 0)
    {
        MessageBox(_T("A foliage area needs a positive tree count."),
                   _T("Foliage Area"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    FoliageArea a;
    a.latMinDeg = latMinDeg;
    a.latMaxDeg = latMaxDeg;
    a.lonMinDeg = lonMinDeg;
    a.lonMaxDeg = lonMaxDeg;
    a.treeCount = count;
    m_scenario->foliageAreas.push_back(a);

    {
        char buf[240];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage::OnFoliageAreaPainted: area %u lat[%.5f..%.5f] lon[%.5f..%.5f] trees=%lld",
                  static_cast<unsigned>(m_scenario->foliageAreas.size()),
                  latMinDeg, latMaxDeg, lonMinDeg, lonMaxDeg, count);
        LOG(buf);
    }

    CString msg;
    msg.Format(_T("Foliage area %d added (%lld trees). Right-drag for another, or click Foliage Area to stop."),
               static_cast<int>(m_scenario->foliageAreas.size()), count);
    SetJobStatus(msg);

    // Stay armed: painting several areas in a row is the common case.
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_MARK_DIRTY, 0, 0);
}

//
// SetFoliageAreaCount — edit one painted area's tree count in place.
//
void CPreviewPage::SetFoliageAreaCount(size_t idx)
{
    if (!m_scenario || idx >= m_scenario->foliageAreas.size()) return;
    FoliageArea& a = m_scenario->foliageAreas[idx];

    CString initial;
    initial.Format(_T("%lld"), a.treeCount);
    CPromptDialog dlg(_T("Foliage Area"), _T("Trees in this area:"), initial, this);
    if (dlg.DoModal() != IDOK) return;

    const long long count = _ttoi64(dlg.Value());
    if (count <= 0)
    {
        MessageBox(_T("A foliage area needs a positive tree count.\n\n")
                   _T("To remove the area entirely, right-click it and choose ")
                   _T("\"Delete Foliage Area\"."),
                   _T("Foliage Area"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    a.treeCount = count;

    CString msg;
    msg.Format(_T("Foliage area %d set to %lld trees."),
               static_cast<int>(idx + 1), count);
    SetJobStatus(msg);
    {
        char buf[160];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage::SetFoliageAreaCount: area %u -> %lld trees",
                  static_cast<unsigned>(idx), count);
        LOG(buf);
    }

    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_FOLIAGE_AREA)) b->Invalidate();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_MARK_DIRTY, 0, 0);
}

//
// DeleteFoliageArea — remove one painted area.
//   Erasing from the middle SHIFTS the remaining areas up, and paint order is
//   what decides who owns overlapped ground. That is the intended behaviour --
//   deleting an area hands its overlaps to whoever is now first -- but it does
//   change where trees land, so the status line says the order changed.
//
void CPreviewPage::DeleteFoliageArea(size_t idx)
{
    if (!m_scenario || idx >= m_scenario->foliageAreas.size()) return;

    const long long trees = m_scenario->foliageAreas[idx].treeCount;
    const bool wasNotLast = (idx + 1 < m_scenario->foliageAreas.size());
    m_scenario->foliageAreas.erase(m_scenario->foliageAreas.begin() +
                                   static_cast<std::ptrdiff_t>(idx));

    CString msg;
    if (wasNotLast && !m_scenario->foliageAreas.empty())
        msg.Format(_T("Deleted foliage area %d (%lld trees); %d left, and later areas moved up in paint order."),
                   static_cast<int>(idx + 1), trees,
                   static_cast<int>(m_scenario->foliageAreas.size()));
    else
        msg.Format(_T("Deleted foliage area %d (%lld trees); %d left."),
                   static_cast<int>(idx + 1), trees,
                   static_cast<int>(m_scenario->foliageAreas.size()));
    SetJobStatus(msg);
    {
        char buf[180];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage::DeleteFoliageArea: removed area %u (%lld trees), %u remain",
                  static_cast<unsigned>(idx), trees,
                  static_cast<unsigned>(m_scenario->foliageAreas.size()));
        LOG(buf);
    }

    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_FOLIAGE_AREA)) b->Invalidate();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_MARK_DIRTY, 0, 0);
}

//
// DeleteAllFoliageAreas — clear the list after a confirmation.
//   Confirmed because there is no undo, and repainting a set of areas by hand is
//   real work.
//
void CPreviewPage::DeleteAllFoliageAreas()
{
    if (!m_scenario || m_scenario->foliageAreas.empty()) return;

    long long total = 0;
    for (const FoliageArea& a : m_scenario->foliageAreas) total += a.treeCount;

    CString msg;
    msg.Format(_T("Delete all %d painted foliage areas (%lld trees)?\n\n")
               _T("This cannot be undone. A bake with no areas painted falls back ")
               _T("to the whole terrain box."),
               static_cast<int>(m_scenario->foliageAreas.size()), total);
    if (MessageBox(msg, _T("Foliage Area"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    const size_t n = m_scenario->foliageAreas.size();
    m_scenario->foliageAreas.clear();

    CString done;
    done.Format(_T("Deleted all %d foliage areas."), static_cast<int>(n));
    SetJobStatus(done);
    {
        char buf[120];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage::DeleteAllFoliageAreas: cleared %u area(s)",
                  static_cast<unsigned>(n));
        LOG(buf);
    }

    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_FOLIAGE_AREA)) b->Invalidate();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_MARK_DIRTY, 0, 0);
}

void CPreviewPage::OnBoundaryPainted(double latMinDeg, double latMaxDeg,
                                     double lonMinDeg, double lonMaxDeg)
{
    if (!m_scenario) return;

    // Normalize so min < max regardless of drag direction.
    if (latMinDeg > latMaxDeg) std::swap(latMinDeg, latMaxDeg);
    if (lonMinDeg > lonMaxDeg) std::swap(lonMinDeg, lonMaxDeg);

    m_scenario->terrainBoundsValid = true;
    m_scenario->terrainLatMinDeg   = latMinDeg;
    m_scenario->terrainLatMaxDeg   = latMaxDeg;
    m_scenario->terrainLonMinDeg   = lonMinDeg;
    m_scenario->terrainLonMaxDeg   = lonMaxDeg;

    {
        char buf[220];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage::OnBoundaryPainted: terrain box lat[%.5f..%.5f] lon[%.5f..%.5f]",
                  latMinDeg, latMaxDeg, lonMinDeg, lonMaxDeg);
        LOG(buf);
    }

    // Recording complete: leave boundary mode (the transient yellow rectangle disappears) and
    // flag the scenario dirty so the box persists into the .ini on Save.
    m_boundaryArmed = false;
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_BOUNDARY)) b->Invalidate();
    if (m_mapLayer == MapLayer::Cesium && m_cesium.GetSafeHwnd())
        m_cesium.SetBoundaryMode(false);   // restore the globe's right-drag zoom
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// OnBuildTerrain — kick off the DEM download + WSL/Docker tiling pipeline for the
//   painted terrain box. Validates the box + locates retile_terrain.cmd, confirms
//   with the operator, then ShellExecutes the script.
//
//
// TerrainTilesDir — the NTFS folder TerrainServer.exe serves from.
//   Tiles are built on ext4 inside WSL (fast, and Docker bind-mounts stay on
//   ext4) and published here afterwards, because a Windows process cannot read
//   /root/terrain/tiles: the \\wsl$ share needs P9RdrService, which is stopped
//   and requires admin to start.
//
//
// TerrainHost / TerrainPort — where the tile server we care about lives.
//   Thin wrappers over ResolveTerrainEndpoint(), which is the ONE place the
//   four modes are turned into an address. COutputPlaybackPage used to make
//   the same decision inline with a different default ("localhost" vs
//   "127.0.0.1"); both now call the shared resolver so they cannot drift.
//
CStringA CPreviewPage::TerrainHost() const
{
    return CStringA(ResolveTerrainEndpoint(theApp.Settings()).host.c_str());
}

unsigned short CPreviewPage::TerrainPort() const
{
    return ResolveTerrainEndpoint(theApp.Settings()).port;
}

//
// TerrainModeName — for status text and log lines.
//
CString CPreviewPage::TerrainModeName() const
{
    switch (theApp.Settings().terrainMode)
    {
        case TerrainServerMode::Legacy:  return _T("Legacy (serve.py in WSL)");
        case TerrainServerMode::Remote:  return _T("Remote");
        case TerrainServerMode::Service: return _T("Service (containerized)");
        default:                         return _T("Local");
    }
}

CString CPreviewPage::TerrainTilesDir() const
{
    TCHAR local[MAX_PATH] = { 0 };
    if (SUCCEEDED(::SHGetFolderPath(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local)))
        return CString(local) + _T("\\DISBrowser\\terrain\\tiles");
    return _T("C:\\DISBrowser\\terrain\\tiles");
}

//
// WslPathOf — "C:\Users\me\x" -> "/mnt/c/Users/me/x", so a Windows path can be
//   handed to a script running inside WSL.
//
CString CPreviewPage::WslPathOf(const CString& winPath) const
{
    CString p = winPath;
    if (p.GetLength() < 2 || p[1] != _T(':')) return p;   // not drive-qualified

    CString drive = p.Left(1);
    drive.MakeLower();
    CString rest = p.Mid(2);
    rest.Replace(_T('\\'), _T('/'));
    return _T("/mnt/") + drive + rest;
}

//
// SetJobStatus — one-line status under the preview, plus the app log. This is
//   the "something is happening" channel; failures additionally raise a modal.
//
void CPreviewPage::SetJobStatus(const CString& text)
{
    if (::IsWindow(GetSafeHwnd()))
        SetDlgItemText(IDC_STATIC_TERRAIN_STATUS, text);
}

//
// ServiceMode — is the terrain server the containerized job service?
//
bool CPreviewPage::ServiceMode() const
{
    return theApp.Settings().terrainMode == TerrainServerMode::Service;
}

//
// FailJob — the ONE failure path, shared by the supervisor and the HTTP job
//   client. Same title, same tail, same modal, whether the work ran in a child
//   process here or in a container somewhere else; the user should not be able
//   to tell which from the dialog.
//
void CPreviewPage::FailJob(const CString& label, const CString& detail)
{
    CString tail;
    for (const CString& s : m_jobTail) { tail += s; tail += _T("\n"); }
    if (tail.IsEmpty()) tail = _T("(no output was produced)");

    CString msg;
    msg.Format(_T("%s failed (%s).\n\nLast output:\n\n%s"),
               (LPCTSTR)label, (LPCTSTR)detail, (LPCTSTR)tail);
    SetJobStatus(label + _T(": FAILED"));
    MessageBox(msg, label, MB_OK | MB_ICONERROR);
}

//
// StartServiceJob — POST a job to the terrain service and follow it.
//   The poll thread posts each new log line here, so the status line and the
//   failure modal behave exactly as they do for a locally supervised build.
//
void CPreviewPage::StartServiceJob(const CString& label, const std::string& jsonBody)
{
    if (m_jobActive || m_proc.IsRunning())
    {
        MessageBox(_T("A terrain job is already running.\n\nEvery job writes the same tile ")
                   _T("tree, so they cannot overlap."), label, MB_OK | MB_ICONINFORMATION);
        return;
    }

    const TerrainEndpoint ep = ResolveTerrainEndpoint(theApp.Settings());

    m_jobLabel = label;
    m_jobTail.clear();
    m_publishPending = false;   // the service holds its own tiles; nothing to publish
    m_jobActive = true;
    UpdateBuildButtons();

    if (!m_svc.StartJob(GetSafeHwnd(), ep.host, ep.port, jsonBody))
    {
        m_jobActive = false;
        UpdateBuildButtons();
        MessageBox(_T("Could not start the job (one is already in flight)."),
                   label, MB_OK | MB_ICONWARNING);
        return;
    }

    CString status;
    status.Format(_T("%s: submitted to %hs:%u..."), (LPCTSTR)label, ep.host.c_str(), ep.port);
    SetJobStatus(status);
    {
        char buf[320];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage: submitted '%s' to terrain service %s:%u",
                  (LPCSTR)CT2A(label), ep.host.c_str(), ep.port);
        LOG(buf);
    }
}

//
// OnTerrainJobLine — one log line from the running service job. Mirrors
//   OnProcOutput: newest into the status line, last N kept for the modal.
//
LRESULT CPreviewPage::OnTerrainJobLine(WPARAM, LPARAM lParam)
{
    char* line = reinterpret_cast<char*>(lParam);
    if (!line) return 0;

    const CString text(line);
    delete[] line;                       // the poster heap-allocated it

    if (!text.IsEmpty())
    {
        m_jobTail.push_back(text);
        while (m_jobTail.size() > 25) m_jobTail.pop_front();
        SetJobStatus(m_jobLabel + _T(": ") + text);
        char buf[512];
        sprintf_s(buf, sizeof(buf), "terrain job: %s", (LPCSTR)CT2A(text));
        LOG(buf);
    }
    return 0;
}

//
// OnTerrainJobDone — terminal state for a service job.
//
LRESULT CPreviewPage::OnTerrainJobDone(WPARAM wParam, LPARAM lParam)
{
    char* detail = reinterpret_cast<char*>(lParam);
    const CString why(detail ? detail : "");
    delete[] detail;

    m_jobActive = false;
    UpdateBuildButtons();

    if (wParam == 0)
    {
        SetJobStatus(m_jobLabel + _T(": done"));
        // Tiles just changed underneath DISBrowser; the probe also re-reads
        // health so a newly-populated tree stops reading as degraded.
        RefreshTerrainServerUi();
    }
    else
    {
        FailJob(m_jobLabel, why.IsEmpty() ? CString(_T("see log")) : why);
    }
    return 0;
}

//
// UpdateBuildButtons — the build actions all write the same tile tree, so only
//   one may run at a time. Previously nothing stopped the operator from
//   launching several concurrent tiling pipelines over each other.
//
void CPreviewPage::UpdateBuildButtons()
{
    // "idle" covers both job sources: a locally supervised child AND an HTTP
    // job running in the container. Either one owns the tile tree.
    const BOOL idle = (m_proc.IsRunning() || m_jobActive) ? FALSE : TRUE;
    const BOOL up   = m_terrainServerUp ? TRUE : FALSE;

    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_BUILD_TERRAIN))  b->EnableWindow(idle && up);
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_BUILD_FOLIAGE))  b->EnableWindow(idle && up);
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_PURGE_TERRAIN))  b->EnableWindow(idle);
}

//
// StartSupervised — launch one external job under the supervisor: output piped
//   into the status line and the app log, real exit code checked on completion.
//   Replaces ::ShellExecute, which could report only launch failures.
//
void CPreviewPage::StartSupervised(const CString& label,
                                   const CString& commandLine,
                                   const CString& workingDir)
{
    // Legacy mode reverts to the original behaviour on purpose: fire-and-forget
    // into a console window, no piped output, no exit code, no job object. It
    // exists so the old path can be fallen back to wholesale; the cost is that
    // failures are invisible again, which is what the newer modes fix.
    if (theApp.Settings().terrainMode == TerrainServerMode::Legacy)
    {
        // commandLine is "\"<exe>\" <args>" — ShellExecute wants them separated.
        CString exe = commandLine, args;
        if (commandLine.GetLength() > 1 && commandLine[0] == _T('"'))
        {
            const int close = commandLine.Find(_T('"'), 1);
            if (close > 0)
            {
                exe  = commandLine.Mid(1, close - 1);
                args = commandLine.Mid(close + 1);
                args.Trim();
            }
        }

        sprintf_s(szError, sizeof(szError), "PreviewPage: legacy launch (%s)",
                  (LPCSTR)CT2A(label));
        LOG(szError);

        HINSTANCE h = ::ShellExecute(GetSafeHwnd(), _T("open"), exe,
                                     args.IsEmpty() ? nullptr : (LPCTSTR)args,
                                     workingDir.IsEmpty() ? nullptr : (LPCTSTR)workingDir,
                                     SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32)
        {
            CString msg;
            msg.Format(_T("Failed to launch %s (error %Id)."), (LPCTSTR)label, (INT_PTR)h);
            MessageBox(msg, label, MB_OK | MB_ICONERROR);
            return;
        }
        SetJobStatus(label + _T(": running in its own console window (legacy mode)"));
        m_publishPending = false;   // legacy serves straight from ext4; no publish
        return;
    }

    if (m_proc.IsRunning())
    {
        CString msg;
        msg.Format(_T("%s is already running.\n\nWait for it to finish before starting another."),
                   (LPCTSTR)m_jobLabel);
        MessageBox(msg, label, MB_OK | MB_ICONINFORMATION);
        return;
    }

    m_jobLabel = label;
    m_jobTail.clear();

    if (!m_proc.Start(GetSafeHwnd(),
                      std::wstring(CT2W(commandLine)),
                      std::wstring(CT2W(workingDir)),
                      /*killWithApp*/ true))
    {
        CString msg;
        msg.Format(_T("Could not start %s.\n\n%s"),
                   (LPCTSTR)label, (LPCTSTR)CString(m_proc.LastError().c_str()));
        MessageBox(msg, label, MB_OK | MB_ICONERROR);
        SetJobStatus(label + _T(": failed to start"));
        return;
    }

    SetJobStatus(label + _T(": running..."));
    UpdateBuildButtons();
}

//
// OnProcOutput — one line from the child. The supervisor allocated it; we own
//   it now (contract documented on WM_APP_PROC_OUTPUT).
//
LRESULT CPreviewPage::OnProcOutput(WPARAM, LPARAM lParam)
{
    char* line = reinterpret_cast<char*>(lParam);
    if (!line) return 0;

    CString text(line);
    delete[] line;

    text.Trim();
    if (text.IsEmpty()) return 0;

    // The server chatters (404s, heartbeats) for as long as it runs; keep its
    // output out of the build status line, but retain a tail so an unexpected
    // stop can explain itself.
    if (m_server.IsRunning() && !m_proc.IsRunning())
    {
        m_serverTail.push_back(text);
        while (m_serverTail.size() > 10) m_serverTail.pop_front();
        return 0;
    }

    SetJobStatus(m_jobLabel + _T(": ") + text);

    // Keep a short tail so a failure can show what actually went wrong
    // instead of just an exit code.
    m_jobTail.push_back(text);
    while (m_jobTail.size() > 25) m_jobTail.pop_front();
    return 0;
}

//
// OnProcExit — the job finished. Non-zero exit is raised as a modal with the
//   tail of the child's own output: the whole point of this rework is that a
//   failure can no longer hide in a console window nobody is looking at.
//
LRESULT CPreviewPage::OnProcExit(WPARAM wParam, LPARAM lParam)
{
    const DWORD code      = static_cast<DWORD>(wParam);
    const bool  cancelled = (LOWORD(lParam) != 0);
    const WORD  tag       = HIWORD(lParam);

    // The terrain server shares this handler. Its exit is not a build result:
    // it means the server stopped (we stopped it, it was killed, or the port
    // was already taken), so report that and leave the build state alone.
    if (tag == kTagServer)
    {
        if (!cancelled && code != 0)
        {
            CString tail;
            for (const CString& s : m_serverTail) { tail += s; tail += _T("\n"); }
            CString msg;
            msg.Format(_T("The terrain server stopped unexpectedly (exit code %lu).\n\n%s"),
                       code, tail.IsEmpty() ? _T("(no output)") : (LPCTSTR)tail);
            SetJobStatus(_T("Terrain server: FAILED"));
            MessageBox(msg, _T("Terrain Server"), MB_OK | MB_ICONERROR);
        }
        RefreshTerrainServerUi();
        return 0;
    }

    m_proc.Join();
    UpdateBuildButtons();

    if (cancelled)
    {
        SetJobStatus(m_jobLabel + _T(": cancelled"));
        m_publishPending = false;
        return 0;
    }

    if (code != 0)
    {
        CString detail;
        detail.Format(_T("exit code %lu"), code);
        m_publishPending = false;
        FailJob(m_jobLabel, detail);
        return 0;
    }

    // Tiling wrote to ext4; publish to NTFS so the native server can see it.
    // Only Local mode needs this: Legacy's serve.py reads ext4 directly, and in
    // Remote mode the tiles being served live on another machine entirely.
    if (m_publishPending && theApp.Settings().terrainMode == TerrainServerMode::Local)
    {
        m_publishPending = false;
        SetJobStatus(m_jobLabel + _T(": tiling done, publishing tiles..."));

        const CString shWin  = ResolveDisBrowserScript(_T("wsl\\publish_tiles.sh"));
        const CString dest   = TerrainTilesDir();
        CString cmd;
        cmd.Format(_T("wsl.exe -d Ubuntu -u root -- bash \"%s\" \"%s\""),
                   (LPCTSTR)WslPathOf(shWin), (LPCTSTR)WslPathOf(dest));
        StartSupervised(_T("Publish terrain tiles"), cmd, _T(""));
        return 0;
    }

    m_publishPending = false;
    SetJobStatus(m_jobLabel + _T(": done"));
    return 0;
}

void CPreviewPage::OnBuildTerrain()
{
    if (!m_scenario || !m_scenario->terrainBoundsValid)
    {
        MessageBox(_T("Paint a terrain boundary first:\n\nClick \"Boundary\", then drag a rectangle on ")
                   _T("the map to mark the area you want as solid 3D terrain."),
                   _T("Build 3D Terrain"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // In Service mode the pipeline lives in the container, so the DISBrowser
    // scripts are irrelevant -- and requiring them would block a user who has
    // no DISBrowser checkout at all. Resolve them only when we run them here.
    CString projDir, cmdPath;
    if (!ServiceMode())
    {
        const ::Settings& st = theApp.Settings();
        if (!st.disBrowserProjectDir.empty())
            projDir = CString(st.disBrowserProjectDir.c_str());
        else
        {
            TCHAR exe[MAX_PATH] = { 0 };
            ::GetModuleFileName(nullptr, exe, _countof(exe));
            CString p(exe);
            int slash = p.ReverseFind(_T('\\'));
            if (slash > 0) p = p.Left(slash);
            projDir = p + _T("\\..\\..\\..\\DISBrowser");
        }
        cmdPath = projDir + _T("\\Scripts\\retile_terrain.cmd");

        if (::GetFileAttributes(cmdPath) == INVALID_FILE_ATTRIBUTES)
        {
            CString msg;
            msg.Format(_T("Cannot find the re-tile script:\n%s\n\nSet the DISBrowser project folder on the Run tab."),
                       (LPCTSTR)cmdPath);
            MessageBox(msg, _T("Build 3D Terrain"), MB_OK | MB_ICONWARNING);
            return;
        }
    }

    CString confirm;
    confirm.Format(_T("Download DEM + build 3D terrain for:\n\n")
                   _T("  lat  %.4f .. %.4f\n  lon  %.4f .. %.4f\n\n")
                   _T("%s Continue?"),
                   m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                   m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg,
                   ServiceMode()
                     ? _T("This is submitted to the terrain service, which downloads the DEM,\n")
                       _T("tiles it, and verifies the result. It can take several minutes.")
                     : _T("This runs the WSL/Docker tiling pipeline and can take several minutes."));
    if (MessageBox(confirm, _T("Build 3D Terrain"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;

    if (ServiceMode())
    {
        char body[256];
        sprintf_s(body, sizeof(body),
                  "{\"type\":\"build\",\"latMin\":%.6f,\"latMax\":%.6f,"
                  "\"lonMin\":%.6f,\"lonMax\":%.6f}",
                  m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                  m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg);
        StartServiceJob(_T("Build 3D Terrain"), body);
        return;
    }

    // retile_terrain.cmd latMin latMax lonMin lonMax
    CString args;
    args.Format(_T("%.6f %.6f %.6f %.6f"),
                m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg);

    // Tiling lands on ext4; publish to NTFS afterwards so TerrainServer.exe
    // (a Windows process, which cannot read /root/terrain) can serve it.
    m_publishPending = true;

    CString cmd;
    cmd.Format(_T("\"%s\" %s"), (LPCTSTR)cmdPath, (LPCTSTR)args);
    StartSupervised(_T("Build 3D Terrain"), cmd, projDir);
}

//
// OnBuildFoliage — bake the streamed i3dm tree tileset for the painted terrain box.
//   Mirrors OnBuildTerrain: needs a painted box + at least one tree type selected, then
//   ShellExecutes build_foliage.cmd, which samples the DEM, land-masks, and writes the
//   i3dm tileset served at http://localhost:8088/foliage/tileset.json. Bakes a placeholder
//   cone until real tree .glb's are wired into the script.
//
void CPreviewPage::OnBuildFoliage()
{
    if (!m_scenario || !m_scenario->terrainBoundsValid)
    {
        MessageBox(_T("Paint a terrain boundary and Build 3D Terrain first:\n\ni3dm foliage samples ")
                   _T("its tree heights from the built 3D terrain's DEM, so the terrain must exist."),
                   _T("Build i3dm Foliage"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    const FoliageConfig& fol = m_scenario->foliage;
    if (!(fol.oak || fol.bigTrees || fol.palm))
    {
        MessageBox(_T("No tree type selected.\n\nClick \"Foliage\" and tick Oak / Big Trees / Palm first."),
                   _T("Build i3dm Foliage"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    CString cmdPath;
    if (!ServiceMode())
    {
        cmdPath = ResolveDisBrowserScript(_T("build_foliage.cmd"));
        if (::GetFileAttributes(cmdPath) == INVALID_FILE_ATTRIBUTES)
        {
            CString msg;
            msg.Format(_T("Cannot find the foliage bake script:\n%s\n\nSet the DISBrowser project folder on the Run tab."),
                       (LPCTSTR)cmdPath);
            MessageBox(msg, _T("Build i3dm Foliage"), MB_OK | MB_ICONWARNING);
            return;
        }
    }

    if (ServiceMode() && m_scenario && !m_scenario->foliageAreas.empty())
    {
        long long total = 0;
        for (const FoliageArea& a : m_scenario->foliageAreas) total += a.treeCount;
        CString msg;
        msg.Format(_T("Bake the i3dm tree tileset for %d painted area(s), %lld trees total?\n\n")
                   _T("Where areas overlap, the shared ground belongs to the area painted ")
                   _T("first, so the overlap is not planted twice."),
                   static_cast<int>(m_scenario->foliageAreas.size()), total);
        if (MessageBox(msg, _T("Build i3dm Foliage"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;

        std::string body = "{\"type\":\"foliage\",\"minLand\":0.5,\"rects\":[";
        for (size_t i = 0; i < m_scenario->foliageAreas.size(); ++i)
        {
            const FoliageArea& a = m_scenario->foliageAreas[i];
            char one[240];
            sprintf_s(one, sizeof(one),
                      "%s{\"latMin\":%.6f,\"latMax\":%.6f,\"lonMin\":%.6f,"
                      "\"lonMax\":%.6f,\"count\":%lld}",
                      i ? "," : "",
                      a.latMinDeg, a.latMaxDeg, a.lonMinDeg, a.lonMaxDeg, a.treeCount);
            body += one;
        }
        body += "]}";
        StartServiceJob(_T("Build i3dm Foliage"), body);
        return;
    }

    CString confirm;
    confirm.Format(_T("Bake the i3dm tree tileset for:\n\n")
                   _T("  lat  %.4f .. %.4f\n  lon  %.4f .. %.4f\n\n")
                   _T("Samples tree heights from the built DEM, land-masks the ocean, and writes\n")
                   _T("the streamed i3dm tileset. Runs the WSL/Docker pipeline. Continue?"),
                   m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                   m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg);
    if (MessageBox(confirm, _T("Build i3dm Foliage"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;

    if (ServiceMode())
    {
        // Reaching here means NOTHING was painted -- the branch above returns
        // whenever there are areas. Fall back to the terrain boundary as a
        // single area so the button still does the obvious thing.
        //
        // NO "count" here, deliberately. Every painted area carries its own
        // count; this is the one request without one, so it omits the field and
        // lets the service apply [Foliage] DefaultCount. Hardcoding a number
        // here would be a second, silently disagreeing default.
        char one[220];
        sprintf_s(one, sizeof(one),
                  "{\"latMin\":%.6f,\"latMax\":%.6f,\"lonMin\":%.6f,"
                  "\"lonMax\":%.6f}",
                  m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                  m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg);
        std::string body = "{\"type\":\"foliage\",\"minLand\":0.5,\"rects\":[";
        body += one;
        body += "]}";
        StartServiceJob(_T("Build i3dm Foliage"), body);
        return;
    }

    // build_foliage.cmd latMin latMax lonMin lonMax --count N --min-land M
    CString args;
    args.Format(_T("%.6f %.6f %.6f %.6f --count 20000 --min-land 0.5"),
                m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg);

    // The i3dm tileset is written under tiles/foliage/, so it needs publishing
    // to NTFS exactly like the terrain does.
    m_publishPending = true;

    CString cmd;
    cmd.Format(_T("\"%s\" %s"), (LPCTSTR)cmdPath, (LPCTSTR)args);
    StartSupervised(_T("Build i3dm Foliage"), cmd, _T(""));
}

//
// OnPurgeTerrain — wipe ALL downloaded terrain (every landscape) after a strong
//   confirmation, then start fresh. Runs retile_terrain.cmd --purge --yes; the
//   GUI Yes/No is the real gate, so --yes skips the script's console prompt.
//
void CPreviewPage::OnPurgeTerrain()
{
    CString cmdPath;
    if (!ServiceMode())
    {
        cmdPath = ResolveDisBrowserScript(_T("retile_terrain.cmd"));
        if (::GetFileAttributes(cmdPath) == INVALID_FILE_ATTRIBUTES)
        {
            CString msg;
            msg.Format(_T("Cannot find the re-tile script:\n%s\n\nSet the DISBrowser project folder on the Run tab."),
                       (LPCTSTR)cmdPath);
            MessageBox(msg, _T("Purge Terrain"), MB_OK | MB_ICONWARNING);
            return;
        }
    }

    if (MessageBox(_T("Delete ALL downloaded 3D terrain?\n\n")
                   _T("This wipes every DEM tile and the entire served tile tree for EVERY ")
                   _T("landscape (Cesium falls back to a bare globe until you build again). ")
                   _T("This cannot be undone."),
                   _T("Purge Terrain"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    if (ServiceMode())
    {
        // The service purges its own tree. Critically, we must NOT also delete
        // the NTFS copy below: in Service mode that copy belongs to the Local
        // fallback, and wiping it would destroy the very thing you fall back to.
        StartServiceJob(_T("Purge Terrain"), "{\"type\":\"purge\"}");
        return;
    }

    CString cmd;
    cmd.Format(_T("\"%s\" --purge --yes"), (LPCTSTR)cmdPath);
    StartSupervised(_T("Purge Terrain"), cmd, _T(""));

    // The published NTFS copy is now stale; drop it so the server doesn't keep
    // serving tiles the source no longer has. Local mode only -- see above.
    const CString dest = TerrainTilesDir();
    if (::GetFileAttributes(dest) != INVALID_FILE_ATTRIBUTES)
    {
        SHFILEOPSTRUCT op = { 0 };
        CString from = dest;
        from.AppendChar(_T('\0'));          // SHFileOperation wants a double-NUL list
        op.wFunc  = FO_DELETE;
        op.pFrom  = from;
        op.fFlags = FOF_NO_UI;
        ::SHFileOperation(&op);
    }
}

CString CPreviewPage::ResolveDisBrowserScript(const CString& fileName) const
{
    // Same project-dir logic the Run tab / OnBuildTerrain use.
    CString projDir;
    const ::Settings& st = theApp.Settings();
    if (!st.disBrowserProjectDir.empty())
        projDir = CString(st.disBrowserProjectDir.c_str());
    else
    {
        TCHAR exe[MAX_PATH] = { 0 };
        ::GetModuleFileName(nullptr, exe, _countof(exe));
        CString p(exe);
        int slash = p.ReverseFind(_T('\\'));
        if (slash > 0) p = p.Left(slash);
        projDir = p + _T("\\..\\..\\..\\DISBrowser");
    }
    return projDir + _T("\\Scripts\\") + fileName;
}

void CPreviewPage::RefreshTerrainServerUi()
{
    const bool up = ProbeTcpPort(TerrainHost(), TerrainPort(), 250);
    const bool changed = (up != m_terrainServerUp);
    m_terrainServerUp = up;

    // Painting a boundary or building tiles only makes sense once the server serves
    // them, so gate those behind the server being up. Stop is only useful when up;
    // Start stays enabled (it repaints its own green "running" check).
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_BOUNDARY))      b->EnableWindow(up);
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_TERRAIN_STOP))  b->EnableWindow(up);
    // Build/Purge also depend on no job already being in flight — they all
    // write the same tile tree, so concurrent runs would corrupt it.
    UpdateBuildButtons();
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_TERRAIN_START)) b->Invalidate();

    // If the server dropped while boundary paint was armed, disarm it so the canvas
    // doesn't keep a live paint gesture against a disabled button.
    if (!up && m_boundaryArmed)
    {
        m_boundaryArmed = false;
        if (CWnd* bb = GetDlgItem(IDC_BTN_PREVIEW_BOUNDARY)) bb->Invalidate();
        if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
        if (m_mapLayer == MapLayer::Cesium && m_cesium.GetSafeHwnd())
            m_cesium.SetBoundaryMode(false);
    }

    // In Service mode the port answering is not the whole story: the container
    // can be up with an empty or unmounted tile tree, which health reports as
    // "degraded". Ask at most every ~5s -- the TCP probe stays the cheap check.
    if (up && ServiceMode())
    {
        if (++m_healthTick >= 4)
        {
            m_healthTick = 0;
            int status = 0;
            std::string body;
            const TerrainEndpoint ep = ResolveTerrainEndpoint(theApp.Settings());
            if (TerrainServiceClient::Get(ep.host, ep.port, "/api/v1/health", status, body) &&
                status == 200)
            {
                std::string s;
                TerrainServiceClient::FindString(body, "status", s);
                const CString now(s.c_str());
                if (now != m_svcHealth)
                {
                    m_svcHealth = now;
                    char buf[160];
                    sprintf_s(buf, sizeof(buf),
                              "PreviewPage: terrain service health=%s", s.c_str());
                    LOG(buf);
                    if (now == _T("degraded") && !m_jobActive)
                        SetJobStatus(_T("Terrain service: reachable but no tiles yet ")
                                     _T("(build one to populate it)"));
                }
            }
        }
    }
    else m_svcHealth.Empty();

    if (changed)
    {
        const TerrainEndpoint ep = ResolveTerrainEndpoint(theApp.Settings());
        // Was hardcoded "localhost:8088" regardless of mode, which made a
        // Remote/Service outage read as a local one.
        char buf[200];
        sprintf_s(buf, sizeof(buf),
                  up ? "PreviewPage: terrain server UP (%s:%u)"
                     : "PreviewPage: terrain server DOWN (%s:%u)",
                  ep.host.c_str(), ep.port);
        LOG(buf);
    }
}

//
// OnStartTerrainServer — launch the self-hosted tile server (serve_terrain.cmd) in
//   its own console unless it's already up; the poll timer flips the UI once :8088
//   answers.
//
void CPreviewPage::OnStartTerrainServer()
{
    if (m_terrainServerUp || ProbeTcpPort(TerrainHost(), TerrainPort(), 250))
    {
        RefreshTerrainServerUi();   // already running — just sync the UI
        return;
    }

    const ::Settings& stt = theApp.Settings();

    // Remote: the tiles are served by another machine, so there is nothing to
    // start. Say so rather than silently doing nothing.
    if (stt.terrainMode == TerrainServerMode::Remote)
    {
        CString msg;
        msg.Format(_T("Terrain mode is Remote (%hs:%u), so there is no local server to start.\n\n")
                   _T("The editor only checks that the remote server answers. To run one here, ")
                   _T("set [Terrain] Mode=Local (or Legacy) in settings.ini."),
                   (LPCSTR)TerrainHost(), static_cast<unsigned>(TerrainPort()));
        MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (stt.terrainMode == TerrainServerMode::Service)
    {
        // The container is deliberately not ours to start: it has
        // restart: unless-stopped and outlives every editor session.
        CString msg;
        msg.Format(_T("Terrain mode is Service (%hs:%u).\n\n")
                   _T("The terrain service runs as an independent container and is not started ")
                   _T("or stopped from here. Bring it up in WSL with:\n\n")
                   _T("    cd ~/projects/planeswalker/terrainserver\n")
                   _T("    ./scripts/up.sh"),
                   (LPCSTR)TerrainHost(), static_cast<unsigned>(TerrainPort()));
        MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONINFORMATION);
        RefreshTerrainServerUi();
        return;
    }

    // Legacy: the original console-window path, kept as a fallback.
    if (stt.terrainMode == TerrainServerMode::Legacy)
    {
        CString cmdPath = ResolveDisBrowserScript(_T("serve_terrain.cmd"));
        if (::GetFileAttributes(cmdPath) == INVALID_FILE_ATTRIBUTES)
        {
            CString msg;
            msg.Format(_T("Cannot find the terrain server script:\n%s\n\nSet the DISBrowser project folder on the Run tab."),
                       (LPCTSTR)cmdPath);
            MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONWARNING);
            return;
        }
        // Its console window is load-bearing here: the foreground wsl.exe is what
        // keeps the WSL VM (and therefore serve.py) alive.
        HINSTANCE h = ::ShellExecute(GetSafeHwnd(), _T("open"), cmdPath, nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32)
        {
            CString msg;
            msg.Format(_T("Failed to launch the terrain server (error %Id)."), (INT_PTR)h);
            MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONERROR);
            return;
        }
        SetJobStatus(_T("Terrain server: launching legacy serve.py in WSL (see its console window)"));
        RefreshTerrainServerUi();
        return;
    }

    // Local: TerrainServer.exe ships beside the editor. No console window, no
    // WSL, and it can be stopped by handle instead of by `pkill -f serve.py`
    // against a guessed distro name.
    TCHAR exe[MAX_PATH] = { 0 };
    ::GetModuleFileName(nullptr, exe, _countof(exe));
    CString serverExe(exe);
    const int slash = serverExe.ReverseFind(_T('\\'));
    if (slash > 0) serverExe = serverExe.Left(slash);
    serverExe += _T("\\TerrainServer.exe");

    if (::GetFileAttributes(serverExe) == INVALID_FILE_ATTRIBUTES)
    {
        CString msg;
        msg.Format(_T("Cannot find the terrain server:\n%s\n\nRebuild the solution ")
                   _T("(the TerrainServer target builds alongside ScenarioEditor)."),
                   (LPCTSTR)serverExe);
        MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONWARNING);
        return;
    }

    const CString tiles = TerrainTilesDir();
    if (::GetFileAttributes(tiles) == INVALID_FILE_ATTRIBUTES)
    {
        CString msg;
        msg.Format(_T("No published terrain tiles yet:\n%s\n\nRun \"Build 3D Terrain\" ")
                   _T("- tiles are published there when tiling finishes."),
                   (LPCTSTR)tiles);
        MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    CString cmd;
    cmd.Format(_T("\"%s\" \"%s\" 8088"), (LPCTSTR)serverExe, (LPCTSTR)tiles);

    // killWithApp = false: closing the editor deliberately leaves the server
    // running so a live DISBrowser keeps its terrain (see OnDestroy).
    if (!m_server.Start(GetSafeHwnd(), std::wstring(CT2W(cmd)), std::wstring(), false))
    {
        CString msg;
        msg.Format(_T("Failed to start the terrain server.\n\n%s"),
                   (LPCTSTR)CString(m_server.LastError().c_str()));
        MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONERROR);
        return;
    }

    SetJobStatus(_T("Terrain server: starting on http://localhost:8088"));
    // The poll timer flips the UI to "running" as soon as the port answers.
    RefreshTerrainServerUi();
}

void CPreviewPage::OnStopTerrainServer()
{
    const ::Settings& stt = theApp.Settings();

    if (stt.terrainMode == TerrainServerMode::Remote)
    {
        MessageBox(_T("Terrain mode is Remote, so there is no local server to stop.\n\n")
                   _T("Stop it on the machine that hosts it."),
                   _T("Stop Terrain Server"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (stt.terrainMode == TerrainServerMode::Service)
    {
        MessageBox(_T("Terrain mode is Service, so there is no local server to stop.\n\n")
                   _T("Stop the container in WSL:\n\n")
                   _T("    cd ~/projects/planeswalker/terrainserver\n")
                   _T("    ./scripts/down.sh"),
                   _T("Stop Terrain Server"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (stt.terrainMode == TerrainServerMode::Legacy)
    {
        // Legacy has no handle to hold: kill by process name inside WSL, exactly
        // as before. Fire-and-forget, and it only finds serve.py in this distro.
        ::ShellExecute(GetSafeHwnd(), _T("open"), _T("wsl.exe"),
                       _T("-d Ubuntu -u root -- pkill -f serve.py"), nullptr, SW_HIDE);
        SetJobStatus(_T("Terrain server: sent pkill to serve.py in WSL"));
        RefreshTerrainServerUi();
        return;
    }

    if (m_server.IsRunning())
    {
        // Stop the process we actually started, by handle.
        m_server.Cancel();
        m_server.Join();
        SetJobStatus(_T("Terrain server: stopped"));
    }
    else
    {
        // Nothing of ours is running, but the port may still be held by a
        // server left over from an earlier editor session (that is the point of
        // not killing it on exit) or by a legacy serve.py inside WSL.
        if (ProbeTcpPort(TerrainHost(), TerrainPort(), 250))
        {
            MessageBox(_T("Port 8088 is being served by a process this editor did not start ")
                       _T("- most likely a terrain server left running from an earlier session, ")
                       _T("or a legacy serve.py inside WSL.\n\n")
                       _T("Close that process (Task Manager: TerrainServer.exe) to free the port."),
                       _T("Stop Terrain Server"), MB_OK | MB_ICONINFORMATION);
        }
    }
    // The port may take a beat to drop; the poll timer settles the final state.
    RefreshTerrainServerUi();
}

//
// DrawCheckBox — paint one "Show" check box in the page's dark theme, with the
//   box drawn kCheckScale times the size Windows would use. The stock glyph is a
//   fixed 13 logical pixels sized for an 8pt dialog; this page's template is 12pt,
//   so the system box ends up far smaller than the text beside it and is genuinely
//   hard to see. Drawing it ourselves is the only way to change that size -- a
//   check box glyph is not settable by style or message -- and it also lets the
//   box follow the display DPI and the row height instead of a fixed pixel count.
//
//   Owner-draw buttons do not track their own state at all, so the checked flag
//   comes from CheckBoxState (the m_show* member), never from the control.
//
void CPreviewPage::DrawCheckBox(LPDRAWITEMSTRUCT dis)
{
    if (!dis) return;

    const bool checked = CheckBoxState(static_cast<int>(dis->CtlID));

    CDC* dc  = CDC::FromHandle(dis->hDC);
    CRect rc = dis->rcItem;

    const COLORREF kText    = RGB(255, 255, 255);
    const COLORREF kBack    = RGB(0, 0, 0);
    const COLORREF kBoxEdge = RGB(200, 200, 200);
    const COLORREF kCheck   = RGB(80, 220, 80);   // the green the armed buttons use

    // The page is a dark theme, but a BS_OWNERDRAW button gets no WM_CTLCOLORBTN
    // treatment -- without this fill it comes up on the default light button face.
    dc->FillSolidRect(&rc, kBack);

    // Box side: the stock glyph scaled for this display's DPI, times kCheckScale,
    // but never taller than the row it has to sit in.
    const int dpiY    = dc->GetDeviceCaps(LOGPIXELSY);
    const int baseBox = std::max(1, MulDiv(kCheckBaseLogicalPx, dpiY, 96));
    int box = baseBox * kCheckScale;
    box = std::min<int>(box, std::max<int>(1, rc.Height() - 2));

    const int boxTop = rc.top + (rc.Height() - box) / 2;
    CRect boxRc(rc.left, boxTop, rc.left + box, boxTop + box);

    // Outline. A hairline edge would vanish next to a doubled box, so scale it too.
    {
        const int edgeW = std::max(1, box / 10);
        CPen pen(PS_SOLID, edgeW, kBoxEdge);
        CPen*   oldPen   = dc->SelectObject(&pen);
        CBrush* oldBrush = (CBrush*)dc->SelectStockObject(NULL_BRUSH);
        dc->Rectangle(&boxRc);
        if (oldPen)   dc->SelectObject(oldPen);
        if (oldBrush) dc->SelectObject(oldBrush);
    }

    // Check mark: a two-segment stroke inside the box, round-capped and joined so
    // the corner reads cleanly at this weight.
    if (checked)
    {
        const LOGBRUSH lb = { BS_SOLID, kCheck, 0 };
        HPEN hp = ::ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                 std::max(2, box / 6), &lb, 0, nullptr);
        if (hp)
        {
            HGDIOBJ oldPen = ::SelectObject(dis->hDC, hp);
            auto at = [&](double fx, double fy)
            {
                return CPoint(boxRc.left + static_cast<int>(box * fx + 0.5),
                              boxRc.top  + static_cast<int>(box * fy + 0.5));
            };
            const CPoint pts[3] = { at(0.24, 0.52), at(0.43, 0.73), at(0.78, 0.27) };
            dc->Polyline(pts, 3);
            ::SelectObject(dis->hDC, oldPen);
            ::DeleteObject(hp);
        }
    }

    // Label, vertically centered against the box.
    CString label;
    ::GetWindowText(dis->hwndItem, label.GetBuffer(128), 128);
    label.ReleaseBuffer();

    HFONT  hf      = (HFONT)::SendMessage(dis->hwndItem, WM_GETFONT, 0, 0);
    CFont* oldFont = hf ? dc->SelectObject(CFont::FromHandle(hf)) : nullptr;
    const int      oldBk    = dc->SetBkMode(TRANSPARENT);
    const COLORREF oldColor = dc->SetTextColor(kText);

    CRect textRc(boxRc.right + static_cast<int>(box * kCheckLabelGap),
                 rc.top, rc.right, rc.bottom);
    dc->DrawText(label, &textRc,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Focus ring around the label, so keyboard users can still see where they are.
    if (dis->itemState & ODS_FOCUS)
    {
        CRect focusRc = textRc;
        focusRc.right = std::min<LONG>(focusRc.right,
                                       textRc.left + dc->GetTextExtent(label).cx + 2);
        dc->DrawFocusRect(&focusRc);
    }

    dc->SetTextColor(oldColor);
    dc->SetBkMode(oldBk);
    if (oldFont) dc->SelectObject(oldFont);
}

void CPreviewPage::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDIS)
{
    // Three owner-draw buttons share this painter: "Set Start" (green "?" while armed),
    // "Boundary" (bold green check while armed), and "Start Terrain Server" (bold green
    // check while the tile server is running). Everything else defers to the base class.
    const bool isSetStart = (nIDCtl == IDC_BTN_PREVIEW_SET_START);
    const bool isBoundary = (nIDCtl == IDC_BTN_PREVIEW_BOUNDARY);
    const bool isSrvStart = (nIDCtl == IDC_BTN_PREVIEW_TERRAIN_START);
    const bool isShowOrig = (nIDCtl == IDC_BTN_PREVIEW_SHOW_ORIGIN);
    const bool isFolArea  = (nIDCtl == IDC_BTN_PREVIEW_FOLIAGE_AREA);

    // The "Show" check boxes are owner-draw too, but painted by DrawCheckBox --
    // they want a box-and-label, not a push-button face.
    if (lpDIS && IsPreviewCheckBox(nIDCtl))
    {
        DrawCheckBox(lpDIS);
        return;
    }

    if ((!isSetStart && !isBoundary && !isSrvStart && !isShowOrig && !isFolArea) || !lpDIS)
    {
        CHelpAwarePage::OnDrawItem(nIDCtl, lpDIS);
        return;
    }

    CDC* dc = CDC::FromHandle(lpDIS->hDC);
    CRect rc = lpDIS->rcItem;
    const bool pressed = (lpDIS->itemState & ODS_SELECTED) != 0;

    // Button face.
    dc->DrawFrameControl(&rc, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0));

    HFONT hf = (HFONT)::SendMessage(lpDIS->hwndItem, WM_GETFONT, 0, 0);
    CFont* oldFont = hf ? dc->SelectObject(CFont::FromHandle(hf)) : nullptr;
    const int oldBk = dc->SetBkMode(TRANSPARENT);

    // Label + a trailing green hint glyph while the mode is armed / server is up /
    // origin marker shown.
    const bool  showHint = isSetStart ? m_startPickArmed
                         : isBoundary ? m_boundaryArmed
                         : isFolArea  ? m_foliageAreaArmed
                         : isShowOrig ? m_showOrigin
                                      : m_terrainServerUp;
    // The Foliage Area label carries the painted count, so the tab shows at a
    // glance whether a bake will use one rectangle or several.
    CString folLabel(_T("Foliage Area"));
    if (isFolArea && m_scenario && !m_scenario->foliageAreas.empty())
        folLabel.Format(_T("Foliage Area (%d)"),
                        static_cast<int>(m_scenario->foliageAreas.size()));

    CString base = isSetStart ? _T("Set Start")
                 : isBoundary ? _T("Boundary")
                 : isFolArea  ? (LPCTSTR)folLabel   // cast: keep every arm LPCTSTR so the
                                                    // chained ?: has one unambiguous type
                 : isShowOrig ? _T("Show Origin")
                 : (m_terrainServerUp ? _T("Terrain Server") : _T("Start Terrain Server"));
    CString hint = isSetStart ? _T(" ?") : _T(" \x2713");   // U+2713 check for Boundary/Server/Origin

    // The Boundary / Server / Origin check is drawn in a bold font so it reads clearly.
    CFont  boldFont;
    CFont* boldPtr = nullptr;
    if ((isBoundary || isSrvStart || isShowOrig) && showHint && hf)
    {
        LOGFONT lf; ::ZeroMemory(&lf, sizeof(lf));
        if (::GetObject(hf, sizeof(lf), &lf))
        {
            lf.lfWeight = FW_BOLD;
            if (boldFont.CreateFontIndirect(&lf)) boldPtr = &boldFont;
        }
    }

    const CSize baseSz = dc->GetTextExtent(base);
    CSize hintSz(0, 0);
    if (showHint)
    {
        CFont* prev = boldPtr ? dc->SelectObject(boldPtr) : nullptr;
        hintSz = dc->GetTextExtent(hint);
        if (prev) dc->SelectObject(prev);
    }
    const int totalW = baseSz.cx + hintSz.cx;

    if (pressed) rc.OffsetRect(1, 1);   // classic pressed nudge
    int x = rc.left + (rc.Width()  - totalW) / 2;
    const int y = rc.top + (rc.Height() - baseSz.cy) / 2;

    const COLORREF oldColor = dc->SetTextColor(::GetSysColor(COLOR_BTNTEXT));
    dc->TextOut(x, y, base);
    x += baseSz.cx;
    if (showHint)
    {
        dc->SetTextColor(RGB(0, 160, 0));
        CFont* prev = boldPtr ? dc->SelectObject(boldPtr) : nullptr;
        dc->TextOut(x, y, hint);
        if (prev) dc->SelectObject(prev);
    }

    dc->SetTextColor(oldColor);
    dc->SetBkMode(oldBk);
    if (oldFont) dc->SelectObject(oldFont);
}

void CPreviewPage::ApplyStartPick(size_t entityIdx, size_t segIdx,
                                  double enuE, double enuN)
{
    if (!m_scenario) return;
    if (entityIdx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[entityIdx];
    if (segIdx >= e.motionSegments.size()) return;
    MotionSegment& seg = e.motionSegments[segIdx];

    // Ellipse center (foci midpoint, ENU) from the matching pick target.
    double centerE = 0.0, centerN = 0.0;
    bool found = false;
    for (const PreviewEllipse& t : m_ellipseTargets)
        if (t.entityIdx == entityIdx && t.segIdx == segIdx)
        {
            centerE = t.centerE; centerN = t.centerN; found = true; break;
        }
    if (!found) return;

    // Compass bearing (0=N, 90=E) from the ellipse center to the snapped point.
    // Inverse of the sampler's bearing→start-point mapping, so the start lands
    // where the user clicked.
    const double vE = enuE - centerE;
    const double vN = enuN - centerN;
    double brg = std::atan2(vE, vN) * (180.0 / 3.14159265358979323846);
    if (brg < 0.0) brg += 360.0;
    seg.startBearingDeg = brg;

    {
        char buf[200];
        sprintf_s(buf, sizeof(buf),
                  "PreviewPage::ApplyStartPick: entity=%zu seg=%zu bearing=%.2f deg",
                  entityIdx, segIdx, brg);
        LOG(buf);
    }

    // Pick complete: leave pick mode (this hides the green "?") and rebuild so
    // the start dot + t=0 entity dot move to the picked point.
    m_startPickArmed = false;
    if (CWnd* btn = GetDlgItem(IDC_BTN_PREVIEW_SET_START))
        btn->Invalidate();

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::DragEntityTo(size_t idx, double enuE, double enuN)
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return;
    Entity& ent = m_scenario->entities[idx];

    // Preserve current altitude — the drag is a 2-D move in the top-down view.
    const double up = (idx < m_state.poses.size()) ? m_state.poses[idx].enuU : 0.0;

    // The t=0 dot comes from the first ENABLED segment, if any, else the
    // entity's static initial pose — mirror MotionSampler::SamplePose so we
    // rewrite exactly the field that drives the start dot.
    int segIdx = -1;
    for (size_t k = 0; k < ent.motionSegments.size(); ++k)
        if (ent.motionSegments[k].enabled) { segIdx = static_cast<int>(k); break; }

    if (segIdx < 0)
    {
        SetEntityInitialEnu(ent, enuE, enuN, up);
    }
    else
    {
        MotionSegment& seg = ent.motionSegments[static_cast<size_t>(segIdx)];
        if (seg.type == MotionType::Ellipse)
        {
            // Dragging the entity resizes/moves the orbit so it passes through the
            // dragged point (foci fixed); that point becomes the orbit's start.
            // Non-Local ellipses fall back to sliding the start bearing.
            if (seg.coordMode == CoordMode::Local)
                SnapEllipse(seg, enuE, enuN);
            else
                SetEllipseStartBearing(idx, static_cast<size_t>(segIdx), seg, enuE, enuN);
        }
        else
        {
            SetSegmentStartEnu(seg, enuE, enuN, up);

            // Moving the START changes the leg length, so its travel time must be
            // re-derived from speed — otherwise the window goes stale and a take-off
            // leg's acceleration curve breaks (it rockets straight to cruise instead
            // of easing in). Mirror DragLineEnd: take-off uses the 2*dist/speed accel
            // window (so the smoothstep ramp reaches EXACTLY cruise at the end); a
            // normal Line uses dist/speed. Then cascade-retime the following legs.
            if (seg.type == MotionType::Line && seg.coordMode == CoordMode::Local &&
                seg.speedMps > 0.0)
            {
                const double dx = seg.endLocalX - seg.startLocalX;
                const double dy = seg.endLocalY - seg.startLocalY;
                const double dz = seg.endLocalZ - seg.startLocalZ;
                const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist > 0.0)
                    seg.endSecond = seg.startSecond +
                        (seg.accelerateFromStop
                             ? (MotionSampler::TakeoffWindowFactor() * dist / seg.speedMps)
                             : (dist / seg.speedMps));
                RechainFollowingTimes(ent, static_cast<size_t>(segIdx));
            }
        }
    }

    // Remember this drop so EndEntityDrag can log the drop ENU alongside the
    // resulting streamed position (entity-drag-drop diagnostic).
    m_dragDiagIdx  = static_cast<int>(idx);
    m_dragDiagEnuE = enuE;
    m_dragDiagEnuN = enuN;

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::DragZoneTo(size_t idx, double centerE, double centerN)
{
    if (!m_scenario || idx >= m_scenario->level.zones.size()) return;
    LevelZone& z = m_scenario->level.zones[idx];

    // Move the whole box: keep its width/height, recenter on (centerE, centerN).
    const double halfE = 0.5 * (z.eastMaxM  - z.eastMinM);
    const double halfN = 0.5 * (z.northMaxM - z.northMinM);
    z.eastMinM  = centerE - halfE;
    z.eastMaxM  = centerE + halfE;
    z.northMinM = centerN - halfN;
    z.northMaxM = centerN + halfN;

    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::EndZoneDrag()
{
    // Commit: reload the other tabs from the shared model and flag dirty so the
    // moved zone persists into the .ini on Save (mirrors EndEntityDrag).
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::EndEntityDrag()
{
    // Reload the other tabs from the shared model and flag the scenario dirty
    // so the new start location persists into the .ini on Save. SendMessage
    // (synchronous) matches the WM_APP_MARK_DIRTY convention used elsewhere.
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);

    // ---- Entity drag-drop diagnostic (appends to error.log) --------------------
    // Logs the DROP point (screen->ENU, so |drop| tells us how far from origin the
    // user actually released) against the AUTHORITATIVE t=0 pose the DIS stream will
    // send (MotionSampler::SamplePose at t=0). If the drop is ~0 but the streamed
    // pose is 25 km, the storage path is wrong; if the drop itself is 25 km, the
    // release landed on mis-georeferenced tiles / the wrong spot.
    if (m_scenario && m_dragDiagIdx >= 0 &&
        static_cast<size_t>(m_dragDiagIdx) < m_scenario->entities.size())
    {
        const Entity& e = m_scenario->entities[static_cast<size_t>(m_dragDiagIdx)];
        const double oLat = m_scenario->originLatDeg;
        const double oLon = m_scenario->originLonDeg;
        const double oAlt = m_scenario->originAltM;

        const double dropE = m_dragDiagEnuE, dropN = m_dragDiagEnuN;
        const double dropDist = std::sqrt(dropE * dropE + dropN * dropN);

        // Authoritative streamed pose at t=0 (what DISBrowser will actually receive).
        const SampledPose p0 = MotionSampler::SamplePose(e, *m_scenario, 0.0);
        double sE = 0, sN = 0, sU = 0, sLat = 0, sLon = 0, sAlt = 0;
        CoordTransforms::EcefToLocalEnuDeg(p0.ecefX, p0.ecefY, p0.ecefZ,
                                           oLat, oLon, oAlt, sE, sN, sU);
        CoordTransforms::EcefToGeodeticDeg(p0.ecefX, p0.ecefY, p0.ecefZ, sLat, sLon, sAlt);
        const double sDist = std::sqrt(sE * sE + sN * sN);

        // Which field the drag wrote: the first ENABLED segment's start, else the
        // entity's static initial pose (mirror of DragEntityTo's own branch choice).
        int segIdx = -1;
        for (size_t k = 0; k < e.motionSegments.size(); ++k)
            if (e.motionSegments[k].enabled) { segIdx = static_cast<int>(k); break; }

        char buf[1400];
        if (segIdx >= 0)
        {
            const MotionSegment& s = e.motionSegments[static_cast<size_t>(segIdx)];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "ENTITY DROP '%s' idx=%d -> wrote seg %d start\n"
                "  drop ENU   = E %.1f  N %.1f m   (|drop| %.1f m = %.2f km from origin)\n"
                "  origin     = lat %.6f  lon %.6f  alt %.1f\n"
                "  t=0 STREAM = ECEF(%.1f,%.1f,%.1f) -> ENU E %.1f N %.1f U %.1f  = %.2f km ; geo lat %.6f lon %.6f\n"
                "  seg.start  = local(%.1f,%.1f,%.1f)  ecef(%.1f,%.1f,%.1f)  lat %.6f lon %.6f  mode=%d",
                e.name.c_str(), m_dragDiagIdx, segIdx,
                dropE, dropN, dropDist, dropDist / 1000.0,
                oLat, oLon, oAlt,
                p0.ecefX, p0.ecefY, p0.ecefZ, sE, sN, sU, sDist / 1000.0, sLat, sLon,
                s.startLocalX, s.startLocalY, s.startLocalZ,
                s.startEcefX, s.startEcefY, s.startEcefZ,
                s.startLat, s.startLon, static_cast<int>(s.coordMode));
        }
        else
        {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "ENTITY DROP '%s' idx=%d -> wrote entity initial\n"
                "  drop ENU   = E %.1f  N %.1f m   (|drop| %.1f m = %.2f km from origin)\n"
                "  origin     = lat %.6f  lon %.6f  alt %.1f\n"
                "  t=0 STREAM = ECEF(%.1f,%.1f,%.1f) -> ENU E %.1f N %.1f U %.1f  = %.2f km ; geo lat %.6f lon %.6f\n"
                "  ent.init   = local(%.1f,%.1f,%.1f)  ecef(%.1f,%.1f,%.1f)  lat %.6f lon %.6f",
                e.name.c_str(), m_dragDiagIdx,
                dropE, dropN, dropDist, dropDist / 1000.0,
                oLat, oLon, oAlt,
                p0.ecefX, p0.ecefY, p0.ecefZ, sE, sN, sU, sDist / 1000.0, sLat, sLon,
                e.localX, e.localY, e.localZ,
                e.ecefX, e.ecefY, e.ecefZ, e.lat, e.lon);
        }
        LOG(buf);
        m_dragDiagIdx = -1;
    }
}

void CPreviewPage::DuplicateEntity(size_t idx)
{
    if (!m_scenario) return;
    if (idx >= m_scenario->entities.size()) return;

    Entity copy = m_scenario->entities[idx];   // deep copy incl. motionSegments

    // Unique EntityID = max existing + 1 (same rule as the Assets page).
    uint16_t next = 1;
    for (const Entity& e : m_scenario->entities)
        if (e.entityId >= next) next = static_cast<uint16_t>(e.entityId + 1);
    copy.entityId = next;

    // Suffix the *displayed* label and keep name + marking in sync so every view
    // (preview uses marking, Assets tree uses name) shows e.g. GARC-1.
    const std::string newLabel =
        MakeDuplicateName(*m_scenario, EntityDisplayName(m_scenario->entities[idx]));
    copy.name = newLabel;
    if (!copy.marking.empty()) copy.marking = newLabel.substr(0, 11);  // DIS marking = 11 chars
    m_scenario->entities.push_back(std::move(copy));

    // Refresh the preview's own caches, then reload the other tabs + mark dirty.
    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::RenameEntity(size_t idx)
{
    if (!m_scenario) return;
    if (idx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[idx];

    // Prompt for a new label (seeded with the current one); write it to both
    // name and marking so every view stays in sync.
    const CString initial(EntityDisplayName(e).c_str());
    CPromptDialog dlg(_T("Rename Entity"), _T("Name:"), initial, this);
    if (dlg.DoModal() != IDOK) return;

    CString v = dlg.Value();
    v.Trim();
    if (v.IsEmpty()) return;

    const CStringA narrow(v);
    const std::string newName(static_cast<const char*>(narrow));
    e.name = newName;
    if (!e.marking.empty()) e.marking = newName.substr(0, 11);  // DIS marking = 11 chars

    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

int CPreviewPage::DeckCrew(size_t idx) const
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return 0;
    return m_scenario->entities[idx].deckCrew;
}

void CPreviewPage::SetDeckCrew(size_t idx)
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[idx];

    // How many of the crew authored for this ship type in the DISBrowser hanger
    // the visualizer puts on this entity (extra ones get random deck spots).
    constexpr int kMaxDeckCrew = 50;
    CString initial;
    initial.Format(_T("%d"), e.deckCrew);
    CPromptDialog dlg(_T("Deck Crew"), _T("Crew on deck (0-50):"), initial, this);
    if (dlg.DoModal() != IDOK) return;

    CString v = dlg.Value();
    v.Trim();
    if (v.IsEmpty() || v.SpanIncluding(_T("0123456789")) != v)
    {
        AfxMessageBox(_T("Enter a whole number from 0 to 50."), MB_ICONWARNING);
        return;
    }
    const int n = _ttoi(v);
    e.deckCrew = std::min(std::max(n, 0), kMaxDeckCrew);

    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::ChangeEntity(size_t idx)
{
    if (!m_scenario) return;
    if (idx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[idx];

    // Read-only catalog tree picker, pre-selected to the entity's current type.
    CEntityTypePickerDialog dlg(&theApp.Catalog(),
                                e.kind, e.domain, e.category, e.subcategory, this);
    if (dlg.DoModal() != IDOK || !dlg.HasSelection()) return;

    uint8_t k = 0, d = 0, c = 0, sub = 0;
    dlg.GetSelection(k, d, c, sub);

    // Re-type (leave country/specific/extra alone).
    e.kind = k; e.domain = d; e.category = c; e.subcategory = sub;

    // Update the on-screen label to the new type's catalog name.
    for (const CatalogEntry& ce : theApp.Catalog().Subcategories(k, d, c))
        if (ce.id == sub)
        {
            e.name = ce.name;
            if (!e.marking.empty()) e.marking = ce.name.substr(0, 11);  // DIS marking = 11 chars
            break;
        }

    // Re-derive speed/attributes from the new type's cruise speed. Silent if the
    // type has no catalog cruise (unlike Refresh Speed, which nags) — the type
    // change itself already succeeded. Validator envelope + dest-time recompute
    // from the type on demand.
    const AirframeProfile prof = theApp.Catalog().Profile(k, d, c, sub);
    if (prof.valid && prof.cruiseSpeedMps > 0.0)
    {
        m_refreshFailed.erase(idx);
        RefreshPathSpeed(e, prof.cruiseSpeedMps, m_scenario);
        e.initialSpeedMps = prof.cruiseSpeedMps;
    }

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::OnNewEntity()
{
    if (!m_scenario) return;

    Entity fresh;
    // Default to the octopus interceptor drone (Platform / Air / UAV).
    fresh.kind = 1; fresh.domain = 2; fresh.category = 50; fresh.subcategory = 98;
    fresh.country = 225;

    // Unique EntityID = max existing + 1 (same rule as the Assets page).
    uint16_t next = 1;
    for (const Entity& e : m_scenario->entities)
        if (e.entityId >= next) next = static_cast<uint16_t>(e.entityId + 1);
    fresh.entityId = next;

    // Label from the catalog type name (fallback if the type isn't catalogued).
    std::string typeName = "Octopus Drone";
    for (const CatalogEntry& ce : theApp.Catalog().Subcategories(1, 2, 50))
        if (ce.id == 98) { typeName = ce.name; break; }
    fresh.name    = typeName;
    fresh.marking = typeName.substr(0, 11);   // DIS marking = 11 chars

    // Place in the upper-right REGION of the current preview view (not jammed
    // into the pixel corner). A brand-new entity faces north (heading 0), so
    // "Plot > Line" draws its 2 km starter segment straight UP; if the entity
    // sat right at the top edge that line — and its draggable end-anchor — would
    // run off-screen and look like nothing happened. Insetting ~20-25% keeps the
    // entity and a northward starter line comfortably visible. Fall back to an
    // up-and-right offset from the view center if the canvas isn't ready yet.
    double east = m_centerEnuE + 500.0;
    double north = m_centerEnuN + 500.0;
    if (m_canvas.GetSafeHwnd())
    {
        CRect rc; m_canvas.GetClientRect(&rc);
        if (rc.Width() > 0 && rc.Height() > 0)
        {
            const int px = rc.left + (rc.Width()  * 4) / 5;   // ~80% across (right side)
            const int py = rc.top  + (rc.Height() * 1) / 4;   // ~25% down  (upper area)
            double e = 0.0, n = 0.0;
            if (m_canvas.UnprojectToEnu(CPoint(px, py), e, n))
            {
                east = e; north = n;
            }
        }
    }
    // Fill all coordinate representations consistently from ENU (east, north, up).
    SetEntityInitialEnu(fresh, east, north, 0.0);

    m_scenario->entities.push_back(std::move(fresh));

    // NOTE: do NOT group-select the new entity. A group-selected entity's
    // right-click menu is the GROUP menu (Set Duration / Refresh Speed only) —
    // the per-entity Plot / Rename / Change Entity items are suppressed. Leaving
    // the freshly-added drone group-selected made "Plot > Line" unavailable.
    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

// ============================ Camera system ============================

//
// GetCameras / MutableCameras — the scenario's camera schedule (or nullptr when
//   no scenario is loaded). The timeline reads via the const form and edits
//   frame boundaries via the mutable form.
//
const std::vector<CameraFrame>* CPreviewPage::GetCameras() const
{
    return m_scenario ? &m_scenario->cameras : nullptr;
}
std::vector<CameraFrame>* CPreviewPage::MutableCameras()
{
    return m_scenario ? &m_scenario->cameras : nullptr;
}

//
// NormalizeCameraSchedule — keep every frame a valid, in-range box: clamp
//   begin/end into [0, EffectivePreviewDuration()], enforce a minimum width, sort
//   by start time, and push any overlaps apart so boxes never overlap. Frames may
//   still leave gaps (the operator arranges them freely); we only guarantee they
//   stay ordered and non-overlapping.
//
void CPreviewPage::NormalizeCameraSchedule()
{
    if (!m_scenario) return;
    auto& cams = m_scenario->cameras;
    if (cams.empty()) return;

    const double D = EffectivePreviewDuration();
    constexpr double kMinW = 1.0;   // a box can't be narrower than this
    for (CameraFrame& c : cams)
    {
        if (c.beginSecond < 0.0) c.beginSecond = 0.0;
        if (c.beginSecond > D)   c.beginSecond = D;
        if (c.endSecond   > D)   c.endSecond   = D;
        if (c.endSecond < c.beginSecond + kMinW)
            c.endSecond = c.beginSecond + kMinW;
        if (c.endSecond > D)     // min width would spill past the end -> pull left
        {
            c.endSecond   = D;
            c.beginSecond = D - kMinW;
            if (c.beginSecond < 0.0) c.beginSecond = 0.0;
        }
    }

    // Stable so an equal-start tie keeps vector order — the timeline places a
    // reordered (dragged-to-front) frame ahead of its tie group and relies on
    // that order surviving here.
    std::stable_sort(cams.begin(), cams.end(),
              [](const CameraFrame& a, const CameraFrame& b)
              { return a.beginSecond < b.beginSecond; });

    // De-overlap: walk left-to-right and shove any box that starts before its
    // predecessor ends to begin exactly at that end (keeping its width). Clamp to
    // the duration so a cascade can't spill past the timeline. This is the model
    // guarantee that camera boxes are never permitted to overlap.
    for (size_t i = 1; i < cams.size(); ++i)
    {
        const double prevEnd = cams[i - 1].endSecond;
        if (cams[i].beginSecond < prevEnd)
        {
            const double w = cams[i].endSecond - cams[i].beginSecond;
            cams[i].beginSecond = prevEnd;
            cams[i].endSecond   = prevEnd + w;
            if (cams[i].endSecond > D)
            {
                cams[i].endSecond   = D;
                cams[i].beginSecond = std::max(0.0, D - w);
            }
        }
    }
}

//
// OnCamScaleChange — the time-scale dropdown changed: apply the new seconds-per-
//   reference-distance to the timeline (boxes grow/shrink), resync the scroll bar
//   to the new content width, and repaint the strip.
//
void CPreviewPage::OnCamScaleChange()
{
    CComboBox* sc = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_SCALE);
    if (!sc) return;
    int sel = sc->GetCurSel();
    if (sel < 0 || sel >= (int)_countof(kCamScales)) sel = kCamScaleDefault;
    m_timeline.SetTimeScale(kCamScales[sel].seconds);
    m_timeline.SetScrollOffset(0);           // re-home the view on a zoom change
    UpdateCameraScrollBar();
    PersistPreviewView();
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
}

//
// UpdateCameraScrollBar — size the camera-strip horizontal scroll bar to the
//   schedule's full pixel width at the current scale. The page (thumb) is the
//   visible strip width; the bar is disabled when everything already fits.
//
void CPreviewPage::UpdateCameraScrollBar()
{
    CScrollBar* sb = (CScrollBar*)GetDlgItem(IDC_CAMERA_HSCROLL);
    if (!sb || !m_timeline.GetSafeHwnd()) return;

    CRect rc; m_timeline.GetClientRect(&rc);
    const int clientW  = std::max<int>(rc.Width(), 1);
    const int contentW = std::max(m_timeline.ContentWidthPx(), clientW);

    int pos = m_timeline.GetScrollOffset();
    const int maxPos = std::max(0, contentW - clientW);
    if (pos > maxPos) pos = maxPos;
    m_timeline.SetScrollOffset(pos);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin   = 0;
    si.nMax   = contentW - 1;      // range is 0..contentW-1
    si.nPage  = clientW;           // thumb covers the visible width
    si.nPos   = pos;
    sb->SetScrollInfo(&si, TRUE);
    sb->EnableWindow(contentW > clientW);
}

//
// AddCameraFrame — insert `f` as a free-floating box of a sensible default width,
//   dropped into a VISIBLE, non-overlapping gap so it always appears where the
//   operator is looking. Placing it in a real gap matters: NormalizeCameraSchedule
//   only shoves boxes that overlap their predecessor, so a gap-placed box stays
//   put instead of being cascaded off-screen (the New-Cam-shows-nothing regression,
//   which surfaced once the schedule sort became stable). The box remains fully
//   movable and resizable afterward.
//
double CPreviewPage::DefaultCameraWidthSec() const
{
    const double D   = EffectivePreviewDuration();
    const double vis = m_timeline.GetSafeHwnd() ? m_timeline.VisibleSpanSeconds() : 0.0;
    double width = (vis > 0.0 ? vis : D) * 0.1;
    if (width > D)    width = D;
    if (width <= 0.0) width = 1.0;
    return width;
}

void CPreviewPage::AddCameraFrame(CameraFrame&& f)
{
    if (!m_scenario) return;
    auto& cams = m_scenario->cameras;
    const double D = EffectivePreviewDuration();

    // Timing the dialog asked for, if any. Consumed here, once, so a later
    // programmatic add cannot inherit it.
    const double askedBegin = m_newCameraBeginSec;
    const double askedWidth = m_newCameraWidthSec;
    m_newCameraBeginSec = -1.0;
    m_newCameraWidthSec = -1.0;

    // Cache the target's catalogued size here rather than at each of the three
    // creation sites, so no path can produce a frame with an unresolved target.
    CameraTargetExtents::Apply(*m_scenario, f);

    // Width: what the operator typed, else a tenth of the time currently visible
    // in the strip so a fresh box is a readable slice at any zoom.
    double width = (askedWidth > 0.0) ? askedWidth : DefaultCameraWidthSec();
    if (width > D)    width = D;
    if (width <= 0.0) width = 1.0;
    const double vis = m_timeline.VisibleSpanSeconds();

    // The window currently on screen: [visLo, visHi]. When the strip isn't realized
    // yet, treat the whole [0, D] as visible.
    double visLo = 0.0, visHi = D;
    if (vis > 0.0)
    {
        const double pps = m_timeline.PxPerSecond();
        visLo = (pps > 0.0) ? m_timeline.GetScrollOffset() / pps : 0.0;
        visHi = visLo + vis;
    }
    if (visLo < 0.0) visLo = 0.0;
    if (visHi > D)   visHi = D;
    if (visHi <= visLo) visHi = D;              // degenerate — fall back to full range

    // Preferred anchor: the playhead when it's on screen (drop the box where the
    // operator is looking), otherwise the left edge of the visible window.
    double anchor = m_previewTimeSec;
    if (anchor < visLo || anchor > visHi) anchor = visLo;

    // Existing occupied intervals, begin-sorted (cams are already normalized).
    std::vector<std::pair<double, double>> busy;
    busy.reserve(cams.size());
    for (const CameraFrame& c : cams) busy.emplace_back(c.beginSecond, c.endSecond);
    std::sort(busy.begin(), busy.end());

    // Largest width that can fit in the visible window at all (shrink for a tight zoom).
    if (width > visHi - visLo) width = std::max(1.0, visHi - visLo);

    // Slide a candidate slot rightward from the anchor, hopping over any box it
    // overlaps, until it fits with no overlap inside [visLo, visHi].
    auto firstFreeFrom = [&](double from) -> double
    {
        double b = std::max(from, visLo);
        bool moved = true;
        while (moved)
        {
            moved = false;
            if (b + width > visHi) return -1.0;      // ran past the visible window
            for (const auto& iv : busy)
                if (b < iv.second && iv.first < b + width)   // overlaps this box
                {
                    b = iv.second;                    // hop to its end and retry
                    moved = true;
                    break;
                }
        }
        return b;
    };

    double begin = firstFreeFrom(anchor);
    if (begin < 0.0) begin = firstFreeFrom(visLo);   // retry scanning the whole window
    if (begin < 0.0)                                 // window fully packed — accept a
        begin = std::min(anchor, std::max(0.0, D - width));  // clamped anchor; Normalize tidies
    if (begin < 0.0) begin = 0.0;

    // A typed start wins over the free-slot search; Normalize below still keeps
    // it inside the play and pushes any overlap apart.
    if (askedBegin >= 0.0) begin = std::min(askedBegin, std::max(0.0, D - width));

    f.beginSecond = begin;
    f.endSecond   = std::min(D, begin + width);
    cams.push_back(std::move(f));
    NormalizeCameraSchedule();   // clamp + sort + de-overlap (leaves gap-placed boxes put)
}

//
// PopulateCameraCombos — (re)fill the Entity, Camera-preset, Target and
//   Transition dropdowns. Entity/Target item-data hold the DIS EntityID; the
//   preset combo's item-data is the index into theApp.CameraPresets().
//
void CPreviewPage::PopulateCameraCombos()
{
    if (!m_scenario) return;

    auto nameOf = [](const Entity& e) -> CString {
        const std::string& s = e.marking.empty() ? e.name : e.marking;
        CString nm(CA2W(s.c_str()));
        CString out; out.Format(_T("%s [%u]"), (LPCTSTR)nm, static_cast<unsigned>(e.entityId));
        return out;
    };

    if (CComboBox* ent = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_ENTITY))
    {
        ent->ResetContent();
        for (const Entity& e : m_scenario->entities)
        {
            const int ix = ent->AddString(nameOf(e));
            ent->SetItemData(ix, e.entityId);
        }
        if (ent->GetCount() > 0) ent->SetCurSel(0);
    }
    if (CComboBox* tgt = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_TARGET))
    {
        tgt->ResetContent();
        const int z = tgt->AddString(_T("(focus on source)"));
        tgt->SetItemData(z, 0);
        for (const Entity& e : m_scenario->entities)
        {
            const int ix = tgt->AddString(nameOf(e));
            tgt->SetItemData(ix, e.entityId);
        }
        tgt->SetCurSel(0);
    }
    RefreshCameraPresetCombo();   // depends on the entity combo filled just above
    if (CComboBox* trn = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_TRANSITION))
    {
        if (trn->GetCount() == 0)   // fixed two-item list; seed once
        {
            trn->AddString(_T("Crossfade / Blend"));   // index 0
            trn->AddString(_T("Hard cut"));            // index 1
        }
        if (trn->GetCurSel() < 0) trn->SetCurSel(0);
    }
}

//-----------------------------------------------------------------------------
// SelectedCameraEntityTypeKey — Cameras.ini <Type> for the selected entity
//   Resolved through DISBrowser's Hanger.ini, which is the same source
//   FDISConfigLoader::ResolveCameraTypeKey uses at runtime. Returns "" when the
//   map could not be loaded (no DISBrowser folder set yet) or the entity's tuple
//   has no hanger entry — the caller treats both as "don't filter".
//
std::string CPreviewPage::SelectedCameraEntityTypeKey() const
{
    if (!m_scenario) return std::string();
    const CComboBox* ent = (const CComboBox*)GetDlgItem(IDC_COMBO_CAM_ENTITY);
    if (!ent) return std::string();
    const int ei = ent->GetCurSel();
    if (ei < 0) return std::string();

    return CameraPresetCombo::TypeKeyForEntityId(
        *m_scenario, static_cast<uint16_t>(ent->GetItemData(ei)));
}

//-----------------------------------------------------------------------------
// RefreshCameraPresetCombo — refill the preset dropdown for the selected entity
//   The listing rules (own type first and preselected, other types below a
//   separator, an explanatory notice when the entity has none) live in
//   CameraPresetCombo::Populate, shared with the New Camera dialog so the two
//   cannot drift apart.
//
void CPreviewPage::RefreshCameraPresetCombo()
{
    CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_PRESET);
    if (!pre) return;

    // Land on a correct preset when one exists; otherwise on the notice, so Add
    // refuses and explains instead of quietly mounting whatever sorted first.
    const int firstMatch = CameraPresetCombo::Populate(*pre, SelectedCameraEntityTypeKey());
    pre->SetCurSel(firstMatch >= 0 ? firstMatch : 0);
}

//-----------------------------------------------------------------------------
// OnCamEntityChanged — the camera Entity dropdown changed selection
//
void CPreviewPage::OnCamEntityChanged()
{
    RefreshCameraPresetCombo();
}

//
// NextStationaryCameraLabel — the lowest unused "Camera N" name. Source-less
//   cameras have no entity to name them after, so they are numbered instead.
//
std::string CPreviewPage::NextStationaryCameraLabel() const
{
    if (!m_scenario) return "Camera 1";
    const auto& cams = m_scenario->cameras;
    auto taken = [&cams](const std::string& s)
    {
        for (const CameraFrame& c : cams)
            if (c.label == s) return true;
        return false;
    };
    for (int n = 1; ; ++n)
    {
        std::string label = "Camera " + std::to_string(n);
        if (!taken(label)) return label;
    }
}

//
// CreateCameraFromDialog — turn an accepted New Camera dialog into a frame.
//
//   Both creation paths land here; they differ only in which source the dialog
//   opened on. A source of 0 means the operator chose "(none)": build the
//   entity-less Stationary camera the "New Cam..." button used to create
//   directly, at a vantage in the upper-left of the current view so it lands
//   somewhere visible and draggable rather than at the scenario origin.
//
void CPreviewPage::CreateCameraFromDialog(const CEntityCameraDialog& dlg)
{
    if (!m_scenario) return;

    const uint16_t srcId = dlg.SourceEntityId();

    // Timing from the dialog rides along to AddCameraFrame through these two
    // members (the mounted path goes through AddEntityCameraFrame's long
    // signature, which is not worth widening for it).
    m_newCameraBeginSec = dlg.BeginSecond();
    m_newCameraWidthSec = dlg.DurationSec();

    if (srcId != 0)
    {
        // Mounted on an entity: reuse the existing Entity-camera path, which also
        // builds the "<entity> / <type> / <angle>" label.
        for (size_t i = 0; i < m_scenario->entities.size(); ++i)
        {
            if (m_scenario->entities[i].entityId != srcId) continue;
            AddEntityCameraFrame(i, dlg.PresetIndex(),
                                 dlg.TargetEntityId(), dlg.Transition(),
                                 dlg.ZoomEnabled(), dlg.ZoomFovDeg(),
                                 dlg.DynamicZoom(), dlg.ZoomFillPct(),
                                 dlg.LimitsEnabled(),
                                 dlg.TransitionSeconds(), dlg.Label());
            // After PresetIndex() has been consumed: writing the envelope reloads
            // the catalog, which invalidates that index.
            ApplyEnvelopeFromDialog(dlg);
            return;
        }
        m_newCameraBeginSec = m_newCameraWidthSec = -1.0;
        return;   // source vanished between opening the dialog and OK
    }

    CameraFrame f;
    f.kind = CameraKind::Stationary;

    // Upper-left region of the current view (fallback: up-and-left of center).
    // RebuildRenderState draws a Stationary frame at vantEast/North, so leaving
    // these at zero would park the new camera on the scenario origin instead.
    double east = m_centerEnuE - 500.0, north = m_centerEnuN + 500.0;
    if (m_canvas.GetSafeHwnd())
    {
        CRect rc; m_canvas.GetClientRect(&rc);
        if (rc.Width() > 0 && rc.Height() > 0)
        {
            const int px = rc.left + rc.Width()  / 10;   // ~10% across
            const int py = rc.top  + rc.Height() / 10;   // ~10% down
            double e = 0.0, n = 0.0;
            if (m_canvas.UnprojectToEnu(CPoint(px, py), e, n)) { east = e; north = n; }
        }
    }
    f.vantEastM = east; f.vantNorthM = north; f.vantUpM = 0.0;

    // A source-less camera carries no preset or envelope, but it can still track a
    // target, cut with a transition, and zoom.
    f.targetEntityId    = dlg.TargetEntityId();
    f.transition        = dlg.Transition();
    f.transitionSeconds = dlg.TransitionSeconds();
    f.zoomEnabled       = dlg.ZoomEnabled();
    f.zoomFovDeg        = dlg.ZoomFovDeg();
    f.dynamicZoom       = dlg.DynamicZoom();
    f.zoomFillPct       = dlg.ZoomFillPct();

    // The operator can now place a stationary camera numerically instead of
    // dragging it — including its height, which the canvas cannot express at all.
    if (dlg.VantEastM() != 0.0 || dlg.VantNorthM() != 0.0 || dlg.VantUpM() != 0.0)
    {
        f.vantEastM  = dlg.VantEastM();
        f.vantNorthM = dlg.VantNorthM();
        f.vantUpM    = dlg.VantUpM();
    }

    // Unique "Camera N": pick the lowest N whose label isn't already in use.
    // Deriving it from cameras.size()+1 collides after a delete-then-add (e.g.
    // deleting one of six leaves size 5, and "Camera 6" is regenerated as a
    // duplicate), which is exactly the two-"Camera 6" case the operator hit.
    f.label = dlg.Label().empty() ? NextStationaryCameraLabel() : dlg.Label();

    AddCameraFrame(std::move(f));

    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// OnNewCamera — "New Cam..." button: the same modal the entity-dot right-click
//   uses, opened with no source preselected so OK yields a Stationary camera.
//   Routing both through one dialog is why the button gained an ellipsis.
//
void CPreviewPage::OnNewCamera()
{
    if (!m_scenario) return;

    CEntityCameraDialog dlg(this);
    dlg.SetContext(m_scenario, 0);      // 0 = "(none)"
    dlg.SetDefaultDuration(DefaultCameraWidthSec());
    if (dlg.DoModal() != IDOK) return;

    CreateCameraFromDialog(dlg);
}

//
// OnAddCamera — "Add" button: build an Entity camera from the dropdown row
//   (entity + Cameras.ini preset + target + transition) and register a timeline
//   frame for it.
//
void CPreviewPage::OnAddCamera()
{
    if (!m_scenario) return;

    CComboBox* ent = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_ENTITY);
    CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_PRESET);
    CComboBox* tgt = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_TARGET);
    CComboBox* trn = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_TRANSITION);
    if (!ent || !pre) return;

    const int ei = ent->GetCurSel();
    if (ei < 0) { AfxMessageBox(_T("Select an entity for the camera.")); return; }

    const auto& presets = theApp.CameraPresets().Presets();
    const int pi = pre->GetCurSel();
    if (pi < 0 || presets.empty())
    {
        AfxMessageBox(_T("No camera presets are available (config\\Cameras.ini)."));
        return;
    }
    const DWORD_PTR pdata = pre->GetItemData(pi);
    if (pdata == CameraPresetCombo::kRowNotSelectable)
    {
        // The separator, or the notice shown when this entity's type has no views.
        AfxMessageBox(_T("That row isn't a camera preset.\n\n")
                      _T("Pick one of this entity's own views, or a preset under ")
                      _T("\"other types\" if you deliberately want another airframe's ")
                      _T("calibration. If this entity has no views yet, author them in ")
                      _T("DISBrowser's hanger first."));
        return;
    }
    const size_t pidx = static_cast<size_t>(pdata);
    if (pidx >= presets.size()) return;

    CameraFrame f;
    f.kind           = CameraKind::Entity;
    f.sourceEntityId = static_cast<uint16_t>(ent->GetItemData(ei));
    f.presetType     = presets[pidx].type;
    f.presetAngle    = presets[pidx].angle;
    f.targetEntityId = (tgt && tgt->GetCurSel() >= 0)
                       ? static_cast<uint16_t>(tgt->GetItemData(tgt->GetCurSel())) : 0;
    f.transition     = (trn && trn->GetCurSel() == 1)
                       ? CameraTransition::HardCut : CameraTransition::CrossfadeBlend;
    // Match the dialog path: a camera with an authored envelope enforces it by default.
    f.limitsEnabled  = presets[pidx].LimitsAvailable();

    std::string ename;
    for (const Entity& e : m_scenario->entities)
        if (e.entityId == f.sourceEntityId) { ename = e.marking.empty() ? e.name : e.marking; break; }
    f.label = ename + " / " + presets[pidx].DisplayLabel();

    AddCameraFrame(std::move(f));

    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// CameraPresetCount / CameraPresetLabel — expose the loaded Cameras.ini presets
//   to the canvas so it can build the entity-dot "New Camera" submenu without
//   reaching into theApp directly.
//
size_t CPreviewPage::CameraPresetCount() const
{
    return theApp.CameraPresets().Presets().size();
}

CString CPreviewPage::CameraPresetLabel(size_t i) const
{
    const auto& presets = theApp.CameraPresets().Presets();
    if (i >= presets.size()) return CString();
    return CString(CA2W(presets[i].DisplayLabel().c_str()));
}

//
// AddEntityCameraFrame — mount an Entity camera on entity `entityIdx` using
//   Cameras.ini preset `presetIndex`, tracking `targetEntityId` (0 = focus on
//   source) with the given `transition` and zoom, then add it as a timeline frame.
//
void CPreviewPage::AddEntityCameraFrame(size_t entityIdx, size_t presetIndex,
                                        uint16_t targetEntityId,
                                        CameraTransition transition,
                                        bool zoomEnabled, double zoomFovDeg,
                                        bool dynamicZoom, double zoomFillPct,
                                        bool limitsEnabled,
                                        double transitionSeconds,
                                        const std::string& label)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;
    const auto& presets = theApp.CameraPresets().Presets();
    if (presetIndex >= presets.size()) return;

    const Entity& src = m_scenario->entities[entityIdx];

    CameraFrame f;
    f.kind           = CameraKind::Entity;
    f.sourceEntityId = src.entityId;
    f.presetType     = presets[presetIndex].type;
    f.presetAngle    = presets[presetIndex].angle;
    f.targetEntityId = targetEntityId;
    f.transition     = transition;
    f.zoomEnabled    = zoomEnabled;
    f.zoomFovDeg     = zoomFovDeg;
    f.dynamicZoom    = dynamicZoom;
    f.zoomFillPct    = zoomFillPct;
    f.limitsEnabled  = limitsEnabled;
    f.transitionSeconds = transitionSeconds;

    // An operator-typed name wins; otherwise the camera is named after its mount.
    f.label = label.empty()
            ? AutoEntityCameraLabel(src.entityId, f.presetType, f.presetAngle)
            : label;

    AddCameraFrame(std::move(f));

    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// NewEntityCameraViaDialog — entity-dot "New Camera..." path: the same modal the
//   "New Cam..." button opens, but with the clicked entity preselected as the
//   source. The operator can still switch it — including to "(none)", which
//   yields a Stationary camera not associated with any entity.
//
//   No up-front "there are no presets" refusal: a source-less camera needs none,
//   and an entity source already gets an explanatory notice inside the dialog.
//
void CPreviewPage::NewEntityCameraViaDialog(size_t entityIdx)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;

    CEntityCameraDialog dlg(this);
    dlg.SetContext(m_scenario, m_scenario->entities[entityIdx].entityId);
    dlg.SetDefaultDuration(DefaultCameraWidthSec());
    if (dlg.DoModal() != IDOK) return;

    CreateCameraFromDialog(dlg);
}

//
// AutoEntityCameraLabel — the name an Entity camera gets when nobody has renamed
//   it: "<entity> / <type> / <angle>". Shared by the create and edit paths so the
//   "has this been customised?" test in UpdateCameraFromDialog compares against
//   exactly what the creator would have produced.
//
std::string CPreviewPage::AutoEntityCameraLabel(uint16_t srcEntityId,
                                                const std::string& presetType,
                                                const std::string& presetAngle) const
{
    std::string ename;
    if (m_scenario)
    {
        for (const Entity& e : m_scenario->entities)
        {
            if (e.entityId != srcEntityId) continue;
            ename = e.marking.empty() ? e.name : e.marking;
            break;
        }
    }
    return ename + " / " + presetType + " / " + presetAngle;
}

//
// ApplyEnvelopeFromDialog — write the dialog's gimbal envelope to both copies of
//   Cameras.ini and re-read the catalog.
//
//   Called only when the operator actually touched the envelope (the dialog has
//   already confirmed the shared scope by then). MUST run after the caller has
//   consumed dlg.PresetIndex(): the reload invalidates every index into
//   CameraPresets(), which is why the frame is built from PresetType()/PresetAngle()
//   instead.
//
void CPreviewPage::ApplyEnvelopeFromDialog(const CEntityCameraDialog& dlg)
{
    if (!dlg.EnvelopeChanged()) return;
    if (dlg.PresetType().empty() || dlg.PresetAngle().empty()) return;  // no mount

    CameraPresetIO::GimbalEnvelope env;
    env.defined  = dlg.EnvelopeDefined();
    env.enabled  = dlg.EnvelopeEnabled();
    env.minPitch = dlg.MinPitch(); env.maxPitch = dlg.MaxPitch();
    env.minYaw   = dlg.MinYaw();   env.maxYaw   = dlg.MaxYaw();
    env.minRoll  = dlg.MinRoll();  env.maxRoll  = dlg.MaxRoll();

    std::wstring warning, error;
    const bool ok = CameraPresetIO::WriteEnvelopeBothCopies(
        theApp.CameraPresetsPath(), theApp.DisBrowserProjectDir(),
        dlg.PresetType(), dlg.PresetAngle(), env, warning, error);

    if (!ok)
    {
        AfxMessageBox((L"The gimbal envelope could not be saved.\n\n" + error).c_str(),
                      MB_ICONERROR);
        return;   // nothing was written, so nothing to reload
    }

    // The catalog is load-once, so without this the dialog would keep offering the
    // pre-write values for the rest of the session.
    theApp.ReloadCameraPresets();

    if (!warning.empty()) AfxMessageBox(warning.c_str(), MB_ICONWARNING);
}

//
// EditCameraViaDialog — "Edit Camera..." on a camera box (timeline or canvas).
//
//   Snapshots the frame BY VALUE before opening the modal. A reference into
//   Scenario::cameras cannot safely outlive a modal loop, and the index itself is
//   only a hint by the time the dialog closes — see UpdateCameraFromDialog.
//
void CPreviewPage::EditCameraViaDialog(size_t frameIdx)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;

    const CameraFrame before = m_scenario->cameras[frameIdx];

    CEntityCameraDialog dlg(this);
    dlg.SetEditContext(m_scenario, before);
    if (dlg.DoModal() != IDOK) return;

    UpdateCameraFromDialog(frameIdx, dlg, before);
}

//
// UpdateCameraFromDialog — apply an accepted Edit Camera dialog to an existing
//   frame. The mirror of CreateCameraFromDialog, with three differences that
//   matter:
//
//   1. The frame is re-located before it is written. NormalizeCameraSchedule and
//      CCameraTimeline::ReindexDraggedFrame both reorder Scenario::cameras, so an
//      index captured before a modal is a hint, not an address.
//   2. beginSecond/endSecond change only when the operator retyped Start or
//      Duration, and only then is the schedule re-normalised: editing any other
//      setting must not move the box.
//   3. The label is only regenerated when it was never customised (see below).
//
void CPreviewPage::UpdateCameraFromDialog(size_t frameIdx,
                                          const CEntityCameraDialog& dlg,
                                          const CameraFrame& before)
{
    if (!m_scenario) return;
    auto& cams = m_scenario->cameras;

    // Re-find the frame if the vector moved under us. Timing plus label identifies
    // it well enough, and giving up beats writing the operator's edit onto some
    // other camera.
    if (frameIdx >= cams.size() ||
        cams[frameIdx].beginSecond != before.beginSecond ||
        cams[frameIdx].endSecond   != before.endSecond   ||
        cams[frameIdx].label       != before.label)
    {
        size_t found = cams.size();
        for (size_t i = 0; i < cams.size(); ++i)
        {
            if (cams[i].beginSecond == before.beginSecond &&
                cams[i].endSecond   == before.endSecond   &&
                cams[i].label       == before.label)
            { found = i; break; }
        }
        if (found >= cams.size()) return;   // deleted or unrecognisable; drop the edit
        frameIdx = found;
    }

    CameraFrame& f = cams[frameIdx];

    const uint16_t srcId = dlg.SourceEntityId();
    const bool sourceChanged = (srcId != before.sourceEntityId) ||
                               ((srcId != 0) != (before.kind == CameraKind::Entity));
    const bool presetChanged = (dlg.PresetType()  != before.presetType) ||
                               (dlg.PresetAngle() != before.presetAngle);

    if (srcId != 0)
    {
        // Mounted. Note the kind can flip either way here: this dialog is the only
        // place a Stationary camera can be given a source, or a mounted one have it
        // taken away.
        f.kind           = CameraKind::Entity;
        f.sourceEntityId = srcId;
        f.presetType     = dlg.PresetType();
        f.presetAngle    = dlg.PresetAngle();
        f.vantEastM = f.vantNorthM = f.vantUpM = 0.0;   // a mount has no fixed vantage
        f.limitsEnabled  = dlg.LimitsEnabled();
    }
    else
    {
        // Source-less: a point in space, with no preset and nothing to be
        // body-relative to. Mirrors the Entity-only LimitsEnabled write in ScenarioIO.
        f.kind           = CameraKind::Stationary;
        f.sourceEntityId = 0;
        f.presetType.clear();
        f.presetAngle.clear();
        f.vantEastM      = dlg.VantEastM();
        f.vantNorthM     = dlg.VantNorthM();
        f.vantUpM        = dlg.VantUpM();
        f.limitsEnabled  = false;
    }

    f.targetEntityId    = dlg.TargetEntityId();
    f.transition        = dlg.Transition();
    f.transitionSeconds = dlg.TransitionSeconds();
    f.zoomEnabled       = dlg.ZoomEnabled();
    f.zoomFovDeg        = dlg.ZoomFovDeg();
    f.dynamicZoom       = dlg.DynamicZoom();
    f.zoomFillPct       = dlg.ZoomFillPct();

    // Label: the name is the operator's once they have typed one, so it is never
    // blindly regenerated. It only follows the mount when it still reads exactly as
    // the creator would have written it AND the mount actually changed.
    {
        const std::string typed = dlg.Label();
        const std::string autoBefore =
            (before.kind == CameraKind::Entity)
                ? AutoEntityCameraLabel(before.sourceEntityId,
                                        before.presetType, before.presetAngle)
                : std::string();

        if (!typed.empty() && typed != autoBefore)
            f.label = typed;                      // named by hand — leave it alone
        else if (f.kind == CameraKind::Entity && (sourceChanged || presetChanged))
            f.label = AutoEntityCameraLabel(f.sourceEntityId, f.presetType, f.presetAngle);
        else if (typed.empty())
            f.label = before.label;               // never blank a camera's name
        else
            f.label = typed;
    }

    // The tracked entity may have changed, and dynamic zoom frames against the
    // TARGET's catalogued dimensions. Without this the shot would keep fitting the
    // previous entity's size.
    CameraTargetExtents::Apply(*m_scenario, f);

    // Cameras.ini last: the reload it triggers invalidates dlg.PresetIndex(), which
    // is why everything above reads PresetType()/PresetAngle() instead.
    ApplyEnvelopeFromDialog(dlg);

    // Timing: only when retyped. Then the schedule is re-normalised, because a
    // longer box can now overlap its neighbour and a moved one can change the
    // running order; otherwise the box stays exactly where it was. The box in the
    // strip is drawn from begin/end, so this is what resizes it.
    if (dlg.TimingChanged())
    {
        const double D = EffectivePreviewDuration();
        double begin = std::max(0.0, dlg.BeginSecond());
        double dur   = std::max(kCameraMinDurationSec, dlg.DurationSec());
        if (begin + dur > D) begin = std::max(0.0, D - dur);
        f.beginSecond = begin;
        f.endSecond   = std::min(D, begin + dur);
        NormalizeCameraSchedule();
        UpdateCameraScrollBar();
    }
    // (WM_APP_REFRESH_UI below is what marks the scenario dirty, the same as every
    // other camera mutator on this page.)
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// DragCameraTo — live-move a Stationary camera's vantage during a canvas drag.
//
void CPreviewPage::DragCameraTo(size_t frameIdx, double enuE, double enuN)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    CameraFrame& c = m_scenario->cameras[frameIdx];
    if (c.kind != CameraKind::Stationary) return;
    c.vantEastM = enuE;
    c.vantNorthM = enuN;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// EndCameraDrag — commit a finished camera-box drag (reload tabs + mark dirty).
//
void CPreviewPage::EndCameraDrag()
{
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// RefreshCameraTargetExtents — re-resolve every frame's cached target dimensions.
//   Cheap, and it catches entity TYPES retyped on the Asset/Entity Editor tab
//   after the camera was authored (the frame stores a size, not a type).
//
void CPreviewPage::RefreshCameraTargetExtents()
{
    if (!m_scenario) return;
    for (CameraFrame& f : m_scenario->cameras)
        CameraTargetExtents::Apply(*m_scenario, f);
}

//
// DeleteCamera — remove frame `frameIdx` and re-tile the remaining schedule.
//
void CPreviewPage::DeleteCamera(size_t frameIdx)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    m_scenario->cameras.erase(m_scenario->cameras.begin() + frameIdx);
    NormalizeCameraSchedule();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// EntityCameraCount — how many Entity cameras are mounted on the entity at
//   `entityIdx` (frames whose sourceEntityId matches the entity's DIS id).
//
size_t CPreviewPage::EntityCameraCount(size_t entityIdx) const
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return 0;
    const uint16_t id = m_scenario->entities[entityIdx].entityId;
    size_t n = 0;
    for (const CameraFrame& c : m_scenario->cameras)
        if (c.kind == CameraKind::Entity && c.sourceEntityId == id) ++n;
    return n;
}

//
// DeleteCamerasForEntity — remove every Entity camera mounted on the entity at
//   `entityIdx` (its timeline boxes go with them), then re-tile the schedule.
//
void CPreviewPage::DeleteCamerasForEntity(size_t entityIdx)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;
    const uint16_t id = m_scenario->entities[entityIdx].entityId;

    auto& cams = m_scenario->cameras;
    const size_t before = cams.size();
    cams.erase(std::remove_if(cams.begin(), cams.end(),
                              [id](const CameraFrame& c) {
                                  return c.kind == CameraKind::Entity &&
                                         c.sourceEntityId == id;
                              }),
               cams.end());
    if (cams.size() == before) return;   // nothing mounted on this entity

    NormalizeCameraSchedule();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// ScrubToTime — move the playback time from a timeline click; sync the slider,
//   drop trail history, and repaint the canvas + strip.
//
void CPreviewPage::ScrubToTime(double t)
{
    const double D = EffectivePreviewDuration();
    if (t < 0.0) t = 0.0;
    if (t > D)   t = D;
    m_previewTimeSec = t;
    m_trails.clear();
    UpdateSliderFromTime();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
}

//
// OnTimelineFrameEditing — live repaint during a timeline box move/resize (only
//   times change; camera positions are unaffected, so the strip alone repaints).
//
void CPreviewPage::OnTimelineFrameEditing()
{
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
}

//
// OnTimelineEdited — commit a timeline box move/resize: clamp/sort the schedule
//   and mark the scenario dirty so it persists on Save.
//
void CPreviewPage::OnTimelineEdited()
{
    NormalizeCameraSchedule();
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// DeleteEntity — remove entity `idx` (never the last one), refresh caches, and
//   reload the other tabs.
//
void CPreviewPage::DeleteEntity(size_t idx)
{
    if (!m_scenario) return;
    if (m_scenario->entities.size() <= 1) return;   // keep at least one (matches the Assets page)
    if (idx >= m_scenario->entities.size()) return;

    // Explosions aimed at this entity lose their target: each becomes an
    // ordinary Line ending where it last hit (explosion.md: cascade deletes).
    SyncImpactEnds();
    const int goneId = static_cast<int>(m_scenario->entities[idx].entityId);
    for (Entity& other : m_scenario->entities)
        for (MotionSegment& s : other.motionSegments)
            if (s.impactEntityId == goneId) s.impactEntityId = -1;

    m_scenario->entities.erase(m_scenario->entities.begin() + static_cast<std::ptrdiff_t>(idx));

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::PlotLineCourse(size_t idx)
{
    if (!m_scenario) return;
    if (idx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[idx];

    // Anchor the course at the entity's CURRENT preview position (where its dot
    // is right now — which is its active-segment start, NOT necessarily its
    // initial pose) so generating the line never moves the entity. Fall back to
    // the initial ECEF only if the pose cache isn't populated yet.
    double east = 0.0, north = 0.0, up = 0.0;
    if (idx < m_state.poses.size())
    {
        east  = m_state.poses[idx].enuE;
        north = m_state.poses[idx].enuN;
        up    = m_state.poses[idx].enuU;
    }
    else
    {
        CoordTransforms::EcefToLocalEnuDeg(
            e.ecefX, e.ecefY, e.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            east, north, up);
    }

    // Speed from the entity TYPE (cruise), same catalog lookup the Motion page
    // uses; falls back to 10 m/s (and logs an error) when the type isn't catalogued.
    double speed = SeedCruiseSpeedMps(&e);

    // The course stays level at the entity's current altitude/depth (its
    // conveyance medium), so generating it never moves the entity vertically.

    // Initial course: a short starter segment along the entity's heading. The
    // user drags the pink end-anchor to the real destination (DragLineEnd then
    // rescales endSecond to keep cruise speed). Local ENU: X=East, Y=North, Z=Up;
    // heading is compass degrees (0=N, 90=E) so East=sin, North=cos.
    const double initLen = 2000.0;   // 2 km starter — visible and grabbable
    const double hr       = e.headingDeg * (3.14159265358979323846 / 180.0);

    MotionSegment seg;
    seg.type            = MotionType::Line;
    seg.coordMode       = CoordMode::Local;
    seg.startSecond     = 0.0;
    seg.endSecond       = (speed > 0.0) ? (initLen / speed) : 60.0;
    seg.speedMps        = speed;
    seg.startLocalX     = east;
    seg.startLocalY     = north;
    seg.startLocalZ     = up;
    seg.endLocalX       = east  + initLen * std::sin(hr);
    seg.endLocalY       = north + initLen * std::cos(hr);
    seg.endLocalZ       = up;
    seg.startHeadingDeg = e.headingDeg;
    seg.endHeadingDeg   = e.headingDeg;

    // "Plot a course" replaces any existing motion with this single Line.
    e.motionSegments.clear();
    e.motionSegments.push_back(std::move(seg));

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

bool CPreviewPage::CanTakeoff(size_t idx) const
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return false;
    const Entity& e = m_scenario->entities[idx];
    // Take-off applies to fixed-wing aircraft: Kind 1 (Platform), Domain 2 (Air).
    return e.kind == 1 && e.domain == 2;
}

void CPreviewPage::PlotTakeoffLine(size_t idx)
{
    if (!m_scenario) return;
    if (idx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[idx];

    // Anchor at the entity's current preview position (its start), like Plot>Line.
    double east = 0.0, north = 0.0, up = 0.0;
    if (idx < m_state.poses.size())
    {
        east  = m_state.poses[idx].enuE;
        north = m_state.poses[idx].enuN;
        up    = m_state.poses[idx].enuU;
    }
    else
    {
        CoordTransforms::EcefToLocalEnuDeg(
            e.ecefX, e.ecefY, e.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            east, north, up);
    }

    // Cruise (the speed reached at the END of the take-off roll); falls back to
    // 10 m/s (and logs an error) when the type isn't catalogued.
    double speed = SeedCruiseSpeedMps(&e);

    // Straight starter roll along the entity's heading, at the current altitude
    // (a take-off roll stays on the runway; the operator climbs on the NEXT leg
    // by raising its end altitude). Drag the pink end-anchor to the real
    // lift-off point. Duration = 2*length/speed so, accelerating from a stop at a
    // constant rate, the entity reaches cruise exactly at the end.
    const double initLen = 2000.0;   // 2 km starter — visible and grabbable
    const double hr      = e.headingDeg * (3.14159265358979323846 / 180.0);

    MotionSegment seg;
    seg.type              = MotionType::Line;
    seg.coordMode         = CoordMode::Local;
    seg.accelerateFromStop = true;   // start stopped, accelerate to `speed`
    seg.startSecond       = 0.0;
    seg.endSecond         = (speed > 0.0) ? (MotionSampler::TakeoffWindowFactor() * initLen / speed) : 60.0;
    seg.speedMps          = speed;
    seg.startLocalX       = east;
    seg.startLocalY       = north;
    seg.startLocalZ       = up;
    seg.endLocalX         = east  + initLen * std::sin(hr);
    seg.endLocalY         = north + initLen * std::cos(hr);
    seg.endLocalZ         = up;
    seg.startHeadingDeg   = e.headingDeg;
    seg.endHeadingDeg     = e.headingDeg;

    // "Plot a course" replaces any existing motion with this single take-off Line.
    e.motionSegments.clear();
    e.motionSegments.push_back(std::move(seg));

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::PlotEllipseCourse(size_t idx, bool clockwise)
{
    if (!m_scenario) return;
    if (idx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[idx];

    // Anchor at the entity's current preview position (where its dot is).
    double east = 0.0, north = 0.0, up = 0.0;
    if (idx < m_state.poses.size())
    {
        east  = m_state.poses[idx].enuE;
        north = m_state.poses[idx].enuN;
        up    = m_state.poses[idx].enuU;
    }
    else
    {
        CoordTransforms::EcefToLocalEnuDeg(
            e.ecefX, e.ecefY, e.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            east, north, up);
    }

    double speed = SeedCruiseSpeedMps(&e);

    // Default orbit: a ~2 km circle just south of the entity (entity sits at its
    // north point), with the two foci straddling the center on the E-W axis so
    // both are visible/grabbable. SnapEllipse fixes length + start bearing so the
    // entity is the orbit's start.
    const double R  = 2000.0;
    const double c0 = 600.0;
    const double cE = east;
    const double cN = north - R;

    double dur = m_scenario->durationSeconds;
    if (dur <= 0.0) dur = 300.0;

    MotionSegment seg;
    seg.type        = MotionType::Ellipse;
    seg.coordMode   = CoordMode::Local;
    seg.startSecond = 0.0;
    seg.endSecond   = dur;
    seg.speedMps    = speed;
    seg.direction   = clockwise ? EllipseDirection::Clockwise
                                : EllipseDirection::CounterClockwise;
    seg.f1LocalX = cE - c0; seg.f1LocalY = cN; seg.f1LocalZ = up;
    seg.f2LocalX = cE + c0; seg.f2LocalY = cN; seg.f2LocalZ = up;
    SnapEllipse(seg, east, north);

    e.motionSegments.clear();
    e.motionSegments.push_back(std::move(seg));

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

bool CPreviewPage::PromptForTargetEntity(size_t selfIdx, size_t& outTargetIdx)
{
    if (!m_scenario) return false;

    // Build the candidate list: every entity except the orbiter itself. Keep a
    // parallel index map so the chosen drop-down row maps back to an entity.
    std::vector<CString> labels;
    std::vector<size_t>  indices;
    for (size_t j = 0; j < m_scenario->entities.size(); ++j)
    {
        if (j == selfIdx) continue;
        labels.push_back(CString(EntityDisplayName(m_scenario->entities[j]).c_str()));
        indices.push_back(j);
    }
    if (indices.empty())
    {
        AfxMessageBox(_T("There are no other entities to orbit. Add another entity first."),
                      MB_ICONWARNING);
        return false;
    }

    CEntityPickerDialog dlg(labels, this);
    if (dlg.DoModal() != IDOK) return false;

    const int sel = dlg.Selection();
    if (sel < 0 || sel >= static_cast<int>(indices.size())) return false;

    outTargetIdx = indices[static_cast<size_t>(sel)];
    return true;
}

void CPreviewPage::PlotEntityEllipse(size_t idx, bool clockwise)
{
    if (!m_scenario) return;
    if (idx >= m_scenario->entities.size()) return;

    size_t targetIdx = 0;
    if (!PromptForTargetEntity(idx, targetIdx)) return;

    Entity& e = m_scenario->entities[idx];
    const Entity& target = m_scenario->entities[targetIdx];

    // Center the initial orbit on the TARGET's position AT THE ORBIT'S START TIME
    // (t = 0 here), NOT at the currently-scrubbed preview time. This is a FOLLOW
    // orbit: at playback the sampler re-centers it on the target's live position,
    // so the orbit start = target(t) + a fixed offset baked in by SnapEllipse. For
    // the orbiter to begin circling right where it sits (no jump onto the orbit),
    // that offset must be measured against the target's position at the same
    // instant the orbit begins. Using the scrubbed-time target position instead
    // introduces a gap whenever the preview isn't parked at t = 0.
    double cE = 0.0, cN = 0.0, up = 0.0;
    {
        const SampledPose tgt0 = MotionSampler::SamplePose(target, *m_scenario, 0.0);
        CoordTransforms::EcefToLocalEnuDeg(
            tgt0.ecefX, tgt0.ecefY, tgt0.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            cE, cN, up);
    }

    // The driven entity's own current position: the orbit is sized/oriented so
    // it PASSES THROUGH and STARTS at this point, so the entity begins circling
    // right where it is (no jump onto the orbit).
    double eE = 0.0, eN = 0.0, eU = 0.0;
    if (idx < m_state.poses.size())
    {
        eE = m_state.poses[idx].enuE;
        eN = m_state.poses[idx].enuN;
        eU = m_state.poses[idx].enuU;
    }
    else
    {
        CoordTransforms::EcefToLocalEnuDeg(
            e.ecefX, e.ecefY, e.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            eE, eN, eU);
    }

    double speed = SeedCruiseSpeedMps(&e);

    // Foci straddle the target E-W (so both are grabbable); the orbit's altitude
    // is the driven entity's (BuildEllipseFrame keeps it when re-centering).
    const double c0 = 600.0;

    double dur = m_scenario->durationSeconds;
    if (dur <= 0.0) dur = 300.0;

    MotionSegment seg;
    seg.type           = MotionType::Ellipse;
    seg.coordMode      = CoordMode::Local;
    seg.startSecond    = 0.0;
    seg.endSecond      = dur;
    seg.speedMps       = speed;
    seg.direction      = clockwise ? EllipseDirection::Clockwise
                                   : EllipseDirection::CounterClockwise;
    seg.followEntityId = static_cast<int>(target.entityId);
    seg.f1LocalX = cE - c0; seg.f1LocalY = cN; seg.f1LocalZ = eU;
    seg.f2LocalX = cE + c0; seg.f2LocalY = cN; seg.f2LocalZ = eU;
    SnapEllipse(seg, eE, eN);   // orbit passes through the driven entity, starts there

    e.motionSegments.clear();
    e.motionSegments.push_back(std::move(seg));

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// ClampImpactOffset — keep an impact point on the target: inside its catalogued
//   footprint (half-length along, half-width across), or within a small radius of
//   its reference point when the catalog has no size for it.
//
static void ClampImpactOffset(double& forwardM, double& rightM, double lengthM, double widthM)
{
    constexpr double kUncataloguedRadiusM = 10.0;
    if (lengthM > 0.0 && widthM > 0.0)
    {
        forwardM = std::min(std::max(forwardM, -0.5 * lengthM), 0.5 * lengthM);
        rightM   = std::min(std::max(rightM,   -0.5 * widthM),  0.5 * widthM);
        return;
    }
    const double r = std::hypot(forwardM, rightM);
    if (r > kUncataloguedRadiusM)
    {
        forwardM *= kUncataloguedRadiusM / r;
        rightM   *= kUncataloguedRadiusM / r;
    }
}

void CPreviewPage::EntityFootprint(size_t idx, double& lengthM, double& widthM) const
{
    lengthM = widthM = 0.0;
    if (!m_scenario || idx >= m_scenario->entities.size()) return;
    const Entity& t = m_scenario->entities[idx];
    const AirframeProfile prof = theApp.Catalog().Profile(t.kind, t.domain, t.category, t.subcategory);
    if (!prof.valid) return;
    lengthM = prof.lengthM;
    widthM  = prof.wingspanM;
}

bool CPreviewPage::HasTerminalExplosion(size_t idx) const
{
    return m_scenario && idx < m_scenario->entities.size() &&
           MotionSampler::TerminalImpactSegment(m_scenario->entities[idx]) != nullptr;
}

void CPreviewPage::SyncImpactEnds()
{
    if (!m_scenario) return;
    for (Entity& e : m_scenario->entities)
        for (MotionSegment& s : e.motionSegments)
        {
            double p[3];
            if (s.impactEntityId < 0 || !MotionSampler::ImpactPointEcef(s, *m_scenario, p)) continue;
            s.endEcefX = p[0]; s.endEcefY = p[1]; s.endEcefZ = p[2];
            MotionSampler::SyncSegmentEndpoint(s, /*isEnd*/true, CoordMode::ECEF,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM);
        }
}

void CPreviewPage::TerminateExplosion(size_t idx)
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return;
    Scenario& scn = *m_scenario;

    // Candidate targets: every other enabled entity, with its catalogued size so
    // the operator knows what the star can be moved over.
    std::vector<CString> labels, sizes;
    std::vector<size_t>  indices;
    for (size_t j = 0; j < scn.entities.size(); ++j)
    {
        const Entity& t = scn.entities[j];
        if (j == idx || !t.enabled) continue;
        CString label;
        label.Format(_T("%s  (ID %u)"), CString(EntityDisplayName(t).c_str()).GetString(),
                     static_cast<unsigned>(t.entityId));
        labels.push_back(label);
        double L = 0.0, W = 0.0;
        EntityFootprint(j, L, W);
        CString size;
        if (L > 0.0 && W > 0.0) size.Format(_T("Target size: %.1f m long x %.1f m wide"), L, W);
        else                    size = _T("Target size not catalogued: the star stays within 10 m of it.");
        sizes.push_back(size);
        indices.push_back(j);
    }
    if (indices.empty())
    {
        AfxMessageBox(_T("There are no other entities to crash into. Add another entity first."),
                      MB_ICONWARNING);
        return;
    }

    // Which leg ends in the explosion: the existing impact leg; else the course's
    // last Line (the plotted line is redirected into the target); else a new leg
    // appended where the course ends (after an orbit or a hold, or from the
    // entity's start when it has no course at all). Take-off rolls are never
    // converted: they accelerate from a stop and belong on the runway.
    const Entity& src = scn.entities[idx];
    int legIdx = -1;
    bool append = false;
    if (const MotionSegment* ex = MotionSampler::TerminalImpactSegment(src))
        legIdx = static_cast<int>(ex - src.motionSegments.data());
    else
    {
        int lastIdx = -1;
        for (int k = static_cast<int>(src.motionSegments.size()) - 1; k >= 0; --k)
            if (src.motionSegments[k].enabled) { lastIdx = k; break; }
        if (lastIdx >= 0 && src.motionSegments[lastIdx].type == MotionType::Line &&
            !src.motionSegments[lastIdx].accelerateFromStop)
            legIdx = lastIdx;
        else
        {
            append = true;
            legIdx = lastIdx;   // the leg the new one chains off (-1 = none)
        }
    }
    const double legStart = append
        ? (legIdx >= 0 ? src.motionSegments[legIdx].endSecond : 0.0)
        : src.motionSegments[legIdx].startSecond;

    CTerminateExplosionDialog dlg(CString(EntityDisplayName(src).c_str()), labels, sizes, this);
    dlg.m_minTime = legStart;
    if (!append && src.motionSegments[legIdx].impactEntityId >= 0)
    {
        // Editing: seed from the existing explosion, keeping its time.
        const MotionSegment& ex = src.motionSegments[legIdx];
        for (size_t k = 0; k < indices.size(); ++k)
            if (static_cast<int>(scn.entities[indices[k]].entityId) == ex.impactEntityId)
                dlg.m_sel = static_cast<int>(k);
        dlg.m_autoTime = false;
        dlg.m_timeSec  = ex.endSecond;
        dlg.m_forwardM = ex.impactForwardM;
        dlg.m_rightM   = ex.impactRightM;
        dlg.m_upM      = ex.impactUpM;
    }
    else
    {
        dlg.m_timeSec = append ? legStart + 60.0 : src.motionSegments[legIdx].endSecond;
        // Suggested aim height per target. A ship's reference point is its
        // waterline, so aim a quarter of its catalogued height up the hull (at
        // least the sampler's 2 m floor); air and land targets aim at their
        // reference point.
        for (size_t j : indices)
        {
            const Entity& t = scn.entities[j];
            double up = 0.0;
            if (t.domain == 3)
            {
                const AirframeProfile prof =
                    theApp.Catalog().Profile(t.kind, t.domain, t.category, t.subcategory);
                up = std::max(2.0, prof.valid ? 0.25 * prof.heightM : 0.0);
            }
            dlg.m_upDefaults.push_back(up);
        }
    }
    if (dlg.DoModal() != IDOK) return;

    Entity& e = scn.entities[idx];
    const size_t targetIdx = indices[static_cast<size_t>(dlg.m_sel)];
    const Entity& target = scn.entities[targetIdx];

    if (append)
    {
        // New leg from wherever the course leaves the entity at legStart.
        const SampledPose p0 = MotionSampler::SamplePose(e, scn, legStart);
        double east = 0.0, north = 0.0, up = 0.0;
        CoordTransforms::EcefToLocalEnuDeg(p0.ecefX, p0.ecefY, p0.ecefZ,
            scn.originLatDeg, scn.originLonDeg, scn.originAltM, east, north, up);
        MotionSegment seg;
        seg.type        = MotionType::Line;
        seg.coordMode   = CoordMode::Local;
        seg.startSecond = legStart;
        seg.endSecond   = legStart + 60.0;
        seg.speedMps    = (legIdx >= 0 && e.motionSegments[legIdx].speedMps > 0.0)
                              ? e.motionSegments[legIdx].speedMps : SeedCruiseSpeedMps(&e);
        seg.startLocalX = east;  seg.startLocalY = north;  seg.startLocalZ = up;
        seg.endLocalX   = east;  seg.endLocalY   = north;  seg.endLocalZ   = up;
        MotionSampler::SyncSegmentEndpoint(seg, /*isEnd*/false, CoordMode::Local,
            scn.originLatDeg, scn.originLonDeg, scn.originAltM);
        e.motionSegments.push_back(std::move(seg));
        legIdx = static_cast<int>(e.motionSegments.size()) - 1;
    }

    // The explosion ends the course: nothing authored after it can play.
    e.motionSegments.erase(e.motionSegments.begin() + legIdx + 1, e.motionSegments.end());

    MotionSegment& seg = e.motionSegments[static_cast<size_t>(legIdx)];
    seg.impactEntityId = static_cast<int>(target.entityId);
    seg.impactForwardM = dlg.m_forwardM;
    seg.impactRightM   = dlg.m_rightM;
    seg.impactUpM      = dlg.m_upM;
    {
        double L = 0.0, W = 0.0;
        EntityFootprint(targetIdx, L, W);
        ClampImpactOffset(seg.impactForwardM, seg.impactRightM, L, W);
    }
    if (seg.speedMps <= 0.0) seg.speedMps = SeedCruiseSpeedMps(&e);

    if (dlg.m_autoTime)
    {
        // Meet the target where it WILL be: solve the intercept at cruise speed.
        const SampledPose s0 = MotionSampler::EvaluateSegment(seg, 0.0, &scn, true);
        const double start[3] = { s0.ecefX, s0.ecefY, s0.ecefZ };
        const double horizon = seg.startSecond + std::max(3600.0, EffectivePreviewDuration());
        seg.endSecond = MotionSampler::SolveImpactTime(seg, scn, start, seg.startSecond,
                                                       seg.speedMps, horizon);
    }
    else
    {
        seg.endSecond = dlg.m_timeSec;
    }

    // Keep the entity's own ES window open until it hits.
    if (e.endSecond < seg.endSecond) e.endSecond = seg.endSecond;

    SyncImpactEnds();
    RebuildPathsCache();
    UpdateSliderRange();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);

    if (scn.durationSeconds > 0.0 && seg.endSecond > scn.durationSeconds)
    {
        CString msg;
        msg.Format(_T("The impact happens at %.1f s, after the scenario ends (%.1f s).\n")
                   _T("Raise the scenario duration or the explosion will not be sent on Run."),
                   seg.endSecond, scn.durationSeconds);
        AfxMessageBox(msg, MB_ICONWARNING);
    }
}

void CPreviewPage::RemoveTerminalExplosion(size_t idx)
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return;
    SyncImpactEnds();   // the leg keeps ending where it last hit
    for (MotionSegment& s : m_scenario->entities[idx].motionSegments)
        s.impactEntityId = -1;

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::DragImpactPoint(size_t entityIdx, size_t segIdx, double enuE, double enuN)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[entityIdx];
    if (segIdx >= e.motionSegments.size()) return;
    MotionSegment& seg = e.motionSegments[segIdx];
    const int ti = MotionSampler::ImpactTargetIndex(seg, *m_scenario);
    if (ti < 0) return;

    // Re-express the dropped point relative to the target at the impact instant,
    // in its heading frame -- the same frame the sampler rebuilds it from.
    const SampledPose tp = MotionSampler::SamplePose(
        m_scenario->entities[static_cast<size_t>(ti)], *m_scenario, seg.endSecond);
    double te = 0.0, tn = 0.0, tu = 0.0;
    CoordTransforms::EcefToLocalEnuDeg(tp.ecefX, tp.ecefY, tp.ecefZ,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM, te, tn, tu);
    const double dE = enuE - te, dN = enuN - tn;
    const double h  = tp.headingDeg * (3.14159265358979323846 / 180.0);
    double fwd   = dE * std::sin(h) + dN * std::cos(h);
    double right = dE * std::cos(h) - dN * std::sin(h);

    double L = 0.0, W = 0.0;
    EntityFootprint(static_cast<size_t>(ti), L, W);
    ClampImpactOffset(fwd, right, L, W);
    seg.impactForwardM = fwd;
    seg.impactRightM   = right;

    RebuildPathsCache();   // re-syncs the stored end
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::DragEllipseFocus(size_t entityIdx, size_t segIdx, int focusIdx,
                                    double e, double n)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;
    Entity& ent = m_scenario->entities[entityIdx];
    if (segIdx >= ent.motionSegments.size()) return;
    MotionSegment& s = ent.motionSegments[segIdx];
    if (s.type != MotionType::Ellipse || s.coordMode != CoordMode::Local) return;

    // The orbit's start point (u=0) in the STORED foci frame, kept fixed so the
    // orbit re-forms around it as the focus moves. We build the frame WITHOUT the
    // follow override so the start point and the foci live in the same (stored)
    // frame -- otherwise a moving follow target and the offset below use different
    // reference times and SnapEllipse blows lengthMeters up to the target's travel
    // distance (the "extremely large orbit" bug).
    const MotionSampler::EllipseFrame f0 =
        MotionSampler::BuildEllipseFrame(s, m_scenario, /*withArcTable*/ false, nullptr);
    const MotionSampler::EllipsePoint sp0 =
        MotionSampler::EvalEllipseAtTheta(f0, f0.theta0);
    double pE = 0.0, pN = 0.0, pU = 0.0;
    CoordTransforms::EcefToLocalEnuDeg(
        sp0.ecefX, sp0.ecefY, sp0.ecefZ,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM, pE, pN, pU);

    // Entity Ellipse: the draggable handles are drawn offset onto the moving
    // target. Convert only the dropped mouse point back into the stored frame so
    // f1/f2 keep defining the shape; pE/pN are already in the stored frame.
    double dE = 0.0, dN = 0.0;
    if (FollowCenterOffset(s, dE, dN)) { e -= dE; n -= dN; }

    if (focusIdx == 0) { s.f1LocalX = e; s.f1LocalY = n; }
    else               { s.f2LocalX = e; s.f2LocalY = n; }
    SnapEllipse(s, pE, pN);

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::DragEllipseShapeHandle(size_t entityIdx, size_t segIdx,
                                          double e, double n)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;
    Entity& ent = m_scenario->entities[entityIdx];
    if (segIdx >= ent.motionSegments.size()) return;
    MotionSegment& s = ent.motionSegments[segIdx];
    if (s.type != MotionType::Ellipse) return;

    // Re-size the orbit so its curve passes through the dragged point; that point
    // becomes the orbit start (same rule the entity-dot anchor used). Foci stay
    // put, so dragging the handle grows/shrinks the orbit.
    if (s.coordMode == CoordMode::Local)
        SnapEllipse(s, e, n);
    else
        SetEllipseStartBearing(entityIdx, segIdx, s, e, n);

    // If a Line immediately precedes this orbit, glue its end to the new start so
    // the path stays connected (no spatial jump at the line->orbit transition).
    int prevIdx = -1;
    for (int k = static_cast<int>(segIdx) - 1; k >= 0; --k)
        if (ent.motionSegments[k].enabled) { prevIdx = k; break; }
    if (prevIdx >= 0)
    {
        MotionSegment& prev = ent.motionSegments[static_cast<size_t>(prevIdx)];
        if (prev.type == MotionType::Line && prev.coordMode == CoordMode::Local)
        {
            prev.endLocalX = e;   // 2-D move; conveyance plane (endLocalZ) kept
            prev.endLocalY = n;
            const double dx = prev.endLocalX - prev.startLocalX;
            const double dy = prev.endLocalY - prev.startLocalY;
            const double dz = prev.endLocalZ - prev.startLocalZ;
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (prev.speedMps > 0.0 && dist > 0.0)
                prev.endSecond = prev.startSecond + dist / prev.speedMps;

            // Keep the orbit time-contiguous with the (re-timed) line.
            const double dur = s.endSecond - s.startSecond;
            s.startSecond = prev.endSecond;
            s.endSecond   = s.startSecond + (dur > 0.0 ? dur : 0.0);
        }
    }

    // Keep any legs following this orbit time-contiguous too.
    RechainFollowingTimes(ent, segIdx);

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// RechainFollowingTimes — after a segment's endSecond changes, walk forward and
//   re-glue every following enabled segment so the whole course stays TIME-
//   contiguous. Each leg's start is pinned to the previous leg's end; a Line's
//   own end is recomputed from its geometry & speed (take-off legs use the
//   2*dist/speed acceleration window), while other leg types (ellipse, etc.)
//   preserve their existing duration. Editing an early leg used to re-glue only
//   the IMMEDIATE next leg, leaving downstream legs with stale start times — the
//   resulting inter-segment time gap froze the entity in place (the "several
//   second pause between the take-off and the next line"). Cascading closes it.
//
void CPreviewPage::RechainFollowingTimes(Entity& e, size_t fromIdx)
{
    // `fromIdx` is the last leg whose timing is already correct; it becomes the
    // anchor. Walk forward, pinning each subsequent ENABLED leg's start to the
    // previous enabled leg's end. (Pointers stay valid — the vector isn't resized.)
    const MotionSegment* prev = nullptr;
    if (fromIdx < e.motionSegments.size() && e.motionSegments[fromIdx].enabled)
        prev = &e.motionSegments[fromIdx];

    for (size_t k = fromIdx + 1; k < e.motionSegments.size(); ++k)
    {
        MotionSegment& cur = e.motionSegments[k];
        if (!cur.enabled) continue;
        if (!prev) { prev = &cur; continue; }   // nothing before it to chain to

        const double dur = cur.endSecond - cur.startSecond;   // preserved for non-Line legs
        cur.startSecond = prev->endSecond;
        if (cur.type == MotionType::Line && cur.coordMode == CoordMode::Local &&
            cur.speedMps > 0.0)
        {
            const double dx = cur.endLocalX - cur.startLocalX;
            const double dy = cur.endLocalY - cur.startLocalY;
            const double dz = cur.endLocalZ - cur.startLocalZ;
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > 0.0)
                cur.endSecond = cur.startSecond +
                    (cur.accelerateFromStop
                         ? (MotionSampler::TakeoffWindowFactor() * dist / cur.speedMps)
                         : (dist / cur.speedMps));
            else
                cur.endSecond = cur.startSecond + (dur > 0.0 ? dur : 0.0);
        }
        else
        {
            cur.endSecond = cur.startSecond + (dur > 0.0 ? dur : 0.0);
        }
        prev = &cur;
    }
}

void CPreviewPage::DragLineEnd(size_t entityIdx, size_t segIdx, double enuE, double enuN)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[entityIdx];
    if (segIdx >= e.motionSegments.size()) return;
    MotionSegment& seg = e.motionSegments[segIdx];
    if (seg.type != MotionType::Line) return;

    // Move the destination horizontally; keep the conveyance plane (endLocalZ).
    seg.endLocalX = enuE;
    seg.endLocalY = enuN;

    // The drag manipulates the Local ENU rep; re-derive this end's ECEF and
    // Lat/Lon/Alt from it so the Motion tab shows the moved point in ANY
    // coordinate mode (not just Local) and the sampler resolves what's drawn.
    MotionSampler::SyncSegmentEndpoint(seg, /*isEnd*/true, CoordMode::Local,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM);

    // Keep travel time consistent with the type's cruise speed over the new
    // distance, so the entity still arrives at the end at endSecond. A take-off
    // leg accelerates from 0 to `speedMps` along the jet tanh profile, so it needs
    // C(k)*dist/speed (MotionSampler::TakeoffWindowFactor) to reach cruise at the end.
    const double dx = seg.endLocalX - seg.startLocalX;
    const double dy = seg.endLocalY - seg.startLocalY;
    const double dz = seg.endLocalZ - seg.startLocalZ;
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (seg.speedMps > 0.0 && dist > 0.0)
        seg.endSecond = seg.startSecond +
            (seg.accelerateFromStop
                 ? (MotionSampler::TakeoffWindowFactor() * dist / seg.speedMps)
                 : (dist / seg.speedMps));

    // Keep a chained next leg glued to this moving end so the multi-leg course
    // stays connected (drag a junction -> the following leg's start follows).
    if (segIdx + 1 < e.motionSegments.size())
    {
        MotionSegment& nxt = e.motionSegments[segIdx + 1];
        if (nxt.enabled && nxt.type == MotionType::Line && nxt.coordMode == CoordMode::Local)
        {
            nxt.startLocalX = seg.endLocalX;
            nxt.startLocalY = seg.endLocalY;
            nxt.startLocalZ = seg.endLocalZ;
            // Same Local->all-reps sync for the glued start, so the following
            // leg's begin point reads correctly in the Motion tab too.
            MotionSampler::SyncSegmentEndpoint(nxt, /*isEnd*/false, CoordMode::Local,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM);
            nxt.startSecond = seg.endSecond;
            const double nx = nxt.endLocalX - nxt.startLocalX;
            const double ny = nxt.endLocalY - nxt.startLocalY;
            const double nz = nxt.endLocalZ - nxt.startLocalZ;
            const double nd = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nxt.speedMps > 0.0 && nd > 0.0)
                nxt.endSecond = nxt.startSecond + nd / nxt.speedMps;
        }
        else if (nxt.enabled && nxt.type == MotionType::Ellipse)
        {
            // Following orbit: re-snap it so the curve passes through — and
            // STARTS at — the line's new end (foci stay put, length + start
            // bearing recomputed). Without this the orbit keeps its old start
            // bearing pointing at where the line USED to end, so the entity
            // teleports from the new line end to the stale start when the orbit
            // begins. Mirrors DragEllipseShapeHandle's line-gluing rule.
            if (nxt.coordMode == CoordMode::Local)
                SnapEllipse(nxt, seg.endLocalX, seg.endLocalY);
            else
                SetEllipseStartBearing(entityIdx, segIdx + 1, nxt,
                                       seg.endLocalX, seg.endLocalY);

            // Keep it time-contiguous so moving the line end can't open a gap
            // before the ellipse (which would otherwise freeze the entity during
            // the gap). Preserve the orbit's own duration.
            const double dur = nxt.endSecond - nxt.startSecond;
            nxt.startSecond = seg.endSecond;
            nxt.endSecond   = nxt.startSecond + (dur > 0.0 ? dur : 0.0);
        }
    }

    // Cascade the re-timing through every leg after the immediate next one, so a
    // multi-leg course can't be left with a frozen time-gap downstream.
    if (segIdx + 1 < e.motionSegments.size())
        RechainFollowingTimes(e, segIdx + 1);

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// EndLineEndDrag — commit a finished line-end drag: reload the other tabs and mark dirty.
//
void CPreviewPage::EndLineEndDrag()
{
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::AddLineToEnd(size_t entityIdx)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[entityIdx];

    // Chain off the entity's last enabled segment.
    int lastIdx = -1;
    for (int k = static_cast<int>(e.motionSegments.size()) - 1; k >= 0; --k)
        if (e.motionSegments[k].enabled) { lastIdx = k; break; }
    if (lastIdx < 0) return;
    const MotionSegment& prev = e.motionSegments[static_cast<size_t>(lastIdx)];

    // Resolve the previous leg's start & end to scenario-origin ENU (any
    // coordMode) so the new leg begins exactly where the old one ends and
    // continues in the same direction.
    auto segEnu = [&](double u, double& ee, double& nn, double& uu)
    {
        const SampledPose p = MotionSampler::EvaluateSegment(prev, u, m_scenario, true);
        CoordTransforms::EcefToLocalEnuDeg(
            p.ecefX, p.ecefY, p.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            ee, nn, uu);
    };
    double s0e, s0n, s0u, e1e, e1n, e1u;
    segEnu(0.0, s0e, s0n, s0u);
    segEnu(1.0, e1e, e1n, e1u);

    double dirE = e1e - s0e, dirN = e1n - s0n;
    double dlen = std::sqrt(dirE * dirE + dirN * dirN);
    if (dlen < 1e-6) { dirE = 0.0; dirN = 1.0; dlen = 1.0; }   // degenerate -> due north
    dirE /= dlen; dirN /= dlen;

    const double speed   = (prev.speedMps > 0.0) ? prev.speedMps : 100.0;
    const double initLen = 2000.0;   // 2 km starter leg; drag the pink end to place it
    double bearing = std::atan2(dirE, dirN) * (180.0 / 3.14159265358979323846);
    if (bearing < 0.0) bearing += 360.0;

    MotionSegment seg;
    seg.type            = MotionType::Line;
    seg.coordMode       = CoordMode::Local;
    seg.startSecond     = prev.endSecond;
    seg.endSecond       = prev.endSecond + initLen / speed;
    seg.speedMps        = speed;
    seg.startLocalX     = e1e;
    seg.startLocalY     = e1n;
    seg.startLocalZ     = e1u;
    seg.endLocalX       = e1e + initLen * dirE;
    seg.endLocalY       = e1n + initLen * dirN;
    seg.endLocalZ       = e1u;
    seg.startHeadingDeg = bearing;
    seg.endHeadingDeg   = bearing;

    e.motionSegments.push_back(std::move(seg));

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::AddEllipseToEnd(size_t entityIdx, bool clockwise)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[entityIdx];

    int lastIdx = -1;
    for (int k = static_cast<int>(e.motionSegments.size()) - 1; k >= 0; --k)
        if (e.motionSegments[k].enabled) { lastIdx = k; break; }
    if (lastIdx < 0) return;
    const MotionSegment& prev = e.motionSegments[static_cast<size_t>(lastIdx)];

    // End point of the previous leg (ENU) — where the orbit attaches.
    const SampledPose pend = MotionSampler::EvaluateSegment(prev, 1.0, m_scenario, true);
    double east = 0.0, north = 0.0, up = 0.0;
    CoordTransforms::EcefToLocalEnuDeg(
        pend.ecefX, pend.ecefY, pend.ecefZ,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
        east, north, up);

    const double speed = (prev.speedMps > 0.0) ? prev.speedMps : 100.0;
    const double R  = 2000.0;
    const double c0 = 600.0;
    const double cE = east;
    const double cN = north - R;

    double dur = m_scenario->durationSeconds;
    if (dur <= 0.0) dur = 300.0;

    MotionSegment seg;
    seg.type        = MotionType::Ellipse;
    seg.coordMode   = CoordMode::Local;
    seg.startSecond = prev.endSecond;
    seg.endSecond   = prev.endSecond + dur;
    seg.speedMps    = speed;
    seg.direction   = clockwise ? EllipseDirection::Clockwise
                                : EllipseDirection::CounterClockwise;
    seg.f1LocalX = cE - c0; seg.f1LocalY = cN; seg.f1LocalZ = up;
    seg.f2LocalX = cE + c0; seg.f2LocalY = cN; seg.f2LocalZ = up;
    SnapEllipse(seg, east, north);   // orbit starts at the attach point

    e.motionSegments.push_back(std::move(seg));

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::AddEntityEllipseToEnd(size_t entityIdx, bool clockwise)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;

    size_t targetIdx = 0;
    if (!PromptForTargetEntity(entityIdx, targetIdx)) return;

    Entity& e = m_scenario->entities[entityIdx];
    const Entity& target = m_scenario->entities[targetIdx];

    int lastIdx = -1;
    for (int k = static_cast<int>(e.motionSegments.size()) - 1; k >= 0; --k)
        if (e.motionSegments[k].enabled) { lastIdx = k; break; }
    if (lastIdx < 0) return;
    MotionSegment& prev = e.motionSegments[static_cast<size_t>(lastIdx)];

    const double speed = (prev.speedMps > 0.0) ? prev.speedMps : 100.0;

    // The attach point = where the previous leg ends (the line's endpoint). The
    // orbit is sized/oriented so it PASSES THROUGH and STARTS at this point, so
    // the entity flows off the line straight onto the orbit edge -- no jump, no
    // crossing into the center. (Same rule as the regular Add Ellipse, but the
    // center is the target instead of an offset from the attach point.)
    const SampledPose pend = MotionSampler::EvaluateSegment(prev, 1.0, m_scenario, true);
    double attachE = 0.0, attachN = 0.0, attachU = 0.0;
    CoordTransforms::EcefToLocalEnuDeg(
        pend.ecefX, pend.ecefY, pend.ecefZ,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
        attachE, attachN, attachU);

    // Center the orbit on the target's position AT THE TRANSITION TIME (when the
    // orbiter actually flies off the line onto the orbit), NOT at whatever time
    // the preview is currently scrubbed to.
    //
    // This is a FOLLOW orbit: at playback the sampler re-centers it every frame on
    // the target's LIVE position (MotionSampler::ResolveFollowCenterEcef), so the
    // orbit's start point is (target(t) + a fixed start-offset baked in by
    // SnapEllipse). For the entity to flow off the line WITHOUT A JUMP, the orbit
    // start must coincide with the line end exactly when the orbiter arrives —
    // t = prev.endSecond (== the new segment's startSecond). Snapping against the
    // target's *authoring-time* (scrubbed) position instead is what left the gap:
    // by the time the orbiter reached the line end, the target (e.g. the cargo
    // ship) had sailed on, and the re-centered orbit no longer began at the line
    // end. Sampling the target at prev.endSecond bakes in the right offset so the
    // re-centering lands the orbit start exactly on the line end at the handoff.
    const double t0 = prev.endSecond;   // == seg.startSecond, set below
    double cE = 0.0, cN = 0.0, up = 0.0;
    {
        const SampledPose tgt0 = MotionSampler::SamplePose(target, *m_scenario, t0);
        CoordTransforms::EcefToLocalEnuDeg(
            tgt0.ecefX, tgt0.ecefY, tgt0.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            cE, cN, up);
    }

    const double c0 = 600.0;

    double dur = m_scenario->durationSeconds;
    if (dur <= 0.0) dur = 300.0;

    MotionSegment seg;
    seg.type           = MotionType::Ellipse;
    seg.coordMode      = CoordMode::Local;
    seg.startSecond    = prev.endSecond;
    seg.endSecond      = prev.endSecond + dur;
    seg.speedMps       = speed;
    seg.direction      = clockwise ? EllipseDirection::Clockwise
                                   : EllipseDirection::CounterClockwise;
    seg.followEntityId = static_cast<int>(target.entityId);
    // Foci straddle the target E-W; orbit altitude = the line-end's (the orbiter
    // keeps its cruise altitude, not the target's).
    seg.f1LocalX = cE - c0; seg.f1LocalY = cN; seg.f1LocalZ = attachU;
    seg.f2LocalX = cE + c0; seg.f2LocalY = cN; seg.f2LocalZ = attachU;
    SnapEllipse(seg, attachE, attachN);   // orbit passes through & starts at the line end

    e.motionSegments.push_back(std::move(seg));

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::SetPathDuration(size_t idx)
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[idx];

    // Enabled segments in order.
    std::vector<size_t> segs;
    for (size_t k = 0; k < e.motionSegments.size(); ++k)
        if (e.motionSegments[k].enabled) segs.push_back(k);
    if (segs.empty())
    {
        AfxMessageBox(_T("This entity has no motion path to set a duration for."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    const double firstStart = e.motionSegments[segs.front()].startSecond;
    const double lastEnd     = e.motionSegments[segs.back()].endSecond;
    const double curDur      = lastEnd - firstStart;

    // Show the current duration and let the operator change it.
    CString init;
    init.Format(_T("%.1f"), curDur);
    CPromptDialog dlg(_T("Set Path Duration"), _T("Duration (seconds):"), init, this);
    if (dlg.DoModal() != IDOK) return;
    const double D = _tstof(dlg.Value());
    if (D <= 0.0) return;

    RescalePathDuration(e, D, m_scenario);

    UpdateSliderRange();
    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::SetEntityDelay(size_t idx)
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return;
    Entity& e = m_scenario->entities[idx];

    // Enabled segments in order.
    std::vector<size_t> segs;
    for (size_t k = 0; k < e.motionSegments.size(); ++k)
        if (e.motionSegments[k].enabled) segs.push_back(k);
    if (segs.empty())
    {
        AfxMessageBox(_T("This entity has no motion path to delay."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    // The current delay is simply where the first leg starts. Treat the entered
    // value as ABSOLUTE (not cumulative) so re-running it is idempotent: shift
    // every leg by (newDelay - currentStart) so the first leg begins at newDelay.
    const double curDelay = e.motionSegments[segs.front()].startSecond;

    CString init;
    init.Format(_T("%.1f"), curDelay);
    CPromptDialog dlg(_T("Set Delay"), _T("Delay before moving (seconds):"), init, this);
    if (dlg.DoModal() != IDOK) return;
    const double delay = _tstof(dlg.Value());
    if (delay < 0.0) return;

    const double shift = delay - curDelay;
    if (shift == 0.0) return;
    for (size_t k : segs)
    {
        MotionSegment& s = e.motionSegments[k];
        s.startSecond += shift;
        s.endSecond   += shift;
    }

    UpdateSliderRange();
    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// SetSelectedEntities — replace the group selection with the given entity indices
//   (clearing refresh-failed flags), then repaint.
//
void CPreviewPage::SetSelectedEntities(const std::vector<size_t>& idxs)
{
    m_selected.clear();
    m_refreshFailed.clear();
    if (m_scenario)
        for (size_t i : idxs)
            if (i < m_scenario->entities.size()) m_selected.insert(i);
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// ClearSelection — drop the group selection and refresh-failed flags, then repaint.
//
void CPreviewPage::ClearSelection()
{
    if (m_selected.empty() && m_refreshFailed.empty()) return;
    m_selected.clear();
    m_refreshFailed.clear();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::SetGroupDuration()
{
    if (!m_scenario || m_selected.empty()) return;

    // Seed the prompt with the first selected entity's current path duration.
    double seedDur = 0.0;
    for (size_t i : m_selected)
    {
        if (i >= m_scenario->entities.size()) continue;
        const Entity& e = m_scenario->entities[i];
        const MotionSegment* f = nullptr;
        const MotionSegment* l = nullptr;
        for (const MotionSegment& s : e.motionSegments)
            if (s.enabled) { if (!f) f = &s; l = &s; }
        if (f && l) { seedDur = l->endSecond - f->startSecond; break; }
    }

    CString init;
    init.Format(_T("%.1f"), seedDur);
    CPromptDialog dlg(_T("Set Duration (selected)"), _T("Duration (seconds):"), init, this);
    if (dlg.DoModal() != IDOK) return;
    const double D = _tstof(dlg.Value());
    if (D <= 0.0) return;

    for (size_t i : m_selected)
        if (i < m_scenario->entities.size())
            RescalePathDuration(m_scenario->entities[i], D, m_scenario);

    UpdateSliderRange();
    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

void CPreviewPage::RefreshGroupSpeed()
{
    if (!m_scenario || m_selected.empty()) return;

    m_refreshFailed.clear();
    for (size_t i : m_selected)
    {
        if (i >= m_scenario->entities.size()) continue;
        Entity& e = m_scenario->entities[i];
        const AirframeProfile prof = theApp.Catalog().Profile(
            e.kind, e.domain, e.category, e.subcategory);
        if (prof.valid && prof.cruiseSpeedMps > 0.0)
            RefreshPathSpeed(e, prof.cruiseSpeedMps, m_scenario);
        else
            m_refreshFailed.insert(i);   // no catalog cruise -> leave unchanged, flag red
    }

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// RefreshEntitySpeed — re-pull one entity's catalog cruise speed onto its moving
//   segments (nags if the type has no catalog cruise).
//
void CPreviewPage::RefreshEntitySpeed(size_t idx)
{
    if (!m_scenario || idx >= m_scenario->entities.size()) return;

    Entity& e = m_scenario->entities[idx];
    const AirframeProfile prof = theApp.Catalog().Profile(
        e.kind, e.domain, e.category, e.subcategory);
    if (prof.valid && prof.cruiseSpeedMps > 0.0)
    {
        m_refreshFailed.erase(idx);
        RefreshPathSpeed(e, prof.cruiseSpeedMps, m_scenario);
    }
    else
    {
        AfxMessageBox(_T("This entity's type has no catalog cruise speed to refresh from."),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// OnDestTimeToggle — show/hide the destination-time grid, populating it when shown.
//
void CPreviewPage::OnDestTimeToggle()
{
    ToggleCheck(IDC_CHK_PREVIEW_DESTTIME, m_showDestTime);
    if (CWnd* lc = GetDlgItem(IDC_LIST_PREVIEW_DESTTIME))
        lc->ShowWindow(m_showDestTime ? SW_SHOW : SW_HIDE);
    if (m_showDestTime) RefreshDestTimeList();
}

// Selecting a row in the Properties grid selects the matching entity in the
// preview. Each row carries its entity index in item data (set during
// RefreshDestTimeList). We act only on the selection-gained transition;
// DeleteAllItems/InsertItem during a refresh never raise that, so there's no
// reentrancy with SetSelectedEntities.
void CPreviewPage::OnPropertiesItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    const NMLISTVIEW* nm = reinterpret_cast<const NMLISTVIEW*>(pNMHDR);
    *pResult = 0;
    if (!nm || nm->iItem < 0) return;
    if (!(nm->uChanged & LVIF_STATE)) return;

    const bool gained = (nm->uNewState & LVNI_SELECTED) && !(nm->uOldState & LVNI_SELECTED);
    if (!gained) return;

    CListCtrl* lc = (CListCtrl*)GetDlgItem(IDC_LIST_PREVIEW_DESTTIME);
    if (!lc || !lc->GetSafeHwnd()) return;

    const size_t idx = static_cast<size_t>(lc->GetItemData(nm->iItem));
    m_suppressPropsRefresh = true;          // don't tear down the list mid-click
    SetSelectedEntities(std::vector<size_t>{ idx });
    m_suppressPropsRefresh = false;
}

//
// RefreshDestTimeList — rebuild the properties grid: per entity the scheduled path
//   Duration, geometric Path Time (at catalog cruise), Cruise speed, and an
//   Infinite flag for terminal orbits. Each row stores its entity index.
//
void CPreviewPage::RefreshDestTimeList()
{
    CListCtrl* lc = (CListCtrl*)GetDlgItem(IDC_LIST_PREVIEW_DESTTIME);
    if (!lc || !lc->GetSafeHwnd()) return;

    lc->SetRedraw(FALSE);
    lc->DeleteAllItems();
    if (m_scenario)
    {
        for (size_t i = 0; i < m_scenario->entities.size(); ++i)
        {
            const Entity& e = m_scenario->entities[i];
            const std::string disp = e.marking.empty() ? e.name : e.marking;
            const CString nm(disp.c_str());

            const MotionSegment* first = nullptr;
            const MotionSegment* last  = nullptr;
            for (const MotionSegment& s : e.motionSegments)
                if (s.enabled) { if (!first) first = &s; last = &s; }

            CString durStr  = _T("-");   // em dash = no path / no destination
            CString pathStr = _T("-");   // geometric traversal time of the path
            bool infinite = false;

            // Catalog cruise speed for this entity's type (independent of any path).
            double  cruiseMps = 0.0;
            CString cruiseStr = _T("-");
            {
                const AirframeProfile prof = theApp.Catalog().Profile(
                    e.kind, e.domain, e.category, e.subcategory);
                if (prof.valid && prof.cruiseSpeedMps > 0.0)
                {
                    cruiseMps = prof.cruiseSpeedMps;
                    cruiseStr.Format(_T("%.9g"), cruiseMps);
                }
            }

            if (first && last)
            {
                // Duration: the entity's scheduled path window.
                durStr.Format(_T("%.1f"), last->endSecond - first->startSecond);

                // Circling: one full orbit is endless, flag it as Infinite.
                if (last->type == MotionType::Ellipse)
                    infinite = true;

                // Path time: time to traverse the moving geometry at the entity's
                // CATALOG cruise speed (the physical model) -- NOT the segment's
                // stored speedMps, which "Set Duration" rescales to hit a manual
                // duration. Falls back to the segment's own speed only when the
                // type has no catalog cruise. Holds contribute no traversal time.
                double pathTime = 0.0;
                bool   havePath = false;
                for (const MotionSegment& s : e.motionSegments)
                {
                    if (!s.enabled) continue;
                    if (s.type != MotionType::Line && s.type != MotionType::Ellipse)
                        continue;
                    const double spd = (cruiseMps > 0.0) ? cruiseMps : s.speedMps;
                    if (spd > 0.0)
                    {
                        const double len = SegmentGeoLength(s, m_scenario);
                        if (len > 0.0) { pathTime += len / spd; havePath = true; }
                    }
                }
                if (havePath) pathStr.Format(_T("%.1f"), pathTime);
            }

            const int row = lc->InsertItem(lc->GetItemCount(), nm);
            lc->SetItemData(row, static_cast<DWORD_PTR>(i));   // row -> entity index
            lc->SetItemText(row, 1, durStr);
            lc->SetItemText(row, 2, pathStr);
            lc->SetItemText(row, 3, cruiseStr);
            lc->SetItemText(row, 4, infinite ? _T("Yes") : _T(""));
        }
    }
    lc->SetRedraw(TRUE);
    lc->Invalidate();
}

//
// SetEntityInitialEnu — write an entity's initial pose from scenario-origin ENU,
//   keeping its ECEF / Local / geodetic representations consistent.
//
void CPreviewPage::SetEntityInitialEnu(Entity& e, double east, double north,
                                       double up) const
{
    if (!m_scenario) return;
    double X = 0.0, Y = 0.0, Z = 0.0;
    CoordTransforms::LocalEnuToEcefDeg(
        east, north, up,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
        X, Y, Z);
    e.ecefX = X; e.ecefY = Y; e.ecefZ = Z;
    e.localX = east; e.localY = north; e.localZ = up;
    // Keep all three representations consistent so the Asset/Entity tab shows
    // the right numbers regardless of the entity's initialCoordMode.
    double lat = 0.0, lon = 0.0, alt = 0.0;
    CoordTransforms::EcefToGeodeticDeg(X, Y, Z, lat, lon, alt);
    e.lat = lat; e.lon = lon; e.alt = alt;
}

//
// SetSegmentStartEnu — write a segment's start point from scenario-origin ENU,
//   keeping its ECEF / Local / geodetic representations consistent.
//
void CPreviewPage::SetSegmentStartEnu(MotionSegment& s, double east, double north,
                                      double up) const
{
    if (!m_scenario) return;
    double X = 0.0, Y = 0.0, Z = 0.0;
    CoordTransforms::LocalEnuToEcefDeg(
        east, north, up,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
        X, Y, Z);
    s.startEcefX = X; s.startEcefY = Y; s.startEcefZ = Z;
    s.startLocalX = east; s.startLocalY = north; s.startLocalZ = up;
    double lat = 0.0, lon = 0.0, alt = 0.0;
    CoordTransforms::EcefToGeodeticDeg(X, Y, Z, lat, lon, alt);
    s.startLat = lat; s.startLon = lon; s.startAlt = alt;
}

//
// SetEllipseStartBearing — set a non-Local ellipse's start bearing so its start
//   point lands at (east,north), using the cached orbit center.
//
void CPreviewPage::SetEllipseStartBearing(size_t entityIdx, size_t segIdx,
                                          MotionSegment& s,
                                          double east, double north) const
{
    double centerE = 0.0, centerN = 0.0;
    bool found = false;
    for (const PreviewEllipse& t : m_ellipseTargets)
        if (t.entityIdx == entityIdx && t.segIdx == segIdx)
        {
            centerE = t.centerE; centerN = t.centerN; found = true; break;
        }
    if (!found) return;

    const double vE = east - centerE;
    const double vN = north - centerN;
    double brg = std::atan2(vE, vN) * (180.0 / 3.14159265358979323846);
    if (brg < 0.0) brg += 360.0;
    s.startBearingDeg = brg;
}

//
// UpdateSliderFromTime — reflect the current preview time on the slider without
//   re-triggering the scroll handler.
//
void CPreviewPage::UpdateSliderFromTime()
{
    if (CSliderCtrl* s = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME))
    {
        m_suppressScroll = true;
        s->SetPos(static_cast<int>(m_previewTimeSec * 10.0 + 0.5));
        m_suppressScroll = false;
    }
}

void CPreviewPage::RebuildPathsCache()
{
    m_pathsCache.clear();
    m_ellipseTargets.clear();
    // Impact legs end on a target that may have been re-routed since: refresh
    // their stored end triples (derived data; not an edit, nothing marked dirty).
    SyncImpactEnds();
    // Painted foliage areas -> ENU for drawing. Projecting all four corners and
    // taking the bounding box keeps the rectangle honest under any origin skew,
    // the same way the boundary paint does in reverse.
    m_state.foliageAreas.clear();
    if (m_scenario)
    {
        m_state.foliageAreas.reserve(m_scenario->foliageAreas.size());
        for (const FoliageArea& a : m_scenario->foliageAreas)
        {
            const double corners[4][2] = {
                { a.latMinDeg, a.lonMinDeg }, { a.latMinDeg, a.lonMaxDeg },
                { a.latMaxDeg, a.lonMinDeg }, { a.latMaxDeg, a.lonMaxDeg },
            };
            double eMin = 1e18, eMax = -1e18, nMin = 1e18, nMax = -1e18;
            for (int i = 0; i < 4; ++i)
            {
                double X = 0, Y = 0, Z = 0, e = 0, nn = 0, u = 0;
                CoordTransforms::GeodeticToEcefDeg(corners[i][0], corners[i][1], 0.0, X, Y, Z);
                CoordTransforms::EcefToLocalEnuDeg(X, Y, Z,
                    m_state.originLatDeg, m_state.originLonDeg, m_state.originAltM, e, nn, u);
                if (e  < eMin) eMin = e;   if (e  > eMax) eMax = e;
                if (nn < nMin) nMin = nn;  if (nn > nMax) nMax = nn;
            }
            PreviewFoliageArea pa;
            pa.eastMinM  = eMin;
            pa.eastMaxM  = eMax;
            pa.northMinM = nMin;
            pa.northMaxM = nMax;
            pa.treeCount = a.treeCount;
            m_state.foliageAreas.push_back(pa);
        }
    }

    if (!m_scenario) return;

    const size_t n = m_scenario->entities.size();
    m_pathsCache.resize(n);

    // Per-segment-type sample counts. Ellipse needs the most points so the
    // curve looks smooth; Line is a straight chord so 2 points suffice
    // (extra points are harmless). Stationary / StopHold contribute one
    // anchor point (the entity's resting position).
    constexpr size_t kLinePts         = 2;
    constexpr size_t kEllipsePts      = 96;
    constexpr size_t kStaticPts       = 1;

    for (size_t i = 0; i < n; ++i)
    {
        const Entity& e = m_scenario->entities[i];
        if (!e.enabled || e.motionSegments.empty()) continue;

        auto enuOf = [&](const SampledPose& pose) -> D2D1_POINT_2F {
            double east, north, up;
            CoordTransforms::EcefToLocalEnuDeg(
                pose.ecefX, pose.ecefY, pose.ecefZ,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
                east, north, up);
            return D2D1::Point2F(static_cast<float>(east), static_cast<float>(north));
        };

        for (size_t segIdx = 0; segIdx < e.motionSegments.size(); ++segIdx)
        {
            const MotionSegment& s = e.motionSegments[segIdx];
            if (!s.enabled) continue;

            // Entity Ellipse: only ONE loop is shown at a time, re-centered on
            // the target's CURRENT position, so it's drawn live per preview frame
            // in RebuildRenderState (see AppendFollowEllipsePath) rather than
            // cached here as a static multi-loop trace.
            if (s.type == MotionType::Ellipse && s.followEntityId >= 0) continue;

            size_t nPts = kStaticPts;
            if (s.type == MotionType::Line)    nPts = kLinePts;
            if (s.type == MotionType::Ellipse) nPts = kEllipsePts;

            // For ellipse segments, collect this segment's points separately
            // so the canvas can hit-test individual orbits in "Set Start".
            PreviewEllipse target;
            const bool isEllipse = (s.type == MotionType::Ellipse);
            if (isEllipse) { target.entityIdx = i; target.segIdx = segIdx; }

            for (size_t k = 0; k < nPts; ++k)
            {
                const double u = (nPts <= 1)
                                  ? 0.0
                                  : static_cast<double>(k) / static_cast<double>(nPts - 1);
                // geometricMode=true: u ∈ [0,1] traces one full cycle of
                // the segment's shape (line start→end, or ONE ellipse
                // revolution), regardless of how many real orbits the
                // segment's time window contains. This kills the chord-
                // overlay "thick ring" artifact for many-orbit ellipses.
                const SampledPose pose = MotionSampler::EvaluateSegment(
                    s, u, m_scenario, /*geometricMode*/ true);
                const D2D1_POINT_2F pt = enuOf(pose);
                m_pathsCache[i].push_back(pt);
                if (isEllipse) target.enuPts.push_back(pt);
            }

            if (isEllipse && target.enuPts.size() >= 2)
            {
                // Ellipse center = foci midpoint, resolved to scenario-origin
                // ENU (mirrors the sampler's coordMode handling). Used to
                // invert a clicked orbit point into a start bearing.
                double f1E, f1N, f2E, f2N;
                FocusToEnu(s, /*focus2*/ false, f1E, f1N);
                FocusToEnu(s, /*focus2*/ true,  f2E, f2N);
                target.centerE = 0.5 * (f1E + f2E);
                target.centerN = 0.5 * (f1N + f2N);
                m_ellipseTargets.push_back(std::move(target));
            }
        }
    }
}

//
// FocusToEnu — resolve one ellipse focus (f1/f2, per coordMode) to scenario-origin
//   ENU east/north.
//
void CPreviewPage::FocusToEnu(const MotionSegment& s, bool focus2,
                              double& outE, double& outN) const
{
    outE = outN = 0.0;
    if (!m_scenario) return;

    double X = 0.0, Y = 0.0, Z = 0.0;
    switch (s.coordMode)
    {
    case CoordMode::Local:
        CoordTransforms::LocalEnuToEcefDeg(
            focus2 ? s.f2LocalX : s.f1LocalX,
            focus2 ? s.f2LocalY : s.f1LocalY,
            focus2 ? s.f2LocalZ : s.f1LocalZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            X, Y, Z);
        break;
    case CoordMode::ECEF:
        X = focus2 ? s.f2EcefX : s.f1EcefX;
        Y = focus2 ? s.f2EcefY : s.f1EcefY;
        Z = focus2 ? s.f2EcefZ : s.f1EcefZ;
        break;
    default: // LatLonAlt
        CoordTransforms::GeodeticToEcefDeg(
            focus2 ? s.f2Lat : s.f1Lat,
            focus2 ? s.f2Lon : s.f1Lon,
            focus2 ? s.f2Alt : s.f1Alt,
            X, Y, Z);
        break;
    }

    double up = 0.0;
    CoordTransforms::EcefToLocalEnuDeg(
        X, Y, Z,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
        outE, outN, up);
}

//
// FindEntityIndexById — index of the entity with matching entityId, or -1.
//
int CPreviewPage::FindEntityIndexById(int entityId) const
{
    if (!m_scenario || entityId < 0) return -1;
    for (size_t j = 0; j < m_scenario->entities.size(); ++j)
        if (static_cast<int>(m_scenario->entities[j].entityId) == entityId)
            return static_cast<int>(j);
    return -1;
}

//
// FollowCenterOffset — ENU delta from a follow-ellipse's stored foci midpoint to
//   the target's current position; false unless it's a follow ellipse with a
//   live target.
//
bool CPreviewPage::FollowCenterOffset(const MotionSegment& s,
                                      double& outDE, double& outDN) const
{
    outDE = outDN = 0.0;
    if (!m_scenario) return false;
    if (s.type != MotionType::Ellipse || s.followEntityId < 0) return false;
    const int ti = FindEntityIndexById(s.followEntityId);
    if (ti < 0 || static_cast<size_t>(ti) >= m_scenario->entities.size()) return false;
    const Entity& target = m_scenario->entities[static_cast<size_t>(ti)];
    if (!target.enabled) return false;

    // Anchor on the target at the HANDOFF time (clamped into the segment) -- the
    // SAME instant AppendFollowEllipsePath re-centers the drawn orbit. Using the
    // raw scrub time here instead left the focus handles floating off the drawn
    // orbit while authoring, so you couldn't grab them to resize it.
    const double tRecenter =
        std::min(std::max(m_previewTimeSec, s.startSecond), s.endSecond);
    const SampledPose tp = MotionSampler::SamplePose(target, *m_scenario, tRecenter);
    double te, tn, tu;
    CoordTransforms::EcefToLocalEnuDeg(
        tp.ecefX, tp.ecefY, tp.ecefZ,
        m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM, te, tn, tu);

    double f1E, f1N, f2E, f2N;
    FocusToEnu(s, /*focus2*/ false, f1E, f1N);
    FocusToEnu(s, /*focus2*/ true,  f2E, f2N);
    const double midE = 0.5 * (f1E + f2E), midN = 0.5 * (f1N + f2N);
    outDE = te - midE;
    outDN = tn - midN;
    return true;
}

void CPreviewPage::AppendFollowEllipsePath(size_t entityIdx, const MotionSegment& s)
{
    if (!m_scenario) return;
    if (s.type != MotionType::Ellipse || s.followEntityId < 0) return;
    if (entityIdx >= m_state.paths.size()) return;

    const int ti = FindEntityIndexById(s.followEntityId);
    if (ti < 0) return;
    const Entity& target = m_scenario->entities[static_cast<size_t>(ti)];

    // Re-center the loop on the target's position, but CLAMP the sample time into
    // this segment's window. Before the handoff (authoring at t=0, or scrubbed onto
    // an earlier leg) we must anchor on the target at the HANDOFF time
    // (s.startSecond == prev.endSecond) -- that's the instant SnapEllipse sized the
    // orbit to pass through & start at the line end. Sampling the target at the raw
    // scrub time instead drew the orbit off the line end, so the entity's polyline
    // rendered a phantom connector ("second line") over to a mis-placed orbit whose
    // foci handles no longer matched (making it un-resizable). During the orbit
    // (startSecond..endSecond) it still follows the target live.
    const double tRecenter =
        std::min(std::max(m_previewTimeSec, s.startSecond), s.endSecond);
    const SampledPose tp = MotionSampler::SamplePose(target, *m_scenario, tRecenter);
    const double ov[3] = { tp.ecefX, tp.ecefY, tp.ecefZ };
    const MotionSampler::EllipseFrame f =
        MotionSampler::BuildEllipseFrame(s, m_scenario, /*withArcTable*/ false, ov);
    if (!f.valid) return;

    constexpr double kTau = 6.28318530717958647692;
    constexpr size_t kLoopPts = 96;
    auto& path = m_state.paths[entityIdx];
    for (size_t k = 0; k < kLoopPts; ++k)
    {
        const double u     = static_cast<double>(k) / static_cast<double>(kLoopPts - 1);
        const double theta = f.theta0 + f.sign * kTau * u;   // one full revolution
        const MotionSampler::EllipsePoint p = MotionSampler::EvalEllipseAtTheta(f, theta);
        double east, north, up;
        CoordTransforms::EcefToLocalEnuDeg(
            p.ecefX, p.ecefY, p.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            east, north, up);
        path.push_back(D2D1::Point2F(static_cast<float>(east), static_cast<float>(north)));
    }
}

//
// FitScenario — compute zoom + center so every entity path (and the terrain
//   overlay) fits the canvas with ~10% padding.
//
void CPreviewPage::FitScenario()
{
    if (!m_scenario || m_scenario->entities.empty())
    {
        m_zoomMetersPerPx = kFreshViewMetersPerPx;   // fresh/empty: frame the origin
        m_centerEnuE = m_centerEnuN = 0.0;
        return;
    }

    double minE =  1e18, minN =  1e18;
    double maxE = -1e18, maxN = -1e18;
    const double dur = std::max(1.0, EffectivePreviewDuration());

    for (const Entity& e : m_scenario->entities)
    {
        if (!e.enabled) continue;
        for (size_t k = 0; k < kFitSampleCount; ++k)
        {
            const double t = dur * static_cast<double>(k)
                           / static_cast<double>(kFitSampleCount - 1);
            const SampledPose pose = MotionSampler::SamplePose(e, *m_scenario, t);
            double east, north, up;
            CoordTransforms::EcefToLocalEnuDeg(
                pose.ecefX, pose.ecefY, pose.ecefZ,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
                east, north, up);
            // Skip positions absurdly far from the origin (phantom default entity /
            // relocated-away scenario) so the fit doesn't zoom into a phantom point.
            if (std::hypot(east, north) > kFitMaxOffsetM) continue;
            if (east < minE) minE = east;
            if (east > maxE) maxE = east;
            if (north < minN) minN = north;
            if (north > maxN) maxN = north;
        }
    }

    // Fold in the terrain overlay so Fit frames the land/ocean too (and so a
    // scenario whose only entity sits at the origin still shows the level).
    if (m_scenario->level.enabled)
    {
        auto foldRect = [&](const LevelRect& r)
        {
            if (!r.enabled) return;
            minE = std::min(minE, std::min(r.eastMinM,  r.eastMaxM));
            maxE = std::max(maxE, std::max(r.eastMinM,  r.eastMaxM));
            minN = std::min(minN, std::min(r.northMinM, r.northMaxM));
            maxN = std::max(maxN, std::max(r.northMinM, r.northMaxM));
        };
        foldRect(m_scenario->level.land);
        foldRect(m_scenario->level.ocean);
        for (const LevelZone& z : m_scenario->level.zones)
        {
            if (!z.enabled) continue;
            minE = std::min(minE, std::min(z.eastMinM,  z.eastMaxM));
            maxE = std::max(maxE, std::max(z.eastMinM,  z.eastMaxM));
            minN = std::min(minN, std::min(z.northMinM, z.northMaxM));
            maxN = std::max(maxN, std::max(z.northMinM, z.northMaxM));
        }
    }

    if (minE > maxE || minN > maxN)
    {
        // Every entity was skipped as far-flung (phantom / relocated-away origin):
        // frame the origin at the comfortable default so the map still shows.
        m_zoomMetersPerPx = kFreshViewMetersPerPx;
        m_centerEnuE = m_centerEnuN = 0.0;
        return;
    }

    // 10% padding around the bounding box. A degenerate box (one stationary
    // entity, or a formation parked on a single point) used to fit to the
    // 0.01 m/px floor -- a 25 m wide canvas in which the entity dot filled the
    // screen and a freshly plotted 2 km starter course ran straight off it.
    // Frame at least kFitMinExtentM so a lone entity gets some map around it
    // and Plot > Line's starter segment lands on screen.
    double boxW = std::max(kFitMinExtentM, (maxE - minE)) * 1.10;
    double boxH = std::max(kFitMinExtentM, (maxN - minN)) * 1.10;
    m_centerEnuE = 0.5 * (minE + maxE);
    m_centerEnuN = 0.5 * (minN + maxN);

    CRect rc;
    if (m_canvas.GetSafeHwnd()) m_canvas.GetClientRect(&rc);
    const double w = std::max<LONG>(rc.Width(),  600);
    const double h = std::max<LONG>(rc.Height(), 400);

    m_zoomMetersPerPx = std::max(boxW / w, boxH / h);
    if (m_zoomMetersPerPx < 0.01) m_zoomMetersPerPx = 0.01;

    {
        char buf[400];
        sprintf_s(buf, sizeof(buf),
                  "FitScenario: origin=(%.4f,%.4f,%.1f) bbox E=[%.1f..%.1f] N=[%.1f..%.1f] "
                  "centerENU=(%.1f,%.1f) zoom=%.3f m/px canvas=%.0fx%.0f",
                  m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
                  minE, maxE, minN, maxN,
                  m_centerEnuE, m_centerEnuN, m_zoomMetersPerPx, w, h);
        LOG(buf);
    }
}

void CPreviewPage::RebuildRenderState()
{
    m_state.poses.clear();
    m_state.paths.clear();
    m_state.trails.clear();
    m_state.ellipseStarts.clear();
    m_state.cameras.clear();

    m_state.zoomMetersPerPx = m_zoomMetersPerPx;
    m_state.centerEnuE      = m_centerEnuE;
    m_state.centerEnuN      = m_centerEnuN;
    m_state.previewTimeSec  = m_previewTimeSec;
    m_state.durationSec     = m_scenario ? EffectivePreviewDuration() : 0.0;
    m_state.showLabels      = m_showLabels;
    m_state.showTrails      = m_showTrails;
    m_state.showPaths       = m_showPaths;
    m_state.showOrientation = m_showOrientation;
    m_state.showLegend      = m_showLegend;
    m_state.showOrigin      = m_showOrigin;

    // Map backdrop: layer + online flag from the page; geographic anchor from
    // the scenario origin so tiles line up with the ENU entity positions.
    m_state.mapLayer     = m_mapLayer;
    m_state.mapOnline    = m_mapOnline;
    m_state.originLatDeg = m_scenario ? m_scenario->originLatDeg : 0.0;
    m_state.originLonDeg = m_scenario ? m_scenario->originLonDeg : 0.0;
    m_state.originAltM   = m_scenario ? m_scenario->originAltM   : 0.0;

    // Terrain overlay: copy the scenario's land/ocean footprints into the
    // render state (only drawn when the [Level] section enabled them).
    m_state.showLevel  = m_showTerrain && m_scenario && m_scenario->level.enabled;
    m_state.showZones  = m_showZones   && m_scenario && m_scenario->level.enabled;
    m_state.levelLand  = {};
    m_state.levelOcean = {};
    if (m_scenario)
    {
        auto copyRect = [](const LevelRect& src, PreviewLevelRect& dst)
        {
            dst.enabled   = src.enabled;
            dst.eastMinM  = src.eastMinM;
            dst.eastMaxM  = src.eastMaxM;
            dst.northMinM = src.northMinM;
            dst.northMaxM = src.northMaxM;
        };
        copyRect(m_scenario->level.land,  m_state.levelLand);
        copyRect(m_scenario->level.ocean, m_state.levelOcean);

        m_state.levelZones.clear();
        m_state.levelZones.reserve(m_scenario->level.zones.size());
        for (const LevelZone& z : m_scenario->level.zones)
        {
            PreviewLevelZone pz;
            pz.enabled   = z.enabled;
            pz.name      = z.name;
            pz.eastMinM  = z.eastMinM;
            pz.eastMaxM  = z.eastMaxM;
            pz.northMinM = z.northMinM;
            pz.northMaxM = z.northMaxM;
            pz.colorRgb  = z.colorRgb;
            m_state.levelZones.push_back(std::move(pz));
        }
    }
    else
    {
        m_state.levelZones.clear();
    }

    if (!m_scenario) return;

    const size_t n = m_scenario->entities.size();
    if (m_trails.size() != n) m_trails.assign(n, {});

    const bool advanceTrail = (m_runState == PreviewState::Playing)
                            && (m_lastTrailTime < 0.0
                                || std::fabs(m_previewTimeSec - m_lastTrailTime) > 0.0);

    for (size_t i = 0; i < n; ++i)
    {
        const Entity& e = m_scenario->entities[i];
        if (!e.enabled) { PreviewEntityPose off; off.enabled = false; off.entityId = e.entityId; m_state.poses.push_back(off); continue; }

        const SampledPose pose = MotionSampler::SamplePose(e, *m_scenario, m_previewTimeSec);
        double east, north, up;
        CoordTransforms::EcefToLocalEnuDeg(
            pose.ecefX, pose.ecefY, pose.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            east, north, up);

        PreviewEntityPose ep;
        ep.enuE       = east;
        ep.enuN       = north;
        ep.enuU       = up;
        ep.headingDeg = pose.headingDeg;
        ep.forceId    = e.forceId;
        ep.entityId   = e.entityId;
        ep.name       = e.marking.empty() ? e.name : e.marking;
        if (const MotionSegment* imp = MotionSampler::TerminalImpactSegment(e))
            ep.exploded = MotionSampler::ImpactTargetIndex(*imp, *m_scenario) >= 0 &&
                          m_previewTimeSec >= imp->endSecond;
        m_state.poses.push_back(std::move(ep));

        if (advanceTrail)
        {
            auto& tr = m_trails[i];
            tr.push_back(D2D1::Point2F(static_cast<float>(east),
                                       static_cast<float>(north)));
            if (tr.size() > kTrailMaxPoints)
                tr.erase(tr.begin(), tr.begin() + (tr.size() - kTrailMaxPoints));
        }
    }

    m_lastTrailTime = m_previewTimeSec;

    // DIAGNOSTIC readout: the (primary) selected entity's live ENU position and
    // straight-line distance from the origin, plus its lat/lon (ENU->geodetic).
    // Click an entity and read exactly where the editor thinks it is — this is the
    // definitive check for "on the runway vs. 28 km out".
    m_state.selectedReadout.clear();
    if (!m_selected.empty())
    {
        const size_t si = *m_selected.begin();
        if (si < m_state.poses.size() && m_state.poses[si].enabled)
        {
            const PreviewEntityPose& p = m_state.poses[si];
            const double distKm = std::sqrt(p.enuE * p.enuE + p.enuN * p.enuN) / 1000.0;
            double X, Y, Z, lat, lon, alt;
            CoordTransforms::LocalEnuToEcefDeg(p.enuE, p.enuN, p.enuU,
                m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM, X, Y, Z);
            CoordTransforms::EcefToGeodeticDeg(X, Y, Z, lat, lon, alt);
            const CA2W wname(p.name.c_str());
            wchar_t buf[256];
            swprintf_s(buf,
                L"%s   E=%.1f  N=%.1f m   %.2f km from origin   (lat %.6f, lon %.6f)%s",
                wname.m_psz ? wname.m_psz : L"(entity)",
                p.enuE, p.enuN, distKm, lat, lon,
                m_selected.size() > 1 ? L"   [+more]" : L"");
            m_state.selectedReadout = buf;
        }
    }

    m_state.paths  = m_pathsCache;
    m_state.trails = m_trails;

    // Entity Ellipse: draw only the CURRENT loop live (re-centered on the target
    // now), appended to each follow-entity's static path. One ellipse at a time,
    // adjusting as the entity traverses and the target moves.
    for (size_t ei = 0; ei < m_scenario->entities.size(); ++ei)
    {
        const Entity& en = m_scenario->entities[ei];
        if (!en.enabled) continue;
        for (const MotionSegment& sg : en.motionSegments)
            if (sg.enabled && sg.type == MotionType::Ellipse && sg.followEntityId >= 0)
                AppendFollowEllipsePath(ei, sg);
    }

    // Pickable orbits for the canvas's "Set Start" hit-test.
    m_state.ellipses = m_ellipseTargets;

    // Start markers: geometric u=0 (each ellipse target's first point) is the
    // entity's start on that orbit.
    for (const PreviewEllipse& t : m_ellipseTargets)
        if (!t.enuPts.empty())
            m_state.ellipseStarts.push_back(t.enuPts.front());

    // Draggable end-anchors ("pink dots") for Local Line segments — the course
    // destinations. (PlotLineCourse always makes Local Lines.)
    m_state.lineAnchors.clear();
    for (size_t ei = 0; ei < m_scenario->entities.size(); ++ei)
    {
        const Entity& en = m_scenario->entities[ei];
        for (size_t si = 0; si < en.motionSegments.size(); ++si)
        {
            const MotionSegment& sg = en.motionSegments[si];
            if (sg.type != MotionType::Line || sg.coordMode != CoordMode::Local) continue;
            if (sg.impactEntityId >= 0) continue;   // ends on its target: the red star instead
            PreviewLineAnchor a;
            a.entityIdx = ei;
            a.segIdx    = si;
            a.enuE      = sg.endLocalX;
            a.enuN      = sg.endLocalY;
            m_state.lineAnchors.push_back(a);
        }
    }

    // Terminal explosions: the red star at each impact point, over the target's
    // footprint at the impact instant (see PreviewImpactMarker).
    m_state.impacts.clear();
    for (size_t ei = 0; ei < m_scenario->entities.size(); ++ei)
    {
        const Entity& en = m_scenario->entities[ei];
        if (!en.enabled) continue;
        const MotionSegment* imp = MotionSampler::TerminalImpactSegment(en);
        if (!imp) continue;
        const int ti = MotionSampler::ImpactTargetIndex(*imp, *m_scenario);
        double p[3];
        if (ti < 0 || !MotionSampler::ImpactPointEcef(*imp, *m_scenario, p)) continue;

        PreviewImpactMarker m;
        m.entityIdx = ei;
        m.segIdx    = static_cast<size_t>(imp - en.motionSegments.data());
        double u = 0.0;
        CoordTransforms::EcefToLocalEnuDeg(p[0], p[1], p[2],
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            m.enuE, m.enuN, u);
        const SampledPose tp = MotionSampler::SamplePose(
            m_scenario->entities[static_cast<size_t>(ti)], *m_scenario, imp->endSecond);
        CoordTransforms::EcefToLocalEnuDeg(tp.ecefX, tp.ecefY, tp.ecefZ,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            m.targetE, m.targetN, u);
        m.headingDeg = tp.headingDeg;
        EntityFootprint(static_cast<size_t>(ti), m.lengthM, m.widthM);
        m.detonated  = m_previewTimeSec >= imp->endSecond;
        m_state.impacts.push_back(m);
    }

    // Draggable focus handles for Local Ellipse segments (F1 then F2 per orbit,
    // so the canvas can draw the connecting major-axis line).
    m_state.focusHandles.clear();
    for (size_t ei = 0; ei < m_scenario->entities.size(); ++ei)
    {
        const Entity& en = m_scenario->entities[ei];
        for (size_t si = 0; si < en.motionSegments.size(); ++si)
        {
            const MotionSegment& sg = en.motionSegments[si];
            if (sg.type != MotionType::Ellipse || sg.coordMode != CoordMode::Local) continue;
            // Entity Ellipse: shift the editable handles onto the target's
            // current position (preserving their relative geometry) so they
            // render around the moving center, not the ignored stored midpoint.
            double dE = 0.0, dN = 0.0;
            FollowCenterOffset(sg, dE, dN);
            for (int fi = 0; fi < 2; ++fi)
            {
                double fe = 0.0, fn = 0.0;
                FocusToEnu(sg, fi == 1, fe, fn);
                fe += dE; fn += dN;
                PreviewFocusHandle h;
                h.entityIdx = ei; h.segIdx = si; h.focusIdx = fi;
                h.enuE = fe; h.enuN = fn;
                m_state.focusHandles.push_back(h);
            }
        }
    }

    // Camera vantage boxes. Stationary cameras sit at their authored ENU vantage;
    // Entity cameras are pinned to their source entity's current sampled pose
    // (skipped when the source is missing/disabled).
    for (size_t ci = 0; ci < m_scenario->cameras.size(); ++ci)
    {
        const CameraFrame& c = m_scenario->cameras[ci];
        PreviewCameraBox box;
        box.frameIdx = ci;
        box.label    = c.label;
        if (c.kind == CameraKind::Stationary)
        {
            box.stationary = true;
            box.enuE = c.vantEastM;
            box.enuN = c.vantNorthM;
        }
        else
        {
            const int si = FindEntityIndexById(c.sourceEntityId);
            if (si < 0 || static_cast<size_t>(si) >= m_state.poses.size() ||
                !m_state.poses[si].enabled)
                continue;   // source gone/disabled -> don't draw
            box.stationary = false;
            box.enuE = m_state.poses[si].enuE;
            box.enuN = m_state.poses[si].enuN;
        }
        m_state.cameras.push_back(std::move(box));
    }

    // Group-selection flags (parallel to poses/entities).
    m_state.selected.assign(m_scenario->entities.size(), false);
    for (size_t si : m_selected)
        if (si < m_state.selected.size()) m_state.selected[si] = true;

    // Red flag for selected entities whose last Refresh Speed found no catalog cruise.
    m_state.selectError.assign(m_scenario->entities.size(), false);
    for (size_t si : m_refreshFailed)
        if (si < m_state.selectError.size()) m_state.selectError[si] = true;

    // Keep the destination-time grid in sync when shown. Skip during playback —
    // these projected times don't change and a 30 Hz repopulate would churn.
    // Also skip when the rebuild was triggered BY a grid-row selection: tearing
    // the list down (DeleteAllItems) from inside its own LVN_ITEMCHANGED handler
    // corrupts the control and leaves it dead to further clicks.
    if (m_showDestTime && m_runState != PreviewState::Playing && !m_suppressPropsRefresh)
        RefreshDestTimeList();

    // Keep the 3D globe's entity positions in sync (no-op unless Cesium is active).
    PushCesiumEntities();
}
