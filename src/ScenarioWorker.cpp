//=============================================================================
//  ScenarioWorker.cpp
//-----------------------------------------------------------------------------
//  Implements the playback thread. Precomputes per-entity motion tracks (an
//  arc-length-equidistant ring for single-ellipse orbits, time-sampling
//  otherwise), then either replays a recorded stream against the wall clock or
//  generates and emits Entity State PDUs at each track's exact interval, with
//  pause/stop handling and OS timer-resolution control for accurate timing.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
// ScenarioWorker.h pulls in <windows.h>; suppress its min/max macros so they
// don't clobber std::min/std::max (this TU doesn't include pch.h/framework.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ScenarioWorker.h"
#include "CoordTransforms.h"
#include "MotionSampler.h"
#include "PduBuilder.h"
#include "../log.h"

#include <timeapi.h>      // timeBeginPeriod / timeEndPeriod (winmm)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    // Default fallback if the snapshot has no usable updateRateHz set.
    constexpr double kFallbackHz = 5.0;

    // Worker sleeps in short chunks so RequestStop is responsive even at
    // low PDU rates (e.g. 1 Hz would otherwise mean a 1 s stop latency).
    constexpr auto kStopPollInterval = std::chrono::milliseconds(20);

    // Max single sleep between emission checks while running. Small enough
    // that 20–50 Hz intervals stay accurate (with timeBeginPeriod(1)) and
    // stop/pause stay responsive.
    constexpr auto kWakeCap = std::chrono::milliseconds(8);

    void PostStatus(HWND hwnd, PlaybackState state)
    {
        if (hwnd) ::PostMessageW(hwnd, WM_APP_PLAYBACK_STATUS,
                                 static_cast<WPARAM>(state), 0);
    }

    // One precomputed PDU sample (ECEF pose + ECEF velocity).
    struct Waypoint
    {
        double ecefX, ecefY, ecefZ;
        float  psi, theta, phi;
        double velX, velY, velZ;
    };

    // Per-entity playback plan. Ellipse orbits become a closed ring of
    // arc-length-equidistant waypoints emitted at exactly dt; everything else
    // is time-sampled at dt with a finite-difference velocity.
    struct EntityTrack
    {
        const Entity* ent = nullptr;
        double dt = 0.2;
        double windowStart = 0.0, windowEnd = 0.0;
        bool   isRing = false;
        std::vector<Waypoint> ring;
        double   nextT = 0.0;     // next emission sim-time (windowStart + k*dt)
        uint64_t k = 0;
    };

    // Build the per-entity tracks (worker thread, once at start; immutable after).
    void BuildTracks(const Scenario& scn, std::vector<EntityTrack>& tracks)
    {
        for (const Entity& e : scn.entities)
        {
            if (!e.enabled) continue;

            EntityTrack tr;
            tr.ent = &e;

            double hz = (e.updateRateHz > 0.0) ? e.updateRateHz
                      : (scn.defaultUpdateRateHz > 0.0 ? scn.defaultUpdateRateHz : kFallbackHz);
            if (hz < 0.1)   hz = 0.1;
            if (hz > 200.0) hz = 200.0;
            tr.dt = 1.0 / hz;

            tr.windowStart = std::max(0.0, e.beginSecond);
            tr.windowEnd   = (scn.durationSeconds > 0.0)
                           ? std::min(scn.durationSeconds, e.endSecond) : e.endSecond;

            // Ring-eligible iff exactly one enabled segment, an Ellipse, whose
            // [startSecond,endSecond] covers the whole emission window.
            const MotionSegment* onlyEll = nullptr;
            int enabledCount = 0;
            for (const MotionSegment& s : e.motionSegments)
            {
                if (!s.enabled) continue;
                ++enabledCount;
                onlyEll = (s.type == MotionType::Ellipse) ? &s : nullptr;
            }
            const bool eligible =
                (enabledCount == 1 && onlyEll != nullptr &&
                 onlyEll->followEntityId < 0 &&   // Entity Ellipse: center moves;
                                                  // needs per-tick SamplePose
                 onlyEll->startSecond <= tr.windowStart + 1e-9 &&
                 onlyEll->endSecond   >= tr.windowEnd   - 1e-9);

            if (eligible)
            {
                MotionSampler::EllipseFrame f =
                    MotionSampler::BuildEllipseFrame(*onlyEll, &scn, /*withArcTable*/ true);
                const double speed = (onlyEll->speedMps > 0.0) ? onlyEll->speedMps : 100.0;

                if (f.valid && f.perimeter > 1e-3 && speed > 0.0)
                {
                    long long N = std::llround(f.perimeter / std::max(speed * tr.dt, 1e-6));
                    if (N < 1) N = 1;
                    const double effSpacing = f.perimeter / static_cast<double>(N);
                    const double effSpeed   = effSpacing / tr.dt;

                    // ENU->ECEF rotation (no translation) at the ellipse frame,
                    // obtained by differencing two transformed points.
                    double o0x, o0y, o0z;
                    CoordTransforms::LocalEnuToEcefDeg(0.0, 0.0, 0.0,
                        f.midLat, f.midLon, f.midAlt, o0x, o0y, o0z);

                    tr.ring.reserve(static_cast<size_t>(N));
                    for (long long i = 0; i < N; ++i)
                    {
                        const double theta = MotionSampler::InvertArcLength(f, i * effSpacing);
                        const MotionSampler::EllipsePoint p = MotionSampler::EvalEllipseAtTheta(f, theta);

                        Waypoint w;
                        w.ecefX = p.ecefX; w.ecefY = p.ecefY; w.ecefZ = p.ecefZ;
                        // This ring bypasses SamplePose, so the surface clamp has to be
                        // applied here as well or a ship orbiting on a single Ellipse -
                        // the exact shape that qualifies for this fast path - keeps the
                        // ellipse frame altitude and flies above the water.
                        MotionSampler::ClampSurfaceToSeaLevel(e, w.ecefX, w.ecefY, w.ecefZ);

                        double tx, ty, tz;
                        CoordTransforms::LocalEnuToEcefDeg(p.tEast, p.tNorth, 0.0,
                            f.midLat, f.midLon, f.midAlt, tx, ty, tz);
                        w.velX = (tx - o0x) * effSpeed;
                        w.velY = (ty - o0y) * effSpeed;
                        w.velZ = (tz - o0z) * effSpeed;

                        double psi, th, ph;
                        CoordTransforms::LocalHprToEcefPsiThetaPhi(
                            p.headingDeg, onlyEll->startPitchDeg, onlyEll->startRollDeg,
                            scn.originLatDeg, scn.originLonDeg, psi, th, ph);
                        w.psi = static_cast<float>(psi);
                        w.theta = static_cast<float>(th);
                        w.phi = static_cast<float>(ph);

                        tr.ring.push_back(w);
                    }
                    tr.isRing = true;
                }
            }

            tr.nextT = tr.windowStart;
            tr.k = 0;
            tracks.push_back(std::move(tr));
        }
    }
}

