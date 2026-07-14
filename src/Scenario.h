//=============================================================================
//  Scenario.h
//-----------------------------------------------------------------------------
//  In-memory data model for a DIS scenario: the plain-old-data structs and
//  enums that describe entities, their per-segment motion, output/sink config,
//  an optional terrain overlay, and scenario-wide defaults. This is the model
//  that the editor UI edits and that ScenarioIO serializes to/from scenario.ini.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
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

// Direction of travel around an Ellipse orbit, as seen from above.
enum class EllipseDirection : uint8_t
{
    Clockwise        = 0,
    CounterClockwise = 1,
};

//-----------------------------------------------------------------------------
// MotionSegment — one timed leg of an entity's trajectory
//   Carries a start/end pose (in Lat/Lon/Alt, Local ENU, or ECEF per coordMode)
//   plus type-specific knobs (Line take-off easing; Ellipse two-foci + length,
//   bearing, speed, direction, optional follow-target). Consumed by the sampler.
//-----------------------------------------------------------------------------
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

    // Take-off (Line only): when true the entity starts STOPPED and accelerates
    // at a constant rate along the line, reaching the segment's speed (cruise) at
    // the end. The sampler eases position in with u^2 instead of a linear lerp;
    // the plot handler sets the duration to 2*length/speed so the end speed
    // equals cruise. A following normal Line then continues at cruise (smooth
    // hand-off) and is where the operator raises the end altitude to climb.
    bool        accelerateFromStop = false;

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

    // Entity Ellipse: if >= 0, the orbit center follows this target's EntityID
    // as it moves through the scenario (the stored foci define only the orbit's
    // shape/orientation; their midpoint is re-centered on the target at sample
    // time). -1 = static ellipse (normal behavior).
    int         followEntityId = -1;

    std::string description;
};

//-----------------------------------------------------------------------------
// Entity — one simulated DIS entity in the scenario
//   Bundles DIS identity/type/marking (wire fields), timing and update rate,
//   an initial pose (Lat/Lon/Alt, Local, or ECEF), orientation, an ordered
//   list of MotionSegments, and per-entity physical-model overrides.
//-----------------------------------------------------------------------------
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

//-----------------------------------------------------------------------------
// OutputConfig — where and how generated PDUs are emitted
//   Holds the selected OutputMode plus the settings each sink needs (UDP
//   unicast/multicast endpoints, file record/replay paths) and playback
//   controls (speed, loop).
//-----------------------------------------------------------------------------
struct OutputConfig
{
    OutputMode  mode            = OutputMode::UdpUnicast;

    // UDP unicast
    std::string unicastIp       = "127.0.0.1";
    uint16_t    unicastPort     = 3001;   // match DISBrowser Config/DISBrowser.ini [DIS] ListenPort

    // UDP multicast
    std::string multicastGroup  = "239.1.2.3";
    uint16_t    multicastPort   = 3001;   // keep aligned with DISBrowser's DIS listen port
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

// Axis-aligned background footprint (e.g. a sim level's land or water body)
// drawn under the entities on the Preview tab. Bounds are in ENU metres
// relative to the scenario origin: East = local X, North = local Y.
struct LevelRect
{
    bool   enabled   = false;
    double eastMinM  = 0.0;
    double eastMaxM  = 0.0;
    double northMinM = 0.0;
    double northMaxM = 0.0;
};

// A named sub-region inside the level overlay (forest, minefield, no-fly box,
// …) painted as a labelled, tinted rectangle on the Preview tab. Bounds share
// LevelRect's ENU-metre convention (East = X, North = Y, relative to origin).
// The optional height range records vertical extent (e.g. tree-canopy heights)
// for round-trip fidelity even though the top-down preview only paints the
// footprint.
struct LevelZone
{
    bool        enabled         = true;
    std::string name;
    double      eastMinM        = 0.0;
    double      eastMaxM        = 0.0;
    double      northMinM       = 0.0;
    double      northMaxM       = 0.0;
    double      heightMinMeters = 0.0;
    double      heightMaxMeters = 0.0;
    uint32_t    colorRgb        = 0x2E7D32;  // 0xRRGGBB fill tint; forest green default
};

// Optional terrain overlay imported from an external simulation level so the
// Preview canvas can show where the land and ocean sit relative to entities.
// Disabled by default; populated only when the scenario .ini carries a
// [Level] section (see ScenarioIO).
struct LevelOverlay
{
    bool                   enabled        = false;
    std::string            name;
    double                 seaLevelMeters = 0.0;
    LevelRect              land;
    LevelRect              ocean;
    std::vector<LevelZone> zones;   // named sub-regions ([Level.Zone0], …)
};

//-----------------------------------------------------------------------------
// Scenario — the top-level document edited by the app and (de)serialized by ScenarioIO
//   Aggregates identity/metadata, DIS header fields, timing defaults, the
//   geodetic origin, an optional painted terrain-bounds box and Preview-tab
//   LevelOverlay, the list of entities, the OutputConfig, and physical-model
//   defaults that individual entities may inherit or override.
//-----------------------------------------------------------------------------
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

    // ----- 3D terrain boundary (Preview tab "Boundary" paint tool) -----
    // A geographic box the operator paints on the Preview map; it defines the DEM footprint that gets
    // tiled into Cesium 3D terrain for the DISBrowser generic level (via Scripts/retile_terrain.cmd) and
    // is written to Config/Startup.ini [Generic]. Invalid until painted.
    bool        terrainBoundsValid  = false;
    double      terrainLatMinDeg    = 0.0;
    double      terrainLatMaxDeg    = 0.0;
    double      terrainLonMinDeg    = 0.0;
    double      terrainLonMaxDeg    = 0.0;

    // ----- Optional terrain overlay for the Preview tab ([Level] section) -----
    LevelOverlay level;

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
