#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// Writes a .disrec file (§13 / Disrec::format). Open() truncates and emits
// the 12-byte header; WriteRecord() appends a (timestampUs, len, bytes)
// record. Close() flushes the FILE.

class DisrecWriter
{
public:
    DisrecWriter() = default;
    ~DisrecWriter();

    DisrecWriter(const DisrecWriter&)            = delete;
    DisrecWriter& operator=(const DisrecWriter&) = delete;

    bool Open(const std::string& utf8Path);
    bool IsOpen() const { return m_fp != nullptr; }

    // Append a single PDU record. Returns false on I/O failure (and logs).
    bool WriteRecord(uint64_t timestampUs, const void* pduBytes, size_t pduLen);

    void Close();

    uint64_t RecordCount() const { return m_recordCount; }

private:
    std::FILE* m_fp = nullptr;
    uint64_t   m_recordCount = 0;
};
