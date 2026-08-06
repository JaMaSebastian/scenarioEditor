//=============================================================================
//  SpeedSeed.cpp
//-----------------------------------------------------------------------------
//  Implements SeedCruiseSpeedMps: look the entity's airframe cruise speed up in
//  the app's EntityTypeCatalog; when the catalog has none, log the standard
//  ERROR line and fall back to 10 m/s.
//=============================================================================
#include "pch.h"
#include "SpeedSeed.h"
#include "ScenarioEditor.h"      // theApp / theApp.Catalog()
#include "EntityTypeCatalog.h"   // AirframeProfile
#include "Scenario.h"            // Entity
#include "../log.h"              // LOG / szError

#include <cstdio>

double SeedCruiseSpeedMps(const Entity* e)
{
    if (e)
    {
        const AirframeProfile prof = theApp.Catalog().Profile(
            e->kind, e->domain, e->category, e->subcategory);
        if (prof.valid && prof.cruiseSpeedMps > 0.0)
            return prof.cruiseSpeedMps;

        sprintf_s(szError, sizeof(szError),
            "ERROR! Entity Catalog shows no cruise speed (mps) for entity %s, entity id %u.",
            e->name.c_str(), static_cast<unsigned>(e->entityId));
        LOG(szError);
    }
    else
    {
        LOG("ERROR! Entity Catalog cruise-speed lookup skipped: no active entity; defaulting to 10 m/s.");
    }
    return 10.0;
}
