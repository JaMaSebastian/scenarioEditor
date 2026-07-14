//=============================================================================
//  Direct2DContext.h
//-----------------------------------------------------------------------------
//  Declares the Direct2DContext namespace: process-wide, lazily-created owners
//  of the shared Direct2D and DirectWrite factories, plus a Shutdown() to
//  release them. Device-dependent resources stay per-window, not here.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-25
//=============================================================================
#pragma once

#include "pch.h"

// Process-wide owners of the Direct2D / DirectWrite factories. Device-
// independent resources (factories, IDWriteTextFormat) live here so every
// canvas window can share them; device-dependent ones (render targets,
// brushes) stay with each window and are recreated on D2DERR_RECREATE_TARGET.
namespace Direct2DContext
{
    ID2D1Factory*   Factory();
    IDWriteFactory* DWrite();
    void            Shutdown();
}
