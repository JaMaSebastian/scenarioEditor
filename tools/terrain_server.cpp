//=============================================================================
//  terrain_server.cpp  ->  TerrainServer.exe
//-----------------------------------------------------------------------------
//  Native replacement for DISBrowser/Scripts/wsl/serve.py: a static HTTP file
//  server for self-hosted Cesium quantized-mesh terrain (and the i3dm foliage
//  tileset served alongside it).
//
//  Why this exists as a Windows binary instead of Python-in-WSL:
//    * serve.py had to run inside WSL, and its console window was load-bearing
//      -- the foreground wsl.exe was what kept the WSL VM (and the server)
//      alive. Closing or losing that window silently killed terrain.
//    * Stopping it meant `pkill -f serve.py` against a hard-coded distro name.
//    * `\\wsl$` is not usable here (P9RdrService is stopped and needs admin),
//      so tiles are published to an NTFS folder and served natively instead.
//
//  Difference from serve.py, deliberate: serve.py decompressed every .terrain
//  on EVERY request because ctb-tile writes them gzipped. Tiles are now
//  gunzipped once during the publish step, so this server does no decompression
//  at all -- it just streams bytes. That keeps the wire format identical (raw
//  quantized-mesh, no Content-Encoding) with less work per request and no zlib
//  dependency.
//
//  Everything the client sees is unchanged from serve.py:
//    * raw quantized-mesh bodies, correct Content-Length, no Content-Encoding
//      (UE's HTTP stack fails on gzipped bodies, and cesium-native can't parse
//      raw gzip without the header -- so neither option works; plain is the
//      only thing that does)
//    * Cache-Control: no-store, no-cache, must-revalidate  (otherwise a rebuild
//      is shadowed by cesium-request-cache.sqlite)
//    * Access-Control-Allow-Origin: *
//    * non-.terrain paths pass through untouched, which is how
//      /foliage/tileset.json and /foliage/*.i3dm are served with no extra code
//
//  Usage: TerrainServer.exe [tilesDir] [port]
//         defaults: %LOCALAPPDATA%\DISBrowser\terrain\tiles   8088
//
//  CONFORMANCE: the containerized Linux server (planeswalker/terrainserver,
//  port 8089) is a separate implementation of the same wire contract, and the
//  two share no source. Nothing keeps them honest except
//      terrainserver/scripts/wire-contract-check.sh <base-url>
//  which must pass against BOTH. Run it after touching anything below.
//  Known divergence as of 2026-08-20: HEAD replies here carry
//  Content-Length: 0 rather than the body length (see the SendResponse call in
//  HandleConnection) -- latent, since Cesium only ever GETs tiles.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-07
//=============================================================================
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
    std::string g_root;
    std::atomic<long long> g_served{0};
    std::atomic<long long> g_missing{0};

    void LogLine(const char* fmt, ...)
    {
        char msg[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        // stdout is piped to the editor's log pane when launched from there.
        printf("[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
        fflush(stdout);
    }

    //
    // PercentDecode - turns %20 and friends back into bytes. Cesium's paths are
    //   plain, but layer.json/tileset.json can come through a URL builder.
    //
    std::string PercentDecode(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '%' && i + 2 < in.size() &&
                isxdigit(static_cast<unsigned char>(in[i + 1])) &&
                isxdigit(static_cast<unsigned char>(in[i + 2])))
            {
                const int hi = isdigit(in[i+1]) ? in[i+1]-'0' : (tolower(in[i+1])-'a'+10);
                const int lo = isdigit(in[i+2]) ? in[i+2]-'0' : (tolower(in[i+2])-'a'+10);
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
            }
            else if (in[i] == '+') out.push_back(' ');
            else out.push_back(in[i]);
        }
        return out;
    }

    //
    // SafeJoin - maps a URL path onto the tiles dir, rejecting anything that
    //   escapes it. Without this, "GET /../../windows/win.ini" would be served.
    //
    bool SafeJoin(const std::string& urlPath, std::string& outPath)
    {
        std::string p = PercentDecode(urlPath);

        // Strip query/fragment: Cesium asks for ".terrain?v=1.1.0".
        const size_t q = p.find_first_of("?#");
        if (q != std::string::npos) p.erase(q);

        for (char& c : p) if (c == '/') c = '\\';
        while (!p.empty() && p.front() == '\\') p.erase(p.begin());

        if (p.find("..") != std::string::npos) return false;
        if (p.find(':') != std::string::npos) return false;

        outPath = g_root;
        if (!outPath.empty() && outPath.back() != '\\') outPath.push_back('\\');
        outPath += p;
        return true;
    }

    const char* ContentTypeFor(const std::string& path)
    {
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) return "application/octet-stream";
        std::string ext = path.substr(dot);
        for (char& c : ext) c = static_cast<char>(tolower(c));

        if (ext == ".json") return "application/json";
        if (ext == ".glb")  return "model/gltf-binary";
        // .terrain and .i3dm are both opaque binaries to the client.
        return "application/octet-stream";
    }

    bool ReadWholeFile(const std::string& path, std::vector<char>& out)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        const std::streamsize n = f.tellg();
        if (n < 0) return false;
        f.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(n));
        if (n > 0 && !f.read(out.data(), n)) return false;
        return true;
    }

    void SendAll(SOCKET s, const char* data, size_t len)
    {
        size_t sent = 0;
        while (sent < len)
        {
            const int n = ::send(s, data + sent, static_cast<int>(len - sent), 0);
            if (n == SOCKET_ERROR) return;   // client hung up; nothing to do
            sent += static_cast<size_t>(n);
        }
    }

    void SendResponse(SOCKET s, int code, const char* reason,
                      const char* contentType, const char* body, size_t bodyLen)
    {
        char hdr[512];
        const int n = sprintf_s(hdr, sizeof(hdr),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            // Match serve.py exactly: without no-store a rebuild's fresh tiles
            // are shadowed by cesium-request-cache.sqlite.
            "Cache-Control: no-store, no-cache, must-revalidate\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n",
            code, reason, contentType, bodyLen);

        SendAll(s, hdr, static_cast<size_t>(n));
        if (bodyLen) SendAll(s, body, bodyLen);
    }

    void HandleConnection(SOCKET client)
    {
        // Read until end of headers. Requests are tiny; one buffer is plenty.
        std::string req;
        char buf[4096];
        for (;;)
        {
            const int n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) { ::closesocket(client); return; }
            req.append(buf, n);
            if (req.find("\r\n\r\n") != std::string::npos) break;
            if (req.size() > 64 * 1024) break;      // not a sane request
        }

        // "GET /path HTTP/1.1"
        const size_t sp1 = req.find(' ');
        const size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                                      : req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos)
        {
            SendResponse(client, 400, "Bad Request", "text/plain", "", 0);
            ::closesocket(client);
            return;
        }

        const std::string method = req.substr(0, sp1);
        const std::string url    = req.substr(sp1 + 1, sp2 - sp1 - 1);

        if (method != "GET" && method != "HEAD")
        {
            SendResponse(client, 405, "Method Not Allowed", "text/plain", "", 0);
            ::closesocket(client);
            return;
        }

        std::string path;
        if (!SafeJoin(url, path))
        {
            LogLine("403 rejected path %s", url.c_str());
            SendResponse(client, 403, "Forbidden", "text/plain", "", 0);
            ::closesocket(client);
            return;
        }

        std::vector<char> body;
        if (!ReadWholeFile(path, body))
        {
            // Missing tiles are the interesting failure: they break Cesium's
            // refinement chain, so name them the way serve.py did.
            const long long m = ++g_missing;
            LogLine("404 MISSING %s  (%lld missing so far)", url.c_str(), m);
            static const char kMsg[] = "tile not found";
            SendResponse(client, 404, "Not Found", "text/plain",
                         kMsg, sizeof(kMsg) - 1);
            ::closesocket(client);
            return;
        }

        SendResponse(client, 200, "OK", ContentTypeFor(path),
                     (method == "HEAD") ? "" : body.data(),
                     (method == "HEAD") ? 0 : body.size());

        const long long n = ++g_served;
        if (n % 500 == 0)
            LogLine("served %lld tiles ok (%lld missing so far)",
                    n, g_missing.load());

        ::closesocket(client);
    }

    std::string DefaultTilesDir()
    {
        char local[MAX_PATH]{};
        if (SUCCEEDED(::SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local)))
            return std::string(local) + "\\DISBrowser\\terrain\\tiles";
        return "C:\\DISBrowser\\terrain\\tiles";
    }
}