//
// ~ScenarioWorker — requests a stop and joins the thread so nothing outlives it.
//
ScenarioWorker::~ScenarioWorker()
{
    RequestStop();
    Join();
}

//
// Start — validates state/snapshot, then spawns the worker thread running Run().
//   Returns false if a worker is already running or the snapshot is null.
//
bool ScenarioWorker::Start(HWND uiHwnd, SnapshotPtr snapshot)
{
    if (m_thread.joinable())
    {
        LOG("ScenarioWorker::Start refused — worker already running (single-worker rule §19.2)");
        return false;
    }
    if (!snapshot)
    {
        LOG("ScenarioWorker::Start refused — null snapshot");
        return false;
    }
    m_uiHwnd   = uiHwnd;
    m_snapshot = std::move(snapshot);
    m_stop.store(false);
    m_pause.store(false);
    m_thread = std::thread([this]{ Run(); });
    return true;
}

//
// Pause — sets the pause flag and posts a Paused status to the UI.
//
void ScenarioWorker::Pause()
{
    if (!m_thread.joinable()) return;
    m_pause.store(true);
    PostStatus(m_uiHwnd, PlaybackState::Paused);
}

//
// Resume — clears the pause flag and posts a Running status to the UI.
//
void ScenarioWorker::Resume()
{
    if (!m_thread.joinable()) return;
    m_pause.store(false);
    PostStatus(m_uiHwnd, PlaybackState::Running);
}

