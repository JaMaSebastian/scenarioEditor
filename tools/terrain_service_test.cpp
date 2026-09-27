//=============================================================================
//  terrain_service_test.cpp  ->  terrain_service_test.exe
//-----------------------------------------------------------------------------
//  Exercises the Mode=Service plumbing without driving the GUI:
//
//    * ResolveTerrainEndpoint() for all four modes — the single resolver that
//      replaced two divergent copies (PreviewPage said "127.0.0.1", the Run tab
//      said "localhost", and nothing forced them to agree).
//    * TerrainServiceClient GET/POST against a live terrainserver, including
//      the submit-then-poll loop the Preview tab uses.
//    * The JSON readers, on real server responses rather than hand-written
//      fixtures.
//
//  Usage: terrain_service_test.exe [host] [port]     (default 127.0.0.1 8089)
//         Tests needing a live service are skipped, not failed, if nothing is
//         listening — the resolver checks still run.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-20
//=============================================================================
#include "pch.h"
#include "TerrainEndpoint.h"
#include "TerrainServiceClient.h"
#include "SettingsIO.h"
#include "StartupIniWriter.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_fail = 0;
    int g_pass = 0;

    void Check(bool cond, const char* what)
    {
        if (cond) { std::printf("  [PASS] %s\n", what); ++g_pass; }
        else      { std::printf("  [FAIL] %s\n", what); ++g_fail; }
    }

    void CheckEq(const std::string& got, const std::string& want, const char* what)
    {
        if (got == want) { std::printf("  [PASS] %s (%s)\n", what, got.c_str()); ++g_pass; }
        else { std::printf("  [FAIL] %s: got '%s', want '%s'\n", what, got.c_str(), want.c_str()); ++g_fail; }
    }

    void CheckEqU(unsigned got, unsigned want, const char* what)
    {
        if (got == want) { std::printf("  [PASS] %s (%u)\n", what, got); ++g_pass; }
        else { std::printf("  [FAIL] %s: got %u, want %u\n", what, got, want); ++g_fail; }
    }
}

