#include "World/MRGeoWorld.h"

#include "ManchesterRide.h"
#include "RideSimTypes.h"
#include "GeoProjection.h"
#include "Vehicles/MRVehicleMeshBuilder.h"
#include "World/MRWorldEnvironment.h"

#include "Async/Async.h"
#include "Components/PointLightComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "ProceduralMeshComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/ConstructorHelpers.h"

using MRSim::ESurface;

namespace GeoBuild
{
	// Material/surface groups. Each group becomes one procedural mesh component per cell
	// with its own colour and physical surface (so tyre grip matches the real surface tag).
	enum EGroup : int32
	{
		Ground, MajorRoad, MinorRoad, Paving, CycleLane, Cobbles, Gravel, DirtPath, Grass, Woodland, Water, Rail,
		MarkWhite, MarkYellow,                                  // road markings (no collision, no shadow)
		Foliage, Bark, PropMetal, PropRed, PropGlass, LampHead, // trees and street furniture (cast shadows)
		PropBrick, PropConcrete, PropBlue, PropWhite,            // viaduct piers, bridge piers, landmark/stadium parts
		Brick, Stone, Glass, Industrial, Count
	};
	inline bool CastsShadow(int32 G) { return G >= Foliage; }
	struct FGroupDef { FLinearColor Color; ESurface Surface; bool bCollide; };
	const FGroupDef Groups[Count] = {
		{ FLinearColor(0.20f, 0.20f, 0.19f), ESurface::Concrete, true },   // Ground (urban default: weathered concrete)
		{ FLinearColor(0.035f, 0.035f, 0.04f), ESurface::Asphalt, true },  // MajorRoad
		{ FLinearColor(0.05f, 0.05f, 0.055f), ESurface::Asphalt, true },   // MinorRoad
		{ FLinearColor(0.30f, 0.29f, 0.27f), ESurface::Concrete, true },   // Paving (footways, pedestrian areas)
		{ FLinearColor(0.30f, 0.06f, 0.05f), ESurface::Asphalt, true },    // CycleLane (UK red surfacing)
		{ FLinearColor(0.18f, 0.17f, 0.17f), ESurface::Cobbles, true },    // Cobbles / setts
		{ FLinearColor(0.30f, 0.27f, 0.22f), ESurface::Gravel, true },     // Gravel / tracks
		{ FLinearColor(0.28f, 0.17f, 0.08f), ESurface::Dirt, true },       // Dirt paths
		{ FLinearColor(0.065f, 0.115f, 0.03f), ESurface::Grass, true },    // Grass / parks (measured turf albedo, not cartoon green)
		{ FLinearColor(0.05f, 0.065f, 0.03f), ESurface::ForestTrail, true },// Woodland floor (leaf litter)
		{ FLinearColor(0.02f, 0.06f, 0.09f), ESurface::Mud, true },        // Water (painted ground / river ribbons; solid + low grip, never a hole)
		{ FLinearColor(0.12f, 0.10f, 0.09f), ESurface::Gravel, true },     // Rail / tram ballast
		{ FLinearColor(0.75f, 0.75f, 0.72f), ESurface::Asphalt, false },   // MarkWhite (centre/edge lines, zebra)
		{ FLinearColor(0.75f, 0.55f, 0.05f), ESurface::Asphalt, false },   // MarkYellow (double yellows)
		{ FLinearColor(0.04f, 0.075f, 0.022f), ESurface::Grass, false },   // Foliage (tree canopies: ride-through)
		{ FLinearColor(0.09f, 0.07f, 0.05f), ESurface::Concrete, true },   // Bark (trunks, timber benches)
		{ FLinearColor(0.09f, 0.1f, 0.1f), ESurface::Concrete, true },     // PropMetal (lamp posts, signals, shelters)
		{ FLinearColor(0.55f, 0.03f, 0.03f), ESurface::Concrete, true },   // PropRed (post boxes, K6 phone boxes)
		{ FLinearColor(0.35f, 0.4f, 0.42f), ESurface::Concrete, true },    // PropGlass (shelter / phone box glazing)
		{ FLinearColor(1.0f, 0.82f, 0.55f), ESurface::Concrete, false },   // LampHead (lit at night)
		{ FLinearColor(0.30f, 0.11f, 0.07f), ESurface::Concrete, true },   // PropBrick (railway viaducts)
		{ FLinearColor(0.42f, 0.41f, 0.39f), ESurface::Concrete, true },   // PropConcrete (road bridge piers)
		{ FLinearColor(0.32f, 0.6f, 0.86f), ESurface::Concrete, true },    // PropBlue (Etihad seats)
		{ FLinearColor(0.85f, 0.86f, 0.88f), ESurface::Concrete, true },   // PropWhite (stadium roofs, masts, columns)
		{ FLinearColor(0.36f, 0.13f, 0.08f), ESurface::Concrete, true },   // Brick (terraces, mills, churches)
		{ FLinearColor(0.42f, 0.40f, 0.36f), ESurface::Concrete, true },   // Stone / render (commercial, civic)
		{ FLinearColor(0.16f, 0.20f, 0.24f), ESurface::Concrete, true },   // Glass / office towers
		{ FLinearColor(0.28f, 0.30f, 0.31f), ESurface::Concrete, true },   // Industrial cladding
	};

	using FSec = FMRMeshSection; // same V/N/T layout; lets the shared shape kit (MRMeshKit) build props and trees

	bool GFlipWinding = false; // set from config if faces render inside-out on some engine version

	// Unreal computes face normals as (P1 - P2) ^ (P0 - P2); orient each triangle to Desired.
	void Tri(FSec& S, const FVector& A, const FVector& B, const FVector& C, const FVector& Desired)
	{
		const FVector Nrm = FVector::CrossProduct(B - C, A - C);
		const bool bSwap = (FVector::DotProduct(Nrm, Desired) < 0.f) != GFlipWinding;
		const int32 Base = S.V.Num();
		S.V.Add(A); S.V.Add(bSwap ? C : B); S.V.Add(bSwap ? B : C);
		S.N.Add(Desired); S.N.Add(Desired); S.N.Add(Desired);
		S.T.Add(Base); S.T.Add(Base + 1); S.T.Add(Base + 2);
	}

	double SignedArea(const TArray<FVector2D>& P)
	{
		double A = 0.0;
		for (int32 i = 0, j = P.Num() - 1; i < P.Num(); j = i++) A += (P[j].X * P[i].Y - P[i].X * P[j].Y);
		return A * 0.5;
	}

	/** Ear clipping triangulation (O(n^2), fine for OSM footprints). Returns index triples. */
	TArray<int32> Triangulate(const TArray<FVector2D>& InPts)
	{
		TArray<int32> Out;
		const int32 N = InPts.Num();
		if (N < 3) return Out;
		TArray<int32> Idx;
		for (int32 i = 0; i < N; ++i) Idx.Add(i);
		const bool bCCW = SignedArea(InPts) > 0.0;
		auto Cross = [](const FVector2D& O, const FVector2D& A, const FVector2D& B) { return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X); };
		auto Inside = [&](const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
		{
			const double D1 = Cross(A, B, P), D2 = Cross(B, C, P), D3 = Cross(C, A, P);
			const bool bNeg = D1 < 0 || D2 < 0 || D3 < 0, bPos = D1 > 0 || D2 > 0 || D3 > 0;
			return !(bNeg && bPos);
		};
		int32 Guard = N * N;
		while (Idx.Num() > 3 && Guard-- > 0)
		{
			bool bClipped = false;
			for (int32 i = 0; i < Idx.Num(); ++i)
			{
				const int32 Ia = Idx[(i + Idx.Num() - 1) % Idx.Num()], Ib = Idx[i], Ic = Idx[(i + 1) % Idx.Num()];
				const FVector2D& A = InPts[Ia]; const FVector2D& B = InPts[Ib]; const FVector2D& C = InPts[Ic];
				const double Cr = Cross(A, B, C);
				if (bCCW ? Cr <= 0 : Cr >= 0) continue; // reflex vertex
				bool bEar = true;
				for (int32 k : Idx)
				{
					if (k == Ia || k == Ib || k == Ic) continue;
					if (Inside(InPts[k], A, B, C)) { bEar = false; break; }
				}
				if (!bEar) continue;
				Out.Add(Ia); Out.Add(Ib); Out.Add(Ic);
				Idx.RemoveAt(i);
				bClipped = true;
				break;
			}
			if (!bClipped) break; // degenerate polygon: give up on the rest
		}
		if (Idx.Num() == 3) { Out.Add(Idx[0]); Out.Add(Idx[1]); Out.Add(Idx[2]); }
		return Out;
	}

	void FlatPolygon(FSec& S, const TArray<FVector2D>& Ring, float Z, bool bFacingUp = true)
	{
		const TArray<int32> T = Triangulate(Ring);
		const FVector Up = bFacingUp ? FVector::UpVector : -FVector::UpVector;
		for (int32 i = 0; i + 2 < T.Num(); i += 3)
		{
			Tri(S, FVector(Ring[T[i]], Z), FVector(Ring[T[i + 1]], Z), FVector(Ring[T[i + 2]], Z), Up);
		}
	}

	/** Ground height for mesh building: the terrain's ground mesh, or flat 0 without terrain data. */
	inline float GroundAt(const FMRTerrain* T, double X, double Y) { return T ? T->GroundZ(X, Y) : 0.f; }

	/**
	 * Road/path/river ribbon draped on the terrain. The centre line is resampled every <= 4 m and
	 * wide roads get extra columns every <= 4 m across, and every vertex sits at the ground mesh
	 * height + Offset, so the terrain never pokes through and there is no gap to fall into.
	 * Bridge decks (DeckZ set by the pipeline, interpolated between the abutments) ignore the ground.
	 */
	void Ribbon(FSec& S, const TArray<FVector2D>& InPts, const TArray<float>& InDeckZ, float Width, float Offset, const FMRTerrain* T)
	{
		if (InPts.Num() < 2) return;
		const bool bDeck = InDeckZ.Num() == InPts.Num();
		TArray<FVector2D> Pts; TArray<float> DeckZ;
		constexpr double StepCm = 400.0;
		for (int32 i = 0; i + 1 < InPts.Num(); ++i)
		{
			const double Len = FVector2D::Distance(InPts[i], InPts[i + 1]);
			const int32 N = FMath::Max(1, FMath::CeilToInt(Len / StepCm));
			for (int32 k = 0; k < N; ++k)
			{
				const double A = static_cast<double>(k) / N;
				Pts.Add(FMath::Lerp(InPts[i], InPts[i + 1], A));
				if (bDeck) DeckZ.Add(FMath::Lerp(InDeckZ[i], InDeckZ[i + 1], static_cast<float>(A)));
			}
		}
		Pts.Add(InPts.Last());
		if (bDeck) DeckZ.Add(InDeckZ.Last());

		const float H = Width * 50.f; // half width in cm
		const int32 Cols = FMath::Max(2, FMath::CeilToInt(Width / 4.f) + 1);
		TArray<FVector> Grid; // Pts.Num() rows x Cols columns, left to right
		Grid.Reserve(Pts.Num() * Cols);
		// Bridge decks meet the ground at their ends. A deck less than 2.5 m above the ground lies on
		// the ground like any road, and takes its own height gradually over the next 2 m of clearance,
		// so there is never a step or a slab wall across the road where a bridge starts.
		TArray<float> Raise; // per row: 0 = on the ground, 1 = on the deck
		Raise.Init(0.f, Pts.Num());
		if (bDeck) for (int32 i = 0; i < Pts.Num(); ++i) Raise[i] = FMath::Clamp((DeckZ[i] - GroundAt(T, Pts[i].X, Pts[i].Y) - 250.f) / 200.f, 0.f, 1.f);
		for (int32 i = 0; i < Pts.Num(); ++i)
		{
			const FVector2D Prev = Pts[FMath::Max(0, i - 1)], Next = Pts[FMath::Min(Pts.Num() - 1, i + 1)];
			FVector2D Dir = (Next - Prev).GetSafeNormal();
			if (Dir.IsNearlyZero()) Dir = FVector2D(1, 0);
			const FVector2D Nrm(-Dir.Y, Dir.X);
			// Miter at joints, limited so sharp corners don't spike
			float Scale = 1.f;
			if (i > 0 && i < Pts.Num() - 1)
			{
				const FVector2D D0 = (Pts[i] - Pts[i - 1]).GetSafeNormal(), D1 = (Pts[i + 1] - Pts[i]).GetSafeNormal();
				const float CosHalf = FMath::Sqrt(FMath::Max(0.05f, (1.f + FVector2D::DotProduct(D0, D1)) * 0.5f));
				Scale = FMath::Min(1.f / CosHalf, 2.5f);
			}
			for (int32 c = 0; c < Cols; ++c)
			{
				const float Across = FMath::Lerp(H, -H, static_cast<float>(c) / (Cols - 1)) * Scale;
				const FVector2D P = Pts[i] + Nrm * Across;
				const float Z = FMath::Lerp(GroundAt(T, P.X, P.Y), bDeck ? DeckZ[i] : 0.f, Raise[i]) + Offset;
				Grid.Add(FVector(P, Z));
			}
		}
		// Crest pass: on convex ground (hill crests, humps) a flat quad between vertices can dip under
		// the terrain. Sample points inside every triangle and lift its corners until the ground is
		// at least 6 cm below. Verified on the real county data by Tools/geo_pipeline/test_ground.mjs.
		if (T) // decks too: where one lies on the ground it must clear it like any road
		{
			// Barycentric sample points per triangle (corners excluded): (i/4, j/4), i + j <= 4.
			TArray<FVector2f> Bary;
			for (int32 i = 0; i <= 4; ++i)
				for (int32 j = 0; i + j <= 4; ++j)
					if (!((i == 0 && j == 0) || i == 4 || j == 4)) Bary.Add(FVector2f(i / 4.f, j / 4.f));
			TArray<float> Lift;
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				Lift.Init(0.f, Grid.Num());
				for (int32 i = 0; i + 1 < Pts.Num(); ++i)
				{
					for (int32 c = 0; c + 1 < Cols; ++c)
					{
						const int32 Ids[4] = { i * Cols + c, i * Cols + c + 1, (i + 1) * Cols + c + 1, (i + 1) * Cols + c };
						const int32 TriIds[2][3] = { { Ids[0], Ids[1], Ids[2] }, { Ids[0], Ids[2], Ids[3] } }; // the two triangles emitted below
						for (const auto& TI : TriIds)
						{
							const FVector& A = Grid[TI[0]]; const FVector& B = Grid[TI[1]]; const FVector& C = Grid[TI[2]];
							float Need = 0.f;
							for (const FVector2f& W : Bary)
							{
								const FVector P = A + (B - A) * W.X + (C - A) * W.Y;
								Need = FMath::Max(Need, T->GroundZ(P.X, P.Y) + 6.f - static_cast<float>(P.Z));
							}
							if (Need > 0.f) for (const int32 K : TI) Lift[K] = FMath::Max(Lift[K], Need);
						}
					}
				}
				for (int32 K = 0; K < Grid.Num(); ++K) Grid[K].Z += Lift[K];
			}
		}

