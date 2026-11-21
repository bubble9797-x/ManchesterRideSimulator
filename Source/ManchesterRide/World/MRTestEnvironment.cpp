#include "World/MRTestEnvironment.h"

#include "World/MRWorldEnvironment.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/ConstructorHelpers.h"

using MRSim::ESurface;

namespace Col
{
	const FLinearColor Grass(0.10f, 0.22f, 0.06f);
	const FLinearColor Asphalt(0.045f, 0.045f, 0.05f);
	const FLinearColor Pavement(0.32f, 0.32f, 0.31f);
	const FLinearColor Concrete(0.45f, 0.44f, 0.42f);
	const FLinearColor Gravel(0.30f, 0.27f, 0.22f);
	const FLinearColor Mud(0.12f, 0.08f, 0.04f);
	const FLinearColor Dirt(0.30f, 0.18f, 0.09f);
	const FLinearColor Forest(0.16f, 0.12f, 0.06f);
	const FLinearColor Cobbles(0.20f, 0.19f, 0.19f);
	const FLinearColor Brick(0.36f, 0.12f, 0.07f);
	const FLinearColor Slate(0.06f, 0.065f, 0.08f);
	const FLinearColor White(0.85f, 0.85f, 0.85f);
	const FLinearColor Yellow(0.80f, 0.62f, 0.02f);
	const FLinearColor Black(0.01f, 0.01f, 0.01f);
	const FLinearColor PostBoxRed(0.55f, 0.01f, 0.01f);
	const FLinearColor Amber(1.0f, 0.45f, 0.0f);
	const FLinearColor DoorBlue(0.02f, 0.08f, 0.25f);
	const FLinearColor Bark(0.10f, 0.07f, 0.04f);
	const FLinearColor Leaves(0.05f, 0.16f, 0.04f);
	const FLinearColor Metal(0.25f, 0.27f, 0.28f);
}

AMRTestEnvironment::AMRTestEnvironment()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Static);
	RootComponent = Root;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MatFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = CubeFinder.Object;
	CylinderMesh = CylFinder.Object;
	ConeMesh = ConeFinder.Object;
	SphereMesh = SphereFinder.Object;
	BaseMaterial = MatFinder.Object;
}

void AMRTestEnvironment::BeginPlay()
{
	Super::BeginPlay();
	Build();
}

FTransform AMRTestEnvironment::GetSpawnTransform(int32 PlayerIndex) const
{
	// Left-hand lane of the main street (UK: keep left), facing +X, staggered.
	const float X = -800.f - (PlayerIndex % 8) * 900.f;
	const float Y = (PlayerIndex / 8) % 2 == 0 ? -175.f : -1700.f;
	return FTransform(FRotator::ZeroRotator, FVector(X, Y, 150.f));
}

