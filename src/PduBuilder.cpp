//=============================================================================
//  PduBuilder.cpp
//-----------------------------------------------------------------------------
//  Implements PduBuilder: fills a DIS v7 Entity State PDU (IDs, type, ECEF
//  location/velocity/orientation, marking) from the scenario and entity, and
//  serializes a PDU to a big-endian byte buffer, writing back the header
//  Length field that open-dis7-cpp does not compute automatically.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#include "pch.h"
#include "PduBuilder.h"
#include "Scenario.h"

#include <cmath>

#include <dis7/EntityID.h>
#include <dis7/EntityType.h>
#include <dis7/EntityMarking.h>
#include <dis7/SimulationAddress.h>
#include <dis7/Vector3Double.h>
#include <dis7/Vector3Float.h>
#include <dis7/EulerAngles.h>
#include <dis7/DeadReckoningParameters.h>
#include <dis7/utils/DataStream.h>

//
// BuildEntityStatePdu (1-arg) — derives an ECEF velocity from
//   entity.initialSpeedMps along the body-forward axis, then delegates to the
//   explicit-velocity overload.
//
DIS::EntityStatePdu PduBuilder::BuildEntityStatePdu(const Scenario& scenario, const Entity& entity)
{
    // Legacy velocity: initialSpeedMps along the body-forward ECEF axis. The
    // entity's body-forward vector in ECEF is the first column of
    // R_body_to_ECEF; for ZYX Tait-Bryan (yaw=psi/pitch=theta/roll=phi) that is
    //   (cos(theta)*cos(psi), cos(theta)*sin(psi), -sin(theta)).
    double vx = 0.0, vy = 0.0, vz = 0.0;
    if (entity.initialSpeedMps != 0.0)
    {
        const double cPsi   = std::cos(entity.psi);
        const double sPsi   = std::sin(entity.psi);
        const double cTheta = std::cos(entity.theta);
        const double sTheta = std::sin(entity.theta);
        vx = entity.initialSpeedMps * cTheta * cPsi;
        vy = entity.initialSpeedMps * cTheta * sPsi;
        vz = entity.initialSpeedMps * (-sTheta);
    }
    return BuildEntityStatePdu(scenario, entity, vx, vy, vz);
}

//
// BuildEntityStatePdu (4-arg) — populates the PDU header, entity ID/type,
//   force, ECEF location, the caller-supplied ECEF linear velocity,
//   orientation, and marking, and returns the completed PDU.
//
DIS::EntityStatePdu PduBuilder::BuildEntityStatePdu(const Scenario& scenario, const Entity& entity,
                                                    double velX, double velY, double velZ)
{
    DIS::EntityStatePdu pdu;

    // Header. PduSuperclass / Pdu set ProtocolVersion = 7 and PduType = 1
    // (Entity State) in their constructors when built against DIS v7; we
    // still set ExerciseID + protocol family explicitly to be safe.
    pdu.setExerciseID(scenario.exerciseId);
    pdu.setProtocolFamily(1); // Entity Information / Interaction
    pdu.setPduType(1);        // Entity State

    DIS::SimulationAddress simAddr;
    simAddr.setSite(entity.siteId);
    simAddr.setApplication(entity.applicationId);

    DIS::EntityID id;
    id.setSimulationAddress(simAddr);
    id.setEntityNumber(entity.entityId);
    pdu.setEntityID(id);

    pdu.setForceId(entity.forceId);

    DIS::EntityType type;
    type.setEntityKind(entity.kind);
    type.setDomain(entity.domain);
    type.setCountry(entity.country);
    type.setCategory(entity.category);
    type.setSubcategory(entity.subcategory);
    type.setSpecific(entity.specific);
    type.setExtra(entity.extra);
    pdu.setEntityType(type);
    pdu.setAlternativeEntityType(type);

    DIS::Vector3Double location;
    location.setX(entity.ecefX);
    location.setY(entity.ecefY);
    location.setZ(entity.ecefZ);
    pdu.setEntityLocation(location);

    // Linear velocity in ECEF (m/s), supplied by the caller. Playback passes
    // the instantaneous tangent velocity so the wire reflects real motion; the
    // 1-arg overload passes the legacy initialSpeedMps-derived vector.
    DIS::Vector3Float velocity;
    velocity.setX(static_cast<float>(velX));
    velocity.setY(static_cast<float>(velY));
    velocity.setZ(static_cast<float>(velZ));
    pdu.setEntityLinearVelocity(velocity);

    // Declare how receivers may extrapolate between PDUs. The library default
    // is 0 ("Other"), which every DIS receiver treats as "do not dead reckon",
    // so at 5 Hz entities step five times a second in the visualizer. FPW
    // (2, fixed-orientation, constant world velocity) matches the velocity
    // supplied above and lets the receiver glide between samples.
    DIS::DeadReckoningParameters deadReckoning;
    deadReckoning.setDeadReckoningAlgorithm(2);
    pdu.setDeadReckoningParameters(deadReckoning);

    DIS::EulerAngles orientation;
    orientation.setPsi(entity.psi);
    orientation.setTheta(entity.theta);
    orientation.setPhi(entity.phi);
    pdu.setEntityOrientation(orientation);

    DIS::EntityMarking marking;
    marking.setCharacterSet(1); // ASCII
    marking.setByStringCharacters(entity.marking.c_str());
    pdu.setMarking(marking);

    pdu.setEntityAppearance(0);
    pdu.setCapabilities(0);

    return pdu;
}