		for (int32 i = 0; i + 1 < Pts.Num(); ++i)
		{
			for (int32 c = 0; c + 1 < Cols; ++c)
			{
				const FVector& A = Grid[i * Cols + c]; const FVector& B = Grid[i * Cols + c + 1];
				const FVector& C = Grid[(i + 1) * Cols + c + 1]; const FVector& D = Grid[(i + 1) * Cols + c];
				Tri(S, A, B, C, FVector::UpVector);
				Tri(S, A, C, D, FVector::UpVector);
			}
		}
		if (bDeck)
		{
			// Deck thickness + side faces so a bridge is a solid slab, not a sheet you can clip into from below.
			constexpr float Thick = 80.f;
			for (int32 i = 0; i + 1 < Pts.Num(); ++i)
			{
				if (Raise[i] <= 0.f && Raise[i + 1] <= 0.f) continue; // lying on the ground: no slab
				for (const int32 c : { 0, Cols - 1 })
				{
					const FVector A = Grid[i * Cols + c], D = Grid[(i + 1) * Cols + c];
					const FVector Out = FVector(FVector2D(A - Grid[i * Cols + (c == 0 ? Cols - 1 : 0)]).GetSafeNormal(), 0.f);
					Tri(S, A, D, D - FVector(0, 0, Thick), Out);
					Tri(S, A, D - FVector(0, 0, Thick), A - FVector(0, 0, Thick), Out);
				}
				const FVector L0 = Grid[i * Cols] - FVector(0, 0, Thick), R0 = Grid[i * Cols + Cols - 1] - FVector(0, 0, Thick);
				const FVector L1 = Grid[(i + 1) * Cols] - FVector(0, 0, Thick), R1 = Grid[(i + 1) * Cols + Cols - 1] - FVector(0, 0, Thick);
				Tri(S, L0, R0, R1, -FVector::UpVector);
				Tri(S, L0, R1, L1, -FVector::UpVector);
			}
		}
	}

	/** Point-in-polygon (ray casting) for area painting. */
	bool InRing(const TArray<FVector2D>& Ring, const FBox2D& Bounds, const FVector2D& P)
	{
		if (!Bounds.IsInside(P)) return false;
		bool bIn = false;
		for (int32 i = 0, j = Ring.Num() - 1; i < Ring.Num(); j = i++)
		{
			const FVector2D& A = Ring[i]; const FVector2D& B = Ring[j];
			if ((A.Y > P.Y) != (B.Y > P.Y) && P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X) bIn = !bIn;
		}
		return bIn;
	}

	void Extrude(FSec& S, const TArray<FVector2D>& Ring, float ZMin, float ZMax)
	{
		const bool bCCW = SignedArea(Ring) > 0.0;
		for (int32 i = 0; i < Ring.Num(); ++i)
		{
			const FVector2D A2 = Ring[i], B2 = Ring[(i + 1) % Ring.Num()];
			const FVector2D Edge = (B2 - A2).GetSafeNormal();
			if (Edge.IsNearlyZero()) continue;
			// Outward normal depends on ring orientation
			const FVector2D Out2 = bCCW ? FVector2D(Edge.Y, -Edge.X) : FVector2D(-Edge.Y, Edge.X);
			const FVector Out(Out2, 0.f);
			const FVector A(A2, ZMin), B(B2, ZMin), C(B2, ZMax), D(A2, ZMax);
			Tri(S, A, B, C, Out);
			Tri(S, A, C, D, Out);
		}
		FlatPolygon(S, Ring, ZMax, true);
	}

	TArray<FVector2D> ReadPts(const TArray<TSharedPtr<FJsonValue>>& Arr)
	{
		TArray<FVector2D> P;
		P.Reserve(Arr.Num());
		for (const TSharedPtr<FJsonValue>& V : Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>& XY = V->AsArray();
			if (XY.Num() >= 2) P.Add(FVector2D(XY[0]->AsNumber(), XY[1]->AsNumber()));
		}
		return P;
	}

	/** Bridge deck heights: the pipeline writes [x, y, z] for bridge points. Empty if any point lacks z. */
	TArray<float> ReadDeckZ(const TArray<TSharedPtr<FJsonValue>>& Arr)
	{
		TArray<float> Z;
		for (const TSharedPtr<FJsonValue>& V : Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>& XYZ = V->AsArray();
			if (XYZ.Num() < 3) return TArray<float>();
			Z.Add(static_cast<float>(XYZ[2]->AsNumber()));
		}
		return Z;
	}

	int32 RoadGroup(const FString& Kind, const FString& Cls, int32 Surface)
	{
		if (Kind.StartsWith(TEXT("railway"))) return Rail;
		if (Kind.StartsWith(TEXT("waterway"))) return Water;
		switch (static_cast<ESurface>(Surface))
		{
		case ESurface::Cobbles: return Cobbles;
		case ESurface::Gravel: return Gravel;
		case ESurface::Dirt: case ESurface::Mud: return DirtPath;
		case ESurface::Grass: return Grass;
		default: break;
		}
		if (Kind == TEXT("motorway") || Kind == TEXT("major")) return MajorRoad;
		if (Kind == TEXT("cycleway")) return CycleLane;
		if (Kind == TEXT("footway") || Kind == TEXT("pedestrian") || Kind == TEXT("steps")) return Paving;
		if (Kind == TEXT("path")) return DirtPath;
		if (Kind == TEXT("track")) return Gravel;
		return MinorRoad;
	}

	int32 BuildingGroup(const FString& Style, const FString& Material)
	{
		if (Material.Contains(TEXT("brick"))) return Brick;
		if (Material.Contains(TEXT("glass"))) return Glass;
		if (Style == TEXT("office")) return Glass;
		if (Style == TEXT("industrial") || Style == TEXT("garage")) return Industrial;
		if (Style == TEXT("commercial") || Style == TEXT("retail") || Style == TEXT("civic") || Style == TEXT("stadium") || Style == TEXT("generic")) return Stone;
		return Brick; // houses, terraces, semis, mills, churches
	}

	// ------------------------------------------------------------------ street detail
	// Deterministic 0..1 noise from a position, so every machine builds identical props and trees.
	float Hash01(double X, double Y, uint32 Salt = 0)
	{
		uint32 H = HashCombine(GetTypeHash(FMath::FloorToInt32(X)), GetTypeHash(FMath::FloorToInt32(Y)));
		H = HashCombine(H, GetTypeHash(Salt));
		return static_cast<float>(H & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
	}

	// ---------------------------------------------------------------- buildings: colour and roofs
	/** Per-building wall colour (linear RGB) from palettes of real Greater Manchester materials. */
	FLinearColor BuildingColour(int32 Group, const FString& Style, float R)
	{
		struct FPick { float Weight; FLinearColor C; };
		auto Choose = [R](std::initializer_list<FPick> L)
		{
			float Sum = 0.f; for (const FPick& P : L) Sum += P.Weight;
			float X = R * Sum;
			for (const FPick& P : L) { if (X < P.Weight) return P.C; X -= P.Weight; }
			return L.begin()->C;
		};
		if (Group == Glass) return Choose({ { 3, FLinearColor(0.16f, 0.2f, 0.24f) }, { 2, FLinearColor(0.08f, 0.09f, 0.1f) }, { 1, FLinearColor(0.12f, 0.18f, 0.18f) }, { 1, FLinearColor(0.35f, 0.37f, 0.4f) } });
		if (Group == Industrial) return Choose({ { 3, FLinearColor(0.28f, 0.3f, 0.31f) }, { 2, FLinearColor(0.15f, 0.22f, 0.3f) }, { 1, FLinearColor(0.15f, 0.25f, 0.18f) }, { 2, FLinearColor(0.45f, 0.42f, 0.36f) }, { 3, FLinearColor(0.32f, 0.12f, 0.07f) } });
		if (Group == Stone) return Choose({ { 3, FLinearColor(0.55f, 0.45f, 0.32f) }, { 2, FLinearColor(0.7f, 0.68f, 0.62f) }, { 2, FLinearColor(0.45f, 0.45f, 0.45f) }, { 2, FLinearColor(0.45f, 0.17f, 0.1f) }, { 2, FLinearColor(0.36f, 0.13f, 0.08f) } });
		// Brick: Victorian/Edwardian terraces and 1930s semis: mostly red brick, some render and pebble-dash.
		const bool bSuburban = Style == TEXT("semi") || Style == TEXT("detached") || Style == TEXT("bungalow");
		return Choose({ { 5, FLinearColor(0.36f, 0.13f, 0.08f) }, { 3, FLinearColor(0.26f, 0.09f, 0.06f) }, { 2, FLinearColor(0.45f, 0.18f, 0.09f) },
			{ 2, FLinearColor(0.28f, 0.15f, 0.09f) }, { 1, FLinearColor(0.18f, 0.09f, 0.07f) }, { 0.5f, FLinearColor(0.55f, 0.42f, 0.28f) },
			{ bSuburban ? 2.f : 0.6f, FLinearColor(0.75f, 0.73f, 0.68f) }, { bSuburban ? 1.6f : 0.3f, FLinearColor(0.45f, 0.43f, 0.4f) } });
	}

	/** Pads the section's colour array so every vertex added since the last call gets this building's colour. */
	void PaintNewVerts(FSec& S, const FLinearColor& Wall, bool bTileRoof)
	{
		FColor C = Wall.ToFColor(false); // vertex colours reach the shader as linear values
		C.A = bTileRoof ? 0 : 255; // facade shader: A = 255 slate/felt roof, 0 red clay tiles
		while (S.C.Num() < S.V.Num()) S.C.Add(C);
	}

	/**
	 * Gabled or hipped roof over the footprint's minimum-area bounding rectangle, ridge along the long
	 * side (how terrace rows and semis are roofed). Pitch ~35 degrees, capped at 4.5 m, 30 cm eaves.
	 */
	void PitchedRoof(FSec& S, const TArray<FVector2D>& Ring, float EavesZ, bool bHipped)
	{
		// Minimum-area rectangle: try each edge direction.
		double BestArea = TNumericLimits<double>::Max();
		FVector2D U(1, 0), Ctr(0, 0); double HalfA = 0, HalfB = 0;
		for (int32 i = 0; i < Ring.Num(); ++i)
		{
			const FVector2D E = (Ring[(i + 1) % Ring.Num()] - Ring[i]).GetSafeNormal();
			if (E.IsNearlyZero()) continue;
			const FVector2D W(-E.Y, E.X);
			double MinU = 1e18, MaxU = -1e18, MinW = 1e18, MaxW = -1e18;
			for (const FVector2D& P : Ring)
			{
				const double Pu = FVector2D::DotProduct(P, E), Pw = FVector2D::DotProduct(P, W);
				MinU = FMath::Min(MinU, Pu); MaxU = FMath::Max(MaxU, Pu); MinW = FMath::Min(MinW, Pw); MaxW = FMath::Max(MaxW, Pw);
			}
			const double Area = (MaxU - MinU) * (MaxW - MinW);
			if (Area < BestArea)
			{
				BestArea = Area;
				const bool bLongU = (MaxU - MinU) >= (MaxW - MinW);
				const FVector2D Mid = E * ((MinU + MaxU) * 0.5) + W * ((MinW + MaxW) * 0.5);
				Ctr = Mid;
				U = bLongU ? E : W;
				HalfA = (bLongU ? (MaxU - MinU) : (MaxW - MinW)) * 0.5;
				HalfB = (bLongU ? (MaxW - MinW) : (MaxU - MinU)) * 0.5;
			}
		}
		if (HalfB < 150.0) return; // sheds etc.: leave flat
		const FVector2D Wd(-U.Y, U.X);
		const double Oh = 30.0;
		const double A = HalfA + Oh, B = HalfB + Oh;
		const float Rise = static_cast<float>(FMath::Min(HalfB * 0.7, 450.0)); // tan(35 deg) ~ 0.7
		auto P3 = [&](double Su, double Sw, float Z) { const FVector2D P = Ctr + U * Su + Wd * Sw; return FVector(P.X, P.Y, Z); };
		const float Ez = EavesZ - 15.f;
		const FVector E1 = P3(-A, -B, Ez), E2 = P3(A, -B, Ez), E3 = P3(A, B, Ez), E4 = P3(-A, B, Ez);
		const double Ridge = bHipped ? FMath::Max(A - B, 0.0) : A;
		const FVector R1 = P3(-Ridge, 0, EavesZ + Rise), R2 = P3(Ridge, 0, EavesZ + Rise);
		const FVector UpS = FVector(-Wd.X, -Wd.Y, 1.f).GetSafeNormal(), UpN = FVector(Wd.X, Wd.Y, 1.f).GetSafeNormal();
		// Two long slopes (both faces, so the eaves read from below too).
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FVector N = Side == 0 ? UpS : UpN;
			const FVector Ea = Side == 0 ? E1 : E4, Eb = Side == 0 ? E2 : E3;
			Tri(S, Ea, Eb, R2, N); Tri(S, Ea, R2, R1, N);
			Tri(S, Ea, R2, Eb, -N); Tri(S, Ea, R1, R2, -N);
		}
		if (bHipped)
		{
			const FVector Nw = FVector(-U.X, -U.Y, 1.f).GetSafeNormal(), Ne = FVector(U.X, U.Y, 1.f).GetSafeNormal();
			Tri(S, E1, E4, R1, Nw); Tri(S, E2, E3, R2, Ne);
		}
		else
		{
			// Gable walls: brick triangles flush with the walls.
			const FVector G1a = P3(-HalfA, -HalfB, EavesZ), G1b = P3(-HalfA, HalfB, EavesZ), G1t = P3(-HalfA, 0, EavesZ + Rise * (HalfB / B));
			const FVector G2a = P3(HalfA, -HalfB, EavesZ), G2b = P3(HalfA, HalfB, EavesZ), G2t = P3(HalfA, 0, EavesZ + Rise * (HalfB / B));
			Tri(S, G1a, G1b, G1t, FVector(-U, 0.f));
			Tri(S, G2a, G2b, G2t, FVector(U, 0.f));
		}
	}

	// ---------------------------------------------------------------- landmarks
	// Recognisable models of Manchester's landmarks, built on their real OSM footprints (or, for the two
	// stadiums, on the real pitches measured from the map data). Heights are the real ones where OSM's are wrong.

	void Paint(FSec& S, const FLinearColor& C, uint8 RoofA = 255)
	{
		FColor F = C.ToFColor(false);
		F.A = RoofA; // facade shader roof: 255 slate/lead, 128 copper, 0 red tile
		while (S.C.Num() < S.V.Num()) S.C.Add(F);
	}

	struct FRect { FVector2D C, U; double HA = 0, HB = 0; };
	FRect MinRect(const TArray<FVector2D>& Ring)
	{
		FRect Best; double BestArea = TNumericLimits<double>::Max();
		for (int32 i = 0; i < Ring.Num(); ++i)
		{
			const FVector2D E = (Ring[(i + 1) % Ring.Num()] - Ring[i]).GetSafeNormal();
			if (E.IsNearlyZero()) continue;
			const FVector2D W(-E.Y, E.X);
			double A0 = 1e18, A1 = -1e18, B0 = 1e18, B1 = -1e18;
			for (const FVector2D& P : Ring)
			{
				const double a = FVector2D::DotProduct(P, E), b = FVector2D::DotProduct(P, W);
				A0 = FMath::Min(A0, a); A1 = FMath::Max(A1, a); B0 = FMath::Min(B0, b); B1 = FMath::Max(B1, b);
			}
			const double Area = (A1 - A0) * (B1 - B0);
			if (Area >= BestArea) continue;
			BestArea = Area;
			const bool bLong = (A1 - A0) >= (B1 - B0);
			Best.C = E * ((A0 + A1) * 0.5) + W * ((B0 + B1) * 0.5);
			Best.U = bLong ? E : W;
			Best.HA = (bLong ? A1 - A0 : B1 - B0) * 0.5;
			Best.HB = (bLong ? B1 - B0 : A1 - A0) * 0.5;
		}
		return Best;
	}

	FVector2D RingCentroid(const TArray<FVector2D>& Ring)
	{
		FVector2D S(0, 0);
		for (const FVector2D& P : Ring) S += P;
		return Ring.Num() ? S / Ring.Num() : S;
	}

	TArray<FVector2D> GrowRing(const TArray<FVector2D>& Ring, double D)
	{
		const FVector2D C = RingCentroid(Ring);
		TArray<FVector2D> O;
		for (const FVector2D& P : Ring) O.Add(P + (P - C).GetSafeNormal() * D);
		return O;
	}

	/** Rounded rectangle around C (long axis U), same vertex count for any size so rings can be lofted. */
	TArray<FVector2D> RoundRect(const FVector2D& C, const FVector2D& U, double HA, double HB, double R, int32 Seg = 8)
	{
		const FVector2D W(-U.Y, U.X);
		const double Ax[4] = { HA - R, -(HA - R), -(HA - R), HA - R }, Bx[4] = { HB - R, HB - R, -(HB - R), -(HB - R) };
		TArray<FVector2D> Out;
		for (int32 k = 0; k < 4; ++k)
			for (int32 s = 0; s <= Seg; ++s)
			{
				const double A = (k * 90.0 + 90.0 * s / Seg) * PI / 180.0;
				Out.Add(C + U * (Ax[k] + R * FMath::Cos(A)) + W * (Bx[k] + R * FMath::Sin(A)));
			}
		return Out;
	}

	/** Surface between two rings of equal count; normals lean towards (Inward > 0) or away from the centre, plus UpBias. */
	void Loft(FSec& S, const TArray<FVector2D>& A, float Za, const TArray<FVector2D>& B, float Zb, const FVector2D& Ctr, float Inward, float UpBias, bool bTwoSided = false)
	{
		const int32 N = FMath::Min(A.Num(), B.Num());
		for (int32 i = 0; i < N; ++i)
		{
			const int32 j = (i + 1) % N;
			const FVector2D Mid = (A[i] + A[j] + B[i] + B[j]) * 0.25;
			const FVector2D ToC = (Ctr - Mid).GetSafeNormal() * Inward;
			const FVector Nrm = FVector(ToC.X, ToC.Y, UpBias).GetSafeNormal();
			const FVector Ai(A[i], Za), Aj(A[j], Za), Bi(B[i], Zb), Bj(B[j], Zb);
			Tri(S, Ai, Aj, Bj, Nrm); Tri(S, Ai, Bj, Bi, Nrm);
			if (bTwoSided) { Tri(S, Ai, Aj, Bj, -Nrm); Tri(S, Ai, Bj, Bi, -Nrm); }
		}
	}

	void Pyramid(FSec& S, const FVector& Base, const FVector2D& U, double Half, float Height)
	{
		const FVector2D W(-U.Y, U.X);
		const FVector Top = Base + FVector(0, 0, Height);
		const FVector2D Dirs[4] = { U, W, -U, -W };
		for (int32 k = 0; k < 4; ++k)
		{
			const FVector2D Ax = Dirs[k], Side = Dirs[(k + 1) % 4];
			const FVector P0 = Base + FVector(Ax * Half + Side * Half, 0.f), P1 = Base + FVector(Ax * Half - Side * Half, 0.f);
			Tri(S, P0, P1, Top, FVector(Ax.X, Ax.Y, 0.6f).GetSafeNormal());
		}
	}

	/** Victorian clock tower: shaft, four clock faces (lit at night), corner pinnacles, slate spire. */
	void ClockTower(FSec* Secs, const FVector& Base, const FVector2D& U, double Half, float Shaft, float Spire, const FLinearColor& StoneC)
	{
		using namespace MRMeshKit;
		const FVector2D W(-U.Y, U.X);
		const FQuat Q = FRotationMatrix::MakeFromXY(FVector(U, 0.f), FVector(W, 0.f)).ToQuat();
		Box(Secs[Stone], Base + FVector(0, 0, Shaft * 0.5f), Q, FVector(Half, Half, Shaft * 0.5f));
		Box(Secs[Stone], Base + FVector(0, 0, Shaft + 60.f), Q, FVector(Half + 60.0, Half + 60.0, 60.f)); // cornice
		for (int32 k = 0; k < 4; ++k)
		{
			const FVector2D Ax = k == 0 ? U : k == 1 ? W : k == 2 ? -U : -W;
			Disc(Secs[LampHead], Base + FVector(Ax * (Half + 15.0), Shaft * 0.8f), FVector(Ax, 0.f), 0.f, static_cast<float>(Half * 0.62), 20.f, 32);
			const FVector2D Corner = (Ax + (k % 2 ? U : W)) * Half;
			Tube(Secs[Stone], Base + FVector(Corner, Shaft), Base + FVector(Corner, Shaft + Spire * 0.35f), 60.f, 25.f, 8, true); // pinnacles
		}
		Pyramid(Secs[PropMetal], Base + FVector(0, 0, Shaft + 120.f), U, Half * 0.85, Spire); // lead/slate spire (too steep for the roof shading)
		Paint(Secs[Stone], StoneC, 255);
	}

	struct FStadium { const TCHAR* Name; FVector2D C; float YawDeg; float HA, HB; bool bCity; };
	// Real pitch centres, orientation and size measured from the OSM pitch polygons (Tools: see DEVELOPMENT_STATUS).
	const FStadium Stadiums[] = {
		{ TEXT("Old Trafford"), FVector2D(-308894, 181195), 160.7f, 5275.f, 3392.f, false },
		{ TEXT("Etihad Stadium"), FVector2D(294992, -42466), 79.9f, 5242.f, 3383.f, true },
	};

	bool InStadium(const FVector2D& P)
	{
		for (const FStadium& St : Stadiums)
		{
			const FVector2D U(FMath::Cos(FMath::DegreesToRadians(St.YawDeg)), FMath::Sin(FMath::DegreesToRadians(St.YawDeg))), W(-U.Y, U.X);
			const FVector2D D = P - St.C;
			if (FMath::Abs(FVector2D::DotProduct(D, U)) < St.HA + 5600.f && FMath::Abs(FVector2D::DotProduct(D, W)) < St.HB + 5600.f) return true;
		}
		return false;
	}

	void BuildStadium(FSec* Secs, const FMRTerrain* T, const FStadium& St)
	{
		using namespace MRMeshKit;
		const FVector2D U(FMath::Cos(FMath::DegreesToRadians(St.YawDeg)), FMath::Sin(FMath::DegreesToRadians(St.YawDeg))), W(-U.Y, U.X);
		const FVector2D C = St.C;
		const float G = GroundAt(T, C.X, C.Y);
		const int32 SeatG = St.bCity ? PropBlue : PropRed;
		const TArray<FVector2D> Inner = RoundRect(C, U, St.HA + 600, St.HB + 600, 900);
		const TArray<FVector2D> Mid = RoundRect(C, U, St.HA + 2900, St.HB + 2900, 2700);
		const TArray<FVector2D> Outer = RoundRect(C, U, St.HA + 4900, St.HB + 4900, 4500);
		const TArray<FVector2D> RoofIn = RoundRect(C, U, St.HA + 1800, St.HB + 1800, 1700);
		Loft(Secs[SeatG], Inner, G + 100.f, Mid, G + 1400.f, C, 1.f, 1.2f);                 // lower tier
		Loft(Secs[PropMetal], Mid, G + 1400.f, Mid, G + 1750.f, C, 1.f, 0.f);                // tier front / executive boxes
		Loft(Secs[SeatG], Mid, G + 1750.f, Outer, G + 4300.f, C, 1.f, 1.2f);                 // upper tier
		Loft(Secs[PropMetal], Outer, G - 150.f, Outer, G + 4700.f, C, -1.f, 0.f);            // outer cladding
		Loft(Secs[PropWhite], Outer, G + 5000.f, RoofIn, G + 4600.f, C, 0.f, 1.f, true);     // cantilever roof
		Loft(Secs[PropMetal], Outer, G + 4300.f, Outer, G + 5000.f, C, 1.f, 0.f);            // roof fascia
		for (int32 i = 0; i < Outer.Num(); i += 2)                                           // roof trusses
			Tube(Secs[PropWhite], FVector(Outer[i], G + 5700.f), FVector(RoofIn[i], G + 4700.f), 45.f, 30.f, 6, false);
		// Pitch: mown stripes, touchlines, halfway line, centre circle, penalty boxes.
		const FQuat Q = FRotationMatrix::MakeFromXY(FVector(U, 0.f), FVector(W, 0.f)).ToQuat();
		for (int32 k = 0; k < 14; ++k)
		{
			const double A0 = -St.HA - 300 + (2.0 * St.HA + 600) * k / 14.0, A1 = -St.HA - 300 + (2.0 * St.HA + 600) * (k + 1) / 14.0;
			Box(Secs[k % 2 ? Grass : Woodland], FVector(C + U * ((A0 + A1) * 0.5), G + 18.f), Q, FVector((A1 - A0) * 0.5, St.HB + 300.0, 2.0));
		}
		auto Line = [&](const FVector2D& A, const FVector2D& B) { Bar(Secs[MarkWhite], FVector(A, G + 22.f), FVector(B, G + 22.f), 12.f, 1.f); };
		const FVector2D Ca = C + U * St.HA, Cb = C - U * St.HA;
		Line(Ca + W * St.HB, Cb + W * St.HB); Line(Ca - W * St.HB, Cb - W * St.HB);
		Line(Ca + W * St.HB, Ca - W * St.HB); Line(Cb + W * St.HB, Cb - W * St.HB);
		Line(C + W * St.HB, C - W * St.HB);
		Torus(Secs[MarkWhite], FVector(C, G + 22.f), FVector::UpVector, 915.f, 6.f, 48, 4, 0.2f);
		for (const float Sg : { 1.f, -1.f })
		{
			const FVector2D E = C + U * (Sg * St.HA), In = C + U * (Sg * (St.HA - 1650.f));
			Line(E + W * 2016.f, In + W * 2016.f); Line(E - W * 2016.f, In - W * 2016.f); Line(In + W * 2016.f, In - W * 2016.f);
		}
		if (St.bCity)
		{
			// The Etihad's twelve cable masts around the outside, each guying the roof.
			for (int32 m = 0; m < 12; ++m)
			{
				const int32 i = (m * Outer.Num()) / 12;
				const FVector2D Out2 = (Outer[i] - C).GetSafeNormal();
				const FVector Foot(Outer[i] + Out2 * 1400.f, G - 50.f), Top(Outer[i] + Out2 * 1100.f, G + 7600.f);
				Tube(Secs[PropWhite], Foot, Top, 140.f, 60.f, 10, true);
				for (const int32 d : { -2, 2 })
				{
					const int32 k = (i + d + Outer.Num()) % Outer.Num();
					Tube(Secs[PropWhite], Top, FVector(Outer[k], G + 5000.f), 9.f, 9.f, 4, false);
				}
				Tube(Secs[PropWhite], Top - FVector(0, 0, 600.f), FVector(RoofIn[i], G + 4650.f), 9.f, 9.f, 4, false);
			}
		}
	}

	/** Named landmark on its real footprint. Returns true when it replaced the plain extrusion. */
	bool BuildNamedLandmark(FSec* Secs, const FMRTerrain* T, const FString& Name, const TArray<FVector2D>& Ring, float GMin)
	{
		using namespace MRMeshKit;
		const FRect R = MinRect(Ring);
		const FVector2D Cn = RingCentroid(Ring);
		auto At = [&](const FVector2D& P, float Up = 0.f) { return FVector(P.X, P.Y, GroundAt(T, P.X, P.Y) + Up); };
		const FLinearColor Sandstone(0.42f, 0.35f, 0.26f), DarkStone(0.27f, 0.21f, 0.16f), RedStone(0.3f, 0.13f, 0.1f), Portland(0.7f, 0.68f, 0.62f);

		if (Name == TEXT("Beetham Tower"))
		{
			// 169 m; the upper floors cantilever 4 m out from the 23rd floor; glass blade on the roof.
			Extrude(Secs[Glass], Ring, GMin - 100.f, GMin + 7500.f);
			Extrude(Secs[Glass], GrowRing(Ring, 250.0), GMin + 7500.f, GMin + 15700.f);
			Bar(Secs[Glass], FVector(R.C - R.U * (R.HA * 0.95), GMin + 16300.f), FVector(R.C + R.U * (R.HA * 0.95), GMin + 16300.f), 90.f, 1300.f);
			Paint(Secs[Glass], FLinearColor(0.11f, 0.14f, 0.19f));
			return true;
		}
		if (Name == TEXT("Co-operative Insurance Tower"))
		{
			Extrude(Secs[Glass], Ring, GMin - 100.f, GMin + 10100.f); // 25 storeys, photovoltaic cladding
			Paint(Secs[Glass], FLinearColor(0.06f, 0.07f, 0.09f));
			return true;
		}
		if (Name == TEXT("CIS Tower"))
		{
			Extrude(Secs[Stone], Ring, GMin - 100.f, GMin + 11800.f); // the concrete service tower, 118 m
			Paint(Secs[Stone], FLinearColor(0.4f, 0.4f, 0.4f));
			return true;
		}
		if (Name == TEXT("Manchester Central Library"))
		{
			// Rotunda with an upper colonnade, entablature and shallow lead dome.
			const float G = GroundAt(T, Cn.X, Cn.Y);
			const float Rad = static_cast<float>((R.HA + R.HB) * 0.5 * 0.93);
			Tube(Secs[Stone], FVector(Cn, G - 100.f), FVector(Cn, G + 1400.f), Rad, Rad, 64, false);
			Tube(Secs[Stone], FVector(Cn, G + 1400.f), FVector(Cn, G + 2450.f), Rad - 300.f, Rad - 300.f, 64, false);
			Disc(Secs[Stone], FVector(Cn, G + 2500.f), FVector::UpVector, 0.f, Rad, 120.f, 64);
			Ellipsoid(Secs[Stone], FVector(Cn, G + 2560.f), FVector(Rad - 450.f, Rad - 450.f, 950.f), FQuat::Identity, 40, 14);
			Tube(Secs[Stone], FVector(Cn, G + 3400.f), FVector(Cn, G + 3900.f), 320.f, 320.f, 16, true);
			Paint(Secs[Stone], Portland);
			for (int32 k = 0; k < 44; ++k)
			{
				const double A = 2.0 * PI * k / 44.0;
				const FVector2D P = Cn + FVector2D(FMath::Cos(A), FMath::Sin(A)) * (Rad - 140.f);
				Tube(Secs[PropWhite], FVector(P, G + 1400.f), FVector(P, G + 2440.f), 48.f, 42.f, 10, false);
			}
			return true;
		}
		if (Name.Contains(TEXT("Town Hall")))
		{
			const bool bManchester = Name == TEXT("Manchester Town Hall");
			const float H = bManchester ? 3000.f : 1900.f;
			Extrude(Secs[Stone], Ring, GMin - 100.f, GMin + H);
			Paint(Secs[Stone], bManchester ? Sandstone : FLinearColor(0.45f, 0.4f, 0.32f));
			FVector2D TowerAt = Cn;
			if (bManchester)
			{
				// The 85 m clock tower stands on the Albert Square front: the footprint point nearest the square.
				double X = 0, Y = 0;
				MRGeo::LocalToUnreal(MRGeo::ToLocal(MRGeo::FLatLon{ 53.47975, -2.24525 }), X, Y);
				const FVector2D Square(X, Y);
				double Best = 1e18;
				for (const FVector2D& P : Ring) { const double D = FVector2D::DistSquared(P, Square); if (D < Best) { Best = D; TowerAt = P; } }
				TowerAt += (Cn - TowerAt).GetSafeNormal() * 900.0;
			}
			ClockTower(Secs, At(TowerAt), R.U, bManchester ? 650.0 : 500.0, bManchester ? 6800.f : 4300.f, bManchester ? 1800.f : 1100.f, bManchester ? Sandstone : FLinearColor(0.45f, 0.4f, 0.32f));
			return true;
		}
		if (Name == TEXT("Manchester Cathedral"))
		{
			Extrude(Secs[Stone], Ring, GMin - 100.f, GMin + 1800.f);
			PitchedRoof(Secs[Stone], Ring, GMin + 1800.f, false);
			// West tower (39 m) with battlements, at the west end of the footprint.
			FVector2D West = Ring[0];
			for (const FVector2D& P : Ring) if (P.X < West.X) West = P;
			West += (Cn - West).GetSafeNormal() * 500.0;
			const FQuat Q = FRotationMatrix::MakeFromX(FVector(R.U, 0.f)).ToQuat();
			const FVector B = At(West);
			Box(Secs[Stone], B + FVector(0, 0, 1950.f), Q, FVector(450.f, 450.f, 1950.f));
			for (int32 k = -2; k <= 2; ++k)
				for (const float Sg : { -1.f, 1.f })
				{
					Box(Secs[Stone], B + Q.RotateVector(FVector(k * 180.f, Sg * 450.f, 3960.f)), Q, FVector(55.f, 30.f, 70.f));
					Box(Secs[Stone], B + Q.RotateVector(FVector(Sg * 450.f, k * 180.f, 3960.f)), Q, FVector(30.f, 55.f, 70.f));
				}
			Paint(Secs[Stone], DarkStone);
			return true;
		}
		if (Name == TEXT("John Rylands Library"))
		{
			Extrude(Secs[Stone], Ring, GMin - 100.f, GMin + 2400.f);
			PitchedRoof(Secs[Stone], Ring, GMin + 2400.f, false);
			const FVector2D W(-R.U.Y, R.U.X);
			for (const FVector2D& Sg : { FVector2D(1, 1), FVector2D(1, -1), FVector2D(-1, 1), FVector2D(-1, -1) })
			{
				const FVector2D P = R.C + R.U * (Sg.X * (R.HA - 150.0)) + W * (Sg.Y * (R.HB - 150.0));
				Tube(Secs[Stone], At(P, -100.f), At(P, 2900.f), 230.f, 230.f, 12, false);
				Pyramid(Secs[PropMetal], At(P, 2900.f), R.U, 240.0, 700.f);
			}
			Paint(Secs[Stone], RedStone);
			return true;
		}
		if (Name == TEXT("The Lowry"))
		{
			Extrude(Secs[Glass], Ring, GMin - 100.f, GMin + 1800.f);
			Tube(Secs[Glass], At(Cn + R.U * (R.HA * 0.4), -100.f), At(Cn + R.U * (R.HA * 0.4), 4000.f), 650.f, 650.f, 24, true); // steel-clad tower
			Paint(Secs[Glass], FLinearColor(0.5f, 0.52f, 0.56f));
			return true;
		}
		if (Name == TEXT("Imperial War Museum North"))
		{
			Extrude(Secs[Industrial], Ring, GMin - 100.f, GMin + 2000.f);
			const FVector2D P = Cn + R.U * (R.HA * 0.55);
			Box(Secs[Industrial], At(P, 2700.f), FQuat(FVector(R.U, 0.f), FMath::DegreesToRadians(6.f)) * FRotationMatrix::MakeFromX(FVector(R.U, 0.f)).ToQuat(), FVector(450.f, 1100.f, 2800.f)); // the Air Shard
			Paint(Secs[Industrial], FLinearColor(0.58f, 0.6f, 0.63f));
			return true;
		}
		if (Name == TEXT("The Trafford Centre"))
		{
			Extrude(Secs[Stone], Ring, GMin - 100.f, GMin + 1500.f);
			Paint(Secs[Stone], FLinearColor(0.72f, 0.64f, 0.5f), 255); // cream stone, grey flat roofs
			Ellipsoid(Secs[Stone], At(R.C, 1500.f), FVector(2600.f, 2600.f, 1900.f), FQuat::Identity, 36, 12);
			const FVector2D W(-R.U.Y, R.U.X);
			for (const FVector2D& Sg : { FVector2D(0.55, 0), FVector2D(-0.55, 0), FVector2D(0.25, 0.5), FVector2D(-0.25, -0.5) })
				Ellipsoid(Secs[Stone], At(R.C + R.U * (Sg.X * R.HA) + W * (Sg.Y * R.HB), 1500.f), FVector(1000.f, 1000.f, 800.f), FQuat::Identity, 20, 8);
			Paint(Secs[Stone], FLinearColor(0.72f, 0.64f, 0.5f), 128); // cream stone, verdigris domes
			return true;
		}
		return false;
	}

	struct FRoadSeg { FVector2D A, B; float HalfW; float Z; };

	bool NearestRoad(const TArray<FRoadSeg>& Segs, const FVector2D& P, double MaxDist, FVector2D& OutDir, FVector2D& OutClosest, const FRoadSeg*& OutSeg)
	{
		double Best = MaxDist;
		OutSeg = nullptr;
		for (const FRoadSeg& S : Segs)
		{
			const FVector2D C = FMath::ClosestPointOnSegment2D(P, S.A, S.B);
			const double D = FVector2D::Distance(P, C);
			if (D < Best) { Best = D; OutClosest = C; OutDir = (S.B - S.A).GetSafeNormal(); OutSeg = &S; }
		}
		return OutSeg != nullptr;
	}

	/** Polyline offset sideways by D cm (+ = left of travel). */
	TArray<FVector2D> OffsetLine(const TArray<FVector2D>& P, double D)
	{
		TArray<FVector2D> O;
		O.Reserve(P.Num());
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const FVector2D Tg = (P[FMath::Min(i + 1, P.Num() - 1)] - P[FMath::Max(i - 1, 0)]).GetSafeNormal();
			O.Add(P[i] + FVector2D(-Tg.Y, Tg.X) * D);
		}
		return O;
	}

	/** Splits a polyline into dashes (Mark cm long, Gap cm apart); keeps bridge deck heights. */
	void Dashes(const TArray<FVector2D>& P, const TArray<float>& Deck, double Mark, double Gap,
		TFunctionRef<void(const TArray<FVector2D>&, const TArray<float>&)> Emit)
	{
		if (P.Num() < 2) return;
		const bool bDeck = Deck.Num() == P.Num();
		TArray<double> Cum;
		Cum.Add(0.0);
		for (int32 i = 1; i < P.Num(); ++i) Cum.Add(Cum.Last() + FVector2D::Distance(P[i - 1], P[i]));
		auto At = [&](double S, FVector2D& OutP, float& OutZ)
		{
			int32 i = 0;
			while (i + 2 < P.Num() && Cum[i + 1] < S) ++i;
			const double Seg = FMath::Max(Cum[i + 1] - Cum[i], 1e-3);
			const double A = FMath::Clamp((S - Cum[i]) / Seg, 0.0, 1.0);
			OutP = FMath::Lerp(P[i], P[i + 1], A);
			OutZ = bDeck ? FMath::Lerp(Deck[i], Deck[i + 1], static_cast<float>(A)) : 0.f;
		};
		for (double S0 = Gap * 0.5; S0 < Cum.Last(); S0 += Mark + Gap)
		{
			const double S1 = FMath::Min(S0 + Mark, Cum.Last());
			TArray<FVector2D> Sub; TArray<float> SubZ;
			FVector2D Q; float Z;
			At(S0, Q, Z); Sub.Add(Q); SubZ.Add(Z);
			for (int32 i = 1; i + 1 < P.Num(); ++i) if (Cum[i] > S0 && Cum[i] < S1) { Sub.Add(P[i]); SubZ.Add(bDeck ? Deck[i] : 0.f); }
			At(S1, Q, Z); Sub.Add(Q); SubZ.Add(Z);
			Emit(Sub, bDeck ? SubZ : TArray<float>());
		}
	}

	FVector At3(const FMRTerrain* T, const FVector2D& P, float Up = 0.f) { return FVector(P.X, P.Y, GroundAt(T, P.X, P.Y) + Up); }

	/** Carriageway lookup: how far a point is from the nearest road edge (below 0 = on a road). */
	struct FSegGrid
	{
		static constexpr double Bucket = 1600.0, Reach = 400.0;
		TArray<FRoadSeg> Segs;
		TMap<FIntPoint, TArray<int32>> Cells;
		void Add(const FRoadSeg& S)
		{
			const int32 Id = Segs.Add(S);
			const double M = S.HalfW + Reach;
			const int32 X0 = FMath::FloorToInt32((FMath::Min(S.A.X, S.B.X) - M) / Bucket), X1 = FMath::FloorToInt32((FMath::Max(S.A.X, S.B.X) + M) / Bucket);
			const int32 Y0 = FMath::FloorToInt32((FMath::Min(S.A.Y, S.B.Y) - M) / Bucket), Y1 = FMath::FloorToInt32((FMath::Max(S.A.Y, S.B.Y) + M) / Bucket);
			for (int32 Y = Y0; Y <= Y1; ++Y) for (int32 X = X0; X <= X1; ++X) Cells.FindOrAdd(FIntPoint(X, Y)).Add(Id);
		}
		/** Distance from P to the nearest carriageway edge in cm, capped at Reach. */
		double Clearance(const FVector2D& P) const
		{
			double Best = Reach;
			if (const TArray<int32>* L = Cells.Find(FIntPoint(FMath::FloorToInt32(P.X / Bucket), FMath::FloorToInt32(P.Y / Bucket))))
			{
				for (const int32 Id : *L)
				{
					const FRoadSeg& S = Segs[Id];
					Best = FMath::Min(Best, FVector2D::Distance(P, FMath::ClosestPointOnSegment2D(P, S.A, S.B)) - S.HalfW);
				}
			}
			return Best;
		}
	};

	/**
	 * Raised pavements with kerbs along both sides of a street: a 12 cm kerb face at the road edge, a
	 * kerb stone, 2 m of flags and a short bevel back down to the ground. The kerb drops to road level
	 * wherever another carriageway meets or crosses (junctions, side streets, driveways), so the road
	 * surface always runs through. NoPavement(P) switches pavements off (fields, woods, water).
	 */
	void Kerbs(FSec* Secs, const FMRTerrain* T, const TArray<FVector2D>& InPts, float HalfW, float RoadZ, const FSegGrid& Roads, TFunctionRef<bool(const FVector2D&)> NoPavement)
	{
		constexpr float KerbH = 12.f, KerbW = 14.f, PaveW = 200.f, Bevel = 35.f;
		struct FSample { FVector2D P, N; bool bVertex; };
		TArray<FSample> S;
		FVector2D PrevDir = FVector2D::ZeroVector;
		for (int32 i = 0; i + 1 < InPts.Num(); ++i)
		{
			const double Len = FVector2D::Distance(InPts[i], InPts[i + 1]);
			const FVector2D Dir = (InPts[i + 1] - InPts[i]).GetSafeNormal();
			if (Dir.IsNearlyZero()) continue;
			const int32 N = FMath::Max(1, FMath::CeilToInt(Len / 100.0));
			for (int32 k = 0; k < N; ++k)
			{
				FVector2D D = Dir;
				if (k == 0 && !PrevDir.IsNearlyZero()) { D = (PrevDir + Dir).GetSafeNormal(); if (D.IsNearlyZero()) D = Dir; }
				S.Add({ FMath::Lerp(InPts[i], InPts[i + 1], static_cast<double>(k) / N), FVector2D(-D.Y, D.X), k == 0 });
			}
			PrevDir = Dir;
		}
		if (S.Num() == 0) return;
		S.Add({ InPts.Last(), FVector2D(-PrevDir.Y, PrevDir.X), true });

		const float Far = HalfW + KerbW + PaveW;
		TArray<float> H;
		TArray<int32> Keep;
		for (const float Side : { 1.f, -1.f })
		{
			H.SetNumUninitialized(S.Num());
			for (int32 i = 0; i < S.Num(); ++i)
			{
				const FVector2D Q = S[i].P + S[i].N * (Side * Far);
				const double D = Roads.Clearance(Q);
				H[i] = KerbH * static_cast<float>(FMath::Clamp((D - 110.0) / 60.0, 0.0, 1.0));
				if (H[i] > 0.f && NoPavement(Q)) H[i] = 0.f;
			}
			// Keep bends, every change of height, and one sample every 6 m on plain straights.
			Keep.Reset();
			for (int32 i = 0; i < S.Num(); ++i)
			{
				const bool bEdge = i == 0 || i == S.Num() - 1;
				if (bEdge || S[i].bVertex || H[i] != H[i - 1] || H[i] != H[i + 1] || i - Keep.Last() >= 6) Keep.Add(i);
			}
			auto Pt = [&](int32 i, float Off, float Up) { const FVector2D Q = S[i].P + S[i].N * (Side * Off); return FVector(Q.X, Q.Y, GroundAt(T, Q.X, Q.Y) + Up); };
			auto Quad = [](FSec& Sec, const FVector& A0, const FVector& B0, const FVector& B1, const FVector& A1, const FVector& Nrm) { Tri(Sec, A0, B0, B1, Nrm); Tri(Sec, A0, B1, A1, Nrm); };
			for (int32 k = 0; k + 1 < Keep.Num(); ++k)
			{
				const int32 a = Keep[k], b = Keep[k + 1];
				if (FMath::Max(H[a], H[b]) < 0.5f) continue;
				const float Ha = RoadZ + H[a], Hb = RoadZ + H[b];
				const FVector A0 = Pt(a, HalfW - 3.f, RoadZ - 4.f), A1 = Pt(b, HalfW - 3.f, RoadZ - 4.f);
				const FVector B0 = Pt(a, HalfW - 3.f, Ha), B1 = Pt(b, HalfW - 3.f, Hb);
				const FVector C0 = Pt(a, HalfW + KerbW, Ha), C1 = Pt(b, HalfW + KerbW, Hb);
				const FVector D0 = Pt(a, Far, Ha), D1 = Pt(b, Far, Hb);
				const FVector E0 = Pt(a, Far + Bevel, 1.f), E1 = Pt(b, Far + Bevel, 1.f);
				const FVector Out(S[a].N * Side, 0.f);
				Quad(Secs[Ground], A0, B0, B1, A1, -Out);                              // kerb face, towards the road
				Quad(Secs[Ground], B0, C0, C1, B1, FVector::UpVector);                 // kerb stone
				Quad(Secs[Paving], C0, D0, D1, C1, FVector::UpVector);                 // flags
				Quad(Secs[Paving], D0, E0, E1, D1, (Out * 0.5f + FVector::UpVector).GetSafeNormal()); // back edge down to the ground
			}
		}
	}

	/** UK column lamp: 8 m pole, short arm out over the road, lantern lit at night. */
	FVector StreetLamp(FSec* Secs, const FMRTerrain* T, const FVector2D& P, const FVector2D& ToRoad)
	{
		using namespace MRMeshKit;
		const FVector Base = At3(T, P, -20.f);
		const FVector Top = Base + FVector(0, 0, 800.f);
		Tube(Secs[PropMetal], Base, Top, 10.f, 6.f, 8, true);
		const FVector ArmEnd = Top + FVector(ToRoad.X * 140.f, ToRoad.Y * 140.f, 15.f);
		Tube(Secs[PropMetal], Top - FVector(0, 0, 20.f), ArmEnd, 4.f, 4.f, 6, true);
		const FQuat Q = FRotationMatrix::MakeFromX(FVector(ToRoad, 0.f)).ToQuat();
		Box(Secs[PropMetal], ArmEnd + FVector(0, 0, 4.f), Q, FVector(28.f, 12.f, 6.f));
		Box(Secs[LampHead], ArmEnd - FVector(0, 0, 3.f), Q, FVector(24.f, 10.f, 1.5f));
		return ArmEnd - FVector(0, 0, 12.f); // where the night light goes
	}

	void TrafficSignal(FSec* Secs, const FMRTerrain* T, const FVector2D& P, const FVector2D& ToRoad)
	{
		using namespace MRMeshKit;
		const FVector Base = At3(T, P, -20.f);
		Tube(Secs[PropMetal], Base, Base + FVector(0, 0, 330.f), 6.f, 6.f, 8, true);
		const FQuat Q = FRotationMatrix::MakeFromX(FVector(ToRoad, 0.f)).ToQuat();
		const FVector H = Base + FVector(0, 0, 310.f);
		Box(Secs[PropMetal], H, Q, FVector(12.f, 16.f, 48.f));
		for (int32 k = 0; k < 3; ++k)
			Disc(Secs[LampHead], H + FVector(ToRoad, 0.f) * 12.5f + FVector(0, 0, (k - 1) * 30.f), FVector(ToRoad, 0.f), 0.f, 10.f, 1.f, 12);
	}

	/** Bus shelter aligned with the kerb: glass back and ends, roof, bench, flag. */
	void BusShelter(FSec* Secs, const FMRTerrain* T, const FVector2D& P, const FVector2D& Along, const FVector2D& ToRoad)
	{
		using namespace MRMeshKit;
		const FVector2D Back = -ToRoad;
		const FVector C = At3(T, P);
		const FVector X(Along, 0.f), Y(Back, 0.f);
		const FQuat Q = FRotationMatrix::MakeFromXY(X, Y).ToQuat();
		for (const float Sx : { -150.f, 150.f })
			for (const float Sy : { -60.f, 60.f })
				Tube(Secs[PropMetal], C + X * Sx + Y * Sy - FVector(0, 0, 20.f), C + X * Sx + Y * Sy + FVector(0, 0, 250.f), 4.f, 4.f, 6, true);
		Box(Secs[PropMetal], C + FVector(0, 0, 254.f), Q, FVector(165.f, 75.f, 5.f));                  // roof
		Box(Secs[PropGlass], C + Y * 60.f + FVector(0, 0, 130.f), Q, FVector(148.f, 1.f, 105.f));      // back glazing
		for (const float Sx : { -150.f, 150.f })
			Box(Secs[PropGlass], C + X * Sx + FVector(0, 0, 130.f), Q, FVector(1.f, 55.f, 105.f));      // end panels
		Box(Secs[Bark], C + Y * 40.f + FVector(0, 0, 48.f), Q, FVector(110.f, 14.f, 3.f));             // perch seat
		const FVector Flag = C - Y * 70.f + X * 190.f;
		Tube(Secs[PropMetal], Flag - FVector(0, 0, 20.f), Flag + FVector(0, 0, 280.f), 4.f, 4.f, 6, true);
		Disc(Secs[PropRed], Flag + FVector(0, 0, 270.f), FVector(Along, 0.f), 0.f, 22.f, 2.f, 16);
	}

	void Bench(FSec* Secs, const FMRTerrain* T, const FVector2D& P, const FVector2D& Along)
	{
		using namespace MRMeshKit;
		const FVector C = At3(T, P);
		const FVector X(Along, 0.f);
		const FVector Y(-Along.Y, Along.X, 0.f);
		const FQuat Q = FRotationMatrix::MakeFromXY(X, Y).ToQuat();
		Box(Secs[Bark], C + FVector(0, 0, 45.f), Q, FVector(90.f, 22.f, 3.f));
		Box(Secs[Bark], C + Y * 22.f + FVector(0, 0, 75.f), Q, FVector(90.f, 3.f, 18.f));
		for (const float Sx : { -75.f, 75.f })
			Box(Secs[PropMetal], C + X * Sx + FVector(0, 0, 22.f), Q, FVector(3.f, 22.f, 25.f));
	}

	void PostBox(FSec* Secs, const FMRTerrain* T, const FVector2D& P)
	{
		using namespace MRMeshKit;
		const FVector C = At3(T, P, -10.f);
		Tube(Secs[PropRed], C, C + FVector(0, 0, 140.f), 28.f, 28.f, 16, false);
		Ellipsoid(Secs[PropRed], C + FVector(0, 0, 140.f), FVector(31.f, 31.f, 14.f), FQuat::Identity, 16, 6);
		Disc(Secs[PropRed], C + FVector(0, 0, 133.f), FVector::UpVector, 0.f, 31.f, 4.f, 16);
		Box(Secs[PropMetal], C + FVector(28.f, 0, 110.f), FQuat::Identity, FVector(1.f, 10.f, 2.f)); // slot
	}

	/** K6 telephone box: red cast iron, glazed sides, domed roof. */
	void PhoneBox(FSec* Secs, const FMRTerrain* T, const FVector2D& P, const FVector2D& Along)
	{
		using namespace MRMeshKit;
		const FVector C = At3(T, P, -5.f);
		const FVector X(Along, 0.f), Y(-Along.Y, Along.X, 0.f);
		const FQuat Q = FRotationMatrix::MakeFromXY(X, Y).ToQuat();
		Box(Secs[PropRed], C + FVector(0, 0, 122.f), Q, FVector(46.f, 46.f, 122.f));
		Ellipsoid(Secs[PropRed], C + FVector(0, 0, 244.f), FVector(48.f, 48.f, 14.f), Q, 12, 6);
		for (int32 Face = 0; Face < 4; ++Face)
		{
			const FVector N = Face < 2 ? X * (Face == 0 ? 1.f : -1.f) : Y * (Face == 2 ? 1.f : -1.f);
			const FVector Tg = Face < 2 ? Y : X;
			Box(Secs[PropGlass], C + N * 46.5f + FVector(0, 0, 125.f), FRotationMatrix::MakeFromXY(N, Tg).ToQuat(), FVector(0.6f, 30.f, 80.f));
		}
		Box(Secs[LampHead], C + X * 46.8f + FVector(0, 0, 222.f), Q, FVector(0.5f, 28.f, 6.f)); // TELEPHONE sign
	}

	void CycleStands(FSec* Secs, const FMRTerrain* T, const FVector2D& P, const FVector2D& Along)
	{
		using namespace MRMeshKit;
		const FVector X(Along, 0.f), Y(-Along.Y, Along.X, 0.f);
		for (int32 k = -1; k <= 1; ++k)
		{
			const FVector C = At3(T, P + Along * (k * 100.f));
			PathTube(Secs[PropMetal], { C - Y * 35.f - FVector(0, 0, 10.f), C - Y * 35.f + FVector(0, 0, 70.f), C - Y * 25.f + FVector(0, 0, 80.f),
				C + Y * 25.f + FVector(0, 0, 80.f), C + Y * 35.f + FVector(0, 0, 70.f), C + Y * 35.f - FVector(0, 0, 10.f) }, 2.4f, 6, false);
		}
	}

	/** Zebra crossing across the carriageway, with Belisha beacons on both kerbs. */
	void Zebra(FSec* Secs, const FMRTerrain* T, const FVector2D& Closest, const FVector2D& Along, float HalfW, float RoadZ)
	{
		using namespace MRMeshKit;
		const FVector2D Across(-Along.Y, Along.X);
		for (float S = -HalfW + 30.f; S <= HalfW - 30.f; S += 100.f)
			Ribbon(Secs[MarkWhite], { Closest + Across * S - Along * 150.f, Closest + Across * S + Along * 150.f }, TArray<float>(), 0.5f, RoadZ + 3.f, T);
		for (const float Side : { -1.f, 1.f })
		{
			const FVector B = At3(T, Closest + Across * (Side * (HalfW + 60.f)), -20.f);
			Tube(Secs[PropMetal], B, B + FVector(0, 0, 260.f), 5.f, 5.f, 8, true);
			Ellipsoid(Secs[LampHead], B + FVector(0, 0, 275.f), FVector(16.f), FQuat::Identity, 12, 8);
		}
	}

	/** Broadleaf tree: trunk + three canopy masses, sized by a per-position hash. */
	void Tree(FSec* Secs, const FMRTerrain* T, const FVector2D& P, float Scale)
	{
		using namespace MRMeshKit;
		const float R = Hash01(P.X, P.Y, 7);
		const float H = (300.f + 260.f * R) * Scale;
		const FVector Base = At3(T, P, -30.f);
		Tube(Secs[Bark], Base, Base + FVector(0, 0, H + 60.f), (16.f + 10.f * R) * Scale, (9.f + 5.f * R) * Scale, 7, false);
		const float Cr = (230.f + 170.f * Hash01(P.X, P.Y, 11)) * Scale;
		const FVector Crown = Base + FVector(0, 0, H + Cr * 0.55f);
		// Crown built from a core plus irregular leaf masses, so the outline is lumpy like a real broadleaf.
		Ellipsoid(Secs[Foliage], Crown, FVector(Cr * 0.8f, Cr * 0.8f, Cr * 0.7f), FQuat::Identity, 9, 5);
		for (int32 k = 0; k < 7; ++k)
		{
			const float A = 2.f * PI * (k / 7.f + 0.12f * Hash01(P.X, P.Y, 13 + k));
			const float Up = (Hash01(P.X, P.Y, 23 + k) - 0.35f) * Cr * 0.7f;
			const float Rk = Cr * (0.38f + 0.2f * Hash01(P.X, P.Y, 31 + k));
			const float Out = Cr * (0.5f + 0.15f * Hash01(P.X, P.Y, 41 + k));
			Ellipsoid(Secs[Foliage], Crown + FVector(FMath::Cos(A) * Out, FMath::Sin(A) * Out, Up), FVector(Rk, Rk, Rk * 0.85f), FQuat::Identity, 7, 4);
		}
		Ellipsoid(Secs[Foliage], Crown + FVector(0, 0, Cr * 0.55f), FVector(Cr * 0.5f), FQuat::Identity, 7, 4); // top
	}

	/** Worker-thread: parse one cell file into mesh sections. */
	void AddMapLine(TArray<AMRGeoWorld::FMapLine>& Lines, const TArray<FVector2D>& Pts, AMRGeoWorld::EMapLine Kind)
	{
		if (Pts.Num() < 2) return;
		AMRGeoWorld::FMapLine& L = Lines.AddDefaulted_GetRef();
		L.Kind = Kind;
		L.Bounds = FBox2f(ForceInit);
		L.Pts.Reserve(Pts.Num());
		for (const FVector2D& P : Pts) { const FVector2f F(P); L.Pts.Add(F); L.Bounds += F; }
	}

	TSharedPtr<AMRGeoWorld::FCellBuild> BuildCell(const FString& Path, FIntPoint Cell, float CellSizeCm, TSharedPtr<const FMRTerrain> TerrainPtr)
	{
		const FMRTerrain* T = TerrainPtr.Get();
		TArray<AMRGeoWorld::FMapLine> MapLines;
		// Cells with no data file still get full terrain ground, so there is no edge to fall off anywhere.
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			Root = MakeShared<FJsonObject>();
		}

		FSec Secs[Count];

		// Painted areas (clipped to this cell by the pipeline). Higher priority wins where they overlap.
		struct FArea { TArray<FVector2D> Ring; FBox2D Bounds; int32 Group; int32 Priority; };
		TArray<FArea> AreaList;
		const TArray<TSharedPtr<FJsonValue>>* Areas;
		if (Root->TryGetArrayField(TEXT("areas"), Areas))
		{
			for (const TSharedPtr<FJsonValue>& V : *Areas)
			{
				const TSharedPtr<FJsonObject>& A = V->AsObject();
				const FString Kind = A->GetStringField(TEXT("kind"));
				FArea Ar;
				if (Kind == TEXT("water")) { Ar.Group = Water; Ar.Priority = 6; }
				else if (Kind == TEXT("pitch")) { Ar.Group = Grass; Ar.Priority = 5; }
				else if (Kind == TEXT("car_park")) { Ar.Group = MinorRoad; Ar.Priority = 5; }
				else if (Kind == TEXT("woodland")) { Ar.Group = Woodland; Ar.Priority = 4; }
				else if (Kind == TEXT("green")) { Ar.Group = Grass; Ar.Priority = 3; }
				else if (Kind == TEXT("scrub")) { Ar.Group = DirtPath; Ar.Priority = 3; }
				else if (Kind == TEXT("quarry")) { Ar.Group = Gravel; Ar.Priority = 3; }
				else if (Kind == TEXT("farmland")) { Ar.Group = Grass; Ar.Priority = 2; }
				else if (Kind == TEXT("industrial") || Kind == TEXT("retail")) { Ar.Group = Paving; Ar.Priority = 1; }
				else continue; // residential etc. keep the ground colour
				Ar.Ring = ReadPts(A->GetArrayField(TEXT("ring")));
				if (Ar.Ring.Num() < 3) continue;
				Ar.Bounds = FBox2D(Ar.Ring);
				AreaList.Add(MoveTemp(Ar));
			}
			AreaList.Sort([](const FArea& A, const FArea& B) { return A.Priority > B.Priority; });
		}

		// Terrain ground: the cell's 8 m lattice, two triangles per square, each painted by the area
		// under its centre. Vertices on cell edges are shared exactly with the neighbour cell, so the
		// ground is watertight across the whole county.
		{
			const double L = FMRTerrain::LatticeCm;
			const int32 N = FMath::Max(1, FMath::RoundToInt(CellSizeCm / L));
			const double X0 = Cell.X * static_cast<double>(CellSizeCm), Y0 = Cell.Y * static_cast<double>(CellSizeCm);
			TArray<FVector> P;
			P.SetNumUninitialized((N + 1) * (N + 1));
			for (int32 j = 0; j <= N; ++j)
				for (int32 i = 0; i <= N; ++i)
				{
					const double X = X0 + i * L, Y = Y0 + j * L;
					P[j * (N + 1) + i] = FVector(X, Y, T ? T->SampleRaw(X, Y) : 0.f);
				}
			auto GroupAt = [&](const FVector& A, const FVector& B, const FVector& C)
			{
				const FVector2D Mid((A.X + B.X + C.X) / 3.0, (A.Y + B.Y + C.Y) / 3.0);
				for (const FArea& Ar : AreaList) if (InRing(Ar.Ring, Ar.Bounds, Mid)) return Ar.Group;
				return static_cast<int32>(Ground);
			};
			for (int32 j = 0; j < N; ++j)
				for (int32 i = 0; i < N; ++i)
				{
					// Same split as FMRTerrain::GroundZ: diagonal from (i+1, j) to (i, j+1).
					const FVector& A = P[j * (N + 1) + i]; const FVector& B = P[j * (N + 1) + i + 1];
					const FVector& C = P[(j + 1) * (N + 1) + i]; const FVector& D = P[(j + 1) * (N + 1) + i + 1];
					Tri(Secs[GroupAt(A, B, C)], A, B, C, FVector::UpVector);
					Tri(Secs[GroupAt(B, D, C)], B, D, C, FVector::UpVector);
				}
		}
		TArray<FRoadSeg> RoadSegs;
		TArray<TPair<FVector2D, FVector2D>> LampSpots; // position, direction towards the road
		const TArray<TSharedPtr<FJsonValue>>* Roads;
		// First pass: where every carriageway is, so kerbs can drop at junctions and nothing solid
		// (bridge piers, lamp posts, street furniture) is ever built in a road.
		FSegGrid Carriage;
		TArray<FVector2D> ThroughSamples; // every 3 m along public streets (not service roads): buildings on these get a passage
		if (Root->TryGetArrayField(TEXT("roads"), Roads))
		{
			for (const TSharedPtr<FJsonValue>& V : *Roads)
			{
				const TSharedPtr<FJsonObject>& R = V->AsObject();
				const FString Kind = R->GetStringField(TEXT("kind"));
				if (Kind.StartsWith(TEXT("waterway")) || Kind.StartsWith(TEXT("railway"))) continue;
				bool bTunnel = false;
				R->TryGetBoolField(TEXT("tunnel"), bTunnel);
				if (bTunnel) continue;
				FString Cls;
				R->TryGetStringField(TEXT("cls"), Cls);
				double Width = 4.0, Surface = 0.0;
				R->TryGetNumberField(TEXT("width"), Width);
				R->TryGetNumberField(TEXT("surface"), Surface);
				const int32 G = RoadGroup(Kind, Cls, static_cast<int32>(Surface));
				if (G != MajorRoad && G != MinorRoad && G != CycleLane && G != Cobbles && G != Gravel) continue;
				const TArray<TSharedPtr<FJsonValue>>& PtsJson = R->GetArrayField(TEXT("pts"));
				if (ReadDeckZ(PtsJson).Num() > 0) continue; // bridge decks are above all this
				const TArray<FVector2D> Pts = ReadPts(PtsJson);
				const bool bThrough = Kind == TEXT("major") || Kind == TEXT("minor") || Kind == TEXT("residential") || Kind == TEXT("cycleway");
				for (int32 i = 0; i + 1 < Pts.Num(); ++i)
				{
					Carriage.Add({ Pts[i], Pts[i + 1], static_cast<float>(Width) * 50.f, 0.f });
					if (!bThrough) continue;
					const int32 N = FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(Pts[i], Pts[i + 1]) / 300.0));
					for (int32 k = 0; k < N; ++k) ThroughSamples.Add(FMath::Lerp(Pts[i], Pts[i + 1], static_cast<double>(k) / N));
				}
			}
		}
		// Pavements belong to streets, not to lanes across fields, woods or water.
		auto NoPavement = [&](const FVector2D& P)
		{
			for (const FArea& Ar : AreaList)
				if (InRing(Ar.Ring, Ar.Bounds, P)) return Ar.Group == Water || Ar.Group == Woodland || (Ar.Group == Grass && Ar.Priority == 2);
			return false;
		};
		if (Root->TryGetArrayField(TEXT("roads"), Roads))
		{
			for (const TSharedPtr<FJsonValue>& V : *Roads)
			{
				const TSharedPtr<FJsonObject>& R = V->AsObject();
				const FString Kind = R->GetStringField(TEXT("kind"));
				FString Cls;
				R->TryGetStringField(TEXT("cls"), Cls);
				double Width = 4.0, Surface = 0.0;
				R->TryGetNumberField(TEXT("width"), Width);
				R->TryGetNumberField(TEXT("surface"), Surface);
				if (Kind.StartsWith(TEXT("waterway"))) Width = Kind.EndsWith(TEXT("canal")) ? 10.0 : (Kind.EndsWith(TEXT("river")) ? 18.0 : 2.0);
				if (Kind.StartsWith(TEXT("railway"))) Width = 4.0;
				bool bTunnel = false;
				R->TryGetBoolField(TEXT("tunnel"), bTunnel);
				if (bTunnel) continue; // tunnels need terrain/underpass geometry (Milestone 4 importer)
				const int32 G = RoadGroup(Kind, Cls, static_cast<int32>(Surface));
				// Height above the ground mesh (cm): distinct per class so overlapping ribbons don't z-fight.
				const float Z = G == MajorRoad ? 12.f : (G == Paving ? 16.f : (G == Water ? 6.f : (G == Rail ? 8.f : 10.f)));
				const TArray<TSharedPtr<FJsonValue>>& PtsJson = R->GetArrayField(TEXT("pts"));
				const TArray<FVector2D> Pts = ReadPts(PtsJson);
				const TArray<float> Deck = ReadDeckZ(PtsJson);
				Ribbon(Secs[G], Pts, Deck, static_cast<float>(Width), Z, T);
				const float HalfW = static_cast<float>(Width) * 50.f;
				if (G == MajorRoad || G == MinorRoad || G == Paving || G == CycleLane || G == Cobbles)
					for (int32 i = 0; i + 1 < Pts.Num(); ++i) RoadSegs.Add({ Pts[i], Pts[i + 1], HalfW, Z });
				// Streets get raised pavements with kerbs on both sides.
				if ((G == MajorRoad || G == MinorRoad) && Deck.Num() == 0 && Width >= 4.5
					&& (Kind == TEXT("major") || Kind == TEXT("minor") || Kind == TEXT("residential")))
					Kerbs(Secs, T, Pts, HalfW, Z, Carriage, NoPavement);
				// UK markings: dashed white centre line on two-way carriageways, solid edge lines on major roads.
				if ((G == MajorRoad || G == MinorRoad) && Width >= 5.0 && Deck.Num() == 0)
				{
					double Lanes = 2.0; bool bOneway = false;
					R->TryGetNumberField(TEXT("lanes"), Lanes);
					R->TryGetBoolField(TEXT("oneway"), bOneway);
					const float MZ = Z + 3.f;
					if (!bOneway && Lanes >= 2.0)
						Dashes(Pts, Deck, G == MajorRoad ? 600.0 : 400.0, G == MajorRoad ? 900.0 : 500.0,
							[&](const TArray<FVector2D>& Sub, const TArray<float>& SubZ) { Ribbon(Secs[MarkWhite], Sub, SubZ, 0.12f, MZ, T); });
					if (G == MajorRoad)
						for (const double Side : { -1.0, 1.0 })
							Ribbon(Secs[MarkWhite], OffsetLine(Pts, Side * (HalfW - 35.0)), Deck, 0.1f, MZ, T);
				}
				// Bridges and viaducts: brick piers and a solid deck under railways (Stockport Viaduct,
				// Castlefield's arches), concrete piers under road bridges, wherever the deck is well above ground.
				if (Deck.Num() == Pts.Num() && (G == Rail || G == MajorRoad || G == MinorRoad))
				{
					const bool bRail = G == Rail;
					const int32 PierG = bRail ? PropBrick : PropConcrete;
					TArray<FVector> Piers;
					Dashes(Pts, Deck, 1.0, bRail ? 1800.0 : 2600.0, [&](const TArray<FVector2D>& Sub, const TArray<float>& SubZ)
					{
						if (SubZ.Num() == 0) return;
						const float GroundZ = GroundAt(T, Sub[0].X, Sub[0].Y);
						if (SubZ[0] - GroundZ < 450.f) { Piers.Add(FVector(Sub[0], -1e9f)); return; } // too low: no pier, breaks the deck run
						const FVector2D Tg = (Sub.Last() - Sub[0]).GetSafeNormal();
						const float Hgt = SubZ[0] - 80.f - GroundZ;
						// No pier where a road runs underneath: the deck just spans it.
						const FVector2D Across(-Tg.Y, Tg.X);
						const float PierHalf = HalfW + (bRail ? 60.f : -100.f);
						bool bInRoad = false;
						for (const float F : { -1.f, -0.5f, 0.f, 0.5f, 1.f }) bInRoad |= Carriage.Clearance(Sub[0] + Across * (F * PierHalf)) < 180.0;
						if (bInRoad) { Piers.Add(FVector(Sub[0], SubZ[0])); return; }
						MRMeshKit::Box(Secs[PierG], FVector(Sub[0], GroundZ - 50.f + Hgt * 0.5f), FRotationMatrix::MakeFromX(FVector(Tg, 0.f)).ToQuat(),
							FVector(bRail ? 150.f : 90.f, HalfW + (bRail ? 60.f : -100.f), Hgt * 0.5f + 50.f));
						Piers.Add(FVector(Sub[0], SubZ[0]));
					});
					for (int32 i = 0; i + 1 < Piers.Num(); ++i)
					{
						if (Piers[i].Z < -1e8f || Piers[i + 1].Z < -1e8f) continue;
						MRMeshKit::Bar(Secs[PierG], FVector(FVector2D(Piers[i]), Piers[i].Z - 70.f), FVector(FVector2D(Piers[i + 1]), Piers[i + 1].Z - 70.f),
							HalfW * 2.f + (bRail ? 120.f : 0.f), bRail ? 140.f : 90.f); // deck body / arch spandrel
					}
				}
				// Street lighting along roads OSM tags as lit (the lamp nodes themselves are sparse).
				bool bLit = false;
				R->TryGetBoolField(TEXT("lit"), bLit);
				if (bLit && (G == MajorRoad || G == MinorRoad) && Deck.Num() == 0)
				{
					int32 k = 0;
					Dashes(Pts, Deck, 1.0, 3200.0, [&](const TArray<FVector2D>& Sub, const TArray<float>&)
					{
						const FVector2D Tg = (Sub.Last() - Sub[0]).GetSafeNormal();
						if (Tg.IsNearlyZero()) return;
						const double Side = (k++ % 2) ? 1.0 : -1.0;
						const FVector2D Left(-Tg.Y, Tg.X);
						LampSpots.Add({ Sub[0] + Left * (Side * (HalfW + 70.0)), Left * -Side });
					});
				}
				using EML = AMRGeoWorld::EMapLine;
				const EML LineKind = G == MajorRoad ? EML::MajorRoad : G == MinorRoad ? EML::MinorRoad : G == CycleLane ? EML::Cycle
					: G == Water ? EML::Water : G == Rail ? EML::Rail : EML::Path;
				AddMapLine(MapLines, Pts, LineKind);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Buildings;
		if (Root->TryGetArrayField(TEXT("buildings"), Buildings))
		{
			for (const TSharedPtr<FJsonValue>& V : *Buildings)
			{
				const TSharedPtr<FJsonObject>& B = V->AsObject();
				TArray<FVector2D> Ring = ReadPts(B->GetArrayField(TEXT("ring")));
				if (Ring.Num() < 3) continue;
				double H = 7.5, MinH = 0.0;
				B->TryGetNumberField(TEXT("h"), H);
				B->TryGetNumberField(TEXT("minH"), MinH);
				FString Style, Material, BType;
				B->TryGetStringField(TEXT("style"), Style);
				B->TryGetStringField(TEXT("material"), Material);
				B->TryGetStringField(TEXT("type"), BType);
				double FootM2 = 0.0;
				B->TryGetNumberField(TEXT("areaM2"), FootM2);
				// Open-sided roofs (petrol station and platform canopies, car ports) are a roof on posts, not a block.
				const bool bCanopy = BType == TEXT("roof") || BType == TEXT("canopy") || BType == TEXT("carport");
				// Untagged heights default to a two-storey house: far too tall for kiosks, substations and huts.
				const bool bSmallType = BType == TEXT("service") || BType == TEXT("kiosk") || BType == TEXT("container") || BType == TEXT("hut")
					|| BType == TEXT("toilets") || BType == TEXT("greenhouse") || BType == TEXT("bicycle_shed") || BType == TEXT("transformer_tower");
				if (H == 7.5 && (bSmallType || (FootM2 > 0.0 && FootM2 < 16.0))) H = 3.0;
				if (H == 7.5 && bCanopy) H = 4.8;
				// Sit on the terrain: walls start 1 m below the lowest ground point under the footprint
				// (no floating gaps on slopes) and the roof is measured from the lowest point too, but
				// never lower than 2.5 m above the highest point (so the uphill side isn't buried).
				float GMin = TNumericLimits<float>::Max(), GMax = -TNumericLimits<float>::Max();
				for (const FVector2D& Pt : Ring) { const float Gz = GroundAt(T, Pt.X, Pt.Y); GMin = FMath::Min(GMin, Gz); GMax = FMath::Max(GMax, Gz); }
				const float Top = FMath::Max(GMin + static_cast<float>(FMath::Max(H, 2.0) * 100.0), GMax + 250.f);
				float Bottom = MinH > 0.0 ? GMin + static_cast<float>(MinH * 100.0) : GMin - 100.f;
				// A public street running through the footprint (untagged archways, buildings drawn across
				// a road): lift the building clear so the street stays open underneath.
				bool bOpenUnder = bCanopy && MinH <= 0.0;
				if (!bOpenUnder && MinH <= 0.0 && ThroughSamples.Num() > 0)
				{
					const FBox2D RB(Ring);
					int32 Inside = 0;
					for (const FVector2D& Q : ThroughSamples) if (InRing(Ring, RB, Q) && ++Inside >= 3) break;
					bOpenUnder = Inside >= 3;
				}
				if (bOpenUnder && !bCanopy) Bottom = FMath::Min(GMax + 450.f, Top - 250.f);
				if (bCanopy && MinH <= 0.0) Bottom = Top - 35.f;
				// Stadium sites: the modelled stadium replaces the OSM stand/outbuilding blocks.
				if (InStadium(RingCentroid(Ring))) continue;
				FString LandmarkName;
				B->TryGetStringField(TEXT("name"), LandmarkName);
				if (!LandmarkName.IsEmpty() && BuildNamedLandmark(Secs, T, LandmarkName, Ring, GMin))
				{
					const FVector2D First = Ring[0];
					Ring.Add(First);
					AddMapLine(MapLines, Ring, AMRGeoWorld::EMapLine::Building);
					continue;
				}
				const int32 BG = BuildingGroup(Style, Material);
				FString Roof;
				B->TryGetStringField(TEXT("roof"), Roof);
				double AreaM2 = 0.0;
				B->TryGetNumberField(TEXT("areaM2"), AreaM2);
				const FVector2D Cen = Ring[0];
				const float R = Hash01(Cen.X, Cen.Y, 21);
				Extrude(Secs[BG], Ring, Bottom, Top);
				if (bOpenUnder)
				{
					FlatPolygon(Secs[BG], Ring, Bottom, false); // underside
					if (bCanopy)
					{
						// Posts at up to eight corners, pulled in a little from the edge.
						const FVector2D Mid = RingCentroid(Ring);
						const int32 Step = FMath::Max(1, FMath::CeilToInt(Ring.Num() / 8.f));
						for (int32 i = 0; i < Ring.Num(); i += Step)
						{
							const FVector2D Q = Ring[i] + (Mid - Ring[i]).GetSafeNormal() * 40.0;
							if (Carriage.Clearance(Q) < 40.0) continue;
							const float Gz = GroundAt(T, Q.X, Q.Y);
							MRMeshKit::Box(Secs[PropMetal], FVector(Q.X, Q.Y, (Gz - 30.f + Bottom) * 0.5f), FQuat::Identity, FVector(8.f, 8.f, (Bottom - Gz + 30.f) * 0.5f));
						}
					}
				}
				// Real roof shapes on houses, terraces, churches and small buildings tagged gabled/hipped.
				const bool bPitched = !bOpenUnder && (Roof.Contains(TEXT("gabled")) || Roof.Contains(TEXT("hipped")) || Roof == TEXT("pyramidal"))
					&& BG != Glass && Style != TEXT("industrial") && Style != TEXT("retail") && Style != TEXT("garage") && (AreaM2 <= 0.0 || AreaM2 < 4000.0);
				if (bPitched) PitchedRoof(Secs[BG], Ring, Top, Roof.StartsWith(TEXT("hipped")) || Roof == TEXT("pyramidal"));
				PaintNewVerts(Secs[BG], BuildingColour(BG, Style, R), bPitched && Hash01(Cen.X, Cen.Y, 23) < (Style == TEXT("semi") || Style == TEXT("detached") || Style == TEXT("bungalow") ? 0.55f : 0.2f));
				const FVector2D First = Ring[0];
				Ring.Add(First); // closed outline (copy first: adding an element of the same array asserts)
				AddMapLine(MapLines, Ring, AMRGeoWorld::EMapLine::Building);
			}
		}

		// Stadiums: built once, by the cell that contains the pitch centre.
		for (const FStadium& St : Stadiums)
		{
			if (FMath::FloorToInt(St.C.X / CellSizeCm) == Cell.X && FMath::FloorToInt(St.C.Y / CellSizeCm) == Cell.Y) BuildStadium(Secs, T, St);
		}

		// Street furniture from OSM nodes, aligned to the nearest carriageway.
		TArray<FVector> LampLights;
		for (const TPair<FVector2D, FVector2D>& L : LampSpots)
			if (Carriage.Clearance(L.Key) >= 40.0) LampLights.Add(StreetLamp(Secs, T, L.Key, L.Value)); // never in a side street's mouth
		const TArray<TSharedPtr<FJsonValue>>* PropsArr;
		if (Root->TryGetArrayField(TEXT("props"), PropsArr))
		{
			for (const TSharedPtr<FJsonValue>& V : *PropsArr)
			{
				const TSharedPtr<FJsonObject>& Pr = V->AsObject();
				FString Type;
				if (!Pr.IsValid() || !Pr->TryGetStringField(TEXT("type"), Type)) continue;
				const TArray<TSharedPtr<FJsonValue>>* XY;
				if (!Pr->TryGetArrayField(TEXT("p"), XY) || XY->Num() < 2) continue;
				const FVector2D P((*XY)[0]->AsNumber(), (*XY)[1]->AsNumber());
				FVector2D Along(1, 0), Closest = P;
				const FRoadSeg* Seg = nullptr;
				const bool bNear = NearestRoad(RoadSegs, P, 1500.0, Along, Closest, Seg);
				const FVector2D ToRoad = bNear && !(Closest - P).IsNearlyZero() ? (Closest - P).GetSafeNormal() : FVector2D(-Along.Y, Along.X);
				// Kerbside spot: just off the carriageway edge, on the prop's side.
				const FVector2D Kerb = bNear ? Closest - ToRoad * (Seg->HalfW + 80.f) : P;
				// Nothing solid is built on a carriageway (map nodes are often placed on the road's centre line).
				auto Clear = [&](const FVector2D& Q, double Min) { return Carriage.Clearance(Q) >= Min; };
				if (Type == TEXT("street_lamp")) { if (Clear(Kerb, 40.0)) LampLights.Add(StreetLamp(Secs, T, Kerb, ToRoad)); }
				else if (Type == TEXT("traffic_signals"))
				{
					if (bNear) for (const float S : { -1.f, 1.f })
					{
						const FVector2D Q = Closest + FVector2D(-Along.Y, Along.X) * (S * (Seg->HalfW + 60.f));
						if (Clear(Q, 30.0)) TrafficSignal(Secs, T, Q, FVector2D(Along.Y, -Along.X) * S);
					}
				}
				else if (Type == TEXT("bus_stop")) { const FVector2D Q = Closest - ToRoad * ((bNear ? Seg->HalfW : 0.f) + 170.f); if (Clear(Q, 90.0)) BusShelter(Secs, T, Q, Along, ToRoad); }
				else if (Type == TEXT("crossing")) { if (bNear && Seg->HalfW >= 250.f && Seg->HalfW <= 900.f) Zebra(Secs, T, Closest, Along, Seg->HalfW, Seg->Z); }
				else if (Type == TEXT("bench")) { if (Clear(P, 100.0)) Bench(Secs, T, P, Along); }
				else if (Type == TEXT("post_box")) { if (Clear(P, 40.0)) PostBox(Secs, T, P); }
				else if (Type == TEXT("telephone")) { if (Clear(P, 60.0)) PhoneBox(Secs, T, P, Along); }
				else if (Type == TEXT("bicycle_parking")) { if (Clear(P, 100.0)) CycleStands(Secs, T, P, Along); }
			}
		}

		// Trees: dense in woodland, scattered in parks and greens, kept off roads and paths.
		{
			const double X0 = Cell.X * static_cast<double>(CellSizeCm), Y0 = Cell.Y * static_cast<double>(CellSizeCm);
			int32 Trees = 0;
			constexpr int32 MaxTrees = 450;
			for (const FArea& Ar : AreaList)
			{
				const bool bWood = Ar.Group == Woodland;
				const bool bPark = Ar.Group == Grass && Ar.Priority == 3;
				if (!bWood && !bPark) continue;
				const double Step = bWood ? 900.0 : 1700.0;
				const float Chance = bWood ? 0.85f : 0.35f;
				const double Ax = FMath::Max(Ar.Bounds.Min.X, X0), Bx = FMath::Min(Ar.Bounds.Max.X, X0 + CellSizeCm);
				const double Ay = FMath::Max(Ar.Bounds.Min.Y, Y0), By = FMath::Min(Ar.Bounds.Max.Y, Y0 + CellSizeCm);
				for (double Y = FMath::FloorToDouble(Ay / Step) * Step; Y < By && Trees < MaxTrees; Y += Step)
					for (double X = FMath::FloorToDouble(Ax / Step) * Step; X < Bx && Trees < MaxTrees; X += Step)
					{
						if (Hash01(X, Y, 3) > Chance) continue;
						const FVector2D P(X + (Hash01(X, Y, 5) - 0.5) * Step * 0.8, Y + (Hash01(X, Y, 6) - 0.5) * Step * 0.8);
						if (P.X < X0 || P.X >= X0 + CellSizeCm || P.Y < Y0 || P.Y >= Y0 + CellSizeCm) continue; // each tree belongs to one cell
						if (!InRing(Ar.Ring, Ar.Bounds, P)) continue;
						FVector2D D, C; const FRoadSeg* S = nullptr;
						if (NearestRoad(RoadSegs, P, 2000.0, D, C, S) && FVector2D::Distance(P, C) < S->HalfW + 250.0) continue;
						Tree(Secs, T, P, bWood ? 1.f : 1.15f);
						++Trees;
					}
			}
		}

		TSharedPtr<AMRGeoWorld::FCellBuild> Out = MakeShared<AMRGeoWorld::FCellBuild>();
		Out->Cell = Cell;
		Out->MapLines = MoveTemp(MapLines);
		Out->LampLights = MoveTemp(LampLights);
		const FVector2D CellMid((Cell.X + 0.5) * CellSizeCm, (Cell.Y + 0.5) * CellSizeCm);
		for (int32 G = 0; G < Count; ++G)
		{
			if (Secs[G].T.Num() == 0) continue;
			if (!CastsShadow(G))
			{
				AMRGeoWorld::FSection S;
				S.Verts = MoveTemp(Secs[G].V);
				S.Tris = MoveTemp(Secs[G].T);
				S.Group = G;
				Out->Sections.Add(MoveTemp(S));
				continue;
			}
			// Shadow casters are split into 3 x 3 pieces (~85 m): one cell-wide non-Nanite caster per group
			// overflows the virtual shadow map marking queue, and smaller pieces also finish in smaller steps.
			const FSec& Src = Secs[G];
			const bool bColours = Src.C.Num() == Src.V.Num();
			AMRGeoWorld::FSection Q[9];
			TArray<int32> Remap[9];
			for (int32 q = 0; q < 9; ++q) { Remap[q].Init(INDEX_NONE, Src.V.Num()); Q[q].Group = G; }
			for (int32 i = 0; i + 2 < Src.T.Num(); i += 3)
			{
				const FVector C = (Src.V[Src.T[i]] + Src.V[Src.T[i + 1]] + Src.V[Src.T[i + 2]]) / 3.0;
				const int32 qx = FMath::Clamp(FMath::FloorToInt((C.X - CellMid.X) / (CellSizeCm / 3.0) + 1.5), 0, 2), qy = FMath::Clamp(FMath::FloorToInt((C.Y - CellMid.Y) / (CellSizeCm / 3.0) + 1.5), 0, 2);
				const int32 q = qx + qy * 3;
				for (int32 k = 0; k < 3; ++k)
				{
					const int32 Vi = Src.T[i + k];
					if (Remap[q][Vi] == INDEX_NONE)
					{
						Remap[q][Vi] = Q[q].Verts.Num();
						Q[q].Verts.Add(Src.V[Vi]);
						if (bColours) Q[q].Colors.Add(Src.C[Vi]);
					}
					Q[q].Tris.Add(Remap[q][Vi]);
				}
			}
			for (int32 q = 0; q < 9; ++q) if (Q[q].Tris.Num() > 0) Out->Sections.Add(MoveTemp(Q[q]));
		}
		const TArray<TSharedPtr<FJsonValue>>* PlacesArr;
		if (Root->TryGetArrayField(TEXT("places"), PlacesArr))
		{
			for (const TSharedPtr<FJsonValue>& V : *PlacesArr)
			{
				const TSharedPtr<FJsonObject>& P = V->AsObject();
				FString Name;
				if (!P->TryGetStringField(TEXT("name"), Name)) continue;
				const TArray<TSharedPtr<FJsonValue>>& XY = P->GetArrayField(TEXT("p"));
				if (XY.Num() >= 2) Out->Places.Add({ Name, FVector(XY[0]->AsNumber(), XY[1]->AsNumber(), GroundAt(T, XY[0]->AsNumber(), XY[1]->AsNumber())) });
			}
		}
		return Out;
	}
}

