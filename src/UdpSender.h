//=============================================================================
//  UdpSender.h
//-----------------------------------------------------------------------------
//  Declares UdpSender, a lazily-opened IPv4 UDP socket wrapper that sends
//  datagrams to a host:port, automatically applying multicast socket options
//  (TTL/interface/loopback) when the destination is in 224.0.0.0/4.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// UDP sender. Single class covers both unicast (the original use case)
// and multicast (with TTL/interface/loopback knobs). The current mode is
// implicit in the destination address: a host in 224.0.0.0/4 triggers the
// multicast socket-options path automatically the first time it's used.

class UdpSender
{
public:
    UdpSender();
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    // Configure the multicast send-side options that apply to *next*
    // Send() targeted at a multicast group. Must be called before the
    // first multicast Send (the socket caches the options on first use).
    // Defaults: TTL=1, interfaceIp="0.0.0.0" (OS-selected), loopback=true.
    void SetMulticastOptions(int ttl, const std::string& interfaceIp, bool loopback);

    // Lazily opens a UDP socket and sends the buffer to host:port. Host
    // is dotted-quad IPv4; if it's in 224.0.0.0/4 the multicast options
    // (TTL/Interface/Loopback) are applied via setsockopt.
    int Send(const std::string& host, uint16_t port,
             const void* data, size_t length);

    // Human-readable reason the last Send returned 0, or empty if the last
    // Send succeeded. Lets callers (e.g. the Network > Test Multicast Send
    // command) report *why* a send failed instead of just "0 bytes".
    const std::string& LastError() const { return m_lastError; }

    void Close();

private:
    bool        m_wsaStarted     = false;
    void*       m_socket         = nullptr;
    int         m_multicastTtl   = 1;
    std::string m_multicastIface = "0.0.0.0";
    bool        m_multicastLoop  = true;
    bool        m_multicastApplied = false;
    std::string m_lastError;
};
