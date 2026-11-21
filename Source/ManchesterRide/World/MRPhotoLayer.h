#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MRPhotoLayer.generated.h"

/**
 * Optional photoreal layer: Google Photorealistic 3D Tiles streamed through Cesium ion
 * (asset 2275207) with the player's own free Cesium ion token.
 *
 * Needs the Cesium for Unreal plugin in Plugins/CesiumForUnreal (WITH_MR_CESIUM). Without it,
 * IsAvailable() is false and the Settings entry says so; the game builds and runs as normal.
 *
 * Alignment: our map is a transverse-Mercator plane around the Town Hall, Cesium uses the curved
 * WGS84 globe. The layer pins the globe to our map at an anchor near the rider (lat/lon from our
 * projection, height from our terrain + the geoid), rotates it by the grid convergence there, then
 * measures the real tile surface under the anchor and corrects the height. It re-anchors every
 * 1.2 km, so the curvature/projection error stays under ~15 cm where you ride.
 *
 * While the layer is on, AMRGeoWorld hides its own visuals and keeps its ground collision 60 cm
 * below as a safety net; riding happens on the photoreal surface (Cesium physics meshes).
 * Each machine streams its own tiles; nothing is replicated.
 */
UCLASS()
class MANCHESTERRIDE_API AMRPhotoLayer : public AActor
{
	GENERATED_BODY()

public:
	AMRPhotoLayer();

	static bool IsAvailable();
	/** Turn the layer on/off for this machine. Token is the player's Cesium ion access token. */
	static void Apply(const UObject* WorldContext, bool bOn, const FString& Token);

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	static constexpr int64 GoogleTilesIonAsset = 2275207;

private:
	void Start(const FString& Token);
	void Anchor(const FVector& UnrealPos);
	bool MeasureOffset(float& OutDeltaCm) const;
	FVector GetViewPos() const;

	UPROPERTY() TObjectPtr<AActor> Georeference;
	UPROPERTY() TObjectPtr<AActor> Tileset;
	FVector AnchorPos = FVector::ZeroVector; // Unreal position the globe is pinned to
	float HeightFix = 0.f;                   // measured correction (cm)
	float CalibTimer = 0.f;
	int32 CalibTries = 0;
	bool bCalibrated = false;
};
