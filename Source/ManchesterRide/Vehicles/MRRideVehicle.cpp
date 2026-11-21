#include "Vehicles/MRRideVehicle.h"

#include "ManchesterRide.h"
#include "Audio/MRRideAudioComponent.h"
#include "Core/MRGameMode.h"
#include "Core/MRPlayerState.h"
#include "Net/MRNetworkSettings.h"
#include "Vehicles/MRVehicleLibrary.h"
#include "Vehicles/MRVehicleMeshBuilder.h"
#include "Vehicles/MRRiderBuilder.h"
#include "ProceduralMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "World/MRGeoWorld.h"
#include "World/MRWorldEnvironment.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float SimDt = 1.f / 120.f;
	constexpr int32 MaxSubsteps = 10;
	constexpr float TraceStartPadM = 0.5f;   // traces start this far above the mount to catch penetration
	constexpr float BoxClearanceCm = 28.f;   // box bottom height above the contact line
	constexpr float CrashRespawnDelay = 2.5f;

	MRSim::ESurface SurfaceFromPhysical(EPhysicalSurface S)
	{
		const int32 Index = static_cast<int32>(S) - static_cast<int32>(SurfaceType1);
		if (Index >= 0 && Index < static_cast<int32>(MRSim::ESurface::Count))
		{
			return static_cast<MRSim::ESurface>(Index);
		}
		return MRSim::ESurface::Asphalt; // SurfaceType_Default
	}

	bool IsBicycleLike(MRSim::EVehicleCategory C)
	{
		using MRSim::EVehicleCategory;
		return C == EVehicleCategory::EBike || C == EVehicleCategory::BMX || C == EVehicleCategory::MountainBike
			|| C == EVehicleCategory::RoadBike || C == EVehicleCategory::DownhillBike;
	}

	bool IsMotorised(MRSim::EVehicleCategory C)
	{
		return C == MRSim::EVehicleCategory::DirtBike || C == MRSim::EVehicleCategory::Motorcycle;
	}
}

AMRRideVehicle::AMRRideVehicle()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	bReplicates = true;
	SetReplicateMovement(false); // we replicate our own compact state
	SetNetUpdateFrequency(30.f);
	SetNetCullDistanceSquared(FMath::Square(150000.f)); // 1.5 km: riders further away stop replicating

	Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
	Collision->SetBoxExtent(FVector(80.f, 25.f, 45.f));
	Collision->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore); // wheel traces use Visibility
	Collision->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Collision->SetSimulatePhysics(false);
	RootComponent = Collision;

	SpinPivot = CreateDefaultSubobject<USceneComponent>(TEXT("SpinPivot"));
	SpinPivot->SetupAttachment(Collision);
	BodyPivot = CreateDefaultSubobject<USceneComponent>(TEXT("BodyPivot"));
	BodyPivot->SetupAttachment(SpinPivot);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(Collision);
	CameraBoom->TargetArmLength = 380.f;
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritPitch = false;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bInheritYaw = true;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 12.f;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraRotationLagSpeed = 7.f;
	CameraBoom->bDoCollisionTest = true;
	CameraBoom->ProbeSize = 15.f;

	ThirdPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ThirdPersonCamera"));
	ThirdPersonCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	ThirdPersonCamera->FieldOfView = 85.f;

	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(BodyPivot);
	FirstPersonCamera->FieldOfView = 95.f;
	FirstPersonCamera->SetAutoActivate(false);

	Audio = CreateDefaultSubobject<UMRRideAudioComponent>(TEXT("RideAudio"));
	Audio->SetupAttachment(Collision);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MatFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = CubeFinder.Object;
	CylinderMesh = CylFinder.Object;
	SphereMesh = SphereFinder.Object;
	BaseMaterial = MatFinder.Object;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SurfaceFinder(TEXT("/Game/MR/Materials/M_MRSurface.M_MRSurface"));
	SurfaceMaterial = SurfaceFinder.Succeeded() ? SurfaceFinder.Object : BaseMaterial; // Tools/make_materials.bat creates it
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RiderFinder(TEXT("/Game/MR/Materials/M_MRRider.M_MRRider"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SkinFinder(TEXT("/Game/MR/Materials/M_MRSkin.M_MRSkin"));
	RiderMaterial = RiderFinder.Succeeded() ? RiderFinder.Object : nullptr;
	SkinMaterial = SkinFinder.Succeeded() ? SkinFinder.Object : nullptr;

	VehicleId = UMRVehicleLibrary::GetDefaultVehicleId();
}

void AMRRideVehicle::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMRRideVehicle, VehicleId);
	DOREPLIFETIME(AMRRideVehicle, PaintColor);
	DOREPLIFETIME(AMRRideVehicle, JacketColor);
	DOREPLIFETIME(AMRRideVehicle, HelmetColor);
	DOREPLIFETIME(AMRRideVehicle, RiderLook);
	DOREPLIFETIME_CONDITION(AMRRideVehicle, ReplicatedState, COND_SkipOwner);
}

void AMRRideVehicle::InitVehicle(FName InVehicleId, FLinearColor InPaint, FLinearColor InJacket, FLinearColor InHelmet)
{
	VehicleId = UMRVehicleLibrary::IsValidVehicle(InVehicleId) ? InVehicleId : UMRVehicleLibrary::GetDefaultVehicleId();
	PaintColor = InPaint;
	JacketColor = InJacket;
	HelmetColor = InHelmet;
	if (HasActorBegunPlay())
	{
		ApplyVehicleConfig();
	}
}

void AMRRideVehicle::SetRiderLook(const FMRRiderLook& InLook)
{
	RiderLook = InLook;
	RiderLook.Sanitise();
	if (HasActorBegunPlay()) ApplyVehicleConfig(); // local preview + listen host; clients get OnRep_VehicleConfig
}

void AMRRideVehicle::BeginPlay()
{
	Super::BeginPlay();
	ApplyVehicleConfig();
	SafeLocation = GetActorLocation();
	SafeYaw = GetActorRotation().Yaw;
	UE_LOG(LogManchesterRide, Log, TEXT("Vehicle %s begins at %s"), *GetName(), *SafeLocation.ToCompactString());
}

void AMRRideVehicle::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	bServerAcceptNextTeleport = true;
}

void AMRRideVehicle::OnRep_VehicleConfig()
{
	ApplyVehicleConfig();
}

void AMRRideVehicle::ApplyVehicleConfig()
{
	const MRSim::FVehicleParams* Found = UMRVehicleLibrary::FindParams(VehicleId);
	if (!Found)
	{
		UE_LOG(LogManchesterRide, Warning, TEXT("Unknown vehicle id '%s', using default"), *VehicleId.ToString());
		Found = UMRVehicleLibrary::FindParams(UMRVehicleLibrary::GetDefaultVehicleId());
	}
	const bool bVehicleChanged = !bConfigApplied || AppliedVehicleId != VehicleId;
	if (bVehicleChanged)
	{
		AppliedVehicleId = VehicleId;
		Params = *Found;
		MRSim::FRideSimulator::InitState(Params, Sim);
		Tricks.Reset();

		const float LengthCm = (Params.Wheelbase + Params.WheelRadiusFront + Params.WheelRadiusRear) * 100.f;
		const float HalfHeight = IsMotorised(Params.Category) ? 45.f : 40.f;
		Collision->SetBoxExtent(FVector(LengthCm * 0.5f, IsMotorised(Params.Category) ? 35.f : 25.f, HalfHeight));
		ContactToCenterUpCm = BoxClearanceCm + HalfHeight;
		ContactToCenterFwdCm = Params.Wheelbase * 50.f;
		SpinPivot->SetRelativeLocation(FVector(0.f, 0.f, -ContactToCenterUpCm));

		CameraBoom->SetRelativeLocation(FVector(0.f, 0.f, -ContactToCenterUpCm + 130.f));
		CameraBoom->TargetArmLength = IsMotorised(Params.Category) ? 460.f : 360.f;

		UMRRideAudioComponent::EVoice Voice = UMRRideAudioComponent::EVoice::Pedal;
		int32 Cyl = 1;
		if (Params.Powertrain.Type == MRSim::EPowertrain::Combustion)
		{
			Voice = UMRRideAudioComponent::EVoice::Combustion;
			Cyl = Params.Powertrain.PeakTorqueNm > 90.f ? 4 : (Params.Powertrain.PeakTorqueNm > 50.f ? 2 : 1);
		}
		else if (Params.Powertrain.Type != MRSim::EPowertrain::Pedal)
		{
			Voice = UMRRideAudioComponent::EVoice::Electric;
		}
		Audio->SetVoice(Voice, Cyl);
		BuildVisuals();
		AppliedLookCode = RiderLook.Encode();
	}
	else if (AppliedLookCode != RiderLook.Encode())
	{
		AppliedLookCode = RiderLook.Encode(); // character editor change: rebuild the rider, keep the ride state
		BuildVisuals();
	}
	UpdateMaterials();
	bConfigApplied = true;
}

// ---------------------------------------------------------------------------
// Placeholder visuals assembled from engine basic shapes, proportioned from the
// vehicle's real geometry (wheelbase, wheel size, COM). Replaced by authored
// meshes in Milestone 7; see TODO.md.
// ---------------------------------------------------------------------------

UStaticMeshComponent* AMRRideVehicle::AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& LocCm, const FRotator& Rot, const FVector& SizeCm, const FLinearColor& Color, int32 MaterialGroup)
{
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(Mesh);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCastShadow(true);
	C->SetupAttachment(Parent);
	C->SetRelativeLocation(LocCm);
	C->SetRelativeRotation(Rot);
	C->SetRelativeScale3D(SizeCm / 100.f);
	C->RegisterComponent();
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	MID->SetVectorParameterValue(TEXT("Color"), Color);
	C->SetMaterial(0, MID);
	switch (MaterialGroup)
	{
	case 0: PaintMaterials.Add(MID); break;
	case 1: JacketMaterials.Add(MID); break;
	case 2: HelmetMaterials.Add(MID); break;
	default: break;
	}
	VisualParts.Add(C);
	return C;
}

