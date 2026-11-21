#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Core/MRRiderLook.h"
#include "Vehicles/MRRiderBuilder.h"
#include "Net/MRNetTypes.h"
#include "RideSim.h"
#include "TrickTracker.h"
#include "MRRideVehicle.generated.h"

class UBoxComponent;
class USpringArmComponent;
class UCameraComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMRRideAudioComponent;
class AMRGeoWorld;
class UProceduralMeshComponent;
struct FMRPartMesh;

UENUM(BlueprintType)
enum class EMRCameraMode : uint8
{
	ThirdPerson,
	FirstPerson,
	GaragePreview,
};

/** A trick reported to the HUD (and to the server for scoring). */
USTRUCT(BlueprintType)
struct FMRTrickPopup
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) FString Label;
	UPROPERTY(BlueprintReadOnly) int32 Points = 0;
	UPROPERTY(BlueprintReadOnly) float TimeShown = 0.f;
};

/**
 * One rideable two-wheeler (every vehicle category uses this class; behaviour comes
 * from MRSim::FVehicleParams in the catalogue, not from duplicated classes).
 *
 * Frames: the actor origin is the centre of the collision box. The "contact frame"
 * origin is the rear tyre contact at full suspension extension, ContactToCenter below
 * and behind it. Actor rotation = (GroundPitch, Yaw, 0); wheelie pitch and lean are
 * applied only to the visual BodyPivot so collision stays stable.
 *
 * Networking (see docs/ARCHITECTURE.md):
 *  - The owning client simulates its own vehicle (no input latency, no rubber-banding)
 *    and sends compact state to the server at ~30 Hz (unreliable).
 *  - The server validates (speed / teleport limits), corrects if needed, and replicates
 *    the state to everyone else (COND_SkipOwner).
 *  - Remote vehicles are rendered from a jitter buffer with clock-offset estimation,
 *    interpolated ~120 ms in the past and briefly extrapolated on packet loss.
 */
UCLASS()
class MANCHESTERRIDE_API AMRRideVehicle : public APawn
{
	GENERATED_BODY()

public:
	AMRRideVehicle();

	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PossessedBy(AController* NewController) override;

	// ---- Configuration (server sets before FinishSpawning; replicated) ----
	UFUNCTION(BlueprintCallable, Category = "Vehicle")
	void InitVehicle(FName InVehicleId, FLinearColor InPaint, FLinearColor InJacket, FLinearColor InHelmet);
	/** Character editor look; rebuilds the rider (server sets it, replicates to all). */
	void SetRiderLook(const FMRRiderLook& InLook);
	const FMRRiderLook& GetRiderLook() const { return RiderLook; }
	/** Set paint/jacket/helmet colours without respawning (garage + character editor preview; server: authoritative). */
	void PreviewColours(const FLinearColor& Paint, const FLinearColor& Jacket, const FLinearColor& Helmet) { PaintColor = Paint; JacketColor = Jacket; HelmetColor = Helmet; UpdateMaterials(); }
	FLinearColor GetPaintColor() const { return PaintColor; }

	UFUNCTION(BlueprintPure, Category = "Vehicle") FName GetVehicleId() const { return VehicleId; }
	UFUNCTION(BlueprintPure, Category = "Vehicle") FLinearColor GetPaint() const { return PaintColor; }

	// ---- Input (called by the owning player controller every frame) ----
	void SetRiderInput(const MRSim::FRiderInput& In) { Input = In; }
	void SetAssistSettings(const MRSim::FAssistSettings& In) { Assist = In; }
	const MRSim::FAssistSettings& GetAssistSettings() const { return Assist; }
	void RequestGearChange(int32 Direction) { PendingGear += Direction; }

	UFUNCTION(BlueprintCallable, Category = "Camera") void CycleCameraMode();
	void SetCameraMode(EMRCameraMode Mode);
	EMRCameraMode GetCameraMode() const { return CameraMode; }
	void AddLookInput(FVector2D Delta);
	void SetCameraSensitivity(float In) { CameraSensitivity = In; }

	UFUNCTION(BlueprintCallable, Category = "Vehicle") void RequestRespawn();
	/** Puts the rider on the ground at Where (Unreal cm), facing YawDeg, stopped. Used by the map's teleport-to-waypoint. */
	void TeleportRider(const FVector2D& Where, float YawDeg);

	// ---- Read-only state for HUD / audio ----
	const MRSim::FRideState& GetSimState() const { return Sim; }
	const MRSim::FVehicleParams& GetParams() const { return Params; }
	const MRSim::FTrickTracker& GetTricks() const { return Tricks; }
	const TArray<FMRTrickPopup>& GetTrickPopups() const { return TrickPopups; }
	float GetSpeedMps() const;
	bool IsRemoteProxy() const;

	/** Lean/pitch/etc. currently displayed (local sim or interpolated network state). */
	float GetDisplayLean() const { return DisplayLean; }

