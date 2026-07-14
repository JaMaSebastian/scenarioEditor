//=============================================================================
//  MapTileService.h
//-----------------------------------------------------------------------------
//  Declares MapTileService, a threaded XYZ/"slippy" raster-tile provider for
//  the Preview map backdrop, plus the MapLayer enum, the MapTilePixels result
//  struct, and static Web Mercator projection helpers shared with the canvas.
//  Returns device-independent BGRA pixels; the canvas builds Direct2D bitmaps.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-10
//=============================================================================
#pragma once

#include "pch.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <string>

// Background raster-tile provider for the Preview map backdrop. Fetches standard
// XYZ / "slippy" tiles (Web Mercator, 256x256) from no-key providers, decodes
// them to device-independent BGRA pixels, and caches them in memory + on disk.
//
// Everything here is device-INDEPENDENT: the canvas turns the returned pixels
// into ID2D1Bitmaps itself (device-dependent) and keeps its own bitmap cache.
// Loads happen on a worker thread; when one finishes, WM_APP_TILE_READY is
// posted to the notify window so the canvas repaints and picks up the new tile.

enum class MapLayer : int
{
    None        = 0,
    Satellite   = 1,   // ESRI World Imagery (aerial)
    Topographic = 2,   // OpenTopoMap (contour/relief)
    Cesium      = 3,   // 3D globe (WebView2 + CesiumJS); not a raster layer
};

// Posted (WM_APP + 7) to the notify window when an async tile load completes.
#ifndef WM_APP_TILE_READY
#define WM_APP_TILE_READY (WM_APP + 7)
#endif

//-----------------------------------------------------------------------------
// MapTilePixels — a decoded tile's raw pixels
//   width x height 32bpp premultiplied BGRA image, device-independent so any
//   thread may produce it and the canvas turns it into an ID2D1Bitmap.
//-----------------------------------------------------------------------------
struct MapTilePixels
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> bgra;   // width*height*4, 32bpp premultiplied BGRA
};

//-----------------------------------------------------------------------------
// MapTileService — async, cached raster-tile fetcher
//   Serves tiles from an in-memory LRU cache; on a miss, a single worker thread
//   reads the on-disk cache / offline pack, falls back to a no-key HTTPS
//   provider, decodes via WIC, and posts WM_APP_TILE_READY so the canvas
//   repaints. Also exposes static Web Mercator lat/lon<->tile conversions.
//-----------------------------------------------------------------------------
class MapTileService
{
public:
    MapTileService();
    ~MapTileService();

    MapTileService(const MapTileService&)            = delete;
    MapTileService& operator=(const MapTileService&) = delete;

    // Where downloaded tiles are cached (and where an offline tile pack can be
    // dropped): <dir>/<layer>/<z>/<x>/<y>.<ext>. Set once at startup.
    void SetCacheDir(const std::wstring& dir);
    // false => never hit the network (offline: disk cache only).
    void SetOnline(bool online);
    // Window that receives WM_APP_TILE_READY when a background load completes.
    void SetNotifyWindow(HWND hwnd);

    // Return the tile's pixels if already decoded in memory; otherwise return
    // nullptr and (once) enqueue a background load. The returned shared_ptr keeps
    // the pixels alive even if the cache evicts the entry, so the canvas can
    // safely build its ID2D1Bitmap from it.
    std::shared_ptr<const MapTilePixels> GetTile(MapLayer layer, int z, int x, int y);

    // Stop the worker thread (call before the notify window is destroyed).
    void Stop();

    // ---- Web Mercator helpers (shared with the canvas' ENU integration) ----
    static double TileXAtLon(double lonDeg, int z);
    static double TileYAtLat(double latDeg, int z);
    static double LonAtTileX(double x, int z);
    static double LatAtTileY(double y, int z);
    static double GroundResolution(double latDeg, int z);   // metres per pixel

    static int          MinZoom(MapLayer layer);
    static int          MaxZoom(MapLayer layer);
    static const wchar_t* Attribution(MapLayer layer);

private:
    struct Key
    {
        int layer, z, x, y;
        bool operator==(const Key& o) const
        { return layer == o.layer && z == o.z && x == o.x && y == o.y; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            uint64_t h = (uint64_t)(uint32_t)k.x * 0x9E3779B1u;
            h ^= (uint64_t)(uint32_t)k.y * 0x85EBCA77u + (h << 6);
            h ^= ((uint64_t)k.z << 3) ^ ((uint64_t)k.layer << 1);
            return (size_t)(h ^ (h >> 29));
        }
    };
    enum class State { Loading, Ready, Failed };
    struct Entry
    {
        State         state = State::Loading;
        MapTilePixels px;
        uint64_t      lru = 0;   // touched-order for eviction
    };

    void WorkerMain();
    // Base cache path for a tile, WITHOUT extension (<dir>/<layer>/<z>/<x>/<y>).
    std::wstring DiskPathBase(const Key& k) const;

    std::wstring                                    m_cacheDir;
    bool                                            m_online = true;
    HWND                                            m_notify = nullptr;

    std::mutex                                      m_mutex;
    std::condition_variable                         m_cv;
    std::unordered_map<Key, std::shared_ptr<Entry>, KeyHash> m_cache;
    std::deque<Key>                                 m_queue;
    uint64_t                                        m_lruClock = 1;
    bool                                            m_stop = false;
    std::thread                                     m_worker;

    static constexpr size_t kMaxCachedTiles = 384;   // ~96 MB worst case
};
