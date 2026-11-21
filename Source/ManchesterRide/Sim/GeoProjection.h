// Manchester Riding Simulator - geographic projection shared by the game and the
// geo import pipeline (Tools/geo_pipeline/geo.mjs implements the identical maths;
// Tests/SimTests checks both agree on reference points).
//
// Projection: Transverse Mercator on the WGS84 ellipsoid, centred on the world
// origin (Manchester Town Hall, Albert Square), scale factor 1. Over the ~45 km
// extent of Greater Manchester the scale error is < 0.003 %, so distances in game
// match real distances.
//
// Unreal axes: +X = East, +Y = South, +Z = Up (keeps Unreal's left-handed frame).
#pragma once

namespace MRGeo
{
	struct FLatLon { double Lat = 0.0; double Lon = 0.0; };
	struct FEastNorth { double East = 0.0; double North = 0.0; };

	constexpr double OriginLat = 53.47936;   // Manchester Town Hall
	constexpr double OriginLon = -2.24484;

	FEastNorth ToLocal(const FLatLon& P, double Lat0 = OriginLat, double Lon0 = OriginLon);
	FLatLon ToLatLon(const FEastNorth& P, double Lat0 = OriginLat, double Lon0 = OriginLon);

	// Unreal centimetres <-> geographic
	inline void LocalToUnreal(const FEastNorth& P, double& OutX, double& OutY) { OutX = P.East * 100.0; OutY = -P.North * 100.0; }
	inline FEastNorth UnrealToLocal(double X, double Y) { return FEastNorth{ X / 100.0, -Y / 100.0 }; }
}
