// Standalone WGS-84 round-trip test for CoordTransforms.
// Built via CMake as the `coord_roundtrip` target. Exits 0 on pass, 1 on fail.

#include "CoordTransforms.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
    struct Geodetic { double lat, lon, alt; const char* label; };

    // Test points spanning latitudes / hemispheres / altitudes. The pole and
    // anti-meridian cases stress the polar-singularity and atan2 wrap paths.
    const Geodetic kPoints[] = {
        {  37.7749, -122.4194,    0.0,  "San Francisco" },
        {  51.5074,    0.1278,   25.0,  "London"        },
        { -33.8688,  151.2093,    8.0,  "Sydney"        },
        {   0.0000,   90.0000, 1000.0,  "Equator/+90E"  },
        {   0.0000, -180.0000,    0.0,  "Equator/Antimeridian" },
        {  82.0000,  -75.0000,  500.0,  "Near North Pole" },
        { -82.0000,  150.0000,  -50.0,  "Near South Pole" },
    };

    int failures = 0;

    void ExpectNear(double a, double b, double tol, const char* what)
    {
        if (std::fabs(a - b) <= tol) return;
        std::printf("  FAIL %s: %.9f != %.9f (delta %.3e, tol %.3e)\n",
                    what, a, b, std::fabs(a - b), tol);
        ++failures;
    }
}

int main()
{
    using namespace CoordTransforms;

    // ---- Geodetic ↔ ECEF round-trip ----
    for (const Geodetic& g : kPoints)
    {
        double x = 0, y = 0, z = 0;
        GeodeticToEcefDeg(g.lat, g.lon, g.alt, x, y, z);

        double lat2 = 0, lon2 = 0, alt2 = 0;
        EcefToGeodeticDeg(x, y, z, lat2, lon2, alt2);

        std::printf("[%s] LLA(%.4f, %.4f, %.1f) -> ECEF(%.2f, %.2f, %.2f) -> LLA(%.6f, %.6f, %.3f)\n",
                    g.label, g.lat, g.lon, g.alt, x, y, z, lat2, lon2, alt2);

        ExpectNear(lat2, g.lat, 1e-6, "lat");
        // Anti-meridian: ±180 are the same angle; allow wrap.
        const double lonDelta = std::fabs(lon2 - g.lon);
        if (lonDelta > 1e-6 && std::fabs(lonDelta - 360.0) > 1e-6)
            ExpectNear(lon2, g.lon, 1e-6, "lon");
        ExpectNear(alt2, g.alt, 1e-3, "alt"); // mm-level tolerance is plenty
    }

    // ---- LocalENU ↔ ECEF round-trip (origin = San Francisco) ----
    const double oLat = 37.7749, oLon = -122.4194, oAlt = 0.0;
    const double kENU[][3] = {
        {   0.0,    0.0,    0.0 },  // origin
        { 100.0,    0.0,    0.0 },  // 100m east
        {   0.0,  100.0,    0.0 },  // 100m north
        {   0.0,    0.0,  100.0 },  // 100m up
        { 1000.0, -500.0,   50.0 }, // mixed
        { 50000.0, 50000.0, 500.0}, // 50km offset — tangent plane error grows but still small
    };
    for (const auto& enu : kENU)
    {
        double x = 0, y = 0, z = 0;
        LocalEnuToEcefDeg(enu[0], enu[1], enu[2], oLat, oLon, oAlt, x, y, z);

        double e = 0, n = 0, u = 0;
        EcefToLocalEnuDeg(x, y, z, oLat, oLon, oAlt, e, n, u);

        std::printf("ENU(%.1f, %.1f, %.1f) -> ECEF(%.2f, %.2f, %.2f) -> ENU(%.6f, %.6f, %.6f)\n",
                    enu[0], enu[1], enu[2], x, y, z, e, n, u);
        ExpectNear(e, enu[0], 1e-6, "east");
        ExpectNear(n, enu[1], 1e-6, "north");
        ExpectNear(u, enu[2], 1e-6, "up");
    }

    // ---- Sanity: ENU origin (0,0,0) maps to the origin's ECEF ----
    {
        double ox = 0, oy = 0, oz = 0;
        GeodeticToEcefDeg(oLat, oLon, oAlt, ox, oy, oz);
        double x = 0, y = 0, z = 0;
        LocalEnuToEcefDeg(0.0, 0.0, 0.0, oLat, oLon, oAlt, x, y, z);
        ExpectNear(x, ox, 1e-6, "origin ENU.x == origin ECEF.x");
        ExpectNear(y, oy, 1e-6, "origin ENU.y == origin ECEF.y");
        ExpectNear(z, oz, 1e-6, "origin ENU.z == origin ECEF.z");
    }

    // ---- H/P/R ↔ ψ/θ/φ round-trip at several origins ----
    struct HprCase { double h, p, r, lat, lon; const char* label; };
    const HprCase kHpr[] = {
        // No rotation, equator/prime-meridian — body-x aligned with NED-N → ECEF-x
        {   0.0,   0.0,   0.0,  0.0,    0.0, "identity@(0,0)"     },
        // Pure heading
        {  45.0,   0.0,   0.0, 37.7749, -122.4194, "heading 45@SF" },
        {  90.0,   0.0,   0.0, 51.5074,    0.1278, "heading 90@London" },
        { 180.0,   0.0,   0.0, -33.8688, 151.2093, "heading 180@Sydney" },
        // Pure pitch / roll
        {   0.0,  30.0,   0.0, 37.7749, -122.4194, "pitch 30@SF"   },
        {   0.0,   0.0, -45.0, 37.7749, -122.4194, "roll -45@SF"   },
        // Mixed
        { 120.0,  15.0, -25.0, 39.0458,  -76.6413, "mixed@DC"      },
        { 270.0, -10.0,  60.0, -33.8688, 151.2093, "mixed@Sydney"  },
        // Near-pole (still well away from pitch=±90 gimbal)
        {  60.0,   5.0,  -5.0, 82.0000,  -75.0000, "near N pole"   },
    };
    for (const HprCase& c : kHpr)
    {
        double psi = 0, theta = 0, phi = 0;
        LocalHprToEcefPsiThetaPhi(c.h, c.p, c.r, c.lat, c.lon, psi, theta, phi);

        double h2 = 0, p2 = 0, r2 = 0;
        EcefPsiThetaPhiToLocalHpr(psi, theta, phi, c.lat, c.lon, h2, p2, r2);

        std::printf("[%s] HPR(%.2f, %.2f, %.2f) -> psi/theta/phi(%.4f, %.4f, %.4f) -> HPR(%.4f, %.4f, %.4f)\n",
                    c.label, c.h, c.p, c.r, psi, theta, phi, h2, p2, r2);

        // Heading wraps at ±360.
        double dH = std::fabs(h2 - c.h);
        if (dH > 180.0) dH = std::fabs(dH - 360.0);
        if (dH > 1e-6) {
            std::printf("  FAIL %s heading: delta %.3e\n", c.label, dH);
            ++failures;
        }
        ExpectNear(p2, c.p, 1e-6, c.label);
        ExpectNear(r2, c.r, 1e-6, c.label);
    }

    if (failures == 0) {
        std::printf("\nALL %d assertions passed.\n", 0);
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", failures);
    return 1;
}
