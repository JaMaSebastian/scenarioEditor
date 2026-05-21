#include "MotionSampler.h"
#include "CoordTransforms.h"
#include "Scenario.h"

#include <cmath>

namespace
{
    double Lerp(double a, double b, double u) { return a + (b - a) * u; }

    // Resolve a segment's start position to ECEF + geodetic lat/lon/alt,
    // honoring its coordinate mode. We need both forms so we can lerp in
    // the segment's native frame and still produce ECEF for the wire.
    struct ResolvedPoint { double lat, lon, alt, x, y, z; };

    ResolvedPoint ResolveStart(const MotionSegment& s)
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
                // Local-ENU segments are deferred to a future slice — fall
                // back to ECEF fields if any were authored, else zeros.
                p.x = s.startEcefX; p.y = s.startEcefY; p.z = s.startEcefZ;
                CoordTransforms::EcefToGeodeticDeg(p.x, p.y, p.z, p.lat, p.lon, p.alt);
                break;
        }
        return p;
    }

    ResolvedPoint ResolveEnd(const MotionSegment& s)
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
                p.x = s.endEcefX; p.y = s.endEcefY; p.z = s.endEcefZ;
                CoordTransforms::EcefToGeodeticDeg(p.x, p.y, p.z, p.lat, p.lon, p.alt);
                break;
        }
        return p;
    }
}

SampledPose MotionSampler::EvaluateSegment(const MotionSegment& s, double u)
{
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;

    SampledPose out{};
    switch (s.type) {
        case MotionType::StopHold:    // fallthrough — caller treats as Stationary
                                      // when invoked in isolation; SamplePose
                                      // injects the prior segment's end pose.
        case MotionType::Stationary: {
            const ResolvedPoint start = ResolveStart(s);
            out.ecefX = start.x;
            out.ecefY = start.y;
            out.ecefZ = start.z;
            out.headingDeg = s.startHeadingDeg;
            out.pitchDeg   = s.startPitchDeg;
            out.rollDeg    = s.startRollDeg;
            break;
        }
        case MotionType::Line: {
            const ResolvedPoint start = ResolveStart(s);
            const ResolvedPoint end   = ResolveEnd(s);
            const double lat = Lerp(start.lat, end.lat, u);
            const double lon = Lerp(start.lon, end.lon, u);
            const double alt = Lerp(start.alt, end.alt, u);
            CoordTransforms::GeodeticToEcefDeg(lat, lon, alt,
                                               out.ecefX, out.ecefY, out.ecefZ);
            out.headingDeg = Lerp(s.startHeadingDeg, s.endHeadingDeg, u);
            out.pitchDeg   = Lerp(s.startPitchDeg,   s.endPitchDeg,   u);
            out.rollDeg    = Lerp(s.startRollDeg,    s.endRollDeg,    u);
            break;
        }
        case MotionType::Ellipse: {
            // Parametric ENU position relative to (centerLat, centerLon,
            // centerAlt). Direction Clockwise (from above) means decreasing
            // mathematical angle, hence the negative sign.
            constexpr double kTau = 6.28318530717958647692;
            const double sign = (s.direction == EllipseDirection::Clockwise) ? -1.0 : 1.0;
            const double phaseDeg = s.startAngleDeg + sign * 360.0 * u;
            const double phaseRad = phaseDeg * (kTau / 360.0);

            // Pre-rotation ellipse offset in the ENU plane.
            const double xPre = s.radiusXMeters * std::cos(phaseRad);
            const double yPre = s.radiusYMeters * std::sin(phaseRad);

            const double rotRad = s.rotationDeg * (kTau / 360.0);
            const double cr = std::cos(rotRad), sr = std::sin(rotRad);
            const double east  = cr * xPre - sr * yPre;
            const double north = sr * xPre + cr * yPre;

            double altOffset = 0.0;
            if (s.altitudeMode == AltitudeMode::Linear) {
                // Linearly drift from centerAlt to (centerAlt + 0) — kept
                // for parity; future enhancement could carry an endAlt.
                altOffset = 0.0;
            }

            CoordTransforms::LocalEnuToEcefDeg(east, north, altOffset,
                                               s.centerLat, s.centerLon, s.centerAlt,
                                               out.ecefX, out.ecefY, out.ecefZ);

            // Tangential heading: derivative of (east, north) w.r.t. phase.
            // d/dφ (rx cos, ry sin) = (-rx sin, ry cos), then apply rotation.
            const double dxPre = -s.radiusXMeters * std::sin(phaseRad);
            const double dyPre =  s.radiusYMeters * std::cos(phaseRad);
            const double dEast  = (cr * dxPre - sr * dyPre) * sign;
            const double dNorth = (sr * dxPre + cr * dyPre) * sign;
            const double headingRad = std::atan2(dEast, dNorth);
            out.headingDeg = headingRad * (360.0 / kTau);
            out.pitchDeg   = s.startPitchDeg;
            out.rollDeg    = s.startRollDeg;
            break;
        }
    }
    return out;
}

SampledPose MotionSampler::SamplePose(const Entity& entity, const Scenario& /*scenario*/,
                                      double scenarioTimeSec)
{
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
            return EvaluateSegment(*first, 0.0);
        return EvaluateSegment(*last, 1.0);
    }

    const double duration = active->endSecond - active->startSecond;
    const double u = (duration > 1e-9)
                     ? (scenarioTimeSec - active->startSecond) / duration
                     : 0.0;

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
            return EvaluateSegment(*prev, 1.0);
        return EvaluateSegment(*active, 0.0);
    }

    return EvaluateSegment(*active, u);
}
