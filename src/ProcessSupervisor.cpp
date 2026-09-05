//=============================================================================
//  ProcessSupervisor.cpp
//-----------------------------------------------------------------------------
//  Implements ProcessSupervisor over CreateProcessW: builds an inheritable
//  stdout/stderr pipe, optionally assigns the child to a kill-on-close Job
//  Object, then pumps the pipe on a worker thread until EOF and reports the
//  exit code.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-07
//=============================================================================
#include "pch.h"
#include "ProcessSupervisor.h"
#include "../log.h"

#include <cstdio>
#include <vector>

namespace
{
    //
    // FormatWinError - "(123) The system cannot find the file specified."
    //   so failures name themselves instead of leaving a bare code.
    //
    std::string FormatWinError(DWORD err)
    {
        char* buf = nullptr;
        const DWORD n = ::FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<char*>(&buf), 0, nullptr);

        char out[512];
        if (n && buf)
        {
            // Trim the trailing CRLF FormatMessage appends.
            std::string msg(buf, n);
            while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n'))
                msg.pop_back();
            sprintf_s(out, sizeof(out), "(%lu) %s", err, msg.c_str());
        }
        else
        {
            sprintf_s(out, sizeof(out), "(%lu)", err);
        }
        if (buf) ::LocalFree(buf);
        return out;
    }

    //
    // PostLine - hands one output line to the UI. The receiver owns the buffer
    //   and must delete[] it; that contract is documented on WM_APP_PROC_OUTPUT.
    //   If the post fails (window gone), free it here so nothing leaks.
    //
    void PostLine(HWND hwnd, const std::string& line)
    {
        if (!hwnd || !::IsWindow(hwnd)) return;

        char* copy = new char[line.size() + 1];
        memcpy(copy, line.c_str(), line.size() + 1);
        if (!::PostMessageW(hwnd, WM_APP_PROC_OUTPUT, 0,
                            reinterpret_cast<LPARAM>(copy)))
            delete[] copy;
    }
}

ProcessSupervisor::ProcessSupervisor() = default;

//
// Run::~Run - the pump thread holds the last reference in the detached case, so
//   handle cleanup belongs here rather than in the supervisor. Closing the job
//   handle is what kills a killWithApp child; a detached (server) run has no
//   job, so closing its handles leaves the process alone.
//
ProcessSupervisor::Run::~Run()
{
    if (readPipe) ::CloseHandle(readPipe);
    if (process)  ::CloseHandle(process);
    if (job)      ::CloseHandle(job);
}

//
// ~ProcessSupervisor - two very different shutdowns:
//
//   killWithApp  : stop the child and join. Fast, and nothing is left behind.
//   !killWithApp : the child is MEANT to outlive us (the terrain server keeps a
//                  running DISBrowser supplied with tiles). Joining would block
//                  until the server exited -- i.e. hang the editor's close --
//                  so detach instead and let the thread finish on its own. It
//                  works only through the shared Run, which stays alive.
//
ProcessSupervisor::~ProcessSupervisor()
{
    if (m_run && m_run->killWithApp)
    {
        Cancel();
        Join();
    }
    else if (m_thread.joinable())
    {
        m_thread.detach();
    }
    m_run.reset();
}

//
// Start - creates the pipe, job and process, then spawns the pump thread.
//
bool ProcessSupervisor::Start(HWND uiHwnd,
                              const std::wstring& commandLine,
                              const std::wstring& workingDir,
                              bool killWithApp)
{
    m_lastError.clear();

    if (IsRunning())
    {
        m_lastError = "a process is already running";
        LOG("ProcessSupervisor::Start refused - already running");
        return false;
    }

    // A previous run's thread may have finished without being joined. Dropping
    // the old Run closes its handles.
    Join();
    m_run.reset();

    auto run = std::make_shared<Run>();
    run->hwnd        = uiHwnd;
    run->killWithApp = killWithApp;
    run->tag         = m_tag;

    // ---- pipe: child writes, we read ----
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;          // write end must be inheritable

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        m_lastError = "CreatePipe failed " + FormatWinError(::GetLastError());
        sprintf_s(szError, sizeof(szError), "ProcessSupervisor: %s", m_lastError.c_str());
        LOG(szError);
        return false;
    }
    // Our read end must NOT be inherited, or the child holds it open and we
    // never see EOF when it exits.
    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // ---- job object (one-shot builds only) ----
    if (killWithApp)
    {
        run->job = ::CreateJobObjectW(nullptr, nullptr);
        if (run->job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(run->job, JobObjectExtendedLimitInformation,
                                      &jeli, sizeof(jeli));
        }
        else
        {
            // Not fatal: we still get output and exit codes, we just lose the
            // crash-proof cleanup guarantee. Say so rather than pretend.
            sprintf_s(szError, sizeof(szError),
                      "ProcessSupervisor: CreateJobObject failed %s - child may outlive a crash",
                      FormatWinError(::GetLastError()).c_str());
            LOG(szError);
        }
    }

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError  = writePipe;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};

    // CreateProcessW may modify the command-line buffer, so hand it a copy.
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');

    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;

    const BOOL ok = ::CreateProcessW(
        nullptr, cmd.data(), nullptr, nullptr,
        TRUE,                                   // inherit the pipe
        flags, nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si, &pi);

    // The child has its own duplicate now; ours must go or EOF never arrives.
    ::CloseHandle(writePipe);

    if (!ok)
    {
        const DWORD err = ::GetLastError();
        m_lastError = "CreateProcess failed " + FormatWinError(err);
        sprintf_s(szError, sizeof(szError), "ProcessSupervisor: %s", m_lastError.c_str());
        LOG(szError);
        ::CloseHandle(readPipe);
        if (run->job) { ::CloseHandle(run->job); run->job = nullptr; }
        return false;
    }

    // Assign before resuming, so nothing the child spawns escapes the job.
    if (run->job) ::AssignProcessToJobObject(run->job, pi.hProcess);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    run->process  = pi.hProcess;
    run->readPipe = readPipe;
    run->pid.store(pi.dwProcessId, std::memory_order_relaxed);
    run->running.store(true, std::memory_order_relaxed);

    {
        // Log the command so the app's own log finally records what ran.
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, commandLine.c_str(), -1,
                                            nullptr, 0, nullptr, nullptr);
        std::string narrow(n > 0 ? n - 1 : 0, '\0');
        if (n > 0)
            ::WideCharToMultiByte(CP_UTF8, 0, commandLine.c_str(), -1,
                                  &narrow[0], n, nullptr, nullptr);
        sprintf_s(szError, sizeof(szError),
                  "ProcessSupervisor: started pid=%lu (killWithApp=%d): %s",
                  pi.dwProcessId, killWithApp ? 1 : 0, narrow.c_str());
        LOG(szError);
    }

    m_run    = run;
    m_thread = std::thread(&ProcessSupervisor::PumpOutput, run);
    return true;
}

