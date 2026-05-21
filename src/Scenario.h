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

// Motion segment types per spec §23.2. v2 ships all four below; Waypoint
// and Circle remain §23.3 deferred.
enum class MotionType : uint8_t
{
    Stationary = 0,  // hold position; orientation fixed at start pose
    StopHold   = 1,  // resume from the previous segment's end pose
    Line       = 2,  // linear interpolation in geodetic lat/lon/alt
    Ellipse    = 3,  // parametric orbit around (centerLat, centerLon, centerAlt)
};

enum class EllipseDirection : uint8_t
{
    Clockwise        = 0,
    CounterClockwise = 1,
};

enum class AltitudeMode : uint8_t
{
    Constant = 0,
    Linear   = 1,
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

    // ---- Line-specific knobs (matches spec §11.2 example) ----
    // SpeedMode / HeadingMode kept as strings for v2; sampler ignores them
    // (always uses constant-rate lerp). Persisted for round-trip fidelity.
    std::string speedMode      = "CalculateFromTime";
    std::string headingMode    = "CalculateFromPath";

    // ---- Ellipse-specific fields ----
    double      centerLat      = 0.0;
    double      centerLon      = 0.0;
    double      centerAlt      = 0.0;
    double      radiusXMeters  = 0.0;     // ellipse semi-axis along ENU East
    double      radiusYMeters  = 0.0;     // ellipse semi-axis along ENU North
    double      rotationDeg    = 0.0;     // CCW rotation of the ellipse
    double      startAngleDeg  = 0.0;     // 0 = +X (East) before rotation
    EllipseDirection direction = EllipseDirection::Clockwise;
    double      periodSeconds  = 60.0;
    AltitudeMode altitudeMode  = AltitudeMode::Constant;

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

    // ----- Initial position (model; ECEF is what the wire builder
    //       consumes today, the others land with coord-transform slice) -----
    CoordMode   initialCoordMode = CoordMode::ECEF;
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
    CoordMode   defaultCoordMode     = CoordMode::LatLonAlt;

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
};
