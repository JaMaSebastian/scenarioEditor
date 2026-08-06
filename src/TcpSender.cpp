//=============================================================================
//  TcpSender.cpp
//-----------------------------------------------------------------------------
//  Implements TcpSender over Winsock. Client role uses a non-blocking connect
//  with a select() timeout so a dead host doesn't hang the worker for the OS
//  default (~20s). Server role binds, listens, and accepts a single client with
//  the same bounded wait. Send loops over partial writes and, for the client
//  role with reconnect enabled, re-dials once after a dropped peer.
//
//  All error strings that can surface in a MessageBox are ASCII only — they
//  travel through CA2T, which converts via the ANSI codepage.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-05
//=============================================================================
#include "pch.h"
#include "TcpSender.h"
#include "../log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>

namespace
{
    inline SOCKET ToSocket(void* p) { return reinterpret_cast<SOCKET>(p); }
    inline void*  FromSocket(SOCKET s) { return reinterpret_cast<void*>(s); }

    //
    // WaitReady — blocks until the socket is readable (accept) or writable
    //   (connect), or the timeout expires. Returns true if ready.
    //
    bool WaitReady(SOCKET s, int timeoutMs, bool forWrite)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);

        timeval tv{};
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        const int rc = forWrite ? ::select(0, nullptr, &set, nullptr, &tv)
                                : ::select(0, &set, nullptr, nullptr, &tv);
        return rc > 0;
    }

    void SetNonBlocking(SOCKET s, bool nonBlocking)
    {
        u_long mode = nonBlocking ? 1u : 0u;
        ::ioctlsocket(s, FIONBIO, &mode);
    }
}

TcpSender::TcpSender() = default;

//
// ~TcpSender — closes both sockets and tears down Winsock.
//
TcpSender::~TcpSender()
{
    Close();
}

//
// Configure — records the role and endpoint settings. Tearing down any
//   existing link is deliberate: the caller changed where we should be
//   pointing, so the old connection is no longer the right one.
//
void TcpSender::Configure(bool listen,
                          const std::string& remoteHost, uint16_t remotePort,
                          uint16_t listenPort,
                          bool reconnect, int timeoutMs)
{
    const bool endpointChanged =
        listen     != m_listen     ||
        remoteHost != m_remoteHost ||
        remotePort != m_remotePort ||
        listenPort != m_listenPort;

    m_listen     = listen;
    m_remoteHost = remoteHost;
    m_remotePort = remotePort;
    m_listenPort = listenPort;
    m_reconnect  = reconnect;
    m_timeoutMs  = timeoutMs > 0 ? timeoutMs : 3000;

    if (endpointChanged)
        Close();
}

//
// EnsureWinsock — starts WSA once. Returns false (with m_lastError set) on
//   failure.
//
bool TcpSender::EnsureWinsock()
{
    if (m_wsaStarted) return true;

    WSADATA wsa{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0)
    {
        sprintf_s(szError, sizeof(szError), "WSAStartup failed: %d", rc);
        m_lastError = szError;
        LOG(szError);
        return false;
    }
    m_wsaStarted = true;
    return true;
}

//
// DescribeEndpoint — "listening on port N" / "connected to host:port", for
//   log lines and the Test TCP Connection dialog.
//
std::string TcpSender::DescribeEndpoint() const
{
    char buf[128];
    if (m_listen)
        sprintf_s(buf, sizeof(buf), "local port %u (server)",
                  static_cast<unsigned>(m_listenPort));
    else
        sprintf_s(buf, sizeof(buf), "%s:%u (client)",
                  m_remoteHost.c_str(), static_cast<unsigned>(m_remotePort));
    return buf;
}

//
// ConnectAsClient — non-blocking connect with a bounded select() wait, so an
//   unreachable host fails in m_timeoutMs instead of the OS default.
//
bool TcpSender::ConnectAsClient()
{
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        sprintf_s(szError, sizeof(szError),
                  "TCP socket() failed: %d", ::WSAGetLastError());
        m_lastError = szError;
        LOG(szError);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = ::htons(m_remotePort);
    if (::InetPtonA(AF_INET, m_remoteHost.c_str(), &addr.sin_addr) != 1)
    {
        sprintf_s(szError, sizeof(szError),
                  "TCP remote host '%s' is not a valid IPv4 address",
                  m_remoteHost.c_str());
        m_lastError = szError;
        LOG(szError);
        ::closesocket(s);
        return false;
    }

    SetNonBlocking(s, true);
    const int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR)
    {
        const int err = ::WSAGetLastError();
        if (err != WSAEWOULDBLOCK)
        {
            sprintf_s(szError, sizeof(szError),
                      "TCP connect(%s:%u) failed: %d",
                      m_remoteHost.c_str(), static_cast<unsigned>(m_remotePort), err);
            m_lastError = szError;
            LOG(szError);
            ::closesocket(s);
            return false;
        }

        if (!WaitReady(s, m_timeoutMs, /*forWrite*/ true))
        {
            sprintf_s(szError, sizeof(szError),
                      "TCP connect(%s:%u) timed out after %d ms - is the receiver listening?",
                      m_remoteHost.c_str(), static_cast<unsigned>(m_remotePort), m_timeoutMs);
            m_lastError = szError;
            LOG(szError);
            ::closesocket(s);
            return false;
        }

        // select() reports writable for both success and refusal; SO_ERROR
        // is what actually distinguishes them.
        int soErr = 0;
        int len = static_cast<int>(sizeof(soErr));
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
        if (soErr != 0)
        {
            sprintf_s(szError, sizeof(szError),
                      "TCP connect(%s:%u) refused: %d",
                      m_remoteHost.c_str(), static_cast<unsigned>(m_remotePort), soErr);
            m_lastError = szError;
            LOG(szError);
            ::closesocket(s);
            return false;
        }
    }

    SetNonBlocking(s, false);
    m_socket = FromSocket(s);

    sprintf_s(szError, sizeof(szError),
              "TCP connected to %s:%u", m_remoteHost.c_str(),
              static_cast<unsigned>(m_remotePort));
    LOG(szError);
    return true;
}