// ------------------------------------------------------------------ terrain

bool FMRTerrain::Load(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 32) return false;
	if (FMemory::Memcmp(Bytes.GetData(), "MRTR", 4) != 0) return false;
	auto I32 = [&](int32 Off) { int32 V; FMemory::Memcpy(&V, Bytes.GetData() + Off, 4); return V; }; // file is little-endian, as are all our targets
	if (I32(4) != 1) return false;
	MinX = I32(8); MinY = I32(12); SpacingCm = I32(16); NX = I32(20); NY = I32(24);
	FMemory::Memcpy(&BaseHeightM, Bytes.GetData() + 28, 4);
	const int64 Count = static_cast<int64>(NX) * NY;
	if (NX < 2 || NY < 2 || SpacingCm <= 0 || Bytes.Num() < 32 + Count * 2) return false;
	Heights.SetNumUninitialized(Count);
	FMemory::Memcpy(Heights.GetData(), Bytes.GetData() + 32, Count * 2);
	return true;
}

float FMRTerrain::SampleRaw(double X, double Y) const
{
	const double GX = (X - MinX) / SpacingCm, GY = (Y - MinY) / SpacingCm;
	const int32 X0 = FMath::FloorToInt(GX), Y0 = FMath::FloorToInt(GY);
	const float FX = static_cast<float>(GX - X0), FY = static_cast<float>(GY - Y0);
	auto At = [this](int32 IX, int32 IY) { return Heights[FMath::Clamp(IY, 0, NY - 1) * NX + FMath::Clamp(IX, 0, NX - 1)] * 10.f; }; // dm -> cm
	return FMath::Lerp(FMath::Lerp(At(X0, Y0), At(X0 + 1, Y0), FX), FMath::Lerp(At(X0, Y0 + 1), At(X0 + 1, Y0 + 1), FX), FY);
}

