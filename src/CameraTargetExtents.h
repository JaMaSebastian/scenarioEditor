//=============================================================================
//  CameraTargetExtents.h
//-----------------------------------------------------------------------------
//  Shared helper for resolving the physical size of the entity a CameraFrame
//  points at. Dynamic zoom needs to know how big the target is so DISBrowser can
//  solve an FOV that keeps the whole mesh in frame; the dimensions live in
//  EntityTypeCatalog.ini (LengthMeters / WingspanMeters / HeightMeters), which
//  only the editor reads. So the editor resolves them at author time and stashes
//  them on the frame, and the runtime just does the trigonometry.
//
//  Author:        Matt Sebastian
//  Date started:  2026-07-28
//=============================================================================
#pragma once

struct Scenario;
struct CameraFrame;

namespace CameraTargetExtents
{
    // Resolve the target entity's length/width/height in metres from the loaded
    // EntityTypeCatalog. The framed entity is targetEntityId when non-zero, else
    // the Entity-kind frame's own sourceEntityId (Stationary frames with no target
    // frame nothing). Returns false — leaving the outputs at 0 — when there is no
    // target, the entity is missing, or the catalog has no dimensions for its type.
    // A false return is normal, not an error: plenty of catalog subcategories carry
    // no dimensions at all.
    bool Resolve(const Scenario& s, const CameraFrame& f,
                 double& outLengthM, double& outWidthM, double& outHeightM);

    // Resolve into the frame's targetLengthM/targetWidthM/targetHeightM, zeroing
    // them when unresolved so a stale size can never outlive a target change.
    void Apply(const Scenario& s, CameraFrame& f);
}