void AMRRideVehicle::ClearVisuals()
{
	for (UStaticMeshComponent* C : VisualParts)
	{
		if (C) C->DestroyComponent();
	}
	VisualParts.Reset();
	PaintMaterials.Reset();
	JacketMaterials.Reset();
	HelmetMaterials.Reset();
	for (UProceduralMeshComponent* C : VehicleMeshes)
	{
		if (C) C->DestroyComponent();
	}
	VehicleMeshes.Reset();
	if (FrontWheel) FrontWheel->DestroyComponent();
	if (RearWheel) RearWheel->DestroyComponent();
	if (FrontLowers) FrontLowers->DestroyComponent();
	if (SteerPivot) SteerPivot->DestroyComponent();
	if (SwingPivotComp) SwingPivotComp->DestroyComponent();
	if (Headlight) Headlight->DestroyComponent();
	for (USceneComponent* C : RiderSegs) if (C) C->DestroyComponent();
	RiderSegs.Reset();
	bRagdoll = false;
	if (CrankPivot) CrankPivot->DestroyComponent();
	CrankPivot = nullptr;
	for (int32 s = 0; s < 2; ++s) { if (PedalComps[s]) PedalComps[s]->DestroyComponent(); PedalComps[s] = nullptr; }
	Headlight = nullptr;
	FrontWheel = RearWheel = SteerPivot = FrontLowers = SwingPivotComp = nullptr;
}