float FMRTerrain::GroundZ(double X, double Y) const
{
	// Exactly the triangles BuildCell emits: lattice-aligned squares split on the (i+1,j)-(i,j+1) diagonal.
	const double I = FMath::FloorToDouble(X / LatticeCm), J = FMath::FloorToDouble(Y / LatticeCm);
	const double X0 = I * LatticeCm, Y0 = J * LatticeCm;
	const float FX = static_cast<float>((X - X0) / LatticeCm), FY = static_cast<float>((Y - Y0) / LatticeCm);
	const float A = SampleRaw(X0, Y0), B = SampleRaw(X0 + LatticeCm, Y0), C = SampleRaw(X0, Y0 + LatticeCm);
	if (FX + FY <= 1.f) return A + (B - A) * FX + (C - A) * FY;
	const float D = SampleRaw(X0 + LatticeCm, Y0 + LatticeCm);
	return D + (C - D) * (1.f - FX) + (B - D) * (1.f - FY);
}

TSharedPtr<const FMRTerrain> AMRGeoWorld::GetTerrain()
{
	check(IsInGameThread());
	static TSharedPtr<const FMRTerrain> Cached;
	static bool bTried = false;
	if (!bTried)
	{
		bTried = true;
		TSharedPtr<FMRTerrain> T = MakeShared<FMRTerrain>();
		const FString Path = FPaths::Combine(GeoDataDir(), TEXT("terrain.bin"));
		if (T->Load(Path))
		{
			Cached = T;
			UE_LOG(LogManchesterRide, Log, TEXT("Terrain: %d x %d samples @ %d m, origin %.1f m above sea level"), T->NX, T->NY, T->SpacingCm / 100, T->BaseHeightM);
		}
		else
		{
			UE_LOG(LogManchesterRide, Warning, TEXT("Terrain: %s missing or invalid; the world will be flat (run Tools/geo_pipeline/fetch_terrain.mjs)"), *Path);
		}
	}
	return Cached;
}

