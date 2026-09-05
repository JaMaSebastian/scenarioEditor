//=============================================================================
//  ProcessSupervisor.h
//-----------------------------------------------------------------------------
//  Declares ProcessSupervisor, the replacement for the Preview tab's
//  fire-and-forget ::ShellExecute calls.
//
//  ShellExecute cannot return a process handle, so the app previously had no
//  way to read a child's output, learn its exit code, or stop it. Every failure
//  landed in a console window the user had to find, and every child outlived
//  the editor. This class fixes all three:
//
//    * stdout/stderr are piped back line by line (WM_APP_PROC_OUTPUT + LOG)
//    * the real exit code is reported (WM_APP_PROC_EXIT); non-zero is a failure
//    * children are put in a Job Object with KILL_ON_JOB_CLOSE, so they die
//      with the editor even if it crashes -- a guarantee TerminateProcess
//      alone cannot make
//
//  Threading mirrors ScenarioWorker: one std::thread per run, results delivered
//  to the UI with PostMessageW. Nothing blocks the message pump.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-07
//=============================================================================
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>

//-----------------------------------------------------------------------------
// UI messages. Kept clear of ScenarioWorker's WM_APP+100..111 block and
// MapTileService's WM_APP+7.
//
//   WM_APP_PROC_OUTPUT : wParam = 0
//                        lParam = const char* to a heap-allocated NUL-terminated
//                                 line. THE HANDLER MUST delete[] IT.
//   WM_APP_PROC_EXIT   : wParam = exit code (0 = success)
//                        lParam = LOWORD: 1 if cancelled, else 0
//                                 HIWORD: the supervisor's Tag()
//
//  The tag matters because one window can own several supervisors (a build and
//  the terrain server), and both post to the same handler -- without it a
//  server exiting would be mistaken for the build finishing.
//-----------------------------------------------------------------------------
#define WM_APP_PROC_OUTPUT (WM_APP + 120)
#define WM_APP_PROC_EXIT   (WM_APP + 121)

class ProcessSupervisor
{
public:
    ProcessSupervisor();
    ~ProcessSupervisor();

    ProcessSupervisor(const ProcessSupervisor&) = delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

    // Identifies this supervisor in WM_APP_PROC_* messages. Set once, before
    // the first Start; 0 if never set.
    void SetTag(WORD tag) { m_tag = tag; }
    WORD Tag() const { return m_tag; }

    // Runs commandLine (a full command line; argv[0] must be quoted if it
    // contains spaces) with workingDir as cwd (may be empty). Output and the
    // exit code are posted to uiHwnd. Returns false immediately if a run is
    // already in flight or the process could not be created -- in which case
    // LastError() explains why and no WM_APP_PROC_EXIT is posted.
    //
    // killWithApp: true for one-shot builds, which must never outlive the
    // editor. false for the terrain server, which is deliberately allowed to
    // keep running after the editor closes so a live DISBrowser keeps its
    // terrain (see CPreviewPage::OnDestroy).
    bool Start(HWND uiHwnd,
               const std::wstring& commandLine,
               const std::wstring& workingDir,
               bool killWithApp);

    // True between a successful Start and the WM_APP_PROC_EXIT post.
    bool IsRunning() const
    { return m_run && m_run->running.load(std::memory_order_relaxed); }

    // Kills the running child (and anything it spawned, when killWithApp was
    // set). The pending WM_APP_PROC_EXIT reports lParam = 1.
    void Cancel();

    // Waits for the worker thread to finish. Safe to call when idle.
    void Join();

    // PID of the running child, or 0. Lets a caller stop a process it started
    // in an earlier session-independent way (used to stop the terrain server).
    DWORD ProcessId() const
    { return m_run ? m_run->pid.load(std::memory_order_relaxed) : 0; }

    // Why the last Start returned false; empty after a successful Start.
    const std::string& LastError() const { return m_lastError; }

private:
    // Everything the pump thread touches lives here behind a shared_ptr, so a
    // child that deliberately outlives this object (the terrain server, started
    // with killWithApp = false) cannot leave the thread writing through a
    // dangling pointer. The thread owns the handles and closes them itself.
    struct Run
    {
        std::atomic<bool>  running{false};
        std::atomic<bool>  cancelled{false};
        std::atomic<DWORD> pid{0};

        HANDLE process     = nullptr;
        HANDLE job         = nullptr;   // only when killWithApp
        HANDLE readPipe    = nullptr;
        HWND   hwnd        = nullptr;
        bool   killWithApp = false;
        WORD   tag         = 0;

        ~Run();
    };

    static void PumpOutput(std::shared_ptr<Run> run);

    std::thread          m_thread;
    std::shared_ptr<Run> m_run;
    std::string          m_lastError;
    WORD                 m_tag = 0;
};
