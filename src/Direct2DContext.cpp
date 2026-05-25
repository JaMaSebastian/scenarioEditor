#include "pch.h"
#include "Direct2DContext.h"

namespace
{
    ID2D1Factory*   g_factory = nullptr;
    IDWriteFactory* g_dwrite  = nullptr;
}

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

void Direct2DContext::Shutdown()
{
    if (g_dwrite)  { g_dwrite->Release();  g_dwrite  = nullptr; }
    if (g_factory) { g_factory->Release(); g_factory = nullptr; }
}