void AMRRideVehicle::AddVehicleMesh(USceneComponent* Parent, const FMRPartMesh& Part)
{
	if (!Parent || Part.NumTris() == 0) return;
	UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(this);
	PMC->SetupAttachment(Parent);
	PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PMC->SetCastShadow(true);
	PMC->RegisterComponent();
	int32 Section = 0;
	for (int32 m = 0; m < static_cast<int32>(EMRVisMat::Count); ++m)
	{
		const FMRMeshSection& S = Part.S[m];
		if (S.T.Num() == 0) continue;
		PMC->CreateMeshSection_LinearColor(Section, S.V, S.T, S.N, TArray<FVector2D>(), TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(SurfaceMaterial ? SurfaceMaterial.Get() : BaseMaterial.Get(), this);
		// Kind: micro detail in M_MRSurface (1 paint, 2 brushed alloy, 3 rubber, 4 textured plastic, 5 seat cover).
		FLinearColor Col; float Metal = 0.f, Rough = 0.5f, Emit = 0.f, Kind = 0.f, Coat = 0.f;
		switch (static_cast<EMRVisMat>(m))
		{
		case EMRVisMat::Paint:  Col = PaintColor; Metal = 0.35f; Rough = 0.4f; Kind = 1.f; Coat = 1.f; PaintMaterials.Add(MID); break; // metallic base under lacquer
		case EMRVisMat::Alloy:  Col = FLinearColor(0.70f, 0.70f, 0.72f); Metal = 1.f; Rough = 0.32f; Kind = 2.f; break;
		case EMRVisMat::Black:  Col = FLinearColor(0.03f, 0.03f, 0.033f); Metal = 0.1f; Rough = 0.45f; Kind = 4.f; Coat = 0.3f; break;
		case EMRVisMat::Rubber: Col = FLinearColor(0.025f, 0.024f, 0.023f); Metal = 0.f; Rough = 0.85f; Kind = 3.f; break;
		case EMRVisMat::Chrome: Col = FLinearColor(0.9f, 0.9f, 0.92f); Metal = 1.f; Rough = 0.08f; Kind = 2.f; break;
		case EMRVisMat::Gold:   Col = FLinearColor(0.85f, 0.58f, 0.22f); Metal = 1.f; Rough = 0.22f; Kind = 2.f; break;
		case EMRVisMat::Seat:   Col = FLinearColor(0.035f, 0.035f, 0.04f); Metal = 0.f; Rough = 0.6f; Kind = 5.f; break;
		case EMRVisMat::RedLens: Col = FLinearColor(0.9f, 0.03f, 0.02f); Metal = 0.f; Rough = 0.15f; Emit = 2.5f; break;
		case EMRVisMat::Amber:  Col = FLinearColor(1.f, 0.42f, 0.02f); Metal = 0.f; Rough = 0.15f; Emit = 0.3f; break;
		case EMRVisMat::Plate:  Col = FLinearColor(0.85f, 0.65f, 0.02f); Metal = 0.f; Rough = 0.35f; break;
		default:                Col = FLinearColor(1.f, 0.96f, 0.88f); Metal = 0.f; Rough = 0.1f; Emit = 4.f; break; // Lens
		}
		MID->SetVectorParameterValue(TEXT("Color"), Col);
		MID->SetScalarParameterValue(TEXT("Metallic"), Metal);
		MID->SetScalarParameterValue(TEXT("Roughness"), Rough);
		MID->SetScalarParameterValue(TEXT("Emissive"), Emit);
		MID->SetScalarParameterValue(TEXT("Kind"), Kind);
		MID->SetScalarParameterValue(TEXT("ClearCoat"), Coat);
		PMC->SetMaterial(Section, MID);
		++Section;
	}
	VehicleMeshes.Add(PMC);
}

void AMRRideVehicle::BuildVisuals()
{
	ClearVisuals();
	const float RF = Params.WheelRadiusFront * 100.f;
	const float RR = Params.WheelRadiusRear * 100.f;

	auto Beam = [&](USceneComponent* Parent, const FVector& A, const FVector& B, float Thick, const FLinearColor& Col, int32 Group)
	{
		const FVector D = B - A;
		const float Len = D.Size();
		if (Len < 1.f) return;
		AddPart(Parent, CubeMesh, (A + B) * 0.5f, FRotationMatrix::MakeFromX(D).Rotator(), FVector(Len, Thick, Thick), Col, Group);
	};
	auto MakeScene = [&](USceneComponent* Parent, const FVector& Loc) -> USceneComponent*
	{
		USceneComponent* C = NewObject<USceneComponent>(this);
		C->SetupAttachment(Parent);
		C->SetRelativeLocation(Loc);
		C->RegisterComponent();
		return C;
	};

	// Detailed vehicle built from this model's own geometry (see MRVehicleMeshBuilder.cpp).
	const FMRVehicleVisual Vis = MRBuildVehicleVisual(Params, VehicleId.ToString());
	RearWheel = MakeScene(BodyPivot, FVector(0.f, 0.f, RR));
	SteerPivot = MakeScene(BodyPivot, FVector(Params.Wheelbase * 100.f, 0.f, 0.f));
	FrontWheel = MakeScene(SteerPivot, FVector(0.f, 0.f, RF));
	FrontLowers = MakeScene(SteerPivot, FVector(0.f, 0.f, RF));
	if (Vis.bHasSwingarm)
	{
		SwingPivotComp = MakeScene(BodyPivot, Vis.SwingPivot);
		SwingRest = FVector(0.f, 0.f, RR) - Vis.SwingPivot;
		AddVehicleMesh(SwingPivotComp, Vis.Swingarm);
	}
	AddVehicleMesh(BodyPivot, Vis.Body);
	AddVehicleMesh(SteerPivot, Vis.Steer);
	AddVehicleMesh(FrontLowers, Vis.FrontLowers);
	AddVehicleMesh(RearWheel, Vis.RearWheel);
	AddVehicleMesh(FrontWheel, Vis.FrontWheel);

	if (Vis.bHasFrontLight)
	{
		Headlight = NewObject<USpotLightComponent>(this);
		Headlight->SetupAttachment(SteerPivot);
		Headlight->SetRelativeLocation(Vis.FrontLight - FVector(Params.Wheelbase * 100.f, 0.f, 0.f) + FVector(4.f, 0, 0));
		Headlight->SetRelativeRotation(FRotator(-7.f, 0.f, 0.f));
		Headlight->SetMobility(EComponentMobility::Movable);
		Headlight->SetIntensityUnits(ELightUnits::Candelas);
		Headlight->SetIntensity(IsMotorised(Params.Category) ? 2500.f : 350.f);
		Headlight->SetInnerConeAngle(14.f);
		Headlight->SetOuterConeAngle(30.f);
		Headlight->SetAttenuationRadius(IsMotorised(Params.Category) ? 6000.f : 3500.f);
		Headlight->SetLightColor(FLinearColor(1.f, 0.97f, 0.92f));
		Headlight->SetCastShadows(false);
		Headlight->SetVisibility(false);
		Headlight->RegisterComponent();
	}
	// Cranks and pedals (bicycles) are their own parts so they turn with the rider's legs.
	RigBB = Vis.BB; RigCrankL = Vis.CrankLength; RigPedalY = Vis.PedalY; RigGearRatio = Vis.GearRatio;
	if (Vis.bHasCranks)
	{
		CrankPivot = MakeScene(BodyPivot, Vis.BB);
		AddVehicleMesh(CrankPivot, Vis.Crank);
		for (int32 s = 0; s < 2; ++s)
		{
			PedalComps[s] = MakeScene(BodyPivot, Vis.BB);
			AddVehicleMesh(PedalComps[s], Vis.Pedal[s]);
		}
	}
	RigSeat = Vis.Seat; RigBars = Vis.Bars; RigFoot = Vis.Foot; RigHead = Vis.Head;
	RigFootHalf = Vis.FootHalfWidth; bRigStanding = Vis.bStanding;
	BarHalfWidth = Vis.BarHalfWidth;
	if (FParse::Param(FCommandLine::Get(), TEXT("NoRider"))) // bike-only screenshots
	{
		FirstPersonCamera->SetRelativeLocation(Vis.Head + FVector(8.f, 0, 4.f));
		UpdateRiderPose(0.f);
		return;
	}

	// Rider: a person built from the character editor look, one mesh per body segment.
	FMRRiderLook Look = RiderLook;
	FString LookHex; // -RiderLook=<hex> (automated screenshots): show another look on the local rider
	if ((IsLocallyControlled() || GetNetMode() == NM_Standalone) && FParse::Value(FCommandLine::Get(), TEXT("RiderLook="), LookHex)) Look = FMRRiderLook::Decode(LookHex);
	const bool bMotoGear = Params.Category == MRSim::EVehicleCategory::Motorcycle || Params.Category == MRSim::EVehicleCategory::DirtBike;
	const FMRRiderMesh Rider = MRBuildRiderMesh(Look, bMotoGear);
	RiderDims = Rider.D;
	const FLinearColor SkinCol = FMRRiderLook::SkinTones()[Look.SkinTone];
	const FLinearColor HairCol = FMRRiderLook::HairColours()[Look.HairColour];
	const FLinearColor AccentCol = FMRRiderLook::Palette()[Look.Accent];
	const FLinearColor LegsCol = FMRRiderLook::Palette()[Look.Legs];
	const FLinearColor GloveCol = FMRRiderLook::Palette()[Look.Gloves];
	const FLinearColor ShoeCol = FMRRiderLook::Palette()[Look.Shoes];
	const FLinearColor VisorCol = FMRRiderLook::VisorColours()[Look.Visor];
	const float TopRough = Look.TopStyle == 3 ? 0.32f : Look.TopStyle == 2 ? 0.45f : 0.8f;

	// Surface pattern for each material (M_MRRider "Pattern"; see Tools/make_materials.py).
	enum : uint8 { PPlain, PKnit, PJersey, PCordura, PTwill, PLeather, PPaint, PHair, PStubble, PMesh, PRubber };
	const uint8 TopPat = Look.TopStyle == 0 ? PCordura : Look.TopStyle == 2 ? PJersey : Look.TopStyle == 3 ? PLeather : PKnit;
	const uint8 LegsPat = Look.TopStyle == 3 ? PLeather : Look.TopStyle == 2 ? PJersey : bMotoGear ? PCordura : PTwill;
	auto PatternFor = [&](EMRRiderMat M) -> int32
	{
		switch (M)
		{
		case EMRRiderMat::Hair:   return PHair;
		case EMRRiderMat::Beard:  return Look.FacialHair == 2 ? PHair : PStubble;
		case EMRRiderMat::Top:    case EMRRiderMat::Accent: return TopPat;
		case EMRRiderMat::Legs:   return LegsPat;
		case EMRRiderMat::Gloves: return bMotoGear ? PLeather : PJersey;
		case EMRRiderMat::Shoes:  return bMotoGear ? PLeather : PMesh;
		case EMRRiderMat::Sole:   return PRubber;
		case EMRRiderMat::Helmet: return PPaint;
		case EMRRiderMat::HiVis:  return PJersey;
		case EMRRiderMat::Bag:    case EMRRiderMat::Strap: return PCordura;
		default:                  return -1; // eyes, visor, reflective tape: plain surface material
		}
	};

	auto AddRiderMesh = [&](USceneComponent* Parent, const FMRRiderPart& Part, EMRRiderSeg SegId)
	{
		if (Part.NumTris() == 0) return;
		UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(this);
		PMC->SetupAttachment(Parent);
		PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PMC->SetCastShadow(true);
		PMC->RegisterComponent();
		int32 Section = 0;
		for (int32 m = 0; m < static_cast<int32>(EMRRiderMat::Count); ++m)
		{
			const FMRMeshSection& S = Part.S[m];
			if (S.T.Num() == 0) continue;
			PMC->CreateMeshSection_LinearColor(Section, S.V, S.T, S.N, TArray<FVector2D>(), TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
			const EMRRiderMat Mat = static_cast<EMRRiderMat>(m);
			const bool bSkin = (Mat == EMRRiderMat::Skin || Mat == EMRRiderMat::Lips) && SkinMaterial;
			const int32 Pat = PatternFor(Mat);
			UMaterialInterface* ParentMat = bSkin ? SkinMaterial.Get() : (Pat >= 0 && RiderMaterial) ? RiderMaterial.Get() : SurfaceMaterial ? SurfaceMaterial.Get() : BaseMaterial.Get();
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ParentMat, this);
			if (bSkin) MID->SetScalarParameterValue(TEXT("Pattern"), Mat == EMRRiderMat::Lips ? 1.f : 0.f);
			else if (Pat >= 0 && RiderMaterial)
			{
				// Creases run across the segment's long axis: X for limbs (bone frames), Z for the torso.
				const bool bTorso = SegId == EMRRiderSeg::Torso;
				const bool bLimb = SegId != EMRRiderSeg::Torso && SegId != EMRRiderSeg::Neck && SegId != EMRRiderSeg::Head;
				MID->SetScalarParameterValue(TEXT("Pattern"), static_cast<float>(Pat));
				MID->SetVectorParameterValue(TEXT("WAxis"), bTorso ? FLinearColor(0, 0, 1, 0) : FLinearColor(1, 0, 0, 0));
				// Tight kit (leathers, lycra) creases less than a hoodie or jeans; bags hardly at all.
				const float Fit = Pat == PLeather ? 0.45f : Pat == PJersey ? 0.4f : (Mat == EMRRiderMat::Bag || Mat == EMRRiderMat::Strap) ? 0.15f : 1.f;
				MID->SetScalarParameterValue(TEXT("Wrinkle"), (bTorso ? 0.6f : bLimb ? 1.f : 0.3f) * Fit);
				const FMRRiderDims& D = Rider.D;
				float SegLen = 0.f;
				switch (SegId)
				{
				case EMRRiderSeg::Torso:     SegLen = D.Torso; break;
				case EMRRiderSeg::UpperArmL: case EMRRiderSeg::UpperArmR: SegLen = D.UpperArm; break;
				case EMRRiderSeg::ForearmL:  case EMRRiderSeg::ForearmR:  SegLen = D.Forearm; break;
				case EMRRiderSeg::ThighL:    case EMRRiderSeg::ThighR:    SegLen = D.Thigh; break;
				case EMRRiderSeg::ShinL:     case EMRRiderSeg::ShinR:     SegLen = D.Shin; break;
				default: break;
				}
				MID->SetScalarParameterValue(TEXT("SegLen"), SegLen);
				MID->SetScalarParameterValue(TEXT("Detail"), (Mat == EMRRiderMat::Legs && Look.Legs == 12) ? 1.f : 0.f); // faded denim
			}
			FLinearColor Col; float Metal = 0.f, Rough = 0.6f, Emit = 0.f;
			switch (static_cast<EMRRiderMat>(m))
			{
			case EMRRiderMat::Skin:    Col = SkinCol; Rough = 0.55f; break;
			case EMRRiderMat::Lips:    Col = SkinCol * FLinearColor(0.85f, 0.55f, 0.55f); Rough = 0.4f; break;
			case EMRRiderMat::Eye:     Col = FLinearColor(0.80f, 0.78f, 0.74f); Rough = 0.2f; break;
			case EMRRiderMat::Iris:    Col = FLinearColor(0.035f, 0.022f, 0.012f); Rough = 0.1f; break;
			case EMRRiderMat::Hair:    Col = HairCol; Rough = 0.65f; break;
			case EMRRiderMat::Beard:   Col = Look.FacialHair == 2 ? HairCol : FMath::Lerp(SkinCol, HairCol * 0.6f, 0.45f); Rough = 0.8f; break;
			case EMRRiderMat::Top:     Col = JacketColor; Rough = TopRough; JacketMaterials.Add(MID); break;
			case EMRRiderMat::Accent:  Col = AccentCol; Rough = 0.55f; break;
			case EMRRiderMat::Legs:    Col = LegsCol; Rough = 0.85f; break;
			case EMRRiderMat::Gloves:  Col = GloveCol; Rough = 0.6f; break;
			case EMRRiderMat::Shoes:   Col = ShoeCol; Rough = 0.5f; break;
			case EMRRiderMat::Sole:    Col = FLinearColor(0.03f, 0.03f, 0.03f); Rough = 0.9f; break;
			case EMRRiderMat::Helmet:  Col = HelmetColor; Metal = 0.1f; Rough = 0.16f; HelmetMaterials.Add(MID); break;
			case EMRRiderMat::Visor:   Col = VisorCol; Metal = 0.7f; Rough = 0.05f; break;
			case EMRRiderMat::HiVis:   Col = FLinearColor(0.85f, 0.95f, 0.05f); Rough = 0.6f; Emit = 0.15f; break;
			case EMRRiderMat::Reflect: Col = FLinearColor(0.7f, 0.7f, 0.72f); Metal = 0.6f; Rough = 0.3f; break;
			case EMRRiderMat::Bag:     Col = Look.Backpack == 2 ? FLinearColor(0.06f, 0.06f, 0.07f) : AccentCol; Rough = 0.7f; break;
			default:                   Col = FLinearColor(0.02f, 0.02f, 0.022f); Rough = 0.7f; break; // Strap
			}
			MID->SetVectorParameterValue(TEXT("Color"), Col);
			MID->SetScalarParameterValue(TEXT("Metallic"), Metal);
			MID->SetScalarParameterValue(TEXT("Roughness"), Rough);
			MID->SetScalarParameterValue(TEXT("Emissive"), Emit);
			PMC->SetMaterial(Section, MID);
			++Section;
		}
		VehicleMeshes.Add(PMC);
	};
	for (int32 i = 0; i < static_cast<int32>(EMRRiderSeg::Count); ++i)
	{
		USceneComponent* Seg = MakeScene(BodyPivot, FVector::ZeroVector);
		RiderSegs.Add(Seg);
		AddRiderMesh(Seg, Rider.Seg[i], static_cast<EMRRiderSeg>(i));
	}
	UpdateRiderPose(0.f);
}

void AMRRideVehicle::UpdateRiderPose(float DeltaSeconds)
{
	using MRSim::EVehicleCategory;
	const float Dt = DeltaSeconds;
	const bool bLocal = IsLocallyControlled();
	const uint8 NetFlags = LatestRemote.Flags;
	const bool bCrashedNow = bLocal ? Sim.bCrashed : (NetFlags & EMRNetFlags::Crashed) != 0;
	const bool bAir = bLocal ? Sim.bAirborne : (NetFlags & EMRNetFlags::Airborne) != 0;
	const bool bOffRoad = bLocal ? bOnRoughGround : (NetFlags & EMRNetFlags::OffRoad) != 0;
	const float Spd = GetSpeedMps();
	static const bool bCrankTest = FParse::Param(FCommandLine::Get(), TEXT("CrankTest")); // screenshots: pedal on the spot
	static const bool bStandTest = FParse::Param(FCommandLine::Get(), TEXT("StandTest")); // screenshots: standing pose
	static float CrashTestAt = 0.f; // screenshots: -CrashTest=<seconds>, thrown off at 30 km/h that long after start
	static const bool bCrashTest = FParse::Value(FCommandLine::Get(), TEXT("CrashTest="), CrashTestAt);

	// Motion cues shared by local and remote riders: the actor's velocity and vertical acceleration.
	if (Dt > 0.f)
	{
		if (DeltaSeconds > 0.f) CrankAccel = FMath::Lerp(CrankAccel, (Spd - CrankSpeedPrev) / Dt, FMath::Min(1.f, Dt * 4.f));
		CrankSpeedPrev = Spd;
		const FVector Loc = GetActorLocation();
		if (bHavePrevLoc)
		{
			const FVector V = (Loc - PrevActorLoc) / Dt;
			if (V.SizeSquared() < FMath::Square(9000.f)) // ignore teleports / respawns
			{
				const float NewVz = FMath::Lerp(VertVelSmooth, static_cast<float>(V.Z), FMath::Min(1.f, Dt * 25.f));
				VertAccel = FMath::Clamp((NewVz - VertVelSmooth) / Dt, -3000.f, 8000.f);
				VertVelSmooth = NewVz;
				if (!bCrashedNow) RiderVel = FMath::Lerp(RiderVel, V, FMath::Min(1.f, Dt * 12.f));
			}
		}
		PrevActorLoc = Loc;
		bHavePrevLoc = true;
		const float LongA = bLocal ? Sim.LongAccel : CrankAccel;
		LongAccelSmooth = FMath::Lerp(LongAccelSmooth, FMath::Clamp(LongA, -12.f, 12.f), FMath::Min(1.f, Dt * 6.f));

		// Rider-on-bike spring: when the bike is pushed up (bump, landing) the body lags and the
		// knees and elbows bend; in the air the legs extend a little. ~1.6 Hz, fairly damped.
		const float W = 10.f, Zeta = 0.5f;
		const float Drive = -FMath::Clamp(VertAccel, -300.f, 6000.f);
		const int32 N = FMath::Clamp(FMath::CeilToInt(Dt / 0.008f), 1, 12);
		const float H = Dt / N;
		for (int32 i = 0; i < N; ++i)
		{
			BobVel += (Drive - W * W * BobPos - 2.f * Zeta * W * BobVel) * H;
			BobPos += BobVel * H;
			if (BobPos < -16.f) { BobPos = -16.f; BobVel = FMath::Max(0.f, BobVel); }
			if (BobPos > 5.f) { BobPos = 5.f; BobVel = FMath::Min(0.f, BobVel); }
		}
	}

	// Cranks: turn with the rear wheel through the gearing while pedalling, settle level when freewheeling.
	bool bPedal = false;
	if (CrankPivot)
	{
		bPedal = bCrankTest || (bLocal ? (Input.Throttle > 0.05f && Spd > 0.15f) : (Spd > 1.f && CrankAccel > -0.25f));
		if (bCrashedNow || bRagdoll) bPedal = false;
		if (bPedal)
		{
			const float Rate = bCrankTest ? 1.2f : FMath::Clamp(Spd / FMath::Max(0.1f, Params.WheelRadiusRear) / RigGearRatio, 4.2f, 13.6f); // 40..130 rpm
			CrankAngle -= Rate * Dt;
		}
		else
		{
			const float Rest = FMath::RoundToFloat(CrankAngle / PI) * PI;
			CrankAngle = FMath::FInterpConstantTo(CrankAngle, Rest, Dt, 3.f);
		}
		if (CrankAngle < -2.f * PI) CrankAngle += 2.f * PI;
		if (CrankAngle > 2.f * PI) CrankAngle -= 2.f * PI;
		CrankPivot->SetRelativeRotation(FRotator(FMath::RadiansToDegrees(CrankAngle), 0.f, 0.f));
		for (int32 s = 0; s < 2; ++s)
		{
			const float A = CrankAngle + (s == 0 ? PI : 0.f);
			if (PedalComps[s]) PedalComps[s]->SetRelativeLocation(RigBB + FVector(FMath::Cos(A), 0.f, FMath::Sin(A)) * RigCrankL + FVector(0.f, (s == 0 ? -7.2f : 7.2f), 0.f));
		}
	}
	if (RiderSegs.Num() != static_cast<int32>(EMRRiderSeg::Count)) return;

	// ---- Crash: the rider is thrown off and tumbles (world-space ragdoll) ----
	static bool bCrashTestDone = false;
	const bool bForceCrash = bCrashTest && !bCrashTestDone && GetWorld()->GetTimeSeconds() > CrashTestAt;
	if ((bCrashedNow || bForceCrash) && !bRagdoll && BodyPivot)
	{
		const FTransform BodyXf = BodyPivot->GetComponentTransform();
		FMRRiderPose W;
		for (int32 i = 0; i < static_cast<int32>(EMRRiderSeg::Count); ++i) W.Seg[i] = LastRiderPose.Seg[i] * BodyXf;
		for (int32 i = 0; i < static_cast<int32>(EMRRiderJoint::Count); ++i) W.Joint[i] = BodyXf.TransformPosition(LastRiderPose.Joint[i]);
		W.Eye = BodyXf.TransformPosition(LastRiderPose.Eye);
		FVector Vel = bForceCrash ? GetActorForwardVector() * 830.f : RiderVel;
		const FVector Fwd = Vel.GetSafeNormal2D();
		const FVector Lat = FVector::CrossProduct(FVector::UpVector, Fwd.IsNearlyZero() ? GetActorForwardVector() : Fwd);
		const float Sp = Vel.Size2D() / 100.f;
		// Pitched over the bars: a little lift and a forward tumble that grows with speed.
		Vel.Z = FMath::Max(static_cast<float>(Vel.Z), 0.f) + FMath::Min(250.f, 25.f * Sp);
		const FVector Spin = Lat * FMath::Min(6.f, 0.35f * Sp) + Fwd * (FMath::FRand() - 0.5f) * 1.5f;
		Ragdoll.Init(W, RiderDims, Vel, Spin);
		bRagdoll = true;
		bCrashTestDone = bCrashTestDone || bForceCrash;
	}
	else if (bRagdoll && !bCrashedNow && !bCrashTest)
	{
		bRagdoll = false; // respawned: back on the bike
		FootDownBlend = StandBlend = BobPos = BobVel = 0.f;
	}
	if (bRagdoll && bCrashTest && Ragdoll.Time > 3.f)
	{
		// Test only: back on the bike, and crash again shortly (screenshots catch every phase).
		bRagdoll = false;
		bCrashTestDone = false;
		CrashTestAt = GetWorld()->GetTimeSeconds() + 0.6f;
	}
	if (bRagdoll)
	{
		UpdateRagdoll(Dt);
		return;
	}

	// ---- Riding position for this vehicle ----
	FMRRiderTargets T;
	const float L = Params.Wheelbase * 100.f;
	const float Hgt = RiderDims.Height;
	const FTransform SteerXf = SteerPivot ? SteerPivot->GetRelativeTransform() : FTransform(FVector(L, 0.f, 0.f));
	const EVehicleCategory Cat = Params.Category;
	const bool bMoto = Cat == EVehicleCategory::Motorcycle || Cat == EVehicleCategory::DirtBike;
	const bool bOffRoadBicycle = Cat == EVehicleCategory::MountainBike || Cat == EVehicleCategory::DownhillBike || Cat == EVehicleCategory::BMX;

	// Stopped: put a foot down (left on bikes, the back foot on a scooter) and lean the bike onto it.
	const bool bStopped = !bAir && FMath::Abs(Spd) < 0.6f && !bCrankTest && !bStandTest;
	FootDownBlend = FMath::FInterpConstantTo(FootDownBlend, bStopped ? 1.f : 0.f, Dt, bStopped ? 2.6f : 4.f);

	// Standing: off the seat on rough ground, over jumps and in the air ("attack position").
	bool bWantStand = bStandTest;
	if (!bRigStanding && !bStopped)
	{
		if (Cat == EVehicleCategory::DirtBike) bWantStand |= Spd > 2.5f && (bOffRoad || bAir);
		else if (bOffRoadBicycle) bWantStand |= !bPedal && Spd > 3.f && (bOffRoad || bAir || Cat == EVehicleCategory::BMX);
		else bWantStand |= bAir && Spd > 3.f;
	}
	StandBlend = FMath::FInterpTo(StandBlend, bWantStand ? 1.f : 0.f, Dt, 5.f);
	const float Sb = FMath::SmoothStep(0.f, 1.f, StandBlend);

	FVector Hip = RigSeat, Head = RigHead;
	if (bRigStanding)
	{
		Hip = FVector(RigSeat.X, 0.f, RigFoot.Z + 0.5f * Hgt);
		Head = FVector(RigHead.X, 0.f, RigFoot.Z + 0.9f * Hgt);
		Hip.Z += FMath::Clamp(BobPos, -12.f, 4.f); // scooter: knees soak up the bumps
	}
	else if (Sb > 0.f)
	{
		// Hips back and up off the seat with bent knees, chest low over the bars.
		const float FootZ = CrankPivot ? RigBB.Z : RigFoot.Z;
		const FVector StandHip(RigSeat.X - (CrankPivot ? 0.03f : 0.02f) * Hgt, 0.f, FootZ + (CrankPivot ? 0.43f : 0.42f) * Hgt + FMath::Clamp(BobPos, -14.f, 4.f));
		const FVector StandHead = StandHip + FVector(0.62f, 0.f, 0.78f) * (0.4f * Hgt);
		Hip = FMath::Lerp(Hip, StandHip, Sb);
		Head = FMath::Lerp(Head, StandHead, Sb);
	}
	if (!bRigStanding) Hip.Z += (1.f - Sb) * FMath::Clamp(BobPos * 0.12f, -1.5f, 0.8f); // seat squish
	T.KneeOut = bRigStanding ? 0.1f : bMoto ? 0.3f : 0.15f;
	T.HeadYaw = DisplaySteer * 0.5f;

	// ---- Weight shifts ----
	const float P = DisplayPitch, Ln = DisplayLean;
	// Accelerating: lean forward over the bars. Braking: slide back on the seat and brace the arms.
	T.TorsoBias += FMath::Clamp(LongAccelSmooth * 0.012f, 0.f, 0.1f);
	Hip.X += (1.f - Sb) * FMath::Clamp(LongAccelSmooth * 0.6f, -5.f, 0.f);
	// Wheelie: stay over the front relative to the bike. Stoppie: weight goes back.
	if (P > 0.f) T.TorsoBias += FMath::Min(P, 0.9f) * 0.5f;
	else { T.TorsoBias += FMath::Max(P, -0.5f) * 0.6f; Hip.X += FMath::Max(P * 30.f, -10.f); }
	if (bLocal)
	{
		T.TorsoBias -= Input.Balance * 0.12f;
		Hip.X -= Input.Balance * 4.f;
	}
	// Landings and bumps fold the body forward a little.
	T.TorsoBias += FMath::Clamp(-BobPos * 0.008f, -0.03f, 0.12f);
	// Cornering: hang off on road motorbikes, stay upright and push the bike down off-road.
	const int32 Inside = Ln >= 0.f ? 1 : 0;
	switch (Cat)
	{
	case EVehicleCategory::Motorcycle:
		T.BodyRoll = Ln * 0.3f;
		Hip.Y += FMath::Clamp(Ln * 16.f, -7.f, 7.f);
		T.KneeOutExtra[Inside] += FMath::Min(FMath::Abs(Ln) * 1.4f, 0.9f);
		break;
	case EVehicleCategory::DirtBike: case EVehicleCategory::MountainBike: case EVehicleCategory::DownhillBike: case EVehicleCategory::BMX:
		T.BodyRoll = -Ln * (0.15f + 0.2f * Sb);
		break;
	case EVehicleCategory::EScooter:
		T.BodyRoll = -Ln * 0.15f;
		break;
	default:
		T.BodyRoll = Ln * 0.05f;
		break;
	}
	// Eyes stay closer to level than the bike: head counter-rolls in corners and dips in wheelies.
	T.HeadRoll = -(Ln + T.BodyRoll) * 0.55f;
	T.HeadPitch = -P * 0.6f;

	// ---- Hands and feet ----
	for (int32 s = 0; s < 2; ++s)
	{
		const float Side = s == 0 ? -1.f : 1.f;
		T.Grip[s] = SteerXf.TransformPosition(RigBars + FVector(0.f, Side * BarHalfWidth, 0.f) - FVector(L, 0.f, 0.f));
		if (CrankPivot)
		{
			const float A = CrankAngle + (s == 0 ? PI : 0.f);
			T.FootBall[s] = RigBB + FVector(FMath::Cos(A), 0.f, FMath::Sin(A)) * RigCrankL + FVector(0.f, Side * RigPedalY, 1.f);
			T.FootPitch[s] = 0.12f;
		}
		else if (bRigStanding)
		{
			T.FootBall[s] = RigFoot + FVector(s == 0 ? 10.f : -12.f, Side * RigFootHalf, 0.f); // one foot ahead of the other
			T.FootPitch[s] = 0.f;
		}
		else
		{
			T.FootBall[s] = RigFoot + FVector(2.f, Side * RigFootHalf, 1.5f); // balls of the feet on the pegs
			T.FootPitch[s] = 0.06f;
		}
	}

	// Foot down: swing one foot off the pedal / peg / deck onto the ground beside the bike.
	FootDownLean = 0.f;
	if (FootDownBlend > 0.f && BodyPivot)
	{
		const int32 Ds = bRigStanding ? 1 : 0;
		const float Side = Ds == 0 ? -1.f : 1.f;
		const float Fd = FMath::SmoothStep(0.f, 1.f, FootDownBlend);
		FootDownLean = Side * (bRigStanding ? 0.03f : 0.08f) * Fd;
		const FTransform BodyRel = BodyPivot->GetRelativeTransform();
		const FVector Guess = bRigStanding ? FVector(RigFoot.X - 22.f, Side * (RigFootHalf + 12.f), 0.f)
			: FVector(Hip.X + 0.04f * Hgt, Side * (RiderDims.HipHalf + 0.11f * Hgt), 0.f);
		FVector Gp = BodyRel.TransformPosition(Guess);
		Gp.Z = 1.f; // sole on the ground (SpinPivot space: z = 0 is the ground)
		const FVector Ground = BodyRel.InverseTransformPosition(Gp);
		// Too far to reach on tiptoe: slide off the saddle towards that foot.
		if (!bRigStanding)
		{
			const FVector Hj = Hip + FVector(0.f, Side * RiderDims.HipHalf, 0.f);
			const FVector Ankle = Ground + FVector(-RiderDims.FootBall * 0.7f, 0.f, RiderDims.AnkleH + 4.f);
			const float Need = FVector::Dist(Hj, Ankle) - (RiderDims.Thigh + RiderDims.Shin) * 0.97f;
			if (Need > 0.f)
			{
				const float Shift = FMath::Min(Need, 12.f) * Fd;
				Hip += FVector(0.f, Side * 0.7f * Shift, -0.6f * Shift);
			}
			T.KneeOutExtra[Ds] += 0.25f * Fd;
		}
		const FVector Lift(0.f, 0.f, FMath::Sin(PI * Fd) * 9.f);
		T.FootBall[Ds] = FMath::Lerp(T.FootBall[Ds], Ground, Fd) + Lift;
		T.FootPitch[Ds] = FMath::Lerp(T.FootPitch[Ds], 0.f, Fd);
	}
	T.Hip = Hip;
	T.HeadHint = Head;

	FMRRiderPose Pose;
	MRSolveRiderPose(RiderDims, T, Pose);
	LastRiderPose = Pose;
	for (int32 i = 0; i < static_cast<int32>(EMRRiderSeg::Count); ++i)
		if (RiderSegs[i]) RiderSegs[i]->SetRelativeTransform(Pose.Seg[i]);
	FirstPersonCamera->SetRelativeLocation(Pose.Eye + FVector(1.5f, 0.f, 0.f));
	// In first person the camera sits in the head: hide the head (and helmet) for the local player.
	if (USceneComponent* HeadSeg = RiderSegs[static_cast<int32>(EMRRiderSeg::Head)])
	{
		const bool bShow = !(bLocal && CameraMode == EMRCameraMode::FirstPerson);
		if (HeadSeg->IsVisible() != bShow) HeadSeg->SetVisibility(bShow, true);
	}
}

void AMRRideVehicle::UpdateRagdoll(float DeltaSeconds)
{
	// Ground under each joint (the rider's own bike doesn't count).
	float GroundZ[FMRRiderRagdoll::NJ];
	FCollisionQueryParams Q(SCENE_QUERY_STAT(MRRagdollTrace), false, this);
	for (int32 i = 0; i < FMRRiderRagdoll::NJ; ++i)
	{
		const FVector Pt = Ragdoll.P[i];
		FHitResult Hit;
		GroundZ[i] = GetWorld()->LineTraceSingleByChannel(Hit, Pt + FVector(0.f, 0.f, 120.f), Pt - FVector(0.f, 0.f, 400.f), ECC_Visibility, Q)
			? static_cast<float>(Hit.ImpactPoint.Z) : -1e7f;
	}
	Ragdoll.Step(DeltaSeconds, GroundZ);
	FMRRiderPose W;
	Ragdoll.GetPose(W);
	for (int32 i = 0; i < static_cast<int32>(EMRRiderSeg::Count); ++i)
		if (RiderSegs[i]) RiderSegs[i]->SetWorldTransform(W.Seg[i]);
	FirstPersonCamera->SetWorldLocation(W.Eye);
	if (USceneComponent* HeadSeg = RiderSegs[static_cast<int32>(EMRRiderSeg::Head)])
		if (!HeadSeg->IsVisible()) HeadSeg->SetVisibility(true, true); // see yourself fall
}

void AMRRideVehicle::UpdateMaterials()
{
	for (UMaterialInstanceDynamic* M : PaintMaterials) if (M) M->SetVectorParameterValue(TEXT("Color"), PaintColor);
	for (UMaterialInstanceDynamic* M : JacketMaterials) if (M) M->SetVectorParameterValue(TEXT("Color"), JacketColor);
	for (UMaterialInstanceDynamic* M : HelmetMaterials) if (M) M->SetVectorParameterValue(TEXT("Color"), HelmetColor);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

bool AMRRideVehicle::IsRemoteProxy() const
{
	return !IsLocallyControlled();
}

FString AMRRideVehicle::GetRiderDisplayName() const
{
	if (!DisplayName.IsEmpty()) return DisplayName;
	const APlayerState* PS = GetPlayerState();
	return PS ? PS->GetPlayerName() : FString();
}

float AMRRideVehicle::GetSpeedMps() const
{
	return IsRemoteProxy() ? LatestRemote.Speed / 100.f : Sim.Speed;
}

void AMRRideVehicle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bConfigApplied) return;

	if (const AMRWorldEnvironment* Env = AMRWorldEnvironment::Get(this))
	{
		if (Headlight && Headlight->IsVisible() != Env->IsNight()) Headlight->SetVisibility(Env->IsNight());
		Wetness = Env->GetRoadWetness();
	}

	if (IsLocallyControlled())
	{
		SimulateLocal(DeltaSeconds);
		KeepAboveGround();

		SendTimer += DeltaSeconds;
		const float SendInterval = 1.f / FMath::Max(5.f, GetDefault<UMRNetworkSettings>()->ClientSendRate);
		if (SendTimer >= SendInterval)
		{
			SendTimer = FMath::Fmod(SendTimer, SendInterval);
			const FMRNetRideState S = PackState();
			if (HasAuthority())
			{
				ReplicatedState = S; // listen-server host: replicate directly
			}
			else
			{
				ServerSubmitState(S);
			}
		}
	}
	else
	{
		InterpolateRemote(DeltaSeconds);
	}

	UpdateVisualPose(DeltaSeconds);
	if (IsLocallyControlled())
	{
		UpdateCamera(DeltaSeconds);
		UpdateForceFeedback(DeltaSeconds);
	}
	UpdateAudio();

	for (int32 i = TrickPopups.Num() - 1; i >= 0; --i)
	{
		TrickPopups[i].TimeShown += DeltaSeconds;
		if (TrickPopups[i].TimeShown > 3.f) TrickPopups.RemoveAt(i);
	}
}

