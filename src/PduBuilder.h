//=============================================================================
//  PduBuilder.h
//-----------------------------------------------------------------------------
//  Declares the PduBuilder namespace: helpers that construct a DIS v7 Entity
//  State PDU from a Scenario/Entity (with legacy or explicit ECEF velocity) and
//  marshal a PDU into a big-endian wire-format byte buffer.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

#include <cstddef>
#include <vector>

#include <dis7/EntityStatePdu.h>
#include <dis7/DetonationPdu.h>

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

    // Build a DIS v7 Detonation PDU for `exploding` blowing up at ECEF `ecefM`
    // (explosion.md section 3). The exploding entity is the thing that detonates,
    // so its own site:app:entity goes in Exploding Entity ID; `target` (may be
    // null) fills Target Entity ID for other DIS tools. Event ID is the
    // scenario's site:app plus `eventNumber`. Result 1 = Entity Impact.
    DIS::DetonationPdu BuildDetonationPdu(const Scenario& scenario, const Entity& exploding,
                                          const Entity* target, const double ecefM[3],
                                          const double velocityMps[3],
                                          unsigned short eventNumber);

    // Marshal a PDU into a big-endian wire-format byte buffer. The buffer
    // is resized to fit. Returns the number of bytes written.
    // Populates the PDU header Length field as a side effect — open-dis7-cpp
    // does not auto-compute it, and leaving it 0 produces malformed wire packets.
    size_t Serialize(DIS::Pdu& pdu, std::vector<unsigned char>& outBuffer);
}
