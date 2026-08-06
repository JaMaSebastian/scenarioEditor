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
#include "ScenarioWorker.h"   // WM_APP_REFRESH_UI / WM_APP_MARK_DIRTY
#include "ScenarioEditor.h"   // theApp.Catalog() — per-type cruise speed
#include "EntityCameraDialog.h"
#include "FoliageDialog.h"          // "Foliage" button dialog
#include "EntityTypeCatalog.h"      // CatalogEntry / AirframeProfile
#include "SpeedSeed.h"              // SeedCruiseSpeedMps — catalog cruise / 10 m/s default
#include "CameraTargetExtents.h"    // cache the framed entity's catalogued size
#include "CameraZoomMenu.h"         // shared "Zoom" RMB submenu (canvas + timeline)
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

    // Restore persisted Preview toggles so the checkboxes come up exactly as the
    // user last left them (settings.ini [Preview], loaded once at app startup).
    {
        const Settings& st = theApp.Settings();
        m_showLabels          = st.previewShowLabels;
        m_showTrails          = st.previewShowTrails;
        m_showPaths           = st.previewShowPaths;
        m_showOrientation     = st.previewShowOrientation;
        m_showTerrain         = st.previewShowTerrain;
        m_showZones           = st.previewShowZones;
        m_showLegend          = st.previewShowLegend;
        m_showDestTime        = st.previewShowProperties;
        m_moveEntitiesWithMap = st.previewMoveEntities;
    }

    CheckDlgButton(IDC_CHK_PREVIEW_LABELS,      m_showLabels      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_TRAILS,      m_showTrails      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_PATHS,       m_showPaths       ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_ORIENTATION, m_showOrientation ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_TERRAIN,     m_showTerrain     ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHK_PREVIEW_ZONES,       m_showZones       ? BST_CHECKED : BST_UNCHECKED);
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
        dt->ShowWindow(SW_HIDE);   // revealed when the Show > "Properties" box is checked
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
    if (CButton* b = (CButton*)GetDlgItem(IDC_CHK_MAP_MOVE_ENTITIES))
        m_moveEntitiesWithMap = (b->GetCheck() == BST_CHECKED);
    PersistToggles();
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
    m_showLabels = IsDlgButtonChecked(IDC_CHK_PREVIEW_LABELS) == BST_CHECKED;
    PersistToggles();
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
    PersistToggles();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnPathsToggle — toggle drawing of configured motion paths and repaint.