// ---------------------------------------------------------------------------
// Local simulation
// ---------------------------------------------------------------------------

void AMRRideVehicle::SimulateLocal(float DeltaSeconds)
{
	SimAccumulator += FMath::Min(DeltaSeconds, 0.1f);
	int32 Steps = 0;
	while (SimAccumulator >= SimDt && Steps < MaxSubsteps)
	{
		SimStep(SimDt);
		SimAccumulator -= SimDt;
		++Steps;
	}
	if (Steps == MaxSubsteps) SimAccumulator = 0.f;

	DisplayPitch = Sim.Pitch;
	DisplayLean = Sim.Lean;
	DisplaySteer = Sim.Steer;
	DisplayAirYaw = Sim.AirYaw;
	DisplayCompF = Sim.CompressionFront;
	DisplayCompR = Sim.CompressionRear;

	ProcessTrickEvents();
	UpdateSafeTransform(DeltaSeconds);

	if (Sim.bCrashed)
	{
		CrashRespawnTimer += DeltaSeconds;
		if (CrashRespawnTimer > CrashRespawnDelay)
		{
			RequestRespawn();
		}
	}
	else
	{
		CrashRespawnTimer = 0.f;
	}

	// Fell out of the world (e.g. off the edge of the streamed area)
	if (GetActorLocation().Z < -20000.f)
	{
		RequestRespawn();
	}
}

