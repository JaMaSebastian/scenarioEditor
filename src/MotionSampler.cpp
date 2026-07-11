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

    constexpr double kTauC = 6.28318530717958647692;

    // Cumulative arc length S(theta) at an absolute parametric angle, by
    // interpolating the EllipseFrame's table (built over phi in [0, 2pi]).
    double ArcAtAngle(const MotionSampler::EllipseFrame& f, double theta)
    {
        double t = std::fmod(theta, kTauC);
        if (t < 0.0) t += kTauC;
        const int K = static_cast<int>(f.arcTable.size()) - 1;
        if (K <= 0) return 0.0;
        const double x = t / kTauC * K;
        int i = static_cast<int>(x);
        if (i >= K) i = K - 1;
        if (i < 0)  i = 0;
        const double frac = x - i;
        return f.arcTable[i] + (f.arcTable[i + 1] - f.arcTable[i]) * frac;
    }

    // Inverse of S: the absolute parametric angle whose cumulative arc length
    // is s (binary search + lerp over the table).
    double AngleAtArc(const MotionSampler::EllipseFrame& f, double s)
    {
        const int K = static_cast<int>(f.arcTable.size()) - 1;
        if (K <= 0 || f.perimeter <= 0.0) return 0.0;
        if (s <= 0.0)           return 0.0;
        if (s >= f.perimeter)   return kTauC;
        int lo = 0, hi = K;
        while (hi - lo > 1)
        {
            const int mid = (lo + hi) / 2;
            if (f.arcTable[mid] <= s) lo = mid; else hi = mid;
        }
        const double s0 = f.arcTable[lo], s1 = f.arcTable[hi];
        const double frac = (s1 > s0) ? (s - s0) / (s1 - s0) : 0.0;
        return (lo + frac) * (kTauC / K);
    }
}

// Mutually-recursive with the Entity-Ellipse follow-center resolution below.
static SampledPose EvaluateSegmentImpl(const MotionSegment& s, double u,
                                       const Scenario* scenario, bool geometricMode,
                                       bool allowFollow);
static SampledPose SamplePoseImpl(const Entity& entity, const Scenario& scenario,
                                  double scenarioTimeSec, bool allowFollow);

// Resolve the moving orbit center for an Entity-Ellipse segment: the follow
// target's ECEF position at `timeSec`. Returns false (callers fall back to the
// static foci) when the segment has no follow target, the scenario is null,
// `allowFollow` is off (one-level recursion guard), or the target isn't found.
static bool ResolveFollowCenterEcef(const MotionSegment& s, const Scenario* scenario,
                                    double timeSec, bool allowFollow, double out[3])
{
    if (!allowFollow || !scenario || s.followEntityId < 0) return false;
    for (const Entity& t : scenario->entities)
    {
        if (static_cast<int>(t.entityId) != s.followEntityId) continue;
        // Sample the target with follow disabled, so a target that itself
        // follows something resolves only one level (treated as static here).
        const SampledPose tp = SamplePoseImpl(t, *scenario, timeSec, /*allowFollow*/false);
        out[0] = tp.ecefX; out[1] = tp.ecefY; out[2] = tp.ecefZ;
        return true;
    }
    return false;
}

