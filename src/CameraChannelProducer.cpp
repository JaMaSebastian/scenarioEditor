//=============================================================================
//  CameraChannelProducer.cpp
//-----------------------------------------------------------------------------
//  See CameraChannelProducer.h. Math is a line-for-line port of
//  scenario_player.py (visualizer repo, tools/dis-server) so the editor and the
//  Python player put the camera in the same place for the same play; the
//  Python file's docstrings are the reference when the two disagree.
//
//  Author:        Matt Sebastian
//  Date started:  2026-09-17
//=============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CameraChannelProducer.h"

#include "CoordTransforms.h"
#include "MotionSampler.h"
#include "../log.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr double kPi      = 3.14159265358979323846;
    constexpr double kDeg2Rad = kPi / 180.0;
    constexpr double kRad2Deg = 180.0 / kPi;
    constexpr int    kProto   = 1;          // CAMERA_PROTO in scenario_player.py
    constexpr double kAspect  = 16.0 / 9.0; // what the FOV solver assumes for the viewport

    struct Vec3 { double x, y, z; };

    Vec3   operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    Vec3   operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    Vec3   operator*(const Vec3& a, double s)      { return { a.x * s, a.y * s, a.z * s }; }
    double Dot(const Vec3& a, const Vec3& b)       { return a.x * b.x + a.y * b.y + a.z * b.z; }
    double Dist(const Vec3& a, const Vec3& b)
    {
        const Vec3 d = b - a;
        return std::sqrt(Dot(d, d));
    }

    // FRotationMatrix's rows -- the rotated basis axes -- for a rotator.
    struct Basis { Vec3 x, y, z; };

    Basis BasisOf(const CameraRotator& r)
    {
        const double p = r.pitch * kDeg2Rad, y = r.yaw * kDeg2Rad, ro = r.roll * kDeg2Rad;
        const double sp = std::sin(p), cp = std::cos(p);
        const double sy = std::sin(y), cy = std::cos(y);
        const double sr = std::sin(ro), cr = std::cos(ro);
        Basis b;
        b.x = { cp * cy,                    cp * sy,                    sp      };
        b.y = { sr * sp * cy - cr * sy,     sr * sp * sy + cr * cy,    -sr * cp };
        b.z = { -(cr * sp * cy + sr * sy),  cy * sr - cr * sp * sy,     cr * cp };
        return b;
    }

    // FMatrix::Rotator: pitch/yaw from the X axis, roll from Y and Z.
    CameraRotator RotatorOf(const Basis& m)
    {
        CameraRotator r;
        const double pitch = std::atan2(m.x.z, std::hypot(m.x.x, m.x.y));
        const double yaw   = std::atan2(m.x.y, m.x.x);
        const Vec3 sy = { -std::sin(yaw), std::cos(yaw), 0.0 };
        const double roll  = std::atan2(Dot(m.z, sy), Dot(m.y, sy));
        r.pitch = pitch * kRad2Deg;
        r.yaw   = yaw   * kRad2Deg;
        r.roll  = roll  * kRad2Deg;
        return r;
    }

    // Local east / north / up unit vectors (in ECEF) at a point.
    void EnuAxes(const Vec3& ecef, Vec3& east, Vec3& north, Vec3& up)
    {
        double latDeg = 0.0, lonDeg = 0.0, alt = 0.0;
        CoordTransforms::EcefToGeodeticDeg(ecef.x, ecef.y, ecef.z, latDeg, lonDeg, alt);
        const double la = latDeg * kDeg2Rad, lo = lonDeg * kDeg2Rad;
        east  = { -std::sin(lo),                std::cos(lo),                 0.0          };
        north = { -std::sin(la) * std::cos(lo), -std::sin(la) * std::sin(lo), std::cos(la) };
        up    = {  std::cos(la) * std::cos(lo),  std::cos(la) * std::sin(lo), std::sin(la) };
    }

    // Heading/pitch of a direction expressed in a (forward, right, up) frame.
    CameraRotator RotatorFromFru(double f, double r, double u)
    {
        CameraRotator out;
        out.pitch = std::atan2(u, std::hypot(f, r)) * kRad2Deg;
        out.yaw   = std::atan2(r, f) * kRad2Deg;
        out.roll  = 0.0;
        return out;
    }

    const char* TransitionName(CameraTransition t)
    {
        if (t == CameraTransition::HardCut) return "HardCut";
        if (t == CameraTransition::FlyTo)   return "FlyTo";
        return "CrossfadeBlend";
    }

    const Entity* FindEntity(const Scenario& scn, uint16_t entityId)
    {
        if (entityId == 0) return nullptr;
        for (const Entity& e : scn.entities)
            if (e.entityId == entityId) return &e;
        return nullptr;
    }

    void AppendId(std::string& out, const char* key, const Entity& e)
    {
        char buf[96];
        sprintf_s(buf, sizeof(buf), ",\"%s\":{\"site\":%u,\"app\":%u,\"ent\":%u}",
                  key, static_cast<unsigned>(e.siteId),
                  static_cast<unsigned>(e.applicationId),
                  static_cast<unsigned>(e.entityId));
        out += buf;
    }

    // Log helper: the worker thread has no szError of its own to share.
    void LogF(const char* fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsprintf_s(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        LOG(buf);
    }
}

