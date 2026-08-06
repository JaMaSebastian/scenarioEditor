//=============================================================================
//  UdpSender.cpp
//-----------------------------------------------------------------------------
//  Implements UdpSender over Winsock: lazily starts WSA and creates the UDP
//  socket on first Send, resolves the dotted-quad destination, applies cached
//  multicast options once when targeting a multicast group, and sends the
//  datagram. Logs failures via szError/LOG and cleans up in Close.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "UdpSender.h"
#include "../log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>

namespace
{
    inline SOCKET ToSocket(void* p) { return reinterpret_cast<SOCKET>(p); }
    inline void*  FromSocket(SOCKET s) { return reinterpret_cast<void*>(s); }

    //
    // IsMulticast — true if the address falls in the 224.0.0.0/4 range.
    //
    bool IsMulticast(const sockaddr_in& a)
    {
        const unsigned long ip = ::ntohl(a.sin_addr.s_addr);
        return (ip & 0xF0000000u) == 0xE0000000u; // 224.0.0.0/4
    }
}

UdpSender::UdpSender() = default;

//
// ~UdpSender — closes the socket and tears down Winsock.
//
UdpSender::~UdpSender()
{
    Close();
}

//
// SetMulticastOptions — stores TTL/interface/loopback and forces them to be
//   re-applied on the next multicast Send.
//
void UdpSender::SetMulticastOptions(int ttl, const std::string& iface, bool loopback)
{
    m_multicastTtl   = ttl;
    m_multicastIface = iface;
    m_multicastLoop  = loopback;
    // Force re-apply next time we send to a multicast address.
    m_multicastApplied = false;
}

//
// Send — lazily starts Winsock and opens the socket, resolves host:port,
//   applies multicast options once for multicast destinations, and sends the
//   datagram. Returns bytes sent, or 0 on any failure (logged via LOG).
//
int UdpSender::Send(const std::string& host, uint16_t port,
                    const void* data, size_t length)
{
    m_lastError.clear();

    if (!m_wsaStarted)
    {
        WSADATA wsa{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0)
        {
            sprintf_s(szError, sizeof(szError), "WSAStartup failed: %d", rc);
            m_lastError = szError;
            LOG(szError);
            return 0;
        }
        m_wsaStarted = true;
    }

    if (!m_socket)
    {
        const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
        {
            sprintf_s(szError, sizeof(szError),
                      "UDP socket() failed: %d", ::WSAGetLastError());
            m_lastError = szError;
            LOG(szError);
            return 0;
        }
        m_socket = FromSocket(s);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = ::htons(port);
    if (::InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        sprintf_s(szError, sizeof(szError),
                  // ASCII only: this string reaches a MessageBox via CA2T, which
                  // converts through the ANSI codepage and mangles non-ASCII.
                  "UDP InetPton('%s') failed - not a valid IPv4 address", host.c_str());
        m_lastError = szError;
        LOG(szError);
        return 0;
    }

    if (IsMulticast(addr) && !m_multicastApplied)
    {
        const SOCKET s = ToSocket(m_socket);

        // TTL is a single byte on the wire; reject out-of-range values here so
        // setsockopt doesn't fail opaquely on e.g. a UI-entered 9999.
        if (m_multicastTtl < 0 || m_multicastTtl > 255)
        {
            m_lastError = "multicast TTL out of range (must be 0..255)";
            sprintf_s(szError, sizeof(szError),
                      "UDP multicast TTL %d out of range (0..255)", m_multicastTtl);
            LOG(szError);
            return 0;
        }

        const DWORD ttl  = static_cast<DWORD>(m_multicastTtl);
        const DWORD loop = m_multicastLoop ? 1u : 0u;

        if (::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL,
                         reinterpret_cast<const char*>(&ttl), sizeof(ttl)) == SOCKET_ERROR)
        {
            const int err = ::WSAGetLastError();
            sprintf_s(szError, sizeof(szError),
                      "setsockopt(IP_MULTICAST_TTL=%d) failed: %d", m_multicastTtl, err);
            m_lastError = szError;
            LOG(szError);
            return 0;
        }

        if (::setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP,
                         reinterpret_cast<const char*>(&loop), sizeof(loop)) == SOCKET_ERROR)
        {
            const int err = ::WSAGetLastError();
            sprintf_s(szError, sizeof(szError),
                      "setsockopt(IP_MULTICAST_LOOP=%u) failed: %d", loop, err);
            m_lastError = szError;
            LOG(szError);
            return 0;
        }

        // Select the outgoing interface. The socket outlives a single run, so
        // an explicit reset to INADDR_ANY is required when the user clears the
        // field back to 0.0.0.0 — merely skipping the call would leave the
        // socket pinned to whichever NIC a previous run selected.
        in_addr ifaddr{};
        ifaddr.s_addr = INADDR_ANY;
        if (!m_multicastIface.empty() && m_multicastIface != "0.0.0.0" &&
            ::InetPtonA(AF_INET, m_multicastIface.c_str(), &ifaddr) != 1)
        {
            sprintf_s(szError, sizeof(szError),
                      "UDP multicast interface '%s' is not a valid IPv4 address",
                      m_multicastIface.c_str());
            m_lastError = szError;
            LOG(szError);
            return 0;
        }

        if (::setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
                         reinterpret_cast<const char*>(&ifaddr), sizeof(ifaddr)) == SOCKET_ERROR)
        {
            const int err = ::WSAGetLastError();
            sprintf_s(szError, sizeof(szError),
                      // ASCII only - see note above; this surfaces in a MessageBox.
                      "setsockopt(IP_MULTICAST_IF=%s) failed: %d - no such local interface?",
                      m_multicastIface.c_str(), err);
            m_lastError = szError;
            LOG(szError);
            return 0;
        }

        sprintf_s(szError, sizeof(szError),
                  "UDP multicast options applied: TTL=%d loopback=%d if=%s",
                  m_multicastTtl, m_multicastLoop ? 1 : 0,
                  m_multicastIface.c_str());
        LOG(szError);
        m_multicastApplied = true;
    }

    const int sent = ::sendto(ToSocket(m_socket),
                              static_cast<const char*>(data),
                              static_cast<int>(length),
                              0,
                              reinterpret_cast<const sockaddr*>(&addr),
                              sizeof(addr));
    if (sent == SOCKET_ERROR)
    {
        sprintf_s(szError, sizeof(szError),
                  "UDP sendto(%s:%u, %zu B) failed: %d",
                  host.c_str(), port, length, ::WSAGetLastError());
        m_lastError = szError;
        LOG(szError);
        return 0;
    }
    return sent;
}

//
// Close — closes the socket (if open) and calls WSACleanup (if started).
//
void UdpSender::Close()
{
    if (m_socket)
    {
        ::closesocket(ToSocket(m_socket));
        m_socket = nullptr;
    }
    if (m_wsaStarted)
    {
        ::WSACleanup();
        m_wsaStarted = false;
    }
}
