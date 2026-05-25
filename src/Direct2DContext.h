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
