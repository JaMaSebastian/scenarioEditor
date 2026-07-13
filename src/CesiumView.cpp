#include "pch.h"
#include "CesiumView.h"
#include "ScenarioEditor.h"   // theApp.SettingsPath() -> WebView2 user-data folder
#include "../log.h"           // LOG()

#ifdef HAVE_WEBVIEW2
#include <wrl.h>
#include <wrl/event.h>
#include <WebView2.h>
#pragma comment(lib, "WebView2LoaderStatic.lib")
using namespace Microsoft::WRL;
#endif

namespace
{
    // Self-contained Cesium page. Loads CesiumJS from a CDN (online), drapes
    // no-key ESRI World Imagery on a smooth ellipsoid (no Ion token, no 3D
    // terrain relief), and listens for host messages: {t:'fly',...} /
    // {t:'ents',items:[...]}. Posts {t:'ready'} once the globe is up.
    const wchar_t* kCesiumHtml = LR"HTML(<!doctype html><html><head><meta charset="utf-8">
<link href="https://cdn.jsdelivr.net/npm/cesium@1.119/Build/Cesium/Widgets/widgets.css" rel="stylesheet">
<style>html,body,#c{width:100%;height:100%;margin:0;padding:0;overflow:hidden;background:#000}</style>
<script>window.CESIUM_BASE_URL='https://cdn.jsdelivr.net/npm/cesium@1.119/Build/Cesium/';</script>
<script src="https://cdn.jsdelivr.net/npm/cesium@1.119/Build/Cesium/Cesium.js"></script>
</head><body><div id="c"></div>
<script>
function boot(){
  if(!window.Cesium){setTimeout(boot,60);return;}
  try{Cesium.Ion.defaultAccessToken=undefined;}catch(e){}
  var viewer=new Cesium.Viewer('c',{
    baseLayer:false, baseLayerPicker:false, geocoder:false, homeButton:false,
    sceneModePicker:false, navigationHelpButton:false, animation:false,
    timeline:false, fullscreenButton:false, infoBox:false, selectionIndicator:false,
    terrainProvider:new Cesium.EllipsoidTerrainProvider()
  });
  viewer.imageryLayers.addImageryProvider(new Cesium.UrlTemplateImageryProvider({
    url:'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
    maximumLevel:19}));
  var ents={};
  function setEnts(items){
    for(var k in ents){viewer.entities.remove(ents[k]);} ents={};
    (items||[]).forEach(function(it){
      var col=Cesium.Color.fromBytes((it.c>>16)&255,(it.c>>8)&255,it.c&255,255);
      ents[it.n]=viewer.entities.add({
        position:Cesium.Cartesian3.fromDegrees(it.lon,it.lat,it.alt||0),
        point:{pixelSize:10,color:col,outlineColor:Cesium.Color.WHITE,outlineWidth:1.5,
               disableDepthTestDistance:Number.POSITIVE_INFINITY},
        label:{text:it.n,font:'12px sans-serif',fillColor:Cesium.Color.WHITE,
               pixelOffset:new Cesium.Cartesian2(10,0),
               disableDepthTestDistance:Number.POSITIVE_INFINITY,
               verticalOrigin:Cesium.VerticalOrigin.CENTER}
      });
    });
  }
  function flyTo(lat,lon,h){
    viewer.camera.flyTo({destination:Cesium.Cartesian3.fromDegrees(lon,lat,h||30000.0),duration:1.0});
  }
  // ----- "Boundary" paint: right-drag a yellow rectangle to mark the 3D-terrain box.
  // Mirrors the 2D canvas gesture. While armed we free RIGHT_DRAG from the camera's zoom
  // (wheel zoom still works) and use it to draw + pick a lat/lon rectangle on the ellipsoid.
  var scene=viewer.scene;
  viewer.canvas.oncontextmenu=function(ev){ev.preventDefault();return false;};
  var bnd={on:false,active:false,startCarto:null,curCarto:null,rectEnt:null,handler:null,savedZoom:null,savedTilt:null};
  function pickCarto(x,y){
    var cc=viewer.camera.pickEllipsoid(new Cesium.Cartesian2(x,y),scene.globe.ellipsoid);
    return cc?scene.globe.ellipsoid.cartesianToCartographic(cc):null;
  }
  function bndRect(){
    if(!bnd.startCarto||!bnd.curCarto)return Cesium.Rectangle.fromDegrees(0,0,0,0);
    return new Cesium.Rectangle(
      Math.min(bnd.startCarto.longitude,bnd.curCarto.longitude),
      Math.min(bnd.startCarto.latitude,bnd.curCarto.latitude),
      Math.max(bnd.startCarto.longitude,bnd.curCarto.longitude),
      Math.max(bnd.startCarto.latitude,bnd.curCarto.latitude));
  }
  // Snap to a straight-down (nadir) view over the current center so a geographic
  // lat/lon rectangle draws as a real rectangle (square corners) instead of an
  // oblique-perspective trapezoid. Keeps the current eye height.
  function lookDownNadir(){
    var eye=viewer.camera.positionCartographic; var h=eye?eye.height:30000;
    var ray=viewer.camera.getPickRay(new Cesium.Cartesian2(
      Math.round(viewer.canvas.clientWidth/2),Math.round(viewer.canvas.clientHeight/2)));
    var pos=ray?scene.globe.pick(ray,scene):null;
    var carto=pos?scene.globe.ellipsoid.cartesianToCartographic(pos):eye;
    if(!carto)return;
    viewer.camera.flyTo({
      destination:Cesium.Cartesian3.fromRadians(carto.longitude,carto.latitude,h),
      orientation:{heading:0.0,pitch:-Cesium.Math.PI_OVER_TWO,roll:0.0},
      duration:0.5});
  }
  function bndCleanup(){
    bnd.active=false;bnd.startCarto=null;bnd.curCarto=null;
    if(bnd.rectEnt){viewer.entities.remove(bnd.rectEnt);bnd.rectEnt=null;}
    if(bnd.savedZoom){scene.screenSpaceCameraController.zoomEventTypes=bnd.savedZoom;bnd.savedZoom=null;}
    if(bnd.savedTilt){scene.screenSpaceCameraController.tiltEventTypes=bnd.savedTilt;bnd.savedTilt=null;}
    if(bnd.handler){bnd.handler.destroy();bnd.handler=null;}
    bnd.on=false;
  }
  function setBoundaryMode(on){
    if(!on){bndCleanup();return;}
    bnd.on=true;
    var ctrl=scene.screenSpaceCameraController;
    bnd.savedZoom=ctrl.zoomEventTypes;
    ctrl.zoomEventTypes=[Cesium.CameraEventType.WHEEL,Cesium.CameraEventType.PINCH];
    bnd.savedTilt=ctrl.tiltEventTypes;
    ctrl.tiltEventTypes=[];   // stay top-down while painting (no re-tilt)
    lookDownNadir();   // top-down so the rectangle draws with square corners
    if(!bnd.handler){
      bnd.handler=new Cesium.ScreenSpaceEventHandler(viewer.canvas);
      bnd.handler.setInputAction(function(m){
        var c=pickCarto(m.position.x,m.position.y);if(!c)return;
        bnd.active=true;bnd.startCarto=c;bnd.curCarto=c;
        if(!bnd.rectEnt)bnd.rectEnt=viewer.entities.add({rectangle:{
          coordinates:new Cesium.CallbackProperty(bndRect,false),
          material:Cesium.Color.YELLOW.withAlpha(0.25),
          outline:true,outlineColor:Cesium.Color.YELLOW,height:0}});
      },Cesium.ScreenSpaceEventType.RIGHT_DOWN);
      bnd.handler.setInputAction(function(m){
        if(!bnd.active)return;var c=pickCarto(m.endPosition.x,m.endPosition.y);if(c)bnd.curCarto=c;
      },Cesium.ScreenSpaceEventType.MOUSE_MOVE);
      bnd.handler.setInputAction(function(m){
        if(!bnd.active)return;var c=pickCarto(m.position.x,m.position.y);if(c)bnd.curCarto=c;
        var laMn=Cesium.Math.toDegrees(Math.min(bnd.startCarto.latitude,bnd.curCarto.latitude));
        var laMx=Cesium.Math.toDegrees(Math.max(bnd.startCarto.latitude,bnd.curCarto.latitude));
        var loMn=Cesium.Math.toDegrees(Math.min(bnd.startCarto.longitude,bnd.curCarto.longitude));
        var loMx=Cesium.Math.toDegrees(Math.max(bnd.startCarto.longitude,bnd.curCarto.longitude));
        bndCleanup();
        if(Math.abs(laMx-laMn)>1e-6&&Math.abs(loMx-loMn)>1e-6)
          window.chrome.webview.postMessage({t:'bnds',latMin:laMn,latMax:laMx,lonMin:loMn,lonMax:loMx});
      },Cesium.ScreenSpaceEventType.RIGHT_UP);
    }
  }
  function reportCam(){
    try{
      var scene=viewer.scene, carto=null;
      var ray=viewer.camera.getPickRay(new Cesium.Cartesian2(
        Math.round(viewer.canvas.clientWidth/2),Math.round(viewer.canvas.clientHeight/2)));
      var pos=ray?scene.globe.pick(ray,scene):null;   // point under screen center
      if(pos) carto=scene.globe.ellipsoid.cartesianToCartographic(pos);
      else carto=viewer.camera.positionCartographic;   // fallback: ground subpoint
      // "Altitude" = eye height (like Google Earth's eye alt): how high the
      // camera is above the ellipsoid, independent of the look-at point.
      var eye=viewer.camera.positionCartographic;
      if(carto) window.chrome.webview.postMessage({t:'cam',
        lat:Cesium.Math.toDegrees(carto.latitude),lon:Cesium.Math.toDegrees(carto.longitude),
        alt:(eye?eye.height:0)});
    }catch(e){}
  }
  window.chrome.webview.addEventListener('message',function(e){
    var m=e.data; if(!m)return;
    if(m.t==='fly') flyTo(m.lat,m.lon,m.h);
    else if(m.t==='ents') setEnts(m.items);
    else if(m.t==='boundary') setBoundaryMode(!!m.on);
  });
  viewer.camera.percentageChanged=0.02;                 // report small camera moves
  viewer.camera.changed.addEventListener(reportCam);    // during drag/zoom
  viewer.scene.camera.moveEnd.addEventListener(reportCam); // and when it settles
  reportCam();
  window.chrome.webview.postMessage({t:'ready'});
}
boot();
</script></body></html>)HTML";

