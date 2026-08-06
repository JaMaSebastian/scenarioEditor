//=============================================================================
//  Validator.cpp
//-----------------------------------------------------------------------------
//  Implements scenario validation: structural checks (scenario/origin/entity/
//  motion field consistency, duplicate ID detection, PDU + bandwidth estimates)
//  plus optional catalog-driven checks: Name-vs-DIS-type consistency and
//  physical-model envelope checks (speed, turn rate, G load, climb/descent
//  rate, and altitude bounds per airframe profile).
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "Validator.h"
#include "Scenario.h"
#include "EntityTypeCatalog.h"

#include <cctype>
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
    //
    // Add — append an Issue to the report and bump the matching severity counter.
    //
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

    //
    // EntitySubject — formats an Issue subject like "Entity 101 (name)".
    //
    std::string EntitySubject(const Entity& e)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Entity %u (%s)",
                      static_cast<unsigned>(e.entityId),
                      e.name.empty() ? "unnamed" : e.name.c_str());
        return buf;
    }

    //
    // MotionSubject — formats an Issue subject like "Entity 7 / Motion 2"
    // (mi is zero-based; displayed one-based).
    //
    std::string MotionSubject(const Entity& e, size_t mi)
    {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "Entity %u / Motion %zu",
                      static_cast<unsigned>(e.entityId), mi + 1);
        return buf;
    }

    //
    // ResolvePhysMode — resolves an entity's effective physical-model mode,
    // falling back to the scenario default when the entity is set to Inherit.
    //
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

    //
    // EffectiveSpeedMultiplier — the speed scale actually applied to an entity:
    // its own override, else the scenario-wide multiplier, else 1.0.
    //
    double EffectiveSpeedMultiplier(const Entity& e, const Scenario& s)
    {
        if (e.overrideSpeedMultiplier) return e.speedMultiplier;
        if (s.speedMultiplierEnabled)  return s.speedMultiplier;
        return 1.0;
    }

    //
    // LowerTrim — lowercased copy of `v` with surrounding whitespace removed,
    // for case-insensitive name comparison.
    //
    std::string LowerTrim(const std::string& v)
    {
        size_t b = 0, e = v.size();
        while (b < e && std::isspace(static_cast<unsigned char>(v[b])))     ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(v[e - 1]))) --e;
        std::string out = v.substr(b, e - b);
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Check that the entity's free-text Name doesn't contradict its DIS type
    // tuple. The Name is editor-only text with nothing tying it to
    // Kind/Domain/Category/Subcategory, while DISBrowser picks the mesh from
    // the wire tuple alone — so a Name that matches a catalogued platform on a
    // DIFFERENT tuple means the Preview label and the rendered model disagree
    // (the "Entity.1" bug: an entity named "F/A-18 Hornet" carrying the F-16's
    // tuple 1.2.1.1, which at the time drew a nose-aft MQ-9 Reaper). The
    // scenario data is left untouched — this only surfaces the conflict.
    void CheckNameTypeMismatch(const Entity& e, const EntityTypeCatalog& cat, Report& r)
    {
        const std::string want = LowerTrim(e.name);
        if (want.empty())
            return;

        // Walk the catalogued Kind/Domain/Category space once, collecting
        // subcategories whose display name equals the entity's Name. If the
        // entity's OWN tuple is among them, Name and type agree — done.
        struct NameHit { uint8_t k, d, c; uint16_t sc; };
        NameHit firstOther{};
        bool haveOther = false;
        for (const CatalogEntry& k : cat.Kinds())
            for (const CatalogEntry& d : cat.Domains(static_cast<uint8_t>(k.id)))
                for (const CatalogEntry& c : cat.Categories(static_cast<uint8_t>(k.id),
                                                            static_cast<uint8_t>(d.id)))
                    for (const CatalogEntry& sc : cat.Subcategories(static_cast<uint8_t>(k.id),
                                                                    static_cast<uint8_t>(d.id),
                                                                    static_cast<uint8_t>(c.id)))
                    {
                        if (LowerTrim(sc.name) != want)
                            continue;
                        if (k.id == e.kind && d.id == e.domain &&
                            c.id == e.category && sc.id == e.subcategory)
                            return;   // Name matches the tuple actually set.
                        if (!haveOther)
                        {
                            firstOther = { static_cast<uint8_t>(k.id),
                                           static_cast<uint8_t>(d.id),
                                           static_cast<uint8_t>(c.id),
                                           sc.id };
                            haveOther = true;
                        }
                    }
        if (!haveOther)
            return;   // Name isn't a catalogued platform — free text, no opinion.

        // What the entity's current tuple resolves to, for the message.
        std::string current = "no catalogued platform";
        for (const CatalogEntry& sc : cat.Subcategories(e.kind, e.domain, e.category))
            if (sc.id == e.subcategory) { current = "\"" + sc.name + "\""; break; }

        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "Name matches catalog platform \"%s\" (%u.%u.%u.%u) but the DIS type "
                      "set here is %u.%u.%u.%u (%s). DISBrowser picks the model from the "
                      "type tuple, not the Name, so this entity will not render as a %s.",
                      e.name.c_str(),
                      static_cast<unsigned>(firstOther.k),  static_cast<unsigned>(firstOther.d),
                      static_cast<unsigned>(firstOther.c),  static_cast<unsigned>(firstOther.sc),
                      static_cast<unsigned>(e.kind),        static_cast<unsigned>(e.domain),
                      static_cast<unsigned>(e.category),    static_cast<unsigned>(e.subcategory),
                      current.c_str(),
                      e.name.c_str());
        Add(r, Severity::Warning, EntitySubject(e), buf);
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
                    // Speed-authoritative: when speedMps is set, MotionSampler
                    // animates at that rate (entity arrives early + holds),
                    // so envelope checks must use speedMps as the truth.
                    // Fall back to dist/duration for segments authored
                    // without a Speed (legacy behavior).
                    const double effective = (m.speedMps > 0.0)
                                              ? m.speedMps
                                              : (dist / duration);
                    const double speed = effective * mult;

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

                    // Climb / descent rate. With speed-authoritative Line,
                    // the entity traverses the segment in dist/speedMps
                    // seconds (not the full segment duration), so the
                    // effective vertical rate is dAlt × speed / dist.
                    const double travelTime = (m.speedMps > 0.0 && dist > 1e-9)
                                               ? (dist / m.speedMps)
                                               : duration;
                    const double vClimb = (travelTime > 0.0)
                                           ? (dAlt / travelTime)
                                           : 0.0;   // signed
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

