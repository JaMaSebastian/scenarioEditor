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
    // only value supported by this slice). This 1-arg form derives the linear
    // velocity from entity.initialSpeedMps along the body-forward axis (legacy).
    DIS::EntityStatePdu BuildEntityStatePdu(const Scenario& scenario, const Entity& entity);

    // Same, but with an explicit ECEF linear velocity (m/s) — used by playback
    // so each PDU carries the entity's instantaneous velocity (tangent to the
    // path) instead of the static t=0 value.
    DIS::EntityStatePdu BuildEntityStatePdu(const Scenario& scenario, const Entity& entity,
                                            double velX, double velY, double velZ);

    // Marshal a PDU into a big-endian wire-format byte buffer. The buffer
    // is resized to fit. Returns the number of bytes written.
    // Populates the PDU header Length field as a side effect — open-dis7-cpp
    // does not auto-compute it, and leaving it 0 produces malformed wire packets.
    size_t Serialize(DIS::Pdu& pdu, std::vector<unsigned char>& outBuffer);
}
