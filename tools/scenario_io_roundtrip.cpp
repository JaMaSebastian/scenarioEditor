// Round-trip test for ScenarioIO::Save / ScenarioIO::Load.
// Built via CMake as the `scenario_io_roundtrip` target. Exits 0 on pass.

#include "Scenario.h"
#include "ScenarioIO.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    int failures = 0;

#define EXPECT_EQ(a, b, what) do {                                              \
        if (!((a) == (b))) {                                                    \
            std::printf("  FAIL %s: %s != %s\n", what, #a, #b);                 \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

#define EXPECT_NEAR(a, b, tol, what) do {                                       \
        if (std::abs((a) - (b)) > (tol)) {                                      \
            std::printf("  FAIL %s: %.9f != %.9f (delta %.3e, tol %.3e)\n",     \
                        what, (double)(a), (double)(b),                         \
                        (double)std::abs((a) - (b)), (double)(tol));            \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

    std::wstring TempIniPath()
    {
        wchar_t dir[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, dir);
        std::wstring out = dir;
        out += L"ScenarioEditorIOTest\\scenario.ini";
        return out;
    }
}

int main()
{
    Scenario s;
    s.name             = "Round Trip Test";
    s.description      = "fixture written by scenario_io_roundtrip";
    s.protocolVersion  = 7;
    s.exerciseId       = 42;
    s.siteId           = 7;
    s.applicationId    = 11;
    s.durationSeconds  = 600.0;
    s.defaultUpdateRateHz = 10.0;
    s.defaultCoordMode = CoordMode::LatLonAlt;
    s.originLatDeg     = 39.0458;
    s.originLonDeg     = -76.6413;
    s.originAltM       = 12.5;

    // Multi-entity fixture so we exercise the per-entity Motion section
    // discovery path. Entity 101 carries four mixed motion segments; entity
    // 7 has none (validates the empty-vector branch).
    s.entities.clear();
    s.entities.push_back(Entity{});
    s.entities.push_back(Entity{});

    Entity& e2 = s.entities[1];
    e2.name     = "Quiet Buddy";
    e2.entityId = 7;
    e2.marking  = "BUDDY-07";

    Entity& e = s.entities[0];
    e.enabled        = false;
    e.name           = "Drone 1";
    e.description    = "fixture entity";
    e.siteId         = 1;
    e.applicationId  = 1;
    e.entityId       = 101;
    e.forceId        = 2; // Opposing
    e.kind           = 1;
    e.domain         = 2;
    e.country        = 225;
    e.category       = 1;
    e.subcategory    = 2;
    e.specific       = 3;
    e.extra          = 4;
    e.marking        = "OPF-101";
    e.beginSecond    = 5.0;
    e.endSecond      = 250.5;
    e.updateRateHz   = 5.0;
    e.initialCoordMode = CoordMode::ECEF;
    e.lat = 51.5074; e.lon = 0.1278;  e.alt = 25.0;
    e.localX = 1.0;  e.localY = -2.0; e.localZ = 3.0;
    e.ecefX = 3978009.83; e.ecefY = 8873.09; e.ecefZ = 4968894.50;
    e.headingDeg = 90.0; e.pitchDeg = 0.0; e.rollDeg = 0.0;

    // ---- Motion segments: one of each type (§11.2 round-trip) ----
    {
        MotionSegment m;
        m.type = MotionType::Stationary;
        m.startSecond = 0.0;  m.endSecond = 30.0;
        m.coordMode = CoordMode::LatLonAlt;
        m.startLat = 39.0458; m.startLon = -76.6413; m.startAlt = 500.0;
        m.startHeadingDeg = 45.0;
        m.description = "Hold at point A";
        e.motionSegments.push_back(m);
    }
    {
        MotionSegment m;
        m.type = MotionType::Line;
        m.startSecond = 30.0; m.endSecond = 120.0;
        m.coordMode = CoordMode::LatLonAlt;
        m.startLat = 39.0458; m.startLon = -76.6413; m.startAlt = 500.0;
        m.endLat   = 39.0550; m.endLon   = -76.6300; m.endAlt   = 500.0;
        m.startHeadingDeg = 45.0;   m.endHeadingDeg = 90.0;
        m.speedMode   = "CalculateFromTime";
        m.headingMode = "CalculateFromPath";
        m.description = "Transit A -> B"; // ASCII; ANSI INI files don't preserve U+2192
        e.motionSegments.push_back(m);
    }
    {
        MotionSegment m;
        m.type = MotionType::Ellipse;
        m.startSecond = 120.0; m.endSecond = 240.0;
        m.coordMode = CoordMode::LatLonAlt;
        m.centerLat = 39.0550; m.centerLon = -76.6300; m.centerAlt = 500.0;
        m.radiusXMeters = 2000.0;
        m.radiusYMeters = 1000.0;
        m.rotationDeg   = 15.0;
        m.startAngleDeg = 0.0;
        m.direction     = EllipseDirection::Clockwise;
        m.periodSeconds = 120.0;
        m.altitudeMode  = AltitudeMode::Constant;
        m.description   = "Orbit B";
        e.motionSegments.push_back(m);
    }
    {
        MotionSegment m;
        m.type = MotionType::StopHold;
        m.startSecond = 240.0; m.endSecond = 300.0;
        m.description = "Park";
        e.motionSegments.push_back(m);
    }

    const std::wstring path = TempIniPath();
    std::printf("Saving to %S\n", path.c_str());
    const ScenarioIO::Result wrc = ScenarioIO::Save(s, path);
    if (wrc != ScenarioIO::Result::Ok) {
        std::printf("FAIL Save returned %d\n", (int)wrc);
        return 1;
    }

    Scenario loaded;
    const ScenarioIO::Result rrc = ScenarioIO::Load(loaded, path);
    if (rrc != ScenarioIO::Result::Ok) {
        std::printf("FAIL Load returned %d\n", (int)rrc);
        return 1;
    }

    // ---- Scenario-level ----
    EXPECT_EQ(loaded.name,            s.name,            "Scenario.Name");
    EXPECT_EQ(loaded.description,     s.description,     "Scenario.Description");
    EXPECT_EQ(loaded.protocolVersion, s.protocolVersion, "DISVersion");
    EXPECT_EQ((int)loaded.exerciseId, (int)s.exerciseId, "ExerciseID");
    EXPECT_EQ(loaded.siteId,          s.siteId,          "SiteID");
    EXPECT_EQ(loaded.applicationId,   s.applicationId,   "ApplicationID");
    EXPECT_NEAR(loaded.durationSeconds, s.durationSeconds, 1e-9, "DurationSeconds");
    EXPECT_NEAR(loaded.defaultUpdateRateHz, s.defaultUpdateRateHz, 1e-9, "DefaultUpdateRateHz");
    EXPECT_EQ((int)loaded.defaultCoordMode, (int)s.defaultCoordMode, "DefaultCoordinateMode");
    EXPECT_NEAR(loaded.originLatDeg, s.originLatDeg, 1e-9, "Origin.Lat");
    EXPECT_NEAR(loaded.originLonDeg, s.originLonDeg, 1e-9, "Origin.Lon");
    EXPECT_NEAR(loaded.originAltM,   s.originAltM,   1e-9, "Origin.Alt");

    // ---- Entity ----
    if (loaded.entities.size() != 2) {
        std::printf("  FAIL entities.size(): expected 2 got %zu\n", loaded.entities.size());
        ++failures;
    } else {
        // Pick the entity with entityId == 101 (insertion order may differ
        // since Load discovers sections via the OS string table).
        const Entity* found = nullptr;
        for (const auto& en : loaded.entities) if (en.entityId == 101) { found = &en; break; }
        if (!found) {
            std::printf("  FAIL: entity 101 not found after load\n"); ++failures;
            goto SkipDetail;
        }
        const Entity& le = *found;
        EXPECT_EQ(le.enabled,       e.enabled,       "Entity.Enabled");
        EXPECT_EQ(le.name,          e.name,          "Entity.Name");
        EXPECT_EQ(le.description,   e.description,   "Entity.Description");
        EXPECT_EQ(le.siteId,        e.siteId,        "Entity.SiteID");
        EXPECT_EQ(le.applicationId, e.applicationId, "Entity.ApplicationID");
        EXPECT_EQ(le.entityId,      e.entityId,      "Entity.EntityID");
        EXPECT_EQ((int)le.forceId,  (int)e.forceId,  "Entity.ForceID");
        EXPECT_EQ((int)le.kind,        (int)e.kind,        "Entity.Kind");
        EXPECT_EQ((int)le.domain,      (int)e.domain,      "Entity.Domain");
        EXPECT_EQ(le.country,          e.country,          "Entity.Country");
        EXPECT_EQ((int)le.category,    (int)e.category,    "Entity.Category");
        EXPECT_EQ((int)le.subcategory, (int)e.subcategory, "Entity.Subcategory");
        EXPECT_EQ((int)le.specific,    (int)e.specific,    "Entity.Specific");
        EXPECT_EQ((int)le.extra,       (int)e.extra,       "Entity.Extra");
        EXPECT_EQ(le.marking,          e.marking,          "Entity.Marking");
        EXPECT_NEAR(le.beginSecond,    e.beginSecond,  1e-9, "Entity.BeginSecond");
        EXPECT_NEAR(le.endSecond,      e.endSecond,    1e-9, "Entity.EndSecond");
        EXPECT_NEAR(le.updateRateHz,   e.updateRateHz, 1e-9, "Entity.UpdateRateHz");
        EXPECT_EQ((int)le.initialCoordMode, (int)e.initialCoordMode, "Entity.InitialCoordinateMode");
        EXPECT_NEAR(le.lat, e.lat, 1e-9, "Entity.Lat");
        EXPECT_NEAR(le.lon, e.lon, 1e-9, "Entity.Lon");
        EXPECT_NEAR(le.alt, e.alt, 1e-9, "Entity.Alt");
        EXPECT_NEAR(le.ecefX, e.ecefX, 1e-3, "Entity.EcefX");
        EXPECT_NEAR(le.ecefY, e.ecefY, 1e-3, "Entity.EcefY");
        EXPECT_NEAR(le.ecefZ, e.ecefZ, 1e-3, "Entity.EcefZ");

        // ---- Motion segments ----
        if (le.motionSegments.size() != e.motionSegments.size()) {
            std::printf("  FAIL motionSegments.size(): expected %zu got %zu\n",
                        e.motionSegments.size(), le.motionSegments.size());
            ++failures;
        } else {
            for (size_t i = 0; i < e.motionSegments.size(); ++i) {
                const MotionSegment& a = e.motionSegments[i];
                const MotionSegment& b = le.motionSegments[i];
                char tag[32]; std::snprintf(tag, sizeof(tag), "Motion[%zu]", i);
                std::string T(tag);

                EXPECT_EQ((int)b.type, (int)a.type, (T + ".type").c_str());
                EXPECT_EQ(b.enabled,    a.enabled,  (T + ".enabled").c_str());
                EXPECT_NEAR(b.startSecond, a.startSecond, 1e-9, (T + ".startSecond").c_str());
                EXPECT_NEAR(b.endSecond,   a.endSecond,   1e-9, (T + ".endSecond").c_str());
                EXPECT_EQ((int)b.coordMode, (int)a.coordMode, (T + ".coordMode").c_str());

                EXPECT_NEAR(b.startLat, a.startLat, 1e-9, (T + ".startLat").c_str());
                EXPECT_NEAR(b.startLon, a.startLon, 1e-9, (T + ".startLon").c_str());
                EXPECT_NEAR(b.startAlt, a.startAlt, 1e-9, (T + ".startAlt").c_str());
                EXPECT_NEAR(b.startHeadingDeg, a.startHeadingDeg, 1e-9, (T + ".startHeading").c_str());
                EXPECT_NEAR(b.endLat,   a.endLat,   1e-9, (T + ".endLat").c_str());
                EXPECT_NEAR(b.endLon,   a.endLon,   1e-9, (T + ".endLon").c_str());
                EXPECT_NEAR(b.endAlt,   a.endAlt,   1e-9, (T + ".endAlt").c_str());

                EXPECT_NEAR(b.centerLat,     a.centerLat,     1e-9, (T + ".centerLat").c_str());
                EXPECT_NEAR(b.centerLon,     a.centerLon,     1e-9, (T + ".centerLon").c_str());
                EXPECT_NEAR(b.centerAlt,     a.centerAlt,     1e-9, (T + ".centerAlt").c_str());
                EXPECT_NEAR(b.radiusXMeters, a.radiusXMeters, 1e-9, (T + ".radiusX").c_str());
                EXPECT_NEAR(b.radiusYMeters, a.radiusYMeters, 1e-9, (T + ".radiusY").c_str());
                EXPECT_NEAR(b.rotationDeg,   a.rotationDeg,   1e-9, (T + ".rotationDeg").c_str());
                EXPECT_NEAR(b.startAngleDeg, a.startAngleDeg, 1e-9, (T + ".startAngleDeg").c_str());
                EXPECT_EQ((int)b.direction,  (int)a.direction,     (T + ".direction").c_str());
                EXPECT_NEAR(b.periodSeconds, a.periodSeconds, 1e-9, (T + ".period").c_str());
                EXPECT_EQ((int)b.altitudeMode, (int)a.altitudeMode, (T + ".altitudeMode").c_str());

                EXPECT_EQ(b.speedMode,   a.speedMode,   (T + ".speedMode").c_str());
                EXPECT_EQ(b.headingMode, a.headingMode, (T + ".headingMode").c_str());
                EXPECT_EQ(b.description, a.description, (T + ".description").c_str());
            }
        }
    SkipDetail:;
    }

    if (failures == 0) {
        std::printf("\nRound-trip PASS\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED\n", failures);
    return 1;
}
