//=============================================================================
//  DisrecReader.h
//-----------------------------------------------------------------------------
//  Declares DisrecReader, a sequential reader for .disrec DIS recording files.
//  Validates the 12-byte header on Open() and hands back one timestamped PDU
//  record at a time via NextRecord(), with Rewind() to replay from the start.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

//-----------------------------------------------------------------------------
// DisrecReader — sequential reader for .disrec recording files
//   Owns the FILE handle, validates the format header, and iterates records
//   (timestamp + raw DIS PDU bytes). Non-copyable; closes the file on destroy.
//-----------------------------------------------------------------------------
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