#ifdef HAVE_WEBVIEW2
    // Write the Cesium page to a local folder so it can be served from a real
    // https origin via a WebView2 virtual host. NavigateToString gives the page
    // a null origin, which blocks Cesium's cross-origin workers + imagery CORS.
    std::wstring PrepareHostFolder()
    {
        std::wstring base = theApp.SettingsPath();
        const size_t slash = base.find_last_of(L"\\/");
        base = (slash != std::wstring::npos) ? base.substr(0, slash) : std::wstring(L".");
        const std::wstring folder = base + L"\\cesium_host";
        ::CreateDirectoryW(folder.c_str(), nullptr);

        const int need = ::WideCharToMultiByte(CP_UTF8, 0, kCesiumHtml, -1,
                                               nullptr, 0, nullptr, nullptr);
        std::string utf8(need > 0 ? need - 1 : 0, '\0');
        if (need > 1)
            ::WideCharToMultiByte(CP_UTF8, 0, kCesiumHtml, -1, &utf8[0], need, nullptr, nullptr);

        const std::wstring path = folder + L"\\index.html";
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
            ::CloseHandle(h);
        }
        return folder;
    }
#endif
}

// pImpl: holds the WebView2 COM objects so the header stays SDK-free.
struct CesiumView::Impl
{
#ifdef HAVE_WEBVIEW2
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller>  controller;
    ComPtr<ICoreWebView2>            webview;
#endif
    int unused = 0;
};

