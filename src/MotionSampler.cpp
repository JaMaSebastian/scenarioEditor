#include "MotionSampler.h"
#include "CoordTransforms.h"
#include "Scenario.h"

#include <algorithm>
#include <cmath>

namespace
{
    double Lerp(double a, double b, double u) { return a + (b - a) * u; }

    // Resolve a segment's start position to ECEF + geodetic lat/lon/alt,
    // honoring its coordinate mode. We need both forms so we can lerp in
    // the segment's native frame and still produce ECEF for the wire.
    struct ResolvedPoint { double lat, lon, alt, x, y, z; };

    ResolvedPoint ResolveStart(const MotionSegment& s, const Scenario* scenario)
    {
        ResolvedPoint p{};
        switch (s.coordMode) {
            case CoordMode::LatLonAlt:
                p.lat = s.startLat; p.lon = s.startLon; p.alt = s.startAlt;
                CoordTransforms::GeodeticToEcefDeg(p.lat, p.lon, p.alt, p.x, p.y, p.z);
                break;
            case CoordMode::ECEF:
                p.x = s.startEcefX; p.y = s.startEcefY; p.z = s.startEcefZ;
                CoordTransforms::EcefToGeodeticDeg(p.x, p.y, p.z, p.lat, p.lon, p.alt);
                break;
            case CoordMode::Local:
                if (scenario) {
                    CoordTransforms::LocalEnuToEcefDeg(
                        s.startLocalX, s.startLocalY, s.startLocalZ,
                        scenario->originLatDeg, scenario->originLonDeg, scenario->originAltM,
                        p.x, p.y, p.z);
                } else {
                    p.x = s.startEcefX; p.y = s.startEcefY; p.z = s.startEcefZ;
                }
                CoordTransforms::EcefToGeodeticDeg(p.x, p.y, p.z, p.lat, p.lon, p.alt);
                break;
        }
        return p;
    }

    ResolvedPoint ResolveEnd(const MotionSegment& s, const Scenario* scenario)
    {
        ResolvedPoint p{};
        switch (s.coordMode) {
            case CoordMode::LatLonAlt:
                p.lat = s.endLat; p.lon = s.endLon; p.alt = s.endAlt;
                CoordTransforms::GeodeticToEcefDeg(p.lat, p.lon, p.alt, p.x, p.y, p.z);
                break;
            case CoordMode::ECEF:
                p.x = s.endEcefX; p.y = s.endEcefY; p.z = s.endEcefZ;
                CoordTransforms::EcefToGeodeticDeg(p.x, p.y, p.z, p.lat, p.lon, p.alt);
                break;
            case CoordMode::Local:
                if (scenario) {
                    CoordTransforms::LocalEnuToEcefDeg(
                        s.endLocalX, s.endLocalY, s.endLocalZ,
                        scenario->originLatDeg, scenario->originLonDeg, scenario->originAltM,
                        p.x, p.y, p.z);
                } else {
                    p.x = s.endEcefX; p.y = s.endEcefY; p.z = s.endEcefZ;
                }
                CoordTransforms::EcefToGeodeticDeg(p.x, p.y, p.z, p.lat, p.lon, p.alt);
                break;
        }
        return p;
    }
}

