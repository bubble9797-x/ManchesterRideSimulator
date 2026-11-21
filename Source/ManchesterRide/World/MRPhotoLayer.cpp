#include "World/MRPhotoLayer.h"

#include "ManchesterRide.h"
#include "GeoProjection.h"
#include "World/MRGeoWorld.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

#if WITH_MR_CESIUM
#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#endif

namespace
{
	constexpr double ReanchorCm = 120000.0;  // 1.2 km
	constexpr double GeoidM = 49.6;          // WGS84 ellipsoid above mean sea level around Manchester (approx.; measured fix below)
}

AMRPhotoLayer::AMRPhotoLayer()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
	bReplicates = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

bool AMRPhotoLayer::IsAvailable()
{
	return WITH_MR_CESIUM != 0;
}

void AMRPhotoLayer::Apply(const UObject* WorldContext, bool bOn, const FString& Token)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!World || World->GetNetMode() == NM_DedicatedServer) return;
	AMRPhotoLayer* Existing = nullptr;
	for (TActorIterator<AMRPhotoLayer> It(World); It; ++It) { Existing = *It; break; }
	AMRGeoWorld* Geo = AMRGeoWorld::Find(WorldContext);

	if (!bOn || Token.IsEmpty() || !IsAvailable() || !Geo)
	{
		if (Existing) Existing->Destroy();
		if (Geo) Geo->SetPhotoMode(false);
		if (bOn && Token.IsEmpty()) UE_LOG(LogManchesterRide, Warning, TEXT("Photoreal map: no Cesium ion token set (Settings > Cesium ion token)"));
		return;
	}
	if (Existing) Existing->Destroy(); // restart cleanly (e.g. new token)
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AMRPhotoLayer* Layer = World->SpawnActor<AMRPhotoLayer>(AMRPhotoLayer::StaticClass(), FTransform::Identity, P))
	{
		Layer->Start(Token);
		Geo->SetPhotoMode(true);
	}
}

FVector AMRPhotoLayer::GetViewPos() const
{
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PC->GetPawn()) return Pawn->GetActorLocation();
		FVector L; FRotator R;
		PC->GetPlayerViewPoint(L, R);
		return L;
	}
	return FVector::ZeroVector;
}

void AMRPhotoLayer::Start(const FString& Token)
{
#if WITH_MR_CESIUM
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACesiumGeoreference* Geo = GetWorld()->SpawnActor<ACesiumGeoreference>(ACesiumGeoreference::StaticClass(), FTransform::Identity, P);
	ACesium3DTileset* Tiles = GetWorld()->SpawnActor<ACesium3DTileset>(ACesium3DTileset::StaticClass(), FTransform::Identity, P);
	if (!Geo || !Tiles) return;
	Tiles->AttachToActor(Geo, FAttachmentTransformRules::KeepRelativeTransform);
	Tiles->SetGeoreference(Geo); // explicit: otherwise it can pick up a default georeference (centred on Denver) and show the wrong place
	Tiles->SetTilesetSource(ETilesetSource::FromCesiumIon);
	Tiles->SetIonAssetID(GoogleTilesIonAsset);
	Tiles->SetIonAccessToken(Token);
	Tiles->SetCreatePhysicsMeshes(true); // only for the height calibration traces (object-type queries)
	// Hybrid: you ride on our ground, so the scan must not block wheels (Visibility traces), the camera or bodies.
	Tiles->BodyInstance.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Tiles->BodyInstance.SetResponseToAllChannels(ECR_Ignore);
	// Sharper close up (default 16): at riding height the street is only metres away. More memory for the cache to match.
	Tiles->SetMaximumScreenSpaceError(6.0);
	Tiles->MaximumCachedBytes = 1024ll * 1024 * 1024;
	Georeference = Geo;
	Tileset = Tiles;
	Anchor(GetViewPos());
	UE_LOG(LogManchesterRide, Log, TEXT("Photoreal map on (Cesium ion asset %lld)"), GoogleTilesIonAsset);
#else
	(void)Token;
#endif
}