MotionSampler::EllipseFrame MotionSampler::BuildEllipseFrame(const MotionSegment& s,
                                                             const Scenario* scenario,
                                                             bool withArcTable,
                                                             const double* overrideCenterEcef)
{
    constexpr double kTau = 6.28318530717958647692;
    constexpr double kPi  = 3.14159265358979323846;
    EllipseFrame f;

    double f1Lat = s.f1Lat, f1Lon = s.f1Lon, f1Alt = s.f1Alt;
    double f2Lat = s.f2Lat, f2Lon = s.f2Lon, f2Alt = s.f2Alt;

    // Resolve each focus to geodetic according to coordMode.
    if (s.coordMode == CoordMode::Local && scenario)
    {
        double ex, ey, ez;
        CoordTransforms::LocalEnuToEcefDeg(
            s.f1LocalX, s.f1LocalY, s.f1LocalZ,
            scenario->originLatDeg, scenario->originLonDeg, scenario->originAltM, ex, ey, ez);
        CoordTransforms::EcefToGeodeticDeg(ex, ey, ez, f1Lat, f1Lon, f1Alt);
        CoordTransforms::LocalEnuToEcefDeg(
            s.f2LocalX, s.f2LocalY, s.f2LocalZ,
            scenario->originLatDeg, scenario->originLonDeg, scenario->originAltM, ex, ey, ez);
        CoordTransforms::EcefToGeodeticDeg(ex, ey, ez, f2Lat, f2Lon, f2Alt);
    }
    else if (s.coordMode == CoordMode::ECEF)
    {
        CoordTransforms::EcefToGeodeticDeg(s.f1EcefX, s.f1EcefY, s.f1EcefZ, f1Lat, f1Lon, f1Alt);
        CoordTransforms::EcefToGeodeticDeg(s.f2EcefX, s.f2EcefY, s.f2EcefZ, f2Lat, f2Lon, f2Alt);
    }

    f.midLat = 0.5 * (f1Lat + f2Lat);
    f.midLon = 0.5 * (f1Lon + f2Lon);
    f.midAlt = 0.5 * (f1Alt + f2Alt);

    double f1Ex, f1Ey, f1Ez, f2Ex, f2Ey, f2Ez;
    CoordTransforms::GeodeticToEcefDeg(f1Lat, f1Lon, f1Alt, f1Ex, f1Ey, f1Ez);
    CoordTransforms::GeodeticToEcefDeg(f2Lat, f2Lon, f2Alt, f2Ex, f2Ey, f2Ez);
    double e1, n1, u1, e2, n2, u2;
    CoordTransforms::EcefToLocalEnuDeg(f1Ex, f1Ey, f1Ez, f.midLat, f.midLon, f.midAlt, e1, n1, u1);
    CoordTransforms::EcefToLocalEnuDeg(f2Ex, f2Ey, f2Ez, f.midLat, f.midLon, f.midAlt, e2, n2, u2);

    const double dx = e2 - e1, dy = n2 - n1;
    const double focusDist = std::sqrt(dx * dx + dy * dy);
    f.c = 0.5 * focusDist;
    f.a = 0.5 * std::max(s.lengthMeters, 2.0 * f.c);   // 2a >= 2c
    f.b = std::sqrt(std::max(0.0, f.a * f.a - f.c * f.c));

    f.majorE = 1.0; f.majorN = 0.0;
    if (focusDist > 1e-9) { f.majorE = dx / focusDist; f.majorN = dy / focusDist; }

    f.centerE = 0.5 * (e1 + e2);
    f.centerN = 0.5 * (n1 + n2);
    f.meanU   = 0.5 * (u1 + u2);

    // Entity Ellipse: translate the orbit center onto the follow target's
    // position (expressed in this frame's ENU), keeping the configured altitude.
    // Shape, orientation, and semi-axes are untouched.
    if (overrideCenterEcef)
    {
        double te, tn, tu;
        CoordTransforms::EcefToLocalEnuDeg(
            overrideCenterEcef[0], overrideCenterEcef[1], overrideCenterEcef[2],
            f.midLat, f.midLon, f.midAlt, te, tn, tu);
        f.centerE = te;
        f.centerN = tn;
    }

    // Start parametric angle from the compass bearing (see EvaluateSegment).
    const double bearingMath = (90.0 - s.startBearingDeg) * (kTau / 360.0);
    const double bxE = std::cos(bearingMath);
    const double bxN = std::sin(bearingMath);
    const double bLocalX =  bxE * f.majorE + bxN * f.majorN;
    const double bLocalY = -bxE * f.majorN + bxN * f.majorE;
    f.theta0 = std::atan2(bLocalY * f.a, bLocalX * f.b);
    f.sign   = (s.direction == EllipseDirection::Clockwise) ? -1.0 : 1.0;

    // Cheap Ramanujan perimeter (sufficient for the time-coupled period).
    f.perimeter = kPi * (3.0 * (f.a + f.b) -
                         std::sqrt((3.0 * f.a + f.b) * (f.a + 3.0 * f.b)));
    f.valid = (f.perimeter > 0.0);

    if (withArcTable)
    {
        // Numerically integrate ds = sqrt(a^2 sin^2 phi + b^2 cos^2 phi) dphi
        // over phi in [0, 2pi] (trapezoid). The exact perimeter replaces the
        // Ramanujan estimate; the cumulative table drives equidistant sampling.
        constexpr int K = 4096;
        f.arcTable.assign(K + 1, 0.0);
        const double dphi = kTau / K;
        double prev = std::sqrt(f.a * f.a * 0.0 + f.b * f.b * 1.0);  // phi=0 -> b
        double acc  = 0.0;
        for (int k = 1; k <= K; ++k)
        {
            const double phi = k * dphi;
            const double sn = std::sin(phi), cs = std::cos(phi);
            const double cur = std::sqrt(f.a * f.a * sn * sn + f.b * f.b * cs * cs);
            acc += 0.5 * (prev + cur) * dphi;
            f.arcTable[k] = acc;
            prev = cur;
        }
        f.perimeter = acc;
        f.valid = (f.perimeter > 0.0);
    }

    return f;
}