	// ---- Dedicated server riders (UMRServerClient): puppets with no controller, fed states from the server ----
	FMRNetRideState GetNetState() const { return PackState(); }
	void ReceiveExternalState(const FMRNetRideState& S) { PushSnapshot(S); }
	void SetDisplayName(const FString& InName) { DisplayName = InName; }
	/** Name for tags and maps: the server account name for server riders, else the player state's name. */
	FString GetRiderDisplayName() const;

protected:
	// ---- Components ----
	UPROPERTY(VisibleAnywhere) TObjectPtr<UBoxComponent> Collision;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> SpinPivot;  // ground point under box centre; air spins rotate here
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> BodyPivot;  // rear contact; wheelie pitch + lean

	UPROPERTY(VisibleAnywhere) TObjectPtr<USpringArmComponent> CameraBoom;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> ThirdPersonCamera;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> FirstPersonCamera;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UMRRideAudioComponent> Audio;

	UPROPERTY(Transient) TObjectPtr<USceneComponent> FrontWheel; // hub (tyre + spoke children)
	UPROPERTY(Transient) TObjectPtr<USceneComponent> RearWheel;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> SteerPivot;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> FrontLowers;   // fork lowers / caliper / mudguard: follow the front axle
	UPROPERTY(Transient) TObjectPtr<USceneComponent> SwingPivotComp; // swingarm: pitches so its end tracks the rear axle
	UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> VehicleMeshes;
	UPROPERTY(Transient) TObjectPtr<class USpotLightComponent> Headlight; // on at night (vehicles that have a front light)
	UPROPERTY() TObjectPtr<UMaterialInterface> SurfaceMaterial;      // /Game/MR/Materials/M_MRSurface (Color/Metallic/Roughness/Emissive)
	UPROPERTY() TObjectPtr<UMaterialInterface> RiderMaterial;        // M_MRRider: fabric / leather / hair / helmet patterns (falls back to SurfaceMaterial)
	UPROPERTY() TObjectPtr<UMaterialInterface> SkinMaterial;         // M_MRSkin: subsurface skin with pores
	FVector SwingRest = FVector::ZeroVector; // pivot -> rear axle at zero compression (body space)
	float BarHalfWidth = 22.f;

	// Rider rig: one mesh per body segment, posed every frame (see MRRiderBuilder.h).
	UPROPERTY(Transient) TArray<TObjectPtr<USceneComponent>> RiderSegs;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> CrankPivot;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> PedalComps[2];
	FMRRiderDims RiderDims;
	FVector RigSeat = FVector::ZeroVector, RigBars = FVector::ZeroVector, RigFoot = FVector::ZeroVector, RigHead = FVector::ZeroVector, RigBB = FVector::ZeroVector;
	float RigCrankL = 17.f, RigPedalY = 13.f, RigFootHalf = 14.f, RigGearRatio = 2.6f;
	bool bRigStanding = false;
	float CrankAngle = 0.f;      // rad; decreases as the rider pedals forwards
	float CrankSpeedPrev = 0.f, CrankAccel = 0.f;
	// Body movement (see UpdateRiderPose): weight shifts, foot down, standing, crash ragdoll.
	FMRRiderPose LastRiderPose;           // body space, last IK pose
	FMRRiderRagdoll Ragdoll;
	bool bRagdoll = false;
	float FootDownBlend = 0.f, StandBlend = 0.f, FootDownLean = 0.f;
	float BobPos = 0.f, BobVel = 0.f;      // rider vs bike vertical spring (cm), soaks up bumps and landings
	float VertVelSmooth = 0.f, VertAccel = 0.f, LongAccelSmooth = 0.f;
	FVector PrevActorLoc = FVector::ZeroVector, RiderVel = FVector::ZeroVector;
	bool bHavePrevLoc = false;
	bool bOnRoughGround = false;          // a wheel is on gravel / grass / mud / dirt / trail
	void UpdateRiderPose(float DeltaSeconds);
	void UpdateRagdoll(float DeltaSeconds);
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> VisualParts;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> PaintMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> JacketMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> HelmetMaterials;

	UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> CylinderMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> SphereMesh;
	UPROPERTY() TObjectPtr<UMaterialInterface> BaseMaterial;

	// ---- Replicated configuration ----
	UPROPERTY(ReplicatedUsing = OnRep_VehicleConfig) FName VehicleId;
	UPROPERTY(ReplicatedUsing = OnRep_VehicleConfig) FLinearColor PaintColor = FLinearColor(0.8f, 0.1f, 0.1f);
	UPROPERTY(ReplicatedUsing = OnRep_VehicleConfig) FLinearColor JacketColor = FLinearColor(0.05f, 0.1f, 0.4f);
	UPROPERTY(ReplicatedUsing = OnRep_VehicleConfig) FLinearColor HelmetColor = FLinearColor(0.9f, 0.9f, 0.9f);
	UPROPERTY(ReplicatedUsing = OnRep_VehicleConfig) FMRRiderLook RiderLook;
	UFUNCTION() void OnRep_VehicleConfig();

