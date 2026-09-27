//=============================================================================
//  CameraChannelProducer.h
//-----------------------------------------------------------------------------
//  The editor's side of the URZA-12253 camera channel: the newline-delimited
//  JSON stream that tells the visualizer which authored [Camera.N] frame is
//  live and where that camera sits, every tick, for as long as a run lasts.
//
//  THE VISUALIZER LISTENS, WE DIAL OUT. That is the opposite of the DIS side,
//  where the visualizer joins a group or binds a port and we fire datagrams
//  at it. Until this existed only dis-service and the Python scenario_player
//  produced the stream, so Run in the editor moved the entities and left the
//  viewport on whatever camera it already had.
//
//  Message shapes are those of tools/dis-server/scenario_player.py in the
//  visualizer repo, which is what the visualizer's CameraChannelListener was
//  written against:
//
//    {"t":"session", "proto":1, "scenario":..., "durationSec":..., "rateHz":...,
//     "aspect":..., "origin":[lat,lon,alt], "frames":N}
//    {"t":"cut",  "f":N, "ts":t, "kind":"entity"|"stationary", "label":...,
//     "presetType":..., "presetAngle":..., "transition":..., "blend":s,
//     "limitsEnabled":b, "src":{"site","app","ent"}, "tgt":{...}}
//    {"t":"pose", "f":N, "ts":t, "kind":..., "frame":"body", "off":[fwd,right,up],
//     "rot":[pitch,heading,roll], "fov":deg, "pinned":false, "src":{...}}
//    {"t":"pose", "f":N, "ts":t, "kind":"stationary", "frame":"world",
//     "lla":[lat,lon,alt], "rot":[pitch,heading,roll], "fov":deg, "pinned":false}
//    {"t":"end",  "ts":t, "reason":"completed"|"stopped"}
//    {"t":"crew", "ent":{"site","app","ent"}, "count":N}
//
//  "crew" goes out once per entity with a deck crew, right after "session": how
//  many of the crew authored for that ship type (DISBrowser hanger, Hanger.ini
//  Crew= lines) the visualizer puts on it. A visualizer that predates it ignores
//  the unknown type.
//
//  Units on the wire are metres and degrees; the visualizer converts once.
//
//  The pose solver is a port of scenario_player.py's solve_pose: a preset's
//  saved Pitch/Yaw/Roll is NOT an orientation, it is a trim captured against a
//  look-at aimed at the entity's own origin, so a shot that looks at its own
//  source composes (look-at-origin ∘ trim) and a shot that tracks another
//  entity composes (aim-at-target ∘ roll-only trim).
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-17
//=============================================================================
#pragma once

#include "CameraPresetCatalog.h"
#include "Scenario.h"
#include "TcpSender.h"

#include <cstdint>
#include <string>
#include <vector>

// A body-frame rotator, Unreal convention: degrees, pitch positive nose-up,
// yaw positive to the right, roll positive right-wing-down.
struct CameraRotator
{
    double pitch = 0.0;
    double yaw   = 0.0;
    double roll  = 0.0;
};

// Mount offset in the source's body frame, metres: forward, right, up.
struct CameraOffset
{
    double forward = 0.0;
    double right   = 0.0;
    double up      = 0.0;
};

// Where an entity is and which way it faces, at one scenario time.
struct CameraEntityState
{
    double ecefX = 0.0, ecefY = 0.0, ecefZ = 0.0;
    double headingDeg = 0.0;      // compass, 0 = north, clockwise positive
};

// One solved camera placement, ready to serialise.
struct CameraSolvedPose
{
    bool          stationary = false;
    CameraOffset  off;            // entity shots: body-frame mount
    double        latDeg = 0.0, lonDeg = 0.0, altM = 0.0;   // stationary shots
    CameraRotator rot;            // body frame (entity) or world HPR (stationary)
    double        fovDeg = 90.0;  // horizontal
};

namespace CameraChannelMath
{
    // FRotationMatrix rows for a rotator; composing two rotators the way
    // FQuat(outer) * FQuat(inner) does is a plain row-vector product.
    CameraRotator Compose(const CameraRotator& outer, const CameraRotator& inner);

