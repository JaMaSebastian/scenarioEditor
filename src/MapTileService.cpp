#include "pch.h"
#include "MapTileService.h"

#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <cmath>
#include <cwchar>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winhttp.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr double kPi  = 3.14159265358979323846;
    constexpr double kTwoPi = 2.0 * kPi;

    // ---- provider description ----
    struct Provider
    {
        const wchar_t* host;        // may contain "{s}" for a subdomain
        const wchar_t* pathTmpl;    // {z}/{x}/{y} order encoded per provider
        bool           esriOrder;   // true => /{z}/{y}/{x}; false => /{z}/{x}/{y}
        const wchar_t* ext;         // native cache extension
    };

    Provider ProviderFor(MapLayer layer)
    {
        switch (layer)
        {
        case MapLayer::Satellite:
            return { L"server.arcgisonline.com",
                     L"/ArcGIS/rest/services/World_Imagery/MapServer/tile",
                     true,  L"jpg" };
        case MapLayer::Topographic:
        default:
            return { L"{s}.tile.opentopomap.org", L"", false, L"png" };
        }
    }

    // Build "host" and "path" for a given tile. Subdomain chosen from x+y.
    void BuildUrl(MapLayer layer, int z, int x, int y,
                  std::wstring& host, std::wstring& path)
    {
        const Provider p = ProviderFor(layer);
        host = p.host;
        const size_t s = host.find(L"{s}");
        if (s != std::wstring::npos)
        {
            const wchar_t sub = L"abc"[(x + y) % 3];
            host.replace(s, 3, std::wstring(1, sub));
        }

        wchar_t buf[256];
        if (layer == MapLayer::Satellite)
            swprintf_s(buf, L"%ls/%d/%d/%d", p.pathTmpl, z, y, x);   // /{z}/{y}/{x}
        else
            swprintf_s(buf, L"/%d/%d/%d.png", z, x, y);              // /{z}/{x}/{y}.png
        path = buf;
    }

    // ---- filesystem ----
    void EnsureDir(const std::wstring& dir)
    {
        if (dir.empty()) return;
        // Create parents first.
        for (size_t i = 0; i < dir.size(); ++i)
        {
            if (dir[i] == L'\\' || dir[i] == L'/')
            {
                if (i >= 2)   // skip drive root "C:\"
                    CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
            }
        }
        CreateDirectoryW(dir.c_str(), nullptr);
    }

    bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20))
        { CloseHandle(h); return false; }
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
        CloseHandle(h);
        if (!ok || got != out.size()) { out.clear(); return false; }
        return true;
    }

    void WriteWholeFile(const std::wstring& path, const std::vector<uint8_t>& data)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD wrote = 0;
        WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr);
        CloseHandle(h);
    }

    // ---- WIC decode to 32bpp premultiplied BGRA ----
    bool DecodeToBgra(IWICImagingFactory* wic, const std::vector<uint8_t>& bytes,
                      MapTilePixels& out)
    {
        if (!wic || bytes.empty()) return false;

        ComPtr<IWICStream> stream;
        if (FAILED(wic->CreateStream(&stream))) return false;
        if (FAILED(stream->InitializeFromMemory(
                const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))))
            return false;

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
            return false;

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return false;

        ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateFormatConverter(&conv))) return false;
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom)))
            return false;

        UINT w = 0, h = 0;
        if (FAILED(conv->GetSize(&w, &h)) || w == 0 || h == 0) return false;

        out.width  = static_cast<int>(w);
        out.height = static_cast<int>(h);
        out.bgra.resize(static_cast<size_t>(w) * h * 4);
        const UINT stride = w * 4;
        const HRESULT hr = conv->CopyPixels(nullptr, stride,
                                            static_cast<UINT>(out.bgra.size()),
                                            out.bgra.data());
        if (FAILED(hr)) { out.bgra.clear(); return false; }
        return true;
    }

    // ---- WinHTTP GET ----
    bool HttpGet(HINTERNET session, const std::wstring& host, const std::wstring& path,
                 std::vector<uint8_t>& out)
    {
        if (!session) return false;
        HINTERNET conn = WinHttpConnect(session, host.c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!conn) return false;

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
        bool ok = false;
        if (req)
        {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr))
            {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(req,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

                if (status == 200)
                {
                    out.clear();
                    DWORD avail = 0;
                    do
                    {
                        avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail)) break;
                        if (avail == 0) break;
                        const size_t base = out.size();
                        out.resize(base + avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(req, out.data() + base, avail, &read))
                        { out.clear(); break; }
                        out.resize(base + read);
                    } while (avail > 0);
                    ok = !out.empty();
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
        return ok;
    }
}

// ---------------------------------------------------------------------------

MapTileService::MapTileService() = default;

MapTileService::~MapTileService()
{
    Stop();
}

void MapTileService::SetCacheDir(const std::wstring& dir) { m_cacheDir = dir; }
void MapTileService::SetOnline(bool online)               { m_online = online; }
void MapTileService::SetNotifyWindow(HWND hwnd)           { m_notify = hwnd; }

void MapTileService::Stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_stop) return;
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

std::wstring MapTileService::DiskPathBase(const Key& k) const
{
    wchar_t buf[64];
    swprintf_s(buf, L"\\%d\\%d\\%d\\%d", k.layer, k.z, k.x, k.y);
    return m_cacheDir + buf;
}

