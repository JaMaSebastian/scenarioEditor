//=============================================================================
//  CameraTargetExtents.cpp
//-----------------------------------------------------------------------------
//  Implements the CameraFrame -> target-entity -> AirframeProfile lookup that
//  seeds a frame's cached target dimensions. Kept out of ScenarioIO on purpose:
//  this needs theApp (for the catalog), and ScenarioIO is also linked into the
//  headless scenario_io_roundtrip tool, which has no MFC app object.
//=============================================================================
#include "pch.h"
#include "CameraTargetExtents.h"
#include "ScenarioEditor.h"      // theApp / theApp.Catalog()
#include "EntityTypeCatalog.h"   // AirframeProfile
#include "Scenario.h"            // Scenario / CameraFrame / Entity

namespace
{
    //
    // FramedEntityId — which entity a frame is actually looking at.
    //   An explicit targetEntityId always wins; otherwise an Entity camera falls
    //   back to focusing on the entity it is mounted on. A Stationary frame with
    //   no target frames nothing, so it yields 0.
    //
    uint16_t FramedEntityId(const CameraFrame& f)
    {
        if (f.targetEntityId != 0)              return f.targetEntityId;
        if (f.kind == CameraKind::Entity)       return f.sourceEntityId;
        return 0;
    }
}

//
// Resolve — see header. Missing dimensions are left at 0 rather than guessed;
//   DISBrowser treats a zero size as "no dynamic fit possible" and holds the
//   static FOV instead of framing something the wrong size.
//
bool CameraTargetExtents::Resolve(const Scenario& s, const CameraFrame& f,
                                  double& outLengthM, double& outWidthM, double& outHeightM)
{
    outLengthM = outWidthM = outHeightM = 0.0;

    const uint16_t id = FramedEntityId(f);
    if (id == 0) return false;

    const Entity* target = nullptr;
    for (const Entity& e : s.entities)
        if (e.entityId == id) { target = &e; break; }
    if (!target) return false;

    const AirframeProfile prof = theApp.Catalog().Profile(
        target->kind, target->domain, target->category, target->subcategory);
    if (!prof.valid) return false;

    outLengthM = prof.lengthM;
    outWidthM  = prof.wingspanM;
    outHeightM = prof.heightM;

    // A catalogued type with every dimension blank is no more useful than an
    // uncatalogued one — report it the same way.
    return (outLengthM > 0.0 || outWidthM > 0.0 || outHeightM > 0.0);
}

//
// Apply — see header.
//
void CameraTargetExtents::Apply(const Scenario& s, CameraFrame& f)
{
    Resolve(s, f, f.targetLengthM, f.targetWidthM, f.targetHeightM);
}