    // Body-frame rotator that points a camera mounted at `off` back at the
    // entity origin: the look-at every hanger preset was trimmed against.
    CameraRotator LookAtOrigin(const CameraOffset& off);

    // Body-frame rotator that points a camera mounted `off` metres on a source
    // facing `headingDeg` at an ECEF target. Roll is always 0.
    CameraRotator AimAtTarget(const CameraEntityState& src, const CameraOffset& off,
                              double tgtX, double tgtY, double tgtZ);

    // World heading/pitch from one ECEF point towards another, in the local
    // east/north/up frame of the first. Roll is 0.
    CameraRotator AimWorld(double fromX, double fromY, double fromZ,
                           double toX,   double toY,   double toZ);

    // Authored ZoomFovDeg, or the dynamic FOV that makes the target's bounding
    // sphere fill ZoomFillPercent of the frame height (DISBrowser's
    // ResolveZoomFov, widened by the aspect because the FOV is horizontal).
    double FovForDistance(const CameraFrame& frame, double distM, double aspect);

    // solve_pose: (off, rot, fov) for an Entity frame. `tgt` is null when the
    // shot looks at its own source (or the target could not be sampled).
    CameraSolvedPose SolveEntityPose(const CameraFrame& frame, const CameraPreset& preset,
                                     const CameraEntityState& src,
                                     const CameraEntityState* tgt, double aspect);

    // A Stationary frame: vantage from the scenario-origin ENU, aimed at the
    // target when there is one, otherwise level along north.
    CameraSolvedPose SolveStationaryPose(const CameraFrame& frame, const Scenario& scn,
                                         const CameraEntityState* tgt, double aspect);

    // The mount used when Cameras.ini has no block for a shot's preset: two
    // metres up with a touch of down-tilt, visibly "on the entity".
    const CameraPreset& DefaultPreset();
}

namespace CameraChannelJson
{
    // Each returns one complete line INCLUDING the trailing newline.
    std::string Session(const Scenario& scn, double rateHz, double aspect);
    std::string Cut(const Scenario& scn, size_t frameIdx, double tSec);
    std::string Pose(const Scenario& scn, size_t frameIdx, double tSec,
                     const CameraSolvedPose& pose);
    std::string End(double tSec, const char* reason);
    std::string Crew(const Entity& e);

    // JSON string escaping for labels and preset names.
    std::string Quote(const std::string& s);
}

class CameraChannelProducer
{
public:
    CameraChannelProducer() = default;
    ~CameraChannelProducer();

    CameraChannelProducer(const CameraChannelProducer&) = delete;
    CameraChannelProducer& operator=(const CameraChannelProducer&) = delete;

    // Dial the visualizer and send the session header. Returns false when it
    // is not listening -- NOT an error for the run: entities still play, the
    // viewport is simply not directed. `presets` must outlive this object.
    bool Start(const Scenario& scn, const std::vector<CameraPreset>& presets,
               const std::string& host, uint16_t port, int timeoutMs, double poseRateHz);

    // Drive from the playback loop with the scenario clock. Emits a cut when
    // the live frame changes and a pose at the configured rate. Cheap when
    // nothing is due.
    void Tick(const Scenario& scn, double tSec);

    // Loop restart: forget the live frame so the first shot cuts again.
    void Rewind();

    // Send the end record and hang up. Safe to call when not live.
    void End(double tSec, const char* reason);

    bool Live() const { return m_live; }

    // How many cuts / poses went out (for the log line at the end of a run).
    uint64_t CutCount()  const { return m_cuts; }
    uint64_t PoseCount() const { return m_poses; }

private:
    bool SendLine(const std::string& line);
    int  ActiveFrame(const Scenario& scn, double tSec) const;
    static bool SampleEntity(const Scenario& scn, uint16_t entityId, double tSec,
                             CameraEntityState& out);

    TcpSender                         m_tcp;
    const std::vector<CameraPreset>*  m_presets  = nullptr;
    bool                              m_live     = false;
    int                               m_current  = -1;     // index into scn.cameras
    double                            m_poseDt   = 1.0 / 30.0;
    double                            m_nextPose = 0.0;
    uint64_t                          m_cuts     = 0;
    uint64_t                          m_poses    = 0;
    std::vector<bool>                 m_warnedPreset;      // per frame: missing preset logged
};