int main(int argc, char** argv)
{
    const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    const unsigned short port =
        static_cast<unsigned short>((argc > 2) ? atoi(argv[2]) : 8089);

    std::printf("terrain_service_test  (%s:%u)\n\n", host.c_str(), port);

    // ---------------------------------------------------------------- resolver
    std::printf("ResolveTerrainEndpoint:\n");
    {
        Settings s;
        s.terrainRemoteHost = "example.internal";
        s.terrainRemotePort = 9999;

        s.terrainMode = TerrainServerMode::Local;
        TerrainEndpoint ep = ResolveTerrainEndpoint(s);
        CheckEq(ep.host, "127.0.0.1", "Local ignores RemoteHost");
        CheckEqU(ep.port, 8088, "Local uses 8088");
        Check(ep.managedHere && !ep.jobApi, "Local is managed here, no job API");

        s.terrainMode = TerrainServerMode::Legacy;
        ep = ResolveTerrainEndpoint(s);
        CheckEqU(ep.port, 8088, "Legacy uses 8088");
        Check(ep.managedHere, "Legacy is managed here");

        s.terrainMode = TerrainServerMode::Remote;
        ep = ResolveTerrainEndpoint(s);
        CheckEq(ep.host, "example.internal", "Remote honours RemoteHost");
        CheckEqU(ep.port, 9999, "Remote honours RemotePort");
        Check(!ep.managedHere && !ep.jobApi, "Remote: not ours, no job API");

        s.terrainMode = TerrainServerMode::Service;
        ep = ResolveTerrainEndpoint(s);
        CheckEq(ep.host, "example.internal", "Service honours RemoteHost");
        Check(!ep.managedHere && ep.jobApi, "Service: not ours, HAS job API");

        // Empty host must not produce "://:8089"; and port 0 must fall back.
        s.terrainRemoteHost.clear();
        s.terrainRemotePort = 0;
        ep = ResolveTerrainEndpoint(s);
        CheckEq(ep.host, "127.0.0.1", "Service falls back to 127.0.0.1 on empty host");
        CheckEqU(ep.port, 8089, "Service defaults to 8089, not 8088");
    }

    // ------------------------------------------------------------ JSON readers
    std::printf("\nJSON readers:\n");
    {
        const std::string sample =
            "{\"id\":\"20260820-1\",\"state\":\"failed\",\"logSeq\":42,"
            "\"error\":\"boom \\\"quoted\\\" and \\\\ slashed\","
            "\"log\":[\"first line\",\"second \\\"quoted\\\" line\"]}";

        std::string v;
        Check(TerrainServiceClient::FindString(sample, "state", v) && v == "failed",
              "FindString reads a plain value");
        Check(TerrainServiceClient::FindString(sample, "error", v) &&
              v == "boom \"quoted\" and \\ slashed",
              "FindString unescapes quotes and backslashes");

        double n = 0;
        Check(TerrainServiceClient::FindNumber(sample, "logSeq", n) && n == 42,
              "FindNumber reads logSeq");

        std::vector<std::string> lines;
        TerrainServiceClient::FindLogLines(sample, lines);
        Check(lines.size() == 2, "FindLogLines finds both lines");
        if (lines.size() == 2)
            Check(lines[1] == "second \"quoted\" line", "log line unescaped correctly");

        // A key that only appears as a VALUE must not be mistaken for a key.
        const std::string tricky = "{\"message\":\"the state is odd\",\"state\":\"queued\"}";
        Check(TerrainServiceClient::FindString(tricky, "state", v) && v == "queued",
              "key lookup is not fooled by the same text inside a value");
    }

    // ------------------------------------------------- the URL DISBrowser gets
    // The whole point of Service mode is that DISBrowser fetches from the
    // container. If Startup.ini still says 8088, the editor is driving one
    // server and the viewer is reading another -- which looks exactly like
    // "my build did nothing".
    //
    // Ordered BEFORE the live-service section on purpose: that section returns early when
    // nothing is listening, and this block needs no server at all — it only asks what the
    // writer puts in the file. Behind that early return it silently never ran.
    std::printf("\nStartup.ini handoff:\n");
    {
        TCHAR tmpDir[MAX_PATH] = { 0 };
        ::GetTempPath(MAX_PATH, tmpDir);
        std::wstring proj = std::wstring(tmpDir) + L"terrain_svc_test_proj";
        ::CreateDirectoryW(proj.c_str(), nullptr);
        ::CreateDirectoryW((proj + L"\\Config").c_str(), nullptr);

        Settings s;
        s.terrainMode       = TerrainServerMode::Service;
        s.terrainRemoteHost = host;
        s.terrainRemotePort = port;
        const TerrainEndpoint ep = ResolveTerrainEndpoint(s);

        StartupIniWriter::FoliageHandoff fol;
        std::wstring err;
        const bool ok = StartupIniWriter::Write(
            proj, "Generic", "Cesium 3D (self-hosted)", false,
            25.0, 121.0, 0.0,
            true, 20.0, 26.0, 118.0, 124.0,
            std::wstring(), fol,
            ep.host, ep.port, err);
        Check(ok, "StartupIniWriter::Write succeeded");

        std::string text;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (proj + L"\\Config\\Startup.ini").c_str(), L"rb") == 0 && f)
        {
            char buf[4096];
            size_t n = 0;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }

        char want[128];
        sprintf_s(want, sizeof(want), "CesiumTilesetUrl=\"http://%s:%u/layer.json\"",
                  host.c_str(), static_cast<unsigned>(port));
        Check(text.find(want) != std::string::npos,
              "Startup.ini points DISBrowser at the service port");
        std::printf("         %s\n", want);
        Check(text.find(":8088/layer.json") == std::string::npos,
              "Startup.ini does NOT still say 8088");

        // Imagery row index. ArcGIS numbers rows from the NORTH, which is Cesium/DISBrowser
        // {y}; {reverseY} is the south-up (TMS) index and silently serves the wrong
        // hemisphere. It shipped that way once, so it is worth a test.
        Check(text.find("MapServer/tile/{z}/{y}/{x}") != std::string::npos,
              "Startup.ini uses the north-up {y} tile row for ArcGIS imagery");
        Check(text.find("{reverseY}") == std::string::npos,
              "Startup.ini does NOT use the south-up {reverseY} row");
    }
    // ------------------------------------------------------------- live server
    std::printf("\nLive service:\n");
    int status = 0;
    std::string body;
    if (!TerrainServiceClient::Get(host, port, "/api/v1/health", status, body) || status != 200)
    {
        std::printf("  [SKIP] no service on %s:%u (health returned %d)\n",
                    host.c_str(), port, status);
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    Check(status == 200, "GET /api/v1/health returns 200");
    std::string sv;
    Check(TerrainServiceClient::FindString(body, "status", sv) &&
          (sv == "healthy" || sv == "degraded"),
          "health reports a known status");
    std::printf("         status=%s\n", sv.c_str());

    // A malformed job must be refused, and the refusal must be readable.
    if (TerrainServiceClient::Post(host, port, "/api/v1/jobs", "{}", status, body))
    {
        Check(status == 400, "POST with no type is refused with 400");
        std::string msg;
        Check(TerrainServiceClient::FindString(body, "message", msg) && !msg.empty(),
              "refusal carries a human-readable message");
        std::printf("         message=%s\n", msg.c_str());
    }

    // The full submit-then-poll loop, on a job that is safe to run repeatedly.
    std::printf("  submitting rebuild-availability...\n");
    if (TerrainServiceClient::Post(host, port, "/api/v1/jobs",
                                   "{\"type\":\"rebuild-availability\"}", status, body))
    {
        Check(status == 202, "POST rebuild-availability returns 202");
        std::string jobId;
        Check(TerrainServiceClient::FindString(body, "jobId", jobId) && !jobId.empty(),
              "response carries a jobId");

        std::string state;
        long long since = 0;
        std::vector<std::string> allLines;
        for (int i = 0; i < 60 && !jobId.empty(); ++i)
        {
            ::Sleep(500);
            std::string js;
            char path[256];
            sprintf_s(path, sizeof(path), "/api/v1/jobs/%s?since=%lld", jobId.c_str(), since);
            if (!TerrainServiceClient::Get(host, port, path, status, js) || status != 200)
                continue;

            std::vector<std::string> lines;
            TerrainServiceClient::FindLogLines(js, lines);
            for (const std::string& l : lines) allLines.push_back(l);

            double seq = 0;
            if (TerrainServiceClient::FindNumber(js, "logSeq", seq))
                since = static_cast<long long>(seq);

            TerrainServiceClient::FindString(js, "state", state);
            if (state == "succeeded" || state == "failed" || state == "cancelled") break;
        }

        CheckEq(state, "succeeded", "job polled to completion");
        Check(!allLines.empty(), "poll collected log lines incrementally");
        for (const std::string& l : allLines) std::printf("         | %s\n", l.c_str());

        // since= must be honoured: asking again from the end yields nothing new.
        std::string js;
        char path[256];
        sprintf_s(path, sizeof(path), "/api/v1/jobs/%s?since=%lld", jobId.c_str(), since);
        if (TerrainServiceClient::Get(host, port, path, status, js) && status == 200)
        {
            std::vector<std::string> lines;
            TerrainServiceClient::FindLogLines(js, lines);
            Check(lines.empty(), "since=<newest> returns no repeated lines");
        }
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