bool AMRGeoWorld::GetGroundHeight(double X, double Y, float& OutZ)
{
	const TSharedPtr<const FMRTerrain> T = GetTerrain();
	if (!T) return false;
	OutZ = T->GroundZ(X, Y);
	return true;
}

bool AMRGeoWorld::IsGroundReadyAt(const FVector& Location) const
{
	const FLoadedCell* L = Loaded.Find(FIntPoint(FMath::FloorToInt(Location.X / CellSizeCm), FMath::FloorToInt(Location.Y / CellSizeCm)));
	return L && L->bGroundReady;
}

AMRGeoWorld* AMRGeoWorld::Find(const UObject* WorldContext)
{
	const UWorld* W = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!W) return nullptr;
	for (TActorIterator<AMRGeoWorld> It(W); It; ++It) return *It;
	return nullptr;
}

// ------------------------------------------------------------------ actor

AMRGeoWorld::AMRGeoWorld()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Tags.Add(TEXT("MRGround"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MatFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	BaseMaterial = MatFinder.Object;
	// Created by Tools/make_materials.bat; the flat BasicShapeMaterial is the fallback.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GroundFinder(TEXT("/Game/MR/Materials/M_MRGround.M_MRGround"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FacadeFinder(TEXT("/Game/MR/Materials/M_MRFacade.M_MRFacade"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SurfaceFinder(TEXT("/Game/MR/Materials/M_MRSurface.M_MRSurface"));
	GroundMaterial = GroundFinder.Succeeded() ? GroundFinder.Object : BaseMaterial;
	FacadeMaterial = FacadeFinder.Succeeded() ? FacadeFinder.Object : BaseMaterial;
	SurfaceMaterial = SurfaceFinder.Succeeded() ? SurfaceFinder.Object : BaseMaterial;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FoliageFinder(TEXT("/Game/MR/Materials/M_MRFoliage.M_MRFoliage"));
	FoliageMaterial = FoliageFinder.Succeeded() ? FoliageFinder.Object : nullptr;
}

FString AMRGeoWorld::GeoDataDir()
{
	return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("GeoData"));
}