// ============================ CameraChannelMath ============================

CameraRotator CameraChannelMath::Compose(const CameraRotator& outer, const CameraRotator& inner)
{
    // m[row][col] = sum_k inner[row][k] * outer[k][col]
    const Basis o = BasisOf(outer), i = BasisOf(inner);
    auto row = [&](const Vec3& ir) -> Vec3 {
        return { ir.x * o.x.x + ir.y * o.y.x + ir.z * o.z.x,
                 ir.x * o.x.y + ir.y * o.y.y + ir.z * o.z.y,
                 ir.x * o.x.z + ir.y * o.y.z + ir.z * o.z.z };
    };
    Basis m;
    m.x = row(i.x);
    m.y = row(i.y);
    m.z = row(i.z);
    return RotatorOf(m);
}

CameraRotator CameraChannelMath::LookAtOrigin(const CameraOffset& off)
{
    const double f = -off.forward, r = -off.right, u = -off.up;
    if (std::fabs(f) < 1e-9 && std::fabs(r) < 1e-9 && std::fabs(u) < 1e-9)
        return CameraRotator{};
    return RotatorFromFru(f, r, u);
}

CameraRotator CameraChannelMath::AimAtTarget(const CameraEntityState& src, const CameraOffset& off,
                                             double tgtX, double tgtY, double tgtZ)
{
    const Vec3 s = { src.ecefX, src.ecefY, src.ecefZ };
    Vec3 east, north, up;
    EnuAxes(s, east, north, up);
    const double h = src.headingDeg * kDeg2Rad;
    const Vec3 fwd   = north * std::cos(h) + east  * std::sin(h);
    const Vec3 right = east  * std::cos(h) - north * std::sin(h);
    const Vec3 cam   = s + fwd * off.forward + right * off.right + up * off.up;
    const Vec3 d     = Vec3{ tgtX, tgtY, tgtZ } - cam;
    return RotatorFromFru(Dot(d, fwd), Dot(d, right), Dot(d, up));
}

CameraRotator CameraChannelMath::AimWorld(double fromX, double fromY, double fromZ,
                                          double toX,   double toY,   double toZ)
{
    const Vec3 from = { fromX, fromY, fromZ };
    Vec3 east, north, up;
    EnuAxes(from, east, north, up);
    const Vec3 d = Vec3{ toX, toY, toZ } - from;
    // Heading is measured from north, clockwise: forward = north, right = east.
    return RotatorFromFru(Dot(d, north), Dot(d, east), Dot(d, up));
}

