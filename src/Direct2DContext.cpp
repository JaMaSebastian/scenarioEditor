//=============================================================================
//  Direct2DContext.cpp
//-----------------------------------------------------------------------------
//  Implements the shared Direct2D/DirectWrite factory accessors. Each factory
//  is created on first use (single-threaded D2D factory, shared DWrite factory)
//  and cached in a process-global; Shutdown() releases both.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-25
//=============================================================================
#include "pch.h"
#include "Direct2DContext.h"

namespace
{
    ID2D1Factory*   g_factory = nullptr;
    IDWriteFactory* g_dwrite  = nullptr;
}

//
// Factory — returns the process-wide ID2D1Factory, creating a single-threaded
//   one on first call. May return null if creation fails.
//
ID2D1Factory* Direct2DContext::Factory()
{
    if (!g_factory)
    {
        D2D1_FACTORY_OPTIONS opts = {};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          __uuidof(ID2D1Factory),
                          &opts,
                          reinterpret_cast<void**>(&g_factory));
    }
    return g_factory;
}

//
// DWrite — returns the process-wide shared IDWriteFactory, creating it on the
//   first call. May return null if creation fails.
//
IDWriteFactory* Direct2DContext::DWrite()
{
    if (!g_dwrite)
    {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&g_dwrite));
    }
    return g_dwrite;
}

//
// Shutdown — releases and nulls both cached factories; call once at process
//   teardown. Safe if the factories were never created.
//
void Direct2DContext::Shutdown()
{
    if (g_dwrite)  { g_dwrite->Release();  g_dwrite  = nullptr; }
    if (g_factory) { g_factory->Release(); g_factory = nullptr; }
}
