#include "Validator.h"
#include "Scenario.h"

#include <cstdio>
#include <set>
#include <string>
#include <tuple>

using Validator::Issue;
using Validator::Severity;
using Validator::Report;

namespace
{
    void Add(Report& r, Severity sev, const std::string& subject, const std::string& msg)
    {
        Issue i; i.severity = sev; i.subject = subject; i.message = msg;
        switch (sev) {
            case Severity::Error:   ++r.errorCount;   break;
            case Severity::Warning: ++r.warningCount; break;
            case Severity::Info:    ++r.infoCount;    break;
        }
        r.issues.push_back(std::move(i));
    }

    std::string EntitySubject(const Entity& e)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Entity %u (%s)",
                      static_cast<unsigned>(e.entityId),
                      e.name.empty() ? "unnamed" : e.name.c_str());
        return buf;
    }

    std::string MotionSubject(const Entity& e, size_t mi)
    {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "Entity %u / Motion %zu",
                      static_cast<unsigned>(e.entityId), mi + 1);
        return buf;
    }
}

Report Validator::Validate(const Scenario& s)
{
    Report r;

    // ---- Scenario-level ----
    if (s.name.empty())
        Add(r, Severity::Warning, "Scenario", "Scenario name is empty.");
    if (s.protocolVersion != 6 && s.protocolVersion != 7)
        Add(r, Severity::Error, "Scenario",
            "DIS version must be 6 or 7.");
    if (s.exerciseId == 0)
        Add(r, Severity::Warning, "Scenario",
            "ExerciseID is 0 — receivers typically expect 1..255.");
    if (s.durationSeconds <= 0.0)
        Add(r, Severity::Error, "Scenario",
            "DurationSeconds must be greater than zero.");
    if (s.defaultUpdateRateHz <= 0.0)
        Add(r, Severity::Error, "Scenario",
            "DefaultUpdateRateHz must be greater than zero.");

    if (s.originLatDeg < -90.0 || s.originLatDeg > 90.0)
        Add(r, Severity::Error, "Origin", "Origin latitude must be in [-90, 90].");
    if (s.originLonDeg < -180.0 || s.originLonDeg > 180.0)
        Add(r, Severity::Error, "Origin", "Origin longitude must be in [-180, 180].");

    // ---- Entities ----
    if (s.entities.empty())
        Add(r, Severity::Error, "Scenario", "No entities defined.");

    std::set<std::tuple<uint16_t, uint16_t, uint16_t>> seenIds;
    int enabledCount = 0;
    uint64_t totalPdus = 0;

    for (size_t ei = 0; ei < s.entities.size(); ++ei)
    {
        const Entity& e = s.entities[ei];
        const std::string subj = EntitySubject(e);

        if (e.marking.empty())
            Add(r, Severity::Info, subj, "Marking is empty.");
        if (e.siteId == 0 || e.applicationId == 0 || e.entityId == 0)
            Add(r, Severity::Error, subj,
                "Site / Application / Entity IDs must all be non-zero.");

        const auto idTuple = std::make_tuple(e.siteId, e.applicationId, e.entityId);
        if (!seenIds.insert(idTuple).second)
            Add(r, Severity::Error, subj,
                "Duplicate (Site, Application, Entity) ID tuple in this scenario.");

        if (e.lat < -90.0 || e.lat > 90.0)
            Add(r, Severity::Error, subj, "Initial latitude out of [-90, 90].");
        if (e.lon < -180.0 || e.lon > 180.0)
            Add(r, Severity::Error, subj, "Initial longitude out of [-180, 180].");

        if (e.updateRateHz < 0.0)
            Add(r, Severity::Error, subj, "Per-entity UpdateRateHz must be ≥ 0.");

        if (e.enabled) {
            ++enabledCount;
            const double rate = (e.updateRateHz > 0.0) ? e.updateRateHz
                                                       : s.defaultUpdateRateHz;
            if (rate > 0.0)
                totalPdus += static_cast<uint64_t>(rate * s.durationSeconds);
        }

        // ---- Motion segments ----
        double prevEnd = -1e18;
        for (size_t mi = 0; mi < e.motionSegments.size(); ++mi)
        {
            const MotionSegment& m = e.motionSegments[mi];
            const std::string msub = MotionSubject(e, mi);

            if (m.endSecond < m.startSecond)
                Add(r, Severity::Error, msub, "endSecond < startSecond.");
            if (m.startSecond < prevEnd)
                Add(r, Severity::Warning, msub,
                    "Segment start overlaps previous segment.");
            if (m.startSecond > prevEnd + 0.5 && mi > 0)
                Add(r, Severity::Info, msub,
                    "Gap between segments (>0.5 s) — entity will hold prior pose.");

            if (m.type == MotionType::Ellipse) {
                if (m.radiusXMeters <= 0.0 || m.radiusYMeters <= 0.0)
                    Add(r, Severity::Error, msub,
                        "Ellipse radius must be positive.");
                if (m.periodSeconds <= 0.0)
                    Add(r, Severity::Error, msub,
                        "Ellipse period must be positive.");
            }
            if (m.type == MotionType::Line) {
                if (m.startLat == m.endLat &&
                    m.startLon == m.endLon &&
                    m.startAlt == m.endAlt)
                    Add(r, Severity::Info, msub,
                        "Line segment endpoints are identical — equivalent to Stationary.");
            }

            prevEnd = m.endSecond;
        }
    }

    if (enabledCount == 0)
        Add(r, Severity::Warning, "Scenario",
            "No enabled entities — Playback will emit zero PDUs.");

    r.pduCount = totalPdus;
    if (s.durationSeconds > 0.0)
        r.mbpsAvg = static_cast<double>(totalPdus) * 144.0 * 8.0
                  / 1.0e6 / s.durationSeconds;

    return r;
}