MotionSampler::EllipsePoint MotionSampler::EvalEllipseAtTheta(const EllipseFrame& f, double theta)
{
    constexpr double kTau = 6.28318530717958647692;
    EllipsePoint p;

    const double lx = f.a * std::cos(theta);
    const double ly = f.b * std::sin(theta);
    const double east  = f.centerE + lx * f.majorE - ly * f.majorN;
    const double north = f.centerN + lx * f.majorN + ly * f.majorE;
    CoordTransforms::LocalEnuToEcefDeg(east, north, f.meanU, f.midLat, f.midLon, f.midAlt,
                                       p.ecefX, p.ecefY, p.ecefZ);

    // Tangent: derivative of (east, north) w.r.t. theta, in the sweep direction.
    const double dlx = -f.a * std::sin(theta);
    const double dly =  f.b * std::cos(theta);
    const double dEast  = (dlx * f.majorE - dly * f.majorN) * f.sign;
    const double dNorth = (dlx * f.majorN + dly * f.majorE) * f.sign;
    const double mag = std::sqrt(dEast * dEast + dNorth * dNorth);
    if (mag > 1e-12) { p.tEast = dEast / mag; p.tNorth = dNorth / mag; }
    p.headingDeg = std::atan2(dEast, dNorth) * (360.0 / kTau);
    return p;
}

double MotionSampler::InvertArcLength(const EllipseFrame& f, double sFromStart)
{
    if (!f.valid || f.perimeter <= 0.0 || f.arcTable.empty()) return f.theta0;
    const double s0 = ArcAtAngle(f, f.theta0);
    double target = s0 + f.sign * sFromStart;
    target = std::fmod(target, f.perimeter);
    if (target < 0.0) target += f.perimeter;
    return AngleAtArc(f, target);
}

SampledPose MotionSampler::EvaluateSegment(const MotionSegment& s, double u,
                                           const Scenario* scenario,
                                           bool geometricMode)
{
    return EvaluateSegmentImpl(s, u, scenario, geometricMode, /*allowFollow*/true);
}

