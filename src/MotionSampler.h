#pragma once

#include <cstdint>

struct Entity;
struct Scenario;
struct MotionSegment;

// Resolved entity pose at a given scenario time. ECEF position is always
// populated (DIS wire frame, §15.6); orientation is geodetic-local
// Heading/Pitch/Roll in degrees, to be converted to (psi, theta, phi) by
// the caller via CoordTransforms.

struct SampledPose
{
    double ecefX = 0.0;
    double ecefY = 0.0;
    double ecefZ = 0.0;
    double headingDeg = 0.0;
    double pitchDeg   = 0.0;
    double rollDeg    = 0.0;
};

namespace MotionSampler
{
    // Sample the entity's pose at `scenarioTimeSec`. If the entity has no
    // motion segments, returns the entity's static initial pose (the same
    // bytes Slice 1 emitted). Otherwise selects the active segment by
    // [startSecond, endSecond] interval (clamped at the start of the first
    // segment and the end of the last segment) and interpolates per
    // segment type.
    SampledPose SamplePose(const Entity& entity, const Scenario& scenario,
                           double scenarioTimeSec);

    // Lower-level helper exposed for unit tests: evaluate a single segment
    // at a normalized time u in [0, 1]. Passing the scenario lets the
    // sampler resolve Local-XYZ centers/endpoints to ECEF via the scenario
    // origin; passing nullptr falls back to LatLonAlt / ECEF fields only
    // (older test behavior).
    SampledPose EvaluateSegment(const MotionSegment& segment, double u,
                                const Scenario* scenario = nullptr);
}