bool AMRGeoWorld::HasGeoData()
{
	return IFileManager::Get().FileExists(*FPaths::Combine(GeoDataDir(), TEXT("manifest.json")));
}

void AMRGeoWorld::BeginPlay()
{
	Super::BeginPlay();
	bAlive = MakeShared<TAtomic<bool>>(true);
	Ready = MakeShared<FReadyQueue>();
	GConfig->GetBool(TEXT("/Script/ManchesterRide.MRGeoWorld"), TEXT("bFlipWinding"), GeoBuild::GFlipWinding, GGameIni);

	SurfaceMaterials.SetNum(static_cast<int32>(ESurface::Count));
	for (int32 i = 0; i < SurfaceMaterials.Num(); ++i)
	{
		UPhysicalMaterial* PM = NewObject<UPhysicalMaterial>(this);
		PM->SurfaceType = static_cast<EPhysicalSurface>(static_cast<int32>(SurfaceType1) + i);
		SurfaceMaterials[i] = PM;
	}
	GroupMaterials.SetNum(GeoBuild::Count);
	for (int32 G = 0; G < GeoBuild::Count; ++G)
	{
		using namespace GeoBuild;
		const bool bTree = (G == Foliage || G == Bark) && FoliageMaterial;
		UMaterialInterface* Parent = bTree ? FoliageMaterial.Get() : G <= Rail ? GroundMaterial.Get() : G >= Brick ? FacadeMaterial.Get() : SurfaceMaterial.Get();
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent ? Parent : BaseMaterial.Get(), this);
		MID->SetVectorParameterValue(TEXT("Color"), Groups[G].Color);
		if (G <= Rail)
		{
			// Photo texture per surface (M_MRGround Tex): 0 asphalt, 1 worn asphalt, 2 concrete flags, 3 setts,
			// 4 grass, 5 woodland floor, 6 dirt, 7 gravel / ballast, 8 water.
			static const float Pattern[] = { 2, 0, 1, 2, 0, 3, 7, 6, 4, 5, 8, 7 };
			static const float Rough[] = { 0.85f, 0.72f, 0.75f, 0.8f, 0.7f, 0.65f, 0.9f, 0.95f, 0.95f, 0.95f, 0.08f, 0.9f };
			MID->SetScalarParameterValue(TEXT("Tex"), Pattern[G]);
			MID->SetScalarParameterValue(TEXT("Roughness"), Rough[G]);
		}
		else if (G >= Brick)
		{
			// Glass share, storey height and window bay width per building type.
			const float GlassShare = G == GeoBuild::Glass ? 0.9f : G == Stone ? 0.2f : 0.f;
			MID->SetScalarParameterValue(TEXT("Glass"), GlassShare);
			// Wall photo texture: 0 red brick, 1 sandstone (render on pale buildings), 2 concrete, 3 corrugated steel.
			MID->SetScalarParameterValue(TEXT("Wall"), G == Brick ? 0.f : G == Stone ? 1.f : G == GeoBuild::Glass ? 2.f : 3.f);
			MID->SetScalarParameterValue(TEXT("FloorHeight"), G == Brick ? 300.f : G == Industrial ? 650.f : 380.f);
			MID->SetScalarParameterValue(TEXT("BaySpacing"), G == Brick ? 260.f : G == Industrial ? 700.f : G == GeoBuild::Glass ? 160.f : 320.f);
			NightFacades.Add(MID);
		}
		else if (bTree)
		{
			MID->SetScalarParameterValue(TEXT("Pattern"), G == Bark ? 1.f : 0.f);
		}
		else
		{
			const bool bMetal = G == PropMetal;
			MID->SetScalarParameterValue(TEXT("Metallic"), bMetal ? 0.6f : 0.f);
			MID->SetScalarParameterValue(TEXT("Roughness"), G == PropGlass ? 0.05f : G == PropRed ? 0.35f : bMetal ? 0.45f : G <= MarkYellow ? 0.6f : 0.85f);
			if (G == LampHead) NightLamps.Add(MID);
		}
		GroupMaterials[G] = MID;
	}
	UpdateNightLighting();

	FString ManifestText;
	if (FFileHelper::LoadFileToString(ManifestText, *FPaths::Combine(GeoDataDir(), TEXT("manifest.json"))))
	{
		TSharedPtr<FJsonObject> M;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestText), M) && M.IsValid())
		{
			CellSizeCm = static_cast<float>(M->GetNumberField(TEXT("cellSizeM")) * 100.0);
			UE_LOG(LogManchesterRide, Log, TEXT("Geo world: %d cells of %.0f m"), static_cast<int32>(M->GetNumberField(TEXT("cellCount"))), CellSizeCm / 100.f);
		}
	}
	GetTerrain();

	// Build the ground under the spawn point (3 x 3 cells) synchronously, so the first rider
	// never spawns over a cell whose collision doesn't exist yet.
	if (!IsRunningDedicatedServer())
	{
		const FVector S = GetSpawnTransform(0).GetLocation();
		const FIntPoint C(FMath::FloorToInt(S.X / CellSizeCm), FMath::FloorToInt(S.Y / CellSizeCm));
		for (int32 DY = -1; DY <= 1; ++DY)
			for (int32 DX = -1; DX <= 1; ++DX) RequestCell(C + FIntPoint(DX, DY), true);
	}
	UpdateStreaming();
}

