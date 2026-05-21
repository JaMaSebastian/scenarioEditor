#pragma once

#include <cstdint>

// ScenarioEditor .disrec file format (V2).
//
// Header (12 bytes):
//   bytes  0..7   : magic = "DISRECv1"
//   bytes  8..9   : uint16 little-endian version (=1)
//   bytes 10..11  : uint16 little-endian reserved (=0)
//
// Followed by zero or more records:
//   uint64 little-endian timestampMicroseconds      (since scenario start)
//   uint32 little-endian pduLengthBytes
//   pduLengthBytes raw bytes (DIS wire format, big-endian per IEEE 1278.1)
//
// All numeric fields in the framing layer are little-endian (this is
// disk-encoding, not wire-encoding — chosen for x86/x64 host efficiency).
// The PDU payload itself is unchanged DIS big-endian per spec §14.5.

namespace Disrec
{
    constexpr char     kMagic[8]   = { 'D','I','S','R','E','C','v','1' };
    constexpr uint16_t kVersion    = 1;
    constexpr size_t   kHeaderBytes = 12;
}
