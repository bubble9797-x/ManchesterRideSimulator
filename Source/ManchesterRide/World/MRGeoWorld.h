#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MRGeoWorld.generated.h"

class UProceduralMeshComponent;
class UPhysicalMaterial;
class UMaterialInterface;

/**
 * Real ground heights for the whole county (Content/GeoData/terrain.bin, written by
 * Tools/geo_pipeline/fetch_terrain.mjs): a 16 m grid of int16 decimetres. Immutable once
 * loaded, so worker threads share it freely.
 */
struct FMRTerrain
{
	int32 MinX = 0, MinY = 0, SpacingCm = 1600, NX = 0, NY = 0;
	float BaseHeightM = 0.f; // metres above sea level at Unreal Z = 0
	TArray<int16> Heights;

	bool Load(const FString& Path);
	/** Bilinear sample of the raw grid (cm). Clamped at the grid edge. */
	float SampleRaw(double X, double Y) const;
	/** Height of the GROUND MESH (cm): the triangulated 8 m lattice that the game actually builds and collides with. */
	float GroundZ(double X, double Y) const;

	static constexpr double LatticeCm = 800.0;
};

/**
 * Streams real Greater Manchester geometry produced by Tools/geo_pipeline
 * (Content/GeoData/cell_X_Y.json) around the local viewer and builds it as
 * procedural meshes: road ribbons (by class and surface), extruded building
 * footprints at their tagged/estimated heights, parks, water and ground.
 *
 * Each machine streams around its OWN view: the server never needs geometry for
 * remote riders because movement is owner-simulated (see ARCHITECTURE.md), so a
 * listen host's CPU/RAM cost does not grow with how spread out players are.
 *
 * This is the Milestone 4 bridge between the data pipeline and World Partition:
 * the same cell files will be baked into World Partition cells + HLODs by an
 * editor importer (TODO.md). Until then this gives a playable, accurate layout.
 */
UCLASS()
class MANCHESTERRIDE_API AMRGeoWorld : public AActor
{
	GENERATED_BODY()

public:
	AMRGeoWorld();

	static bool HasGeoData();
	static FString GeoDataDir();

	/** County terrain, loaded on first use (game thread). Null if terrain.bin is missing (flat world). */
	static TSharedPtr<const FMRTerrain> GetTerrain();
	/** Height of the solid ground mesh at X/Y (cm). False when there is no terrain data. */
	static bool GetGroundHeight(double X, double Y, float& OutZ);
	/** True once the cell containing Location has its meshes and collision built. */
	bool IsGroundReadyAt(const FVector& Location) const;
	static AMRGeoWorld* Find(const UObject* WorldContext);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** A good spawn point (Piccadilly Gardens) and spacing for multiple players. */
	FTransform GetSpawnTransform(int32 PlayerIndex) const;

	int32 GetLoadedCellCount() const { return Loaded.Num(); }

	/** Photoreal layer on: hide our own visuals, drop props/building collision, and keep our ground
	 *  collision 60 cm lower as a safety net under the photoreal surface. */
	void SetPhotoMode(bool bOn);
	bool IsPhotoMode() const { return bPhotoMode; }
	FString GetNearestPlaceName(const FVector& Location) const;

	/** Mesh data built on a worker thread, turned into components on the game thread. */
	struct FSection
	{
		TArray<FVector> Verts;
		TArray<int32> Tris;
		TArray<FColor> Colors; // per-vertex (buildings: wall colour in RGB, roof slate/tile in A); empty otherwise
		int32 Group = 0; // index into the material/surface groups
	};
	/** 2D outline for the HUD minimap: a road/path/water/rail centreline or a building footprint. */
	enum class EMapLine : uint8 { MajorRoad, MinorRoad, Path, Cycle, Water, Rail, Building };
	struct FMapLine
	{
		TArray<FVector2f> Pts; // Unreal X/Y (cm)
		FBox2f Bounds;
		EMapLine Kind = EMapLine::MinorRoad;
	};
	struct FCellBuild
	{
		FIntPoint Cell;
		TArray<FSection> Sections;
		TArray<TPair<FString, FVector>> Places;
		TArray<FMapLine> MapLines;
		TArray<FVector> LampLights; // street lamp heads (night light pool candidates)
	};

	/** Calls Fn for every minimap line in the loaded cells that overlaps Area (Unreal X/Y, cm). */
	void ForEachMapLine(const FBox2f& Area, TFunctionRef<void(const FMapLine&)> Fn) const;

protected:
	UPROPERTY() TObjectPtr<USceneComponent> Root;
	UPROPERTY() TObjectPtr<UMaterialInterface> BaseMaterial;
	UPROPERTY(Transient) TArray<TObjectPtr<UPhysicalMaterial>> SurfaceMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInterface>> GroupMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> LiveComponents; // keeps streamed meshes referenced for GC
	UPROPERTY() TObjectPtr<UMaterialInterface> GroundMaterial;  // /Game/MR/Materials/M_MRGround
	UPROPERTY() TObjectPtr<UMaterialInterface> FacadeMaterial;  // /Game/MR/Materials/M_MRFacade
	UPROPERTY() TObjectPtr<UMaterialInterface> FoliageMaterial; // /Game/MR/Materials/M_MRFoliage (canopies and bark; null = surface material)
	UPROPERTY() TObjectPtr<UMaterialInterface> SurfaceMaterial; // /Game/MR/Materials/M_MRSurface
	UPROPERTY(Transient) TArray<TObjectPtr<class UMaterialInstanceDynamic>> NightFacades; // windows light up at night
	UPROPERTY(Transient) TArray<TObjectPtr<class UMaterialInstanceDynamic>> NightLamps;

private:
	void UpdateStreaming();
	void RequestCell(const FIntPoint& Cell, bool bSynchronous = false);
	void FinishCell(TSharedPtr<FCellBuild> Build);
	void UnloadCell(const FIntPoint& Cell);
	void UpdateNightLighting();
	float NightTimer = 0.f;
	FVector GetViewLocation() const;

	struct FLoadedCell { TArray<TObjectPtr<UProceduralMeshComponent>> Components; TArray<FMapLine> MapLines; TArray<FVector> LampLights; bool bGroundReady = false; };
	// Real lights for the nearest street lamps at night (a fixed pool moved to the closest lamp heads).
	UPROPERTY(Transient) TArray<TObjectPtr<class UPointLightComponent>> LampPool;
	float CurrentNight = 0.f;
	bool bPhotoMode = false;
	void ApplyPhotoMode(UProceduralMeshComponent* PMC) const;
	void UpdateLampPool();
	bool BeginFinish(const TSharedPtr<FCellBuild>& Build);
	void FinishSection(FLoadedCell& L, const FSection& S);
	void FinishPending(double Deadline); // Deadline <= 0: finish the current cell completely
	TSharedPtr<FCellBuild> Finishing;
	int32 FinishingNext = 0;
	TMap<FIntPoint, FLoadedCell> Loaded;
	TSet<FIntPoint> Pending;
	using FReadyQueue = TQueue<TSharedPtr<FCellBuild>, EQueueMode::Mpsc>;
	TSharedPtr<FReadyQueue> Ready;
	TArray<TPair<FString, FVector>> Places;

	float CellSizeCm = 25600.f;
	float LoadRadiusCm = 70000.f;   // 700 m
	float UnloadRadiusCm = 95000.f;
	float StreamTimer = 0.f;
	float StatTimer = 0.f;
	int32 StatFrames = 0;
	TSharedPtr<TAtomic<bool>> bAlive;
};
