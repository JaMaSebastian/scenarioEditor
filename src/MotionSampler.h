#pragma once

#include <cstdint>
#include <vector>

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
    //
    // geometricMode = true makes u parametrize the *geometric* path
    // (u ∈ [0,1] = one full cycle: start→end for Line, one revolution
    // for Ellipse), rather than the time-coupled trajectory. Used by
    // the preview path renderer so the trace covers the shape exactly
    // once regardless of how many real orbits fit in the segment window.
    SampledPose EvaluateSegment(const MotionSegment& segment, double u,
                                const Scenario* scenario = nullptr,
                                bool geometricMode = false);

    // ----- Ellipse geometry (shared by the time sampler and the playback
    //       equidistant-waypoint precompute) -----

    // Fully resolved ellipse frame for one Ellipse MotionSegment: the ENU
    // plane (centered at the foci midpoint), semi-axes, start angle, sweep
    // direction, and a cumulative arc-length table for equidistant sampling.
    struct EllipseFrame
    {
        double a = 0.0, b = 0.0, c = 0.0;          // semi-axes + focal half-distance (m)
        double centerE = 0.0, centerN = 0.0;       // ellipse center in ENU (foci midpoint)
        double meanU   = 0.0;                       // mean focus up (m)
        double majorE  = 1.0, majorN = 0.0;        // unit major-axis direction in ENU
        double midLat = 0.0, midLon = 0.0, midAlt = 0.0; // ENU origin frame (mean of foci)
        double theta0 = 0.0;                         // start parametric angle (from bearing)
        double sign   = 1.0;                         // +1 CCW, -1 CW
        double perimeter = 0.0;                      // numerically integrated arc length (m)
        std::vector<double> arcTable;                // cumulative s over phi in [0, 2pi]
        bool   valid  = false;                       // false if not an ellipse / degenerate
    };

    // A single evaluated point on the ellipse: ECEF position, the unit
    // tangent in the ENU plane (already in the sweep direction), and the
    // compass heading derived from it.
    struct EllipsePoint
    {
        double ecefX = 0.0, ecefY = 0.0, ecefZ = 0.0;
        double tEast = 0.0, tNorth = 0.0;            // unit tangent in ENU (sweep dir)
        double headingDeg = 0.0;
    };

    // Resolve a single Ellipse MotionSegment to its ENU frame. By default the
    // perimeter is the cheap Ramanujan approximation (enough for the time
    // sampler). Pass withArcTable=true to also build the numerically-integrated
    // cumulative arc-length table (perimeter becomes the exact integral) — used
    // by the playback equidistant-waypoint precompute.
    //
    // overrideCenterEcef (Entity Ellipse): when non-null, the orbit is re-centered
    // on this ECEF point (the follow-target's position at sample time) instead of
    // the stored foci midpoint. The foci still define the shape/orientation; only
    // the center translates, keeping the configured altitude.
    EllipseFrame BuildEllipseFrame(const MotionSegment& segment, const Scenario* scenario,
                                   bool withArcTable = false,
                                   const double* overrideCenterEcef = nullptr);

    // Evaluate the ellipse at parametric angle theta (radians, math convention
    // in the ellipse-local frame).
    EllipsePoint EvalEllipseAtTheta(const EllipseFrame& f, double theta);

    // Inverse arc length: given a distance measured FROM theta0 along the
    // sweep direction, return the parametric angle theta.
    double InvertArcLength(const EllipseFrame& f, double sFromStart);
}