static SampledPose EvaluateSegmentImpl(const MotionSegment& s, double u,
                                       const Scenario* scenario, bool geometricMode,
                                       bool allowFollow)
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
            // Two-foci + length parametrization. The geometry now lives in the
            // shared BuildEllipseFrame / EvalEllipseAtTheta helpers (also used
            // by the playback equidistant-waypoint precompute). Sampling here
            // stays constant-ANGULAR in time (preserves prior behavior; the
            // preview path trace uses geometricMode for one even revolution).
            constexpr double kTau = 6.28318530717958647692;

            const double segDuration = s.endSecond - s.startSecond;

            // Entity Ellipse: re-center the orbit on the follow target at the
            // real time this u maps to (start + u * window). Non-follow orbits
            // pass a null override and behave exactly as before.
            const double absTime = s.startSecond + u * segDuration;
            double ov[3];
            const double* povr = ResolveFollowCenterEcef(s, scenario, absTime, allowFollow, ov)
                                 ? ov : nullptr;

            const MotionSampler::EllipseFrame f =
                MotionSampler::BuildEllipseFrame(s, scenario, false, povr);   // Ramanujan perimeter
            const double speed  = (s.speedMps > 0.0) ? s.speedMps : 100.0;
            const double period = (f.perimeter > 0.0) ? (f.perimeter / speed) : 1.0;

            const double theta = geometricMode
                ? (f.theta0 + f.sign * kTau * u)
                : (f.theta0 + f.sign * kTau * (u * segDuration / period));

            const MotionSampler::EllipsePoint p = MotionSampler::EvalEllipseAtTheta(f, theta);
            out.ecefX = p.ecefX; out.ecefY = p.ecefY; out.ecefZ = p.ecefZ;
            out.headingDeg = p.headingDeg;
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
    return SamplePoseImpl(entity, scenario, scenarioTimeSec, /*allowFollow*/true);
}

static SampledPose SamplePoseImpl(const Entity& entity, const Scenario& scenario,
                                  double scenarioTimeSec, bool allowFollow)
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
        // Time falls before the first segment, in a GAP between two segments, or
        // after the last one. Before the first -> hold at its start. Otherwise
        // hold at the END pose of the most recently COMPLETED segment. Using the
        // latest finished segment (rather than always the last one) keeps an
        // entity frozen where the prior leg left it during an inter-segment gap,
        // instead of teleporting it across to the final segment's end pose.
        if (scenarioTimeSec < first->startSecond)
            return EvaluateSegmentImpl(*first, 0.0, scn, false, allowFollow);

        const MotionSegment* prevDone = first;
        for (const MotionSegment& s : entity.motionSegments)
        {
            if (!s.enabled) continue;
            if (s.endSecond <= scenarioTimeSec) prevDone = &s; else break;
        }

        // A TERMINAL ellipse circles endlessly: once the entity reaches the
        // final orbit it keeps going around for the rest of the timeline rather
        // than freezing at the segment's end-of-window pose. We advance the
        // parametric angle by the real elapsed time since the orbit began, so
        // the motion is seamless with the active-window sampling below.
        if (prevDone == last && last->type == MotionType::Ellipse)
        {
            constexpr double kTau = 6.28318530717958647692;
            // Entity Ellipse: keep circling the target's CURRENT position after
            // the segment window ends (center resolved at scenarioTimeSec).
            double ov[3];
            const double* povr = ResolveFollowCenterEcef(*last, scn, scenarioTimeSec,
                                                          allowFollow, ov) ? ov : nullptr;
            const MotionSampler::EllipseFrame f =
                MotionSampler::BuildEllipseFrame(*last, scn, false, povr);
            const double speed  = (last->speedMps > 0.0) ? last->speedMps : 100.0;
            const double period = (f.perimeter > 0.0) ? (f.perimeter / speed) : 1.0;
            const double elapsed = scenarioTimeSec - last->startSecond;
            const MotionSampler::EllipsePoint p =
                MotionSampler::EvalEllipseAtTheta(f, f.theta0 + f.sign * kTau * (elapsed / period));
            SampledPose ep{};
            ep.ecefX = p.ecefX; ep.ecefY = p.ecefY; ep.ecefZ = p.ecefZ;
            ep.headingDeg = p.headingDeg;
            ep.pitchDeg   = last->startPitchDeg;
            ep.rollDeg    = last->startRollDeg;
            return ep;
        }

        return EvaluateSegmentImpl(*prevDone, 1.0, scn, false, allowFollow);
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
            return EvaluateSegmentImpl(*prev, 1.0, scn, false, allowFollow);
        return EvaluateSegmentImpl(*active, 0.0, scn, false, allowFollow);
    }

    return EvaluateSegmentImpl(*active, u, scn, false, allowFollow);
}