void AMRGeoWorld::EndPlay(const EEndPlayReason::Type Reason)
{
	if (bAlive) *bAlive = false; // in-flight worker results are dropped
	Super::EndPlay(Reason);
}

FTransform AMRGeoWorld::GetSpawnTransform(int32 PlayerIndex) const
{
	// Piccadilly Gardens (53.48099 N, 2.23697 W) = 522 m E, 181 m N of the Town Hall origin.
	FVector Base(52248.f - (PlayerIndex % 6) * 400.f, -18144.f + (PlayerIndex / 6) * 400.f, 0.f);
	// -StartLatLon=53.4420,-2.2770 starts somewhere else (testing / screenshots).
	FString LatLon;
	if (FParse::Value(FCommandLine::Get(), TEXT("StartLatLon="), LatLon, false)) // false: keep the comma
	{
		FString A, B;
		if (LatLon.Split(TEXT(","), &A, &B))
		{
			double X = 0, Y = 0;
			MRGeo::LocalToUnreal(MRGeo::ToLocal(MRGeo::FLatLon{ FCString::Atod(*A), FCString::Atod(*B) }), X, Y);
			Base.X = static_cast<float>(X) - (PlayerIndex % 6) * 400.f;
			Base.Y = static_cast<float>(Y) + (PlayerIndex / 6) * 400.f;
		}
	}
	float GroundZ = 0.f;
	GetGroundHeight(Base.X, Base.Y, GroundZ);
	Base.Z = GroundZ + 200.f;
	return FTransform(FRotator(0.f, 0.f, 0.f), Base);
}

