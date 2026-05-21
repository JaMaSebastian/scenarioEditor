#include "ScenarioWorker.h"
#include "CoordTransforms.h"
#include "MotionSampler.h"
#include "PduBuilder.h"
#include "../log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
    // Default fallback if the snapshot has no usable updateRateHz set.
    constexpr double kFallbackHz = 5.0;

    // Worker sleeps in short chunks so RequestStop is responsive even at
    // low PDU rates (e.g. 1 Hz would otherwise mean a 1 s stop latency).
    constexpr auto kStopPollInterval = std::chrono::milliseconds(20);

    void PostStatus(HWND hwnd, PlaybackState state)
    {
        if (hwnd) ::PostMessageW(hwnd, WM_APP_PLAYBACK_STATUS,
                                 static_cast<WPARAM>(state), 0);
    }
}

ScenarioWorker::~ScenarioWorker()
{
    RequestStop();
    Join();
}

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

void ScenarioWorker::Pause()
{
    if (!m_thread.joinable()) return;
    m_pause.store(true);
    PostStatus(m_uiHwnd, PlaybackState::Paused);
}

void ScenarioWorker::Resume()
{
    if (!m_thread.joinable()) return;
    m_pause.store(false);
    PostStatus(m_uiHwnd, PlaybackState::Running);
}

void ScenarioWorker::RequestStop()
{
    m_stop.store(true);
}

void ScenarioWorker::Join()
{
    if (m_thread.joinable())
        m_thread.join();
}

void ScenarioWorker::Run()
{
    PostStatus(m_uiHwnd, PlaybackState::Running);
    if (m_snapshot->replay) {
        RunReplay();
    } else {
        RunGeneration();
    }
}

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

    // Replay always sends over UDP; multicast/unicast picked from OutputConfig.
    std::string destHost; uint16_t destPort = 0;
    switch (out.mode) {
        case OutputMode::UdpMulticast:
            destHost = out.multicastGroup; destPort = out.multicastPort;
            m_udp.SetMulticastOptions(out.multicastTtl, out.multicastInterface,
                                       out.multicastLoopback);
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

            const int sent = m_udp.Send(destHost, destPort,
                                         bytes.data(), bytes.size());
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

    PostStatus(m_uiHwnd, PlaybackState::Stopped);
    sprintf_s(szError, sizeof(szError),
              "ScenarioWorker replay finished after %llu PDUs at %.2fx",
              static_cast<unsigned long long>(pduCount), speed);
    LOG(szError);
}

