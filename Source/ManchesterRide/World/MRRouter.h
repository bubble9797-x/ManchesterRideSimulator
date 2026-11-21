#pragma once

#include "CoreMinimal.h"

/**
 * Sat-nav for map waypoints: the county road network from Content/GeoData/routes.bin (written by
 * Tools/geo_pipeline/make_routes.mjs) and an A* route finder over it. Unreal cm, +X east, +Y south.
 *
 * Roads, cycleways, tracks and shared paths are included; motorways, footways and steps are not.
 * One-way streets are obeyed (cycleways and paths are two-way). Read-only after load, so routes can
 * be found on worker threads.
 */
struct MANCHESTERRIDE_API FMRRouteGraph
{
	struct FEdge { int32 A, B, First, Num; float Len, Cost; uint32 Flags; };
	TArray<FVector2f> Nodes;
	TArray<FVector2f> Points;
	TArray<FEdge> Edges;
	// Per node: outgoing (edge, forward?) in a flat array, CSR style.
	TArray<int32> AdjStart; // Nodes.Num() + 1
	TArray<int32> Adj;      // edge index * 2 + (1 if travelling A->B, 0 if B->A)
	// 250 m buckets of edge indices for snapping (main network only).
	static constexpr float BucketCm = 25000.f;
	FVector2f GridMin = FVector2f::ZeroVector;
	int32 GridW = 0, GridH = 0;
	TArray<TArray<int32>> Buckets;

	/** Starts loading on first call; returns null until ready (or if routes.bin is missing). */
	static TSharedPtr<const FMRRouteGraph> Get();

	/** Loads and indexes a routes.bin file (used by Get and the tests). */
	bool Load(const FString& Path);

	/**
	 * Route along the network from From to To. OutPts runs from the point on the road nearest From
	 * to the point nearest To. Returns false if either end is more than MaxSnapCm from a road.
	 */
	bool FindRoute(const FVector2D& From, const FVector2D& To, TArray<FVector2f>& OutPts, float& OutLenCm, float MaxSnapCm = 30000.f) const;

	struct FSnap { int32 Edge = INDEX_NONE; int32 Seg = 0; float T = 0.f; float Along = 0.f; FVector2f P = FVector2f::ZeroVector; };
	bool Snap(const FVector2D& P, float MaxCm, FSnap& Out) const;
};

/** Distance from P to a polyline, and the index of the segment it is nearest (for progress along a route). */
MANCHESTERRIDE_API float MRDistanceToPolyline(const TArray<FVector2f>& Pts, const FVector2D& P, int32 FirstSeg, int32 LastSeg, int32& OutSeg, float& OutT);