BEGIN_MESSAGE_MAP(CesiumView, CWnd)
    ON_WM_SIZE()
    ON_WM_DESTROY()
    ON_WM_PAINT()
END_MESSAGE_MAP()

CesiumView::CesiumView() = default;

CesiumView::~CesiumView()
{
    delete m_impl;
    m_impl = nullptr;
}

bool CesiumView::Create(CWnd* parent, const CRect& rc)
{
    if (GetSafeHwnd()) return true;
    if (!parent) return false;

    LPCTSTR cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW,
                                      ::LoadCursor(nullptr, IDC_ARROW),
                                      (HBRUSH)::GetStockObject(BLACK_BRUSH));
    if (!CreateEx(0, cls, _T("CesiumView"),
                  WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, rc, parent, 0))
        return false;

    m_impl = new Impl();

#ifdef HAVE_WEBVIEW2
    // WebView2 needs a writable user-data folder; put it next to settings.ini.
    std::wstring udf = theApp.SettingsPath();
    const size_t slash = udf.find_last_of(L"\\/");
    udf = (slash != std::wstring::npos) ? udf.substr(0, slash) : std::wstring(L".");
    udf += L"\\webview2";

    HWND host = GetSafeHwnd();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, host](HRESULT r, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(r) || !env || !m_impl) return r;
                m_impl->env = env;
                env->CreateCoreWebView2Controller(host,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT r2, ICoreWebView2Controller* ctl) -> HRESULT
                        {
                            if (FAILED(r2) || !ctl || !m_impl) return r2;
                            m_impl->controller = ctl;
                            ctl->get_CoreWebView2(&m_impl->webview);
                            if (::IsWindow(GetSafeHwnd()))
                            {
                                RECT rcb; ::GetClientRect(GetSafeHwnd(), &rcb);
                                ctl->put_Bounds(rcb);
                            }
                            if (m_impl->webview)
                            {
                                EventRegistrationToken tok;
                                m_impl->webview->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [this](ICoreWebView2*,
                                               ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                                        {
                                            LPWSTR js = nullptr;
                                            if (args) args->get_WebMessageAsJson(&js);
                                            if (js)
                                            {
                                                std::wstring s(js);
                                                CoTaskMemFree(js);
                                                if (s.find(L"\"cam\"") != std::wstring::npos)
                                                {
                                                    // Cache the camera look-at point (UI thread).
                                                    auto num = [&](const wchar_t* key, double& out) -> bool
                                                    {
                                                        size_t p = s.find(key);
                                                        if (p == std::wstring::npos) return false;
                                                        p = s.find(L':', p);
                                                        if (p == std::wstring::npos) return false;
                                                        out = _wtof(s.c_str() + p + 1);
                                                        return true;
                                                    };
                                                    double la = 0.0, lo = 0.0, al = 0.0;
                                                    if (num(L"\"lat\"", la) && num(L"\"lon\"", lo))
                                                    {
                                                        m_lookAtLat = la; m_lookAtLon = lo;
                                                        num(L"\"alt\"", al);   // optional
                                                        m_lookAtAlt = al;
                                                        m_haveLookAt = true;
                                                    }
                                                }
                                                else if (s.find(L"\"bnds\"") != std::wstring::npos)
                                                {
                                                    // Painted terrain box from the globe → the page.
                                                    auto num = [&](const wchar_t* key, double& out) -> bool
                                                    {
                                                        size_t p = s.find(key);
                                                        if (p == std::wstring::npos) return false;
                                                        p = s.find(L':', p);
                                                        if (p == std::wstring::npos) return false;
                                                        out = _wtof(s.c_str() + p + 1);
                                                        return true;
                                                    };
                                                    double laMn = 0, laMx = 0, loMn = 0, loMx = 0;
                                                    if (num(L"\"latMin\"", laMn) && num(L"\"latMax\"", laMx) &&
                                                        num(L"\"lonMin\"", loMn) && num(L"\"lonMax\"", loMx) &&
                                                        m_onBounds)
                                                        m_onBounds(laMn, laMx, loMn, loMx);
                                                }
                                                else if (s.find(L"ready") != std::wstring::npos)
                                                    OnWebViewReady();
                                            }
                                            return S_OK;
                                        }).Get(), &tok);

                                // Serve from a real https origin (fixes Cesium's
                                // cross-origin workers + imagery CORS). Fall back
                                // to NavigateToString if the interface is missing.
                                ComPtr<ICoreWebView2_3> wv3;
                                if (SUCCEEDED(m_impl->webview.As(&wv3)) && wv3)
                                {
                                    const std::wstring folder = PrepareHostFolder();
                                    wv3->SetVirtualHostNameToFolderMapping(
                                        L"appassets.local", folder.c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                    // Cache-bust with a token tied to the page content so
                                    // WebView2 never serves a stale index.html after the
                                    // embedded HTML/JS changes between builds.
                                    const std::wstring url =
                                        L"https://appassets.local/index.html?v=" +
                                        std::to_wstring(wcslen(kCesiumHtml));
                                    m_impl->webview->Navigate(url.c_str());
                                }
                                else
                                {
                                    m_impl->webview->NavigateToString(kCesiumHtml);
                                }
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr))
        LOG("CesiumView: CreateCoreWebView2EnvironmentWithOptions failed "
            "(is the WebView2 Runtime installed?)");
