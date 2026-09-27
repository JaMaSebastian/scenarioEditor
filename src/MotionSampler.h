//=============================================================================
//  MotionSampler.h
//-----------------------------------------------------------------------------
//  Declares the MotionSampler API: resolves an entity's pose (ECEF position +
//  Heading/Pitch/Roll) at any scenario time by selecting and interpolating its
//  motion segments (Stationary, Line, Ellipse), plus shared ellipse geometry
//  helpers used by both the time sampler and the playback waypoint precompute.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include <cstdint>
#include <vector>

struct Entity;
struct Scenario;
struct MotionSegment;
enum class CoordMode : uint8_t;

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

    // Pin a DIS Domain 3 (Surface) platform to altitude 0, in place.
    //
    // SamplePose applies this itself, but it has to be reachable because the playback
    // waypoint ring in ScenarioWorker is a fast path for a lone non-following Ellipse:
    // it builds ECEF points straight from the ellipse frame and never calls SamplePose.
    // A ship on a closed orbit is precisely the case that takes that path, so a clamp
    // that lived only in SamplePose missed the entity it existed to fix.
    // seaLevelM is the ellipsoid height of the local sea surface: the scenario
    // origin altitude, which is what the author sets it to (about +20 m around
    // Taiwan, where the geoid sits that far above the ellipsoid).
    void ClampSurfaceToSeaLevel(const Entity& entity, double seaLevelM,
                                double& ecefX, double& ecefY, double& ecefZ);

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

    // ----- Segment endpoint representation sync -----

    // A Line segment's start/end each store THREE coordinate representations
    // (Local ENU offsets, ECEF, geodetic Lat/Lon/Alt); the sampler and the
    // Motion tab both pick one per `s.coordMode`. Editors that touch only one
    // rep (the Preview drag writes Local; the Motion tab writes the coordMode
    // rep) leave the other two stale, so the Motion tab shows wrong/unchanged
    // numbers and the sampler can resolve a different point than the one drawn.
    //
    // SyncSegmentEndpoint re-derives all three reps of one endpoint from the
    // rep named by `fromMode`, pivoting through ECEF, using the scenario origin
    // for the Local frame. Call it after any single-rep edit to keep the three
    // representations consistent. `isEnd` selects the end triple; false selects
    // the start triple.
    void SyncSegmentEndpoint(MotionSegment& s, bool isEnd, CoordMode fromMode,
                             double originLatDeg, double originLonDeg,
                             double originAltM);

    // ----- Terminal explosion (MotionSegment::impactEntityId) -----

    // The leg that ends `entity` in an explosion: its first ENABLED Line whose
    // impactEntityId is set, or nullptr. The entity explodes at that leg's
    // endSecond; anything authored after it never plays.
    const MotionSegment* TerminalImpactSegment(const Entity& entity);

    // Index of the target entity in scenario.entities for an impact leg, or -1
    // when the leg has no target or the target is missing / disabled.
    int ImpactTargetIndex(const MotionSegment& segment, const Scenario& scenario);

    // ECEF point where an impact leg hits its target: the target's pose at the
    // leg's endSecond, offset by impactForward/Right/Up in the target's heading
    // frame. Returns false when the target cannot be resolved.
    bool ImpactPointEcef(const MotionSegment& segment, const Scenario& scenario,
                         double outEcef[3]);

    // Solve the impact time for a leg that leaves `startEcef` at `startSecond` at
    // `speedMps` toward `target` (+ the leg's impact offset): the earliest t with
    // |impact(t) - start| <= speed * (t - startSecond), so a moving target is met
    // where it will be, not where it was. Searches up to `maxSecond`; if the
    // target cannot be caught by then, returns the time with the smallest shortfall.
    double SolveImpactTime(const MotionSegment& segment, const Scenario& scenario,
                           const double startEcef[3], double startSecond,
                           double speedMps, double maxSecond);

    // ----- Take-off (accelerateFromStop) acceleration profile -----
    // A take-off Line starts at a full stop and accelerates along a jet profile:
    // strong push at brake release that tapers as drag builds (physics
    // dv/dt = A - B*v^2 → velocity ~ tanh(k*u); see EvaluateSegment). TakeoffTaperK()
    // is the shape constant k. TakeoffWindowFactor() is C(k) = k*tanh(k)/ln(cosh(k)),
    // the multiplier a take-off window MUST use — endSecond = start + C(k)*length/speed
    // — so the entity reaches EXACTLY cruise at the end. Keep every take-off window
    // site (PlotTakeoffLine / RechainFollowingTimes / DragLineEnd) on this factor and
    // in sync with the easing in MotionSampler.cpp (and dis-service's copy).
    double TakeoffTaperK();
    double TakeoffWindowFactor();
}