//
// Validate — structural pass over scenario, origin, entities and motion
// segments; accumulates Issues and computes pduCount / mbpsAvg. Returns the
// populated Report.
//
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
    // An empty scenario is a valid blank sheet (a brand-new document), not an error:
    // it just produces no PDUs. Entity-level checks below simply iterate zero times.

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

    // Warn only when the scenario HAS entities but none are enabled (probably
    // unintended). A brand-new empty scenario is a valid blank sheet — no message.
    if (!s.entities.empty() && enabledCount == 0)
        Add(r, Severity::Warning, "Scenario",
            "No enabled entities — Playback will emit zero PDUs.");

    r.pduCount = totalPdus;
    if (s.durationSeconds > 0.0)
        r.mbpsAvg = static_cast<double>(totalPdus) * 144.0 * 8.0
                  / 1.0e6 / s.durationSeconds;

    return r;
}

//
// Validate (catalog overload) — runs the structural pass, then layers per-entity
// physical-model envelope checks on each enabled entity whose mode is not Ignore.
//
Report Validator::Validate(const Scenario& s, const EntityTypeCatalog& catalog)
{
    Report r = Validate(s);

    for (const Entity& e : s.entities)
    {
        if (!e.enabled) continue;

        // Name-vs-type consistency runs regardless of the physical-model
        // mode — Ignore only opts out of envelope checks.
        CheckNameTypeMismatch(e, catalog, r);

        // Physical-model envelope checks layered on top of the structural pass.
        const PhysicalModelMode mode = ResolvePhysMode(e, s);
        if (mode == PhysicalModelMode::Ignore) continue;
        CheckEnvelope(e, s, catalog, r);
    }
    return r;
}
