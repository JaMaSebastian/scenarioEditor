//=============================================================================
//  TerrainServiceClient.cpp  —  see TerrainServiceClient.h
//=============================================================================
#include "pch.h"
#include "TerrainServiceClient.h"
#include "log.h"

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <chrono>
#include <vector>

namespace
{
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                            static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                              &w[0], n);
        return w;
    }

    char* HeapCopy(const std::string& s)
    {
        char* p = new char[s.size() + 1];
        ::memcpy(p, s.c_str(), s.size() + 1);
        return p;
    }

    // One request, start to finish. Plain HTTP: the service binds loopback (or
    // sits behind an SSH tunnel on EC2), so TLS here would be ceremony.
    bool Request(const std::string& host, unsigned short port,
                 const wchar_t* verb, const std::string& path,
                 const std::string& requestBody,
                 int& status, std::string& body)
    {
        status = 0;
        body.clear();

        HINTERNET session = ::WinHttpOpen(L"ScenarioEditor/1.0",
                                          WINHTTP_ACCESS_TYPE_NO_PROXY,
                                          WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;

        // Short timeouts: this runs behind a UI poll, so a hung service must
        // not wedge the editor.
        ::WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);

        bool ok = false;
        HINTERNET conn = ::WinHttpConnect(session, Widen(host).c_str(), port, 0);
        if (conn)
        {
            HINTERNET req = ::WinHttpOpenRequest(conn, verb, Widen(path).c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (req)
            {
                const wchar_t* headers = requestBody.empty()
                    ? WINHTTP_NO_ADDITIONAL_HEADERS
                    : L"Content-Type: application/json\r\n";

                if (::WinHttpSendRequest(req, headers,
                        requestBody.empty() ? 0 : DWORD(-1L),
                        requestBody.empty() ? WINHTTP_NO_REQUEST_DATA
                                            : const_cast<char*>(requestBody.data()),
                        static_cast<DWORD>(requestBody.size()),
                        static_cast<DWORD>(requestBody.size()), 0) &&
                    ::WinHttpReceiveResponse(req, nullptr))
                {
                    DWORD code = 0, len = sizeof(code);
                    ::WinHttpQueryHeaders(req,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                        WINHTTP_NO_HEADER_INDEX);
                    status = static_cast<int>(code);

                    DWORD avail = 0;
                    do
                    {
                        avail = 0;
                        if (!::WinHttpQueryDataAvailable(req, &avail)) break;
                        if (avail == 0) break;
                        const size_t base = body.size();
                        body.resize(base + avail);
                        DWORD read = 0;
                        if (!::WinHttpReadData(req, &body[base], avail, &read)) break;
                        body.resize(base + read);
                    } while (avail > 0);

                    ok = (status != 0);
                }
                ::WinHttpCloseHandle(req);
            }
            ::WinHttpCloseHandle(conn);
        }
        ::WinHttpCloseHandle(session);
        return ok;
    }

    size_t FindKeyValue(const std::string& json, const std::string& key)
    {
        const std::string needle = "\"" + key + "\"";
        size_t pos = 0;
        while ((pos = json.find(needle, pos)) != std::string::npos)
        {
            size_t i = pos + needle.size();
            while (i < json.size() && isspace(static_cast<unsigned char>(json[i]))) ++i;
            if (i < json.size() && json[i] == ':') return i + 1;
            pos = i;
        }
        return std::string::npos;
    }

    std::string Unescape(const std::string& in)
    {
        std::string out;
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '\\' && i + 1 < in.size())
            {
                ++i;
                switch (in[i])
                {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default:  out.push_back(in[i]); break;
                }
            }
            else out.push_back(in[i]);
        }
        return out;
    }
}

TerrainServiceClient::~TerrainServiceClient()
{
    m_cancel = true;
    if (m_thread.joinable()) m_thread.detach();   // the poll loop exits on its own
}

bool TerrainServiceClient::Get(const std::string& host, unsigned short port,
                               const std::string& path, int& status, std::string& body)
{
    return Request(host, port, L"GET", path, std::string(), status, body);
}

bool TerrainServiceClient::Post(const std::string& host, unsigned short port,
                                const std::string& path, const std::string& requestBody,
                                int& status, std::string& body)
{
    return Request(host, port, L"POST", path, requestBody, status, body);
}

bool TerrainServiceClient::FindString(const std::string& json, const std::string& key,
                                      std::string& out)
{
    size_t i = FindKeyValue(json, key);
    if (i == std::string::npos) return false;
    while (i < json.size() && isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != '"') return false;
    ++i;
    std::string v;
    while (i < json.size() && json[i] != '"')
    {
        if (json[i] == '\\' && i + 1 < json.size()) { v.push_back(json[i]); ++i; }
        v.push_back(json[i]);
        ++i;
    }
    out = Unescape(v);
    return true;
}

