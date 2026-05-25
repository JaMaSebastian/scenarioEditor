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

    if (failures == 0) {
        std::printf("\nMotion sampler PASS\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED\n", failures);
    return 1;
}