UStaticMeshComponent* AMRTestEnvironment::Box(const FVector& Center, const FVector& Size, const FRotator& Rot, const FLinearColor& Color, ESurface Surface, bool bCollide)
{
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(CubeMesh);
	C->SetMobility(EComponentMobility::Static);
	C->SetupAttachment(Root);
	C->SetRelativeLocation(Center);
	C->SetRelativeRotation(Rot);
	C->SetRelativeScale3D(Size / 100.f);
	C->SetCollisionEnabled(bCollide ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	C->SetCollisionProfileName(bCollide ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName);
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	MID->SetVectorParameterValue(TEXT("Color"), Color);
	C->SetMaterial(0, MID);
	if (bCollide) C->SetPhysMaterialOverride(SurfaceMaterials[static_cast<int32>(Surface)]);
	C->RegisterComponent();
	BuiltComponents.Add(C);
	return C;
}

UStaticMeshComponent* AMRTestEnvironment::Disc(const FVector& Center, float Radius, float Height, const FLinearColor& Color, ESurface Surface)
{
	UStaticMeshComponent* C = Box(Center, FVector(Radius * 2.f, Radius * 2.f, Height), FRotator::ZeroRotator, Color, Surface);
	C->SetStaticMesh(CylinderMesh);
	return C;
}

UInstancedStaticMeshComponent* AMRTestEnvironment::MakeISM(UStaticMesh* Mesh, const FLinearColor& Color, ESurface Surface, bool bCollide)
{
	UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this);
	C->SetStaticMesh(Mesh);
	C->SetMobility(EComponentMobility::Static);
	C->SetupAttachment(Root);
	C->SetCollisionEnabled(bCollide ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	C->SetCollisionProfileName(bCollide ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName);
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	MID->SetVectorParameterValue(TEXT("Color"), Color);
	C->SetMaterial(0, MID);
	if (bCollide) C->SetPhysMaterialOverride(SurfaceMaterials[static_cast<int32>(Surface)]);
	C->SetCullDistances(0, 60000); // 600 m
	C->RegisterComponent();
	BuiltComponents.Add(C);
	return C;
}

void AMRTestEnvironment::Ramp(const FVector& Base, float Yaw, float Length, float Width, float Height, const FLinearColor& Color, ESurface Surface)
{
	// A ramp whose top surface rises from Base (z = Base.Z) to Base.Z + Height over Length, facing Yaw.
	const float Angle = FMath::Atan2(Height, Length);
	const float Thick = 20.f;
	const FRotator YawRot(0.f, Yaw, 0.f);
	const FVector TopMid = Base + YawRot.RotateVector(FVector(Length * 0.5f, 0.f, Height * 0.5f));
	const FVector Normal = YawRot.RotateVector(FVector(-FMath::Sin(Angle), 0.f, FMath::Cos(Angle)));
	Box(TopMid - Normal * (Thick * 0.5f), FVector(FMath::Sqrt(Length * Length + Height * Height), Width, Thick),
		FRotator(FMath::RadiansToDegrees(Angle), Yaw, 0.f), Color, Surface);
	// Support block under the high half so the ramp isn't floating.
	const float SupportTop = Height * 0.5f - Thick;
	if (SupportTop > 10.f)
	{
		Box(Base + YawRot.RotateVector(FVector(Length * 0.75f, 0.f, SupportTop * 0.5f)), FVector(Length * 0.5f - 10.f, Width - 4.f, SupportTop), YawRot, Color * 0.8f, Surface);
	}
}

void AMRTestEnvironment::Tabletop(const FVector& Center, float Yaw, float Height, float TopLength, float RampLength, float Width, const FLinearColor& Color, ESurface Surface)
{
	const FRotator YawRot(0.f, Yaw, 0.f);
	const FVector Fwd = YawRot.Vector();
	Ramp(Center - Fwd * (TopLength * 0.5f + RampLength), Yaw, RampLength, Width, Height, Color, Surface);
	Box(Center + FVector(0, 0, Height * 0.5f), FVector(TopLength, Width, Height), YawRot, Color, Surface);
	Ramp(Center + Fwd * (TopLength * 0.5f + RampLength), Yaw + 180.f, RampLength * 1.4f, Width, Height, Color, Surface); // longer landing
}

void AMRTestEnvironment::Build()
{
	if (bBuilt) return;
	bBuilt = true;

	SurfaceMaterials.SetNum(static_cast<int32>(ESurface::Count));
	for (int32 i = 0; i < SurfaceMaterials.Num(); ++i)
	{
		UPhysicalMaterial* PM = NewObject<UPhysicalMaterial>(this);
		PM->SurfaceType = static_cast<EPhysicalSurface>(static_cast<int32>(SurfaceType1) + i);
		SurfaceMaterials[i] = PM;
	}

	// 600 m x 600 m grass base; everything else sits on top.
	Box(FVector(0, 0, -50.f), FVector(60000.f, 60000.f, 100.f), FRotator::ZeroRotator, Col::Grass, ESurface::Grass);
	Areas.Add({ TEXT("Test district (not the real map)"), FVector2D(0, 0), FVector2D(60000, 60000), Col::Grass });

	BuildStreet();
	BuildSkatepark();
	BuildDirtTrack();
	BuildTrails();
	BuildSurfacePads();
}

void AMRTestEnvironment::BuildStreet()
{
	const float HalfLen = 25000.f;
	const float RoadHalfW = 350.f;
	// Main street (E-W) and cross street (N-S)
	Box(FVector(0, 0, -3.f), FVector(HalfLen * 2.f, RoadHalfW * 2.f, 10.f), FRotator::ZeroRotator, Col::Asphalt, ESurface::Asphalt);
	Box(FVector(0, 0, -3.f), FVector(RoadHalfW * 2.f, HalfLen * 2.f, 10.f), FRotator::ZeroRotator, Col::Asphalt, ESurface::Asphalt);
	Areas.Add({ TEXT("Terrace Street"), FVector2D(0, 0), FVector2D(HalfLen * 2.f, 1300.f), Col::Asphalt });

	UInstancedStaticMeshComponent* Markings = MakeISM(CubeMesh, Col::White, ESurface::Asphalt, false);
	UInstancedStaticMeshComponent* Yellows = MakeISM(CubeMesh, Col::Yellow, ESurface::Asphalt, false);
	auto Mark = [&](UInstancedStaticMeshComponent* ISM, const FVector& C, const FVector& S, float Yaw = 0.f)
	{
		ISM->AddInstance(FTransform(FRotator(0, Yaw, 0), C, S / 100.f));
	};
	// Centre line dashes (4 m line, 8 m gap - UK urban "1004" marking), skipping the junction
	for (float X = -HalfLen; X < HalfLen; X += 1200.f)
	{
		if (FMath::Abs(X) < 900.f) continue;
		Mark(Markings, FVector(X + 200.f, 0, 2.5f), FVector(400.f, 10.f, 1.f));
		Mark(Markings, FVector(0, X + 200.f, 2.5f), FVector(10.f, 400.f, 1.f));
	}
	// Double yellow lines along the kerbs near the junction (no waiting at any time)
	for (float Side : { -1.f, 1.f })
	{
		for (float Off : { 0.f, 20.f })
		{
			Mark(Yellows, FVector(2500.f, Side * (RoadHalfW - 25.f - Off), 2.5f), FVector(3200.f, 8.f, 1.f));
			Mark(Yellows, FVector(-2500.f, Side * (RoadHalfW - 25.f - Off), 2.5f), FVector(3200.f, 8.f, 1.f));
		}
		// Give-way lines (double dashed) where the cross street meets the main street
		for (float Y = -RoadHalfW; Y < RoadHalfW; Y += 90.f)
		{
			Mark(Markings, FVector(Side * 500.f, Y + 30.f, 2.5f), FVector(10.f, 60.f, 1.f));
			Mark(Markings, FVector(Side * 530.f, Y + 30.f, 2.5f), FVector(10.f, 60.f, 1.f));
		}
	}
	// Mini roundabout disc at the junction
	Disc(FVector(0, 0, 2.f), 120.f, 3.f, Col::White, ESurface::Asphalt);

	// Zebra crossing with Belisha beacons at x = -3500
	for (float Y = -RoadHalfW + 25.f; Y < RoadHalfW; Y += 100.f)
	{
		Mark(Markings, FVector(-3500.f, Y + 25.f, 2.5f), FVector(300.f, 50.f, 1.f));
	}
	UInstancedStaticMeshComponent* Poles = MakeISM(CylinderMesh, Col::Black, ESurface::Concrete, true);
	UInstancedStaticMeshComponent* Beacons = MakeISM(SphereMesh, Col::Amber, ESurface::Concrete, false);
	for (float Side : { -1.f, 1.f })
	{
		Poles->AddInstance(FTransform(FRotator::ZeroRotator, FVector(-3700.f, Side * 450.f, 150.f), FVector(0.1f, 0.1f, 3.f)));
		Beacons->AddInstance(FTransform(FRotator::ZeroRotator, FVector(-3700.f, Side * 450.f, 320.f), FVector(0.35f)));
	}

	// Pavements (raised 12 cm kerbs) both sides of the main street
	for (float Side : { -1.f, 1.f })
	{
		Box(FVector(0, Side * (RoadHalfW + 150.f), 6.f - 3.f), FVector(HalfLen * 2.f, 300.f, 18.f), FRotator::ZeroRotator, Col::Pavement, ESurface::Concrete);
	}

	// Terraced houses with slate roofs, chimneys and front doors; cobbled back alleys behind.
	UInstancedStaticMeshComponent* Houses = MakeISM(CubeMesh, Col::Brick, ESurface::Concrete, true);
	UInstancedStaticMeshComponent* Roofs = MakeISM(CubeMesh, Col::Slate, ESurface::Concrete, true);
	UInstancedStaticMeshComponent* Chimneys = MakeISM(CubeMesh, Col::Brick * 0.8f, ESurface::Concrete, false);
	UInstancedStaticMeshComponent* Doors = MakeISM(CubeMesh, Col::DoorBlue, ESurface::Concrete, false);
	UInstancedStaticMeshComponent* Windows = MakeISM(CubeMesh, Col::White, ESurface::Concrete, false);
	const float HouseW = 480.f, HouseD = 800.f, HouseH = 680.f, RowOffset = 650.f + HouseD * 0.5f + 50.f;
	for (float Side : { -1.f, 1.f })
	{
		for (float X = -20000.f; X < 20000.f; X += HouseW)
		{
			const int32 Index = FMath::RoundToInt((X + 20000.f) / HouseW);
			if (FMath::Abs(X) < 1200.f || Index % 9 == 8) continue; // junction + ginnels (gaps) every 8 houses
			const float Y = Side * RowOffset;
			Houses->AddInstance(FTransform(FRotator::ZeroRotator, FVector(X, Y, HouseH * 0.5f), FVector(HouseW - 4.f, HouseD, HouseH) / 100.f));
			for (float RoofSide : { -1.f, 1.f })
			{
				Roofs->AddInstance(FTransform(FRotator(0.f, 0.f, RoofSide * 32.f), FVector(X, Y + RoofSide * HouseD * 0.25f, HouseH + 120.f),
					FVector(HouseW, HouseD * 0.6f, 15.f) / 100.f));
			}
			if (Index % 2 == 0)
			{
				Chimneys->AddInstance(FTransform(FRotator::ZeroRotator, FVector(X + HouseW * 0.5f, Y, HouseH + 300.f), FVector(70.f, 60.f, 160.f) / 100.f));
			}
			const float FrontY = Y - Side * (HouseD * 0.5f + 1.f);
			Doors->AddInstance(FTransform(FRotator::ZeroRotator, FVector(X - 140.f, FrontY, 105.f), FVector(95.f, 6.f, 210.f) / 100.f));
			Windows->AddInstance(FTransform(FRotator::ZeroRotator, FVector(X + 80.f, FrontY, 150.f), FVector(170.f, 6.f, 140.f) / 100.f));
			Windows->AddInstance(FTransform(FRotator::ZeroRotator, FVector(X - 40.f, FrontY, 470.f), FVector(150.f, 6.f, 130.f) / 100.f));
		}
		// Cobbled back alley ("ginnel"/back entry) behind each row
		Box(FVector(0, Side * (RowOffset + HouseD * 0.5f + 200.f), -1.f), FVector(40000.f, 400.f, 6.f), FRotator::ZeroRotator, Col::Cobbles, ESurface::Cobbles);
		Areas.Add({ TEXT("Cobbled back alley"), FVector2D(0, Side * (RowOffset + HouseD * 0.5f + 200.f)), FVector2D(40000.f, 400.f), Col::Cobbles });
	}

	// Lamp posts every 30 m; real lights only on the central stretch to keep it cheap.
	UInstancedStaticMeshComponent* Lamps = MakeISM(CylinderMesh, Col::Metal, ESurface::Concrete, true);
	UInstancedStaticMeshComponent* LampHeads = MakeISM(CubeMesh, Col::Metal, ESurface::Concrete, false);
	AMRWorldEnvironment* Env = AMRWorldEnvironment::Get(this);
	for (float X = -24000.f; X <= 24000.f; X += 3000.f)
	{
		for (float Side : { -1.f, 1.f })
		{
			const float Y = Side * (RoadHalfW + 260.f);
			Lamps->AddInstance(FTransform(FRotator::ZeroRotator, FVector(X, Y, 400.f), FVector(0.14f, 0.14f, 8.f)));
			LampHeads->AddInstance(FTransform(FRotator::ZeroRotator, FVector(X, Y - Side * 60.f, 800.f), FVector(1.4f, 0.4f, 0.15f)));
			if (FMath::Abs(X) <= 9000.f)
			{
				UPointLightComponent* L = NewObject<UPointLightComponent>(this);
				L->SetMobility(EComponentMobility::Movable);
				L->SetupAttachment(Root);
				L->SetRelativeLocation(FVector(X, Y - Side * 60.f, 780.f));
				L->SetAttenuationRadius(1800.f);
				L->SetLightColor(FLinearColor(1.f, 0.78f, 0.5f)); // sodium-ish warm white
				L->SetCastShadows(false);
				L->SetIntensity(0.f);
				L->SetVisibility(false);
				L->RegisterComponent();
				// Actor BeginPlay order differs between server and clients: register now if the
				// environment exists, otherwise it collects these in its own BeginPlay.
				if (Env) Env->RegisterNightLight(L, 8000.f);
				else PendingNightLights.Add(L);
			}
		}
	}

	// Bus stop (shelter, bench, flag pole) and a pillar post box.
	const FVector Stop(5000.f, -(RoadHalfW + 220.f), 0.f);
	Box(Stop + FVector(0, -110.f, 125.f), FVector(400.f, 8.f, 230.f), FRotator::ZeroRotator, Col::Metal * 1.5f, ESurface::Concrete);
	Box(Stop + FVector(0, -40.f, 250.f), FVector(420.f, 160.f, 8.f), FRotator::ZeroRotator, Col::Metal, ESurface::Concrete);
	Box(Stop + FVector(0, -80.f, 45.f), FVector(300.f, 40.f, 8.f), FRotator::ZeroRotator, Col::Metal, ESurface::Concrete, false);
	Poles->AddInstance(FTransform(FRotator::ZeroRotator, Stop + FVector(260.f, 60.f, 150.f), FVector(0.08f, 0.08f, 3.f)));
	Box(Stop + FVector(260.f, 60.f, 290.f), FVector(4.f, 50.f, 50.f), FRotator::ZeroRotator, Col::White, ESurface::Concrete, false);
	Disc(FVector(-3000.f, RoadHalfW + 220.f, 80.f), 28.f, 160.f, Col::PostBoxRed, ESurface::Concrete);
}

void AMRTestEnvironment::BuildSkatepark()
{
	const FVector C(-12000.f, 9000.f, 0.f);
	Box(C + FVector(0, 0, -2.f), FVector(6000.f, 4000.f, 8.f), FRotator::ZeroRotator, Col::Concrete, ESurface::Concrete);
	Areas.Add({ TEXT("Skatepark"), FVector2D(C), FVector2D(6000.f, 4000.f), Col::Concrete });
	const FVector Top = C + FVector(0, 0, 2.f);
	Ramp(Top + FVector(-2500.f, -1200.f, 0), 0.f, 250.f, 250.f, 80.f, Col::Concrete * 0.9f, ESurface::Concrete);   // small kicker
	Ramp(Top + FVector(-2500.f, 0.f, 0), 0.f, 300.f, 250.f, 130.f, Col::Concrete * 0.9f, ESurface::Concrete);      // medium kicker
	Ramp(Top + FVector(-2500.f, 1200.f, 0), 0.f, 350.f, 250.f, 180.f, Col::Concrete * 0.9f, ESurface::Concrete);   // big kicker
	Tabletop(Top + FVector(300.f, 0, 0), 0.f, 80.f, 1500.f, 300.f, 500.f, Col::Concrete * 0.85f, ESurface::Concrete); // funbox
	Box(Top + FVector(300.f, 1300.f, 15.f), FVector(2000.f, 400.f, 30.f), FRotator::ZeroRotator, Col::Concrete * 0.8f, ESurface::Concrete); // manual pad
	Ramp(Top + FVector(2400.f, -1000.f, 0), 0.f, 300.f, 800.f, 260.f, Col::Concrete * 0.9f, ESurface::Concrete);   // steep quarter
	Ramp(Top + FVector(2000.f, 1200.f, 0), 0.f, 700.f, 600.f, 200.f, Col::Concrete * 0.9f, ESurface::Concrete);    // bank
	for (int32 Step = 0; Step < 5; ++Step) // five-stair set with a top deck
	{
		const float H = (Step + 1) * 18.f;
		Box(Top + FVector(-1000.f + Step * 45.f, -1500.f, H * 0.5f), FVector(45.f, 500.f, H), FRotator::ZeroRotator, Col::Concrete * 0.8f, ESurface::Concrete);
	}
	Box(Top + FVector(-1000.f + 5 * 45.f + 200.f, -1500.f, 45.f), FVector(400.f, 500.f, 90.f), FRotator::ZeroRotator, Col::Concrete * 0.8f, ESurface::Concrete);
}

void AMRTestEnvironment::BuildDirtTrack()
{
	const FVector C(12000.f, 9000.f, 0.f);
	Box(C + FVector(0, 0, -1.f), FVector(8000.f, 5000.f, 6.f), FRotator::ZeroRotator, Col::Dirt, ESurface::Dirt);
	Areas.Add({ TEXT("Dirt jumps & MX track"), FVector2D(C), FVector2D(8000.f, 5000.f), Col::Dirt });
	const FVector Top = C + FVector(0, 0, 2.f);
	Tabletop(Top + FVector(-2200.f, -1200.f, 0), 0.f, 120.f, 400.f, 350.f, 400.f, Col::Dirt * 1.1f, ESurface::Dirt);
	Tabletop(Top + FVector(1000.f, -1200.f, 0), 0.f, 200.f, 800.f, 500.f, 500.f, Col::Dirt * 1.1f, ESurface::Dirt);
	Tabletop(Top + FVector(-1500.f, 1200.f, 0), 180.f, 250.f, 1200.f, 600.f, 600.f, Col::Dirt * 1.1f, ESurface::Dirt);
	// Double: take-off and separate landing with a gap
	Ramp(Top + FVector(2000.f, 1200.f, 0), 180.f, 400.f, 400.f, 150.f, Col::Dirt * 1.1f, ESurface::Dirt);
	Ramp(Top + FVector(500.f, 1200.f, 0), 0.f, 600.f, 400.f, 150.f, Col::Dirt * 1.1f, ESurface::Dirt);
	// Berms: banked turns at each end
	for (float Side : { -1.f, 1.f })
	{
		Box(Top + FVector(Side * 3700.f, 0, 60.f), FVector(400.f, 4400.f, 30.f), FRotator(0.f, 0.f, 0.f) + FRotator(Side * -35.f, 0.f, 0.f), Col::Dirt * 0.9f, ESurface::Dirt);
	}
	// Mud pit inside the track
	Box(Top + FVector(0, 0, -1.f), FVector(1500.f, 900.f, 4.f), FRotator::ZeroRotator, Col::Mud, ESurface::Mud);
}

void AMRTestEnvironment::BuildTrails()
{
	const FVector C(-12000.f, -10000.f, 0.f);
	Box(C + FVector(0, 0, -1.f), FVector(9000.f, 7000.f, 4.f), FRotator::ZeroRotator, Col::Forest, ESurface::ForestTrail);
	Areas.Add({ TEXT("Woodland trails"), FVector2D(C), FVector2D(9000.f, 7000.f), Col::Forest });

	// Gravel loop path
	Box(C + FVector(0, -3000.f, 1.f), FVector(8000.f, 300.f, 4.f), FRotator::ZeroRotator, Col::Gravel, ESurface::Gravel);
	Box(C + FVector(0, 3000.f, 1.f), FVector(8000.f, 300.f, 4.f), FRotator::ZeroRotator, Col::Gravel, ESurface::Gravel);
	Box(C + FVector(-4000.f, 0, 1.f), FVector(300.f, 6300.f, 4.f), FRotator::ZeroRotator, Col::Gravel, ESurface::Gravel);
	Box(C + FVector(4000.f, 0, 1.f), FVector(300.f, 6300.f, 4.f), FRotator::ZeroRotator, Col::Gravel, ESurface::Gravel);

	// Trees (deterministic so every machine builds the same forest), kept off the paths.
	UInstancedStaticMeshComponent* Trunks = MakeISM(CylinderMesh, Col::Bark, ESurface::ForestTrail, true);
	UInstancedStaticMeshComponent* Canopy = MakeISM(SphereMesh, Col::Leaves, ESurface::ForestTrail, false);
	UInstancedStaticMeshComponent* Conifers = MakeISM(ConeMesh, Col::Leaves * 0.7f, ESurface::ForestTrail, false);
	FRandomStream Rng(5301);
	for (int32 i = 0; i < 260; ++i)
	{
		const FVector P = C + FVector(Rng.FRandRange(-4300.f, 4300.f), Rng.FRandRange(-3300.f, 3300.f), 0.f);
		const FVector Local = P - C;
		if (FMath::Abs(FMath::Abs(Local.Y) - 3000.f) < 350.f || FMath::Abs(FMath::Abs(Local.X) - 4000.f) < 350.f) continue;
		if (FMath::Abs(Local.Y) < 350.f) continue; // central singletrack corridor
		const float H = Rng.FRandRange(600.f, 1400.f);
		Trunks->AddInstance(FTransform(FRotator::ZeroRotator, P + FVector(0, 0, H * 0.5f), FVector(0.35f, 0.35f, H / 100.f)));
		if (Rng.FRand() < 0.6f)
			Canopy->AddInstance(FTransform(FRotator::ZeroRotator, P + FVector(0, 0, H), FVector(Rng.FRandRange(3.f, 6.f))));
		else
			Conifers->AddInstance(FTransform(FRotator::ZeroRotator, P + FVector(0, 0, H * 0.7f), FVector(4.f, 4.f, H / 100.f)));
	}
	// Roots/rollers on the central singletrack
	for (int32 i = 0; i < 12; ++i)
	{
		Box(C + FVector(-3500.f + i * 600.f, 0, 8.f), FVector(60.f, 250.f, 16.f), FRotator(0, Rng.FRandRange(-20.f, 20.f), 0), Col::Bark, ESurface::ForestTrail);
	}
}

void AMRTestEnvironment::BuildSurfacePads()
{
	const FVector C(12000.f, -10000.f, 0.f);
	struct FPad { const TCHAR* Name; FLinearColor Color; ESurface Surface; };
	const FPad Pads[] = {
		{ TEXT("Asphalt"), Col::Asphalt, ESurface::Asphalt }, { TEXT("Concrete"), Col::Concrete, ESurface::Concrete },
		{ TEXT("Gravel"), Col::Gravel, ESurface::Gravel },     { TEXT("Grass"), Col::Grass * 1.3f, ESurface::Grass },
		{ TEXT("Mud"), Col::Mud, ESurface::Mud },              { TEXT("Dirt"), Col::Dirt, ESurface::Dirt },
		{ TEXT("Forest trail"), Col::Forest, ESurface::ForestTrail }, { TEXT("Cobbles (setts)"), Col::Cobbles, ESurface::Cobbles },
	};
	for (int32 i = 0; i < 8; ++i)
	{
		const FVector P = C + FVector(-3500.f + i * 1000.f, 0.f, 0.f);
		Box(P + FVector(0, 0, 0.f), FVector(900.f, 5000.f, 4.f), FRotator::ZeroRotator, Pads[i].Color, Pads[i].Surface);
		Areas.Add({ FString::Printf(TEXT("Surface test: %s"), Pads[i].Name), FVector2D(P), FVector2D(900.f, 5000.f), Pads[i].Color });
	}
	// Slope test: 10 and 20 degree hills with plateaus
	for (int32 i = 0; i < 2; ++i)
	{
		const float Deg = i == 0 ? 10.f : 20.f;
		const float Len = 3000.f;
		const float H = Len * FMath::Tan(FMath::DegreesToRadians(Deg));
		const FVector Base = C + FVector(-3500.f, -4500.f - i * 1500.f, 0.f);
		Ramp(Base, 0.f, Len, 900.f, H, Col::Asphalt * 1.4f, ESurface::Asphalt);
		Box(Base + FVector(Len + 1000.f, 0, H * 0.5f), FVector(2000.f, 900.f, H), FRotator::ZeroRotator, Col::Asphalt * 1.4f, ESurface::Asphalt);
		Ramp(Base + FVector(Len * 2.f + 2000.f, 0, 0), 180.f, Len, 900.f, H, Col::Asphalt * 1.4f, ESurface::Asphalt);
	}
	Areas.Add({ TEXT("Hill test (10 / 20 deg)"), FVector2D(C + FVector(1000.f, -5250.f, 0)), FVector2D(9000.f, 3000.f), Col::Asphalt * 1.4f });
}