//
// ConnectAsServer — binds/listens once (the listen socket is kept across
//   client drops so the port stays claimed), then accepts one client within
//   m_timeoutMs.
//
bool TcpSender::ConnectAsServer()
{
    if (!m_listenSocket)
    {
        const SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET)
        {
            sprintf_s(szError, sizeof(szError),
                      "TCP socket() failed: %d", ::WSAGetLastError());
            m_lastError = szError;
            LOG(szError);
            return false;
        }

        // Let a restarted run rebind immediately instead of waiting out
        // TIME_WAIT on the previous listener.
        BOOL reuse = TRUE;
        ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = ::htons(m_listenPort);

        if (::bind(ls, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
        {
            const int err = ::WSAGetLastError();
            sprintf_s(szError, sizeof(szError),
                      "TCP bind(port %u) failed: %d%s",
                      static_cast<unsigned>(m_listenPort), err,
                      err == WSAEADDRINUSE ? " - port already in use" : "");
            m_lastError = szError;
            LOG(szError);
            ::closesocket(ls);
            return false;
        }

        if (::listen(ls, SOMAXCONN) == SOCKET_ERROR)
        {
            sprintf_s(szError, sizeof(szError),
                      "TCP listen(port %u) failed: %d",
                      static_cast<unsigned>(m_listenPort), ::WSAGetLastError());
            m_lastError = szError;
            LOG(szError);
            ::closesocket(ls);
            return false;
        }

        m_listenSocket = FromSocket(ls);
        sprintf_s(szError, sizeof(szError),
                  "TCP listening on port %u", static_cast<unsigned>(m_listenPort));
        LOG(szError);
    }

    const SOCKET ls = ToSocket(m_listenSocket);
    if (!WaitReady(ls, m_timeoutMs, /*forWrite*/ false))
    {
        sprintf_s(szError, sizeof(szError),
                  "TCP accept on port %u timed out after %d ms - no client connected",
                  static_cast<unsigned>(m_listenPort), m_timeoutMs);
        m_lastError = szError;
        LOG(szError);
        return false;
    }

    const SOCKET cs = ::accept(ls, nullptr, nullptr);
    if (cs == INVALID_SOCKET)
    {
        sprintf_s(szError, sizeof(szError),
                  "TCP accept failed: %d", ::WSAGetLastError());
        m_lastError = szError;
        LOG(szError);
        return false;
    }

    m_socket = FromSocket(cs);
    sprintf_s(szError, sizeof(szError),
              "TCP client accepted on port %u", static_cast<unsigned>(m_listenPort));
    LOG(szError);
    return true;
}

//
// Connect — establishes the link if it isn't already up.
//
bool TcpSender::Connect()
{
    m_lastError.clear();

    if (m_socket) return true;
    if (!EnsureWinsock()) return false;

    return m_listen ? ConnectAsServer() : ConnectAsClient();
}

//
// CloseSocketOnly — drops the data socket but keeps a server's listen socket,
//   so the next Connect() can accept a replacement client on the same port.
//
void TcpSender::CloseSocketOnly()
{
    if (m_socket)
    {
        ::closesocket(ToSocket(m_socket));
        m_socket = nullptr;
    }
}

//
// Send — writes the whole buffer. TCP may accept only part of a buffer per
//   call, so loop until it's all out. On a dropped peer, a client with
//   reconnect enabled re-dials once and retries from the start of the buffer
//   (a half-written PDU can't be resumed on a new connection).
//
int TcpSender::Send(const void* data, size_t length)
{
    m_lastError.clear();
    if (length == 0) return 0;

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (!m_socket && !Connect())
            return 0;

        const char* p = static_cast<const char*>(data);
        size_t remaining = length;
        bool dropped = false;

        while (remaining > 0)
        {
            const int sent = ::send(ToSocket(m_socket), p,
                                    static_cast<int>(remaining), 0);
            if (sent == SOCKET_ERROR)
            {
                const int err = ::WSAGetLastError();
                sprintf_s(szError, sizeof(szError),
                          "TCP send(%zu B) failed: %d%s", length, err,
                          (err == WSAECONNRESET || err == WSAECONNABORTED)
                              ? " - peer closed the connection" : "");
                m_lastError = szError;
                LOG(szError);
                CloseSocketOnly();
                dropped = true;
                break;
            }
            p         += sent;
            remaining -= static_cast<size_t>(sent);
        }

        if (!dropped)
        {
            m_lastError.clear();
            return static_cast<int>(length);
        }

        // Only a reconnecting client gets a second attempt. A server has no
        // one to re-dial; it waits for a new client on the next Send.
        if (m_listen || !m_reconnect)
            return 0;

        LOG("TCP link dropped - attempting one reconnect");
    }

    return 0;
}

//
// Close — closes the data and listen sockets and calls WSACleanup.
//
void TcpSender::Close()
{
    CloseSocketOnly();
    if (m_listenSocket)
    {
        ::closesocket(ToSocket(m_listenSocket));
        m_listenSocket = nullptr;
    }
    if (m_wsaStarted)
    {
        ::WSACleanup();
        m_wsaStarted = false;
    }
}
