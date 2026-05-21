#pragma once

#include <cstddef>
#include <vector>

#include <dis7/EntityStatePdu.h>

struct Scenario;
struct Entity;

namespace PduBuilder
{
    // Build a DIS v7 Entity State PDU populated from the scenario + entity.
    // Caller is expected to have set Scenario::protocolVersion to 7 (the
    // only value supported by this slice).
    DIS::EntityStatePdu BuildEntityStatePdu(const Scenario& scenario, const Entity& entity);

    // Marshal a PDU into a big-endian wire-format byte buffer. The buffer
    // is resized to fit. Returns the number of bytes written.
    size_t Serialize(const DIS::Pdu& pdu, std::vector<unsigned char>& outBuffer);
}