#endif
    return true;
}

void CesiumView::OnWebViewReady()
{
    m_ready = true;
    FlushPending();
}

void CesiumView::PostJson(const std::wstring& json)
{
#ifdef HAVE_WEBVIEW2
    if (m_ready && m_impl && m_impl->webview)
    {
        m_impl->webview->PostWebMessageAsJson(json.c_str());
        return;
    }
#endif
    m_pending.push_back(json);
}

void CesiumView::FlushPending()
{
#ifdef HAVE_WEBVIEW2
    if (m_impl && m_impl->webview)
        for (const std::wstring& m : m_pending)
            m_impl->webview->PostWebMessageAsJson(m.c_str());
#endif
    m_pending.clear();
}

void CesiumView::FlyTo(double lat, double lon, double heightM)
{
    wchar_t buf[160];
    swprintf_s(buf, L"{\"t\":\"fly\",\"lat\":%.8f,\"lon\":%.8f,\"h\":%.1f}",
               lat, lon, heightM);
    PostJson(buf);
}

void CesiumView::SetBoundaryMode(bool on)
{
    PostJson(on ? L"{\"t\":\"boundary\",\"on\":true}"
                : L"{\"t\":\"boundary\",\"on\":false}");
}

void CesiumView::SetEntities(const std::vector<CesiumEntityPt>& pts)
{
    std::wstring json = L"{\"t\":\"ents\",\"items\":[";
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const CesiumEntityPt& p = pts[i];
        // Escape the name for JSON (quotes / backslashes).
        std::wstring nm;
        const CA2W wname(p.name.c_str());
        for (const wchar_t* c = (wname.m_psz ? wname.m_psz : L""); *c; ++c)
        {
            if (*c == L'"' || *c == L'\\') nm.push_back(L'\\');
            nm.push_back(*c);
        }
        wchar_t buf[256];
        swprintf_s(buf,
            L"%ls{\"n\":\"%ls\",\"lat\":%.8f,\"lon\":%.8f,\"alt\":%.1f,\"c\":%u}",
            (i ? L"," : L""), nm.c_str(), p.lat, p.lon, p.altM, (unsigned)(p.colorRgb & 0xFFFFFF));
        json += buf;
    }
    json += L"]}";
    PostJson(json);
}