//
void CPreviewPage::OnPathsToggle()
{
    m_showPaths = IsDlgButtonChecked(IDC_CHK_PREVIEW_PATHS) == BST_CHECKED;
    PersistToggles();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnOrientationToggle — toggle per-entity heading vectors and repaint.
//
void CPreviewPage::OnOrientationToggle()
{
    m_showOrientation = IsDlgButtonChecked(IDC_CHK_PREVIEW_ORIENTATION) == BST_CHECKED;
    PersistToggles();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnTerrainToggle — toggle the level land/ocean footprint overlay and repaint.
//
void CPreviewPage::OnTerrainToggle()
{
    m_showTerrain = IsDlgButtonChecked(IDC_CHK_PREVIEW_TERRAIN) == BST_CHECKED;
    PersistToggles();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// PersistToggles — write the current Preview display-toggle states into
//   settings.ini so they come back the same way next session.
//
void CPreviewPage::PersistToggles()
{
    Settings& st = theApp.Settings();
    st.previewShowLabels      = m_showLabels;
    st.previewShowTrails      = m_showTrails;
    st.previewShowPaths       = m_showPaths;
    st.previewShowOrientation = m_showOrientation;
    st.previewShowTerrain     = m_showTerrain;
    st.previewShowZones       = m_showZones;
    st.previewShowLegend      = m_showLegend;
    st.previewShowProperties  = m_showDestTime;
    st.previewMoveEntities    = m_moveEntitiesWithMap;
    SettingsIO::Save(st, theApp.SettingsPath());
}

//
// OnZonesToggle — toggle the named level zones (e.g. Palm Forest) independently
//   of the land/ocean footprint, so the big terrain box can be hidden while a
//   zone stays visible. Repaints.
//
void CPreviewPage::OnZonesToggle()
{
    m_showZones = IsDlgButtonChecked(IDC_CHK_PREVIEW_ZONES) == BST_CHECKED;
    PersistToggles();
    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
}

//
// OnLegendToggle — toggle the size/direction legend overlay and repaint.
//
void CPreviewPage::OnLegendToggle()
{
    m_showLegend = IsDlgButtonChecked(IDC_CHK_PREVIEW_LEGEND) == BST_CHECKED;
    PersistToggles();
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

    CString cmdPath = ResolveDisBrowserScript(_T("build_foliage.cmd"));
    if (::GetFileAttributes(cmdPath) == INVALID_FILE_ATTRIBUTES)
    {
        CString msg;
        msg.Format(_T("Cannot find the foliage bake script:\n%s\n\nSet the DISBrowser project folder on the Run tab."),
                   (LPCTSTR)cmdPath);
        MessageBox(msg, _T("Build i3dm Foliage"), MB_OK | MB_ICONWARNING);
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

    // build_foliage.cmd latMin latMax lonMin lonMax --count N --min-land M
    CString args;
    args.Format(_T("%.6f %.6f %.6f %.6f --count 20000 --min-land 0.5"),
                m_scenario->terrainLatMinDeg, m_scenario->terrainLatMaxDeg,
                m_scenario->terrainLonMinDeg, m_scenario->terrainLonMaxDeg);

    HINSTANCE h = ::ShellExecute(GetSafeHwnd(), _T("open"), cmdPath, args, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32)
    {
        CString msg;
        msg.Format(_T("Failed to launch the foliage bake script (error %Id)."), (INT_PTR)h);
        MessageBox(msg, _T("Build i3dm Foliage"), MB_OK | MB_ICONERROR);
    }
}

//
// OnPurgeTerrain — wipe ALL downloaded terrain (every landscape) after a strong
//   confirmation, then start fresh. Runs retile_terrain.cmd --purge --yes; the
//   GUI Yes/No is the real gate, so --yes skips the script's console prompt.
//
void CPreviewPage::OnPurgeTerrain()
{
    CString cmdPath = ResolveDisBrowserScript(_T("retile_terrain.cmd"));
    if (::GetFileAttributes(cmdPath) == INVALID_FILE_ATTRIBUTES)
    {
        CString msg;
        msg.Format(_T("Cannot find the re-tile script:\n%s\n\nSet the DISBrowser project folder on the Run tab."),
                   (LPCTSTR)cmdPath);
        MessageBox(msg, _T("Purge Terrain"), MB_OK | MB_ICONWARNING);
        return;
    }

    if (MessageBox(_T("Delete ALL downloaded 3D terrain?\n\n")
                   _T("This wipes every DEM tile and the entire served tile tree for EVERY ")
                   _T("landscape (Cesium falls back to a bare globe until you build again). ")
                   _T("This cannot be undone."),
                   _T("Purge Terrain"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    HINSTANCE h = ::ShellExecute(GetSafeHwnd(), _T("open"), cmdPath, _T("--purge --yes"),
                                 nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32)
    {
        CString msg;
        msg.Format(_T("Failed to launch the purge (error %Id)."), (INT_PTR)h);
        MessageBox(msg, _T("Purge Terrain"), MB_OK | MB_ICONERROR);
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
    const bool isShowOrig = (nIDCtl == IDC_BTN_PREVIEW_SHOW_ORIGIN);
    if ((!isSetStart && !isBoundary && !isSrvStart && !isShowOrig) || !lpDIS)
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
                         : isShowOrig ? m_showOrigin
                                      : m_terrainServerUp;
    CString base = isSetStart ? _T("Set Start")
                 : isBoundary ? _T("Boundary")
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
void CPreviewPage::AddCameraFrame(CameraFrame&& f)
{
    if (!m_scenario) return;
    auto& cams = m_scenario->cameras;
    const double D = EffectivePreviewDuration();

    // Cache the target's catalogued size here rather than at each of the three
    // creation sites, so no path can produce a frame with an unresolved target.
    CameraTargetExtents::Apply(*m_scenario, f);

    // Default width = one tenth of the time currently visible in the strip (the
    // parent camera-duration control), so a fresh box is a readable slice at any
    // zoom. Fall back to a tenth of the full duration before the strip is realized.
    const double vis = m_timeline.VisibleSpanSeconds();
    double width = (vis > 0.0 ? vis : D) * 0.1;
    if (width > D)    width = D;
    if (width <= 0.0) width = 1.0;

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
    if (CComboBox* pre = (CComboBox*)GetDlgItem(IDC_COMBO_CAM_PRESET))
    {
        pre->ResetContent();
        const auto& presets = theApp.CameraPresets().Presets();
        for (size_t i = 0; i < presets.size(); ++i)
        {
            CString label(CA2W(presets[i].DisplayLabel().c_str()));
            const int ix = pre->AddString(label);
            pre->SetItemData(ix, static_cast<DWORD_PTR>(i));
        }
        if (pre->GetCount() > 0) pre->SetCurSel(0);
    }
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

//
// OnNewCamera — "New Cam" button: drop a Stationary camera box in the upper-left
//   of the current view and register a matching frame on the timeline.
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

void CPreviewPage::OnNewCamera()
{
    if (!m_scenario) return;

    CameraFrame f;
    f.kind = CameraKind::Stationary;

    // Upper-left region of the current view (fallback: up-and-left of center).
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
    // Unique "Camera N": pick the lowest N whose label isn't already in use.
    // Deriving it from cameras.size()+1 collides after a delete-then-add (e.g.
    // deleting one of six leaves size 5, and "Camera 6" is regenerated as a
    // duplicate), which is exactly the two-"Camera 6" case the operator hit.
    f.label = NextStationaryCameraLabel();

    AddCameraFrame(std::move(f));

    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
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
    const size_t pidx = static_cast<size_t>(pre->GetItemData(pi));
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
                                        bool limitsEnabled)
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

    const std::string ename = src.marking.empty() ? src.name : src.marking;
    f.label = ename + " / " + presets[presetIndex].DisplayLabel();

    AddCameraFrame(std::move(f));

    RebuildRenderState();
    if (m_canvas.GetSafeHwnd()) m_canvas.Invalidate(FALSE);
    if (m_timeline.GetSafeHwnd()) m_timeline.Invalidate(FALSE);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// NewEntityCameraViaDialog — entity-dot "New Camera..." path: pop the modal that
//   lets the operator choose the camera preset, the tracked target, the zoom and
//   the transition (source is the clicked entity), then create the frame on OK.
//
void CPreviewPage::NewEntityCameraViaDialog(size_t entityIdx)
{
    if (!m_scenario || entityIdx >= m_scenario->entities.size()) return;

    if (theApp.CameraPresets().Presets().empty())
    {
        AfxMessageBox(_T("No camera presets are available (config\\Cameras.ini)."));
        return;
    }

    const Entity& src = m_scenario->entities[entityIdx];
    const std::string sname = src.marking.empty() ? src.name : src.marking;
    CString srcLabel;
    srcLabel.Format(_T("%s [%u]"),
                    (LPCTSTR)CString(CA2W(sname.c_str())),
                    static_cast<unsigned>(src.entityId));

    CEntityCameraDialog dlg(this);
    dlg.SetContext(m_scenario, src.entityId, srcLabel);
    if (dlg.DoModal() != IDOK) return;

    AddEntityCameraFrame(entityIdx, dlg.PresetIndex(),
                         dlg.TargetEntityId(), dlg.Transition(),
                         dlg.ZoomEnabled(), dlg.ZoomFovDeg(),
                         dlg.DynamicZoom(), dlg.ZoomFillPct(),
                         dlg.LimitsEnabled());
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
// SetCameraTarget — set frame `frameIdx`'s tracked entity (0 = focus on source).
//
void CPreviewPage::SetCameraTarget(size_t frameIdx, uint16_t targetEntityId)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    CameraFrame& f = m_scenario->cameras[frameIdx];
    f.targetEntityId = targetEntityId;
    // A new target is a new size — dynamic zoom would otherwise keep framing the
    // old entity's dimensions.
    CameraTargetExtents::Apply(*m_scenario, f);
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// SetCameraTransition — set the transition used entering frame `frameIdx`.
//
void CPreviewPage::SetCameraTransition(size_t frameIdx, CameraTransition t)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    m_scenario->cameras[frameIdx].transition = t;
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// SetCameraZoomEnabled — master zoom toggle for frame `frameIdx`. Off restores
//   the legacy behaviour (preset FOV for Entity frames, engine default otherwise).
//
void CPreviewPage::SetCameraZoomEnabled(size_t frameIdx, bool on)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    m_scenario->cameras[frameIdx].zoomEnabled = on;
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// SetCameraDynamicZoom — auto-fit the target instead of holding a fixed FOV.
//
void CPreviewPage::SetCameraDynamicZoom(size_t frameIdx, bool on)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    m_scenario->cameras[frameIdx].dynamicZoom = on;
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// SetCameraGimbalLimits — enforce the mounted preset's travel envelope for this frame.
//   Off restores the legacy behaviour: the camera aims wherever the target is, however
//   far off-boresight that would really be.
//
void CPreviewPage::SetCameraGimbalLimits(size_t frameIdx, bool on)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    m_scenario->cameras[frameIdx].limitsEnabled = on;
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// SetCameraZoomFov — static FOV override (degrees), clamped to the authoring range.
//
void CPreviewPage::SetCameraZoomFov(size_t frameIdx, double fovDeg)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    if (fovDeg < kZoomFovMinDeg) fovDeg = kZoomFovMinDeg;
    if (fovDeg > kZoomFovMaxDeg) fovDeg = kZoomFovMaxDeg;
    m_scenario->cameras[frameIdx].zoomFovDeg = fovDeg;
    if (CWnd* top = GetTopLevelParent())
        top->SendMessage(WM_APP_REFRESH_UI, 0, 0);
}

//
// SetCameraZoomFill — how much of the frame a dynamic fit should fill (percent).
//
void CPreviewPage::SetCameraZoomFill(size_t frameIdx, double fillPct)
{
    if (!m_scenario || frameIdx >= m_scenario->cameras.size()) return;
    if (fillPct < kZoomFillMinPct) fillPct = kZoomFillMinPct;
    if (fillPct > kZoomFillMaxPct) fillPct = kZoomFillMaxPct;
    m_scenario->cameras[frameIdx].zoomFillPct = fillPct;
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
    m_showDestTime = IsDlgButtonChecked(IDC_CHK_PREVIEW_DESTTIME) == BST_CHECKED;
    PersistToggles();
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
