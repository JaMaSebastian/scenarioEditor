#include "Validator.h"
#include "Scenario.h"
#include "EntityTypeCatalog.h"

#include <cmath>
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

    PhysicalModelMode ResolvePhysMode(const Entity& e, const Scenario& s)
    {
        switch (e.physicalModelOverride) {
            case PhysicalModelOverride::Ignore:   return PhysicalModelMode::Ignore;
            case PhysicalModelOverride::Validate: return PhysicalModelMode::Validate;
            case PhysicalModelOverride::Limit:    return PhysicalModelMode::Limit;
            case PhysicalModelOverride::Inherit:
            default:                              return s.defaultPhysicalModel;
        }
    }

    double EffectiveSpeedMultiplier(const Entity& e, const Scenario& s)
    {
        if (e.overrideSpeedMultiplier) return e.speedMultiplier;
        if (s.speedMultiplierEnabled)  return s.speedMultiplier;
        return 1.0;
    }

    // Run envelope checks for one entity. Adds Issues to `r` per violation.
    // Skips silently when the catalog has no AirframeProfile for the entity's
    // type tuple, or when the profile is found but the relevant field is
    // unset (== 0).
    void CheckEnvelope(const Entity& e, const Scenario& s,
                       const EntityTypeCatalog& cat, Report& r)
    {
        const AirframeProfile p = cat.Profile(e.kind, e.domain, e.category, e.subcategory);
        const std::string subj = EntitySubject(e);
        if (!p.valid)
        {
            Add(r, Severity::Info, subj,
                "No airframe profile catalogued for this entity type "
                "(extend EntityTypeCatalog.ini [Airframe.k.d.c.sc] to enable physical-model checks).");
            return;
        }

        const double mult = EffectiveSpeedMultiplier(e, s);

        // Per-segment checks.
        for (size_t mi = 0; mi < e.motionSegments.size(); ++mi)
        {
            const MotionSegment& m = e.motionSegments[mi];
            const std::string msub = MotionSubject(e, mi);

            if (m.type == MotionType::Ellipse)
            {
                const double a = 0.5 * m.lengthMeters;   // semi-major
                const double speed = m.speedMps * mult;
                if (a > 0.0 && speed > 0.0)
                {
                    const double angularDps = (speed / a) * (180.0 / 3.14159265358979323846);
                    char buf[200];
                    if (p.neverExceedMps > 0.0 && speed > p.neverExceedMps) {
                        std::snprintf(buf, sizeof(buf),
                                      "%s: Ellipse tangential speed %.1f m/s exceeds NeverExceedSpeed %.1f m/s.",
                                      p.name.empty() ? "Airframe" : p.name.c_str(),
                                      speed, p.neverExceedMps);
                        Add(r, Severity::Error, msub, buf);
                    } else if (p.maxSpeedMps > 0.0 && speed > p.maxSpeedMps) {
                        std::snprintf(buf, sizeof(buf),
                                      "%s: Ellipse tangential speed %.1f m/s exceeds MaxSpeed %.1f m/s.",
                                      p.name.empty() ? "Airframe" : p.name.c_str(),
                                      speed, p.maxSpeedMps);
                        Add(r, Severity::Warning, msub, buf);
                    }
                    if (p.stallSpeedMps > 0.0 && speed < p.stallSpeedMps) {
                        std::snprintf(buf, sizeof(buf),
                                      "%s: Ellipse tangential speed %.1f m/s below StallSpeed %.1f m/s.",
                                      p.name.empty() ? "Airframe" : p.name.c_str(),
                                      speed, p.stallSpeedMps);
                        Add(r, Severity::Warning, msub, buf);
                    }
                    if (p.maxTurnRateDps > 0.0 && angularDps > p.maxTurnRateDps) {
                        std::snprintf(buf, sizeof(buf),
                                      "%s: Ellipse angular rate %.1f deg/s exceeds MaxTurnRate %.1f deg/s.",
                                      p.name.empty() ? "Airframe" : p.name.c_str(),
                                      angularDps, p.maxTurnRateDps);
                        Add(r, Severity::Warning, msub, buf);
                    }
                    if (p.maxG > 0.0) {
                        // Centripetal G = v² / (a · g) (using semi-major as
                        // effective radius for a non-circular orbit).
                        const double centripG = (speed * speed) / (a * 9.80665);
                        if (centripG > p.maxG) {
                            std::snprintf(buf, sizeof(buf),
                                          "%s: Ellipse centripetal load %.1f G exceeds MaxG %.1f.",
                                          p.name.empty() ? "Airframe" : p.name.c_str(),
                                          centripG, p.maxG);
                            Add(r, Severity::Error, msub, buf);
                        }
                    }
                }

                // Ellipse altitude: mean of the two foci altitudes (we
                // assume horizontal orbits in Phase 1). In Local mode the
                // altitude is originAltM + mean(f1LocalZ, f2LocalZ).
                double altM = 0.0;
                if (m.coordMode == CoordMode::Local)
                    altM = s.originAltM + 0.5 * (m.f1LocalZ + m.f2LocalZ);
                else
                    altM = 0.5 * (m.f1Alt + m.f2Alt);
                if (p.serviceCeilingM > 0.0 && altM > p.serviceCeilingM) {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf),
                                  "%s: Ellipse altitude %.0f m exceeds ServiceCeiling %.0f m.",
                                  p.name.empty() ? "Airframe" : p.name.c_str(),
                                  altM, p.serviceCeilingM);
                    Add(r, Severity::Warning, msub, buf);
                }
                if (p.minAltM > 0.0 && altM < p.minAltM) {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf),
                                  "%s: Ellipse altitude %.0f m below MinAltitude %.0f m.",
                                  p.name.empty() ? "Airframe" : p.name.c_str(),
                                  altM, p.minAltM);
                    Add(r, Severity::Warning, msub, buf);
                }
            }
            else if (m.type == MotionType::Line)
            {
                const double duration = m.endSecond - m.startSecond;
                if (duration > 0.0)
                {
                    // Use lat/lon/alt deltas to estimate path length. Quick
                    // and dirty: 1 degree latitude ≈ 111 320 m, longitude
                    // scaled by cos(midLat). Altitude added quadratically.
                    const double dLat = m.endLat - m.startLat;
                    const double dLon = m.endLon - m.startLon;
                    const double dAlt = m.endAlt - m.startAlt;
                    const double midLat = (m.startLat + m.endLat) * 0.5;
                    const double m_per_deg_lat = 111320.0;
                    const double m_per_deg_lon = 111320.0 * std::cos(midLat * 3.14159265358979323846 / 180.0);
                    const double horiz = std::sqrt((dLat * m_per_deg_lat) * (dLat * m_per_deg_lat)
                                                 + (dLon * m_per_deg_lon) * (dLon * m_per_deg_lon));
                    const double dist  = std::sqrt(horiz * horiz + dAlt * dAlt);
                    const double speed = (dist / duration) * mult;

                    char buf[200];
                    if (p.neverExceedMps > 0.0 && speed > p.neverExceedMps) {
                        std::snprintf(buf, sizeof(buf),
                                      "%s: Line speed %.1f m/s exceeds NeverExceedSpeed %.1f m/s.",
                                      p.name.empty() ? "Airframe" : p.name.c_str(),
                                      speed, p.neverExceedMps);
                        Add(r, Severity::Error, msub, buf);
                    } else if (p.maxSpeedMps > 0.0 && speed > p.maxSpeedMps) {
                        std::snprintf(buf, sizeof(buf),
                                      "%s: Line speed %.1f m/s exceeds MaxSpeed %.1f m/s.",
                                      p.name.empty() ? "Airframe" : p.name.c_str(),
                                      speed, p.maxSpeedMps);
                        Add(r, Severity::Warning, msub, buf);
                    }

                    // Climb / descent rate.
                    const double vClimb = dAlt / duration;   // signed
                    if (vClimb > 0 && p.maxClimbRateMps > 0.0 && vClimb > p.maxClimbRateMps) {
                        std::snprintf(buf, sizeof(buf),
                                      "%s: Line climb rate %.1f m/s exceeds MaxClimbRate %.1f m/s.",
                                      p.name.empty() ? "Airframe" : p.name.c_str(),
                                      vClimb, p.maxClimbRateMps);
                        Add(r, Severity::Warning, msub, buf);
                    }
                    if (vClimb < 0 && p.maxDescentRateMps > 0.0 && -vClimb > p.maxDescentRateMps) {
                        std::snprintf(buf, sizeof(buf),
                                      "%s: Line descent rate %.1f m/s exceeds MaxDescentRate %.1f m/s.",
                                      p.name.empty() ? "Airframe" : p.name.c_str(),
                                      -vClimb, p.maxDescentRateMps);
                        Add(r, Severity::Warning, msub, buf);
                    }
                }

                // Altitude bounds at both endpoints (LatLonAlt mode only —
                // Local/ECEF resolution deferred).
                if (m.coordMode == CoordMode::LatLonAlt) {
                    for (double a : { m.startAlt, m.endAlt }) {
                        if (p.serviceCeilingM > 0.0 && a > p.serviceCeilingM) {
                            char buf[200];
                            std::snprintf(buf, sizeof(buf),
                                          "%s: Line altitude %.0f m exceeds ServiceCeiling %.0f m.",
                                          p.name.empty() ? "Airframe" : p.name.c_str(),
                                          a, p.serviceCeilingM);
                            Add(r, Severity::Warning, msub, buf);
                        }
                    }
                }
            }
        }
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
                if (m.lengthMeters <= 0.0)
                    Add(r, Severity::Error, msub,
                        "Ellipse Length must be positive.");
                if (m.speedMps <= 0.0)
                    Add(r, Severity::Error, msub,
                        "Ellipse Speed must be positive.");
                // 2a >= 2c (the foci must be inside the orbit). Reject
                // length < focus distance.
                double f1Lat = m.f1Lat, f1Lon = m.f1Lon, f1Alt = m.f1Alt;
                double f2Lat = m.f2Lat, f2Lon = m.f2Lon, f2Alt = m.f2Alt;
                (void)f1Alt; (void)f2Alt; // unused in this check; suppress warning
                // Crude planar focus distance using (lat,lon) at mean lat —
                // good enough to catch grossly-mis-set Length values.
                const double midLat = 0.5 * (f1Lat + f2Lat);
                const double mPerDegLat = 111320.0;
                const double mPerDegLon = 111320.0 *
                    std::cos(midLat * 3.14159265358979323846 / 180.0);
                const double dE = (f2Lon - f1Lon) * mPerDegLon;
                const double dN = (f2Lat - f1Lat) * mPerDegLat;
                const double focusDist = std::sqrt(dE * dE + dN * dN);
                if (m.lengthMeters > 0.0 && m.lengthMeters < focusDist)
                    Add(r, Severity::Error, msub,
                        "Ellipse Length is shorter than the distance between the two foci.");
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

Report Validator::Validate(const Scenario& s, const EntityTypeCatalog& catalog)
{
    Report r = Validate(s);

    // Physical-model envelope checks layered on top of the structural pass.
    for (const Entity& e : s.entities)
    {
        if (!e.enabled) continue;
        const PhysicalModelMode mode = ResolvePhysMode(e, s);
        if (mode == PhysicalModelMode::Ignore) continue;
        CheckEnvelope(e, s, catalog, r);
    }
    return r;
}