//
// BuildDetonationPdu — see header. The visualizer reads only the exercise ID,
//   the exploding entity ID and the world location; the rest is filled so other
//   DIS tools read the event correctly (explosion.md section 3.3).
//
DIS::DetonationPdu PduBuilder::BuildDetonationPdu(const Scenario& scenario, const Entity& exploding,
                                                  const Entity* target, const double ecefM[3],
                                                  const double velocityMps[3],
                                                  unsigned short eventNumber)
{
    DIS::DetonationPdu pdu;
    pdu.setExerciseID(scenario.exerciseId);
    pdu.setProtocolFamily(2); // Warfare
    pdu.setPduType(3);        // Detonation

    auto makeId = [](uint16_t site, uint16_t app, uint16_t num)
    {
        DIS::SimulationAddress a;
        a.setSite(site);
        a.setApplication(app);
        DIS::EntityID id;
        id.setSimulationAddress(a);
        id.setEntityNumber(num);
        return id;
    };

    // "Kill X" is ExplodingEntityID = X, with X's own site/app (explosion.md 1).
    pdu.setExplodingEntityID(makeId(exploding.siteId, exploding.applicationId, exploding.entityId));
    pdu.setFiringEntityID(makeId(0, 0, 0));
    pdu.setTargetEntityID(target ? makeId(target->siteId, target->applicationId, target->entityId)
                                 : makeId(0, 0, 0));

    DIS::SimulationAddress eventSite;
    eventSite.setSite(scenario.siteId);
    eventSite.setApplication(scenario.applicationId);
    DIS::EventIdentifier eventId;
    eventId.setSimulationAddress(eventSite);
    eventId.setEventNumber(eventNumber);
    pdu.setEventID(eventId);

    DIS::Vector3Float velocity;
    velocity.setX(static_cast<float>(velocityMps[0]));
    velocity.setY(static_cast<float>(velocityMps[1]));
    velocity.setZ(static_cast<float>(velocityMps[2]));
    pdu.setVelocity(velocity);

    DIS::Vector3Double location;
    location.setX(ecefM[0]);
    location.setY(ecefM[1]);
    location.setZ(ecefM[2]);
    pdu.setLocationInWorldCoordinates(location);

    // Munition descriptor: the exploding entity IS the munition, so describe it
    // with its own entity type, one round, no warhead/fuse codes.
    DIS::EntityType munitionType;
    munitionType.setEntityKind(exploding.kind);
    munitionType.setDomain(exploding.domain);
    munitionType.setCountry(exploding.country);
    munitionType.setCategory(exploding.category);
    munitionType.setSubcategory(exploding.subcategory);
    munitionType.setSpecific(exploding.specific);
    munitionType.setExtra(exploding.extra);
    DIS::MunitionDescriptor descriptor;
    descriptor.setMunitionType(munitionType);
    descriptor.setWarhead(0);
    descriptor.setFuse(0);
    descriptor.setQuantity(1);
    descriptor.setRate(0);
    pdu.setDescriptor(descriptor);

    DIS::Vector3Float entityLocation;   // 0,0,0: relative to the target's origin
    pdu.setLocationOfEntityCoordinates(entityLocation);
    pdu.setDetonationResult(target ? 1 : 5);   // 1 Entity Impact, 5 Detonation

    return pdu;
}

//
// Serialize — marshals the PDU big-endian, writing the header Length field back
//   before the final marshal; resizes outBuffer to fit and returns byte count.
//
size_t PduBuilder::Serialize(DIS::Pdu& pdu, std::vector<unsigned char>& outBuffer)
{
    // DIS wire format is big-endian per IEEE 1278.1.
    // open-dis7-cpp does not auto-compute the header Length field, so marshal
    // once to measure, write the length back, then marshal again for real.
    {
        DIS::DataStream sizing(DIS::BIG);
        pdu.marshal(sizing);
        pdu.setLength(static_cast<unsigned short>(sizing.size()));
    }

    DIS::DataStream stream(DIS::BIG);
    pdu.marshal(stream);

    const size_t n = stream.size();
    outBuffer.resize(n);
    for (size_t i = 0; i < n; ++i)
        outBuffer[i] = static_cast<unsigned char>(stream[static_cast<unsigned int>(i)]);
    return n;
}