FVector AMRGeoWorld::GetViewLocation() const
{
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (const APawn* P = PC->GetPawn()) return P->GetActorLocation();
		FVector Loc; FRotator Rot;
		PC->GetPlayerViewPoint(Loc, Rot);
		return Loc;
	}
	return GetSpawnTransform(0).GetLocation();
}

void AMRGeoWorld::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	StatTimer += DeltaSeconds; ++StatFrames;
	if (StatTimer > 5.f)
	{
		UE_LOG(LogManchesterRide, Verbose, TEXT("Geo stream: %.1f fps, %d cells loaded, %d pending, %d components"), StatFrames / StatTimer, Loaded.Num(), Pending.Num(), LiveComponents.Num());
		StatTimer = 0.f; StatFrames = 0;
	}
	NightTimer += DeltaSeconds;
	if (NightTimer > 2.f)
	{
		NightTimer = 0.f;
		UpdateNightLighting();
	}
	StreamTimer += DeltaSeconds;
	if (StreamTimer > 0.5f)
	{
		StreamTimer = 0.f;
		UpdateStreaming();
		UpdateLampPool();
	}
	// Turn worker results into components, a section at a time, within a frame budget.
	FinishPending(FPlatformTime::Seconds() + 0.006);
}

void AMRGeoWorld::UpdateStreaming()
{
	if (IsRunningDedicatedServer()) return; // no local viewer: the server needs no geometry (see header)
	const FVector View = GetViewLocation();
	const int32 R = FMath::CeilToInt(LoadRadiusCm / CellSizeCm);
	const FIntPoint Centre(FMath::FloorToInt(View.X / CellSizeCm), FMath::FloorToInt(View.Y / CellSizeCm));
	// The rider's own cell is built immediately if it's missing (e.g. after a long fast-travel), so
	// there is never a frame where the ground under them has no collision.
	if (Finishing.IsValid() && Finishing->Cell == Centre) FinishPending(0.0); // rider reached a half-finished cell: complete it now
	if (!Loaded.Contains(Centre))
	{
		Pending.Remove(Centre); // an in-flight async build of it is ignored when it lands (already loaded)
		RequestCell(Centre, true);
	}
	// Nearest cells first, so the ground ahead is ready before distant buildings.
	TArray<TPair<float, FIntPoint>> Wanted;
	for (int32 DY = -R; DY <= R; ++DY)
	{
		for (int32 DX = -R; DX <= R; ++DX)
		{
			const FIntPoint C(Centre.X + DX, Centre.Y + DY);
			const FVector2D CellCentre((C.X + 0.5f) * CellSizeCm, (C.Y + 0.5f) * CellSizeCm);
			const float D = FVector2D::Distance(CellCentre, FVector2D(View));
			if (D > LoadRadiusCm) continue;
			if (!Loaded.Contains(C) && !Pending.Contains(C)) Wanted.Add({ D, C });
		}
	}
	Wanted.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key < B.Key; });
	for (const TPair<float, FIntPoint>& W : Wanted) RequestCell(W.Value);
	TArray<FIntPoint> ToUnload;
	for (const TPair<FIntPoint, FLoadedCell>& L : Loaded)
	{
		const FVector2D CellCentre((L.Key.X + 0.5f) * CellSizeCm, (L.Key.Y + 0.5f) * CellSizeCm);
		if (FVector2D::Distance(CellCentre, FVector2D(View)) > UnloadRadiusCm) ToUnload.Add(L.Key);
	}
	for (const FIntPoint& C : ToUnload) UnloadCell(C);
}

void AMRGeoWorld::RequestCell(const FIntPoint& Cell, bool bSynchronous)
{
	const FString Path = FPaths::Combine(GeoDataDir(), FString::Printf(TEXT("cell_%d_%d.json"), Cell.X, Cell.Y));
	TSharedPtr<const FMRTerrain> Terrain = GetTerrain();
	const float Size = CellSizeCm;
	if (bSynchronous)
	{
		const double T0 = FPlatformTime::Seconds();
		TSharedPtr<FCellBuild> B = GeoBuild::BuildCell(Path, Cell, Size, Terrain);
		UE_LOG(LogManchesterRide, Verbose, TEXT("Cell %d,%d built synchronously in %.0f ms"), Cell.X, Cell.Y, (FPlatformTime::Seconds() - T0) * 1000.0);
		FinishCell(B);
		return;
	}
	Pending.Add(Cell);
	// The worker only touches the shared queue (never `this`), so the actor can be destroyed safely mid-build.
	TSharedPtr<FReadyQueue> Queue = Ready;
	TSharedPtr<TAtomic<bool>> Alive = bAlive;
	Async(EAsyncExecution::ThreadPool, [Queue, Alive, Path, Cell, Size, Terrain]()
	{
		TSharedPtr<FCellBuild> B = GeoBuild::BuildCell(Path, Cell, Size, Terrain);
		if (*Alive) Queue->Enqueue(B);
	});
}

bool AMRGeoWorld::BeginFinish(const TSharedPtr<FCellBuild>& Build)
{
	Pending.Remove(Build->Cell);
	if (Loaded.Contains(Build->Cell)) return false; // already built synchronously
	FLoadedCell& L = Loaded.FindOrAdd(Build->Cell);
	Places.Append(Build->Places);
	L.MapLines = MoveTemp(Build->MapLines);
	L.LampLights = MoveTemp(Build->LampLights);
	return true;
}

void AMRGeoWorld::FinishSection(FLoadedCell& L, const FSection& S)
{
	const GeoBuild::FGroupDef& G = GeoBuild::Groups[S.Group];
	UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(this);
	PMC->SetupAttachment(Root);
	// Ground, roads and paths cook collision immediately (the rider may be standing on them this frame);
	// trees, street furniture and buildings can cook in the background.
	PMC->bUseAsyncCooking = S.Group >= GeoBuild::Foliage;
	// Ground, roads, paths, grass, water and markings are near-flat sheets covering the whole cell: as
	// non-Nanite casters they flood the virtual shadow map marking queue ("Non-Nanite Marking Job Queue
	// overflow") while adding almost no visible shadow. Trees, street furniture and buildings cast
	// (in quarter-cell pieces, see BuildCell); everything receives.
	PMC->SetCastShadow(GeoBuild::CastsShadow(S.Group));
	TArray<FVector> Normals;
	Normals.SetNumUninitialized(S.Verts.Num());
	// Recompute per-vertex normals from the (already oriented) triangles
	for (int32 i = 0; i + 2 < S.Tris.Num(); i += 3)
	{
		const FVector& A = S.Verts[S.Tris[i]]; const FVector& B = S.Verts[S.Tris[i + 1]]; const FVector& C = S.Verts[S.Tris[i + 2]];
		FVector N = FVector::CrossProduct(B - C, A - C).GetSafeNormal();
		if (GeoBuild::GFlipWinding) N = -N;
		Normals[S.Tris[i]] = Normals[S.Tris[i + 1]] = Normals[S.Tris[i + 2]] = N;
	}
	if (S.Colors.Num() == S.Verts.Num())
		PMC->CreateMeshSection(0, S.Verts, S.Tris, Normals, TArray<FVector2D>(), S.Colors, TArray<FProcMeshTangent>(), G.bCollide);
	else
		PMC->CreateMeshSection_LinearColor(0, S.Verts, S.Tris, Normals, TArray<FVector2D>(), TArray<FLinearColor>(), TArray<FProcMeshTangent>(), G.bCollide);
	PMC->SetMaterial(0, GroupMaterials[S.Group]);
	if (G.bCollide)
	{
		PMC->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		PMC->SetPhysMaterialOverride(SurfaceMaterials[static_cast<int32>(G.Surface)]);
	}
	else
	{
		PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (S.Group <= GeoBuild::Rail) PMC->ComponentTags.Add(TEXT("MRGroundLayer"));
	if (S.Group <= GeoBuild::MarkYellow) PMC->ComponentTags.Add(TEXT("MRKeepInPhoto")); // ground and road markings stay visible in photoreal mode
	if (G.bCollide) PMC->ComponentTags.Add(TEXT("MRCollide"));
	PMC->RegisterComponent();
	if (bPhotoMode) ApplyPhotoMode(PMC);
	L.Components.Add(PMC);
	LiveComponents.Add(PMC);
}

void AMRGeoWorld::ApplyPhotoMode(UProceduralMeshComponent* PMC) const
{
	// Hybrid photoreal: our crisp, photo-textured ground and road markings stay (you ride on them);
	// buildings, trees and street furniture are hidden so Google's scan shows instead, but keep their
	// collision: they stand on the same OSM footprints as the scanned buildings, so walls stay solid.
	if (!PMC) return;
	PMC->SetVisibility(!bPhotoMode || PMC->ComponentHasTag(TEXT("MRKeepInPhoto")));
}

void AMRGeoWorld::SetPhotoMode(bool bOn)
{
	if (bPhotoMode == bOn) return;
	bPhotoMode = bOn;
	for (UProceduralMeshComponent* C : LiveComponents) ApplyPhotoMode(C);
	UE_LOG(LogManchesterRide, Log, TEXT("Geo world photo mode %s"), bOn ? TEXT("on") : TEXT("off"));
}

void AMRGeoWorld::FinishCell(TSharedPtr<FCellBuild> Build)
{
	if (!BeginFinish(Build)) return;
	FLoadedCell& L = Loaded.FindChecked(Build->Cell);
	for (const FSection& S : Build->Sections) FinishSection(L, S);
	L.bGroundReady = true;
}

void AMRGeoWorld::FinishPending(double Deadline)
{
	// Sections are finished one at a time against a frame budget, so a dense cell (hundreds of trees,
	// lamps and buildings) never causes a single long frame. Sections are ordered ground first.
	while (FPlatformTime::Seconds() < Deadline || Deadline <= 0.0)
	{
		if (!Finishing.IsValid())
		{
			TSharedPtr<FCellBuild> B;
			if (!Ready->Dequeue(B)) return;
			if (!BeginFinish(B)) continue;
			Finishing = B;
			FinishingNext = 0;
		}
		FLoadedCell* L = Loaded.Find(Finishing->Cell);
		if (!L) { Finishing.Reset(); continue; } // unloaded while finishing
		if (FinishingNext >= Finishing->Sections.Num())
		{
			L->bGroundReady = true;
			Finishing.Reset();
			if (Deadline <= 0.0) return;
			continue;
		}
		const FSection& S = Finishing->Sections[FinishingNext++];
		if (GeoBuild::CastsShadow(S.Group)) L->bGroundReady = true; // all ground/road sections are in
		FinishSection(*L, S);
	}
}

void AMRGeoWorld::UnloadCell(const FIntPoint& Cell)
{
	if (FLoadedCell* L = Loaded.Find(Cell))
	{
		for (UProceduralMeshComponent* C : L->Components)
		{
			if (C) { LiveComponents.RemoveSwap(C); C->DestroyComponent(); }
		}
	}
	Loaded.Remove(Cell);
	if (Finishing.IsValid() && Finishing->Cell == Cell) Finishing.Reset();
}

void AMRGeoWorld::UpdateNightLighting()
{
	// 0 in daylight, 1 at night, easing through dusk (18:30-20:00) and dawn (6:00-7:30).
	float Night = 0.f;
	if (const AMRWorldEnvironment* Env = AMRWorldEnvironment::Get(this))
	{
		const float H = Env->GetTimeOfDayHours();
		Night = FMath::Max(1.f - FMath::SmoothStep(6.f, 7.5f, H), FMath::SmoothStep(18.5f, 20.f, H));
	}
	for (UMaterialInstanceDynamic* M : NightFacades) if (M) M->SetScalarParameterValue(TEXT("Night"), Night);
	for (UMaterialInstanceDynamic* M : NightLamps) if (M) M->SetScalarParameterValue(TEXT("Emissive"), 0.15f + Night * 12.f);
	CurrentNight = Night;
}

void AMRGeoWorld::UpdateLampPool()
{
	if (IsRunningDedicatedServer()) return;
	constexpr int32 PoolSize = 24;
	if (LampPool.Num() == 0)
	{
		for (int32 i = 0; i < PoolSize; ++i)
		{
			UPointLightComponent* L = NewObject<UPointLightComponent>(this);
			L->SetupAttachment(Root);
			L->SetMobility(EComponentMobility::Movable);
			L->SetIntensityUnits(ELightUnits::Candelas);
			L->SetIntensity(0.f);
			L->SetAttenuationRadius(2200.f);
			L->SetLightColor(FLinearColor(1.f, 0.8f, 0.55f));
			L->SetCastShadows(false);
			L->SetVisibility(false);
			L->RegisterComponent();
			LampPool.Add(L);
		}
	}
	const bool bOn = CurrentNight > 0.02f;
	TArray<TPair<double, FVector>> Near;
	if (bOn)
	{
		const FVector View = GetViewLocation();
		for (const TPair<FIntPoint, FLoadedCell>& C : Loaded)
			for (const FVector& P : C.Value.LampLights)
			{
				const double D = FVector::DistSquared2D(P, View);
				if (D < FMath::Square(15000.0)) Near.Add({ D, P });
			}
		Near.Sort([](const TPair<double, FVector>& A, const TPair<double, FVector>& B) { return A.Key < B.Key; });
	}
	for (int32 i = 0; i < LampPool.Num(); ++i)
	{
		UPointLightComponent* L = LampPool[i];
		if (!L) continue;
		const bool bUse = bOn && Near.IsValidIndex(i);
		L->SetVisibility(bUse);
		if (!bUse) continue;
		L->SetWorldLocation(Near[i].Value);
		L->SetIntensity(700.f * CurrentNight);
	}
}

void AMRGeoWorld::ForEachMapLine(const FBox2f& Area, TFunctionRef<void(const FMapLine&)> Fn) const
{
	for (const TPair<FIntPoint, FLoadedCell>& C : Loaded)
	{
		for (const FMapLine& L : C.Value.MapLines)
		{
			if (L.Bounds.Intersect(Area)) Fn(L);
		}
	}
}

FString AMRGeoWorld::GetNearestPlaceName(const FVector& Location) const
{
	FString Best;
	float BestD = TNumericLimits<float>::Max();
	for (const TPair<FString, FVector>& P : Places)
	{
		const float D = FVector::DistSquared2D(P.Value, Location);
		if (D < BestD) { BestD = D; Best = P.Key; }
	}
	return Best;
}