//
// PumpOutput - blocking reads until the child closes the pipe, splitting into
//   lines; then collects the exit code and posts WM_APP_PROC_EXIT.
//
void ProcessSupervisor::PumpOutput(std::shared_ptr<Run> run)
{
    const HWND   uiHwnd   = run->hwnd;
    const HANDLE readPipe = run->readPipe;
    const HANDLE process  = run->process;

    std::string pending;
    char buf[4096];

    for (;;)
    {
        DWORD got = 0;
        // Returns FALSE with ERROR_BROKEN_PIPE when the child exits - that is
        // the normal end of stream, not an error.
        if (!::ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) || got == 0)
            break;

        pending.append(buf, got);

        size_t start = 0;
        for (;;)
        {
            const size_t nl = pending.find('\n', start);
            if (nl == std::string::npos) break;

            std::string line = pending.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // The scripts draw a VT100 progress UI; strip the escape sequences
            // so the editor's log pane shows text rather than control junk.
            std::string clean;
            clean.reserve(line.size());
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '\x1b')
                {
                    // Skip CSI ... final-byte, or a 2-char sequence like ESC 7.
                    if (i + 1 < line.size() && line[i + 1] == '[')
                    {
                        i += 2;
                        while (i < line.size() &&
                               !(line[i] >= '@' && line[i] <= '~')) ++i;
                    }
                    else ++i;
                    continue;
                }
                clean.push_back(line[i]);
            }

            if (!clean.empty())
            {
                PostLine(uiHwnd, clean);
                sprintf_s(szError, sizeof(szError), "  [child] %.900s", clean.c_str());
                LOG(szError);
            }
            start = nl + 1;
        }
        pending.erase(0, start);
    }

    if (!pending.empty())
        PostLine(uiHwnd, pending);

    DWORD exitCode = 0;
    ::WaitForSingleObject(process, INFINITE);
    ::GetExitCodeProcess(process, &exitCode);

    const bool cancelled = run->cancelled.load(std::memory_order_relaxed);

    sprintf_s(szError, sizeof(szError),
              "ProcessSupervisor: pid=%lu exited code=%lu%s",
              run->pid.load(std::memory_order_relaxed), exitCode,
              cancelled ? " (cancelled)" : "");
    LOG(szError);

    run->pid.store(0, std::memory_order_relaxed);
    run->running.store(false, std::memory_order_relaxed);

    // Handles are closed by ~Run when the last reference goes; in the detached
    // case that is this thread's own shared_ptr, on return.
    if (uiHwnd && ::IsWindow(uiHwnd))
        ::PostMessageW(uiHwnd, WM_APP_PROC_EXIT,
                       static_cast<WPARAM>(exitCode),
                       MAKELPARAM(cancelled ? 1 : 0, run->tag));
}

//
// Cancel - kills the whole job when we have one (so docker/wsl grandchildren
//   go too), otherwise just the direct child.
//
void ProcessSupervisor::Cancel()
{
    if (!m_run || !m_run->running.load(std::memory_order_relaxed)) return;

    m_run->cancelled.store(true, std::memory_order_relaxed);

    // Killing the job takes the wsl.exe / docker grandchildren with it; without
    // one, only the direct child can be reached.
    if (m_run->job)
        ::TerminateJobObject(m_run->job, 1);
    else if (m_run->process)
        ::TerminateProcess(m_run->process, 1);

    LOG("ProcessSupervisor: cancel requested");
}

void ProcessSupervisor::Join()
{
    if (m_thread.joinable()) m_thread.join();
}