bool CesiumView::GetLookAt(double& latDeg, double& lonDeg, double& altM) const
{
    if (!m_haveLookAt) return false;
    latDeg = m_lookAtLat;
    lonDeg = m_lookAtLon;
    altM   = m_lookAtAlt;
    return true;
}

void CesiumView::SetBounds(const CRect& rc)
{
    if (!GetSafeHwnd()) return;
    MoveWindow(rc);
#ifdef HAVE_WEBVIEW2
    if (m_impl && m_impl->controller)
    {
        RECT rcb; ::GetClientRect(GetSafeHwnd(), &rcb);
        m_impl->controller->put_Bounds(rcb);
    }
#endif
}

void CesiumView::ShowView(bool show)
{
    if (!GetSafeHwnd()) return;
    ShowWindow(show ? SW_SHOW : SW_HIDE);
#ifdef HAVE_WEBVIEW2
    if (m_impl && m_impl->controller)
        m_impl->controller->put_IsVisible(show ? TRUE : FALSE);
#endif
}

void CesiumView::OnSize(UINT nType, int cx, int cy)
{
    CWnd::OnSize(nType, cx, cy);
#ifdef HAVE_WEBVIEW2
    if (m_impl && m_impl->controller)
    {
        RECT rcb = { 0, 0, cx, cy };
        m_impl->controller->put_Bounds(rcb);
    }
#endif
}

void CesiumView::OnDestroy()
{
#ifdef HAVE_WEBVIEW2
    if (m_impl && m_impl->controller)
        m_impl->controller->Close();
#endif
    CWnd::OnDestroy();
}

void CesiumView::OnPaint()
{
    CPaintDC dc(this);
    CRect rc; GetClientRect(&rc);
    dc.FillSolidRect(rc, RGB(0, 0, 0));
#ifndef HAVE_WEBVIEW2
    // Placeholder for the CMake build (no WebView2 SDK).
    dc.SetTextColor(RGB(220, 220, 220));
    dc.SetBkMode(TRANSPARENT);
    dc.DrawText(_T("Cesium 3D requires the Visual Studio build (WebView2)."),
                rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_WORDBREAK);
#endif
}
