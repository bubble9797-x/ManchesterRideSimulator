#pragma once

#include "CoreMinimal.h"

/**
 * County-wide road map for the full-screen map (M): Content/GeoData/overview.json, written by
 * Tools/geo_pipeline/make_overview.mjs. Loaded once on a worker thread; all coordinates are
 * Unreal cm (+X east, +Y south). Read-only after load, so the HUD can use it freely.
 */
struct FMRMapData
{
	enum EKind : uint8 { Motorway, MajorRoad, Street, Rail, Tram, River, Canal, NumKinds };

	struct FLine { uint8 Kind; FBox2f Bounds; int32 First; int32 Num; };
	struct FPlace { FString Name; uint8 Rank; FVector2f P; }; // Rank 0 city, 1 town, 2 suburb/village, 3 neighbourhood/other
	struct FLabel { FString Name; FVector2f P; float AngleDeg; uint8 Kind; };

	TArray<FVector2f> Points;
	TArray<FLine> Lines;
	TArray<FPlace> Places;
	TArray<FLabel> Labels;

	// 2 km buckets of line and label indices for culling.
	static constexpr float BucketCm = 200000.f;
	FVector2f GridMin = FVector2f::ZeroVector;
	int32 GridW = 0, GridH = 0;
	TArray<TArray<int32>> LineBuckets, LabelBuckets;

	/** Starts loading on first call; returns null until ready. */
	static TSharedPtr<const FMRMapData> Get();

	/** Calls Fn(LineIndex) once per line whose bounds overlap Area. */
	void ForEachLine(const FBox2f& Area, TFunctionRef<void(const FLine&)> Fn) const;
	void ForEachLabel(const FBox2f& Area, TFunctionRef<void(const FLabel&)> Fn) const;

private:
	bool Load(const FString& Path);
	void BuildGrid();
};
