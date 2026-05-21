#include "pch.h"
#include "PduBuilder.h"
#include "Scenario.h"

#include <dis7/EntityID.h>
#include <dis7/EntityType.h>
#include <dis7/EntityMarking.h>
#include <dis7/SimulationAddress.h>
#include <dis7/Vector3Double.h>
#include <dis7/Vector3Float.h>
#include <dis7/EulerAngles.h>
#include <dis7/utils/DataStream.h>

DIS::EntityStatePdu PduBuilder::BuildEntityStatePdu(const Scenario& scenario, const Entity& entity)
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

    DIS::Vector3Float velocity;
    velocity.setX(0.0f);
    velocity.setY(0.0f);
    velocity.setZ(0.0f);
    pdu.setEntityLinearVelocity(velocity);

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

size_t PduBuilder::Serialize(const DIS::Pdu& pdu, std::vector<unsigned char>& outBuffer)
{
    // DIS wire format is big-endian per IEEE 1278.1.
    DIS::DataStream stream(DIS::BIG);
    pdu.marshal(stream);

    const size_t n = stream.size();
    outBuffer.resize(n);
    for (size_t i = 0; i < n; ++i)
        outBuffer[i] = static_cast<unsigned char>(stream[static_cast<unsigned int>(i)]);
    return n;
}