double CameraChannelMath::FovForDistance(const CameraFrame& frame, double distM, double aspect)
{
    const double staticFov = frame.zoomEnabled ? frame.zoomFovDeg : 90.0;
    if (!frame.zoomEnabled || !frame.dynamicZoom) return staticFov;

    const double l = frame.targetLengthM, w = frame.targetWidthM, h = frame.targetHeightM;
    const double radius = 0.5 * std::sqrt(l * l + w * w + h * h);
    if (radius <= 0.0 || distM <= radius) return staticFov;

    const double fill   = std::min(std::max(frame.zoomFillPct, 1.0), 100.0) / 100.0;
    const double theta  = std::asin(std::min(radius / distM, 0.999));
    const double padded = std::min(theta / fill, kPi / 2.0 - 1e-6);
    const double fov    = 2.0 * std::atan(aspect * std::tan(padded)) * kRad2Deg;
    return std::min(std::max(fov, 5.0), 170.0);      // DISBrowser's clamp
}

CameraSolvedPose CameraChannelMath::SolveEntityPose(const CameraFrame& frame, const CameraPreset& preset,
                                                    const CameraEntityState& src,
                                                    const CameraEntityState* tgt, double aspect)
{
    CameraSolvedPose out;
    out.stationary = false;
    out.off = { preset.x, preset.y, preset.z };
    out.fovDeg = frame.zoomEnabled ? frame.zoomFovDeg : preset.fov;

    if (tgt)
    {
        // Tracks another entity: mount from the preset, aim from the target,
        // trim dropped except roll (horizon framing), zoom closed to the fill.
        const CameraRotator look = AimAtTarget(src, out.off, tgt->ecefX, tgt->ecefY, tgt->ecefZ);
        out.rot = Compose(look, CameraRotator{ 0.0, 0.0, preset.roll });
        if (frame.zoomEnabled)
        {
            const double d = Dist({ src.ecefX, src.ecefY, src.ecefZ },
                                  { tgt->ecefX, tgt->ecefY, tgt->ecefZ });
            out.fovDeg = FovForDistance(frame, d, aspect);
        }
    }
    else
    {
        // Looks at its own source: look-at the origin from the mount, then the
        // full trim -- exactly the view the hanger HUD showed when it was saved.
        out.rot = Compose(LookAtOrigin(out.off),
                          CameraRotator{ preset.pitch, preset.yaw, preset.roll });
    }
    return out;
}

CameraSolvedPose CameraChannelMath::SolveStationaryPose(const CameraFrame& frame, const Scenario& scn,
                                                        const CameraEntityState* tgt, double aspect)
{
    CameraSolvedPose out;
    out.stationary = true;

    double X = 0.0, Y = 0.0, Z = 0.0;
    CoordTransforms::LocalEnuToEcefDeg(frame.vantEastM, frame.vantNorthM, frame.vantUpM,
                                       scn.originLatDeg, scn.originLonDeg, scn.originAltM,
                                       X, Y, Z);
    CoordTransforms::EcefToGeodeticDeg(X, Y, Z, out.latDeg, out.lonDeg, out.altM);

    out.fovDeg = frame.zoomEnabled ? frame.zoomFovDeg : 90.0;
    if (tgt)
    {
        out.rot = AimWorld(X, Y, Z, tgt->ecefX, tgt->ecefY, tgt->ecefZ);
        if (frame.zoomEnabled)
            out.fovDeg = FovForDistance(frame, Dist({ X, Y, Z }, { tgt->ecefX, tgt->ecefY, tgt->ecefZ }), aspect);
    }
    else
    {
        out.rot = CameraRotator{};       // level, looking north
    }
    return out;
}

const CameraPreset& CameraChannelMath::DefaultPreset()
{
    static const CameraPreset kDefault = []() {
        CameraPreset p;
        p.type = "(default)"; p.angle = "(default)";
        p.z = 2.0; p.pitch = -5.0; p.fov = 90.0;
        return p;
    }();
    return kDefault;
}

// ============================ CameraChannelJson ============================

