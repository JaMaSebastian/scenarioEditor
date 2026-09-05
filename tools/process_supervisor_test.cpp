//=============================================================================
//  process_supervisor_test.cpp
//-----------------------------------------------------------------------------
//  Regression test for ProcessSupervisor — the three guarantees the Preview
//  tab's old ::ShellExecute calls could not make:
//
//    1. a real exit code is reported (a failing script is distinguishable
//       from a successful one)
//    2. stdout/stderr come back as lines (failures can be shown in the app
//       instead of a console window nobody is watching)
//    3. a killWithApp child dies with the supervisor — no orphans — while a
//       killWithApp = false child deliberately survives
//
//  Uses a message-only window to receive WM_APP_PROC_*, since the supervisor
//  delivers results by PostMessage.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-07
//=============================================================================
#include "pch.h"
#include "ProcessSupervisor.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    std::vector<std::string> g_lines;
    DWORD g_exitCode = 0xFFFFFFFF;
    bool  g_exited   = false;
    WORD  g_exitTag  = 0;

    LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
    {
        if (msg == WM_APP_PROC_OUTPUT)
        {
            char* line = reinterpret_cast<char*>(l);
            if (line) { g_lines.push_back(line); delete[] line; }
            return 0;
        }
        if (msg == WM_APP_PROC_EXIT)
        {
            g_exitCode = static_cast<DWORD>(w);
            g_exitTag  = HIWORD(l);
            g_exited   = true;
            return 0;
        }
        return ::DefWindowProc(h, msg, w, l);
    }

    HWND MakeMessageWindow()
    {
        static const wchar_t* kClass = L"ProcSupTestWnd";
        WNDCLASSW wc{};
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        ::RegisterClassW(&wc);
        return ::CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }

    // Pumps messages until the child exits or the deadline passes.
    bool PumpUntilExit(int timeoutMs)
    {
        const DWORD start = ::GetTickCount();
        MSG msg;
        while (!g_exited && (::GetTickCount() - start) < static_cast<DWORD>(timeoutMs))
        {
            while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
            }
            ::Sleep(10);
        }
        return g_exited;
    }

    void Reset()
    {
        g_lines.clear();
        g_exitCode = 0xFFFFFFFF;
        g_exited   = false;
        g_exitTag  = 0;
    }

    int g_failures = 0;
    void Check(bool ok, const char* what)
    {
        printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
        if (!ok) ++g_failures;
    }

    bool ProcessAlive(DWORD pid)
    {
        if (pid == 0) return false;
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return false;
        DWORD code = 0;
        const bool alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
        ::CloseHandle(h);
        return alive;
    }
}

int main()
{
    HWND wnd = MakeMessageWindow();
    if (!wnd) { printf("could not create message window\n"); return 1; }

    printf("process_supervisor_test\n");

    // --- 1. exit codes are reported ------------------------------------
    {
        Reset();
        ProcessSupervisor sup;
        sup.SetTag(7);
        const bool started = sup.Start(wnd, L"cmd.exe /c exit 3", L"", true);
        Check(started, "non-zero exit: Start succeeded");
        Check(PumpUntilExit(15000), "non-zero exit: exit message received");
        Check(g_exitCode == 3, "non-zero exit: code is 3 (not just 'launched ok')");
        Check(g_exitTag == 7, "non-zero exit: tag round-trips");
        sup.Join();
    }

    // --- 2. stdout and stderr are captured ------------------------------
    {
        Reset();
        ProcessSupervisor sup;
        // 1>&2 sends the second line to stderr; both must arrive.
        sup.Start(wnd, L"cmd.exe /c echo alpha & echo beta 1>&2", L"", true);
        Check(PumpUntilExit(15000), "output: exit message received");
        bool sawAlpha = false, sawBeta = false;
        for (const std::string& s : g_lines)
        {
            if (s.find("alpha") != std::string::npos) sawAlpha = true;
            if (s.find("beta")  != std::string::npos) sawBeta  = true;
        }
        Check(sawAlpha, "output: stdout line captured");
        Check(sawBeta,  "output: stderr line captured");
        Check(g_exitCode == 0, "output: success reported as 0");
        sup.Join();
    }

    // --- 3a. killWithApp child dies with the supervisor -----------------
    {
        Reset();
        DWORD pid = 0;
        {
            ProcessSupervisor sup;
            // A child that would otherwise sit for two minutes.
            sup.Start(wnd, L"cmd.exe /c ping -n 120 127.0.0.1 > nul", L"", true);
            ::Sleep(600);
            pid = sup.ProcessId();
            Check(pid != 0 && ProcessAlive(pid), "orphan: child is running");
        }   // destructor: cancel + join
        ::Sleep(400);
        Check(!ProcessAlive(pid), "orphan: child died with the supervisor");
    }

    // --- 3b. killWithApp = false child survives (the terrain server case) --
    {
        Reset();
        DWORD pid = 0;
        {
            ProcessSupervisor sup;
            sup.Start(wnd, L"cmd.exe /c ping -n 30 127.0.0.1 > nul", L"", false);
            ::Sleep(600);
            pid = sup.ProcessId();
            Check(pid != 0 && ProcessAlive(pid), "survivor: child is running");
        }   // destructor: detach, must NOT kill
        ::Sleep(400);
        const bool alive = ProcessAlive(pid);
        Check(alive, "survivor: child outlived the supervisor");
        if (alive)
        {
            HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) { ::TerminateProcess(h, 0); ::CloseHandle(h); }
        }
    }

    // --- 4. a bogus command fails at Start, with a reason ----------------
    {
        Reset();
        ProcessSupervisor sup;
        const bool started = sup.Start(wnd, L"no_such_program_xyz.exe", L"", true);
        Check(!started, "bad command: Start returns false");
        Check(!sup.LastError().empty(), "bad command: LastError explains why");
    }

    ::DestroyWindow(wnd);

    printf("\nprocess_supervisor_test %s (%d failure%s)\n",
           g_failures == 0 ? "PASS" : "FAIL",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
