#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RideSimTypes.h"
#include "MRTestEnvironment.generated.h"

class UStaticMesh;
class UMaterialInterface;
class UPhysicalMaterial;
class UInstancedStaticMeshComponent;

/**
 * Milestone 1-3 test district, generated procedurally on every machine so the
 * project is playable with zero binary map assets. Covers every simulated surface,
 * jumps, a skatepark, a dirt track, slopes and a UK-style terraced street
 * (left-hand traffic markings, kerbs, lamp posts, bus stop, post box).
 *
 * This is NOT the Greater Manchester map; that comes from the geo pipeline in
 * Milestone 4 (Tools/geo_pipeline). Repeated props use instanced static meshes.
 */
UCLASS()
class MANCHESTERRIDE_API AMRTestEnvironment : public AActor
{
	GENERATED_BODY()

public:
	AMRTestEnvironment();
	virtual void BeginPlay() override;

	/** Spawn transforms on the main street, spaced so players don't overlap. */
	FTransform GetSpawnTransform(int32 PlayerIndex) const;

	/** Named areas (for the HUD map / GPS). */
	struct FArea { FString Name; FVector2D Center; FVector2D Size; FLinearColor Color; };
	const TArray<FArea>& GetAreas() const { return Areas; }

	/** Street lights built before the world environment existed (handed over once). */
	TArray<TObjectPtr<class UPointLightComponent>> TakePendingNightLights() { return MoveTemp(PendingNightLights); }

protected:
	UPROPERTY() TObjectPtr<USceneComponent> Root;
	UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> CylinderMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> ConeMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> SphereMesh;
	UPROPERTY() TObjectPtr<UMaterialInterface> BaseMaterial;
	UPROPERTY(Transient) TArray<TObjectPtr<UPhysicalMaterial>> SurfaceMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<UPrimitiveComponent>> BuiltComponents; // GC references for runtime-built parts
	UPROPERTY(Transient) TArray<TObjectPtr<class UPointLightComponent>> PendingNightLights;

private:
	void Build();
	UStaticMeshComponent* Box(const FVector& Center, const FVector& Size, const FRotator& Rot, const FLinearColor& Color, MRSim::ESurface Surface, bool bCollide = true);
	UStaticMeshComponent* Disc(const FVector& Center, float Radius, float Height, const FLinearColor& Color, MRSim::ESurface Surface);
	UInstancedStaticMeshComponent* MakeISM(UStaticMesh* Mesh, const FLinearColor& Color, MRSim::ESurface Surface, bool bCollide);
	void Ramp(const FVector& Base, float Yaw, float Length, float Width, float Height, const FLinearColor& Color, MRSim::ESurface Surface);
	void Tabletop(const FVector& Center, float Yaw, float Height, float TopLength, float RampLength, float Width, const FLinearColor& Color, MRSim::ESurface Surface);
	void BuildStreet();
	void BuildSkatepark();
	void BuildDirtTrack();
	void BuildTrails();
	void BuildSurfacePads();

	TArray<FArea> Areas;
	bool bBuilt = false;
};