SampledPose MotionSampler::EvaluateSegment(const MotionSegment& s, double u,
                                           const Scenario* scenario,
                                           bool geometricMode)
{
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;

    SampledPose out{};
    switch (s.type) {
        case MotionType::StopHold:    // fallthrough — caller treats as Stationary
                                      // when invoked in isolation; SamplePose
                                      // injects the prior segment's end pose.
        case MotionType::Stationary: {
            const ResolvedPoint start = ResolveStart(s, scenario);
            out.ecefX = start.x;
            out.ecefY = start.y;
            out.ecefZ = start.z;
            out.headingDeg = s.startHeadingDeg;
            out.pitchDeg   = s.startPitchDeg;
            out.rollDeg    = s.startRollDeg;
            break;
        }
        case MotionType::Line: {
            const ResolvedPoint start = ResolveStart(s, scenario);
            const ResolvedPoint end   = ResolveEnd(s, scenario);
            const double lat = Lerp(start.lat, end.lat, u);
            const double lon = Lerp(start.lon, end.lon, u);
            const double alt = Lerp(start.alt, end.alt, u);
            CoordTransforms::GeodeticToEcefDeg(lat, lon, alt,
                                               out.ecefX, out.ecefY, out.ecefZ);

            // Heading: if start/end heading are BOTH at the default 0, the
            // user almost certainly hasn't authored an orientation and
            // expects the entity to face along the path. Compute heading
            // from the geodetic delta (East/North bearing). Otherwise lerp
            // the authored values.
            if (s.startHeadingDeg == 0.0 && s.endHeadingDeg == 0.0)
            {
                const double dLat = end.lat - start.lat;
                const double dLon = end.lon - start.lon;
                if (dLat != 0.0 || dLon != 0.0)
                {
                    constexpr double kPi = 3.14159265358979323846;
                    const double midLatRad = 0.5 * (start.lat + end.lat) * (kPi / 180.0);
                    const double dEast  = dLon * 111320.0 * std::cos(midLatRad);
                    const double dNorth = dLat * 111320.0;
                    // Compass bearing: atan2(East, North), 0 = North, 90 = East.
                    double bearingDeg = std::atan2(dEast, dNorth) * (180.0 / kPi);
                    if (bearingDeg < 0.0) bearingDeg += 360.0;
                    out.headingDeg = bearingDeg;
                }
                else
                {
                    out.headingDeg = 0.0;
                }
            }
            else
            {
                out.headingDeg = Lerp(s.startHeadingDeg, s.endHeadingDeg, u);
            }
            out.pitchDeg   = Lerp(s.startPitchDeg,   s.endPitchDeg,   u);
            out.rollDeg    = Lerp(s.startRollDeg,    s.endRollDeg,    u);
            break;
        }
        case MotionType::Ellipse: {
            // Two-foci + length parametrization. Resolve both foci to
            // geodetic, project both into ENU at the mean foci position,
            // build the ellipse from (center, a=length/2, c=focusDist/2),
            // start at the bearing intercept, sweep with speed.
            constexpr double kTau = 6.28318530717958647692;
            constexpr double kPi  = 3.14159265358979323846;

            double f1Lat = s.f1Lat, f1Lon = s.f1Lon, f1Alt = s.f1Alt;
            double f2Lat = s.f2Lat, f2Lon = s.f2Lon, f2Alt = s.f2Alt;

            // Resolve each focus to geodetic according to coordMode.
            if (s.coordMode == CoordMode::Local && scenario)
            {
                double ex, ey, ez;
                CoordTransforms::LocalEnuToEcefDeg(
                    s.f1LocalX, s.f1LocalY, s.f1LocalZ,
                    scenario->originLatDeg, scenario->originLonDeg, scenario->originAltM,
                    ex, ey, ez);
                CoordTransforms::EcefToGeodeticDeg(ex, ey, ez, f1Lat, f1Lon, f1Alt);
                CoordTransforms::LocalEnuToEcefDeg(
                    s.f2LocalX, s.f2LocalY, s.f2LocalZ,
                    scenario->originLatDeg, scenario->originLonDeg, scenario->originAltM,
                    ex, ey, ez);
                CoordTransforms::EcefToGeodeticDeg(ex, ey, ez, f2Lat, f2Lon, f2Alt);
            }
            else if (s.coordMode == CoordMode::ECEF)
            {
                CoordTransforms::EcefToGeodeticDeg(s.f1EcefX, s.f1EcefY, s.f1EcefZ,
                                                   f1Lat, f1Lon, f1Alt);
                CoordTransforms::EcefToGeodeticDeg(s.f2EcefX, s.f2EcefY, s.f2EcefZ,
                                                   f2Lat, f2Lon, f2Alt);
            }

            // Mean geodetic position used as ENU origin for planar math.
            const double midLat = 0.5 * (f1Lat + f2Lat);
            const double midLon = 0.5 * (f1Lon + f2Lon);
            const double midAlt = 0.5 * (f1Alt + f2Alt);

            // Project both foci to ENU at midLat/midLon/midAlt.
            double f1Ex, f1Ey, f1Ez;
            CoordTransforms::GeodeticToEcefDeg(f1Lat, f1Lon, f1Alt, f1Ex, f1Ey, f1Ez);
            double f2Ex, f2Ey, f2Ez;
            CoordTransforms::GeodeticToEcefDeg(f2Lat, f2Lon, f2Alt, f2Ex, f2Ey, f2Ez);
            double e1, n1, u1, e2, n2, u2;
            CoordTransforms::EcefToLocalEnuDeg(f1Ex, f1Ey, f1Ez,
                                               midLat, midLon, midAlt, e1, n1, u1);
            CoordTransforms::EcefToLocalEnuDeg(f2Ex, f2Ey, f2Ez,
                                               midLat, midLon, midAlt, e2, n2, u2);

            // Ellipse geometry in the ENU plane.
            const double dx = e2 - e1;
            const double dy = n2 - n1;
            const double focusDist = std::sqrt(dx * dx + dy * dy);
            const double c = 0.5 * focusDist;
            const double a = 0.5 * std::max(s.lengthMeters, 2.0 * c);  // 2a >= 2c
            const double b = std::sqrt(std::max(0.0, a * a - c * c));

            // Major-axis unit vector in ENU. For circles (c=0): default +East.
            double majorE = 1.0, majorN = 0.0;
            if (focusDist > 1e-9)
            {
                majorE = dx / focusDist;
                majorN = dy / focusDist;
            }

            // Period from speed via Ramanujan circumference approx.
            const double circumference = kPi *
                (3.0 * (a + b) - std::sqrt((3.0 * a + b) * (a + 3.0 * b)));
            const double speed  = (s.speedMps > 0.0) ? s.speedMps : 100.0;
            const double period = (circumference > 0.0) ? (circumference / speed) : 1.0;
            const double segDuration = s.endSecond - s.startSecond;

            // Start parameter angle: place the entity at the orbit point
            // that lies on the bearing line from center. Compass bearing
            // (0=N, 90=E ...) maps to math angle (measured CCW from +East).
            const double bearingMath = (90.0 - s.startBearingDeg) * (kTau / 360.0);
            // Bearing direction in ENU plane:
            const double bxE = std::cos(bearingMath);
            const double bxN = std::sin(bearingMath);
            // Express the bearing direction in the ELLIPSE-LOCAL frame
            // (major-axis aligned). Local +X = (majorE, majorN).
            const double bLocalX =  bxE * majorE + bxN * majorN;   // along major
            const double bLocalY = -bxE * majorN + bxN * majorE;   // along minor
            // Parametric angle θ such that (a·cos θ, b·sin θ) points along
            // (bLocalX, bLocalY) is θ = atan2(bLocalY / b, bLocalX / a).
            const double theta0 = std::atan2(bLocalY * a, bLocalX * b);

            // Sweep direction. Clockwise from above = decreasing math angle.
            const double sign = (s.direction == EllipseDirection::Clockwise) ? -1.0 : 1.0;
            // In geometric mode, u ∈ [0,1] maps to exactly one full
            // revolution — used by the path renderer so the trace covers
            // the ellipse once regardless of how many orbits fit in the
            // segment time window. Otherwise use the time-coupled formula
            // that respects speed and segment duration.
            const double theta = geometricMode
                ? (theta0 + sign * kTau * u)
                : (theta0 + sign * kTau * (u * segDuration / period));

            // Parametric position in ellipse-local frame.
            const double lx = a * std::cos(theta);
            const double ly = b * std::sin(theta);
            // Rotate back to ENU plane (major axis aligned with majorE/N).
            const double east  = 0.5 * (e1 + e2) + lx * majorE - ly * majorN;
            const double north = 0.5 * (n1 + n2) + lx * majorN + ly * majorE;
            const double up    = 0.5 * (u1 + u2);

            // Back to ECEF.
            CoordTransforms::LocalEnuToEcefDeg(east, north, up,
                                               midLat, midLon, midAlt,
                                               out.ecefX, out.ecefY, out.ecefZ);

            // Tangential heading: derivative of (east, north) w.r.t. θ.
            const double dlx = -a * std::sin(theta);
            const double dly =  b * std::cos(theta);
            const double dEast  = (dlx * majorE - dly * majorN) * sign;
            const double dNorth = (dlx * majorN + dly * majorE) * sign;
            // Heading (compass deg, 0=N, 90=E) from velocity vector.
            const double headingRad = std::atan2(dEast, dNorth);
            out.headingDeg = headingRad * (360.0 / kTau);
            out.pitchDeg   = s.startPitchDeg;
            out.rollDeg    = s.startRollDeg;
            break;
        }
    }
    return out;
}

