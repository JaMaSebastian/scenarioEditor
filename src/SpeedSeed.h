//=============================================================================
//  SpeedSeed.h
//-----------------------------------------------------------------------------
//  Shared helper for seeding a motion leg's speed from the entity's airframe
//  CRUISE speed. Used by the Motion Path editor and the Preview "generate
//  default path" helpers so the cruise-lookup-else-default rule (and its error
//  log) live in one place instead of being duplicated at every seed site.
//=============================================================================
#pragma once

struct Entity;

// Resolve the airframe cruise speed (m/s) for the entity's DIS type from the
// loaded EntityTypeCatalog. Returns the catalog cruise speed when present;
// otherwise logs an ERROR naming the entity and returns 10.0 m/s — the default
// for a leg authored without a speed. A null entity logs a generic error and
// also returns 10.0.
double SeedCruiseSpeedMps(const Entity* e);