	// ---- Replicated ride state (server -> non-owners) ----
	UPROPERTY(ReplicatedUsing = OnRep_ReplicatedState) FMRNetRideState ReplicatedState;
	UFUNCTION() void OnRep_ReplicatedState();

	// ---- RPCs ----
	UFUNCTION(Server, Unreliable) void ServerSubmitState(const FMRNetRideState& State);
	UFUNCTION(Client, Reliable) void ClientTeleport(FVector Location, float YawDegrees, bool bResetMotion);
	UFUNCTION(Server, Reliable) void ServerRequestRespawn();
	UFUNCTION(Server, Reliable) void ServerTeleportTo(FVector Location, float YawDegrees);
	UFUNCTION(Server, Reliable) void ServerReportTrick(uint8 TrickType, int32 Points, float Distance, float Duration);

private:
	void ApplyVehicleConfig();
	void BuildVisuals();
	void ClearVisuals();
	void AddVehicleMesh(USceneComponent* Parent, const FMRPartMesh& Part);
	UStaticMeshComponent* AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& LocCm, const FRotator& Rot, const FVector& SizeCm, const FLinearColor& Color, int32 MaterialGroup);
	void UpdateMaterials();

	// Local simulation
	void SimulateLocal(float DeltaSeconds);
	void SimStep(float Dt);
	void ComputeContactPoints(FVector& OutRearContactLocal, FVector& OutFrontContactLocal) const; // cm, actor space
	MRSim::FWheelContact TraceWheel(const FVector& MountWorld, float RestLengthM, FVector& OutHitPoint, FVector& OutHitNormal) const;
	FVector ContactFrameOriginWorld() const;
	void HandleWallHit(const FHitResult& Hit);
	void ProcessTrickEvents();
	void UpdateSafeTransform(float DeltaSeconds);
	void DoRespawn(const FVector& Location, float YawDeg);

	// Networking
	FMRNetRideState PackState() const;
	void ApplyNetState(const FMRNetRideState& S, bool bSnapTransform);
	void PushSnapshot(const FMRNetRideState& S);
	void InterpolateRemote(float DeltaSeconds);
	bool ValidateClientState(const FMRNetRideState& S);

	// Presentation
	void UpdateVisualPose(float DeltaSeconds);
	void UpdateCamera(float DeltaSeconds);
	void UpdateAudio();
	void UpdateForceFeedback(float DeltaSeconds);

	MRSim::FVehicleParams Params;
	MRSim::FRideState Sim;
	MRSim::FTrickTracker Tricks;
	MRSim::FRiderInput Input;
	MRSim::FAssistSettings Assist;
	bool bConfigApplied = false;
	FName AppliedVehicleId;
	FString AppliedLookCode; // rider look the visuals were last built with
	float SimAccumulator = 0.f;
	int32 PendingGear = 0;
	float ContactToCenterUpCm = 0.f;  // rear contact -> box centre, along actor up
	float ContactToCenterFwdCm = 0.f; // along actor forward
	float Wetness = 0.f;
	float LastLandingImpact = 0.f;

	// Respawn / safety
	void KeepAboveGround();
	TWeakObjectPtr<AMRGeoWorld> GeoWorld;
	float GeoCheckTimer = 0.f;
	FVector SafeLocation = FVector::ZeroVector;
	float SafeYaw = 0.f;
	float SafeTimer = 0.f;
	float CrashRespawnTimer = 0.f;

	// Camera
	EMRCameraMode CameraMode = EMRCameraMode::ThirdPerson;
	FRotator LookOffset = FRotator::ZeroRotator;
	float LookIdleTime = 0.f;
	float CameraSensitivity = 1.f;
	float GarageOrbitYaw = 0.f;
	static inline float ShowcaseYaw = 90.f;

	// Display (shared by local and remote paths)
	float DisplayPitch = 0.f, DisplayLean = 0.f, DisplaySteer = 0.f, DisplayAirYaw = 0.f;
	float DisplayCompF = 0.f, DisplayCompR = 0.f, WheelSpin = 0.f;

	// Net send
	float SendTimer = 0.f;
	FMRServerMoveCheck ServerCheck;
	bool bServerAcceptNextTeleport = true;

	struct FSnapshot { FMRNetRideState State; float RecvTime = 0.f; };
	TArray<FSnapshot> Snapshots;
	float ClockOffset = 0.f;      // estimated (local time - sender time)
	bool bHaveClockOffset = false;
	FMRNetRideState LatestRemote;

	TArray<FMRTrickPopup> TrickPopups;
	float FeedbackCooldown = 0.f;
	FString DisplayName;
};