std::string CameraChannelJson::Quote(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                sprintf_s(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

std::string CameraChannelJson::Session(const Scenario& scn, double rateHz, double aspect)
{
    char buf[256];
    std::string out = "{\"t\":\"session\",\"proto\":";
    sprintf_s(buf, sizeof(buf), "%d,\"scenario\":", kProto);
    out += buf;
    out += Quote(scn.name);
    sprintf_s(buf, sizeof(buf),
              ",\"durationSec\":%.3f,\"rateHz\":%.3f,\"aspect\":%.9f,"
              "\"origin\":[%.7f,%.7f,%.3f],\"frames\":%u}\n",
              scn.durationSeconds, rateHz, aspect,
              scn.originLatDeg, scn.originLonDeg, scn.originAltM,
              static_cast<unsigned>(scn.cameras.size()));
    out += buf;
    return out;
}

std::string CameraChannelJson::Crew(const Entity& e)
{
    std::string out = "{\"t\":\"crew\"";
    AppendId(out, "ent", e);
    char buf[48];
    sprintf_s(buf, sizeof(buf), ",\"count\":%d}\n", e.deckCrew);
    out += buf;
    return out;
}

std::string CameraChannelJson::Cut(const Scenario& scn, size_t frameIdx, double tSec)
{
    const CameraFrame& f = scn.cameras[frameIdx];
    const bool stationary = (f.kind == CameraKind::Stationary);
    const double blend = (f.transition == CameraTransition::HardCut) ? 0.0 : f.transitionSeconds;

    char buf[256];
    std::string out;
    sprintf_s(buf, sizeof(buf), "{\"t\":\"cut\",\"f\":%u,\"ts\":%.3f,\"kind\":\"%s\",\"label\":",
              static_cast<unsigned>(frameIdx + 1), tSec, stationary ? "stationary" : "entity");
    out += buf;
    out += Quote(f.label);
    out += ",\"presetType\":";
    out += Quote(f.presetType);
    out += ",\"presetAngle\":";
    out += Quote(f.presetAngle);
    sprintf_s(buf, sizeof(buf), ",\"transition\":\"%s\",\"blend\":%.3f,\"limitsEnabled\":%s",
              TransitionName(f.transition), blend, f.limitsEnabled ? "true" : "false");
    out += buf;

    if (!stationary)
        if (const Entity* src = FindEntity(scn, f.sourceEntityId)) AppendId(out, "src", *src);
    if (f.targetEntityId != 0)
        if (const Entity* tgt = FindEntity(scn, f.targetEntityId)) AppendId(out, "tgt", *tgt);

    out += "}\n";
    return out;
}

std::string CameraChannelJson::Pose(const Scenario& scn, size_t frameIdx, double tSec,
                                    const CameraSolvedPose& p)
{
    const CameraFrame& f = scn.cameras[frameIdx];
    char buf[320];
    std::string out;
    if (p.stationary)
    {
        sprintf_s(buf, sizeof(buf),
                  "{\"t\":\"pose\",\"f\":%u,\"ts\":%.3f,\"kind\":\"stationary\",\"frame\":\"world\","
                  "\"lla\":[%.7f,%.7f,%.3f],\"rot\":[%.3f,%.3f,%.3f],\"fov\":%.2f,\"pinned\":false}\n",
                  static_cast<unsigned>(frameIdx + 1), tSec,
                  p.latDeg, p.lonDeg, p.altM,
                  p.rot.pitch, p.rot.yaw, p.rot.roll, p.fovDeg);
        out += buf;
    }
    else
    {
        sprintf_s(buf, sizeof(buf),
                  "{\"t\":\"pose\",\"f\":%u,\"ts\":%.3f,\"kind\":\"entity\",\"frame\":\"body\","
                  "\"off\":[%.3f,%.3f,%.3f],\"rot\":[%.3f,%.3f,%.3f],\"fov\":%.2f,\"pinned\":false",
                  static_cast<unsigned>(frameIdx + 1), tSec,
                  p.off.forward, p.off.right, p.off.up,
                  p.rot.pitch, p.rot.yaw, p.rot.roll, p.fovDeg);
        out += buf;
        if (const Entity* src = FindEntity(scn, f.sourceEntityId)) AppendId(out, "src", *src);
        out += "}\n";
    }
    return out;
}

std::string CameraChannelJson::End(double tSec, const char* reason)
{
    char buf[128];
    sprintf_s(buf, sizeof(buf), "{\"t\":\"end\",\"ts\":%.3f,\"reason\":\"%s\"}\n", tSec, reason);
    return buf;
}

// ========================== CameraChannelProducer ==========================

CameraChannelProducer::~CameraChannelProducer()
{
    if (m_live) m_tcp.Close();
}

bool CameraChannelProducer::Start(const Scenario& scn, const std::vector<CameraPreset>& presets,
                                  const std::string& host, uint16_t port, int timeoutMs,
                                  double poseRateHz)
{
    m_presets  = &presets;
    m_current  = -1;
    m_cuts     = 0;
    m_poses    = 0;
    m_poseDt   = (poseRateHz > 0.1) ? 1.0 / poseRateHz : 1.0 / 30.0;
    m_nextPose = 0.0;
    m_warnedPreset.assign(scn.cameras.size(), false);

    // Client only, no re-dial: a visualizer that goes away mid-run takes the
    // camera with it, and a blocking reconnect inside the emission loop would
    // stall every entity's PDUs for the timeout.
    m_tcp.Configure(/*listen*/ false, host, port, /*listenPort*/ 0,
                    /*reconnect*/ false, timeoutMs);
    if (!m_tcp.Connect())
    {
        LogF("CameraChannel: visualizer not listening on %s:%u (%s) - running without a directed camera",
             host.c_str(), static_cast<unsigned>(port), m_tcp.LastError().c_str());
        return false;
    }
    m_live = true;
    LogF("CameraChannel: connected to %s:%u, %zu frame(s), poses at %.0f Hz",
         host.c_str(), static_cast<unsigned>(port), scn.cameras.size(), 1.0 / m_poseDt);

    SendLine(CameraChannelJson::Session(scn, 1.0 / m_poseDt, kAspect));

    // Deck crew ride the same connection: one line per entity that carries any.
    for (const Entity& e : scn.entities)
    {
        if (!m_live || !e.enabled || e.deckCrew <= 0) continue;
        SendLine(CameraChannelJson::Crew(e));
        LogF("CameraChannel: deck crew %d on entity %u:%u:%u", e.deckCrew,
             static_cast<unsigned>(e.siteId), static_cast<unsigned>(e.applicationId),
             static_cast<unsigned>(e.entityId));
    }
    return m_live;
}

void CameraChannelProducer::Rewind()
{
    m_current  = -1;
    m_nextPose = 0.0;
}

int CameraChannelProducer::ActiveFrame(const Scenario& scn, double tSec) const
{
    for (size_t i = 0; i < scn.cameras.size(); ++i)
    {
        const CameraFrame& f = scn.cameras[i];
        if (f.beginSecond <= tSec && tSec < f.endSecond) return static_cast<int>(i);
    }
    return -1;
}

bool CameraChannelProducer::SampleEntity(const Scenario& scn, uint16_t entityId, double tSec,
                                         CameraEntityState& out)
{
    const Entity* e = FindEntity(scn, entityId);
    if (!e || !e->enabled) return false;
    const SampledPose p = MotionSampler::SamplePose(*e, scn, tSec);
    out.ecefX = p.ecefX; out.ecefY = p.ecefY; out.ecefZ = p.ecefZ;
    out.headingDeg = p.headingDeg;
    return true;
}

void CameraChannelProducer::Tick(const Scenario& scn, double tSec)
{
    if (!m_live) return;

    // Shot boundary: the first frame whose window holds the clock is live.
    // Like the Python player, keep posing the last shot after its window ends
    // until another one begins, so the viewport is never left unparented.
    const int active = ActiveFrame(scn, tSec);
    if (active >= 0 && active != m_current)
    {
        m_current = active;
        if (SendLine(CameraChannelJson::Cut(scn, static_cast<size_t>(active), tSec)))
        {
            ++m_cuts;
            const CameraFrame& f = scn.cameras[static_cast<size_t>(active)];
            LogF("CameraChannel: t=%.2f cut %d '%s'", tSec, active + 1, f.label.c_str());
        }
        m_nextPose = tSec;     // a pose right behind the cut, not a frame later
    }
    if (m_current < 0 || tSec + 1e-9 < m_nextPose) return;

    // Catch up in whole steps so a late tick does not bunch poses.
    m_nextPose += m_poseDt;
    if (m_nextPose <= tSec) m_nextPose = tSec + m_poseDt;

    const size_t idx = static_cast<size_t>(m_current);
    const CameraFrame& f = scn.cameras[idx];

    CameraEntityState tgtState;
    const bool haveTarget =
        f.targetEntityId != 0 &&
        (f.kind == CameraKind::Stationary || f.targetEntityId != f.sourceEntityId) &&
        SampleEntity(scn, f.targetEntityId, tSec, tgtState);

    CameraSolvedPose pose;
    if (f.kind == CameraKind::Stationary)
    {
        pose = CameraChannelMath::SolveStationaryPose(f, scn, haveTarget ? &tgtState : nullptr, kAspect);
    }
    else
    {
        CameraEntityState srcState;
        if (!SampleEntity(scn, f.sourceEntityId, tSec, srcState)) return;   // no source, no pose

        const CameraPreset* preset = nullptr;
        if (m_presets)
        {
            for (const CameraPreset& p : *m_presets)
                if (p.type == f.presetType && p.angle == f.presetAngle) { preset = &p; break; }
            if (!preset)
                for (const CameraPreset& p : *m_presets)
                    if (_stricmp(p.type.c_str(),  f.presetType.c_str())  == 0 &&
                        _stricmp(p.angle.c_str(), f.presetAngle.c_str()) == 0) { preset = &p; break; }
        }
        if (!preset)
        {
            preset = &CameraChannelMath::DefaultPreset();
            if (idx < m_warnedPreset.size() && !m_warnedPreset[idx])
            {
                m_warnedPreset[idx] = true;
                LogF("CameraChannel: frame %u names preset %s/%s, which Cameras.ini does not have; using the default mount",
                     static_cast<unsigned>(idx + 1), f.presetType.c_str(), f.presetAngle.c_str());
            }
        }
        pose = CameraChannelMath::SolveEntityPose(f, *preset, srcState,
                                                  haveTarget ? &tgtState : nullptr, kAspect);
    }

    if (SendLine(CameraChannelJson::Pose(scn, idx, tSec, pose))) ++m_poses;
}

void CameraChannelProducer::End(double tSec, const char* reason)
{
    if (!m_live) return;
    SendLine(CameraChannelJson::End(tSec, reason));
    m_tcp.Close();
    m_live = false;
    LogF("CameraChannel: end (%s) after %llu cut(s), %llu pose(s)", reason,
         static_cast<unsigned long long>(m_cuts), static_cast<unsigned long long>(m_poses));
}

bool CameraChannelProducer::SendLine(const std::string& line)
{
    if (!m_live) return false;
    const int sent = m_tcp.Send(line.data(), line.size());
    if (sent <= 0)
    {
        // TcpSender has already logged the Winsock error. Stop here rather than
        // re-dialling from inside the emission loop.
        LogF("CameraChannel: link dropped after %llu cut(s), %llu pose(s); entities continue without a directed camera",
             static_cast<unsigned long long>(m_cuts), static_cast<unsigned long long>(m_poses));
        m_tcp.Close();
        m_live = false;
        return false;
    }
    return true;
}
