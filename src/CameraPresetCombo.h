//=============================================================================
//  CameraPresetCombo.h
//-----------------------------------------------------------------------------
//  Declares the shared filler for the "Camera" preset dropdown. Three places
//  offer that list — the Preview tab's inline Add row, the New Camera dialog,
//  and (since the dialog gained a Source dropdown) the dialog again on every
//  source change — and all three must agree on the same rules:
//
//    * the source entity's OWN camera type is listed first and preselected;
//    * every other type follows below a separator, so deliberately borrowing
//      another airframe's calibration stays possible;
//    * when the entity has no views of its own, a notice row explains which
//      prerequisite is missing instead of silently offering someone else's.
//
//  Listing the presets flat (as this once did) makes it easy to mount the wrong
//  calibration by accident: "F-18 / back" and "F22 / back" sit a few rows apart
//  in a ~80-entry list, and a mis-picked preset resolves and applies with no
//  complaint, just with the wrong offsets and trim.
//
//  A source-less (Stationary) camera has no mount and therefore no camera type
//  at all — there is only one camera, with no back/front/left/right — so it gets
//  its own single explanatory row via PopulateNoSource.
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-04
//=============================================================================
#pragma once

#include <afxwin.h>
#include <cstdint>
#include <string>

struct Scenario;

namespace CameraPresetCombo
{
    // Item-data marker for the rows that are NOT presets: the "other types"
    // separator, the "no views authored" notice, and the stationary row. Not a
    // valid index into CameraPresets(), so callers must reject it before using
    // a selection as an index.
    constexpr DWORD_PTR kRowNotSelectable = static_cast<DWORD_PTR>(-1);

    // Cameras.ini <Type> for `entityId`, resolved through DISBrowser's Hanger.ini
    // (the same source FDISConfigLoader::ResolveCameraTypeKey uses at runtime).
    // Returns "" when the map could not be loaded (no DISBrowser folder set yet),
    // the id isn't in the scenario, or the tuple has no hanger entry — callers
    // treat all three as "don't filter".
    std::string TypeKeyForEntityId(const Scenario& s, uint16_t entityId);

    // Clear and refill `combo` for a source whose camera type is `typeKey` (which
    // may be ""). Item-data is the index into theApp.CameraPresets().Presets(),
    // or kRowNotSelectable. Returns the index of the first own-type row so the
    // caller can preselect it, or -1 when the entity has none.
    int Populate(CComboBox& combo, const std::string& typeKey);

    // Clear and refill `combo` for a source-less Stationary camera: one
    // non-selectable row saying why there is nothing to choose.
    void PopulateNoSource(CComboBox& combo);
}
