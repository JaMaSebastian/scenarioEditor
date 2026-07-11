#pragma once

#include "DisrecReader.h"
#include "DisrecWriter.h"
#include "Scenario.h"
#include "UdpSender.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

// Per spec §19.4 — custom Windows messages used by ScenarioWorker to
// reach the UI thread. wParam / lParam usage documented per message.
//
//   STATUS:   wParam = PlaybackState
//   PROGRESS: wParam = scenarioTimeMs, lParam = total PDUs sent
//   ERROR:    wParam = error code (per-source)
//   MARK_DIRTY: posted by child pages after a scenario-mutating action
//   REFRESH_UI: like MARK_DIRTY, but also reloads every page's controls from
//               the shared Scenario (e.g. the Preview tab dragged an entity's
//               start location and the Asset/Motion tabs must re-read it).
#define WM_APP_PLAYBACK_STATUS   (WM_APP + 100)
#define WM_APP_PLAYBACK_PROGRESS (WM_APP + 101)
#define WM_APP_PLAYBACK_ERROR    (WM_APP + 102)
#define WM_APP_MARK_DIRTY        (WM_APP + 110)
#define WM_APP_REFRESH_UI        (WM_APP + 111)

// Spec §19.5
enum class PlaybackState : int
{
    Idle      = 0,
    Running   = 1,
    Paused    = 2,
    Stopping  = 3,
    Stopped   = 4,
    Completed = 5,
    Error     = 6,
};

// Spec §18.0 — immutable snapshot handed to the worker at Start time.
// The snapshot's OutputConfig drives sink selection at Start.
struct RuntimeScenarioSnapshot
{
    Scenario scenario;       // deep copy of editable model
    bool     replay = false; // true → worker reads scenario.output.replayPath
                             //         and forwards records over UDP
};

class ScenarioWorker
{
public:
    using SnapshotPtr = std::shared_ptr<const RuntimeScenarioSnapshot>;

    ScenarioWorker() = default;
    ~ScenarioWorker();

    ScenarioWorker(const ScenarioWorker&) = delete;
    ScenarioWorker& operator=(const ScenarioWorker&) = delete;

    // Spawn the worker thread. uiHwnd receives WM_APP_PLAYBACK_* messages.
    // Returns false if a worker is already running (§19.2 single-worker rule).
    bool Start(HWND uiHwnd, SnapshotPtr snapshot);

    void Pause();        // sets pause flag; loop holds at next sleep tick
    void Resume();       // clears pause flag
    void RequestStop();  // sets stop flag; loop exits and posts Stopped status

    // Joins the worker thread if running. Safe to call from dtor.
    void Join();

    bool IsRunning() const { return m_thread.joinable(); }

private:
    void Run();
    void RunReplay();
    void RunGeneration();

    HWND                m_uiHwnd   = nullptr;
    SnapshotPtr         m_snapshot;
    std::atomic<bool>   m_stop{false};
    std::atomic<bool>   m_pause{false};
    std::thread         m_thread;
    UdpSender           m_udp;
    DisrecWriter        m_recorder;
};
