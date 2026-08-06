#include "Scenario.h"
#include "Validator.h"
#include "EntityTypeCatalog.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace
{
    int failures = 0;
    void Expect(bool cond, const char* what) {
        if (!cond) { std::printf("  FAIL %s\n", what); ++failures; }
    }

    // A fresh Scenario starts with NO entities (the operator adds them via
    // "New"), so every case that pokes entity fields must add one first.
    Entity& AddEntity(Scenario& s)
    {
        s.entities.emplace_back();
        return s.entities.back();
    }
}

int main()
{
    using V = Validator::Severity;

    // ---- Case 1: default scenario is mostly OK but flags origin warnings ----
    {
        Scenario s;
        auto r = Validator::Validate(s);
        Expect(r.errorCount == 0, "default scenario: no errors");
    }

    // ---- Case 2: duplicate Entity IDs trigger an Error ----
    {
        Scenario s;
        s.entities.clear();
        Entity a; a.siteId = 1; a.applicationId = 1; a.entityId = 100;
        Entity b; b.siteId = 1; b.applicationId = 1; b.entityId = 100; b.name = "dup";
        s.entities.push_back(a);
        s.entities.push_back(b);
        auto r = Validator::Validate(s);
        Expect(r.errorCount >= 1, "duplicate entity IDs → error");
    }

    // ---- Case 3: invalid lat/lon out of range ----
    {
        Scenario s;
        Entity& e = AddEntity(s);
        e.lat = 95.0;
        e.lon = -200.0;
        auto r = Validator::Validate(s);
        Expect(r.errorCount >= 2, "lat/lon out of range → errors");
    }

    // ---- Case 4: motion segments overlap and gap behavior ----
    {
        Scenario s;
        Entity& e = AddEntity(s);
        MotionSegment a; a.startSecond = 0;  a.endSecond = 30; e.motionSegments.push_back(a);
        MotionSegment b; b.startSecond = 25; b.endSecond = 60; e.motionSegments.push_back(b); // overlap
        MotionSegment c; c.startSecond = 80; c.endSecond = 100; e.motionSegments.push_back(c); // gap
        auto r = Validator::Validate(s);
        Expect(r.warningCount >= 1, "overlap → warning");
        Expect(r.infoCount    >= 1, "gap → info");
    }

    // ---- Case 5: PDU/bandwidth estimate ----
    {
        Scenario s;
        Entity& e = AddEntity(s);
        s.durationSeconds = 60.0;
        e.enabled      = true;
        e.updateRateHz = 5.0;
        auto r = Validator::Validate(s);
        // 60 s × 5 Hz = 300 PDUs; 300 × 144 × 8 / 1e6 / 60 ≈ 0.00576 Mbps
        Expect(r.pduCount == 300, "pdu estimate = 300");
        Expect(r.mbpsAvg > 0.005 && r.mbpsAvg < 0.01, "Mbps estimate band");
    }

    // ---- Case 6: ellipse without length / speed → errors ----
    {
        Scenario s;
        Entity& e = AddEntity(s);
        MotionSegment m; m.type = MotionType::Ellipse; m.startSecond = 0; m.endSecond = 60;
        m.lengthMeters = 0; m.speedMps = 0;
        e.motionSegments.push_back(m);
        auto r = Validator::Validate(s);
        Expect(r.errorCount >= 2, "bad ellipse → errors");
    }

    // ---- Case 7: Name matching a catalog platform on a different tuple → warning ----
    // The "Entity.1" bug: an entity labelled "F/A-18 Hornet" whose Subcategory
    // was silently reset to 1 — the F-16's tuple — so DISBrowser rendered the
    // wrong airframe while the Preview label still said Hornet.
    {
        EntityTypeCatalog cat;
        cat.MutableKinds().push_back({ 1, "Platform", {}, {} });
        cat.MutableDomains()[1].push_back({ 2, "Air", {}, {} });
        cat.MutableCategories()[{ 1, 2 }].push_back({ 1,  "Fighter", {}, {} });
        cat.MutableCategories()[{ 1, 2 }].push_back({ 50, "UAV",     {}, {} });
        cat.MutableSubcategories()[{ 1, 2, 1 }].push_back({ 1, "F-16 Fighting Falcon", {}, {} });
        cat.MutableSubcategories()[{ 1, 2, 1 }].push_back({ 3, "F/A-18 Hornet",        {}, {} });
        cat.MutableSubcategories()[{ 1, 2, 50 }].push_back({ 2, "MQ-9 Reaper",         {}, {} });

        auto countNameWarnings = [](const Validator::Report& rep) {
            return std::count_if(rep.issues.begin(), rep.issues.end(),
                [](const Validator::Issue& i) {
                    return i.severity == V::Warning &&
                           i.message.find("Name matches catalog platform") != std::string::npos;
                });
        };

        Scenario s;
        Entity& e = AddEntity(s);
        e.enabled = true;
        e.name = "F/A-18 Hornet";
        e.kind = 1; e.domain = 2; e.category = 1; e.subcategory = 1;   // F-16's tuple
        Expect(countNameWarnings(Validator::Validate(s, cat)) == 1,
               "Hornet name on F-16 tuple → one name/type warning");

        e.subcategory = 3;                                             // the real Hornet
        Expect(countNameWarnings(Validator::Validate(s, cat)) == 0,
               "Hornet name on Hornet tuple → no name/type warning");

        e.name = "MQ-9 Reaper";                                        // still 1.2.1.3
        Expect(countNameWarnings(Validator::Validate(s, cat)) == 1,
               "Reaper name on Hornet tuple → one name/type warning");

        e.category = 50; e.subcategory = 2;                            // real MQ-9 tuple
        Expect(countNameWarnings(Validator::Validate(s, cat)) == 0,
               "Reaper name on Reaper tuple → no name/type warning");

        e.name = "Blue Lead";                                          // free text, not catalogued
        e.category = 1; e.subcategory = 1;
        Expect(countNameWarnings(Validator::Validate(s, cat)) == 0,
               "non-catalog free-text name → no name/type warning");
    }

    if (failures == 0) { std::printf("\nvalidator_test PASS\n"); return 0; }
    std::printf("\n%d assertion(s) FAILED\n", failures);
    return 1;
}