bool TerrainServiceClient::FindNumber(const std::string& json, const std::string& key,
                                      double& out)
{
    size_t i = FindKeyValue(json, key);
    if (i == std::string::npos) return false;
    while (i < json.size() && isspace(static_cast<unsigned char>(json[i]))) ++i;
    char* end = nullptr;
    const double v = strtod(json.c_str() + i, &end);
    if (end == json.c_str() + i) return false;
    out = v;
    return true;
}

void TerrainServiceClient::FindLogLines(const std::string& json,
                                        std::vector<std::string>& out)
{
    out.clear();
    size_t i = FindKeyValue(json, "log");
    if (i == std::string::npos) return;
    while (i < json.size() && json[i] != '[') ++i;
    if (i >= json.size()) return;
    ++i;

    while (i < json.size() && json[i] != ']')
    {
        if (json[i] != '"') { ++i; continue; }
        ++i;
        std::string v;
        while (i < json.size() && json[i] != '"')
        {
            if (json[i] == '\\' && i + 1 < json.size()) { v.push_back(json[i]); ++i; }
            v.push_back(json[i]);
            ++i;
        }
        out.push_back(Unescape(v));
        ++i;
    }
}

bool TerrainServiceClient::StartJob(HWND hwnd, const std::string& host,
                                    unsigned short port, const std::string& jsonBody)
{
    if (m_running.exchange(true)) return false;
    m_cancel = false;
    m_jobId.clear();

    if (m_thread.joinable()) m_thread.detach();
    m_thread = std::thread(&TerrainServiceClient::Run, this, hwnd, host, port, jsonBody);
    return true;
}

void TerrainServiceClient::Cancel()
{
    if (!m_running.load()) return;
    m_cancel = true;
}

void TerrainServiceClient::Run(HWND hwnd, std::string host, unsigned short port,
                               std::string jsonBody)
{
    auto line = [&](const std::string& s) {
        ::PostMessage(hwnd, WM_APP_TERRAIN_JOB_LINE, 0,
                      reinterpret_cast<LPARAM>(HeapCopy(s)));
    };
    auto finish = [&](bool ok, const std::string& detail) {
        m_running = false;
        ::PostMessage(hwnd, WM_APP_TERRAIN_JOB_DONE, ok ? 0 : 1,
                      reinterpret_cast<LPARAM>(HeapCopy(detail)));
    };

    int status = 0;
    std::string body;
    if (!Post(host, port, "/api/v1/jobs", jsonBody, status, body))
    {
        finish(false, "Cannot reach the terrain service at " + host + ":" +
                      std::to_string(port) + ".");
        return;
    }

    if (status != 202)
    {
        // The server's own message is far more useful than a generic failure:
        // 409 names the job in the way, 422 names the unknown type.
        std::string msg;
        FindString(body, "message", msg);
        std::string blocker;
        FindString(body, "jobId", blocker);
        std::string detail = "The service refused the job (HTTP " +
                             std::to_string(status) + ")";
        if (!msg.empty())     detail += ": " + msg;
        if (status == 409 && !blocker.empty())
            detail += "\n\nAlready running: " + blocker +
                      "\nEvery job writes the same tile tree, so they cannot overlap.";
        finish(false, detail);
        return;
    }

    if (!FindString(body, "jobId", m_jobId) || m_jobId.empty())
    {
        finish(false, "The service accepted the job but returned no job id.");
        return;
    }
    {
        char buf[200];
        sprintf_s(buf, sizeof(buf),
                  "TerrainServiceClient: job %s submitted", m_jobId.c_str());
        LOG(buf);
    }

    long long since = 0;
    bool cancelSent = false;

    for (;;)
    {
        if (m_cancel.load() && !cancelSent)
        {
            cancelSent = true;
            int s2 = 0; std::string b2;
            Post(host, port, "/api/v1/jobs/" + m_jobId + "/cancel", "", s2, b2);
            line("-- cancel requested");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        int st = 0;
        std::string js;
        if (!Get(host, port,
                 "/api/v1/jobs/" + m_jobId + "?since=" + std::to_string(since),
                 st, js) || st != 200)
        {
            // A blip must not kill a long build; only a sustained outage should.
            continue;
        }

        std::vector<std::string> lines;
        FindLogLines(js, lines);
        for (const std::string& l : lines) line(l);

        double seq = 0;
        if (FindNumber(js, "logSeq", seq)) since = static_cast<long long>(seq);

        std::string state;
        FindString(js, "state", state);
        if (state == "succeeded") { finish(true, std::string()); return; }
        if (state == "cancelled") { finish(false, "Job cancelled."); return; }
        if (state == "failed")
        {
            std::string err;
            FindString(js, "error", err);
            finish(false, err.empty() ? "The job failed (no reason reported)." : err);
            return;
        }
    }
}
