// Standalone test for MotionSampler. Built via CMake as the
// `motion_sampler_test` target. Exits 0 on pass.

#include "CoordTransforms.h"
#include "MotionSampler.h"
#include "Scenario.h"

#include <cmath>
#include <cstdio>

namespace
{
    int failures = 0;

    void ExpectNear(double a, double b, double tol, const char* what)
    {
        if (std::fabs(a - b) <= tol) return;
        std::printf("  FAIL %s: %.9f != %.9f (delta %.3e, tol %.3e)\n",
                    what, a, b, std::fabs(a - b), tol);
        ++failures;
    }
}

int main()
{
    // ---- Case 1: entity with no motion segments returns static pose ----
    Scenario scn;
    scn.entities.emplace_back();   // fresh scenarios start with no entities
    Entity& e = scn.entity();
    e.ecefX = -2706174.85;
    e.ecefY = -4261059.49;
    e.ecefZ =  3885725.49;
    e.headingDeg = 12.5;
    e.pitchDeg   =  3.0;
    e.rollDeg    = -1.5;

    {
        const auto p = MotionSampler::SamplePose(e, scn, 0.0);
        ExpectNear(p.ecefX, e.ecefX, 1e-6, "static.ecefX");
        ExpectNear(p.ecefY, e.ecefY, 1e-6, "static.ecefY");
        ExpectNear(p.ecefZ, e.ecefZ, 1e-6, "static.ecefZ");
        ExpectNear(p.headingDeg, e.headingDeg, 1e-9, "static.heading");

        const auto p2 = MotionSampler::SamplePose(e, scn, 100.0);
        ExpectNear(p2.ecefX, p.ecefX, 1e-9, "static.ecefX@t=100");
    }

    // ---- Case 2: Stationary segment at known geodetic position ----
    {
        MotionSegment s;
        s.type           = MotionType::Stationary;
        s.startSecond    = 0.0;  s.endSecond = 60.0;
        s.coordMode      = CoordMode::LatLonAlt;
        s.startLat = 51.5074; s.startLon = 0.1278; s.startAlt = 25.0;
        s.startHeadingDeg = 90.0;
        e.motionSegments = { s };

        double expX = 0, expY = 0, expZ = 0;
        CoordTransforms::GeodeticToEcefDeg(51.5074, 0.1278, 25.0, expX, expY, expZ);

        for (double t : {0.0, 30.0, 60.0}) {
            const auto p = MotionSampler::SamplePose(e, scn, t);
            ExpectNear(p.ecefX, expX, 1e-3, "stat.ecefX");
            ExpectNear(p.ecefY, expY, 1e-3, "stat.ecefY");
            ExpectNear(p.ecefZ, expZ, 1e-3, "stat.ecefZ");
            ExpectNear(p.headingDeg, 90.0, 1e-9, "stat.heading");
        }
    }

    // ---- Case 3: Line segment in Lat/Lon/Alt over 60 seconds ----
    {
        MotionSegment s;
        s.type           = MotionType::Line;
        s.startSecond    = 0.0;  s.endSecond = 60.0;
        s.coordMode      = CoordMode::LatLonAlt;
        s.startLat = 37.7749; s.startLon = -122.4194; s.startAlt = 0.0;
        s.endLat   = 37.7849; s.endLon   = -122.4194; s.endAlt   = 100.0;
        s.startHeadingDeg = 0.0;
        s.endHeadingDeg   = 0.0;
        e.motionSegments = { s };

        // t = 0  → start
        {
            const auto p = MotionSampler::SamplePose(e, scn, 0.0);
            double expX, expY, expZ;
            CoordTransforms::GeodeticToEcefDeg(s.startLat, s.startLon, s.startAlt,
                                               expX, expY, expZ);
            ExpectNear(p.ecefX, expX, 1e-3, "line.t=0.ecefX");
            ExpectNear(p.ecefY, expY, 1e-3, "line.t=0.ecefY");
            ExpectNear(p.ecefZ, expZ, 1e-3, "line.t=0.ecefZ");
        }
        // t = 30 → midpoint
        {
            const auto p = MotionSampler::SamplePose(e, scn, 30.0);
            const double midLat = 0.5 * (s.startLat + s.endLat);
            const double midLon = 0.5 * (s.startLon + s.endLon);
            const double midAlt = 0.5 * (s.startAlt + s.endAlt);
            double expX, expY, expZ;
            CoordTransforms::GeodeticToEcefDeg(midLat, midLon, midAlt,
                                               expX, expY, expZ);
            ExpectNear(p.ecefX, expX, 1e-3, "line.t=30.ecefX");
            ExpectNear(p.ecefY, expY, 1e-3, "line.t=30.ecefY");
            ExpectNear(p.ecefZ, expZ, 1e-3, "line.t=30.ecefZ");
        }
        // t = 60 → end
        {
            const auto p = MotionSampler::SamplePose(e, scn, 60.0);
            double expX, expY, expZ;
            CoordTransforms::GeodeticToEcefDeg(s.endLat, s.endLon, s.endAlt,
                                               expX, expY, expZ);
            ExpectNear(p.ecefX, expX, 1e-3, "line.t=60.ecefX");
            ExpectNear(p.ecefY, expY, 1e-3, "line.t=60.ecefY");
            ExpectNear(p.ecefZ, expZ, 1e-3, "line.t=60.ecefZ");
        }
        // t = 90 → past end, should clamp to end pose
        {
            const auto p = MotionSampler::SamplePose(e, scn, 90.0);
            double expX, expY, expZ;
            CoordTransforms::GeodeticToEcefDeg(s.endLat, s.endLon, s.endAlt,
                                               expX, expY, expZ);
            ExpectNear(p.ecefX, expX, 1e-3, "line.t=90.clamped.ecefX");
        }
    }

    // ---- Case 4: StopHold after a Line. Holds at line's end pose. ----
    {
        MotionSegment line;
        line.type           = MotionType::Line;
        line.startSecond    = 0.0;  line.endSecond = 30.0;
        line.coordMode      = CoordMode::LatLonAlt;
        line.startLat = 0.0; line.startLon = 0.0; line.startAlt = 0.0;
        line.endLat   = 0.1; line.endLon   = 0.1; line.endAlt   = 50.0;

        MotionSegment stop;
        stop.type           = MotionType::StopHold;
        stop.startSecond    = 30.0; stop.endSecond = 90.0;

        e.motionSegments = { line, stop };

        // t=60 is inside StopHold; should match line's end pose (t=30 in line).
        const auto p = MotionSampler::SamplePose(e, scn, 60.0);
        double expX, expY, expZ;
        CoordTransforms::GeodeticToEcefDeg(0.1, 0.1, 50.0, expX, expY, expZ);
        ExpectNear(p.ecefX, expX, 1e-3, "stophold.x@t=60");
        ExpectNear(p.ecefY, expY, 1e-3, "stophold.y@t=60");
        ExpectNear(p.ecefZ, expZ, 1e-3, "stophold.z@t=60");
    }

    // ---- Case 5: Circular ellipse (F1 = F2) around (0,0,1000). Sample at
    //              start / quarter / half. Speed = 2π·R/120s for a R=1000m
    //              orbit in 120 seconds.
    {
        constexpr double kRadius = 1000.0;
        constexpr double kPeriod = 120.0;
        constexpr double kPi     = 3.14159265358979323846;
        MotionSegment s;
        s.type            = MotionType::Ellipse;
        s.startSecond     = 0.0; s.endSecond = kPeriod;
        s.f1Lat = 0.0; s.f1Lon = 0.0; s.f1Alt = 1000.0;
        s.f2Lat = 0.0; s.f2Lon = 0.0; s.f2Alt = 1000.0;
        s.lengthMeters    = 2.0 * kRadius;                  // 2a = 2R for circle
        s.startBearingDeg = 90.0;                            // 0=N, 90=E → start at +East
        s.speedMps        = (2.0 * kPi * kRadius) / kPeriod; // ~52.36 m/s
        s.direction       = EllipseDirection::CounterClockwise;
        e.motionSegments  = { s };

        // u=0 → start at +East from center (bearing 90°). ENU (R, 0, 0).
        const auto p0 = MotionSampler::SamplePose(e, scn, 0.0);
        double xE, yE, zE;
        CoordTransforms::LocalEnuToEcefDeg(kRadius, 0.0, 0.0,
                                           s.f1Lat, s.f1Lon, s.f1Alt,
                                           xE, yE, zE);
        ExpectNear(p0.ecefX, xE, 1.0, "ellipse.t=0.x");
        ExpectNear(p0.ecefY, yE, 1.0, "ellipse.t=0.y");
        ExpectNear(p0.ecefZ, zE, 1.0, "ellipse.t=0.z");

        // u=0.25 → quarter orbit CCW from +East → +North (R, 0) → (0, R).
        const auto p1 = MotionSampler::SamplePose(e, scn, 30.0);
        double xQ, yQ, zQ;
        CoordTransforms::LocalEnuToEcefDeg(0.0, kRadius, 0.0,
                                           s.f1Lat, s.f1Lon, s.f1Alt,
                                           xQ, yQ, zQ);
        ExpectNear(p1.ecefX, xQ, 1.0, "ellipse.t=30.x");
        ExpectNear(p1.ecefY, yQ, 1.0, "ellipse.t=30.y");
        ExpectNear(p1.ecefZ, zQ, 1.0, "ellipse.t=30.z");

        // u=0.5 → half orbit → -East side.
        const auto p2 = MotionSampler::SamplePose(e, scn, 60.0);
        double xH, yH, zH;
        CoordTransforms::LocalEnuToEcefDeg(-kRadius, 0.0, 0.0,
                                           s.f1Lat, s.f1Lon, s.f1Alt,
                                           xH, yH, zH);
        ExpectNear(p2.ecefX, xH, 1.0, "ellipse.t=60.x");
        ExpectNear(p2.ecefY, yH, 1.0, "ellipse.t=60.y");
        ExpectNear(p2.ecefZ, zH, 1.0, "ellipse.t=60.z");
    }

    // ---- Case 6: Terminal explosion into a MOVING target. The target flies
    //              due East at 50 m/s; the source starts 5 km North of it and
    //              flies at 200 m/s. The impact leg must meet the target where
    //              it will be at the solved intercept time, at the authored
    //              offset in the target's heading frame. ----
    {
        Scenario s2;
        s2.originLatDeg = 22.6; s2.originLonDeg = 120.25; s2.originAltM = 0.0;
        s2.entities.resize(2);
        Entity& tgt = s2.entities[0];
        Entity& src = s2.entities[1];
        tgt.entityId = 7;
        src.entityId = 8;

        MotionSegment tl;
        tl.type = MotionType::Line;
        tl.coordMode = CoordMode::Local;
        tl.startSecond = 0.0; tl.endSecond = 200.0;
        tl.startLocalX = 0.0;     tl.startLocalY = 0.0; tl.startLocalZ = 100.0;
        tl.endLocalX   = 10000.0; tl.endLocalY   = 0.0; tl.endLocalZ   = 100.0;
        tgt.motionSegments = { tl };

        MotionSegment il;
        il.type = MotionType::Line;
        il.coordMode = CoordMode::Local;
        il.startSecond = 0.0; il.endSecond = 1.0;
        il.speedMps = 200.0;
        il.startLocalX = 0.0; il.startLocalY = 5000.0; il.startLocalZ = 100.0;
        il.impactEntityId = 7;
        il.impactForwardM = 10.0;   // 10 m ahead of the target's reference point
        src.motionSegments = { il };

        const SampledPose s0 = MotionSampler::EvaluateSegment(il, 0.0, &s2, true);
        const double start[3] = { s0.ecefX, s0.ecefY, s0.ecefZ };
        const double T = MotionSampler::SolveImpactTime(il, s2, start, 0.0, 200.0, 200.0);
        src.motionSegments[0].endSecond = T;

        // Closed form (flat): |(50T + 10, -5000)| = 200T  ->  T ~= 25.83 s.
        ExpectNear(T, 25.83, 0.05, "impact.solvedTime");

        const SampledPose tp = MotionSampler::SamplePose(tgt, s2, T);
        const SampledPose sp = MotionSampler::SamplePose(src, s2, T);
        double te, tn, tu, se, sn, su;
        CoordTransforms::EcefToLocalEnuDeg(tp.ecefX, tp.ecefY, tp.ecefZ,
            s2.originLatDeg, s2.originLonDeg, s2.originAltM, te, tn, tu);
        CoordTransforms::EcefToLocalEnuDeg(sp.ecefX, sp.ecefY, sp.ecefZ,
            s2.originLatDeg, s2.originLonDeg, s2.originAltM, se, sn, su);
        ExpectNear(se, te + 10.0, 0.05, "impact.onTarget.east (+10 m forward)");
        ExpectNear(sn, tn,        0.05, "impact.onTarget.north");
        ExpectNear(su, tu,        0.05, "impact.onTarget.up");

        double ip[3];
        const bool ok = MotionSampler::ImpactPointEcef(src.motionSegments[0], s2, ip);
        ExpectNear(ok ? 1.0 : 0.0, 1.0, 0.0, "impact.ImpactPointEcef ok");
        ExpectNear(sp.ecefX, ip[0], 1e-3, "impact.ImpactPointEcef.x");

        // Re-route the target: the leg still ends on it at T, no re-authoring.
        tgt.motionSegments[0].endLocalY = 4000.0;
        const SampledPose tp2 = MotionSampler::SamplePose(tgt, s2, T);
        const SampledPose sp2 = MotionSampler::SamplePose(src, s2, T);
        const double miss = std::sqrt((tp2.ecefX - sp2.ecefX) * (tp2.ecefX - sp2.ecefX) +
                                      (tp2.ecefY - sp2.ecefY) * (tp2.ecefY - sp2.ecefY) +
                                      (tp2.ecefZ - sp2.ecefZ) * (tp2.ecefZ - sp2.ecefZ));
        ExpectNear(miss, 10.0, 0.05, "impact.rerouted target still hit (10 m offset)");

        // Terminal-impact lookup + a missing target disables the explosion.
        ExpectNear(MotionSampler::TerminalImpactSegment(src) ? 1.0 : 0.0, 1.0, 0.0,
                   "impact.TerminalImpactSegment");
        tgt.enabled = false;
        ExpectNear(MotionSampler::ImpactTargetIndex(src.motionSegments[0], s2), -1.0, 0.0,
                   "impact.disabled target -> no explosion");
    }

    // ---- Case 7: plays/hanger.ini shape -- a LUCAS at 70 m crashing into a
    //              Type 052C on the sea (origin altitude 20 m = sea level),
    //              authored with Up = 0. The attacker must stay out of the
    //              water all the way in and hit the hull above the waterline,
    //              holding altitude until a short terminal dive. ----
    {
        Scenario s3;
        s3.originLatDeg = 22.627169; s3.originLonDeg = 120.262939; s3.originAltM = 20.0;
        s3.entities.resize(2);
        Entity& ship  = s3.entities[0];
        Entity& lucas = s3.entities[1];
        ship.entityId = 21;  ship.domain = 3;    // Surface: pinned to sea level
        lucas.entityId = 8;  lucas.domain = 2;   // Air

        MotionSegment sl;
        sl.type = MotionType::Line;
        sl.coordMode = CoordMode::Local;
        sl.startSecond = 0.0; sl.endSecond = 1000.0;
        sl.startLocalX = -10600.0; sl.startLocalY = 1300.0;
        sl.endLocalX   = -10600.0; sl.endLocalY   = 9000.0;   // steaming north
        ship.motionSegments = { sl };

        MotionSegment il;
        il.type = MotionType::Line;
        il.coordMode = CoordMode::Local;
        il.startSecond = 0.0; il.endSecond = 197.5;
        il.speedMps = 51.0;
        il.startLocalX = -512.0; il.startLocalY = 1234.0; il.startLocalZ = 49.86;  // ~70 m
        il.impactEntityId = 21;                                   // Up = 0 as authored
        lucas.motionSegments = { il };

        const double seaLevel = s3.originAltM;
        double minAbove = 1e18;
        for (int k = 0; k <= 2000; ++k)
        {
            const double t = il.endSecond * k / 2000.0;
            const SampledPose p = MotionSampler::SamplePose(lucas, s3, t);
            double lat, lon, alt;
            CoordTransforms::EcefToGeodeticDeg(p.ecefX, p.ecefY, p.ecefZ, lat, lon, alt);
            minAbove = std::min(minAbove, alt - seaLevel);
        }
        if (minAbove < 1.99) {
            std::printf("  FAIL ship impact: attacker dipped to %.2f m above the sea\n", minAbove);
            ++failures;
        }

        // Impact point on the hull: 2 m above the waterline, directly over the ship.
        const SampledPose hit = MotionSampler::SamplePose(lucas, s3, il.endSecond);
        const SampledPose tgt = MotionSampler::SamplePose(ship,  s3, il.endSecond);
        double hLat, hLon, hAlt, tLat, tLon, tAlt;
        CoordTransforms::EcefToGeodeticDeg(hit.ecefX, hit.ecefY, hit.ecefZ, hLat, hLon, hAlt);
        CoordTransforms::EcefToGeodeticDeg(tgt.ecefX, tgt.ecefY, tgt.ecefZ, tLat, tLon, tAlt);
        ExpectNear(tAlt, seaLevel, 1e-3, "ship.onSea");
        ExpectNear(hAlt - seaLevel, 2.0, 0.05, "ship.impact 2 m above waterline");
        ExpectNear(hLat, tLat, 1e-6, "ship.impact lat on target");
        ExpectNear(hLon, tLon, 1e-6, "ship.impact lon on target");

        // Level until the terminal dive: halfway in it is still at ~70 m, and
        // just before impact it is pitched down in the dive.
        const SampledPose mid = MotionSampler::SamplePose(lucas, s3, 0.5 * il.endSecond);
        double mLat, mLon, mAlt;
        CoordTransforms::EcefToGeodeticDeg(mid.ecefX, mid.ecefY, mid.ecefZ, mLat, mLon, mAlt);
        ExpectNear(mAlt, 70.0, 0.5, "ship.cruise altitude held before the dive");
        const SampledPose late = MotionSampler::SamplePose(lucas, s3, il.endSecond - 0.5);
        ExpectNear(late.pitchDeg, -20.0, 1e-6, "ship.terminal dive pitch");
    }

    if (failures == 0) {
        std::printf("\nMotion sampler PASS\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED\n", failures);
    return 1;
}
