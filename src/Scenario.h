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

// How a CameraFrame is anchored. Entity cameras mount a saved Cameras.ini
// preset on a scenario entity; Stationary cameras sit at a fixed vantage point.
enum class CameraKind : uint8_t
{
    Entity     = 0,  // preset mounted on sourceEntityId (from config\Cameras.ini)
    Stationary = 1,  // fixed vantage at vantEast/North/Up (scenario-origin ENU metres)
};

// Visual transition used when the schedule switches from the previous frame's
// camera to this one.
enum class CameraTransition : uint8_t
{
    CrossfadeBlend = 0,  // dissolve/blend between the two views
    HardCut        = 1,  // instantaneous switch
};

//-----------------------------------------------------------------------------
// CameraFrame — one timed camera in the scenario's camera schedule
//   Authored on the Preview tab and serialized as a [Camera.N] section inside
//   scenario.ini by ScenarioIO. Frames tile [0, duration] contiguously (shared
//   edges): sorted by beginSecond with front().begin==0, begin[i]==end[i-1], and
//   back().end==duration. Entity cameras mount a Cameras.ini preset on a source
//   entity; Stationary cameras sit at a fixed origin-ENU vantage. Either kind may
//   track a target entity (targetEntityId==0 keeps an Entity camera focused on its
//   own source).
//
//   Exactly one frame is live at a time — DISBrowser's director picks the frame
//   whose [beginSecond, endSecond] contains the scenario clock — so the zoom
//   fields below only ever apply to the camera whose timeline box is active.
//-----------------------------------------------------------------------------
struct CameraFrame
{
    CameraKind        kind           = CameraKind::Entity;

    // ----- Entity camera (kind == Entity) -----
    uint16_t          sourceEntityId = 0;   // entity whose preset we mount (0 = unset)
    std::string       presetType;           // Cameras.ini <Type>  e.g. "Octopus"
    std::string       presetAngle;          // Cameras.ini <Angle> e.g. "front"

    // ----- Focus/target (both kinds) -----
    uint16_t          targetEntityId = 0;   // entity to track; 0 = focus on source

    // ----- Stationary vantage (kind == Stationary), scenario-origin ENU metres -----
    double            vantEastM  = 0.0;
    double            vantNorthM = 0.0;
    double            vantUpM    = 0.0;

    CameraTransition  transition   = CameraTransition::CrossfadeBlend; // vs previous frame
    double            beginSecond  = 0.0;
    double            endSecond    = 0.0;
    std::string       label;                // shown on the timeline box

    // ----- Zoom (both kinds; authored here, executed by DISBrowser) -----
    // zoomEnabled off is the legacy behaviour: Entity frames keep the FOV baked
    // into their Cameras.ini preset and Stationary frames keep the engine default.
    bool              zoomEnabled  = false;  // master toggle for this frame
    double            zoomFovDeg   = 90.0;   // static FOV override (used when !dynamicZoom)
    bool              dynamicZoom  = false;  // auto-fit the target; overrides zoomFovDeg
    double            zoomFillPct  = 85.0;   // % of the frame the target should occupy

    // Target extents (metres) resolved from EntityTypeCatalog at author time so
    // DISBrowser needs no catalog reader. 0 = the catalog had no dimensions for
    // that type, in which case the runtime falls back to the static FOV.
    // Refreshed by CPreviewPage::RefreshCameraTargetExtents().
    double            targetLengthM = 0.0;
    double            targetWidthM  = 0.0;   // wingspan / beam
    double            targetHeightM = 0.0;

    // ----- Gimbal limits (Entity frames only; authored in DISBrowser's hanger) -----
    // Enforce the mounted preset's body-relative travel envelope while this frame is live.
    // DISBrowser clamps only when this is on AND the preset carries LimitsEnabled=1, so a
    // play authored before the feature is unaffected no matter what Cameras.ini later says.
    bool              limitsEnabled = false;

    // True when the catalog gave us something to fit to. Dynamic zoom is
    // meaningless without it (DISBrowser falls back to the static FOV), so the
    // authoring UI grays the Dynamic toggle when this is false.
    bool HasTargetSize() const
    {
        return targetLengthM > 0.0 || targetWidthM > 0.0 || targetHeightM > 0.0;
    }
};

// Zoom authoring limits, shared by the Entity Camera dialog and the canvas /
// timeline popups so both reject the same values. The FOV range matches
// DISBrowser's own FOV slider, so anything authored here is reachable in the
// runtime HUD too.
constexpr double kZoomFovMinDeg  = 5.0;
constexpr double kZoomFovMaxDeg  = 170.0;
constexpr double kZoomFillMinPct = 10.0;
constexpr double kZoomFillMaxPct = 100.0;

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

