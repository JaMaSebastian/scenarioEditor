//=============================================================================
//  Validator.h
//-----------------------------------------------------------------------------
//  Declares the scenario validation API: severity levels, per-issue and
//  aggregate Report structures (including PDU/bandwidth estimates), and the
//  Validate() entry points. One overload runs structural checks; the
//  catalog-aware overload adds physical-model envelope checks per entity.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Scenario;
class  EntityTypeCatalog;

namespace Validator
{
    // Severity — ordinal ranking of an issue (Info < Warning < Error).
    enum class Severity : uint8_t
    {
        Info    = 0,
        Warning = 1,
        Error   = 2,
    };

    //-----------------------------------------------------------------------------
    // Issue — one validation finding: a severity, the subject it concerns, and a
    //   human-readable message.
    //-----------------------------------------------------------------------------
    struct Issue
    {
        Severity     severity = Severity::Info;
        std::string  subject;     // "Entity 101" / "Scenario" / "Entity 7 / Motion 2"
        std::string  message;     // human-readable
    };

    //-----------------------------------------------------------------------------
    // Report — full result of a validation pass: the list of Issues, per-severity
    //   counts, and the estimated PDU count / average bandwidth for the scenario.
    //-----------------------------------------------------------------------------
    struct Report
    {
        std::vector<Issue> issues;
        int errorCount    = 0;
        int warningCount  = 0;
        int infoCount     = 0;

        // PDU and bandwidth estimates over scenario.durationSeconds.
        // pduCount = Σ over enabled entities of (rateHz × duration).
        // mbpsAvg  = pduCount × 144 bytes × 8 / (1e6 × duration).
        uint64_t pduCount = 0;
        double   mbpsAvg  = 0.0;
    };

    // Structural pass: checks scenario/entity/motion fields for internal
    // consistency and computes the PDU + bandwidth estimates.
    Report Validate(const Scenario& s);

    // Catalog-aware overload: in addition to the structural checks, also
    // run physical-model envelope checks for each entity whose effective
    // mode resolves to Validate or Limit. Skips silently when the entity's
    // Kind/Domain/Category/Subcategory has no catalogued AirframeProfile.
    Report Validate(const Scenario& s, const EntityTypeCatalog& catalog);
}
