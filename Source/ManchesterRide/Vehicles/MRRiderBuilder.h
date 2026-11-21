// Manchester Riding Simulator - procedural human rider.
//
// The rider is built from real human proportions (scaled by the character editor's height and
// build) as one mesh per body segment: torso, neck, head, upper arms, forearms, hands, thighs,
// shins and feet. Each segment is modelled in its own joint frame, so the vehicle can pose the
// rider every frame with simple two-bone IK: hands on the grips (following the steering), feet on
// the pedals (following the cranks), on the footpegs, or on the scooter deck.
#pragma once

#include "CoreMinimal.h"
#include "Vehicles/MRVehicleMeshBuilder.h"
#include "Core/MRRiderLook.h"

enum class EMRRiderMat : uint8
{
	Skin, Lips, Eye, Iris, Hair, Beard,
	Top,     // jacket / hoodie / jersey / leathers / T-shirt (the player's jacket colour)
	Accent,  // trim, stripes, pockets
	Legs,    // trousers
	Gloves, Shoes, Sole,
	Helmet,  // shell (the player's helmet colour)
	Visor, Strap, HiVis, Reflect, Bag,
	Count
};

enum class EMRRiderSeg : uint8
{
	Torso, Neck, Head,
	UpperArmL, UpperArmR, ForearmL, ForearmR, HandL, HandR,
	ThighL, ThighR, ShinL, ShinR, FootL, FootR,
	Count
};

struct FMRRiderPart
{
	FMRMeshSection S[static_cast<int32>(EMRRiderMat::Count)];
	FMRMeshSection& operator[](EMRRiderMat M) { return S[static_cast<int32>(M)]; }
	int32 NumTris() const { int32 N = 0; for (const FMRMeshSection& X : S) N += X.T.Num() / 3; return N; }
};

/** Segment lengths (cm) derived from the rider's height and build. */
struct FMRRiderDims
{
	float Height = 178.f;
	float Torso = 51.f;        // hip-joint centre -> shoulder-joint line
	float ShoulderHalf = 18.f; // half the distance between the shoulder joints
	float HipHalf = 9.f;
	float Neck = 10.f;
	float UpperArm = 30.f, Forearm = 28.f, Hand = 8.f; // Hand: wrist -> grip centre
	float Thigh = 43.f, Shin = 43.f;
	float AnkleH = 7.6f;  // ankle joint above the bottom of the sole
	float FootBall = 13.f; // ball of the foot, forward of the ankle
};

struct FMRRiderMesh
{
	FMRRiderPart Seg[static_cast<int32>(EMRRiderSeg::Count)];
	FMRRiderDims D;
};

/** bMotoGear: motorbike boots and gauntlets. */
FMRRiderMesh MRBuildRiderMesh(const FMRRiderLook& Look, bool bMotoGear);

/** Where the rider has to put their hands and feet (vehicle body space). Index 0 = left (-Y), 1 = right (+Y). */
struct FMRRiderTargets
{
	FVector Hip = FVector::ZeroVector;      // hip-joint centre
	FVector HeadHint = FVector::ZeroVector; // rough head position for the vehicle's riding position
	FVector Grip[2];
	FVector FootBall[2];  // where the ball of each foot rests (pedal / peg / deck top)
	float FootPitch[2] = { 0.f, 0.f }; // starting toe-down angle (rad); raised automatically if the leg can't reach
	float KneeOut = 0.25f;
	float KneeOutExtra[2] = { 0.f, 0.f }; // per leg, e.g. the inside knee when hanging off
	float HeadYaw = 0.f;   // rad, look into the turn
	// Weight shifts (all relative to the vehicle body):
	float TorsoBias = 0.f; // rad added to the preferred lean (+ = further forward over the bars)
	float BodyRoll = 0.f;  // rad, torso roll about the hips (+ = right, same sense as the bike's lean)
	float HeadPitch = 0.f; // rad added to the head's pitch (+ = chin down)
	float HeadRoll = 0.f;  // rad added to the head's roll (keeps the eyes level in corners)
};

/** Joint positions the solver produces; the crash ragdoll starts from these. */
enum class EMRRiderJoint : uint8
{
	Pelvis, HipL, HipR, Chest, ShL, ShR, Neck, Head, HeadTop, HeadFwd,
	ElbowL, ElbowR, WristL, WristR, HandL, HandR, KneeL, KneeR, AnkleL, AnkleR, ToeL, ToeR,
	Count
};

struct FMRRiderPose
{
	FTransform Seg[static_cast<int32>(EMRRiderSeg::Count)];
	FVector Eye = FVector::ZeroVector;
	FVector Joint[static_cast<int32>(EMRRiderJoint::Count)];
};

void MRSolveRiderPose(const FMRRiderDims& D, const FMRRiderTargets& T, FMRRiderPose& Out);

/**
 * Crash ragdoll: the joints become point masses joined by distance links (Verlet integration),
 * falling under gravity, tumbling and sliding to a stop on the ground. Works in world space so
 * the rider leaves the bike. Each segment's rotation is rebuilt from three of its joints.
 */
struct FMRRiderRagdoll
{
	static constexpr int32 NJ = static_cast<int32>(EMRRiderJoint::Count);
	static constexpr int32 NS = static_cast<int32>(EMRRiderSeg::Count);
	FVector P[NJ], Prev[NJ];
	float Radius[NJ];
	struct FLink { uint8 A, B; float Len; bool bMinOnly; };
	TArray<FLink> Links;
	struct FSegRef { uint8 O = 0, A = 0, B = 0; FQuat LocalBasisInv = FQuat::Identity; };
	FSegRef Seg[NS];
	FVector EyeLocal = FVector::ZeroVector; // eye in the head frame
	float Accum = 0.f;
	float Time = 0.f;

	/** Pose in world space. Vel: cm/s of the whole body. Spin: rad/s about world axes (around the pelvis). */
	void Init(const FMRRiderPose& WorldPose, const FMRRiderDims& D, const FVector& Vel, const FVector& Spin);
	/** GroundZ: ground height (cm) under each joint, or a very low value where there is none. */
	void Step(float Dt, const float* GroundZ);
	/** World-space segment transforms and eye. */
	void GetPose(FMRRiderPose& Out) const;
};
