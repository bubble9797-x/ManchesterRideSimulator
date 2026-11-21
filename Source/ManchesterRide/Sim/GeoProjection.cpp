#include "GeoProjection.h"

#include <cmath>

namespace MRGeo
{
	namespace
	{
		constexpr double A = 6378137.0;              // WGS84 semi-major axis
		constexpr double F = 1.0 / 298.257223563;    // flattening
		constexpr double E2 = F * (2.0 - F);         // first eccentricity squared
		constexpr double Ep2 = E2 / (1.0 - E2);      // second eccentricity squared
		constexpr double Deg = 3.14159265358979323846 / 180.0;

		double MeridianArc(double Phi)
		{
			const double E4 = E2 * E2, E6 = E4 * E2;
			return A * ((1.0 - E2 / 4.0 - 3.0 * E4 / 64.0 - 5.0 * E6 / 256.0) * Phi
				- (3.0 * E2 / 8.0 + 3.0 * E4 / 32.0 + 45.0 * E6 / 1024.0) * std::sin(2.0 * Phi)
				+ (15.0 * E4 / 256.0 + 45.0 * E6 / 1024.0) * std::sin(4.0 * Phi)
				- (35.0 * E6 / 3072.0) * std::sin(6.0 * Phi));
		}
	}

	// Snyder, "Map Projections - A Working Manual" (USGS 1395), eqs. 8-9..8-25.
	FEastNorth ToLocal(const FLatLon& P, double Lat0, double Lon0)
	{
		const double Phi = P.Lat * Deg, Lam = P.Lon * Deg, Phi0 = Lat0 * Deg, Lam0 = Lon0 * Deg;
		const double SinPhi = std::sin(Phi), CosPhi = std::cos(Phi), TanPhi = std::tan(Phi);
		const double N = A / std::sqrt(1.0 - E2 * SinPhi * SinPhi);
		const double T = TanPhi * TanPhi;
		const double C = Ep2 * CosPhi * CosPhi;
		const double Aa = (Lam - Lam0) * CosPhi;
		const double M = MeridianArc(Phi), M0 = MeridianArc(Phi0);

		FEastNorth Out;
		Out.East = N * (Aa + (1.0 - T + C) * std::pow(Aa, 3) / 6.0 + (5.0 - 18.0 * T + T * T + 72.0 * C - 58.0 * Ep2) * std::pow(Aa, 5) / 120.0);
		Out.North = (M - M0) + N * TanPhi * (Aa * Aa / 2.0 + (5.0 - T + 9.0 * C + 4.0 * C * C) * std::pow(Aa, 4) / 24.0
			+ (61.0 - 58.0 * T + T * T + 600.0 * C - 330.0 * Ep2) * std::pow(Aa, 6) / 720.0);
		return Out;
	}

	FLatLon ToLatLon(const FEastNorth& P, double Lat0, double Lon0)
	{
		const double Phi0 = Lat0 * Deg, Lam0 = Lon0 * Deg;
		const double M = MeridianArc(Phi0) + P.North;
		const double E4 = E2 * E2, E6 = E4 * E2;
		const double Mu = M / (A * (1.0 - E2 / 4.0 - 3.0 * E4 / 64.0 - 5.0 * E6 / 256.0));
		const double E1 = (1.0 - std::sqrt(1.0 - E2)) / (1.0 + std::sqrt(1.0 - E2));
		const double Phi1 = Mu + (3.0 * E1 / 2.0 - 27.0 * std::pow(E1, 3) / 32.0) * std::sin(2.0 * Mu)
			+ (21.0 * E1 * E1 / 16.0 - 55.0 * std::pow(E1, 4) / 32.0) * std::sin(4.0 * Mu)
			+ (151.0 * std::pow(E1, 3) / 96.0) * std::sin(6.0 * Mu)
			+ (1097.0 * std::pow(E1, 4) / 512.0) * std::sin(8.0 * Mu);
		const double Sin1 = std::sin(Phi1), Cos1 = std::cos(Phi1), Tan1 = std::tan(Phi1);
		const double C1 = Ep2 * Cos1 * Cos1;
		const double T1 = Tan1 * Tan1;
		const double N1 = A / std::sqrt(1.0 - E2 * Sin1 * Sin1);
		const double R1 = A * (1.0 - E2) / std::pow(1.0 - E2 * Sin1 * Sin1, 1.5);
		const double D = P.East / N1;

		FLatLon Out;
		Out.Lat = (Phi1 - (N1 * Tan1 / R1) * (D * D / 2.0 - (5.0 + 3.0 * T1 + 10.0 * C1 - 4.0 * C1 * C1 - 9.0 * Ep2) * std::pow(D, 4) / 24.0
			+ (61.0 + 90.0 * T1 + 298.0 * C1 + 45.0 * T1 * T1 - 252.0 * Ep2 - 3.0 * C1 * C1) * std::pow(D, 6) / 720.0)) / Deg;
		Out.Lon = (Lam0 + (D - (1.0 + 2.0 * T1 + C1) * std::pow(D, 3) / 6.0
			+ (5.0 - 2.0 * C1 + 28.0 * T1 - 3.0 * C1 * C1 + 8.0 * Ep2 + 24.0 * T1 * T1) * std::pow(D, 5) / 120.0) / Cos1) / Deg;
		return Out;
	}
}