void AMRPhotoLayer::Anchor(const FVector& UnrealPos)
{
#if WITH_MR_CESIUM
	ACesiumGeoreference* Geo = Cast<ACesiumGeoreference>(Georeference);
	if (!Geo) return;
	float GroundZ = 0.f;
	AMRGeoWorld::GetGroundHeight(UnrealPos.X, UnrealPos.Y, GroundZ);
	const TSharedPtr<const FMRTerrain> T = AMRGeoWorld::GetTerrain();
	const double BaseM = T.IsValid() ? T->BaseHeightM : 0.0;
	AnchorPos = FVector(UnrealPos.X, UnrealPos.Y, GroundZ);

	// Our map coordinates -> WGS84 at the anchor.
	const MRGeo::FEastNorth Local = MRGeo::UnrealToLocal(UnrealPos.X, UnrealPos.Y);
	const MRGeo::FLatLon LL = MRGeo::ToLatLon(Local);
	const double HeightM = GroundZ / 100.0 + BaseM + GeoidM;
	Geo->SetOriginLongitudeLatitudeHeight(FVector(LL.Lon, LL.Lat, HeightM));

	// Grid convergence: true north in our transverse-Mercator grid is rotated away from grid north
	// east/west of the central meridian. Cesium's frame at the anchor uses true north, so rotate it.
	MRGeo::FLatLon North = LL;
	North.Lat += 0.01;
	const MRGeo::FEastNorth N2 = MRGeo::ToLocal(North);
	const double Gamma = FMath::Atan2(N2.East - Local.East, N2.North - Local.North);
	Geo->SetActorLocationAndRotation(AnchorPos + FVector(0, 0, HeightFix), FRotator(0.f, FMath::RadiansToDegrees(Gamma), 0.f));
	bCalibrated = false;
	CalibTries = 0;
	CalibTimer = 0.f;
#else
	(void)UnrealPos;
#endif
}

bool AMRPhotoLayer::MeasureOffset(float& OutDeltaCm) const
{
	if (!Tileset) return false;
	TArray<float> Deltas;
	for (const FVector2D& O : { FVector2D(0, 0), FVector2D(600, 0), FVector2D(-600, 0), FVector2D(0, 600), FVector2D(0, -600) })
	{
		const FVector P(AnchorPos.X + O.X, AnchorPos.Y + O.Y, 0.f);
		float Ours = 0.f;
		if (!AMRGeoWorld::GetGroundHeight(P.X, P.Y, Ours)) continue;
		TArray<FHitResult> Hits;
		FCollisionQueryParams Q(TEXT("MRPhotoCalib"), true);
		GetWorld()->LineTraceMultiByObjectType(Hits, FVector(P.X, P.Y, Ours + 8000.f), FVector(P.X, P.Y, Ours - 8000.f),
			FCollisionObjectQueryParams::AllStaticObjects, Q);
		for (const FHitResult& H : Hits)
		{
			if (H.GetActor() == Tileset) { Deltas.Add(H.ImpactPoint.Z - Ours); break; }
		}
	}
	if (Deltas.Num() < 3) return false; // tiles under the anchor not loaded yet
	Deltas.Sort();
	OutDeltaCm = Deltas[Deltas.Num() / 2]; // median: ignores a car or bench on one sample
	return true;
}

void AMRPhotoLayer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Georeference) return;
	const FVector View = GetViewPos();
	if (FVector::Dist2D(View, AnchorPos) > ReanchorCm) Anchor(View);
	if (bCalibrated) return;
	CalibTimer += DeltaSeconds;
	if (CalibTimer < 1.f) return;
	CalibTimer = 0.f;
	float Delta = 0.f;
	if (MeasureOffset(Delta))
	{
		// Sink the scan's (melted) ground just under ours, so our crisp ground covers it and its
		// buildings still meet the street.
		constexpr float SinkCm = 25.f;
		const float Err = Delta + SinkCm;
		HeightFix -= Err;
		Georeference->SetActorLocation(AnchorPos + FVector(0, 0, HeightFix));
		if (FMath::Abs(Err) < 5.f) bCalibrated = true;
		UE_LOG(LogManchesterRide, Log, TEXT("Photoreal map: height offset %.0f cm (fix now %.0f cm)"), Delta, HeightFix);
	}
	if (++CalibTries > 40) bCalibrated = true; // give up quietly; geoid estimate stays
}

void AMRPhotoLayer::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Tileset) Tileset->Destroy();
	if (Georeference) Georeference->Destroy();
	Super::EndPlay(Reason);
}
