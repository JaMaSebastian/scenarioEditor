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
#include "Scenario.h"
#include "MotionSampler.h"
#include "CoordTransforms.h"
#include "ScenarioWorker.h"   // WM_APP_REFRESH_UI
#include "ScenarioEditor.h"   // theApp.Catalog() — per-type cruise speed
#include "EntityTypeCatalog.h"      // CatalogEntry / AirframeProfile
#include "EntityTypePickerDialog.h" // "Change Entity" catalog tree picker
#include "../log.h"

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
    // True if something is listening on 127.0.0.1:<port> (WSL2 forwards localhost),
    // used to detect the self-hosted Cesium terrain server (python3 serve.py :8088).
    // Non-blocking connect with a short timeout so the UI poll never stalls.
    bool ProbeTcpPort(const char* host, unsigned short port, int timeoutMs)
    {
        static bool wsaUp = false;
        if (!wsaUp)
        {
            WSADATA wsa{};
            if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
            wsaUp = true;   // process-lifetime; cleaned up at exit
        }

        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;

        u_long nonBlocking = 1;
        ::ioctlsocket(s, FIONBIO, &nonBlocking);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = ::htons(port);
        ::InetPtonA(AF_INET, host, &addr.sin_addr);

        bool ok = false;
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
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
        { IDC_CHK_PREVIEW_TERRAIN,     _T("Show the level's land/ocean footprint (when the scenario defines one)."), false },
        { IDC_CHK_PREVIEW_LEGEND,      _T("Show size (per-area edge distances) and N/S/E/W direction labels."), false },
        { IDC_BTN_PREVIEW_SET_START,   _T("Click, then click an ellipse to set where that entity starts."), false },
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

    // Sample N evenly-spaced points along [0, duration] for FitScenario
    // and the per-entity motion-path cache.
    constexpr size_t kFitSampleCount  = 32;
    // (path sampling now lives per-segment inside RebuildPathsCache —
    // see kLinePts / kEllipsePts there)
}

