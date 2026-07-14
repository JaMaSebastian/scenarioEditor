//=============================================================================
//  CoordTransforms.h
//-----------------------------------------------------------------------------
//  Declares the pure-C++ WGS-84 coordinate and orientation transforms used to
//  convert between geodetic (lat/lon/alt), local ENU tangent-plane, and ECEF
//  frames, plus aviation H/P/R <-> DIS (psi, theta, phi) orientation mapping.
//  No Unreal dependencies; ported from DISBrowser's FDISCoordinateConverter.
//
//  Author:        Matt Sebastian
//  Date started:  2026-05-21
//=============================================================================
#pragma once

// Pure C++ WGS-84 coordinate transforms (§15). Ported from DISBrowser's
// FDISCoordinateConverter — same math, no Unreal dependencies.
//
// Three frames:
//   - Geodetic Lat/Lon/Alt:  WGS-84 ellipsoidal, degrees + metres
//   - Local ENU:             East/North/Up tangent plane at a scenario origin
//   - ECEF:                  Earth-Centered Earth-Fixed metres (DIS wire frame)
//
// All inputs and outputs are doubles. Functions never throw; degenerate
// inputs (e.g. ECEF point at the pole) are handled with a sentinel branch.

namespace CoordTransforms
{
    namespace WGS84
    {
        constexpr double a  = 6378137.0;                 // semi-major axis (m)
        constexpr double f  = 1.0 / 298.257223563;       // flattening
        constexpr double e2 = 2.0 * f - f * f;           // first eccentricity²
    }

    // ---------- primitives (radians-in) ----------

    //
    // GeodeticToEcefRad — geodetic lat/lon (rad) + altitude (m) to ECEF x/y/z (m).
    //
    void GeodeticToEcefRad(double latRad, double lonRad, double altM,
                           double& outX, double& outY, double& outZ);

    // Iterative Bowring-style inverse — converges in ~3 iterations.
    void EcefToGeodeticRad(double x, double y, double z,
                           double& outLatRad, double& outLonRad, double& outAltM);

    //
    // LocalEnuToEcef — ENU offsets (m) about a geodetic origin (rad) to ECEF x/y/z (m).
    //
    void LocalEnuToEcef(double east, double north, double up,
                        double originLatRad, double originLonRad, double originAltM,
                        double& outX, double& outY, double& outZ);

    //
    // EcefToLocalEnu — ECEF x/y/z (m) to ENU offsets (m) about a geodetic origin (rad).
    //
    void EcefToLocalEnu(double x, double y, double z,
                        double originLatRad, double originLonRad, double originAltM,
                        double& outEast, double& outNorth, double& outUp);

    // ---------- degrees-in convenience wrappers ----------
    // Each forwards to its radians-in primitive after a deg->rad conversion.

    void GeodeticToEcefDeg(double latDeg, double lonDeg, double altM,
                           double& outX, double& outY, double& outZ);

    void EcefToGeodeticDeg(double x, double y, double z,
                           double& outLatDeg, double& outLonDeg, double& outAltM);

    void LocalEnuToEcefDeg(double east, double north, double up,
                           double originLatDeg, double originLonDeg, double originAltM,
                           double& outX, double& outY, double& outZ);

    void EcefToLocalEnuDeg(double x, double y, double z,
                           double originLatDeg, double originLonDeg, double originAltM,
                           double& outEast, double& outNorth, double& outUp);

    // ---------- orientation ----------
    //
    // Maps the user-facing aviation Euler triple (Heading, Pitch, Roll)
    // expressed in the local tangent plane at `origin` to the DIS Entity
    // Orientation triple (psi, theta, phi) per §14.5.2 / IEEE 1278.1
    // §B.1.6.4 (body→ECEF ZYX Tait-Bryan).
    //
    // Conventions per spec §7.3.1:
    //   Heading  degrees, 0 = North, clockwise from above.
    //   Pitch    degrees, positive = nose up.
    //   Roll     degrees, positive = right wing down.
    //   psi/theta/phi   radians, DIS wire values.
    //
    // The body frame is the standard DIS aircraft frame
    // (x = forward, y = right, z = down). The local frame for these
    // angles is NED (North-East-Down) — the aviation companion to the
    // ENU tangent plane mentioned in §7.3.1; both describe the same
    // physical orientation.
    void LocalHprToEcefPsiThetaPhi(double headingDeg, double pitchDeg, double rollDeg,
                                   double originLatDeg, double originLonDeg,
                                   double& outPsiRad, double& outThetaRad,
                                   double& outPhiRad);

    // Inverse: recover H/P/R at `origin` from DIS (psi, theta, phi).
    // Provided for the round-trip test; not used by the live PDU path.
    void EcefPsiThetaPhiToLocalHpr(double psiRad, double thetaRad, double phiRad,
                                   double originLatDeg, double originLonDeg,
                                   double& outHeadingDeg, double& outPitchDeg,
                                   double& outRollDeg);
}