std::shared_ptr<const MapTilePixels> MapTileService::GetTile(MapLayer layer,
                                                             int z, int x, int y)
{
    const Key key{ static_cast<int>(layer), z, x, y };

    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_cache.find(key);
    if (it != m_cache.end())
    {
        const std::shared_ptr<Entry>& e = it->second;
        if (e->state == State::Ready)
        {
            e->lru = ++m_lruClock;
            // Aliasing shared_ptr: keeps the Entry alive, exposes its pixels.
            return std::shared_ptr<const MapTilePixels>(e, &e->px);
        }
        return nullptr;   // Loading or Failed
    }

    // New tile: mark Loading, enqueue, kick the worker (spawned lazily).
    auto e = std::make_shared<Entry>();
    e->state = State::Loading;
    m_cache.emplace(key, e);
    m_queue.push_back(key);
    if (!m_worker.joinable() && !m_stop)
        m_worker = std::thread(&MapTileService::WorkerMain, this);
    m_cv.notify_one();
    return nullptr;
}

void MapTileService::WorkerMain()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&wic));

    HINTERNET session = WinHttpOpen(L"ScenarioEditor/1.0 (map preview)",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session)
        WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);

    for (;;)
    {
        Key key{};
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [&] { return m_stop || !m_queue.empty(); });
            if (m_stop) break;
            key = m_queue.front();
            m_queue.pop_front();
        }

        const MapLayer layer = static_cast<MapLayer>(key.layer);
        const Provider prov = ProviderFor(layer);

        MapTilePixels px;
        bool ok = false;

        // 1) Disk cache / offline pack (try native ext, then the alternates).
        const std::wstring base = DiskPathBase(key);
        std::vector<uint8_t> bytes;
        const wchar_t* exts[] = { prov.ext, L"png", L"jpg", L"jpeg", L"tile" };
        for (const wchar_t* ext : exts)
        {
            if (ReadWholeFile(base + L"." + ext, bytes)) break;
            bytes.clear();
        }

        // 2) Network.
        bool fromNet = false;
        if (bytes.empty() && m_online)
        {
            std::wstring host, path;
            BuildUrl(layer, key.z, key.x, key.y, host, path);
            if (HttpGet(session, host, path, bytes))
                fromNet = true;
        }

        if (!bytes.empty())
            ok = DecodeToBgra(wic.Get(), bytes, px);

        if (ok && fromNet)
        {
            const std::wstring dir = base.substr(0, base.find_last_of(L'\\'));
            EnsureDir(dir);
            WriteWholeFile(base + L"." + prov.ext, bytes);
        }

        // Store result + prune the cache to its cap.
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                it->second->state = ok ? State::Ready : State::Failed;
                if (ok) { it->second->px = std::move(px); it->second->lru = ++m_lruClock; }
            }

            if (m_cache.size() > kMaxCachedTiles)
            {
                // Evict the oldest Ready/Failed entries until under the cap.
                while (m_cache.size() > kMaxCachedTiles)
                {
                    auto oldest = m_cache.end();
                    uint64_t best = UINT64_MAX;
                    for (auto i = m_cache.begin(); i != m_cache.end(); ++i)
                    {
                        if (i->second->state == State::Loading) continue;
                        if (i->second->lru < best) { best = i->second->lru; oldest = i; }
                    }
                    if (oldest == m_cache.end()) break;
                    m_cache.erase(oldest);
                }
            }
        }

        if (m_notify) ::PostMessageW(m_notify, WM_APP_TILE_READY, 0, 0);
    }

    if (session) WinHttpCloseHandle(session);
    wic.Reset();
    CoUninitialize();
}

// ---- Web Mercator helpers -------------------------------------------------

double MapTileService::TileXAtLon(double lonDeg, int z)
{
    return (lonDeg + 180.0) / 360.0 * static_cast<double>(1 << z);
}

double MapTileService::TileYAtLat(double latDeg, int z)
{
    const double lat = latDeg * kPi / 180.0;
    const double n = static_cast<double>(1 << z);
    return (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / kPi) / 2.0 * n;
}

double MapTileService::LonAtTileX(double x, int z)
{
    return x / static_cast<double>(1 << z) * 360.0 - 180.0;
}

double MapTileService::LatAtTileY(double y, int z)
{
    const double n = kPi - kTwoPi * y / static_cast<double>(1 << z);
    return 180.0 / kPi * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

double MapTileService::GroundResolution(double latDeg, int z)
{
    // Metres per pixel at the given latitude/zoom for 256px Web Mercator tiles.
    return 156543.03392804097 * std::cos(latDeg * kPi / 180.0)
         / static_cast<double>(1 << z);
}

int MapTileService::MinZoom(MapLayer) { return 2; }

int MapTileService::MaxZoom(MapLayer layer)
{
    return (layer == MapLayer::Topographic) ? 17 : 19;
}

const wchar_t* MapTileService::Attribution(MapLayer layer)
{
    switch (layer)
    {
    case MapLayer::Satellite:
        return L"Imagery © Esri, Maxar, Earthstar Geographics";
    case MapLayer::Topographic:
        return L"© OpenStreetMap contributors, SRTM | © OpenTopoMap (CC-BY-SA)";
    default:
        return L"";
    }
}