BEGIN_MESSAGE_MAP(CPreviewPage, CHelpAwarePage)
    ON_WM_CTLCOLOR()
    ON_WM_DESTROY()
    ON_WM_TIMER()
    ON_WM_HSCROLL()
    ON_WM_SHOWWINDOW()
    ON_BN_CLICKED(IDC_BTN_PREVIEW_START,       &CPreviewPage::OnStart)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_PAUSE,       &CPreviewPage::OnPause)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_RESUME,      &CPreviewPage::OnResume)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_STOP,        &CPreviewPage::OnStop)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_ZOOM_IN,     &CPreviewPage::OnZoomIn)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_ZOOM_OUT,    &CPreviewPage::OnZoomOut)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_FIT,         &CPreviewPage::OnFit)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_NEW,         &CPreviewPage::OnNewEntity)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_LABELS,      &CPreviewPage::OnLabelsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_TRAILS,      &CPreviewPage::OnTrailsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_PATHS,       &CPreviewPage::OnPathsToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_ORIENTATION, &CPreviewPage::OnOrientationToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_TERRAIN,     &CPreviewPage::OnTerrainToggle)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_LEGEND,      &CPreviewPage::OnLegendToggle)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_SET_START,   &CPreviewPage::OnSetStart)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_BOUNDARY,    &CPreviewPage::OnBoundary)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_BUILD_TERRAIN, &CPreviewPage::OnBuildTerrain)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_TERRAIN_START, &CPreviewPage::OnStartTerrainServer)
    ON_BN_CLICKED(IDC_BTN_PREVIEW_TERRAIN_STOP,  &CPreviewPage::OnStopTerrainServer)
    ON_CBN_SELCHANGE(IDC_COMBO_PREVIEW_SPEED,  &CPreviewPage::OnSpeedChange)
    ON_BN_CLICKED(IDC_CHK_PREVIEW_DESTTIME,    &CPreviewPage::OnDestTimeToggle)
    ON_CBN_SELCHANGE(IDC_COMBO_MAP_LAYER,      &CPreviewPage::OnMapLayerChange)
    ON_CBN_SELCHANGE(IDC_COMBO_MAP_PLACE,      &CPreviewPage::OnMapPlaceChange)
    ON_BN_CLICKED(IDC_BTN_MAP_ADDPLACE,        &CPreviewPage::OnMapAddPlace)
    ON_BN_CLICKED(IDC_BTN_MAP_DELPLACE,        &CPreviewPage::OnMapDelPlace)
    ON_BN_CLICKED(IDC_BTN_MAP_CAPTURE,         &CPreviewPage::OnMapCapturePlace)
    ON_BN_CLICKED(IDC_CHK_MAP_MOVE_ENTITIES,   &CPreviewPage::OnMapMoveEntitiesToggle)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST_PREVIEW_DESTTIME, &CPreviewPage::OnPropertiesItemChanged)
    ON_WM_DRAWITEM()
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

    CheckDlgButton(IDC_CHK_PREVIEW_LABELS,      m_showLabels      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_TRAILS,      m_showTrails      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_PATHS,       m_showPaths       ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_ORIENTATION, m_showOrientation ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_TERRAIN,     m_showTerrain     ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_LEGEND,      m_showLegend      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_MAP_MOVE_ENTITIES,   m_moveEntitiesWithMap ? BST_CHECKED : BST_UNCHECKED);

    if (CComboBox* spd = (CComboBox*)GetDlgItem(IDC_COMBO_PREVIEW_SPEED))
    {
        spd->ResetContent();
        // NOTE: keep in sync with kMul[] in OnSpeedChange(). Large multipliers
        // exist for wide-area scenarios where a single lap/leg spans tens of km.
        const wchar_t* kSpeeds[] = { L"1x", L"2x", L"5x", L"10x", L"30x", L"60x",
                                     L"120x", L"300x", L"600x", L"1200x" };
        for (const wchar_t* s : kSpeeds) spd->AddString(s);
        spd->SetCurSel(3);   // default 10x so entity motion is visible
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
        dt->ShowWindow(SW_HIDE);   // revealed when "Show Properties" is checked
    }
    CheckDlgButton(IDC_CHK_PREVIEW_DESTTIME, m_showDestTime ? BST_CHECKED : BST_UNCHECKED);

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

    UpdateSliderRange();
    if (CSliderCtrl* s = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME))
        s->SetPos(0);

    RebuildPathsCache();
    FitScenario();
    RebuildRenderState();

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
        RebuildPathsCache();
        FitScenario();
        RebuildRenderState();
        if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
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
        if (!p.enabled) continue;
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
    ApplyMapPlace(mp.lat, mp.lon, mp.alt > 0.0 ? mp.alt : -1.0);
}

//
// OnMapAddPlace — "Add Location": prompt (prefilled from the current view),
//   append the new MapPlace, persist Settings, then fly there.
//
void CPreviewPage::OnMapAddPlace()
{
    const double lat = m_scenario ? m_scenario->originLatDeg : 0.0;
    const double lon = m_scenario ? m_scenario->originLonDeg : 0.0;
    CAddPlaceDialog dlg(lat, lon, CurrentViewAltitude(), this);
    if (dlg.DoModal() != IDOK) return;

    MapPlace p;
    p.label = std::string(CStringA(dlg.m_label).GetString());
    p.lat   = dlg.m_lat;
    p.lon   = dlg.m_lon;
    p.alt   = dlg.m_alt;
    theApp.Settings().mapPlaces.push_back(p);
    SettingsIO::Save(theApp.Settings(), theApp.SettingsPath());

    PopulatePlacesCombo(static_cast<int>(theApp.Settings().mapPlaces.size()) - 1);
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
}