void ScenarioWorker::RunGeneration()
{
    const Scenario& scn = m_snapshot->scenario;
    const OutputConfig& out = scn.output;
    const double speed = (out.playbackSpeed > 1e-6) ? out.playbackSpeed : 1.0;

    // Resolve dest endpoint + multicast options from OutputConfig.
    std::string destHost; uint16_t destPort = 0;
    bool sinkUdp = true;
    switch (out.mode) {
        case OutputMode::UdpUnicast:
            destHost = out.unicastIp;   destPort = out.unicastPort; break;
        case OutputMode::UdpMulticast:
            destHost = out.multicastGroup; destPort = out.multicastPort;
            m_udp.SetMulticastOptions(out.multicastTtl, out.multicastInterface,
                                       out.multicastLoopback);
            break;
        case OutputMode::Tcp:
            // Deferred per spec §23.2. Fall back to no-op for now so the
            // worker doesn't crash, but report Error so the user sees why.
            sprintf_s(szError, sizeof(szError),
                      "ScenarioWorker: TCP output not yet implemented (§23.2 deferred)");
            LOG(szError);
            if (m_uiHwnd) ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_ERROR, 2, 0);
            PostStatus(m_uiHwnd, PlaybackState::Stopped);
            return;
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

    // Tick rate = max of per-entity update rates (default 5 Hz). Each tick
    // emits one PDU per enabled entity, so the total wire rate scales with
    // entity count.
    double rateHz = scn.defaultUpdateRateHz > 0.0 ? scn.defaultUpdateRateHz : kFallbackHz;
    for (const Entity& e : scn.entities) {
        if (e.enabled && e.updateRateHz > rateHz) rateHz = e.updateRateHz;
    }
    if (rateHz < 0.1)  rateHz = 0.1;
    if (rateHz > 200)  rateHz = 200;

    const auto period = std::chrono::microseconds(
        static_cast<long long>(1'000'000.0 / rateHz));

    const auto scenarioStart = std::chrono::steady_clock::now();
    uint64_t pduCount = 0;
    auto nextSend = scenarioStart;

    while (!m_stop.load(std::memory_order_relaxed))
    {
        const auto now = std::chrono::steady_clock::now();

        if (!m_pause.load(std::memory_order_relaxed) && now >= nextSend)
        {
            // Speed multiplier scales the scenario clock relative to wall
            // time. 2.0x means the motion sampler is asked for "twice as
            // far into the scenario" at each wall-clock tick.
            const double tSec = std::chrono::duration<double>(now - scenarioStart).count() * speed;

            // Fan out one PDU per enabled entity per tick.
            bool sendFailure = false;
            for (const Entity& entStatic : scn.entities)
            {
                if (!entStatic.enabled) continue;

                const SampledPose pose = MotionSampler::SamplePose(entStatic, scn, tSec);

                Entity sampled = entStatic;
                sampled.ecefX = pose.ecefX;
                sampled.ecefY = pose.ecefY;
                sampled.ecefZ = pose.ecefZ;
                double psiRad = 0.0, thetaRad = 0.0, phiRad = 0.0;
                CoordTransforms::LocalHprToEcefPsiThetaPhi(
                    pose.headingDeg, pose.pitchDeg, pose.rollDeg,
                    scn.originLatDeg, scn.originLonDeg,
                    psiRad, thetaRad, phiRad);
                sampled.psi   = static_cast<float>(psiRad);
                sampled.theta = static_cast<float>(thetaRad);
                sampled.phi   = static_cast<float>(phiRad);

                DIS::EntityStatePdu pdu = PduBuilder::BuildEntityStatePdu(scn, sampled);
                std::vector<unsigned char> bytes;
                PduBuilder::Serialize(pdu, bytes);

                int sent = static_cast<int>(bytes.size());
                if (sinkUdp)
                {
                    sent = m_udp.Send(destHost, destPort,
                                       bytes.data(), bytes.size());
                }
                else if (out.mode == OutputMode::FileRecording && m_recorder.IsOpen())
                {
                    const uint64_t tsUs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            now - scenarioStart).count());
                    if (!m_recorder.WriteRecord(tsUs, bytes.data(), bytes.size()))
                        sent = 0;
                }
                // PreviewOnly: no sink; counts as "sent" so we still tick.

                if (sent > 0)
                {
                    ++pduCount;
                }
                else
                {
                    sprintf_s(szError, sizeof(szError),
                              "ScenarioWorker: send failed after %llu PDUs",
                              static_cast<unsigned long long>(pduCount));
                    LOG(szError);
                    if (m_uiHwnd)
                        ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_ERROR,
                                       1, 0);
                    sendFailure = true;
                    break;
                }
            }
            if (sendFailure) break;

            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - scenarioStart).count();
            if (m_uiHwnd)
                ::PostMessageW(m_uiHwnd, WM_APP_PLAYBACK_PROGRESS,
                               static_cast<WPARAM>(elapsedMs),
                               static_cast<LPARAM>(pduCount));

            nextSend += period;
            if (now - nextSend > period * 4) nextSend = now + period;
        }

        std::this_thread::sleep_for(kStopPollInterval);
    }

    m_recorder.Close();
    PostStatus(m_uiHwnd, PlaybackState::Stopped);

    sprintf_s(szError, sizeof(szError),
              "ScenarioWorker finished after %llu PDUs at ~%.2f Hz",
              static_cast<unsigned long long>(pduCount), rateHz);
    LOG(szError);
}
