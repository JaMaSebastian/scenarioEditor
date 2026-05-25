#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Coordinate mode used by the scenario's default and per-entity initial
// position. Stored as numeric for compact comparison; the INI writer maps
// to/from the spec's named strings ("LatLonAlt", "Local", "ECEF").
enum class CoordMode : uint8_t
{
    LatLonAlt = 0,
    Local     = 1,
    ECEF      = 2,
};

// Physical-model application mode. Drives the Validator (warn / error on
// envelope violations) in Phase 1; the sampler will clamp motion to
// envelope in Phase 2 (Limit mode).
enum class PhysicalModelMode : uint8_t
{
    Ignore   = 0,  // no envelope checks; current Slice 1 behavior
    Validate = 1,  // emit motion as authored; Validator surfaces violations
    Limit    = 2,  // sampler clamps to envelope at runtime (Phase 2)
};

// Per-entity override of the scenario-wide PhysicalModelMode. Inherit
// means "fall through to scenario.defaultPhysicalModel".
enum class PhysicalModelOverride : uint8_t
{
    Inherit  = 0,
    Ignore   = 1,
    Validate = 2,
    Limit    = 3,
};

// Motion segment types per spec §23.2. v2 ships all four below; Waypoint
// and Circle remain §23.3 deferred.
enum class MotionType : uint8_t
{
    Stationary = 0,  // hold position; orientation fixed at start pose
    StopHold   = 1,  // resume from the previous segment's end pose
    Line       = 2,  // linear interpolation in geodetic lat/lon/alt
    Ellipse    = 3,  // parametric orbit defined by two foci + length + speed + bearing
};

enum class EllipseDirection : uint8_t
{
    Clockwise        = 0,
    CounterClockwise = 1,
};

struct MotionSegment
{
    bool        enabled        = true;
    MotionType  type           = MotionType::Stationary;

    double      startSecond    = 0.0;
    double      endSecond      = 0.0;

    // ---- Start / End pose (Stationary uses start only; Line interpolates
    //      start→end; StopHold ignores both and samples the prior segment) ----
    CoordMode   coordMode      = CoordMode::LatLonAlt;
    double      startLat       = 0.0;
    double      startLon       = 0.0;
    double      startAlt       = 0.0;
    double      startEcefX     = 0.0;
    double      startEcefY     = 0.0;
    double      startEcefZ     = 0.0;
    double      startHeadingDeg = 0.0;
    double      startPitchDeg   = 0.0;
    double      startRollDeg    = 0.0;

    double      endLat         = 0.0;
    double      endLon         = 0.0;
    double      endAlt         = 0.0;
    double      endEcefX       = 0.0;
    double      endEcefY       = 0.0;
    double      endEcefZ       = 0.0;
    double      endHeadingDeg  = 0.0;
    double      endPitchDeg    = 0.0;
    double      endRollDeg     = 0.0;

    // ---- Local-XYZ siblings (used when coordMode == Local). Offsets in
    //      metres from the SCENARIO origin (originLatDeg/originLonDeg/
    //      originAltM), in ENU (East/North/Up). Resolved to ECEF on demand
    //      by MotionSampler via CoordTransforms::LocalEnuToEcefDeg. ----
    double      startLocalX    = 0.0;
    double      startLocalY    = 0.0;
    double      startLocalZ    = 0.0;
    double      endLocalX      = 0.0;
    double      endLocalY      = 0.0;
    double      endLocalZ      = 0.0;

    // ---- Line-specific knobs (matches spec §11.2 example) ----
    // SpeedMode / HeadingMode kept as strings for v2; sampler ignores them
    // (always uses constant-rate lerp). Persisted for round-trip fidelity.
    std::string speedMode      = "CalculateFromTime";
    std::string headingMode    = "CalculateFromPath";

    // ---- Ellipse-specific fields (two-foci + length form) ----
    // Foci of the ellipse in the segment's coordMode frame. Each focus
    // carries Lat/Lon/Alt + Local X/Y/Z + ECEF X/Y/Z; the active triple
    // is chosen by `coordMode` at sample time, same pattern as Line
    // start/end. The "string length" (sum of distances from any orbit
    // point to both foci, = 2a) determines the orbit size.
    double      f1Lat          = 0.0;
    double      f1Lon          = 0.0;
    double      f1Alt          = 0.0;
    double      f1LocalX       = 0.0;
    double      f1LocalY       = 0.0;
    double      f1LocalZ       = 0.0;
    double      f1EcefX        = 0.0;
    double      f1EcefY        = 0.0;
    double      f1EcefZ        = 0.0;
    double      f2Lat          = 0.0;
    double      f2Lon          = 0.0;
    double      f2Alt          = 0.0;
    double      f2LocalX       = 0.0;
    double      f2LocalY       = 0.0;
    double      f2LocalZ       = 0.0;
    double      f2EcefX        = 0.0;
    double      f2EcefY        = 0.0;
    double      f2EcefZ        = 0.0;
    double      lengthMeters   = 0.0;     // "string" = sum of distances = 2a
    double      startBearingDeg = 0.0;    // compass deg from center: 0=N, 90=E, 180=S, 270=W
    double      speedMps       = 0.0;     // tangential m/s (0 means seed from airframe cruise)
    EllipseDirection direction = EllipseDirection::Clockwise;

    std::string description;
};

