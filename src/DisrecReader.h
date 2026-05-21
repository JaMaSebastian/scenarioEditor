#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class DisrecReader
{
public:
    DisrecReader() = default;
    ~DisrecReader();

    DisrecReader(const DisrecReader&)            = delete;
    DisrecReader& operator=(const DisrecReader&) = delete;

    // Open + validate the 12-byte header. Returns false (and logs) if the
    // magic, version, or header length is wrong.
    bool Open(const std::string& utf8Path);
    bool IsOpen() const { return m_fp != nullptr; }

    // Read the next record into outTimestampUs + outBytes (resized). Returns
    // false on EOF or I/O error.
    bool NextRecord(uint64_t& outTimestampUs, std::vector<unsigned char>& outBytes);

    void Rewind();
    void Close();

private:
    std::FILE* m_fp = nullptr;
};
