//=============================================================================
//  DisrecWriter.cpp
//-----------------------------------------------------------------------------
//  Implements DisrecWriter: truncates/creates a .disrec file, writes the
//  12-byte magic/version header, and appends each record with its little-endian
//  timestamp and length framing around the raw DIS PDU bytes.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "DisrecWriter.h"
#include "DisrecFormat.h"
#include "../log.h"

#include <cstdio>
#include <cstring>

//
// ~DisrecWriter — flushes and closes the open file (if any) on destruction.
//
DisrecWriter::~DisrecWriter()
{
    Close();
}

//
// Open — opens utf8Path for binary write (truncating any existing file) and
//   writes the 12-byte header (magic, little-endian version, reserved zeros).
//   Resets the record count. Logs and returns false on open or write failure.
//
bool DisrecWriter::Open(const std::string& path)
{
    Close();
    if (fopen_s(&m_fp, path.c_str(), "wb") != 0 || !m_fp) {
        sprintf_s(szError, sizeof(szError),
                  "DisrecWriter: could not open %s for write", path.c_str());
        LOG(szError);
        m_fp = nullptr;
        return false;
    }

    unsigned char header[Disrec::kHeaderBytes] = {};
    std::memcpy(header, Disrec::kMagic, 8);
    header[8]  = static_cast<unsigned char>(Disrec::kVersion & 0xFF);
    header[9]  = static_cast<unsigned char>((Disrec::kVersion >> 8) & 0xFF);
    header[10] = 0;
    header[11] = 0;
    if (std::fwrite(header, 1, Disrec::kHeaderBytes, m_fp) != Disrec::kHeaderBytes) {
        sprintf_s(szError, sizeof(szError), "DisrecWriter: header write failed");
        LOG(szError);
        Close();
        return false;
    }
    m_recordCount = 0;
    return true;
}

//
// WriteRecord — appends one record: the 8-byte little-endian timestamp, the
//   4-byte little-endian length, then pduLen raw PDU bytes. Rejects lengths
//   above 0xFFFFFFFF, bumps the record count, and returns false on I/O failure.
//
bool DisrecWriter::WriteRecord(uint64_t timestampUs, const void* pduBytes, size_t pduLen)
{
    if (!m_fp) return false;
    if (pduLen > 0xFFFFFFFFu) return false;

    unsigned char tsBuf[8];
    for (int i = 0; i < 8; ++i)
        tsBuf[i] = static_cast<unsigned char>((timestampUs >> (8 * i)) & 0xFF);

    unsigned char lenBuf[4];
    const uint32_t len32 = static_cast<uint32_t>(pduLen);
    for (int i = 0; i < 4; ++i)
        lenBuf[i] = static_cast<unsigned char>((len32 >> (8 * i)) & 0xFF);

    if (std::fwrite(tsBuf, 1, 8, m_fp) != 8) return false;
    if (std::fwrite(lenBuf, 1, 4, m_fp) != 4) return false;
    if (pduLen && std::fwrite(pduBytes, 1, pduLen, m_fp) != pduLen) return false;
    ++m_recordCount;
    return true;
}

//
// Close — flushes buffered data and closes the file handle; safe when not open.
//
void DisrecWriter::Close()
{
    if (m_fp) {
        std::fflush(m_fp);
        std::fclose(m_fp);
        m_fp = nullptr;
    }
}