struct Entity
{
    // ----- Model bookkeeping (not on the wire) -----
    bool        enabled        = true;
    std::string name           = "Drone 1";
    std::string description;

    // ----- DIS Entity ID (wire) -----
    uint16_t    siteId         = 1;
    uint16_t    applicationId  = 1;
    uint16_t    entityId       = 1;

    // ----- Force ID (wire): 0 Other, 1 Friendly, 2 Opposing, 3 Neutral -----
    uint8_t     forceId        = 1;

    // ----- Entity Type (wire, SISO-REF-010) -----
    uint8_t     kind           = 1;
    uint8_t     domain         = 2;
    uint16_t    country        = 225;
    uint8_t     category       = 1;
    uint8_t     subcategory    = 1;
    uint8_t     specific       = 0;
    uint8_t     extra          = 0;

    // ----- Marking (wire) -----
    std::string marking        = "SCN-001";

    // ----- Timing (model, drives PDU generation later) -----
    double      beginSecond    = 0.0;
    double      endSecond      = 300.0;
    double      updateRateHz   = 5.0;

    // Initial linear speed in m/s. Used by the PDU builder to seed the
    // EntityStatePdu LinearVelocity field (§14.5.2). Sign convention is
    // "scalar speed along the entity's initial heading vector"; for
    // stationary entities this is 0. The motion sampler may overwrite the
    // velocity at runtime; this is just the t=0 value.
    double      initialSpeedMps = 0.0;

    // ----- Initial position (model; ECEF is what the wire builder
    //       consumes today, the others land with coord-transform slice) -----
    CoordMode   initialCoordMode = CoordMode::Local;
    double      lat            = 0.0;
    double      lon            = 0.0;
    double      alt            = 0.0;
    double      localX         = 0.0;
    double      localY         = 0.0;
    double      localZ         = 0.0;
    double      ecefX          = -2706178.0;
    double      ecefY          = -4261067.0;
    double      ecefZ          = 3885731.0;

    // ----- Orientation (wire) in radians -----
    float       psi            = 0.0f;
    float       theta          = 0.0f;
    float       phi            = 0.0f;
    // ...and degree-friendly UI mirrors that the page surfaces:
    double      headingDeg     = 0.0;
    double      pitchDeg       = 0.0;
    double      rollDeg        = 0.0;

    // ----- Motion segments (§8 / §16). Empty = entity stays at the
    //       initial pose forever (Slice 1 "Hello DIS" behavior). -----
    std::vector<MotionSegment> motionSegments;

    // ----- Physical model (Phase 1: validator hint; Phase 2: clamp). -----
    PhysicalModelOverride physicalModelOverride  = PhysicalModelOverride::Inherit;
    bool                  overrideSpeedMultiplier = false;
    double                speedMultiplier         = 1.0;
};

// Output settings — drives ScenarioWorker's sink selection at Start. v2
// supports UDP unicast, UDP multicast, file recording, and preview (no I/O).
// TCP is deferred to a later version per spec §23.2.
enum class OutputMode : uint8_t
{
    UdpUnicast     = 0,
    UdpMulticast   = 1,
    Tcp            = 2,   // deferred; selectable in UI but rejected at Start
    FileRecording  = 3,
    PreviewOnly    = 4,
};

struct OutputConfig
{
    OutputMode  mode            = OutputMode::UdpUnicast;

    // UDP unicast
    std::string unicastIp       = "127.0.0.1";
    uint16_t    unicastPort     = 3000;

    // UDP multicast
    std::string multicastGroup  = "239.1.2.3";
    uint16_t    multicastPort   = 3000;
    std::string multicastInterface = "0.0.0.0"; // 0.0.0.0 = OS default IF
    int         multicastTtl    = 1;
    bool        multicastLoopback = true;

    // File recording / replay
    std::string recordingPath;
    std::string replayPath;

    // Playback controls
    double      playbackSpeed   = 1.0;
    bool        loopEnabled     = false;
};

struct Scenario
{
    // ----- Identity / metadata -----
    std::string name           = "Untitled";
    std::string description;

    // ----- DIS header fields -----
    uint8_t     protocolVersion = 7;
    uint8_t     exerciseId     = 1;
    uint16_t    siteId         = 1;
    uint16_t    applicationId  = 1;

    // ----- Timing defaults -----
    double      durationSeconds      = 300.0;
    double      defaultUpdateRateHz  = 5.0;
    CoordMode   defaultCoordMode     = CoordMode::Local;

    // ----- Origin (deferred to coord-transform slice; persisted nonetheless) -----
    double      originLatDeg   = 0.0;
    double      originLonDeg   = 0.0;
    double      originAltM     = 0.0;

    // ----- Entities (asset tree). UI edits entities[selectedIdx]; the
    //       sample worker fans out per-tick across all enabled entries. -----
    std::vector<Entity> entities = { Entity{} };

    Entity&       entity()       { return entities.front(); }
    const Entity& entity() const { return entities.front(); }

    // ----- Output config (§9, §11.2 [Output] section) -----
    OutputConfig output;

    // ----- Physical-model defaults (§spec.mk Future Enhancements). Each
    //       entity may override or inherit these values. Phase 1 wires
    //       the validator; Phase 2 wires the sampler. -----
    PhysicalModelMode defaultPhysicalModel   = PhysicalModelMode::Ignore;
    bool              speedMultiplierEnabled = false;
    double            speedMultiplier        = 1.0;
};
