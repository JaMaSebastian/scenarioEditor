//=============================================================================
//  CesiumView.h
//-----------------------------------------------------------------------------
//  Declares CesiumView, an MFC CWnd that hosts a 3D CesiumJS globe inside a
//  WebView2 control for the Preview area's "Cesium (3D)" map layer, plus the
//  CesiumEntityPt struct used to plot labelled dots on that globe. The public
//  API is identical whether or not the WebView2 SDK is present at build time.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-10
//=============================================================================
#pragma once

#include "pch.h"

#include <string>
#include <vector>
#include <functional>

// A 3D Cesium globe hosted in a WebView2 control, shown over the Preview area
// when the "Cesium (3D)" map layer is selected.
//
// All WebView2 SDK usage lives in the .cpp behind HAVE_WEBVIEW2 (defined in the
// Visual Studio build, where the NuGet "Microsoft.Web.WebView2" package supplies
// the SDK). Without that define the class still compiles and shows a small
// placeholder message, so the CMake build (which has no WebView2 SDK) stays
// green. The public API is identical either way.

//-----------------------------------------------------------------------------
// CesiumEntityPt — one entity marker to plot on the globe
//   Geographic position (lat/lon/altitude, metres) plus a display name and an
//   0xRRGGBB dot colour. Passed as a batch to CesiumView::SetEntities.
//-----------------------------------------------------------------------------
struct CesiumEntityPt
{
    std::string  name;
    double       lat     = 0.0;
    double       lon     = 0.0;
    double       altM    = 0.0;
    unsigned int colorRgb = 0x78BEFF;   // 0xRRGGBB dot color
};

//-----------------------------------------------------------------------------
// CesiumView — WebView2-hosted 3D Cesium globe child window
//   Owns the WebView2 environment/controller/webview (via a pImpl so the header
//   stays SDK-free), navigates it to an embedded CesiumJS page, and marshals
//   host<->page JSON messages: fly-to, entity dots, boundary paint, camera
//   look-at reports.
//-----------------------------------------------------------------------------
class CesiumView : public CWnd
{
public:
    CesiumView();
    ~CesiumView() override;

    // Create the host window as a child of `parent` at client-rect `rc`. The
    // WebView2 environment/control initialize asynchronously; queued messages
    // flush once it's ready.
    bool Create(CWnd* parent, const CRect& rc);
    void SetBounds(const CRect& rc);         // keep the webview sized to the area
    void ShowView(bool show);
    bool IsReady() const { return m_ready; }

    // Fly the camera to look down on (lat,lon) from `heightM` metres.
    void FlyTo(double lat, double lon, double heightM);
    // Replace the globe's entity dots/labels.
    void SetEntities(const std::vector<CesiumEntityPt>& pts);

    // Geographic point the camera is currently centered on plus the eye altitude
    // (metres above the ellipsoid). The globe reports these on every camera move;
    // returns false until the first report arrives.
    bool GetLookAt(double& latDeg, double& lonDeg, double& altM) const;

    // Arm/disarm "Boundary" paint mode on the globe. While armed a right-drag draws a
    // yellow rectangle and, on release, reports the lat/lon box via the bounds callback.
    void SetBoundaryMode(bool on);
    // Called (UI thread) with the painted box (degrees) when the user finishes a right-drag.
    void SetBoundsCallback(std::function<void(double,double,double,double)> cb) { m_onBounds = std::move(cb); }

protected:
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnDestroy();
    afx_msg void OnPaint();
    DECLARE_MESSAGE_MAP()

private:
    void PostJson(const std::wstring& json);   // send now, or queue until ready
    void FlushPending();
    void OnWebViewReady();                      // navigation complete -> flush

    bool                       m_ready = false;
    std::vector<std::wstring>  m_pending;       // messages queued pre-ready

    // Invoked with a painted terrain box (latMin,latMax,lonMin,lonMax in degrees).
    std::function<void(double,double,double,double)> m_onBounds;

    // Latest camera look-at point reported by the globe (UI-thread only).
    bool                       m_haveLookAt = false;
    double                     m_lookAtLat  = 0.0;
    double                     m_lookAtLon  = 0.0;
    double                     m_lookAtAlt  = 0.0;   // eye height, metres

    // WebView2 COM objects are held in a pImpl so this header stays SDK-free.
    struct Impl;
    Impl* m_impl = nullptr;
};
