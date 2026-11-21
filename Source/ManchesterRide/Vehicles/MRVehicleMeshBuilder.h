// Manchester Riding Simulator - detailed procedural vehicle meshes.
//
// Every vehicle in the catalogue is built from its own measurements (wheelbase, wheel sizes,
// suspension travel, powertrain, category) plus real-world frame geometry for its type (head and
// seat angles, chainstay length, bottom-bracket drop, fork length, rake). Output is plain mesh data
// per moving part, so the actor can put each part on the component that animates it (body, steering,
// fork lowers, swingarm, wheels).
#pragma once

#include "CoreMinimal.h"
#include "RideSimTypes.h"

enum class EMRVisMat : uint8
{
	Paint,   // frame / bodywork: the player's paint colour
	Alloy,   // brushed aluminium
	Black,   // anodised / powder-coated black
	Rubber,  // tyres, grips
	Chrome,  // rotors, stanchions, exhaust, chain
	Gold,    // kashima / gold-anodised (stanchion coats, springs, chain on bikes that use it)
	Seat,    // saddle / seat cover
	Lens,    // lights (emissive)
	RedLens, // tail lights / rear reflectors
	Amber,   // indicators / side reflectors
	Plate,   // UK rear number plate (yellow)
	Count
};

struct FMRMeshSection
{
	TArray<FVector> V;
	TArray<FVector> N;
	TArray<int32> T;
	TArray<FColor> C; // optional per-vertex colour (used only when C.Num() == V.Num(); buildings use it for wall/roof colour)
};

struct FMRPartMesh
{
	FMRMeshSection S[static_cast<int32>(EMRVisMat::Count)];
	FMRMeshSection& operator[](EMRVisMat M) { return S[static_cast<int32>(M)]; }
	int32 NumTris() const { int32 N = 0; for (const FMRMeshSection& X : S) N += X.T.Num() / 3; return N; }
};

/** Low-level shape kit. All sizes in cm, normals are smooth and triangles face outwards. */
namespace MRMeshKit
{
	void Tube(FMRMeshSection& S, const FVector& A, const FVector& B, float R0, float R1, int32 Sides = 12, bool bCaps = true);
	void PathTube(FMRMeshSection& S, const TArray<FVector>& Pts, float R, int32 Sides = 10, bool bCaps = true);
	/** Torus around Axis. MinorAxisScale squashes the section along the axis (tyre width vs height). */
	void Torus(FMRMeshSection& S, const FVector& C, const FVector& Axis, float MajorR, float MinorR, int32 Seg = 48, int32 Sides = 12, float MinorAxisScale = 1.f);
	/** Flat ring (Rin = 0 gives a solid disc) of the given thickness along Axis. */
	void Disc(FMRMeshSection& S, const FVector& C, const FVector& Axis, float Rin, float Rout, float Thick, int32 Seg = 32);
	void Box(FMRMeshSection& S, const FVector& C, const FQuat& Q, const FVector& Half);
	/** Box from A to B (long axis) with the given width (Y-ish) and height. */
	void Bar(FMRMeshSection& S, const FVector& A, const FVector& B, float Width, float Height, const FVector& UpHint = FVector::UpVector);
	void Ellipsoid(FMRMeshSection& S, const FVector& C, const FVector& Radii, const FQuat& Q = FQuat::Identity, int32 Seg = 18, int32 Rings = 10);
	void Helix(FMRMeshSection& S, const FVector& A, const FVector& B, float CoilR, float WireR, float Turns);
}

struct FMRVehicleVisual
{
	// Body: BodyPivot space (rear contact patch at the origin, +X forward, +Z up, +Y right).
	FMRPartMesh Body;
	// Steer: SteerPivot space (front contact patch at the origin, rotates about +Z with steering).
	FMRPartMesh Steer;
	// FrontLowers: follows the front axle (fork lowers, caliper, low mudguard); origin at the axle.
	FMRPartMesh FrontLowers;
	// Swingarm: rotates about SwingPivot (body space) so its rear end tracks the rear axle; origin at the pivot.
	FMRPartMesh Swingarm;
	FVector SwingPivot = FVector::ZeroVector;
	bool bHasSwingarm = false;
	// Wheels: hub space, spin axis +Y.
	FMRPartMesh FrontWheel, RearWheel;

	// Cranks (bicycles): Crank rotates about +Y at BB (arms built at angle 0: right arm along +X);
	// each pedal is its own part whose origin is the pedal axle, kept level as the cranks turn.
	FMRPartMesh Crank, Pedal[2]; // Pedal[0] left (-Y), [1] right (+Y)
	bool bHasCranks = false;
	FVector BB = FVector::ZeroVector;
	float CrankLength = 17.f;
	float PedalY = 13.f;   // pedal centre, either side of the centre line
	float GearRatio = 2.6f; // rear wheel turns per crank turn in a typical riding gear

	// Rider contact points (body space) used to pose the rider.
	FVector Seat, Bars, Foot, Head;
	float BarHalfWidth = 36.f;
	float FootHalfWidth = 14.f; // footpegs / deck stance either side of the centre line
	bool bStanding = false;     // e-scooters: the rider stands on the deck
	FVector FrontLight = FVector::ZeroVector;
	bool bHasFrontLight = false;
};

/** Builds the full detailed vehicle. StyleId is the catalogue id (used for style cues such as "cruiser", "trials"). */
FMRVehicleVisual MRBuildVehicleVisual(const MRSim::FVehicleParams& P, const FString& StyleId);
