#include "CoordTransforms.h"

#include <cmath>

namespace
{
    constexpr double kDeg2Rad = 0.017453292519943295769; // pi/180
    constexpr double kRad2Deg = 57.295779513082320877;   // 180/pi
    constexpr double kHalfPi  = 1.5707963267948966192;
}

void CoordTransforms::GeodeticToEcefRad(double latRad, double lonRad, double altM,
                                        double& outX, double& outY, double& outZ)
{
    const double sLat = std::sin(latRad);
    const double cLat = std::cos(latRad);
    const double sLon = std::sin(lonRad);
    const double cLon = std::cos(lonRad);
    const double N    = WGS84::a / std::sqrt(1.0 - WGS84::e2 * sLat * sLat);

    outX = (N + altM) * cLat * cLon;
    outY = (N + altM) * cLat * sLon;
    outZ = (N * (1.0 - WGS84::e2) + altM) * sLat;
}

void CoordTransforms::EcefToGeodeticRad(double x, double y, double z,
                                        double& outLatRad, double& outLonRad,
                                        double& outAltM)
{
    outLonRad = std::atan2(y, x);

    const double p = std::sqrt(x * x + y * y);
    if (p < 1.0)
    {
        // Polar singularity.
        outLatRad = (z >= 0.0 ? kHalfPi : -kHalfPi);
        outAltM   = std::fabs(z) - WGS84::a * std::sqrt(1.0 - WGS84::e2);
        return;
    }

    double latRad = std::atan2(z, p * (1.0 - WGS84::e2));
    for (int i = 0; i < 5; ++i)
    {
        const double sLat = std::sin(latRad);
        const double N    = WGS84::a / std::sqrt(1.0 - WGS84::e2 * sLat * sLat);
        const double h    = p / std::cos(latRad) - N;
        const double newLat = std::atan2(z, p * (1.0 - WGS84::e2 * N / (N + h)));
        if (std::fabs(newLat - latRad) < 1e-12) { latRad = newLat; break; }
        latRad = newLat;
    }
    const double sLat = std::sin(latRad);
    const double N    = WGS84::a / std::sqrt(1.0 - WGS84::e2 * sLat * sLat);
    outLatRad = latRad;
    outAltM   = p / std::cos(latRad) - N;
}

void CoordTransforms::LocalEnuToEcef(double east, double north, double up,
                                     double originLatRad, double originLonRad,
                                     double originAltM,
                                     double& outX, double& outY, double& outZ)
{
    double ox = 0.0, oy = 0.0, oz = 0.0;
    GeodeticToEcefRad(originLatRad, originLonRad, originAltM, ox, oy, oz);

    const double sLat = std::sin(originLatRad);
    const double cLat = std::cos(originLatRad);
    const double sLon = std::sin(originLonRad);
    const double cLon = std::cos(originLonRad);

    // ENU→ECEF is the transpose of the rotation in DISBrowser. Columns:
    //   E = (-sLon, cLon, 0)
    //   N = (-sLat·cLon, -sLat·sLon, cLat)
    //   U = ( cLat·cLon,  cLat·sLon, sLat)
    outX = ox + (-sLon)        * east + (-sLat * cLon) * north + ( cLat * cLon) * up;
    outY = oy + ( cLon)        * east + (-sLat * sLon) * north + ( cLat * sLon) * up;
    outZ = oz + ( 0.0 )        * east + ( cLat)        * north + ( sLat)        * up;
}