FVector AMRRideVehicle::ContactFrameOriginWorld() const
{
	const FTransform& T = GetActorTransform();
	return T.TransformPosition(FVector(-ContactToCenterFwdCm, 0.f, -ContactToCenterUpCm));
}

void AMRRideVehicle::ComputeContactPoints(FVector& OutRear, FVector& OutFront) const
{
	// Contact frame (cm): x forward from rear contact, z up. Wheelie pivots on the
	// rear contact, stoppie on the front contact - same convention as Tests/SimTests.
	const float L = Params.Wheelbase * 100.f;
	const float P = Sim.Pitch;
	if (P >= 0.f)
	{
		OutRear = FVector::ZeroVector;
		OutFront = FVector(L * FMath::Cos(P), 0.f, L * FMath::Sin(P));
	}
	else
	{
		OutFront = FVector(L, 0.f, 0.f);
		OutRear = FVector(L - L * FMath::Cos(P), 0.f, -L * FMath::Sin(P));
	}
}

MRSim::FWheelContact AMRRideVehicle::TraceWheel(const FVector& MountWorld, float RestLengthM, FVector& OutHitPoint, FVector& OutHitNormal) const
{
	MRSim::FWheelContact C;
	const FVector Up = GetActorUpVector();
	const FVector Start = MountWorld + Up * (TraceStartPadM * 100.f);
	const FVector End = MountWorld - Up * ((RestLengthM + 0.05f) * 100.f);

	FCollisionQueryParams Q(SCENE_QUERY_STAT(MRWheelTrace), false, this);
	Q.bReturnPhysicalMaterial = true;
	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Q))
	{
		C.bHit = true;
		C.Distance = FMath::Max(0.f, Hit.Distance / 100.f - TraceStartPadM);
		C.NormalZ = FMath::Max(0.f, FVector::DotProduct(Hit.ImpactNormal, Up));
		C.Surface = SurfaceFromPhysical(UPhysicalMaterial::DetermineSurfaceType(Hit.PhysMaterial.Get()));
		OutHitPoint = Hit.ImpactPoint;
		OutHitNormal = Hit.ImpactNormal;
	}
	return C;
}