SampledPose MotionSampler::SamplePose(const Entity& entity, const Scenario& scenario,
                                      double scenarioTimeSec)
{
    const Scenario* const scn = &scenario;
    SampledPose out{};

    if (entity.motionSegments.empty())
    {
        // No motion authored — emit the entity's static initial pose,
        // matching the Slice 1 "Hello DIS" behavior.
        out.ecefX = entity.ecefX;
        out.ecefY = entity.ecefY;
        out.ecefZ = entity.ecefZ;
        out.headingDeg = entity.headingDeg;
        out.pitchDeg   = entity.pitchDeg;
        out.rollDeg    = entity.rollDeg;
        return out;
    }

    // Locate the active segment. Segments are expected to be ordered by
    // startSecond; we don't reorder here (validation §20 will enforce).
    const MotionSegment* active = nullptr;
    const MotionSegment* first  = nullptr;
    const MotionSegment* last   = nullptr;
    for (const MotionSegment& s : entity.motionSegments)
    {
        if (!s.enabled) continue;
        if (!first) first = &s;
        last = &s;
        if (scenarioTimeSec >= s.startSecond && scenarioTimeSec <= s.endSecond) {
            active = &s;
            break;
        }
    }

    if (!first || !last)
    {
        // All segments disabled — fall back to static pose.
        out.ecefX = entity.ecefX;
        out.ecefY = entity.ecefY;
        out.ecefZ = entity.ecefZ;
        out.headingDeg = entity.headingDeg;
        out.pitchDeg   = entity.pitchDeg;
        out.rollDeg    = entity.rollDeg;
        return out;
    }

    if (!active)
    {
        // Time falls before first segment or after last — clamp.
        if (scenarioTimeSec < first->startSecond)
            return EvaluateSegment(*first, 0.0, scn);
        return EvaluateSegment(*last, 1.0, scn);
    }

    const double duration = active->endSecond - active->startSecond;
    double u = (duration > 1e-9)
               ? (scenarioTimeSec - active->startSecond) / duration
               : 0.0;

    // Speed-authoritative Line: when speedMps is set, position advances at
    // exactly that rate (in ECEF chord distance, close to great-circle for
    // short segments). The entity arrives at the end point once
    // speed × elapsed >= distance and HOLDS there until the segment's
    // endSecond — at which point the segment-selection loop above hands
    // off to the next segment (or the post-last clamp). This makes the
    // visible motion match the authored speedMps regardless of how big
    // the segment's time window is.
    //
    // speedMps <= 0 falls back to the legacy duration-based lerp so any
    // segments authored without a Speed value behave as before.
    if (active->type == MotionType::Line && active->speedMps > 0.0)
    {
        const ResolvedPoint sp = ResolveStart(*active, scn);
        const ResolvedPoint ep = ResolveEnd(*active, scn);
        const double dx = ep.x - sp.x;
        const double dy = ep.y - sp.y;
        const double dz = ep.z - sp.z;
        const double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dist > 1e-9)
        {
            const double elapsed = scenarioTimeSec - active->startSecond;
            const double traveled = active->speedMps * elapsed;
            u = traveled / dist;
            if (u < 0.0) u = 0.0;
            if (u > 1.0) u = 1.0;
        }
        else
        {
            u = 0.0;
        }
    }

    // StopHold = freeze at the end pose of the immediately preceding
    // segment. With no predecessor, fall back to its own start (matches
    // Stationary semantics).
    if (active->type == MotionType::StopHold)
    {
        const MotionSegment* prev = nullptr;
        for (const MotionSegment& s : entity.motionSegments)
        {
            if (!s.enabled) continue;
            if (&s == active) break;
            prev = &s;
        }
        if (prev)
            return EvaluateSegment(*prev, 1.0, scn);
        return EvaluateSegment(*active, 0.0, scn);
    }

    return EvaluateSegment(*active, u, scn);
}
