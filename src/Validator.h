#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Scenario;

namespace Validator
{
    enum class Severity : uint8_t
    {
        Info    = 0,
        Warning = 1,
        Error   = 2,
    };

    struct Issue
    {
        Severity     severity = Severity::Info;
        std::string  subject;     // "Entity 101" / "Scenario" / "Entity 7 / Motion 2"
        std::string  message;     // human-readable
    };

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

    Report Validate(const Scenario& s);
}