//
// RequestStop — sets the stop flag; the worker loop exits at its next check.
//
void ScenarioWorker::RequestStop()
{
    m_stop.store(true);
}

//
// Join — blocks until the worker thread finishes (no-op if not running).
//
void ScenarioWorker::Join()
{
    if (m_thread.joinable())
        m_thread.join();
}

//
// Run — thread entry point; posts Running, then dispatches to replay or
//   generation based on the snapshot's replay flag.
//
void ScenarioWorker::Run()
{
    PostStatus(m_uiHwnd, PlaybackState::Running);
    if (m_snapshot->replay) {
        RunReplay();
    } else {
        RunGeneration();
    }
}

//
// RunReplay — opens the recorded stream and forwards each record over UDP,
//   scheduling by record timestamp scaled by playbackSpeed, honoring
//   pause/stop and optional looping; posts progress and a final Stopped status.
//
void ScenarioWorker::RunReplay()
{
    const Scenario& scn = m_snapshot->scenario;
    const OutputConfig& out = scn.output;

    DisrecReader reader;
    if (out.replayPath.empty() || !reader.Open(out.replayPath))
    {
        sprintf_s(szError, sizeof(szError),
                  "ScenarioWorker: cannot open replay '%s'", out.replayPath.c_str());
        LOG(szError);
        if (m_uiHwnd) ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_ERROR, 4, 0);
        PostStatus(m_uiHwnd, PlaybackState::Stopped);
        return;
    }

    // Replay goes out over the network; unicast/multicast/TCP per OutputConfig.
    // FileRecording and PreviewOnly have no network endpoint of their own, so
    // they fall back to unicast rather than silently sending nowhere.
    std::string destHost; uint16_t destPort = 0;
    bool sinkTcp = false;
    switch (out.mode) {
        case OutputMode::UdpMulticast:
            destHost = out.multicastGroup; destPort = out.multicastPort;
            m_udp.SetMulticastOptions(out.multicastTtl, out.multicastInterface,
                                       out.multicastLoopback);
            break;
        case OutputMode::Tcp:
            sinkTcp = true;
            m_tcp.Configure(out.tcpListen, out.tcpRemoteHost, out.tcpRemotePort,
                            out.tcpListenPort, out.tcpReconnect, out.tcpTimeoutMs);
            if (!m_tcp.Connect())
            {
                sprintf_s(szError, sizeof(szError),
                          "ScenarioWorker: replay TCP connect failed (%s): %s",
                          m_tcp.DescribeEndpoint().c_str(),
                          m_tcp.LastError().c_str());
                LOG(szError);
                if (m_uiHwnd) ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_ERROR, 2, 0);
                PostStatus(m_uiHwnd, PlaybackState::Stopped);
                return;
            }
            break;
        case OutputMode::UdpUnicast:
        default:
            destHost = out.unicastIp; destPort = out.unicastPort; break;
    }

    const double speed = (out.playbackSpeed > 1e-6) ? out.playbackSpeed : 1.0;

    uint64_t pduCount = 0;
    do {
        const auto wallStart = std::chrono::steady_clock::now();
        reader.Rewind();

        uint64_t tsUs = 0;
        std::vector<unsigned char> bytes;
        while (!m_stop.load(std::memory_order_relaxed) &&
               reader.NextRecord(tsUs, bytes))
        {
            // Honor pause.
            while (m_pause.load(std::memory_order_relaxed) &&
                   !m_stop.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(kStopPollInterval);
            if (m_stop.load(std::memory_order_relaxed)) break;

            // Schedule against the wall clock, scaled by speed.
            const auto target = wallStart + std::chrono::microseconds(
                static_cast<long long>(tsUs / speed));
            const auto now = std::chrono::steady_clock::now();
            if (target > now)
                std::this_thread::sleep_for(target - now);

            const int sent = sinkTcp
                ? m_tcp.Send(bytes.data(), bytes.size())
                : m_udp.Send(destHost, destPort, bytes.data(), bytes.size());
            if (sent > 0) {
                ++pduCount;
                if (m_uiHwnd) {
                    const auto elapsedMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - wallStart).count();
                    ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_PROGRESS,
                                   static_cast<WPARAM>(elapsedMs),
                                   static_cast<LPARAM>(pduCount));
                }
            }
        }
    } while (out.loopEnabled && !m_stop.load(std::memory_order_relaxed));

    if (sinkTcp) m_tcp.Close();
    PostStatus(m_uiHwnd, PlaybackState::Stopped);
    sprintf_s(szError, sizeof(szError),
              "ScenarioWorker replay finished after %llu PDUs at %.2fx",
              static_cast<unsigned long long>(pduCount), speed);
    LOG(szError);
}