void AMRRideVehicle::SimStep(float Dt)
{
	// --- Build inputs ---
	MRSim::FRiderInput In = Input;
	In.GearRequest = FMath::Clamp(PendingGear, -1, 1);
	PendingGear -= In.GearRequest;

	// --- Sample wheel contacts at the hub mount points ---
	FVector CR, CF;
	ComputeContactPoints(CR, CF);
	const float P = Sim.Pitch;
	const FVector BodyUp(-FMath::Sin(P), 0.f, FMath::Cos(P));
	const FVector MountR = CR + BodyUp * (Params.Rear.RestLength * 100.f);
	const FVector MountF = CF + BodyUp * (Params.Front.RestLength * 100.f);
	const FTransform& T = GetActorTransform();
	const FVector Origin(-ContactToCenterFwdCm, 0.f, -ContactToCenterUpCm);

	FVector HitR = FVector::ZeroVector, HitF = FVector::ZeroVector, NR = FVector::UpVector, NF = FVector::UpVector;
	const MRSim::FWheelContact Rear = TraceWheel(T.TransformPosition(Origin + MountR), Params.Rear.RestLength, HitR, NR);
	const MRSim::FWheelContact Front = TraceWheel(T.TransformPosition(Origin + MountF), Params.Front.RestLength, HitF, NF);
	{
		auto Rough = [](const MRSim::FWheelContact& C) { using MRSim::ESurface; return C.bHit && (C.Surface == ESurface::Gravel || C.Surface == ESurface::Grass || C.Surface == ESurface::Mud || C.Surface == ESurface::Dirt || C.Surface == ESurface::ForestTrail); };
		bOnRoughGround = Rough(Rear) || Rough(Front);
	}

	MRSim::FEnvironment Env;
	Env.Wetness = Wetness;
	Env.SlopePitch = Sim.GroundPitch;
	const FVector Fwd = GetActorForwardVector();
	const FVector Up = GetActorUpVector();
	auto NormalSlope = [&](const FVector& N) { return FMath::Atan2(-FVector::DotProduct(N, Fwd), FVector::DotProduct(N, Up)); };
	if (Rear.bHit && Front.bHit)
	{
		const FVector D = HitF - HitR;
		Env.SlopePitch = Sim.GroundPitch + FMath::Atan2(FVector::DotProduct(D, Up), FMath::Max(10.f, FVector::DotProduct(D, Fwd)));
	}
	else if (Rear.bHit) Env.SlopePitch = Sim.GroundPitch + NormalSlope(NR);
	else if (Front.bHit) Env.SlopePitch = Sim.GroundPitch + NormalSlope(NF);

	// --- Step the simulation ---
	const MRSim::FStepResult R = MRSim::FRideSimulator::Step(Params, Assist, In, Front, Rear, Env, Sim, Dt);
	Tricks.Update(Params, Sim, In, R, Dt);
	if (R.bLanded)
	{
		LastLandingImpact = FMath::Max(LastLandingImpact, R.LandingImpactSpeed);
		Audio->TriggerThump(R.LandingImpactSpeed / 6.f);
	}

	// --- Move the actor ---
	FVector Delta;
	if (!Sim.bAirborne && !R.bLanded)
	{
		Delta = (Fwd * R.Forward + GetActorRightVector() * R.Right + Up * R.Up) * 100.f;
	}
	else
	{
		const FVector FlatFwd = FVector(Fwd.X, Fwd.Y, 0.f).GetSafeNormal();
		const FVector FlatRight = FVector(-FlatFwd.Y, FlatFwd.X, 0.f);
		Delta = (FlatFwd * R.Forward + FlatRight * R.Right + FVector::UpVector * R.Up) * 100.f;
	}

	FHitResult Hit;
	AddActorWorldOffset(Delta, true, &Hit);
	if (Hit.bBlockingHit)
	{
		HandleWallHit(Hit);
		const FVector Remaining = FVector::VectorPlaneProject(Delta * (1.f - Hit.Time), Hit.ImpactNormal);
		AddActorWorldOffset(Remaining, true);
	}

	const float NewYaw = GetActorRotation().Yaw + FMath::RadiansToDegrees(R.Yaw);
	SetActorRotation(FRotator(FMath::RadiansToDegrees(Sim.GroundPitch), NewYaw, 0.f));
}

void AMRRideVehicle::HandleWallHit(const FHitResult& Hit)
{
	const FVector N = Hit.ImpactNormal;
	if (N.Z > 0.65f)
	{
		// The box bottomed out on the ground (very steep transition): treat like a bump stop.
		Sim.VerticalSpeed = FMath::Max(Sim.VerticalSpeed, 0.f);
		return;
	}
	const FVector Fwd = GetActorForwardVector();
	const float Into = FMath::Max(0.f, -FVector::DotProduct(Fwd, N)); // 1 = head-on
	const float ImpactSpeed = FMath::Abs(Sim.Speed) * Into;
	if (ImpactSpeed > 6.5f)
	{
		Sim.bCrashed = true;
		Sim.Speed *= 0.2f;
		if (APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			PC->PlayDynamicForceFeedback(1.f, 0.4f, true, true, true, true);
		}
		Audio->TriggerThump(1.f);
	}
	else
	{
		Sim.Speed *= (1.f - Into);
		Sim.LateralSpeed *= 0.5f;
		if (ImpactSpeed > 1.f) Audio->TriggerThump(ImpactSpeed / 6.5f);
	}
}

void AMRRideVehicle::ProcessTrickEvents()
{
	for (const MRSim::FTrickEvent& E : Tricks.ConsumeEvents())
	{
		FMRTrickPopup Pop;
		Pop.Label = UTF8_TO_TCHAR(E.Label.c_str());
		Pop.Points = E.Points;
		TrickPopups.Add(Pop);
		if (E.Trick == MRSim::ETrick::Bail) continue;
		if (HasAuthority())
		{
			if (AMRGameMode* GM = GetWorld()->GetAuthGameMode<AMRGameMode>())
			{
				GM->ScoreTrick(GetPlayerState<AMRPlayerState>(), static_cast<uint8>(E.Trick), E.Points, E.Distance, E.Duration);
			}
		}
		else
		{
			ServerReportTrick(static_cast<uint8>(E.Trick), E.Points, E.Distance, E.Duration);
		}
	}
}

void AMRRideVehicle::UpdateSafeTransform(float DeltaSeconds)
{
	SafeTimer += DeltaSeconds;
	const bool bSafe = !Sim.bCrashed && !Sim.bAirborne && Sim.bFrontGrounded && Sim.bRearGrounded && FMath::Abs(Sim.GroundPitch) < 0.3f;
	if (bSafe && SafeTimer > 1.f)
	{
		SafeTimer = 0.f;
		SafeLocation = GetActorLocation() + FVector(0, 0, 30.f);
		SafeYaw = GetActorRotation().Yaw;
	}
}

void AMRRideVehicle::KeepAboveGround()
{
	// Safety net against falling through the world. The streamed ground is watertight and the
	// rider's own cell is always built synchronously, but if anything ever leaves the rider below
	// the real terrain surface (a missed collision, a bad teleport), put them back on top.
	if (!GeoWorld.IsValid())
	{
		GeoCheckTimer -= GetWorld()->GetDeltaSeconds();
		if (GeoCheckTimer > 0.f) return;
		GeoCheckTimer = 2.f;
		GeoWorld = AMRGeoWorld::Find(this);
		if (!GeoWorld.IsValid()) return; // test district: flat ground at Z=0, nothing to do
	}
	const FVector Loc = GetActorLocation();
	float GroundZ = 0.f;
	if (!AMRGeoWorld::GetGroundHeight(Loc.X, Loc.Y, GroundZ)) return;
	// Photoreal map: the scanned ground can sit a little off our terrain model, so allow more slack.
	const AMRGeoWorld* GeoW = AMRGeoWorld::Find(this);
	const float Slack = (GeoW && GeoW->IsPhotoMode()) ? 450.f : 150.f;
	if (Loc.Z + ContactToCenterUpCm < GroundZ - Slack) // wheels well under the surface
	{
		float SafeGround = 0.f;
		if (AMRGeoWorld::GetGroundHeight(SafeLocation.X, SafeLocation.Y, SafeGround) && SafeLocation.Z < SafeGround + ContactToCenterUpCm)
		{
			SafeLocation.Z = SafeGround + ContactToCenterUpCm + 50.f;
		}
		UE_LOG(LogManchesterRide, Warning, TEXT("Rider was %.0f cm below the ground at (%.0f, %.0f); respawning"), GroundZ - Loc.Z, Loc.X, Loc.Y);
		RequestRespawn();
	}
}

