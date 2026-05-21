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

    bool IsMulticast(const sockaddr_in& a)
    {
        const unsigned long ip = ::ntohl(a.sin_addr.s_addr);
        return (ip & 0xF0000000u) == 0xE0000000u; // 224.0.0.0/4
    }
}

UdpSender::UdpSender() = default;

UdpSender::~UdpSender()
{
    Close();
}

void UdpSender::SetMulticastOptions(int ttl, const std::string& iface, bool loopback)
{
    m_multicastTtl   = ttl;
    m_multicastIface = iface;
    m_multicastLoop  = loopback;
    // Force re-apply next time we send to a multicast address.
    m_multicastApplied = false;
}

int UdpSender::Send(const std::string& host, uint16_t port,
                    const void* data, size_t length)
{
    if (!m_wsaStarted)
    {
        WSADATA wsa{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0)
        {
            sprintf_s(szError, sizeof(szError), "WSAStartup failed: %d", rc);
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
                  "UDP InetPton('%s') failed", host.c_str());
        LOG(szError);
        return 0;
    }

    if (IsMulticast(addr) && !m_multicastApplied)
    {
        const SOCKET s = ToSocket(m_socket);

        const DWORD ttl  = static_cast<DWORD>(m_multicastTtl);
        const DWORD loop = m_multicastLoop ? 1u : 0u;
        ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL,
                     reinterpret_cast<const char*>(&ttl), sizeof(ttl));
        ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP,
                     reinterpret_cast<const char*>(&loop), sizeof(loop));

        // Bind the outgoing interface if the user named one explicitly.
        if (!m_multicastIface.empty() && m_multicastIface != "0.0.0.0")
        {
            in_addr ifaddr{};
            if (::InetPtonA(AF_INET, m_multicastIface.c_str(), &ifaddr) == 1)
                ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
                             reinterpret_cast<const char*>(&ifaddr), sizeof(ifaddr));
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
        LOG(szError);
        return 0;
    }
    return sent;
}

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