// Output settings — drives ScenarioWorker's sink selection at Start. Supports
// UDP unicast, UDP multicast, TCP direct connection (§17.4), file recording,
// and preview (no I/O).
enum class OutputMode : uint8_t
{
    UdpUnicast     = 0,
    UdpMulticast   = 1,
    Tcp            = 2,
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
    // 224.252.0.1 is DISBrowser's documented default and sits in the DIS
    // administrative scope 224.252.0.0-224.255.255.255 (RFC 2365 §6.3); the
    // low octet matches the default ExerciseID (1), which DISBrowser checks.
    std::string multicastGroup  = "224.252.0.1";
    uint16_t    multicastPort   = 3001;   // keep aligned with DISBrowser's DIS listen port
    std::string multicastInterface = "0.0.0.0"; // 0.0.0.0 = OS default IF
    int         multicastTtl    = 1;
    bool        multicastLoopback = true;

    // TCP direct connection (§9.5, §17.4). The stream carries raw DIS PDUs
    // back to back with no extra framing: each PDU's header Length field
    // (bytes 8-9, big-endian) delimits it, so the bytes on a TCP connection
    // are identical to what the UDP sinks emit.
    bool        tcpListen       = false;  // false = connect out (client), true = listen (server)
    std::string tcpRemoteHost   = "127.0.0.1";  // client mode: where to connect
    uint16_t    tcpRemotePort   = 3002;         // client mode: DISBrowser's [DIS] TcpListenPort
    uint16_t    tcpListenPort   = 3002;         // server mode: local port to accept on
    bool        tcpReconnect    = true;   // client mode: retry a dropped/refused connection
    int         tcpTimeoutMs    = 3000;   // connect / accept timeout

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

// Which sub-set of the Coconut_Palm_Pack palms to place (only meaningful when
// FoliageConfig::palm is true).
enum class PalmKind : uint8_t { All = 0, Tall = 1, Straight = 2 };

// How the selected tree meshes should be placed on the terrain. Chosen on the
// Preview-tab Foliage dialog so the same scenario can be experimented with under
// each backend. (The backends themselves are separate, larger work; this only
// records the choice.)
enum class FoliageRenderMode : uint8_t { InEngine = 0, I3dm = 1, BlenderGIS = 2 };

//-----------------------------------------------------------------------------
// FoliageConfig — Preview-tab "Foliage" selections, persisted in [Foliage]
//   Which tree packs to scatter (Oak=TreePackOak, BigTrees=Landscaping_Big_Trees,
//   Palm=Coconut_Palm_Pack), the palm sub-kind, and the rendering backend.
//-----------------------------------------------------------------------------
struct FoliageConfig
{
    bool              oak        = false;   // TreePackOak
    bool              bigTrees   = false;   // Landscaping_Big_Trees
    bool              palm       = false;   // Coconut_Palm_Pack
    PalmKind          palmKind   = PalmKind::All;            // used only when palm==true
    FoliageRenderMode renderMode = FoliageRenderMode::InEngine;
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

    // ----- Origin -----
    // A fresh scenario's origin is PROVISIONAL (originSet=false): the lat/lon here
    // is just the default map view (Hurlburt Field, FL) the operator navigates from;
    // it reads blank in the UI and isn't handed to DISBrowser. Saving commits it
    // (originSet=true), and any loaded scenario is committed. Altitude defaults to
    // 0 m (sea level / ellipsoid) so local Z=0 matches the water surface in
    // DISBrowser's self-hosted terrain (ocean tiles fill at height 0).
    double      originLatDeg   = 30.4275;    // Hurlburt Field, FL — default view
    double      originLonDeg   = -86.6893;
    double      originAltM     = 0.0;
    bool        originSet      = false;      // false = provisional (blank in UI); true = committed

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

    // ----- Foliage selections (Preview-tab "Foliage" dialog; [Foliage] section) -----
    FoliageConfig foliage;

    // ----- Entities (asset tree). UI edits entities[selectedIdx]; the
    //       sample worker fans out per-tick across all enabled entries. A fresh
    //       scenario starts EMPTY — the operator adds entities via "New". -----
    std::vector<Entity> entities;

    Entity&       entity()       { return entities.front(); }
    const Entity& entity() const { return entities.front(); }

    // ----- Output config (§9, §11.2 [Output] section) -----
    OutputConfig output;

    // ----- Camera schedule (Preview-tab authoring; [Camera.N] in scenario.ini).
    //       A contiguous list of CameraFrames tiling [0, duration]. Empty = none. -----
    std::vector<CameraFrame> cameras;

    // ----- Physical-model defaults (§spec.mk Future Enhancements). Each
    //       entity may override or inherit these values. Phase 1 wires
    //       the validator; Phase 2 wires the sampler. -----
    PhysicalModelMode defaultPhysicalModel   = PhysicalModelMode::Ignore;
    bool              speedMultiplierEnabled = false;
    double            speedMultiplier        = 1.0;
};