void AMRRideVehicle::RequestRespawn()
{
	CrashRespawnTimer = 0.f;
	if (HasAuthority())
	{
		DoRespawn(SafeLocation, SafeYaw);
	}
	else
	{
		ServerRequestRespawn();
		DoRespawn(SafeLocation, SafeYaw); // predicted; the server confirms with ClientTeleport
	}
}

void AMRRideVehicle::DoRespawn(const FVector& Location, float YawDeg)
{
	SetActorLocationAndRotation(Location, FRotator(0.f, YawDeg, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	MRSim::FRideState Fresh;
	MRSim::FRideSimulator::InitState(Params, Fresh);
	Fresh.BatteryWh = Sim.BatteryWh > 0.f ? Sim.BatteryWh : Fresh.BatteryWh; // keep charge across respawns
	Fresh.FuelLitres = Sim.FuelLitres > 0.f ? Sim.FuelLitres : Fresh.FuelLitres;
	Sim = Fresh;
	SimAccumulator = 0.f;
	bServerAcceptNextTeleport = true;
}

void AMRRideVehicle::TeleportRider(const FVector2D& Where, float YawDeg)
{
	float GroundZ = 0.f;
	AMRGeoWorld::GetGroundHeight(Where.X, Where.Y, GroundZ);
	const FVector Loc(Where.X, Where.Y, GroundZ + ContactToCenterUpCm + 60.f); // just above the road; settles in a moment
	SafeLocation = Loc;
	SafeYaw = YawDeg;
	CrashRespawnTimer = 0.f;
	if (!HasAuthority()) ServerTeleportTo(Loc, YawDeg);
	DoRespawn(Loc, YawDeg);
}

void AMRRideVehicle::ServerTeleportTo_Implementation(FVector Location, float YawDegrees)
{
	bServerAcceptNextTeleport = true;
	SafeLocation = Location;
	SafeYaw = YawDegrees;
	SetActorLocationAndRotation(Location, FRotator(0.f, YawDegrees, 0.f));
	Snapshots.Reset();
}

void AMRRideVehicle::ServerRequestRespawn_Implementation()
{
	bServerAcceptNextTeleport = true;
	ClientTeleport(SafeLocation, SafeYaw, true);
	SetActorLocationAndRotation(SafeLocation, FRotator(0.f, SafeYaw, 0.f));
	Snapshots.Reset();
}

void AMRRideVehicle::ClientTeleport_Implementation(FVector Location, float YawDegrees, bool bResetMotion)
{
	if (bResetMotion)
	{
		DoRespawn(Location, YawDegrees);
	}
	else
	{
		SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AMRRideVehicle::ServerReportTrick_Implementation(uint8 TrickType, int32 Points, float Distance, float Duration)
{
	if (AMRGameMode* GM = GetWorld()->GetAuthGameMode<AMRGameMode>())
	{
		GM->ScoreTrick(GetPlayerState<AMRPlayerState>(), TrickType, Points, Distance, Duration);
	}
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------

FMRNetRideState AMRRideVehicle::PackState() const
{
	FMRNetRideState S;
	S.Location = GetActorLocation();
	S.SetYawDegrees(GetActorRotation().Yaw);
	S.GroundPitch = FMRNetRideState::PackAngle(Sim.GroundPitch, 5000.f);
	S.Pitch = FMRNetRideState::PackAngle(Sim.Pitch, 5000.f);
	S.Lean = FMRNetRideState::PackAngle(Sim.Lean, 5000.f);
	S.AirYaw = FMRNetRideState::PackAngle(FMath::UnwindRadians(Sim.AirYaw), 2000.f);
	S.Speed = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Sim.Speed * 100.f), -32767, 32767));
	S.VerticalSpeed = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Sim.VerticalSpeed * 100.f), -32767, 32767));
	S.Steer = static_cast<int8>(FMath::Clamp(FMath::RoundToInt(Sim.Steer * 100.f), -127, 127));
	auto PackComp = [](float C, float Travel) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(C / (Travel * 1.5f) * 255.f), 0, 255)); };
	S.CompFront = PackComp(Sim.CompressionFront, Params.Front.Travel);
	S.CompRear = PackComp(Sim.CompressionRear, Params.Rear.Travel);
	S.RpmDiv64 = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Sim.EngineRpm / 64.f), 0, 255));
	S.Flags = static_cast<uint8>((Sim.bAirborne ? EMRNetFlags::Airborne : 0) | (Sim.bCrashed ? EMRNetFlags::Crashed : 0)
		| (Sim.bFrontGrounded ? EMRNetFlags::FrontGrounded : 0) | (Sim.bRearGrounded ? EMRNetFlags::RearGrounded : 0)
		| (Sim.bRearSliding ? EMRNetFlags::Sliding : 0) | (bOnRoughGround ? EMRNetFlags::OffRoad : 0));
	S.Timestamp = GetWorld()->GetTimeSeconds();
	return S;
}

bool AMRRideVehicle::ValidateClientState(const FMRNetRideState& S)
{
	const FVector Loc = S.Location;
	if (bServerAcceptNextTeleport || ServerCheck.LastTime < 0.f)
	{
		bServerAcceptNextTeleport = false;
		ServerCheck.LastLocation = Loc;
		ServerCheck.LastTime = S.Timestamp;
		return true;
	}
	const float Dt = S.Timestamp - ServerCheck.LastTime;
	if (Dt <= 0.f) return false; // stale / out of order (unreliable RPCs can reorder)

	const float MaxSpeedCm = (Params.TopSpeed * 1.25f + 10.f) * 100.f;
	const float Horizontal = FVector::Dist2D(Loc, ServerCheck.LastLocation);
	const float Vertical = FMath::Abs(Loc.Z - ServerCheck.LastLocation.Z);
	const float Allowed = MaxSpeedCm * FMath::Min(Dt, 1.f) * 1.5f + 300.f;
	if (Horizontal > Allowed || Vertical > 4000.f * FMath::Min(Dt, 1.f) + 300.f || FMath::Abs(S.Speed) > MaxSpeedCm)
	{
		ServerCheck.Rejections++;
		UE_LOG(LogManchesterRide, Warning, TEXT("Rejected ride state from %s (moved %.0f cm in %.3f s, allowed %.0f)"),
			*GetNameSafe(GetPlayerState()), Horizontal, Dt, Allowed);
		ClientTeleport(ServerCheck.LastLocation, S.GetYawDegrees(), false);
		return false;
	}
	ServerCheck.LastLocation = Loc;
	ServerCheck.LastTime = S.Timestamp;
	return true;
}

void AMRRideVehicle::ServerSubmitState_Implementation(const FMRNetRideState& State)
{
	if (!ValidateClientState(State)) return;
	ReplicatedState = State;
	PushSnapshot(State);
	if ((State.Flags & (EMRNetFlags::FrontGrounded | EMRNetFlags::RearGrounded)) && !(State.Flags & EMRNetFlags::Crashed)
		&& FMath::Abs(FMRNetRideState::UnpackAngle(State.GroundPitch, 5000.f)) < 0.3f)
	{
		SafeLocation = FVector(State.Location) + FVector(0, 0, 30.f);
		SafeYaw = State.GetYawDegrees();
	}
}

void AMRRideVehicle::OnRep_ReplicatedState()
{
	PushSnapshot(ReplicatedState);
}

void AMRRideVehicle::PushSnapshot(const FMRNetRideState& S)
{
	const float Now = GetWorld()->GetTimeSeconds();
	// Clock offset estimate: the minimum observed (receive - send) is the least-delayed packet.
	const float Offset = Now - S.Timestamp;
	if (!bHaveClockOffset || Offset < ClockOffset)
	{
		ClockOffset = Offset;
		bHaveClockOffset = true;
	}
	else
	{
		ClockOffset += (Offset - ClockOffset) * 0.002f; // slow drift correction
	}
	if (Snapshots.Num() > 0 && S.Timestamp <= Snapshots.Last().State.Timestamp) return; // reordered
	FSnapshot Snap;
	Snap.State = S;
	Snap.RecvTime = Now;
	Snapshots.Add(Snap);
	if (Snapshots.Num() > 32) Snapshots.RemoveAt(0, Snapshots.Num() - 32);
	LatestRemote = S;
}

void AMRRideVehicle::ApplyNetState(const FMRNetRideState& S, bool bSnapTransform)
{
	if (bSnapTransform)
	{
		SetActorLocationAndRotation(S.Location, FRotator(FMath::RadiansToDegrees(FMRNetRideState::UnpackAngle(S.GroundPitch, 5000.f)), S.GetYawDegrees(), 0.f));
	}
	DisplayPitch = FMRNetRideState::UnpackAngle(S.Pitch, 5000.f);
	DisplayLean = FMRNetRideState::UnpackAngle(S.Lean, 5000.f);
	DisplaySteer = S.Steer / 100.f;
	DisplayAirYaw = FMRNetRideState::UnpackAngle(S.AirYaw, 2000.f);
	DisplayCompF = S.CompFront / 255.f * Params.Front.Travel * 1.5f;
	DisplayCompR = S.CompRear / 255.f * Params.Rear.Travel * 1.5f;
}