//
// RunGeneration — resolves the output sink (UDP uni/multicast, TCP, file recording,
//   or preview), builds per-entity tracks, then runs the emission loop: samples
//   each due track, builds/serializes an Entity State PDU, sends or records it,
//   and handles pause, looping, duration limits, and timing. The nested
//   emitTrack lambda emits one entity's PDU (ring lookup or time-sample).
//
void ScenarioWorker::RunGeneration()
{
    const Scenario& scn = m_snapshot->scenario;
    const OutputConfig& out = scn.output;
    const double speed = (out.playbackSpeed > 1e-6) ? out.playbackSpeed : 1.0;

    // Resolve dest endpoint + multicast options from OutputConfig.
    std::string destHost; uint16_t destPort = 0;
    bool sinkUdp = true;
    bool sinkTcp = false;
    switch (out.mode) {
        case OutputMode::UdpUnicast:
            destHost = out.unicastIp;   destPort = out.unicastPort; break;
        case OutputMode::UdpMulticast:
            destHost = out.multicastGroup; destPort = out.multicastPort;
            m_udp.SetMulticastOptions(out.multicastTtl, out.multicastInterface,
                                       out.multicastLoopback);
            break;
        case OutputMode::Tcp:
            sinkUdp = false;
            sinkTcp = true;
            destHost.clear();
            m_tcp.Configure(out.tcpListen, out.tcpRemoteHost, out.tcpRemotePort,
                            out.tcpListenPort, out.tcpReconnect, out.tcpTimeoutMs);
            // Fail-fast per §17.7: a socket that cannot be established is
            // unrecoverable, so surface it now rather than after N failed PDUs.
            if (!m_tcp.Connect())
            {
                sprintf_s(szError, sizeof(szError),
                          "ScenarioWorker: TCP connect failed (%s): %s",
                          m_tcp.DescribeEndpoint().c_str(),
                          m_tcp.LastError().c_str());
                LOG(szError);
                if (m_uiHwnd) ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_ERROR, 2, 0);
                PostStatus(m_uiHwnd, PlaybackState::Stopped);
                return;
            }
            break;
        case OutputMode::FileRecording:
            sinkUdp = false;
            destHost.clear();
            if (out.recordingPath.empty() ||
                !m_recorder.Open(out.recordingPath))
            {
                sprintf_s(szError, sizeof(szError),
                          "ScenarioWorker: cannot open recording '%s'",
                          out.recordingPath.c_str());
                LOG(szError);
                if (m_uiHwnd) ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_ERROR, 3, 0);
                PostStatus(m_uiHwnd, PlaybackState::Stopped);
                return;
            }
            break;
        case OutputMode::PreviewOnly:
            sinkUdp = false;
            destHost.clear();
            break;
    }

    // Precompute each entity's playback track (arc-length-equidistant ring for
    // single-ellipse orbits; time-sampled otherwise) so emission is just a
    // table lookup at an exact per-entity interval.
    std::vector<EntityTrack> tracks;
    BuildTracks(scn, tracks);

    // Emit one entity's PDU (ring lookup or time-sample). Returns false on a
    // send failure. `now` is the wall clock used for recording timestamps.
    auto emitTrack = [&](EntityTrack& tr,
                         std::chrono::steady_clock::time_point now,
                         std::chrono::steady_clock::time_point scenarioStart,
                         uint64_t& pduCount) -> bool
    {
        const Entity& e = *tr.ent;
        Entity sampled = e;
        double vx = 0.0, vy = 0.0, vz = 0.0;

        if (tr.isRing && !tr.ring.empty())
        {
            const Waypoint& w = tr.ring[tr.k % tr.ring.size()];   // k % N closes the loop
            sampled.ecefX = w.ecefX; sampled.ecefY = w.ecefY; sampled.ecefZ = w.ecefZ;
            sampled.psi = w.psi; sampled.theta = w.theta; sampled.phi = w.phi;
            vx = w.velX; vy = w.velY; vz = w.velZ;
        }
        else
        {
            const double t = tr.nextT;
            const SampledPose p  = MotionSampler::SamplePose(e, scn, t);
            const SampledPose pa = MotionSampler::SamplePose(e, scn, t + tr.dt);
            const SampledPose pb = MotionSampler::SamplePose(e, scn, t - tr.dt);
            sampled.ecefX = p.ecefX; sampled.ecefY = p.ecefY; sampled.ecefZ = p.ecefZ;
            double psi = 0.0, th = 0.0, ph = 0.0;
            CoordTransforms::LocalHprToEcefPsiThetaPhi(
                p.headingDeg, p.pitchDeg, p.rollDeg,
                scn.originLatDeg, scn.originLonDeg, psi, th, ph);
            sampled.psi = static_cast<float>(psi);
            sampled.theta = static_cast<float>(th);
            sampled.phi = static_cast<float>(ph);
            const double inv = (tr.dt > 1e-9) ? 1.0 / (2.0 * tr.dt) : 0.0;
            vx = (pa.ecefX - pb.ecefX) * inv;
            vy = (pa.ecefY - pb.ecefY) * inv;
            vz = (pa.ecefZ - pb.ecefZ) * inv;
        }

        DIS::EntityStatePdu pdu = PduBuilder::BuildEntityStatePdu(scn, sampled, vx, vy, vz);
        std::vector<unsigned char> bytes;
        PduBuilder::Serialize(pdu, bytes);

        int sent = static_cast<int>(bytes.size());
        if (sinkUdp)
        {
            sent = m_udp.Send(destHost, destPort, bytes.data(), bytes.size());
        }
        else if (sinkTcp)
        {
            sent = m_tcp.Send(bytes.data(), bytes.size());
        }
        else if (out.mode == OutputMode::FileRecording && m_recorder.IsOpen())
        {
            const uint64_t tsUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - scenarioStart).count());
            if (!m_recorder.WriteRecord(tsUs, bytes.data(), bytes.size()))
                sent = 0;
        }
        // PreviewOnly: no sink; counts as "sent".

        if (sent > 0) { ++pduCount; return true; }

        sprintf_s(szError, sizeof(szError),
                  "ScenarioWorker: send failed after %llu PDUs%s%s",
                  static_cast<unsigned long long>(pduCount),
                  sinkTcp ? " - " : "",
                  sinkTcp ? m_tcp.LastError().c_str() : "");
        LOG(szError);
        if (m_uiHwnd) ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_ERROR, 1, 0);
        return false;
    };

    // Raise the OS timer resolution so sub-20 ms sleeps are accurate (needed
    // for precise 20–50 Hz intervals). Paired with timeEndPeriod below.
    timeBeginPeriod(1);

    auto scenarioStart = std::chrono::steady_clock::now();
    uint64_t pduCount = 0;
    bool wasPaused = false;
    bool sendFailure = false;
    std::chrono::steady_clock::time_point pauseStart;

    while (!m_stop.load(std::memory_order_relaxed))
    {
        auto now = std::chrono::steady_clock::now();

        // Pause freezes sim time: on resume, shift scenarioStart by the paused
        // wall duration so no emissions are skipped or bunched.
        if (m_pause.load(std::memory_order_relaxed))
        {
            if (!wasPaused) { pauseStart = now; wasPaused = true; }
            std::this_thread::sleep_for(kStopPollInterval);
            continue;
        }
        if (wasPaused)
        {
            scenarioStart += (now - pauseStart);
            wasPaused = false;
            now = std::chrono::steady_clock::now();
        }

        const double tNow =
            std::chrono::duration<double>(now - scenarioStart).count() * speed;

        // Scenario duration reached → loop (reuse rings) or stop.
        if (scn.durationSeconds > 0.0 && tNow >= scn.durationSeconds)
        {
            if (out.loopEnabled)
            {
                scenarioStart = now;
                for (auto& tr : tracks) { tr.k = 0; tr.nextT = tr.windowStart; }
                continue;
            }
            break;
        }

        // Emit every track that is due (sim time has reached its nextT); each
        // advances by an exact dt so there is no accumulated drift.
        bool emitted = false;
        bool anyPending = false;
        double minPending = std::numeric_limits<double>::infinity();

        for (auto& tr : tracks)
        {
            if (tr.windowEnd <= tr.windowStart) continue;   // empty window
            while (tr.nextT < tr.windowEnd && tNow + 1e-9 >= tr.nextT)
            {
                if (!emitTrack(tr, now, scenarioStart, pduCount)) { sendFailure = true; break; }
                emitted = true;
                tr.k += 1;
                tr.nextT = tr.windowStart + static_cast<double>(tr.k) * tr.dt;
            }
            if (sendFailure) break;
            if (tr.nextT < tr.windowEnd)
            {
                anyPending = true;
                minPending = std::min(minPending, tr.nextT);
            }
        }
        if (sendFailure) break;

        if (emitted && m_uiHwnd)
        {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - scenarioStart).count();
            ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_PROGRESS,
                           static_cast<WPARAM>(elapsedMs),
                           static_cast<LPARAM>(pduCount));
        }

        // All tracks have finished their windows.
        if (!anyPending)
        {
            if (out.loopEnabled)
            {
                scenarioStart = now;
                for (auto& tr : tracks) { tr.k = 0; tr.nextT = tr.windowStart; }
                continue;
            }
            if (scn.durationSeconds <= 0.0) break;   // nothing left and no duration → done
            // else: idle until the scenario duration elapses, then stop above.
        }

        // Sleep until the earliest pending wall target, capped for responsiveness.
        auto sleepDur = kWakeCap;
        if (anyPending)
        {
            const auto target = scenarioStart +
                std::chrono::microseconds(static_cast<long long>(minPending / speed * 1e6));
            auto d = target - std::chrono::steady_clock::now();
            if (d < std::chrono::steady_clock::duration::zero())
                d = std::chrono::steady_clock::duration::zero();
            const auto dms = std::chrono::duration_cast<std::chrono::milliseconds>(d);
            if (dms < sleepDur) sleepDur = dms;
        }
        std::this_thread::sleep_for(sleepDur);
    }

    timeEndPeriod(1);
    m_recorder.Close();
    // Close the TCP link so the receiver sees a clean EOF rather than a
    // half-open connection lingering until the next run.
    if (sinkTcp) m_tcp.Close();
    PostStatus(m_uiHwnd, PlaybackState::Stopped);

    sprintf_s(szError, sizeof(szError),
              "ScenarioWorker finished after %llu PDUs (%zu tracks)",
              static_cast<unsigned long long>(pduCount), tracks.size());
    LOG(szError);
}