void CoordTransforms::EcefToLocalEnu(double x, double y, double z,
                                     double originLatRad, double originLonRad,
                                     double originAltM,
                                     double& outEast, double& outNorth, double& outUp)
{
    double ox = 0.0, oy = 0.0, oz = 0.0;
    GeodeticToEcefRad(originLatRad, originLonRad, originAltM, ox, oy, oz);

    const double dx = x - ox;
    const double dy = y - oy;
    const double dz = z - oz;

    const double sLat = std::sin(originLatRad);
    const double cLat = std::cos(originLatRad);
    const double sLon = std::sin(originLonRad);
    const double cLon = std::cos(originLonRad);

    outEast  = -sLon        * dx +  cLon        * dy + 0.0  * dz;
    outNorth = -sLat * cLon * dx + -sLat * sLon * dy + cLat * dz;
    outUp    =  cLat * cLon * dx +  cLat * sLon * dy + sLat * dz;
}

// -------------------- degrees-in wrappers --------------------

void CoordTransforms::GeodeticToEcefDeg(double latDeg, double lonDeg, double altM,
                                        double& outX, double& outY, double& outZ)
{
    GeodeticToEcefRad(latDeg * kDeg2Rad, lonDeg * kDeg2Rad, altM, outX, outY, outZ);
}

void CoordTransforms::EcefToGeodeticDeg(double x, double y, double z,
                                        double& outLatDeg, double& outLonDeg,
                                        double& outAltM)
{
    double latRad = 0.0, lonRad = 0.0;
    EcefToGeodeticRad(x, y, z, latRad, lonRad, outAltM);
    outLatDeg = latRad * kRad2Deg;
    outLonDeg = lonRad * kRad2Deg;
}

void CoordTransforms::LocalEnuToEcefDeg(double east, double north, double up,
                                        double originLatDeg, double originLonDeg,
                                        double originAltM,
                                        double& outX, double& outY, double& outZ)
{
    LocalEnuToEcef(east, north, up,
                   originLatDeg * kDeg2Rad, originLonDeg * kDeg2Rad, originAltM,
                   outX, outY, outZ);
}

void CoordTransforms::EcefToLocalEnuDeg(double x, double y, double z,
                                        double originLatDeg, double originLonDeg,
                                        double originAltM,
                                        double& outEast, double& outNorth, double& outUp)
{
    EcefToLocalEnu(x, y, z,
                   originLatDeg * kDeg2Rad, originLonDeg * kDeg2Rad, originAltM,
                   outEast, outNorth, outUp);
}

// ---------------------------------------------------------------------------
// Orientation transforms (§14.5.2 / §15)
// ---------------------------------------------------------------------------

namespace
{
    // 3×3 matrix helpers (row-major).
    using Mat3 = double[3][3];

    void MatMul(const Mat3& A, const Mat3& B, Mat3& out)
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double s = 0.0;
                for (int k = 0; k < 3; ++k) s += A[i][k] * B[k][j];
                out[i][j] = s;
            }
    }

    void MatTranspose(const Mat3& A, Mat3& out)
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out[i][j] = A[j][i];
    }

    // R_NED_to_ECEF — columns are N, E, D expressed in ECEF.
    void NedToEcefMatrix(double latRad, double lonRad, Mat3& out)
    {
        const double sLat = std::sin(latRad);
        const double cLat = std::cos(latRad);
        const double sLon = std::sin(lonRad);
        const double cLon = std::cos(lonRad);
        // col 0 = North in ECEF
        out[0][0] = -sLat * cLon; out[1][0] = -sLat * sLon; out[2][0] =  cLat;
        // col 1 = East in ECEF
        out[0][1] = -sLon;        out[1][1] =  cLon;        out[2][1] =  0.0;
        // col 2 = Down in ECEF
        out[0][2] = -cLat * cLon; out[1][2] = -cLat * sLon; out[2][2] = -sLat;
    }

    // R_body_to_NED for aviation H/P/R (radians). ZYX Tait-Bryan.
    //   yaw   = heading       (about Down)
    //   pitch = pitch         (about intermediate-East)
    //   roll  = roll          (about body-Forward)
    void BodyToNedMatrix(double yawRad, double pitchRad, double rollRad, Mat3& out)
    {
        const double cy = std::cos(yawRad),   sy = std::sin(yawRad);
        const double cp = std::cos(pitchRad), sp = std::sin(pitchRad);
        const double cr = std::cos(rollRad),  sr = std::sin(rollRad);

        out[0][0] =  cp * cy;
        out[0][1] = -cr * sy + sr * sp * cy;
        out[0][2] =  sr * sy + cr * sp * cy;

        out[1][0] =  cp * sy;
        out[1][1] =  cr * cy + sr * sp * sy;
        out[1][2] = -sr * cy + cr * sp * sy;

        out[2][0] = -sp;
        out[2][1] =  sr * cp;
        out[2][2] =  cr * cp;
    }

    // Extract ZYX Tait-Bryan (psi, theta, phi) from R_body_to_world.
    // Handles the singular |theta| ≈ pi/2 case by zeroing psi.
    void ExtractZyx(const Mat3& R, double& psi, double& theta, double& phi)
    {
        double r20 = R[2][0];
        if (r20 >  1.0) r20 =  1.0;
        if (r20 < -1.0) r20 = -1.0;

        theta = std::asin(-r20);
        const double cTheta = std::cos(theta);
        if (std::fabs(cTheta) > 1e-9)
        {
            psi = std::atan2(R[1][0], R[0][0]);
            phi = std::atan2(R[2][1], R[2][2]);
        }
        else
        {
            // Gimbal lock — pick psi=0 and roll absorbs the residual yaw.
            psi = 0.0;
            phi = std::atan2(-R[0][1], R[1][1]);
        }
    }
}

