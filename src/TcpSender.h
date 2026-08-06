//=============================================================================
//  TcpSender.h
//-----------------------------------------------------------------------------
//  Declares TcpSender, a lazily-opened IPv4 TCP sink for DIS PDUs (spec §17.4).
//  Supports both roles: client (connect out to a remote host:port) and server
//  (listen on a local port and accept one client). Bytes are written to the
//  stream unframed — each DIS PDU's own header Length field delimits it — so a
//  TCP stream is byte-identical to the datagrams the UDP sinks emit.
//
//  Author:        Matt Sebastian
//  Date started:  2026-08-05
//=============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class TcpSender
{
public:
    TcpSender();
    ~TcpSender();

    TcpSender(const TcpSender&) = delete;
    TcpSender& operator=(const TcpSender&) = delete;

    // Client role: connect out to host:port. Server role: bind listenPort and
    // wait for one client. Call before the first Send; Send() will also apply
    // these lazily if it has to (re)establish the link.
    //   timeoutMs  - connect (client) / accept (server) timeout
    //   reconnect  - client only: allow Send to re-dial after a drop
    void Configure(bool listen,
                   const std::string& remoteHost, uint16_t remotePort,
                   uint16_t listenPort,
                   bool reconnect, int timeoutMs);

    // Establishes the connection per Configure(). Safe to call repeatedly —
    // returns true immediately if already connected. Returns false and sets
    // LastError() on failure.
    bool Connect();

    // Writes the whole buffer, looping over partial sends. Returns bytes
    // written, or 0 on failure. If the peer dropped and reconnect is enabled
    // (client role), one re-dial is attempted before giving up.
    int Send(const void* data, size_t length);

    bool IsConnected() const { return m_socket != nullptr; }

    // Why the last Connect/Send failed; empty after a success.
    const std::string& LastError() const { return m_lastError; }

    // Human-readable description of the current link, for logs and dialogs.
    std::string DescribeEndpoint() const;

    void Close();

private:
    bool EnsureWinsock();
    void CloseSocketOnly();      // drops the data socket, keeps the listener
    bool ConnectAsClient();
    bool ConnectAsServer();

    bool        m_wsaStarted   = false;
    void*       m_socket       = nullptr;   // connected data socket
    void*       m_listenSocket = nullptr;   // server role only

    bool        m_listen       = false;
    std::string m_remoteHost   = "127.0.0.1";
    uint16_t    m_remotePort   = 3002;
    uint16_t    m_listenPort   = 3002;
    bool        m_reconnect    = true;
    int         m_timeoutMs    = 3000;

    std::string m_lastError;
};