void CPreviewPage::OnMapCapturePlace()
{
    // "Current location" = wherever the map view is currently centered:
    //   - Cesium (3D): the point the globe is centered on (where the user flew).
    //   - 2D layers:   the lat/lon under the center of the panned/zoomed canvas.
    // Falls back to the scenario origin only when nothing better is available.
    // Seeds the Add Place dialog so the captured lat/lon can be named and saved;
    // this only bookmarks the spot -- it doesn't relocate the scenario (pick it
    // from the Location list to do that).
    double lat = m_scenario ? m_scenario->originLatDeg : 0.0;
    double lon = m_scenario ? m_scenario->originLonDeg : 0.0;
    double alt = CurrentViewAltitude();   // eye/viewing height
    if (m_mapLayer == MapLayer::Cesium)
    {
        // Use the globe's reported center + eye altitude; don't silently fall
        // back to the origin (that would present the wrong point as the view).
        if (!m_cesium.GetLookAt(lat, lon, alt))
        {
            AfxMessageBox(_T("The 3D globe hasn't reported its position yet. ")
                          _T("Rotate or zoom the globe once, then click Capture."));
            return;
        }
    }
    else if (m_scenario)
    {
        // m_centerEnuE/N are the ENU metres (East/North) from the scenario origin
        // at the middle of the view; resolve them back to geodetic. Altitude is
        // the 2D view's eye-height equivalent (from zoom), computed above.
        double X = 0.0, Y = 0.0, Z = 0.0;
        CoordTransforms::LocalEnuToEcefDeg(
            m_centerEnuE, m_centerEnuN, 0.0,
            m_scenario->originLatDeg, m_scenario->originLonDeg, m_scenario->originAltM,
            X, Y, Z);
        double ga = 0.0;
        CoordTransforms::EcefToGeodeticDeg(X, Y, Z, lat, lon, ga);
    }

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
}

//
// OnMapMoveEntitiesToggle — remember whether a Location change should drag the
//   entities along (vs. leaving them world-fixed).
//
void CPreviewPage::OnMapMoveEntitiesToggle()
{
    if (CButton* b = (CButton*)GetDlgItem(IDC_CHK_MAP_MOVE_ENTITIES))
        m_moveEntitiesWithMap = (b->GetCheck() == BST_CHECKED);
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
    // Keep in sync with kSpeeds[] in OnInitDialog().
    static const double kMul[] = { 1.0, 2.0, 5.0, 10.0, 30.0, 60.0,
                                   120.0, 300.0, 600.0, 1200.0 };
    if (CComboBox* spd = (CComboBox*)GetDlgItem(IDC_COMBO_PREVIEW_SPEED))
    {
        const int sel = spd->GetCurSel();
        if (sel >= 0 && sel < static_cast<int>(sizeof(kMul) / sizeof(kMul[0])))
            m_playSpeed = kMul[sel];
    }
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
}

