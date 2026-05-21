#include "DisrecReader.h"
#include "DisrecFormat.h"
#include "../log.h"

#include <cstdio>
#include <cstring>

DisrecReader::~DisrecReader()
{
    Close();
}

bool DisrecReader::Open(const std::string& path)
{
    Close();
    if (fopen_s(&m_fp, path.c_str(), "rb") != 0 || !m_fp) {
        sprintf_s(szError, sizeof(szError),
                  "DisrecReader: could not open %s", path.c_str());
        LOG(szError);
        m_fp = nullptr;
        return false;
    }
    unsigned char header[Disrec::kHeaderBytes] = {};
    if (std::fread(header, 1, Disrec::kHeaderBytes, m_fp) != Disrec::kHeaderBytes) {
        sprintf_s(szError, sizeof(szError),
                  "DisrecReader: header short read on %s", path.c_str());
        LOG(szError);
        Close();
        return false;
    }
    if (std::memcmp(header, Disrec::kMagic, 8) != 0) {
        LOG("DisrecReader: bad magic");
        Close();
        return false;
    }
    const uint16_t v = static_cast<uint16_t>(header[8]) |
                       (static_cast<uint16_t>(header[9]) << 8);
    if (v != Disrec::kVersion) {
        sprintf_s(szError, sizeof(szError),
                  "DisrecReader: unsupported version %u", static_cast<unsigned>(v));
        LOG(szError);
        Close();
        return false;
    }
    return true;
}

bool DisrecReader::NextRecord(uint64_t& outTs, std::vector<unsigned char>& outBytes)
{
    if (!m_fp) return false;
    unsigned char tsBuf[8];
    if (std::fread(tsBuf, 1, 8, m_fp) != 8) return false;
    outTs = 0;
    for (int i = 0; i < 8; ++i) outTs |= (static_cast<uint64_t>(tsBuf[i]) << (8 * i));

    unsigned char lenBuf[4];
    if (std::fread(lenBuf, 1, 4, m_fp) != 4) return false;
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) len |= (static_cast<uint32_t>(lenBuf[i]) << (8 * i));

    outBytes.resize(len);
    if (len && std::fread(outBytes.data(), 1, len, m_fp) != len) return false;
    return true;
}

void DisrecReader::Rewind()
{
    if (m_fp) std::fseek(m_fp, static_cast<long>(Disrec::kHeaderBytes), SEEK_SET);
}

void DisrecReader::Close()
{
    if (m_fp) { std::fclose(m_fp); m_fp = nullptr; }
}
