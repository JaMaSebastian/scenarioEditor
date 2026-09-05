//=============================================================================
//  TerrainServiceClient.h  —  talk to the containerized terrain service.
//-----------------------------------------------------------------------------
//  [Terrain] Mode=Service points the editor at terrainserver (default
//  127.0.0.1:8089), which both serves tiles AND builds them. Builds are
//  submitted over HTTP instead of run here through WSL:
//
//      POST /api/v1/jobs            -> 202 {"jobId":...}
//      GET  /api/v1/jobs/{id}?since=N  -> state + only the NEW log lines
//
//  Deliberately NOT built on ProcessSupervisor. That class models "a child
//  process I own" — it has a job object, a pipe, an exit code and no Stop().
//  Here there is no child at all: the work happens in a container this editor
//  did not start and must not kill. The only thing shared is the UX, and that
//  is preserved by posting the same shape of messages so the status line and
//  failure modal behave identically.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-20
//=============================================================================
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Posted to the page as a job runs.
//   WM_APP_TERRAIN_JOB_LINE : lParam = heap char*, handler MUST delete[] it.
//   WM_APP_TERRAIN_JOB_DONE : wParam = 0 success / 1 failure,
//                             lParam = heap char* detail, handler MUST delete[].
// +130/131 is clear of ScenarioWorker (+100..111) and ProcessSupervisor
// (+120/121); collisions here are silent and miserable to debug.
#define WM_APP_TERRAIN_JOB_LINE (WM_APP + 130)
#define WM_APP_TERRAIN_JOB_DONE (WM_APP + 131)

class TerrainServiceClient
{
public:
    ~TerrainServiceClient();

    // One-shot GET, used for the health probe. Returns false on transport
    // failure; `status` carries the HTTP code when the request completed.
    static bool Get(const std::string& host, unsigned short port,
                    const std::string& path,
                    int& status, std::string& body);

    static bool Post(const std::string& host, unsigned short port,
                     const std::string& path, const std::string& requestBody,
                     int& status, std::string& body);

    // Submit `jsonBody` and poll it to completion on a worker thread, posting
    // WM_APP_TERRAIN_JOB_LINE / _DONE to `hwnd`. Returns false if a job is
    // already in flight on this client.
    bool StartJob(HWND hwnd, const std::string& host, unsigned short port,
                  const std::string& jsonBody);

    bool IsRunning() const { return m_running.load(); }

    // Ask the server to cancel; the poll loop ends when the state goes
    // terminal. Does not block.
    void Cancel();

    const std::string& JobId() const { return m_jobId; }

    // --- tiny response readers (the service's shapes are fixed and flat) ---
    static bool FindString(const std::string& json, const std::string& key, std::string& out);
    static bool FindNumber(const std::string& json, const std::string& key, double& out);
    // Extracts the "log":[...] array, unescaping each element.
    static void FindLogLines(const std::string& json, std::vector<std::string>& out);

private:
    void Run(HWND hwnd, std::string host, unsigned short port, std::string jsonBody);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::thread       m_thread;
    std::string       m_jobId;
};