//
// OnHScroll — time-slider scrub: set preview time from the slider position,
//   discard trail history, and repaint.
//
void CPreviewPage::OnHScroll(UINT code, UINT pos, CScrollBar* sb)
{
    if (m_suppressScroll) { CHelpAwarePage::OnHScroll(code, pos, sb); return; }
    CSliderCtrl* slider = (CSliderCtrl*)GetDlgItem(IDC_SLIDER_PREVIEW_TIME);
    if (sb && slider && sb->GetSafeHwnd() == slider->GetSafeHwnd())
    {
        const int p = slider->GetPos();
        m_previewTimeSec = p / 10.0;
        m_trails.clear();   // scrubbing invalidates trail history
        RebuildRenderState();
        if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
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
    m_showLabels = IsDlgButtonChecked(IDC_CHK_PREVIEW_LABELS) == BST_CHECKED;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnTrailsToggle — toggle motion trails (clearing history when off) and repaint.
//
void CPreviewPage::OnTrailsToggle()
{
    m_showTrails = IsDlgButtonChecked(IDC_CHK_PREVIEW_TRAILS) == BST_CHECKED;
    if (!m_showTrails) m_trails.clear();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnPathsToggle — toggle drawing of configured motion paths and repaint.
//
void CPreviewPage::OnPathsToggle()
{
    m_showPaths = IsDlgButtonChecked(IDC_CHK_PREVIEW_PATHS) == BST_CHECKED;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnOrientationToggle — toggle per-entity heading vectors and repaint.
//
void CPreviewPage::OnOrientationToggle()
{
    m_showOrientation = IsDlgButtonChecked(IDC_CHK_PREVIEW_ORIENTATION) == BST_CHECKED;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnTerrainToggle — toggle the level land/ocean footprint overlay and repaint.
//
void CPreviewPage::OnTerrainToggle()
{
    m_showTerrain = IsDlgButtonChecked(IDC_CHK_PREVIEW_TERRAIN) == BST_CHECKED;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnLegendToggle — toggle the size/direction legend overlay and repaint.
//
void CPreviewPage::OnLegendToggle()
{
    m_showLegend = IsDlgButtonChecked(IDC_CHK_PREVIEW_LEGEND) == BST_CHECKED;
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
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
    if (m_boundaryArmed) m_startPickArmed = false;   // the two paint modes are mutually exclusive
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
void CPreviewPage::OnBuildTerrain()
{
    if (!m_scenario || !m_scenario->terrainBoundsValid)
    {
        MessageBox(_T("Paint a terrain boundary first:\n\nClick \"Boundary\", then drag a rectangle on ")
                   _T("the map to mark the area you want as solid 3D terrain."),
                   _T("Build 3D Terrain"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Resolve DISBrowser\Scripts\retile_terrain.cmd (same project-dir logic as the Run tab).
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
    CString cmdPath = projDir + _T("\\Scripts\\retile_terrain.cmd");

    if (::GetFileAttributes(cmdPath) == INVALID_FILE_ATTRIBUTES)
    {
        CString msg;
        msg.Format(_T("Cannot find the re-tile script:\n%s\n\nSet the DISBrowser project folder on the Run tab."),
                   (LPCTSTR)cmdPath);
        MessageBox(msg, _T("Build 3D Terrain"), MB_OK | MB_ICONWARNING);
        return;
    }

    CString confirm;
    confirm.Format(_T("Download DEM + build 3D terrain for:\n\n")
                   _T("  lat  %.4f .. %.4f\n  lon  %.4f .. %.4f\n\n")
                   _T("This runs the WSL/Docker tiling pipeline and can take several minutes. Continue?"),
                   m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                   m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg);
    if (MessageBox(confirm, _T("Build 3D Terrain"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;

    // retile_terrain.cmd latMin latMax lonMin lonMax
    CString args;
    args.Format(_T("%.6f %.6f %.6f %.6f"),
                m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg);

    HINSTANCE h = ::ShellExecute(GetSafeHwnd(), _T("open"), cmdPath, args, projDir, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32)
    {
        CString msg;
        msg.Format(_T("Failed to launch the re-tile script (error %Id)."), (INT_PTR)h);
        MessageBox(msg, _T("Build 3D Terrain"), MB_OK | MB_ICONERROR);
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
    const bool up = ProbeTcpPort("127.0.0.1", 8088, 250);
    const bool changed = (up != m_terrainServerUp);
    m_terrainServerUp = up;

    // Painting a boundary or building tiles only makes sense once the server serves
    // them, so gate those behind the server being up. Stop is only useful when up;
    // Start stays enabled (it repaints its own green "running" check).
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_BOUNDARY))      b->EnableWindow(up);
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_BUILD_TERRAIN)) b->EnableWindow(up);
    if (CWnd* b = GetDlgItem(IDC_BTN_PREVIEW_TERRAIN_STOP))  b->EnableWindow(up);
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

    if (changed)
        LOG(up ? "PreviewPage: terrain server UP (localhost:8088)"
               : "PreviewPage: terrain server DOWN");
}

//
// OnStartTerrainServer — launch the self-hosted tile server (serve_terrain.cmd) in
//   its own console unless it's already up; the poll timer flips the UI once :8088
//   answers.
//
void CPreviewPage::OnStartTerrainServer()
{
    if (m_terrainServerUp || ProbeTcpPort("127.0.0.1", 8088, 250))
    {
        RefreshTerrainServerUi();   // already running — just sync the UI
        return;
    }

    CString cmdPath = ResolveDisBrowserScript(_T("serve_terrain.cmd"));
    if (::GetFileAttributes(cmdPath) == INVALID_FILE_ATTRIBUTES)
    {
        CString msg;
        msg.Format(_T("Cannot find the terrain server script:\n%s\n\nSet the DISBrowser project folder on the Run tab."),
                   (LPCTSTR)cmdPath);
        MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONWARNING);
        return;
    }

    // Launch the server in its own console (it stays up independently of this editor).
    HINSTANCE h = ::ShellExecute(GetSafeHwnd(), _T("open"), cmdPath, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32)
    {
        CString msg;
        msg.Format(_T("Failed to launch the terrain server (error %Id)."), (INT_PTR)h);
        MessageBox(msg, _T("Start Terrain Server"), MB_OK | MB_ICONERROR);
        return;
    }
    // The server needs a moment to bind :8088; the poll timer flips the UI to
    // "running" (green check + enables Boundary/Build) as soon as the port answers.
    RefreshTerrainServerUi();
}

void CPreviewPage::OnStopTerrainServer()
{
    // Kill the WSL-side server process; its console window then closes on its own.
    ::ShellExecute(GetSafeHwnd(), _T("open"), _T("wsl.exe"),
                   _T("-d Ubuntu -u root -- pkill -f serve.py"), nullptr, SW_HIDE);
    // The port may take a beat to drop; the poll timer will settle the final state.
    RefreshTerrainServerUi();
}

void CPreviewPage::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDIS)
{
    // Three owner-draw buttons share this painter: "Set Start" (green "?" while armed),
    // "Boundary" (bold green check while armed), and "Start Terrain Server" (bold green
    // check while the tile server is running). Everything else defers to the base class.
    const bool isSetStart = (nIDCtl == IDC_BTN_PREVIEW_SET_START);
    const bool isBoundary = (nIDCtl == IDC_BTN_PREVIEW_BOUNDARY);
    const bool isSrvStart = (nIDCtl == IDC_BTN_PREVIEW_TERRAIN_START);
    if ((!isSetStart && !isBoundary && !isSrvStart) || !lpDIS)
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

    // Label + a trailing green hint glyph while the mode is armed / server is up.
    const bool  showHint = isSetStart ? m_startPickArmed
                         : isBoundary ? m_boundaryArmed
                                      : m_terrainServerUp;
    CString base = isSetStart ? _T("Set Start")
                 : isBoundary ? _T("Boundary")
                 : (m_terrainServerUp ? _T("Terrain Server") : _T("Start Terrain Server"));
    CString hint = isSetStart ? _T(" ?") : _T(" \x2713");   // U+2713 check for Boundary/Server

    // The Boundary / Server check is drawn in a bold font so it reads clearly.
    CFont  boldFont;
    CFont* boldPtr = nullptr;
    if ((isBoundary || isSrvStart) && showHint && hf)
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
            SetSegmentStartEnu(seg, enuE, enuN, up);
    }

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

void CPreviewPage::EndEntityDrag()
{
    // Reload the other tabs from the shared model and flag the scenario dirty
    // so the new start location persists into the .ini on Save. SendMessage
    // (synchronous) matches the WM_APP_MARK_DIRTY convention used elsewhere.
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
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

//
// DeleteEntity — remove entity `idx` (never the last one), refresh caches, and
//   reload the other tabs.
//
void CPreviewPage::DeleteEntity(size_t idx)
{
    if (!m_scenario) return;
    if (m_scenario->entities.size() <= 1) return;   // keep at least one (matches the Assets page)
    if (idx >= m_scenario->entities.size()) return;

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
    // uses; fall back to 100 m/s when the type isn't catalogued.
    double speed = 0.0;
    const AirframeProfile prof = theApp.Catalog().Profile(
        e.kind, e.domain, e.category, e.subcategory);
    if (prof.valid && prof.cruiseSpeedMps > 0.0) speed = prof.cruiseSpeedMps;
    if (speed <= 0.0) speed = 100.0;

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

    // Cruise (the speed reached at the END of the take-off roll); fall back to
    // 100 m/s when the type isn't catalogued.
    double speed = 0.0;
    const AirframeProfile prof = theApp.Catalog().Profile(
        e.kind, e.domain, e.category, e.subcategory);
    if (prof.valid && prof.cruiseSpeedMps > 0.0) speed = prof.cruiseSpeedMps;
    if (speed <= 0.0) speed = 100.0;

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
    seg.endSecond         = (speed > 0.0) ? (2.0 * initLen / speed) : 60.0;
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

    double speed = 0.0;
    const AirframeProfile prof = theApp.Catalog().Profile(
        e.kind, e.domain, e.category, e.subcategory);
    if (prof.valid && prof.cruiseSpeedMps > 0.0) speed = prof.cruiseSpeedMps;
    if (speed <= 0.0) speed = 100.0;

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

    double speed = 0.0;
    const AirframeProfile prof = theApp.Catalog().Profile(
        e.kind, e.domain, e.category, e.subcategory);
    if (prof.valid && prof.cruiseSpeedMps > 0.0) speed = prof.cruiseSpeedMps;
    if (speed <= 0.0) speed = 100.0;

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

    RebuildPathsCache();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
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

    // Keep travel time consistent with the type's cruise speed over the new
    // distance, so the entity still arrives at the end at endSecond. A take-off
    // leg accelerates from 0 to `speedMps`, so its average speed is half cruise
    // and it needs twice the time (2*dist/speed) to reach cruise at the end.
    const double dx = seg.endLocalX - seg.startLocalX;
    const double dy = seg.endLocalY - seg.startLocalY;
    const double dz = seg.endLocalZ - seg.startLocalZ;
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (seg.speedMps > 0.0 && dist > 0.0)
        seg.endSecond = seg.startSecond +
            (seg.accelerateFromStop ? (2.0 * dist / seg.speedMps) : (dist / seg.speedMps));

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
    m_showDestTime = IsDlgButtonChecked(IDC_CHK_PREVIEW_DESTTIME) == BST_CHECKED;
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
    if (s.type != MotionType::Ellipse || s.followEntityId < 0) return false;
    const int ti = FindEntityIndexById(s.followEntityId);
    if (ti < 0 || static_cast<size_t>(ti) >= m_state.poses.size()) return false;
    if (!m_state.poses[ti].enabled) return false;

    double f1E, f1N, f2E, f2N;
    FocusToEnu(s, /*focus2*/ false, f1E, f1N);
    FocusToEnu(s, /*focus2*/ true,  f2E, f2N);
    const double midE = 0.5 * (f1E + f2E), midN = 0.5 * (f1N + f2N);
    outDE = m_state.poses[ti].enuE - midE;
    outDN = m_state.poses[ti].enuN - midN;
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

    // Re-center the loop on the target's position at the current preview time.
    const SampledPose tp = MotionSampler::SamplePose(target, *m_scenario, m_previewTimeSec);
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
        m_zoomMetersPerPx = 1.0;
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
        m_zoomMetersPerPx = 1.0;
        m_centerEnuE = m_centerEnuN = 0.0;
        return;
    }

    // 10% padding around the bounding box; ensure non-zero extent.
    double boxW = std::max(1.0, (maxE - minE)) * 1.10;
    double boxH = std::max(1.0, (maxN - minN)) * 1.10;
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
        if (!e.enabled) { PreviewEntityPose off; off.enabled = false; m_state.poses.push_back(off); continue; }

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
        ep.name       = e.marking.empty() ? e.name : e.marking;
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
            PreviewLineAnchor a;
            a.entityIdx = ei;
            a.segIdx    = si;
            a.enuE      = sg.endLocalX;
            a.enuN      = sg.endLocalY;
            m_state.lineAnchors.push_back(a);
        }
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