void AMRRideVehicle::InterpolateRemote(float DeltaSeconds)
{
	if (Snapshots.Num() == 0) return;
	const float Delay = GetDefault<UMRNetworkSettings>()->InterpolationDelay;
	const float RenderTime = GetWorld()->GetTimeSeconds() - ClockOffset - Delay; // in sender's clock

	// Drop snapshots that are fully in the past (keep one before RenderTime)
	while (Snapshots.Num() > 2 && Snapshots[1].State.Timestamp <= RenderTime)
	{
		Snapshots.RemoveAt(0);
	}

	const FMRNetRideState& A = Snapshots[0].State;
	if (Snapshots.Num() >= 2 && RenderTime <= Snapshots[1].State.Timestamp && RenderTime >= A.Timestamp)
	{
		const FMRNetRideState& B = Snapshots[1].State;
		const float Span = FMath::Max(B.Timestamp - A.Timestamp, KINDA_SMALL_NUMBER);
		const float Alpha = FMath::Clamp((RenderTime - A.Timestamp) / Span, 0.f, 1.f);
		const FVector Loc = FMath::Lerp(FVector(A.Location), FVector(B.Location), Alpha);
		const FQuat QA = FRotator(FMath::RadiansToDegrees(FMRNetRideState::UnpackAngle(A.GroundPitch, 5000.f)), A.GetYawDegrees(), 0.f).Quaternion();
		const FQuat QB = FRotator(FMath::RadiansToDegrees(FMRNetRideState::UnpackAngle(B.GroundPitch, 5000.f)), B.GetYawDegrees(), 0.f).Quaternion();
		SetActorLocationAndRotation(Loc, FQuat::Slerp(QA, QB, Alpha));
		ApplyNetState(Alpha < 0.5f ? A : B, false);
		DisplayPitch = FMath::Lerp(FMRNetRideState::UnpackAngle(A.Pitch, 5000.f), FMRNetRideState::UnpackAngle(B.Pitch, 5000.f), Alpha);
		DisplayLean = FMath::Lerp(FMRNetRideState::UnpackAngle(A.Lean, 5000.f), FMRNetRideState::UnpackAngle(B.Lean, 5000.f), Alpha);
	}
	else
	{
		// Starved (packet loss or the first packet): extrapolate briefly along the last heading.
		const FMRNetRideState& L = Snapshots.Last().State;
		const float Ahead = FMath::Clamp(RenderTime - L.Timestamp, 0.f, 0.25f);
		const FRotator Rot(FMath::RadiansToDegrees(FMRNetRideState::UnpackAngle(L.GroundPitch, 5000.f)), L.GetYawDegrees(), 0.f);
		const FVector Vel = Rot.Vector() * L.Speed + FVector(0, 0, L.VerticalSpeed);
		const FVector Target = FVector(L.Location) + Vel * Ahead;
		const float Blend = FMath::Clamp(DeltaSeconds * 15.f, 0.f, 1.f);
		SetActorLocationAndRotation(FMath::Lerp(GetActorLocation(), Target, Blend), FQuat::Slerp(GetActorQuat(), Rot.Quaternion(), Blend));
		ApplyNetState(L, false);
	}
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

void AMRRideVehicle::UpdateVisualPose(float DeltaSeconds)
{
	const float L = Params.Wheelbase * 100.f;
	const float P = DisplayPitch;
	FVector CR;
	if (P >= 0.f) CR = FVector::ZeroVector;
	else CR = FVector(L - L * FMath::Cos(P), 0.f, -L * FMath::Sin(P));

	// SpinPivot sits on the ground under the box centre (contact frame x = L/2).
	SpinPivot->SetRelativeRotation(FRotator(0.f, FMath::RadiansToDegrees(DisplayAirYaw), 0.f));
	BodyPivot->SetRelativeLocation(CR - FVector(ContactToCenterFwdCm, 0.f, 0.f));
	BodyPivot->SetRelativeRotation(FRotator(FMath::RadiansToDegrees(P), 0.f, FMath::RadiansToDegrees(DisplayLean + FootDownLean))); // + leaning onto a foot when stopped

	if (SteerPivot) SteerPivot->SetRelativeRotation(FRotator(0.f, FMath::RadiansToDegrees(DisplaySteer), 0.f));

	const float Speed = GetSpeedMps();
	WheelSpin = FMath::Fmod(WheelSpin - FMath::RadiansToDegrees(Speed / FMath::Max(0.1f, Params.WheelRadiusRear)) * DeltaSeconds, 360.f);
	if (RearWheel)
	{
		RearWheel->SetRelativeLocation(FVector(0.f, 0.f, (Params.WheelRadiusRear + DisplayCompR) * 100.f));
		RearWheel->SetRelativeRotation(FRotator(WheelSpin, 0.f, 0.f));
	}
	if (FrontWheel)
	{
		FrontWheel->SetRelativeLocation(FVector(0.f, 0.f, (Params.WheelRadiusFront + DisplayCompF) * 100.f));
		FrontWheel->SetRelativeRotation(FRotator(WheelSpin, 0.f, 0.f));
	}
	if (FrontLowers) FrontLowers->SetRelativeLocation(FVector(0.f, 0.f, (Params.WheelRadiusFront + DisplayCompF) * 100.f));
	if (SwingPivotComp && !SwingRest.IsNearlyZero())
	{
		// Pitch the swingarm so its end follows the rear axle through the suspension travel.
		const FVector Now = SwingRest + FVector(0.f, 0.f, DisplayCompR * 100.f);
		const float Ang = FMath::Atan2(Now.Z, Now.X) - FMath::Atan2(SwingRest.Z, SwingRest.X);
		SwingPivotComp->SetRelativeRotation(FRotator(FMath::RadiansToDegrees(Ang), 0.f, 0.f));
	}
	UpdateRiderPose(DeltaSeconds);
}

void AMRRideVehicle::AddLookInput(FVector2D Delta)
{
	if (Delta.IsNearlyZero()) return;
	LookOffset.Yaw = FRotator::NormalizeAxis(LookOffset.Yaw + Delta.X * CameraSensitivity);
	LookOffset.Pitch = FMath::Clamp(LookOffset.Pitch + Delta.Y * CameraSensitivity, -60.f, 40.f);
	LookIdleTime = 0.f;
}

void AMRRideVehicle::CycleCameraMode()
{
	SetCameraMode(CameraMode == EMRCameraMode::ThirdPerson ? EMRCameraMode::FirstPerson : EMRCameraMode::ThirdPerson);
}

void AMRRideVehicle::SetCameraMode(EMRCameraMode Mode)
{
	CameraMode = Mode;
	const bool bFirst = Mode == EMRCameraMode::FirstPerson;
	FirstPersonCamera->SetActive(bFirst);
	ThirdPersonCamera->SetActive(!bFirst);
	LookOffset = FRotator::ZeroRotator;
}

void AMRRideVehicle::UpdateCamera(float DeltaSeconds)
{
	const float Speed = FMath::Abs(GetSpeedMps());
	LookIdleTime += DeltaSeconds;
	if (LookIdleTime > 2.f && CameraMode != EMRCameraMode::GaragePreview)
	{
		LookOffset = FMath::RInterpTo(LookOffset, FRotator::ZeroRotator, DeltaSeconds, 2.5f); // recentre behind the rider
	}

	// -Showcase=<yaw> (automated screenshots): a fixed garage-style view of the local vehicle.
	static const bool bShowcase = FParse::Value(FCommandLine::Get(), TEXT("Showcase="), ShowcaseYaw);
	if (bShowcase && IsLocallyControlled())
	{
		CameraBoom->SetRelativeLocation(FVector(0.f, 0.f, -ContactToCenterUpCm + 75.f));
		CameraBoom->SetRelativeRotation(FRotator(-6.f, ShowcaseYaw, 0.f));
		CameraBoom->TargetArmLength = IsMotorised(Params.Category) ? 560.f : (Params.Category == MRSim::EVehicleCategory::EScooter ? 360.f : 430.f);
		ThirdPersonCamera->SetFieldOfView(60.f);
		ThirdPersonCamera->SetRelativeRotation(FRotator::ZeroRotator);
		static const bool bClose = FParse::Param(FCommandLine::Get(), TEXT("ShowcaseClose")); // rider close-up (materials)
		if (bClose)
		{
			CameraBoom->SetRelativeLocation(FVector(0.f, 0.f, -ContactToCenterUpCm + RiderDims.Height * 0.75f));
			CameraBoom->TargetArmLength = 150.f;
		}
		return;
	}
	if (CameraMode == EMRCameraMode::GaragePreview)
	{
		GarageOrbitYaw = FRotator::NormalizeAxis(GarageOrbitYaw + DeltaSeconds * 20.f);
		CameraBoom->SetRelativeRotation(FRotator(-15.f, GarageOrbitYaw + LookOffset.Yaw, 0.f));
		CameraBoom->TargetArmLength = IsMotorised(Params.Category) ? 330.f : 260.f;
		ThirdPersonCamera->SetFieldOfView(70.f);
		return;
	}

	CameraBoom->TargetArmLength = IsMotorised(Params.Category) ? 460.f : 360.f;
	// Look down a little more at low speed, flatter at speed; slight horizon roll with lean.
	const float BasePitch = FMath::Lerp(-14.f, -8.f, FMath::Clamp(Speed / 25.f, 0.f, 1.f));
	CameraBoom->SetRelativeRotation(FRotator(BasePitch + LookOffset.Pitch, LookOffset.Yaw, 0.f));
	ThirdPersonCamera->SetRelativeRotation(FRotator(0.f, 0.f, FMath::RadiansToDegrees(DisplayLean) * 0.15f));
	ThirdPersonCamera->SetFieldOfView(FMath::FInterpTo(ThirdPersonCamera->FieldOfView, 82.f + FMath::Clamp(Speed * 0.6f, 0.f, 18.f), DeltaSeconds, 3.f));
	// First person: keep some lean but reduce it so the horizon is comfortable.
	FirstPersonCamera->SetRelativeRotation(FRotator(LookOffset.Pitch, LookOffset.Yaw, -FMath::RadiansToDegrees(DisplayLean) * 0.4f));
}

void AMRRideVehicle::UpdateAudio()
{
	const bool bRemote = IsRemoteProxy();
	const float Rpm = bRemote ? LatestRemote.RpmDiv64 * 64.f : Sim.EngineRpm;
	const float Speed = FMath::Abs(GetSpeedMps());
	const float Throttle = bRemote ? 0.5f : Input.Throttle;
	const float Power = bRemote ? Speed * 40.f : Sim.MotorPowerW;
	Audio->SetDriveState(Rpm, Throttle, Power, Speed);
	const bool bSliding = bRemote ? (LatestRemote.Flags & EMRNetFlags::Sliding) != 0 : Sim.bRearSliding;
	const bool bGrounded = bRemote ? (LatestRemote.Flags & EMRNetFlags::Airborne) == 0 : !Sim.bAirborne;
	Audio->SetTyreState(Params.Tyre == MRSim::ETyre::Motocross ? 0.6f : 0.2f, bSliding ? 1.f : 0.f, bGrounded);
}

void AMRRideVehicle::UpdateForceFeedback(float DeltaSeconds)
{
	FeedbackCooldown -= DeltaSeconds;
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return;
	if (LastLandingImpact > 1.f && FeedbackCooldown <= 0.f)
	{
		PC->PlayDynamicForceFeedback(FMath::Clamp(LastLandingImpact / 8.f, 0.15f, 1.f), 0.2f, true, true, true, true);
		FeedbackCooldown = 0.15f;
	}
	LastLandingImpact = 0.f;
	if (Sim.bRearSliding && FeedbackCooldown <= 0.f)
	{
		PC->PlayDynamicForceFeedback(0.25f, 0.08f, false, true, false, true);
		FeedbackCooldown = 0.08f;
	}
}
