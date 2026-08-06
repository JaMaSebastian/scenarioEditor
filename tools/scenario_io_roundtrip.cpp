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

    // ---- Level terrain overlay (land + ocean footprints) ----
    s.level.enabled        = true;
    s.level.name           = "Test Beach";
    s.level.seaLevelMeters = 0.0;
    s.level.land  = { true, -10000.0, 10000.0,     0.0, 10000.0 };
    s.level.ocean = { true, -10000.0, 10000.0, -5000.0,  5000.0 };
    {
        LevelZone z;
        z.enabled         = true;
        z.name            = "Palm Forest";
        z.eastMinM        = 1.5;
        z.eastMaxM        = 761.5;
        z.northMinM       = -209.75;
        z.northMaxM       = 210.25;
        z.heightMinMeters = 19.6;
        z.heightMaxMeters = 40.14;
        z.colorRgb        = 0x2E7D32;
        s.level.zones.push_back(z);
    }

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
        // Two foci ~3 km apart on an E-W axis → eccentric ellipse.
        m.f1Lat = 39.0550; m.f1Lon = -76.6300; m.f1Alt = 500.0;
        m.f2Lat = 39.0550; m.f2Lon = -76.6125; m.f2Alt = 500.0;
        m.lengthMeters    = 4000.0;   // 2a = 4 km > 2c (~3 km)
        m.startBearingDeg = 90.0;
        m.speedMps        = 100.0;
        m.direction       = EllipseDirection::Clockwise;
        m.description     = "Orbit B";
        e.motionSegments.push_back(m);
    }
    {
        MotionSegment m;
        m.type = MotionType::StopHold;
        m.startSecond = 240.0; m.endSecond = 300.0;
        m.description = "Park";
        e.motionSegments.push_back(m);
    }

    // ---- Camera schedule (now persisted inside scenario.ini as [Camera.N]) ----
    {
        CameraFrame c0;                     // Entity camera mounted on entity 101
        c0.kind           = CameraKind::Entity;
        c0.sourceEntityId = 101;
        c0.presetType     = "Octopus";
        c0.presetAngle    = "front";
        c0.targetEntityId = 7;
        c0.transition     = CameraTransition::HardCut;
        c0.beginSecond    = 0.0;
        c0.endSecond      = 120.5;
        c0.label          = "Drone 1 / Octopus / front";
        c0.zoomEnabled    = true;           // dynamic fit, with cached target size
        c0.dynamicZoom    = true;
        c0.zoomFillPct    = 72.5;
        c0.targetLengthM  = 15.06;
        c0.targetWidthM   = 9.96;
        c0.targetHeightM  = 4.88;
        c0.limitsEnabled  = true;           // enforce the preset's gimbal envelope
        s.cameras.push_back(c0);

        CameraFrame c1;                     // Stationary vantage
        c1.kind        = CameraKind::Stationary;
        c1.vantEastM   = 245.83;
        c1.vantNorthM  = -944.72;
        c1.vantUpM     = 3.5;
        c1.targetEntityId = 101;
        c1.transition  = CameraTransition::CrossfadeBlend;
        c1.beginSecond = 200.0;
        c1.endSecond   = 260.0;
        c1.label       = "Camera 1";
        c1.zoomEnabled = true;              // static FOV override
        c1.zoomFovDeg  = 37.25;
        c1.dynamicZoom = false;
        s.cameras.push_back(c1);
    }

    // ---- Foliage selection ([Foliage] section) ----
    s.foliage.oak        = true;
    s.foliage.bigTrees   = false;
    s.foliage.palm       = true;
    s.foliage.palmKind   = PalmKind::Straight;
    s.foliage.renderMode = FoliageRenderMode::I3dm;

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

    // ---- Level overlay ----
    EXPECT_EQ(loaded.level.enabled, s.level.enabled, "Level.Enabled");
    EXPECT_EQ(loaded.level.name,    s.level.name,    "Level.Name");
    EXPECT_NEAR(loaded.level.seaLevelMeters, s.level.seaLevelMeters, 1e-9, "Level.SeaLevel");
    EXPECT_EQ(loaded.level.land.enabled,  s.level.land.enabled,  "Level.Land.Enabled");
    EXPECT_NEAR(loaded.level.land.eastMinM,  s.level.land.eastMinM,  1e-9, "Level.Land.EastMin");
    EXPECT_NEAR(loaded.level.land.eastMaxM,  s.level.land.eastMaxM,  1e-9, "Level.Land.EastMax");
    EXPECT_NEAR(loaded.level.land.northMinM, s.level.land.northMinM, 1e-9, "Level.Land.NorthMin");
    EXPECT_NEAR(loaded.level.land.northMaxM, s.level.land.northMaxM, 1e-9, "Level.Land.NorthMax");
    EXPECT_EQ(loaded.level.ocean.enabled, s.level.ocean.enabled, "Level.Ocean.Enabled");
    EXPECT_NEAR(loaded.level.ocean.eastMinM,  s.level.ocean.eastMinM,  1e-9, "Level.Ocean.EastMin");
    EXPECT_NEAR(loaded.level.ocean.eastMaxM,  s.level.ocean.eastMaxM,  1e-9, "Level.Ocean.EastMax");
    EXPECT_NEAR(loaded.level.ocean.northMinM, s.level.ocean.northMinM, 1e-9, "Level.Ocean.NorthMin");
    EXPECT_NEAR(loaded.level.ocean.northMaxM, s.level.ocean.northMaxM, 1e-9, "Level.Ocean.NorthMax");

    // ---- Level named zones ----
    EXPECT_EQ(loaded.level.zones.size(), s.level.zones.size(), "Level.ZoneCount");
    if (loaded.level.zones.size() == s.level.zones.size())
    {
        for (size_t i = 0; i < s.level.zones.size(); ++i)
        {
            const LevelZone& a = s.level.zones[i];
            const LevelZone& b = loaded.level.zones[i];
            EXPECT_EQ(b.enabled, a.enabled, "Zone.Enabled");
            EXPECT_EQ(b.name,    a.name,    "Zone.Name");
            EXPECT_NEAR(b.eastMinM,        a.eastMinM,        1e-9, "Zone.EastMin");
            EXPECT_NEAR(b.eastMaxM,        a.eastMaxM,        1e-9, "Zone.EastMax");
            EXPECT_NEAR(b.northMinM,       a.northMinM,       1e-9, "Zone.NorthMin");
            EXPECT_NEAR(b.northMaxM,       a.northMaxM,       1e-9, "Zone.NorthMax");
            EXPECT_NEAR(b.heightMinMeters, a.heightMinMeters, 1e-9, "Zone.HeightMin");
            EXPECT_NEAR(b.heightMaxMeters, a.heightMaxMeters, 1e-9, "Zone.HeightMax");
            EXPECT_EQ(b.colorRgb, a.colorRgb, "Zone.ColorRGB");
        }
    }

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

                EXPECT_NEAR(b.f1Lat,         a.f1Lat,         1e-9, (T + ".f1Lat").c_str());
                EXPECT_NEAR(b.f1Lon,         a.f1Lon,         1e-9, (T + ".f1Lon").c_str());
                EXPECT_NEAR(b.f1Alt,         a.f1Alt,         1e-9, (T + ".f1Alt").c_str());
                EXPECT_NEAR(b.f2Lat,         a.f2Lat,         1e-9, (T + ".f2Lat").c_str());
                EXPECT_NEAR(b.f2Lon,         a.f2Lon,         1e-9, (T + ".f2Lon").c_str());
                EXPECT_NEAR(b.f2Alt,         a.f2Alt,         1e-9, (T + ".f2Alt").c_str());
                EXPECT_NEAR(b.lengthMeters,  a.lengthMeters,  1e-9, (T + ".length").c_str());
                EXPECT_NEAR(b.startBearingDeg, a.startBearingDeg, 1e-9, (T + ".bearing").c_str());
                EXPECT_NEAR(b.speedMps,      a.speedMps,      1e-9, (T + ".speed").c_str());
                EXPECT_EQ((int)b.direction,  (int)a.direction,     (T + ".direction").c_str());

                EXPECT_EQ(b.speedMode,   a.speedMode,   (T + ".speedMode").c_str());
                EXPECT_EQ(b.headingMode, a.headingMode, (T + ".headingMode").c_str());
                EXPECT_EQ(b.description, a.description, (T + ".description").c_str());
            }
        }
    SkipDetail:;
    }

    // ---- Camera schedule ----
    EXPECT_EQ(loaded.cameras.size(), s.cameras.size(), "Camera.Count");
    if (loaded.cameras.size() == s.cameras.size())
    {
        for (size_t i = 0; i < s.cameras.size(); ++i)
        {
            const CameraFrame& a = s.cameras[i];
            const CameraFrame& b = loaded.cameras[i];
            char tag[32]; std::snprintf(tag, sizeof(tag), "Camera[%zu]", i);
            std::string T(tag);
            EXPECT_EQ((int)b.kind, (int)a.kind, (T + ".kind").c_str());
            EXPECT_EQ(b.sourceEntityId, a.sourceEntityId, (T + ".sourceEntityId").c_str());
            EXPECT_EQ(b.presetType,  a.presetType,  (T + ".presetType").c_str());
            EXPECT_EQ(b.presetAngle, a.presetAngle, (T + ".presetAngle").c_str());
            EXPECT_EQ(b.targetEntityId, a.targetEntityId, (T + ".targetEntityId").c_str());
            EXPECT_NEAR(b.vantEastM,  a.vantEastM,  1e-9, (T + ".vantEast").c_str());
            EXPECT_NEAR(b.vantNorthM, a.vantNorthM, 1e-9, (T + ".vantNorth").c_str());
            EXPECT_NEAR(b.vantUpM,    a.vantUpM,    1e-9, (T + ".vantUp").c_str());
            EXPECT_EQ((int)b.transition, (int)a.transition, (T + ".transition").c_str());
            EXPECT_NEAR(b.beginSecond, a.beginSecond, 1e-9, (T + ".begin").c_str());
            EXPECT_NEAR(b.endSecond,   a.endSecond,   1e-9, (T + ".end").c_str());
            EXPECT_EQ(b.label, a.label, (T + ".label").c_str());
            EXPECT_EQ((int)b.zoomEnabled, (int)a.zoomEnabled, (T + ".zoomEnabled").c_str());
            EXPECT_NEAR(b.zoomFovDeg,  a.zoomFovDeg,  1e-9, (T + ".zoomFovDeg").c_str());
            EXPECT_EQ((int)b.dynamicZoom, (int)a.dynamicZoom, (T + ".dynamicZoom").c_str());
            EXPECT_NEAR(b.zoomFillPct, a.zoomFillPct, 1e-9, (T + ".zoomFillPct").c_str());
            EXPECT_NEAR(b.targetLengthM, a.targetLengthM, 1e-9, (T + ".targetLengthM").c_str());
            EXPECT_NEAR(b.targetWidthM,  a.targetWidthM,  1e-9, (T + ".targetWidthM").c_str());
            EXPECT_NEAR(b.targetHeightM, a.targetHeightM, 1e-9, (T + ".targetHeightM").c_str());
            // Entity-only key, so a Stationary frame must come back with it off.
            EXPECT_EQ((int)b.limitsEnabled, (int)a.limitsEnabled, (T + ".limitsEnabled").c_str());
        }
    }

    // ---- Foliage selection ----
    EXPECT_EQ(loaded.foliage.oak,        s.foliage.oak,        "Foliage.Oak");
    EXPECT_EQ(loaded.foliage.bigTrees,   s.foliage.bigTrees,   "Foliage.BigTrees");
    EXPECT_EQ(loaded.foliage.palm,       s.foliage.palm,       "Foliage.Palm");
    EXPECT_EQ((int)loaded.foliage.palmKind,   (int)s.foliage.palmKind,   "Foliage.PalmKind");
    EXPECT_EQ((int)loaded.foliage.renderMode, (int)s.foliage.renderMode, "Foliage.RenderMode");

    if (failures == 0) {
        std::printf("\nRound-trip PASS\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED\n", failures);
    return 1;
}