int main(int argc, char** argv)
{
    g_root = (argc > 1) ? argv[1] : DefaultTilesDir();
    const unsigned short port =
        static_cast<unsigned short>((argc > 2) ? atoi(argv[2]) : 8088);

    const DWORD attr = ::GetFileAttributesA(g_root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        LogLine("ERROR: tiles directory not found: %s", g_root.c_str());
        LogLine("Build 3D Terrain first, or pass the directory as argument 1.");
        return 2;
    }

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        LogLine("ERROR: WSAStartup failed");
        return 1;
    }

    const SOCKET listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET)
    {
        LogLine("ERROR: socket() failed: %d", ::WSAGetLastError());
        ::WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    ::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = ::htons(port);

    if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        const int err = ::WSAGetLastError();
        // Say it plainly, the way serve.py did -- this is the single most
        // common startup failure and a bare code helps nobody.
        LogLine("ERROR: cannot bind port %u (%d).", port, err);
        if (err == WSAEADDRINUSE)
            LogLine("Port %u is already in use - is another TerrainServer, a "
                    "serve.py inside WSL, or a docker container publishing %u "
                    "still running?", port, port);
        ::closesocket(listenSock);
        ::WSACleanup();
        return 1;
    }

    if (::listen(listenSock, SOMAXCONN) == SOCKET_ERROR)
    {
        LogLine("ERROR: listen() failed: %d", ::WSAGetLastError());
        ::closesocket(listenSock);
        ::WSACleanup();
        return 1;
    }

    LogLine("Serving %s on http://localhost:%u  (raw quantized-mesh, no gzip)",
            g_root.c_str(), port);

    for (;;)
    {
        const SOCKET client = ::accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            if (::WSAGetLastError() == WSAEINTR) break;
            continue;
        }
        // Thread per connection: responses are close-delimited (as serve.py's
        // HTTP/1.0 handler was), so connections are short-lived.
        std::thread(HandleConnection, client).detach();
    }

    ::closesocket(listenSock);
    ::WSACleanup();
    return 0;
}