void CoordTransforms::LocalHprToEcefPsiThetaPhi(double headingDeg, double pitchDeg,
                                                double rollDeg,
                                                double originLatDeg, double originLonDeg,
                                                double& outPsiRad, double& outThetaRad,
                                                double& outPhiRad)
{
    const double H = headingDeg * kDeg2Rad;
    const double P = pitchDeg   * kDeg2Rad;
    const double R = rollDeg    * kDeg2Rad;

    Mat3 Rbody_to_ned;
    BodyToNedMatrix(H, P, R, Rbody_to_ned);

    Mat3 Rned_to_ecef;
    NedToEcefMatrix(originLatDeg * kDeg2Rad, originLonDeg * kDeg2Rad, Rned_to_ecef);

    Mat3 Rbody_to_ecef;
    MatMul(Rned_to_ecef, Rbody_to_ned, Rbody_to_ecef);

    ExtractZyx(Rbody_to_ecef, outPsiRad, outThetaRad, outPhiRad);
}

void CoordTransforms::EcefPsiThetaPhiToLocalHpr(double psiRad, double thetaRad,
                                                double phiRad,
                                                double originLatDeg, double originLonDeg,
                                                double& outHeadingDeg, double& outPitchDeg,
                                                double& outRollDeg)
{
    // Reconstruct R_body_to_ECEF from DIS (psi, theta, phi).
    Mat3 Rbody_to_ecef;
    BodyToNedMatrix(psiRad, thetaRad, phiRad, Rbody_to_ecef);

    // R_body_to_NED = R_ECEF_to_NED · R_body_to_ECEF, and R_ECEF_to_NED is the
    // transpose of R_NED_to_ECEF (rotation matrices are orthonormal).
    Mat3 Rned_to_ecef, Recef_to_ned;
    NedToEcefMatrix(originLatDeg * kDeg2Rad, originLonDeg * kDeg2Rad, Rned_to_ecef);
    MatTranspose(Rned_to_ecef, Recef_to_ned);

    Mat3 Rbody_to_ned;
    MatMul(Recef_to_ned, Rbody_to_ecef, Rbody_to_ned);

    double H = 0.0, P = 0.0, R = 0.0;
    ExtractZyx(Rbody_to_ned, H, P, R);
    outHeadingDeg = H * kRad2Deg;
    outPitchDeg   = P * kRad2Deg;
    outRollDeg    = R * kRad2Deg;
}
