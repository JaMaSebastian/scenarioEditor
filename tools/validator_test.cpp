#include "Scenario.h"
#include "Validator.h"

#include <cstdio>
#include <string>

namespace
{
    int failures = 0;
    void Expect(bool cond, const char* what) {
        if (!cond) { std::printf("  FAIL %s\n", what); ++failures; }
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
        s.entities.front().lat = 95.0;
        s.entities.front().lon = -200.0;
        auto r = Validator::Validate(s);
        Expect(r.errorCount >= 2, "lat/lon out of range → errors");
    }

    // ---- Case 4: motion segments overlap and gap behavior ----
    {
        Scenario s;
        Entity& e = s.entities.front();
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
        s.durationSeconds = 60.0;
        s.entities.front().enabled      = true;
        s.entities.front().updateRateHz = 5.0;
        auto r = Validator::Validate(s);
        // 60 s × 5 Hz = 300 PDUs; 300 × 144 × 8 / 1e6 / 60 ≈ 0.00576 Mbps
        Expect(r.pduCount == 300, "pdu estimate = 300");
        Expect(r.mbpsAvg > 0.005 && r.mbpsAvg < 0.01, "Mbps estimate band");
    }

    // ---- Case 6: ellipse without radius/period → errors ----
    {
        Scenario s;
        Entity& e = s.entities.front();
        MotionSegment m; m.type = MotionType::Ellipse; m.startSecond = 0; m.endSecond = 60;
        m.radiusXMeters = 0; m.radiusYMeters = 0; m.periodSeconds = 0;
        e.motionSegments.push_back(m);
        auto r = Validator::Validate(s);
        Expect(r.errorCount >= 2, "bad ellipse → errors");
    }

    if (failures == 0) { std::printf("\nvalidator_test PASS\n"); return 0; }
    std::printf("\n%d assertion(s) FAILED\n", failures);
    return 1;
}
