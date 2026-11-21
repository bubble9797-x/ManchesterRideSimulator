#include "Core/MRPlayerController.h"

#include "ManchesterRide.h"
#include "Core/MRGameInstance.h"
#include "Core/MRGameMode.h"
#include "Core/MRGameState.h"
#include "Core/MRPlayerState.h"
#include "Core/MRSaveGame.h"
#include "Input/MRInputConfig.h"
#include "Net/MRNetworkSettings.h"
#include "Net/MRServerClient.h"
#include "Vehicles/MRRideVehicle.h"
#include "Vehicles/MRVehicleLibrary.h"
#include "World/MRGeoWorld.h"
#include "World/MRPhotoLayer.h"
#include "World/MRMapData.h"
#include "World/MRRouter.h"
#include "Async/Async.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GeoProjection.h"
#include "World/MRWorldEnvironment.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/LocalPlayer.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Kismet/KismetSystemLibrary.h"

#define LOCTEXT_NAMESPACE "MRMenu"

bool AMRPlayerController::bShownStartMenu = false;

namespace
{
	const FLinearColor Palette[] = {
		FLinearColor(0.75f, 0.06f, 0.06f), FLinearColor(0.04f, 0.10f, 0.40f), FLinearColor(0.05f, 0.35f, 0.08f),
		FLinearColor(0.85f, 0.65f, 0.02f), FLinearColor(0.90f, 0.30f, 0.02f), FLinearColor(0.90f, 0.90f, 0.90f),
		FLinearColor(0.02f, 0.02f, 0.02f), FLinearColor(0.30f, 0.30f, 0.32f), FLinearColor(0.30f, 0.05f, 0.45f),
		FLinearColor(0.02f, 0.40f, 0.40f), FLinearColor(0.85f, 0.25f, 0.45f), FLinearColor(0.45f, 0.85f, 0.05f),
	};
	const TCHAR* PaletteNames[] = { TEXT("Red"), TEXT("Navy"), TEXT("Racing green"), TEXT("Yellow"), TEXT("Orange"), TEXT("White"),
		TEXT("Black"), TEXT("Gunmetal"), TEXT("Purple"), TEXT("Teal"), TEXT("Pink"), TEXT("Lime") };
	constexpr int32 PaletteCount = UE_ARRAY_COUNT(Palette);

	int32 NearestPalette(const FLinearColor& C)
	{
		int32 Best = 0;
		float BestD = TNumericLimits<float>::Max();
		for (int32 i = 0; i < PaletteCount; ++i)
		{
			const float D = FVector(C.R - Palette[i].R, C.G - Palette[i].G, C.B - Palette[i].B).SizeSquared();
			if (D < BestD) { BestD = D; Best = i; }
		}
		return Best;
	}

	FString Paste()
	{
		FString S;
		FPlatformApplicationMisc::ClipboardPaste(S);
		return S.TrimStartAndEnd().Left(200);
	}

	/** Clipboard text without the 200-character cap (Cesium ion tokens are ~330 characters). */
	FString PasteLong()
	{
		FString S;
		FPlatformApplicationMisc::ClipboardPaste(S);
		S.ReplaceInline(TEXT("\r"), TEXT(""));
		S.ReplaceInline(TEXT("\n"), TEXT(""));
		return S.TrimStartAndEnd().Left(4096);
	}

	FText OnOff(bool b) { return b ? LOCTEXT("On", "On") : LOCTEXT("Off", "Off"); }

	FText AssistName(uint8 A)
	{
		switch (A)
		{
		case 0: return LOCTEXT("Casual", "Casual (balance assist, no loop-outs)");
		case 1: return LOCTEXT("Balanced", "Balanced");
		default: return LOCTEXT("Realistic", "Realistic (no assist)");
		}
	}

	FText VisName(EMRSessionVisibility V)
	{
		switch (V)
		{
		case EMRSessionVisibility::Public: return LOCTEXT("Public", "Public (anyone with the address)");
		case EMRSessionVisibility::Private: return LOCTEXT("Private", "Private (password)");
		default: return LOCTEXT("FriendsOnly", "Friends only (name list)");
		}
	}

	FText Num(float V, int32 Frac = 0)
	{
		FNumberFormattingOptions O;
		O.MinimumFractionalDigits = Frac;
		O.MaximumFractionalDigits = Frac;
		O.UseGrouping = false;
		return FText::AsNumber(V, &O);
	}
}

AMRPlayerController::AMRPlayerController()
{
	bShowMouseCursor = false;
}

UMRGameInstance* AMRPlayerController::GetMRGameInstance() const
{
	return GetGameInstance<UMRGameInstance>();
}

UMRSaveGame* AMRPlayerController::GetProfile() const
{
	const UMRGameInstance* GI = GetMRGameInstance();
	return GI ? GI->GetProfile() : nullptr;
}

AMRRideVehicle* AMRPlayerController::GetVehicle() const
{
	return Cast<AMRRideVehicle>(GetPawn());
}

void AMRPlayerController::SaveProfile()
{
	if (UMRGameInstance* GI = GetMRGameInstance()) GI->SaveProfile();
}

void AMRPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (!IsLocalController()) return;

	if (const UMRSaveGame* P = GetProfile())
	{
		UMRInputConfig::Get()->Rebuild(P->KeyBindings, P->bInvertLookY);
		GarageVehicle = P->VehicleId;
	}
	BindAllActions();
	ApplyInputContexts();
	FMRMapData::Get(); // start loading the county map (M) in the background
	// -ViewCam=lat,lon,heightM,yawDeg,pitchDeg: a fixed camera anywhere (landmark screenshots). Yaw 0 = north.
	FString ViewCam;
	if (FParse::Value(FCommandLine::Get(), TEXT("ViewCam="), ViewCam, false))
	{
		TArray<FString> P;
		ViewCam.ParseIntoArray(P, TEXT(","));
		if (P.Num() >= 5)
		{
			double X = 0, Y = 0;
			MRGeo::LocalToUnreal(MRGeo::ToLocal(MRGeo::FLatLon{ FCString::Atod(*P[0]), FCString::Atod(*P[1]) }), X, Y);
			float Gz = 0.f;
			AMRGeoWorld::GetGroundHeight(X, Y, Gz);
			const FVector Loc(X, Y, Gz + FCString::Atof(*P[2]) * 100.f);
			const FRotator Rot(FCString::Atof(*P[4]), FCString::Atof(*P[3]) - 90.f, 0.f); // compass yaw -> Unreal yaw (+X east, +Y south)
			FTimerHandle CamHandle;
			GetWorldTimerManager().SetTimer(CamHandle, FTimerDelegate::CreateWeakLambda(this, [this, Loc, Rot]()
			{
				FActorSpawnParameters SP;
				SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				if (ACameraActor* Cam = GetWorld()->SpawnActor<ACameraActor>(Loc, Rot, SP))
				{
					Cam->GetCameraComponent()->SetFieldOfView(70.f);
					SetViewTarget(Cam);
					if (AMRRideVehicle* V = GetVehicle()) V->SetActorLocation(Loc - FVector(0, 0, 200.f), false, nullptr, ETeleportType::TeleportPhysics); // stream the area
				}
			}), 2.f, false);
		}
	}
	float TestMapZoom = 0.f; // -MapZoom=<cm per pixel>: open the full map on start (automated screenshots)
	if (FParse::Value(FCommandLine::Get(), TEXT("MapZoom="), TestMapZoom))
	{
		FTimerHandle MapHandle;
		GetWorldTimerManager().SetTimer(MapHandle, FTimerDelegate::CreateWeakLambda(this, [this, TestMapZoom]()
		{
			OnOpenMap(FInputActionValue());
			MapCmPerPixel = FMath::Clamp(TestMapZoom, 40.f, 20000.f);
		}), 3.f, false);
	}
	FString TestWaypoint; // -Waypoint=<lat>,<lon>: set a waypoint on start (automated screenshots)
	if (FParse::Value(FCommandLine::Get(), TEXT("Waypoint="), TestWaypoint, false)) // false: keep the comma
	{
		FString A, B;
		if (TestWaypoint.Split(TEXT(","), &A, &B))
		{
			const float Lat = FCString::Atof(*A), Lon = FCString::Atof(*B);
			FTimerHandle WpHandle;
			GetWorldTimerManager().SetTimer(WpHandle, FTimerDelegate::CreateWeakLambda(this, [this, Lat, Lon]() { MRWaypoint(Lat, Lon); }), 2.f, false);
			if (FParse::Param(FCommandLine::Get(), TEXT("TeleportWaypoint"))) // testing: jump there once the road network has loaded
			{
				FTimerHandle TpHandle;
				GetWorldTimerManager().SetTimer(TpHandle, FTimerDelegate::CreateWeakLambda(this, [this]() { TeleportToWaypoint(); }), 6.f, false);
			}
		}
	}

	// Photoreal map from the saved profile (the world actors exist by now; give streaming a moment).
	// Host / solo only: friends who join get the host's choice through the game state.
	// -PhotoMap (testing): switch it on with the saved token without changing the profile.
	if (const UMRSaveGame* P = GetProfile(); HasAuthority() && P && (P->bPhotoMap || FParse::Param(FCommandLine::Get(), TEXT("PhotoMap"))) && !P->PhotoMapToken.IsEmpty() && AMRPhotoLayer::IsAvailable())
	{
		const FString Tok = P->PhotoMapToken;
		FTimerHandle PhotoHandle;
		GetWorldTimerManager().SetTimer(PhotoHandle, FTimerDelegate::CreateWeakLambda(this, [this, Tok]()
		{
			if (AMRGameState* GS = GetWorld()->GetGameState<AMRGameState>()) GS->SetPhotoMap(true, Tok);
		}), 1.5f, false);
	}

	// -Vehicle=<id> (automated screenshot runs): ride that vehicle without touching the saved profile.
	FString TestVehicle;
	if (HasAuthority() && FParse::Value(FCommandLine::Get(), TEXT("Vehicle="), TestVehicle))
	{
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(this, [this, TestVehicle]()
		{
			if (AMRGameMode* GM = GetWorld()->GetAuthGameMode<AMRGameMode>())
			{
				GM->ChangeVehicle(this, FName(*TestVehicle), FLinearColor(0.75f, 0.08f, 0.08f), FLinearColor(0.04f, 0.1f, 0.35f), FLinearColor(0.92f, 0.92f, 0.92f));
			}
		}), 1.f, false);
	}

	if (GetNetMode() == NM_Client)
	{
		if (UMRGameInstance* GI = GetMRGameInstance()) GI->NotifyConnectedToServer();
	}
	if (!bShownStartMenu && GetNetMode() == NM_Standalone)
	{
		bShownStartMenu = true;
		UMRServerClient* SC = GetServer();
		FString TestLogin; // automated tests: -ServerLogin=name:password [-ServerRegister]
		if (SC && FParse::Value(FCommandLine::Get(), TEXT("ServerLogin="), TestLogin, false) && TestLogin.Contains(TEXT(":")))
		{
			FString N, Pw;
			TestLogin.Split(TEXT(":"), &N, &Pw);
			SC->Connect(FParse::Param(FCommandLine::Get(), TEXT("ServerRegister")) ? EMRLoginMode::Register : EMRLoginMode::Password, N, Pw, true);
		}
		// -JoinServer (the installer passes it after an update): straight back onto the server with the saved login.
		else if (SC && FParse::Param(FCommandLine::Get(), TEXT("JoinServer")) && SC->HasSavedLogin())
		{
			SC->Connect(EMRLoginMode::Saved, FString(), FString());
			OpenMenu(EMRMenuScreen::Server);
		}
		// -NoMenu starts straight in free roam (automated screenshot/soak runs).
		else if (!FParse::Param(FCommandLine::Get(), TEXT("NoMenu"))) OpenMenu(EMRMenuScreen::Main);
	}
	else if (const UMRGameInstance* GI = GetMRGameInstance(); GI && GI->GetLastError() != EMRConnectError::None)
	{
		OpenMenu(EMRMenuScreen::Multiplayer); // came back from a failed/lost connection: show why
	}
}

void AMRPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	BindAllActions();
}

void AMRPlayerController::BindAllActions()
{
	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EIC || bInputBound) return;
	bInputBound = true;
	UMRInputConfig* C = UMRInputConfig::Get();
	EIC->BindAction(C->GearUp, ETriggerEvent::Started, this, &AMRPlayerController::OnGearUp);
	EIC->BindAction(C->GearDown, ETriggerEvent::Started, this, &AMRPlayerController::OnGearDown);
	EIC->BindAction(C->AssistUp, ETriggerEvent::Started, this, &AMRPlayerController::OnAssistUp);
	EIC->BindAction(C->AssistDown, ETriggerEvent::Started, this, &AMRPlayerController::OnAssistDown);
	EIC->BindAction(C->CameraToggle, ETriggerEvent::Started, this, &AMRPlayerController::OnCamera);
	EIC->BindAction(C->Respawn, ETriggerEvent::Started, this, &AMRPlayerController::OnRespawn);
	EIC->BindAction(C->OpenMenu, ETriggerEvent::Started, this, &AMRPlayerController::OnOpenMenu);
	EIC->BindAction(C->OpenMap, ETriggerEvent::Started, this, &AMRPlayerController::OnOpenMap);
	EIC->BindAction(C->MenuUp, ETriggerEvent::Started, this, &AMRPlayerController::OnMenuUp);
	EIC->BindAction(C->MenuDown, ETriggerEvent::Started, this, &AMRPlayerController::OnMenuDown);
	EIC->BindAction(C->MenuLeft, ETriggerEvent::Started, this, &AMRPlayerController::OnMenuLeft);
	EIC->BindAction(C->MenuRight, ETriggerEvent::Started, this, &AMRPlayerController::OnMenuRight);
	EIC->BindAction(C->MenuConfirm, ETriggerEvent::Started, this, &AMRPlayerController::OnMenuConfirm);
	EIC->BindAction(C->MenuBack, ETriggerEvent::Started, this, &AMRPlayerController::OnMenuBack);
}

void AMRPlayerController::ApplyInputContexts()
{
	UEnhancedInputLocalPlayerSubsystem* Sub = GetLocalPlayer() ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()) : nullptr;
	if (!Sub) return;
	Sub->ClearAllMappings();
	if (bChatOpen) return; // typing: no riding or menu keys at all
	UMRInputConfig* C = UMRInputConfig::Get();
	// The full map takes the menu context too, so riding keys (W/S/Esc...) don't fire underneath it.
	Sub->AddMappingContext((IsMenuOpen() || bMapOpen) ? C->MenuContext.Get() : C->RidingContext.Get(), 0);
}

// ---------------------------------------------------------------------------
// Full-screen map (M)
// ---------------------------------------------------------------------------

void AMRPlayerController::OnOpenMap(const FInputActionValue&)
{
	if (IsMenuOpen() || bMapOpen) return;
	bMapOpen = true;
	MapOpenedFrame = GFrameCounter; // the same key press must not also close it this frame
	MapCentre = GetVehicle() ? FVector2D(GetVehicle()->GetActorLocation()) : FVector2D::ZeroVector;
	if (MapCmPerPixel <= 0.f) MapCmPerPixel = 400.f; // 4 m per pixel: a few streets around you
	FMRMapData::Get();                                // start loading the county overview if needed
	FMRRouteGraph::Get();                             // and the road network for waypoints
	bShowMouseCursor = true;                          // click the map to set a waypoint
	ApplyInputContexts();
}

void AMRPlayerController::CloseMap()
{
	if (!bMapOpen) return;
	bMapOpen = false;
	bShowMouseCursor = false;
	ApplyInputContexts();
}

// ---------------------------------------------------------------------------
// Waypoint sat-nav
// ---------------------------------------------------------------------------

void AMRPlayerController::SetWaypoint(const FVector2D& P)
{
	bHasWaypoint = true;
	Waypoint = P;
	Route.Reset();
	RouteSeg = 0;
	bRouteFailed = false;
	bRouting = false;
	++RouteRequestId; // drop any route still being worked out for the old waypoint
	LastRouteRequest = -100.0;
	RequestRoute();
}

void AMRPlayerController::ClearWaypoint()
{
	bHasWaypoint = false;
	Route.Reset();
	bRouting = false;
	bRouteFailed = false;
	++RouteRequestId;
}

void AMRPlayerController::TeleportToWaypoint()
{
	AMRRideVehicle* V = GetVehicle();
	if (!V || !bHasWaypoint) return;
	// Land on the nearest road to the marker, facing along it (never inside a building).
	FVector2D Dest = Waypoint;
	float Yaw = V->GetActorRotation().Yaw;
	FMRRouteGraph::FSnap S;
	if (const TSharedPtr<const FMRRouteGraph> G = FMRRouteGraph::Get(); G.IsValid() && G->Snap(Waypoint, 30000.f, S))
	{
		const FMRRouteGraph::FEdge& E = G->Edges[S.Edge];
		const FVector2f Dir = G->Points[E.First + S.Seg + 1] - G->Points[E.First + S.Seg];
		Dest = FVector2D(S.P);
		if (!Dir.IsNearlyZero()) Yaw = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
	}
	UE_LOG(LogManchesterRide, Log, TEXT("Teleport to waypoint: (%.0f, %.0f)"), Dest.X, Dest.Y);
	V->TeleportRider(Dest, Yaw);
	ClearWaypoint();
	CloseMap();
}

void AMRPlayerController::MRWaypoint(float LatOrX, float LonOrY)
{
	// MRWaypoint <lat> <lon> (e.g. 53.4831 -2.2004 for the Etihad), or Unreal X Y in cm.
	if (FMath::Abs(LatOrX) <= 90.f && FMath::Abs(LonOrY) <= 180.f)
	{
		double X, Y;
		MRGeo::LocalToUnreal(MRGeo::ToLocal(MRGeo::FLatLon{ LatOrX, LonOrY }), X, Y);
		SetWaypoint(FVector2D(X, Y));
	}
	else SetWaypoint(FVector2D(LatOrX, LonOrY));
}

void AMRPlayerController::RequestRoute()
{
	const AMRRideVehicle* V = GetVehicle();
	if (!V || !bHasWaypoint || bRouting) return;
	const TSharedPtr<const FMRRouteGraph> G = FMRRouteGraph::Get();
	if (!G.IsValid()) return; // still loading: UpdateRoute asks again
	bRouting = true;
	LastRouteRequest = GetWorld()->GetTimeSeconds();
	const int32 Id = ++RouteRequestId;
	const FVector2D From(V->GetActorLocation()), To = Waypoint;
	TWeakObjectPtr<AMRPlayerController> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [G, From, To, Id, WeakThis]()
	{
		TArray<FVector2f> Pts;
		float Len = 0.f;
		const double T0 = FPlatformTime::Seconds();
		const bool bOk = G->FindRoute(From, To, Pts, Len);
		UE_LOG(LogManchesterRide, Log, TEXT("Route: %s, %.2f km, %d points (%.0f ms)"), bOk ? TEXT("found") : TEXT("none"), Len / 100000.f, Pts.Num(), (FPlatformTime::Seconds() - T0) * 1000.0);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Id, bOk, Pts = MoveTemp(Pts)]() mutable
		{
			AMRPlayerController* PC = WeakThis.Get();
			if (!PC || Id != PC->RouteRequestId) return; // waypoint changed meanwhile
			PC->bRouting = false;
			PC->bRouteFailed = !bOk;
			PC->Route = MoveTemp(Pts);
			PC->RouteSeg = 0;
		});
	});
}

void AMRPlayerController::UpdateRoute()
{
	if (!bHasWaypoint) return;
	const AMRRideVehicle* V = GetVehicle();
	if (!V) return;
	const float Now = GetWorld()->GetTimeSeconds();
	const FVector2D Me(V->GetActorLocation());
	auto Arrive = [&]() { ArrivedTime = Now; ClearWaypoint(); };
	if (FVector2D::Distance(Me, Waypoint) < 2500.f) { Arrive(); return; } // within 25 m

	if (Route.Num() < 2)
	{
		RouteRemainingCm = FVector2D::Distance(Me, Waypoint);
		if (!bRouting && Now - LastRouteRequest > 2.0) RequestRoute();
		return;
	}
	// Where along the route you are: search near the last position first, then the whole route.
	int32 Seg; float T;
	float D = MRDistanceToPolyline(Route, Me, RouteSeg - 2, RouteSeg + 60, Seg, T);
	if (D > 3000.f) D = MRDistanceToPolyline(Route, Me, 0, Route.Num(), Seg, T);
	RouteSeg = Seg;
	RouteHere = FMath::Lerp(Route[Seg], Route[Seg + 1], T);
	float Rem = FVector2f::Distance(RouteHere, Route[Seg + 1]);
	for (int32 i = Seg + 1; i + 1 < Route.Num(); ++i) Rem += FVector2f::Distance(Route[i], Route[i + 1]);
	RouteRemainingCm = Rem;
	if (Rem < 2500.f) { Arrive(); return; }
	// Off the route by more than 40 m (wrong turn, short cut, respawn): work out a new one.
	if (D > 4000.f && !bRouting && Now - LastRouteRequest > 3.0) RequestRoute();
}

void AMRPlayerController::UpdateMapInput(float DeltaTime)
{
	// Polled directly: smooth panning and zooming while keys are held.
	auto Down = [&](const FKey& K) { return IsInputKeyDown(K); };
	auto Pressed = [&](const FKey& K) { return WasInputKeyJustPressed(K); };
	const FKey MapKey = UMRInputConfig::Get()->GetKeyboardKey(TEXT("Map"));
	if (GFrameCounter != MapOpenedFrame && (Pressed(MapKey) || Pressed(EKeys::Escape) || Pressed(EKeys::Gamepad_FaceButton_Right) || Pressed(EKeys::Gamepad_RightThumbstick)))
	{
		CloseMap();
		return;
	}
	FVector2D Pan(0, 0);
	if (Down(EKeys::W) || Down(EKeys::Up)) Pan.Y -= 1.f;
	if (Down(EKeys::S) || Down(EKeys::Down)) Pan.Y += 1.f;
	if (Down(EKeys::A) || Down(EKeys::Left)) Pan.X -= 1.f;
	if (Down(EKeys::D) || Down(EKeys::Right)) Pan.X += 1.f;
	Pan.X += GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
	Pan.Y -= GetInputAnalogKeyState(EKeys::Gamepad_LeftY);
	const float Boost = (Down(EKeys::LeftShift) || Down(EKeys::Gamepad_LeftTrigger)) ? 3.f : 1.f;
	MapCentre += Pan * (MapCmPerPixel * 700.f * Boost * DeltaTime); // ~700 screen px per second

	float Zoom = 0.f;
	if (Pressed(EKeys::MouseScrollUp)) Zoom -= 0.25f;
	if (Pressed(EKeys::MouseScrollDown)) Zoom += 0.25f;
	if (Down(EKeys::E) || Down(EKeys::PageUp) || Down(EKeys::Add) || Down(EKeys::Gamepad_RightShoulder)) Zoom -= 1.5f * DeltaTime;
	if (Down(EKeys::Q) || Down(EKeys::PageDown) || Down(EKeys::Subtract) || Down(EKeys::Gamepad_LeftShoulder)) Zoom += 1.5f * DeltaTime;
	if (Zoom != 0.f) MapCmPerPixel = FMath::Clamp(MapCmPerPixel * FMath::Exp(Zoom), 40.f, 20000.f); // 0.4 m/px to 200 m/px
	if (Pressed(EKeys::SpaceBar) || Pressed(EKeys::Gamepad_FaceButton_Bottom))
	{
		if (const AMRRideVehicle* V = GetVehicle()) MapCentre = FVector2D(V->GetActorLocation());
	}
	// Waypoints: click where you want to go (or Enter / X for the crosshair in the middle).
	if (Pressed(EKeys::LeftMouseButton))
	{
		float Mx = 0.f, My = 0.f;
		int32 Vw = 0, Vh = 0;
		if (GetMousePosition(Mx, My))
		{
			GetViewportSize(Vw, Vh);
			SetWaypoint(MapCentre + (FVector2D(Mx, My) - FVector2D(Vw, Vh) * 0.5) * MapCmPerPixel);
		}
	}
	if (Pressed(EKeys::Enter) || Pressed(EKeys::Gamepad_FaceButton_Left)) SetWaypoint(MapCentre);
	if (Pressed(EKeys::T) || Pressed(EKeys::Gamepad_LeftThumbstick)) { TeleportToWaypoint(); return; }
	if (Pressed(EKeys::RightMouseButton) || Pressed(EKeys::BackSpace) || Pressed(EKeys::Delete) || Pressed(EKeys::Gamepad_FaceButton_Top)) ClearWaypoint();
}

// ---------------------------------------------------------------------------
// Riding input
// ---------------------------------------------------------------------------

void AMRPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	if (!IsLocalController()) return;

	AMRRideVehicle* V = GetVehicle();
	if (V != LastVehicle.Get())
	{
		LastVehicle = V;
		ApplyProfileToVehicle();
	}
	if (bMapOpen)
	{
		if (IsMenuOpen()) CloseMap();
		else UpdateMapInput(DeltaTime);
	}
	UpdateRoute();
	UpdateRidingInput(DeltaTime);

	if (UMRServerClient* SC = GetServer())
	{
		if (SC->ConsumeJustJoined() && (Screen == EMRMenuScreen::Server || Screen == EMRMenuScreen::ServerLogin || Screen == EMRMenuScreen::ServerRegister))
		{
			LoginPassword.Reset(); RegPassword.Reset(); RegPassword2.Reset();
			CloseMenu(); // on the server: ride
		}
		if (bChatOpen && !SC->IsOnline()) CloseChat();
		// Keep the server screens' status (connecting, download progress, errors) current.
		const double Now = FPlatformTime::Seconds();
		if ((Screen == EMRMenuScreen::Server || Screen == EMRMenuScreen::ServerLogin || Screen == EMRMenuScreen::ServerRegister) && Now - LastMenuRefresh > 0.25)
		{
			LastMenuRefresh = Now;
			RebuildMenu();
		}
	}
}

UMRServerClient* AMRPlayerController::GetServer() const { return UMRServerClient::Get(this); }

bool AMRPlayerController::IsEditingText() const
{
	return IsMenuOpen() && !bAwaitingKey && Items.IsValidIndex(Selected) && Items[Selected].EditText != nullptr;
}

void AMRPlayerController::OpenChat(const FString& Prefill)
{
	bChatOpen = true;
	ChatDraft = Prefill;
	if (UMRServerClient* SC = GetServer()) SC->SetPushToTalk(false);
	ApplyInputContexts();
}

void AMRPlayerController::CloseChat()
{
	if (!bChatOpen) return;
	bChatOpen = false;
	ChatDraft.Reset();
	SkipChar = 0;
	ApplyInputContexts();
}

bool AMRPlayerController::HandleTextChar(TCHAR C)
{
	if (SkipChar != 0)
	{
		const TCHAR Skip = SkipChar;
		SkipChar = 0;
		if (FChar::ToLower(C) == Skip) return true;
	}
	const bool bTyping = bChatOpen || IsEditingText();
	if (!bTyping) return false;
	if (C < 32 || C == 127) return true; // Enter, Backspace, Ctrl+V etc. arrive as keys (InputKey)
	if (bChatOpen)
	{
		if (ChatDraft.Len() < 200) ChatDraft.AppendChar(C);
		return true;
	}
	FMRMenuItem& I = Items[Selected];
	if (I.EditText->Len() < I.MaxLen) I.EditText->AppendChar(C);
	LocalFormError = FText::GetEmpty();
	return true;
}

void AMRPlayerController::ApplyProfileToVehicle()
{
	AMRRideVehicle* V = GetVehicle();
	const UMRSaveGame* P = GetProfile();
	if (!V || !P) return;
	MRSim::FAssistSettings A;
	A.WheelieAssist = static_cast<MRSim::EWheelieAssist>(FMath::Clamp<int32>(P->WheelieAssist, 0, 2));
	A.bAutoClutch = P->bAutoClutch;
	A.bAutoShift = P->bAutoShift;
	A.bLandingAssist = P->bLandingAssist;
	A.bABS = P->bABS;
	A.SteerSensitivity = P->SteerSensitivity;
	V->SetAssistSettings(A);
	V->SetCameraSensitivity(P->CameraSensitivity);
	V->SetCameraMode((Screen == EMRMenuScreen::Garage || Screen == EMRMenuScreen::Rider) ? EMRCameraMode::GaragePreview : (P->bFirstPerson ? EMRCameraMode::FirstPerson : EMRCameraMode::ThirdPerson));
	SetViewTarget(V);
}

void AMRPlayerController::UpdateRidingInput(float DeltaTime)
{
	AMRRideVehicle* V = GetVehicle();
	UEnhancedPlayerInput* EIn = Cast<UEnhancedPlayerInput>(PlayerInput);
	const UMRSaveGame* P = GetProfile();
	if (!V || !EIn || !P) return;

	MRSim::FRiderInput In;
	if (!IsMenuOpen() && !bChatOpen)
	{
		const UMRInputConfig* C = UMRInputConfig::Get();
		auto Axis = [&](const UInputAction* A) { return EIn->GetActionValue(A).Get<float>(); };
		auto Button = [&](const UInputAction* A) { return EIn->GetActionValue(A).Get<bool>(); };

		In.Throttle = FMath::Clamp(Axis(C->Throttle), 0.f, 1.f);
		In.FrontBrake = FMath::Clamp(Axis(C->FrontBrake) * P->BrakeSensitivity, 0.f, 1.f);
		// S/brake brakes while rolling forwards; once stopped (and not pedalling) holding it reverses.
		// Latched so it keeps reversing through zero instead of flicking back to braking.
		const float Fwd = V->GetSpeedMps();
		if (In.FrontBrake > 0.3f && In.Throttle < 0.05f && (Fwd < 0.4f || bReversing))
		{
			bReversing = true;
			In.Reverse = FMath::Clamp(Axis(C->FrontBrake), 0.f, 1.f);
			In.FrontBrake = 0.f;
		}
		else
		{
			bReversing = false;
		}
		In.RearBrake = FMath::Clamp(Axis(C->RearBrake) * P->BrakeSensitivity, 0.f, 1.f);
		// Progressive steering curve gives fine control near centre on a stick; keyboard
		// input (exactly +-1) is ramped so tapping A/D doesn't snap the bars.
		const float RawSteer = FMath::Clamp(Axis(C->Steer), -1.f, 1.f);
		const float Curved = FMath::Sign(RawSteer) * FMath::Pow(FMath::Abs(RawSteer), 1.5f);
		SmoothedSteer = FMath::FInterpConstantTo(SmoothedSteer, Curved, DeltaTime, 5.f);
		In.Steer = SmoothedSteer;
		In.Balance = FMath::Clamp(Axis(C->Balance), -1.f, 1.f);
		In.Clutch = FMath::Clamp(Axis(C->Clutch), 0.f, 1.f);
		In.bWheelie = Button(C->Wheelie);
		In.bHop = Button(C->Hop);
		bPlayerListHeld = Button(C->PlayerList);

		const FVector2D Mouse = EIn->GetActionValue(C->Look).Get<FVector2D>();
		const FVector2D Pad = EIn->GetActionValue(C->LookPad).Get<FVector2D>();
		V->AddLookInput(FVector2D(Mouse.X * 0.25f, Mouse.Y * 0.25f) + FVector2D(Pad.X, Pad.Y) * 140.f * DeltaTime);
	}
	else
	{
		bPlayerListHeld = false;
		SmoothedSteer = 0.f;
	}
	In.AssistLevel = P->AssistLevel;
	V->SetRiderInput(In);
}

void AMRPlayerController::OnGearUp(const FInputActionValue&) { if (AMRRideVehicle* V = GetVehicle()) V->RequestGearChange(+1); }
void AMRPlayerController::OnGearDown(const FInputActionValue&) { if (AMRRideVehicle* V = GetVehicle()) V->RequestGearChange(-1); }
void AMRPlayerController::OnAssistUp(const FInputActionValue&) { if (UMRSaveGame* P = GetProfile()) P->AssistLevel = FMath::Min(3, P->AssistLevel + 1); }
void AMRPlayerController::OnAssistDown(const FInputActionValue&) { if (UMRSaveGame* P = GetProfile()) P->AssistLevel = FMath::Max(0, P->AssistLevel - 1); }
void AMRPlayerController::OnRespawn(const FInputActionValue&) { if (AMRRideVehicle* V = GetVehicle()) V->RequestRespawn(); }
void AMRPlayerController::OnOpenMenu(const FInputActionValue&) { if (!IsMenuOpen()) OpenMenu(EMRMenuScreen::Main); }

void AMRPlayerController::OnCamera(const FInputActionValue&)
{
	if (AMRRideVehicle* V = GetVehicle())
	{
		V->CycleCameraMode();
		if (UMRSaveGame* P = GetProfile()) P->bFirstPerson = V->GetCameraMode() == EMRCameraMode::FirstPerson;
	}
}

// ---------------------------------------------------------------------------
// Key capture for rebinding
// ---------------------------------------------------------------------------

bool AMRPlayerController::InputKey(const FInputKeyParams& Params)
{
	if (bAwaitingKey && Params.Event == IE_Pressed && !Params.Key.IsMouseButton() && !Params.Key.IsAxis2D())
	{
		bAwaitingKey = false;
		if (Params.Key != EKeys::Escape && Params.Key != EKeys::Gamepad_Special_Right)
		{
			if (UMRSaveGame* P = GetProfile())
			{
				FMRKeyBinding* B = P->KeyBindings.FindByPredicate([&](const FMRKeyBinding& K) { return K.Action == RebindAction; });
				if (!B) { B = &P->KeyBindings.AddDefaulted_GetRef(); B->Action = RebindAction; }
				(Params.Key.IsGamepadKey() ? B->Gamepad : B->Keyboard) = Params.Key;
				UMRInputConfig::Get()->Rebuild(P->KeyBindings, P->bInvertLookY);
				SaveProfile();
			}
		}
		ApplyInputContexts();
		RebuildMenu();
		return true; // swallow the key so it doesn't also trigger an action
	}

	const FKey& K = Params.Key;
	const bool bDown = Params.Event == IE_Pressed || Params.Event == IE_Repeat;
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	auto PasteLine = []() { FString S; FPlatformApplicationMisc::ClipboardPaste(S); int32 Nl; if (S.FindChar(TEXT('\n'), Nl)) S.LeftInline(Nl); S.ReplaceInline(TEXT("\r"), TEXT("")); return S; };
	UMRServerClient* SC = GetServer();

	// Chat box open: every key goes to the text (riding keys are off while you type).
	if (bChatOpen)
	{
		if (bDown && !K.IsGamepadKey() && !K.IsMouseButton())
		{
			if (K == EKeys::Enter) { if (SC) SC->SendChat(ChatDraft); CloseChat(); }
			else if (K == EKeys::Escape) CloseChat();
			else if (K == EKeys::BackSpace) ChatDraft.LeftChopInline(1, EAllowShrinking::No);
			else if (K == EKeys::V && bCtrl) ChatDraft = (ChatDraft + PasteLine()).Left(200);
		}
		else if (bDown && (K == EKeys::Gamepad_FaceButton_Right || K == EKeys::Gamepad_Special_Right)) CloseChat();
		return true;
	}

	// A text box is selected in the menu: letters, space and Backspace type instead of moving the menu.
	if (IsEditingText())
	{
		const bool bNav = K == EKeys::Up || K == EKeys::Down || K == EKeys::Enter || K == EKeys::Escape || K.IsGamepadKey() || K.IsMouseButton();
		if (K == EKeys::Tab)
		{
			if (Params.Event == IE_Pressed) MoveSelection((IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift)) ? -1 : +1);
			return true;
		}
		if (!bNav)
		{
			if (bDown)
			{
				FMRMenuItem& I = Items[Selected];
				if (K == EKeys::BackSpace) I.EditText->LeftChopInline(1, EAllowShrinking::No);
				else if (K == EKeys::V && bCtrl) *I.EditText = (*I.EditText + PasteLine().TrimStartAndEnd()).Left(I.MaxLen);
				else if (K == EKeys::Delete) I.EditText->Reset();
			}
			return true;
		}
	}

	if (SC && SC->IsOnline() && !IsMenuOpen() && !bMapOpen && Params.Event == IE_Pressed)
	{
		if (K == EKeys::T || K == EKeys::Enter) { OpenChat(FString()); SkipChar = K == EKeys::T ? TEXT('t') : 0; return true; }
		if (K == EKeys::Slash) { OpenChat(TEXT("/")); SkipChar = TEXT('/'); return true; }
	}
	// Push-to-talk: hold V.
	if (SC && K == EKeys::V)
	{
		if (Params.Event == IE_Pressed) SC->SetPushToTalk(!IsMenuOpen());
		else if (Params.Event == IE_Released) SC->SetPushToTalk(false);
	}
	return Super::InputKey(Params);
}

// ---------------------------------------------------------------------------
// Menu system (rendered by AMRHUD on the canvas; works with pad or keyboard)
// ---------------------------------------------------------------------------

void AMRPlayerController::OpenMenu(EMRMenuScreen NewScreen)
{
	if (NewScreen == EMRMenuScreen::None) { CloseMenu(); return; }
	if (Screen != EMRMenuScreen::None && Screen != NewScreen) ScreenStack.Push(Screen);
	if (NewScreen == EMRMenuScreen::Garage && GetProfile()) GarageVehicle = GetProfile()->VehicleId;
	Screen = NewScreen;
	Selected = 0;
	RebuildMenu();
	ApplyInputContexts();
	if (AMRRideVehicle* V = GetVehicle())
	{
		if (Screen == EMRMenuScreen::Garage || Screen == EMRMenuScreen::Rider) V->SetCameraMode(EMRCameraMode::GaragePreview);
		else if (V->GetCameraMode() == EMRCameraMode::GaragePreview) ApplyProfileToVehicle();
	}
}

void AMRPlayerController::CloseMenu()
{
	Screen = EMRMenuScreen::None;
	ScreenStack.Reset();
	Items.Reset();
	bAwaitingKey = false;
	SaveProfile();
	ApplyInputContexts();
	ApplyProfileToVehicle();
}

void AMRPlayerController::OnMenuBack(const FInputActionValue&)
{
	if (!IsMenuOpen()) return;
	if (bAwaitingKey) return; // handled in InputKey
	if (ScreenStack.Num() > 0)
	{
		const EMRMenuScreen Prev = ScreenStack.Pop();
		Screen = EMRMenuScreen::None; // so OpenMenu doesn't push again
		OpenMenu(Prev);
	}
	else
	{
		CloseMenu();
	}
}

void AMRPlayerController::MoveSelection(int32 Dir)
{
	if (Items.Num() == 0) return;
	for (int32 Tries = 0; Tries < Items.Num(); ++Tries)
	{
		Selected = (Selected + Dir + Items.Num()) % Items.Num();
		if (Items[Selected].bEnabled) return;
	}
}

void AMRPlayerController::OnMenuUp(const FInputActionValue&) { if (IsMenuOpen() && !bAwaitingKey) MoveSelection(-1); }
void AMRPlayerController::OnMenuDown(const FInputActionValue&) { if (IsMenuOpen() && !bAwaitingKey) MoveSelection(+1); }

void AMRPlayerController::OnMenuLeft(const FInputActionValue&)
{
	if (!IsMenuOpen() || bAwaitingKey || !Items.IsValidIndex(Selected)) return;
	if (Items[Selected].OnAdjust) { Items[Selected].OnAdjust(-1); RebuildMenu(); }
}

void AMRPlayerController::OnMenuRight(const FInputActionValue&)
{
	if (!IsMenuOpen() || bAwaitingKey || !Items.IsValidIndex(Selected)) return;
	if (Items[Selected].OnAdjust) { Items[Selected].OnAdjust(+1); RebuildMenu(); }
}

void AMRPlayerController::OnMenuConfirm(const FInputActionValue&)
{
	if (!IsMenuOpen() || bAwaitingKey || !Items.IsValidIndex(Selected)) return;
	const EMRMenuScreen Before = Screen;
	if (Items[Selected].OnConfirm) Items[Selected].OnConfirm();
	else if (Items[Selected].OnAdjust) Items[Selected].OnAdjust(+1);
	if (IsMenuOpen() && Screen == Before) RebuildMenu();
}

void AMRPlayerController::ApplyCustomisation(bool bRespawnVehicle)
{
	UMRSaveGame* P = GetProfile();
	if (!P) return;
	P->PaintPerVehicle.Add(P->VehicleId, P->PaintColor);
	SaveProfile();
	if (bRespawnVehicle)
	{
		ServerChangeVehicle(P->VehicleId, P->PaintColor, P->JacketColor, P->HelmetColor);
	}
}

void AMRPlayerController::ServerChangeVehicle_Implementation(FName VehicleId, FLinearColor Paint, FLinearColor Jacket, FLinearColor Helmet)
{
	if (AMRGameMode* GM = GetWorld()->GetAuthGameMode<AMRGameMode>())
	{
		GM->ChangeVehicle(this, VehicleId, Paint, Jacket, Helmet);
	}
}

void AMRPlayerController::ServerSetRiderLook_Implementation(FMRRiderLook Look, FLinearColor Jacket, FLinearColor Helmet)
{
	Look.Sanitise();
	if (AMRPlayerState* PS = GetPlayerState<AMRPlayerState>())
	{
		PS->RiderLook = Look;
		PS->JacketColor = Jacket;
		PS->HelmetColor = Helmet;
	}
	if (AMRRideVehicle* V = GetVehicle())
	{
		V->PreviewColours(V->GetPaintColor(), Jacket, Helmet);
		V->SetRiderLook(Look); // both replicate to everyone; no respawn needed
	}
}

void AMRPlayerController::ServerStartCompetition_Implementation(EMRCompetition Type, float Seconds)
{
	// Only the host (the local controller on a listen server) may start competitions.
	if (!IsLocalController()) return;
	if (AMRGameMode* GM = GetWorld()->GetAuthGameMode<AMRGameMode>())
	{
		GM->StartCompetition(Type, FMath::Clamp(Seconds, 30.f, 1800.f));
	}
}

void AMRPlayerController::ServerSetEnvironment_Implementation(int32 Weather, float Hours)
{
	if (!IsLocalController()) return; // host only
	if (AMRWorldEnvironment* Env = AMRWorldEnvironment::Get(this))
	{
		if (Weather >= 0) Env->SetWeather(static_cast<EMRWeather>(FMath::Clamp(Weather, 0, 5)), true);
		if (Hours >= 0.f) Env->SetTimeOfDay(Hours);
	}
}

template <typename FAddFn, typename FInfoFn, typename FGoFn>
void AMRPlayerController::BuildServerMenu(FAddFn& Add, FInfoFn& Info, FGoFn& Go)
{
	UMRServerClient* SC = GetServer();
	UMRSaveGame* P = GetProfile();
	UMRGameInstance* GI = GetMRGameInstance();
	if (!SC || !P || !GI) return;
	const EMRServerState St = SC->GetState();
	const bool bBusy = SC->IsBusy();
	auto Field = [&](const FText& Label, FString& Target, bool bPw, int32 MaxLen, const FText& Hint)
	{
		FMRMenuItem& I = Add(Label, Hint, [this]() { MoveSelection(+1); });
		I.EditText = &Target;
		I.bPassword = bPw;
		I.MaxLen = MaxLen;
	};
	auto VoiceSettings = [&]()
	{
		static const FText Modes[] = { LOCTEXT("VOff", "Off"), LOCTEXT("VPtt", "Push to talk (hold V)"), LOCTEXT("VOpen", "Open mic") };
		Add(LOCTEXT("Voice", "Voice chat"), Modes[FMath::Min<int32>(P->VoiceMode, 2)], nullptr,
			[this, P, SC](int32 D) { P->VoiceMode = uint8((P->VoiceMode + D + 3) % 3); SC->ApplyVoiceSettings(); SaveProfile(); });
		Add(LOCTEXT("VRange", "Hear voices from"), P->VoiceRange == 0 ? LOCTEXT("VAll", "Everyone on the server") : LOCTEXT("VNear", "Riders near you (150 m)"), nullptr,
			[this, P, SC](int32) { P->VoiceRange = P->VoiceRange ? 0 : 1; SC->ApplyVoiceSettings(); SaveProfile(); });
		Add(LOCTEXT("VVol", "Voice volume"), FText::AsPercent(P->VoiceVolume), nullptr,
			[this, P, SC](int32 D) { P->VoiceVolume = FMath::Clamp(P->VoiceVolume + D * 0.1f, 0.f, 2.f); SC->ApplyVoiceSettings(); SaveProfile(); });
		Add(LOCTEXT("MVol", "Microphone volume"), FText::AsPercent(P->MicVolume), nullptr,
			[this, P](int32 D) { P->MicVolume = FMath::Clamp(P->MicVolume + D * 0.1f, 0.f, 3.f); SaveProfile(); });
	};
	auto Status = [&]()
	{
		if (bBusy || St == EMRServerState::Offline)
		{
			if (!SC->GetStatusText().IsEmpty()) Info(SC->GetStatusText());
		}
	};

	if (Screen == EMRMenuScreen::ServerLogin)
	{
		Title = LOCTEXT("TitleLogin", "LOG IN - MANCHESTER RIDE SERVER");
		Field(LOCTEXT("LName", "Name"), LoginName, false, 16, LOCTEXT("LNameHint", "type your name"));
		Field(LOCTEXT("LPw", "Password"), LoginPassword, true, 64, LOCTEXT("LPwHint", "type your password"));
		Add(LOCTEXT("Remember", "Stay logged in on this PC"), OnOff(bRememberLogin), nullptr, [this](int32) { bRememberLogin = !bRememberLogin; });
		if (bBusy) Add(LOCTEXT("Cancel", "Cancel"), FText::GetEmpty(), [SC]() { SC->Disconnect(); });
		else Add(LOCTEXT("DoLogin", "Log in and join"), FText::GetEmpty(), [this, SC]()
		{
			LocalFormError = FText::GetEmpty();
			if (LoginName.TrimStartAndEnd().IsEmpty() || LoginPassword.IsEmpty()) { LocalFormError = LOCTEXT("NeedBoth", "Type your name and password first."); return; }
			SC->Connect(EMRLoginMode::Password, LoginName, LoginPassword, bRememberLogin);
		});
		Add(LOCTEXT("ToReg", "No account yet? Create one"), FText::GetEmpty(), [this]() { RegName = LoginName; Screen = EMRMenuScreen::Server; OpenMenu(EMRMenuScreen::ServerRegister); });
		Status();
		Footer = LOCTEXT("LoginFooter", "Type in the selected box   Up/Down or Tab: next box   Enter: confirm   Ctrl+V: paste   Esc: back");
		return;
	}
	if (Screen == EMRMenuScreen::ServerRegister)
	{
		Title = LOCTEXT("TitleReg", "CREATE AN ACCOUNT - MANCHESTER RIDE SERVER");
		Field(LOCTEXT("RName", "Name"), RegName, false, 16, LOCTEXT("RNameHint", "3 to 16 letters, numbers or _"));
		Field(LOCTEXT("RPw", "Password"), RegPassword, true, 64, LOCTEXT("RPwHint", "at least 6 characters"));
		Field(LOCTEXT("RPw2", "Password again"), RegPassword2, true, 64, LOCTEXT("RPw2Hint", "the same again"));
		Add(LOCTEXT("Remember2", "Stay logged in on this PC"), OnOff(bRememberLogin), nullptr, [this](int32) { bRememberLogin = !bRememberLogin; });
		if (bBusy) Add(LOCTEXT("Cancel2", "Cancel"), FText::GetEmpty(), [SC]() { SC->Disconnect(); });
		else Add(LOCTEXT("DoReg", "Create account and join"), FText::GetEmpty(), [this, SC]()
		{
			LocalFormError = FText::GetEmpty();
			const FString N = RegName.TrimStartAndEnd();
			bool bNameOk = N.Len() >= 3 && N.Len() <= 16;
			for (TCHAR Ch : N) bNameOk = bNameOk && ((FChar::IsAlnum(Ch) && Ch < 128) || Ch == TEXT('_'));
			if (!bNameOk) LocalFormError = LOCTEXT("BadName", "Names are 3 to 16 letters, numbers or _ (no spaces).");
			else if (RegPassword.Len() < 6) LocalFormError = LOCTEXT("ShortPw", "Passwords need at least 6 characters.");
			else if (RegPassword != RegPassword2) LocalFormError = LOCTEXT("PwMismatch", "The two passwords are different. Type them again.");
			else SC->Connect(EMRLoginMode::Register, N, RegPassword, bRememberLogin);
		});
		Info(LOCTEXT("PwAdvice", "Use a password you don't use anywhere else. The server only keeps a scrambled (hashed) copy of it, and the connection is encrypted."));
		Status();
		Footer = LOCTEXT("RegFooter", "Type in the selected box   Up/Down or Tab: next box   Enter: confirm   Esc: back");
		return;
	}

	// The server page
	Title = LOCTEXT("TitleServer", "MULTIPLAYER - MANCHESTER RIDE SERVER");
	switch (St)
	{
	case EMRServerState::Online:
	{
		Info(LOCTEXT("SName", "Server"), FText::FromString(SC->GetServerName()));
		Info(LOCTEXT("SYou", "Logged in as"), FText::Format(LOCTEXT("SYouFmt", "{0}{1}"), FText::FromString(SC->GetMyName()), SC->IsOp() ? LOCTEXT("SOp", "  (operator: /help for commands)") : FText::GetEmpty()));
		Info(LOCTEXT("SRiders", "Riders online"), FText::Format(LOCTEXT("SRidersFmt", "{0} / {1}     ping {2} ms"), SC->GetRiders().Num() + 1, SC->GetMaxPlayers(), SC->GetPingMs()));
		Add(LOCTEXT("SChat", "Chat (or press T while riding)"), FText::GetEmpty(), [this]() { CloseMenu(); OpenChat(FString()); });
		VoiceSettings();
		Add(LOCTEXT("SLeave", "Leave the server"), FText::GetEmpty(), [SC]() { SC->Disconnect(); });
		Add(LOCTEXT("SLogout", "Log out (forget my login on this PC)"), FText::GetEmpty(), [SC]() { SC->ForgetSavedLogin(); SC->Disconnect(); });
		break;
	}
	case EMRServerState::Updating:
	{
		const double Mb = SC->GetUpdateReceived() / 1048576.0, Total = SC->GetUpdateSize() / 1048576.0;
		Info(FText::Format(LOCTEXT("UpdTitle", "The server has a new game version ({0}). Downloading it now:"), FText::FromString(SC->GetUpdateVersion())));
		Info(LOCTEXT("UpdProg", "Downloaded"), SC->IsVerifyingUpdate() ? LOCTEXT("UpdCheck", "checking...")
			: FText::Format(LOCTEXT("UpdProgFmt", "{0} / {1} MB   ({2} MB/s)"), FText::AsNumber(FMath::RoundToInt(Mb)), FText::AsNumber(FMath::RoundToInt(Total)), FText::AsNumber(FMath::RoundToInt(SC->GetUpdateRate() / 104857.6) / 10.0)));
		Info(LOCTEXT("UpdNote", "When it's done the game closes, updates itself and comes back on the server."));
		Add(LOCTEXT("UpdCancel", "Cancel"), FText::GetEmpty(), [SC]() { SC->Disconnect(); });
		break;
	}
	case EMRServerState::Connecting:
	case EMRServerState::LoggingIn:
		Info(SC->GetStatusText());
		Add(LOCTEXT("SCancel", "Cancel"), FText::GetEmpty(), [SC]() { SC->Disconnect(); });
		break;
	default:
	{
		Info(LOCTEXT("SAddr", "Server"), FText::FromString(SC->GetAddress()));
		if (GI->IsInMultiplayer())
		{
			Info(LOCTEXT("SInSession", "You're in a hosted / joined session. Leave it first (Other ways to play)."));
		}
		else
		{
			if (SC->IsKeyMismatch())
			{
				Add(LOCTEXT("STrust", "Trust the server's new key and join"), FText::GetEmpty(), [this, SC]()
				{
					SC->TrustNewServerKey();
					if (SC->HasSavedLogin()) SC->Connect(EMRLoginMode::Saved, FString(), FString());
					else OpenMenu(EMRMenuScreen::ServerLogin);
				});
			}
			if (SC->HasSavedLogin())
			{
				Add(FText::Format(LOCTEXT("SJoinAs", "Join as {0}"), FText::FromString(SC->GetSavedUser())), FText::GetEmpty(), [SC]() { SC->Connect(EMRLoginMode::Saved, FString(), FString()); });
				Add(LOCTEXT("SOther", "Log in as someone else"), FText::GetEmpty(), Go(EMRMenuScreen::ServerLogin));
				Add(FText::Format(LOCTEXT("SForget", "Log out {0} on this PC"), FText::FromString(SC->GetSavedUser())), FText::GetEmpty(), [SC]() { SC->ForgetSavedLogin(); });
			}
			else
			{
				Add(LOCTEXT("SLogin", "Log in"), FText::GetEmpty(), Go(EMRMenuScreen::ServerLogin));
				Add(LOCTEXT("SReg", "Create an account (first time here)"), FText::GetEmpty(), Go(EMRMenuScreen::ServerRegister));
			}
		}
		VoiceSettings();
		Status();
		break;
	}
	}
	Add(LOCTEXT("SOtherWays", "Other ways to play (host your own, join by address)"), FText::GetEmpty(), Go(EMRMenuScreen::Multiplayer));
	Footer = LOCTEXT("ServerFooter", "Riding on the server: T chat   hold V talk   Tab riders   operators: /help");
}

void AMRPlayerController::RebuildMenu()
{
	Items.Reset();
	Footer = LOCTEXT("Footer", "Up/Down select   Left/Right change   A / Enter confirm   B / Esc back");
	UMRSaveGame* P = GetProfile();
	UMRGameInstance* GI = GetMRGameInstance();
	if (!P || !GI) return;
	const UMRNetworkSettings* NS = GetDefault<UMRNetworkSettings>();
	const bool bHost = HasAuthority(); // standalone or listen-server host

	auto Add = [&](const FText& Label, const FText& Value, TFunction<void()> Confirm, TFunction<void(int32)> Adjust = nullptr) -> FMRMenuItem&
	{
		FMRMenuItem& I = Items.AddDefaulted_GetRef();
		I.Label = Label; I.Value = Value; I.OnConfirm = MoveTemp(Confirm); I.OnAdjust = MoveTemp(Adjust);
		return I;
	};
	auto Info = [&](const FText& Label, const FText& Value = FText::GetEmpty())
	{
		// Long notes (errors, help) wrap onto several rows instead of running off the panel.
		FString Rest = Label.ToString();
		const int32 Wrap = Value.IsEmpty() ? 92 : 1000;
		while (true)
		{
			FString Line = Rest;
			if (Rest.Len() > Wrap)
			{
				int32 Cut = Rest.Left(Wrap).Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				if (Cut <= 0) Cut = Wrap;
				Line = Rest.Left(Cut);
				Rest = Rest.Mid(Cut).TrimStart();
			}
			else Rest.Reset();
			FMRMenuItem& I = Add(FText::FromString(Line), Rest.IsEmpty() ? Value : FText::GetEmpty(), nullptr);
			I.bEnabled = false;
			if (Rest.IsEmpty()) break;
		}
	};
	auto Go = [this](EMRMenuScreen S) { return [this, S]() { OpenMenu(S); }; };

	switch (Screen)
	{
	case EMRMenuScreen::Main:
		Title = LOCTEXT("TitleMain", "MANCHESTER RIDING SIMULATOR");
		Add(LOCTEXT("Ride", "Ride (free roam)"), FText::GetEmpty(), [this]() { CloseMenu(); });
		Add(LOCTEXT("MP", "Multiplayer"), GetServer() && GetServer()->IsOnline() ? FText::Format(LOCTEXT("MPOnline", "online as {0}"), FText::FromString(GetServer()->GetMyName())) : FText::GetEmpty(),
			Go(EMRMenuScreen::Server));
		if (!GI->IsInMultiplayer() && AMRGeoWorld::HasGeoData())
		{
			Add(LOCTEXT("World", "World"), GI->IsTestDistrictSelected() ? LOCTEXT("WorldTest", "Test district (switch to Manchester)") : LOCTEXT("WorldGeo", "Manchester - imported OSM area (switch to test district)"),
				[GI]() { GI->OpenWorld(!GI->IsTestDistrictSelected()); });
		}
		Add(LOCTEXT("Garage", "Garage - every vehicle is free"), FText::GetEmpty(), Go(EMRMenuScreen::Garage));
		Add(LOCTEXT("Rider", "Rider - character editor"), FText::GetEmpty(), Go(EMRMenuScreen::Rider));
		Add(LOCTEXT("Settings", "Settings"), FText::GetEmpty(), Go(EMRMenuScreen::Settings));
		Add(LOCTEXT("Controls", "Controls"), FText::GetEmpty(), Go(EMRMenuScreen::Controls));
		Add(LOCTEXT("Credits", "Credits"), FText::GetEmpty(), Go(EMRMenuScreen::Credits));
		Add(LOCTEXT("Exit", "Exit game"), FText::GetEmpty(), [this]() { SaveProfile(); UKismetSystemLibrary::QuitGame(this, this, EQuitPreference::Quit, false); });
		break;

	case EMRMenuScreen::Server:
	case EMRMenuScreen::ServerLogin:
	case EMRMenuScreen::ServerRegister:
		BuildServerMenu(Add, Info, Go);
		break;

	case EMRMenuScreen::Multiplayer:
	{
		Title = LOCTEXT("TitleMP", "OTHER WAYS TO PLAY (host your own / join by address)");
		if (GetServer() && (GetServer()->IsOnline() || GetServer()->IsBusy()))
		{
			Info(LOCTEXT("OnServer", "You're on the Manchester Ride server. Leave it first (Multiplayer)."));
		}
		else if (GI->IsInMultiplayer())
		{
			const AMRGameState* GS = GetWorld()->GetGameState<AMRGameState>();
			Info(LOCTEXT("Session", "Session"), GS ? FText::Format(LOCTEXT("SessFmt", "{0}  ({1}/{2} riders)"), FText::FromString(GS->SessionName), GS->PlayerArray.Num(), GS->MaxPlayers) : FText::GetEmpty());
			if (bHost)
			{
				Info(LOCTEXT("Port", "Listening on UDP port"), Num(P->HostPort));
				Add(LOCTEXT("CopyInvite", "Copy invite address to clipboard"), P->PlayitPublicAddress.IsEmpty() ? LOCTEXT("NoAddr", "(set your Playit address in Host settings)") : FText::FromString(P->PlayitPublicAddress),
					[P]() { if (!P->PlayitPublicAddress.IsEmpty()) FPlatformApplicationMisc::ClipboardCopy(*P->PlayitPublicAddress); });
				Add(LOCTEXT("CompWheelie", "Start wheelie competition (2 min)"), FText::GetEmpty(), [this]() { ServerStartCompetition(EMRCompetition::LongestWheelie, 120.f); CloseMenu(); });
				Add(LOCTEXT("CompTricks", "Start trick competition (3 min)"), FText::GetEmpty(), [this]() { ServerStartCompetition(EMRCompetition::TrickScore, 180.f); CloseMenu(); });
			}
			Add(LOCTEXT("Leave", "Leave session"), FText::GetEmpty(), [GI]() { GI->LeaveSession(); });
		}
		else
		{
			Add(LOCTEXT("Host", "Host a session"), FText::GetEmpty(), Go(EMRMenuScreen::Host));
			Add(LOCTEXT("Join", "Join by address"), FText::GetEmpty(), Go(EMRMenuScreen::Join));
			Add(LOCTEXT("Browser", "Server browser (favourites & recent)"), FText::GetEmpty(), Go(EMRMenuScreen::Browser));
			if (GI->GetLastError() == EMRConnectError::Timeout || GI->GetLastError() == EMRConnectError::Disconnected || GI->GetLastError() == EMRConnectError::ServerUnavailable)
			{
				Add(LOCTEXT("Reconnect", "Reconnect"), FText::FromString(GI->GetLastJoinAddress()), [GI]() { GI->Reconnect(); });
			}
		}
		if (!GI->GetStatusText().IsEmpty()) Info(LOCTEXT("Status", "Status"), GI->GetStatusText());
		break;
	}

	case EMRMenuScreen::Host:
		Title = LOCTEXT("TitleHost", "HOST A SESSION");
		Add(LOCTEXT("SessName", "Session name (A: paste)"), FText::FromString(P->SessionName), [P]() { const FString S = Paste(); if (!S.IsEmpty()) P->SessionName = S.Left(40); });
		Add(LOCTEXT("HostPort", "UDP port"), Num(P->HostPort), nullptr, [P](int32 D) { P->HostPort = FMath::Clamp(P->HostPort + D, 1024, 65535); });
		Add(LOCTEXT("MaxP", "Max players"), Num(P->HostMaxPlayers), nullptr, [P, NS](int32 D) { P->HostMaxPlayers = FMath::Clamp(P->HostMaxPlayers + D, 2, NS->MaxPlayersCeiling); });
		Add(LOCTEXT("Vis", "Who can join"), VisName(P->HostVisibility), nullptr, [P](int32 D) { P->HostVisibility = static_cast<EMRSessionVisibility>((static_cast<int32>(P->HostVisibility) + D + 3) % 3); });
		Add(LOCTEXT("Pw", "Password (A: paste)"), P->HostPassword.IsEmpty() ? LOCTEXT("NotSet", "not set") : FText::FromString(FString::ChrN(P->HostPassword.Len(), TEXT('*'))),
			[P]() { P->HostPassword = Paste(); });
		Add(LOCTEXT("Friends", "Friends list (A: paste a name to add/remove)"), FText::Format(LOCTEXT("FriendsN", "{0} names"), P->FriendsList.Num()),
			[P]() { const FString N = Paste(); if (N.IsEmpty()) return; if (P->FriendsList.Remove(N) == 0) P->FriendsList.Add(N); });
		Add(LOCTEXT("PlayitAddr", "Playit public address (A: paste)"), P->PlayitPublicAddress.IsEmpty() ? LOCTEXT("NotSet2", "not set - copy it from the Playit dashboard") : FText::FromString(P->PlayitPublicAddress),
			[P]() { P->PlayitPublicAddress = Paste(); });
		Add(LOCTEXT("StartHost", "Start hosting"), FText::GetEmpty(), [this, GI]() { SaveProfile(); CloseMenu(); GI->HostSession(); });
		Info(FText::Format(LOCTEXT("PlayitHelp", "Playit.gg: run the playit agent, add a UDP tunnel to local port {0}, then share the tunnel address."), Num(P->HostPort)));
		break;

	case EMRMenuScreen::Join:
		Title = LOCTEXT("TitleJoin", "JOIN A SESSION");
		Add(LOCTEXT("Addr", "Address (A: paste from clipboard)"), JoinAddressDraft.IsEmpty() ? LOCTEXT("Empty", "e.g. name.gl.at.ply.gg:12345") : FText::FromString(JoinAddressDraft),
			[this]() { JoinAddressDraft = Paste(); });
		Add(LOCTEXT("JPw", "Password (A: paste)"), JoinPasswordDraft.IsEmpty() ? LOCTEXT("None", "none") : FText::FromString(FString::ChrN(JoinPasswordDraft.Len(), TEXT('*'))),
			[this]() { JoinPasswordDraft = Paste(); });
		Add(LOCTEXT("Connect", "Connect"), FText::GetEmpty(), [this, GI]() { GI->JoinByAddress(JoinAddressDraft, JoinPasswordDraft); });
		Add(LOCTEXT("Fav", "Add to favourites"), FText::GetEmpty(), [this, P]()
		{
			FString H; int32 Port;
			if (UMRGameInstance::ParseAddress(JoinAddressDraft, H, Port) && !P->Favourites.ContainsByPredicate([&](const FMRServerEntry& E) { return E.Address == JoinAddressDraft; }))
			{
				FMRServerEntry E; E.Address = JoinAddressDraft; E.Name = JoinAddressDraft; P->Favourites.Add(E);
			}
		});
		if (GI->IsJoining() || !GI->GetStatusText().IsEmpty()) Info(LOCTEXT("JStatus", "Status"), GI->GetStatusText());
		break;

	case EMRMenuScreen::Browser:
		Title = LOCTEXT("TitleBrowser", "SERVER BROWSER");
		Info(LOCTEXT("BrowserNote", "Playit tunnels have no discovery service: sessions are listed from your favourites and history."));
		for (int32 i = 0; i < P->Favourites.Num(); ++i)
		{
			const FString Addr = P->Favourites[i].Address;
			Add(FText::Format(LOCTEXT("FavRow", "* {0}"), FText::FromString(P->Favourites[i].Name)), LOCTEXT("FavHint", "A join   Left remove"),
				[this, GI, Addr]() { GI->JoinByAddress(Addr, JoinPasswordDraft); },
				[P, i](int32 D) { if (D < 0 && P->Favourites.IsValidIndex(i)) P->Favourites.RemoveAt(i); });
		}
		for (const FMRServerEntry& R : P->RecentServers)
		{
			const FString Addr = R.Address;
			Add(FText::Format(LOCTEXT("RecentRow", "{0}"), FText::FromString(Addr)), FText::Format(LOCTEXT("RecentWhen", "last {0}   Right: favourite"), FText::AsDate(R.LastJoined)),
				[this, GI, Addr]() { GI->JoinByAddress(Addr, JoinPasswordDraft); },
				[P, Addr](int32 D) { if (D > 0 && !P->Favourites.ContainsByPredicate([&](const FMRServerEntry& E) { return E.Address == Addr; })) { FMRServerEntry E; E.Address = Addr; E.Name = Addr; P->Favourites.Add(E); } });
		}
		if (P->Favourites.Num() == 0 && P->RecentServers.Num() == 0) Info(LOCTEXT("NoServers", "No saved servers yet - join one by address first."));
		break;

	case EMRMenuScreen::Garage:
	{
		Title = LOCTEXT("TitleGarage", "GARAGE - ALL VEHICLES FREE");
		FMRVehicleInfo VI;
		UMRVehicleLibrary::GetVehicleInfo(GarageVehicle, VI);
		int32 InCat = 0, CatCount = 0;
		UMRVehicleLibrary::CycleWithinCategory(GarageVehicle, 0, InCat, CatCount);
		const int32 Total = UMRVehicleLibrary::GetAllVehicles().Num();
		Add(LOCTEXT("Category", "Category"), FText::Format(LOCTEXT("CatFmt", "< {0} >   ({1} models, {2} in total)"), VI.Category, CatCount, Total), nullptr,
			[this](int32 D) { GarageVehicle = UMRVehicleLibrary::CycleCategory(GarageVehicle, D); });
		Add(LOCTEXT("Vehicle", "Vehicle"), FText::Format(LOCTEXT("VehFmt", "< {0} >   ({1}/{2})"), VI.DisplayName, InCat + 1, CatCount), nullptr,
			[this](int32 D) { int32 I, N; GarageVehicle = UMRVehicleLibrary::CycleWithinCategory(GarageVehicle, D, I, N); });
		Info(VI.Category, VI.Powertrain);
		Info(VI.Description);
		Info(LOCTEXT("Stats", "Mass (with rider) / governed top speed"), FText::Format(LOCTEXT("StatsFmt", "{0} kg / {1} mph"), Num(VI.MassKg), Num(VI.GovernedTopSpeedMph)));
		Add(LOCTEXT("Paint", "Paint"), FText::FromString(PaletteNames[NearestPalette(P->PaintColor)]), nullptr,
			[P](int32 D) { P->PaintColor = Palette[(NearestPalette(P->PaintColor) + D + PaletteCount) % PaletteCount]; });
		Add(LOCTEXT("Jacket", "Rider jacket"), FText::FromString(PaletteNames[NearestPalette(P->JacketColor)]), nullptr,
			[P](int32 D) { P->JacketColor = Palette[(NearestPalette(P->JacketColor) + D + PaletteCount) % PaletteCount]; });
		Add(LOCTEXT("Helmet", "Helmet"), FText::FromString(PaletteNames[NearestPalette(P->HelmetColor)]), nullptr,
			[P](int32 D) { P->HelmetColor = Palette[(NearestPalette(P->HelmetColor) + D + PaletteCount) % PaletteCount]; });
		Add(LOCTEXT("RideThis", "Ride this vehicle"), FText::GetEmpty(), [this, P]()
		{
			P->VehicleId = GarageVehicle;
			if (const FLinearColor* Saved = P->PaintPerVehicle.Find(GarageVehicle)) P->PaintColor = *Saved;
			ApplyCustomisation(true);
			CloseMenu();
		});
		Add(LOCTEXT("EditRider", "Edit rider (character editor)"), FText::GetEmpty(), Go(EMRMenuScreen::Rider));
		Footer = LOCTEXT("GarageFooter", "No prices, no unlocks: pick anything, change it any time.");
		break;
	}

	case EMRMenuScreen::Rider:
	{
		Title = LOCTEXT("TitleRider", "RIDER - CHARACTER EDITOR");
		FMRRiderLook& L = P->RiderLook;
		// Every change previews live on your rider; "Save" stores it and shows it to other players.
		auto Preview = [this, P]()
		{
			P->RiderLook.Sanitise();
			if (AMRRideVehicle* V = GetVehicle()) V->SetRiderLook(P->RiderLook);
		};
		auto Cycle = [Preview](uint8& Field, int32 Num)
		{
			return [&Field, Num, Preview](int32 D) { Field = static_cast<uint8>((Field + D + Num) % Num); Preview(); };
		};
		auto Name = [](const TArray<FString>& Names, uint8 I) { return FText::FromString(Names.IsValidIndex(I) ? Names[I] : FString()); };
		const TArray<FString>& Pal = FMRRiderLook::PaletteNames();

		Add(LOCTEXT("RBody", "Build"), Name(FMRRiderLook::BodyNames(), L.Body), nullptr, Cycle(L.Body, FMRRiderLook::BodyNames().Num()));
		Add(LOCTEXT("RHeight", "Height"), FText::Format(LOCTEXT("RHeightFmt", "{0} cm"), L.HeightCm), nullptr,
			[&L, Preview](int32 D) { L.HeightCm = static_cast<uint8>(FMath::Clamp<int32>(L.HeightCm + D * 2, 150, 200)); Preview(); });
		Add(LOCTEXT("RSkin", "Skin tone"), Name(FMRRiderLook::SkinToneNames(), L.SkinTone), nullptr, Cycle(L.SkinTone, FMRRiderLook::SkinTones().Num()));
		Add(LOCTEXT("RHair", "Hair"), Name(FMRRiderLook::HairStyles(), L.HairStyle), nullptr, Cycle(L.HairStyle, FMRRiderLook::HairStyles().Num()));
		Add(LOCTEXT("RHairCol", "Hair colour"), Name(FMRRiderLook::HairColourNames(), L.HairColour), nullptr, Cycle(L.HairColour, FMRRiderLook::HairColours().Num()));
		Add(LOCTEXT("RFacial", "Facial hair"), Name(FMRRiderLook::FacialHairNames(), L.FacialHair), nullptr, Cycle(L.FacialHair, FMRRiderLook::FacialHairNames().Num()));
		Add(LOCTEXT("RHelmet", "Helmet style"), Name(FMRRiderLook::HelmetStyles(), L.HelmetStyle), nullptr, Cycle(L.HelmetStyle, FMRRiderLook::HelmetStyles().Num()));
		Add(LOCTEXT("RHelmetCol", "Helmet colour"), FText::FromString(PaletteNames[NearestPalette(P->HelmetColor)]), nullptr,
			[P, this](int32 D) { P->HelmetColor = Palette[(NearestPalette(P->HelmetColor) + D + PaletteCount) % PaletteCount]; if (AMRRideVehicle* V = GetVehicle()) V->PreviewColours(P->PaintColor, P->JacketColor, P->HelmetColor); });
		Add(LOCTEXT("RVisor", "Visor / goggles"), Name(FMRRiderLook::VisorNames(), L.Visor), nullptr, Cycle(L.Visor, FMRRiderLook::VisorColours().Num()));
		Add(LOCTEXT("RTop", "Top"), Name(FMRRiderLook::TopStyles(), L.TopStyle), nullptr, Cycle(L.TopStyle, FMRRiderLook::TopStyles().Num()));
		Add(LOCTEXT("RTopCol", "Top colour"), FText::FromString(PaletteNames[NearestPalette(P->JacketColor)]), nullptr,
			[P, this](int32 D) { P->JacketColor = Palette[(NearestPalette(P->JacketColor) + D + PaletteCount) % PaletteCount]; if (AMRRideVehicle* V = GetVehicle()) V->PreviewColours(P->PaintColor, P->JacketColor, P->HelmetColor); });
		Add(LOCTEXT("RAccent", "Stripes / trim"), Name(Pal, L.Accent), nullptr, Cycle(L.Accent, Pal.Num()));
		Add(LOCTEXT("RLegs", "Trousers"), Name(Pal, L.Legs), nullptr, Cycle(L.Legs, Pal.Num()));
		Add(LOCTEXT("RGloves", "Gloves"), Name(Pal, L.Gloves), nullptr, Cycle(L.Gloves, Pal.Num()));
		Add(LOCTEXT("RShoes", "Shoes / boots"), Name(Pal, L.Shoes), nullptr, Cycle(L.Shoes, Pal.Num()));
		Add(LOCTEXT("RBag", "Bag"), Name(FMRRiderLook::BackpackNames(), L.Backpack), nullptr, Cycle(L.Backpack, FMRRiderLook::BackpackNames().Num()));
		Add(LOCTEXT("RRandom", "Randomise"), FText::GetEmpty(), [P, Preview]() { P->RiderLook = FMRRiderLook::Random(FMath::Rand()); Preview(); });
		Add(LOCTEXT("RReset", "Reset to default"), FText::GetEmpty(), [P, Preview]() { P->RiderLook = FMRRiderLook(); Preview(); });
		Add(LOCTEXT("RSave", "Save rider"), FText::GetEmpty(), [this, P]()
		{
			SaveProfile();
			ServerSetRiderLook(P->RiderLook, P->JacketColor, P->HelmetColor);
			OnMenuBack(FInputActionValue());
		});
		Footer = LOCTEXT("RiderFooter", "Left/Right to change. Everything here is free. Changes preview live; Save shows them to other riders.");
		break;
	}

	case EMRMenuScreen::Settings:
	{
		Title = LOCTEXT("TitleSettings", "SETTINGS");
		Add(LOCTEXT("Gfx", "Graphics preset"), UMRGameInstance::GraphicsPresetName(P->GraphicsPreset), nullptr,
			[GI, P](int32 D) { GI->ApplyGraphicsPreset(FMath::Clamp(P->GraphicsPreset + D, 0, 3)); });
		Add(LOCTEXT("Wheelie", "Wheelie / stunt assist"), AssistName(P->WheelieAssist), nullptr,
			[this, P](int32 D) { P->WheelieAssist = static_cast<uint8>(FMath::Clamp<int32>(P->WheelieAssist + D, 0, 2)); ApplyProfileToVehicle(); });
		Add(LOCTEXT("AutoClutch", "Auto clutch (motorbikes)"), OnOff(P->bAutoClutch), nullptr, [this, P](int32) { P->bAutoClutch = !P->bAutoClutch; ApplyProfileToVehicle(); });
		Add(LOCTEXT("AutoShift", "Automatic gears"), OnOff(P->bAutoShift), nullptr, [this, P](int32) { P->bAutoShift = !P->bAutoShift; ApplyProfileToVehicle(); });
		Add(LOCTEXT("Landing", "Landing assist"), OnOff(P->bLandingAssist), nullptr, [this, P](int32) { P->bLandingAssist = !P->bLandingAssist; ApplyProfileToVehicle(); });
		Add(LOCTEXT("ABS", "ABS (front wheel)"), OnOff(P->bABS), nullptr, [this, P](int32) { P->bABS = !P->bABS; ApplyProfileToVehicle(); });
		Add(LOCTEXT("SteerSens", "Steering sensitivity"), Num(P->SteerSensitivity, 1), nullptr, [this, P](int32 D) { P->SteerSensitivity = FMath::Clamp(P->SteerSensitivity + D * 0.1f, 0.3f, 2.f); ApplyProfileToVehicle(); });
		Add(LOCTEXT("BrakeSens", "Brake sensitivity"), Num(P->BrakeSensitivity, 1), nullptr, [P](int32 D) { P->BrakeSensitivity = FMath::Clamp(P->BrakeSensitivity + D * 0.1f, 0.3f, 2.f); });
		Add(LOCTEXT("CamSens", "Camera sensitivity"), Num(P->CameraSensitivity, 1), nullptr, [this, P](int32 D) { P->CameraSensitivity = FMath::Clamp(P->CameraSensitivity + D * 0.1f, 0.2f, 3.f); ApplyProfileToVehicle(); });
		Add(LOCTEXT("Invert", "Invert look Y"), OnOff(P->bInvertLookY), nullptr, [this, P](int32) { P->bInvertLookY = !P->bInvertLookY; UMRInputConfig::Get()->Rebuild(P->KeyBindings, P->bInvertLookY); });
		Add(LOCTEXT("Vibration", "Controller vibration"), OnOff(P->bVibration), nullptr, [this, P](int32) { P->bVibration = !P->bVibration; SetDisableHaptics(!P->bVibration); });
		Add(LOCTEXT("Name", "Player name (A: paste)"), FText::FromString(P->PlayerName), [this, P]()
		{
			const FString N = Paste();
			if (!N.IsEmpty()) { P->PlayerName = N.Left(24); SetName(P->PlayerName); }
		});
		// Photoreal map (Google 3D tiles via Cesium ion): the player's own token, pasted from the clipboard.
		// The photoreal map is a session setting: the host (or a solo player) decides, friends who join follow.
		AMRGameState* PhotoGS = GetWorld()->GetGameState<AMRGameState>();
		if (!HasAuthority())
		{
			Add(LOCTEXT("PhotoMapClient", "Photoreal map (set by host)"), OnOff(PhotoGS && PhotoGS->bPhotoMap), nullptr, nullptr);
		}
		else
		{
			Add(LOCTEXT("PhotoMap", "Photoreal map (Google 3D tiles)"),
				!AMRPhotoLayer::IsAvailable() ? LOCTEXT("PhotoNoPlugin", "needs the Cesium plugin")
					: (P->bPhotoMap && P->PhotoMapToken.IsEmpty() ? LOCTEXT("PhotoNeedsToken", "On (paste a token below)") : OnOff(P->bPhotoMap)), nullptr,
				[P, PhotoGS](int32)
				{
					if (!AMRPhotoLayer::IsAvailable()) return;
					P->bPhotoMap = !P->bPhotoMap;
					if (PhotoGS) PhotoGS->SetPhotoMap(P->bPhotoMap, P->PhotoMapToken);
				});
			Add(LOCTEXT("PhotoToken", "Cesium ion token (A: paste)"),
				bPhotoTokenPasteFailed ? LOCTEXT("PhotoTokenBad", "clipboard has no token - copy it on ion.cesium.com, then press A again")
					: P->PhotoMapToken.IsEmpty() ? LOCTEXT("PhotoTokenNone", "not set - copy your token, then press A")
					: FText::FromString(FString::Printf(TEXT("set (...%s)"), *P->PhotoMapToken.Right(4))),
				[this, P, PhotoGS]()
				{
					const FString Tok = PasteLong();
					// Cesium ion tokens are JWTs: three base64url parts starting "eyJ".
					bPhotoTokenPasteFailed = !(Tok.StartsWith(TEXT("eyJ")) && Tok.Len() > 100 && !Tok.Contains(TEXT(" ")));
					if (bPhotoTokenPasteFailed) return;
					P->PhotoMapToken = Tok;
					SaveProfile();
					if (P->bPhotoMap && PhotoGS) PhotoGS->SetPhotoMap(true, Tok);
				});
		}
		if (bHost)
		{
			const AMRWorldEnvironment* Env = AMRWorldEnvironment::Get(this);
			const int32 W = Env ? static_cast<int32>(Env->GetWeather()) : 0;
			const float H = Env ? Env->GetTimeOfDayHours() : 12.f;
			Add(LOCTEXT("Weather", "Weather (host)"), Env ? AMRWorldEnvironment::WeatherName(Env->GetWeather()) : FText::GetEmpty(), nullptr,
				[this, W](int32 D) { ServerSetEnvironment((W + D + 6) % 6, -1.f); });
			Add(LOCTEXT("Time", "Time of day (host)"), FText::Format(LOCTEXT("TimeFmt", "{0}:00"), Num(FMath::FloorToFloat(H))), nullptr,
				[this, H](int32 D) { ServerSetEnvironment(-1, FMath::Fmod(FMath::FloorToFloat(H) + D + 24.f, 24.f)); });
		}
		break;
	}

	case EMRMenuScreen::Controls:
	{
		Title = LOCTEXT("TitleControls", "CONTROLS");
		UMRInputConfig* C = UMRInputConfig::Get();
		for (const UMRInputConfig::FBindable& B : C->GetBindables())
		{
			const FName Name = B.Name;
			const FText Value = (bAwaitingKey && RebindAction == Name) ? LOCTEXT("PressKey", "press a key or button... (Esc cancels)")
				: FText::Format(LOCTEXT("KeyFmt", "{0}   /   {1}"), C->GetKeyboardKey(Name).GetDisplayName(), C->GetGamepadKey(Name).GetDisplayName());
			Add(B.Label, Value, [this, Name]() { bAwaitingKey = true; RebindAction = Name; });
		}
		Info(LOCTEXT("Fixed", "Steer: A/D or left stick X.  Lean back/forward: Up/Down or left stick Y.  Look: mouse or right stick."));
		Add(LOCTEXT("ResetKeys", "Reset all to defaults"), FText::GetEmpty(), [this, P]() { P->KeyBindings.Reset(); UMRInputConfig::Get()->Rebuild(P->KeyBindings, P->bInvertLookY); ApplyInputContexts(); });
		Footer = LOCTEXT("ControlsFooter", "A / Enter: rebind (keyboard key sets keyboard, pad button sets pad)");
		break;
	}

	case EMRMenuScreen::Credits:
		Title = LOCTEXT("TitleCredits", "CREDITS");
		Info(LOCTEXT("C1", "Manchester Riding Simulator - an open-world riding project"));
		Info(LOCTEXT("C2", "Built with Unreal Engine 5"));
		Info(LOCTEXT("C3", "Map data (c) OpenStreetMap contributors, available under the ODbL"));
		Info(LOCTEXT("C3b", "Photo textures: ambientCG.com (CC0 public domain)"));
		Info(LOCTEXT("C3b", "Terrain: Tilezen Terrain Tiles (AWS Open Data), derived from SRTM / EU-DEM"));
		Info(LOCTEXT("C4", "Multiplayer tunnelling: Playit.gg (no affiliation)"));
		Info(LOCTEXT("C5", "All vehicles are free."));
		Info(LOCTEXT("C6", "Vehicle brand and model names are trademarks of their owners, used for identification only."));
		Info(LOCTEXT("C7", "No affiliation or endorsement. Handling figures are modelled on each type, not manufacturer data."));
		break;

	default:
		break;
	}

	if (const UMRGameInstance* G = GI; G->GetLastError() != EMRConnectError::None && (Screen == EMRMenuScreen::Multiplayer || Screen == EMRMenuScreen::Join || Screen == EMRMenuScreen::Browser))
	{
		Info(LOCTEXT("Error", "Error"), G->GetLastErrorText());
	}
	if (const UMRServerClient* SC = GetServer(); SC && (Screen == EMRMenuScreen::Server || Screen == EMRMenuScreen::ServerLogin || Screen == EMRMenuScreen::ServerRegister))
	{
		if (!LocalFormError.IsEmpty()) Info(LocalFormError);
		else if (SC->GetState() == EMRServerState::Failed && !SC->GetErrorText().IsEmpty()) Info(SC->GetErrorText());
	}
	if (!Items.IsValidIndex(Selected) || !Items[Selected].bEnabled)
	{
		Selected = Items.IndexOfByPredicate([](const FMRMenuItem& I) { return I.bEnabled; });
		if (Selected == INDEX_NONE) Selected = 0;
	}
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------

void AMRPlayerController::MRHost() { if (UMRGameInstance* GI = GetMRGameInstance()) GI->HostSession(); }
void AMRPlayerController::MRJoin(const FString& Address, const FString& Password) { if (UMRGameInstance* GI = GetMRGameInstance()) GI->JoinByAddress(Address, Password); }
void AMRPlayerController::MRLeave() { if (UMRGameInstance* GI = GetMRGameInstance()) GI->LeaveSession(); }

void AMRPlayerController::MRVehicle(const FString& VehicleId)
{
	UMRSaveGame* P = GetProfile();
	if (!P || !UMRVehicleLibrary::IsValidVehicle(FName(*VehicleId)))
	{
		ClientMessage(TEXT("Unknown vehicle. Use MRListVehicles."));
		return;
	}
	P->VehicleId = FName(*VehicleId);
	ApplyCustomisation(true);
}

void AMRPlayerController::MRListVehicles()
{
	for (const FMRVehicleInfo& I : UMRVehicleLibrary::GetAllVehicles())
	{
		ClientMessage(FString::Printf(TEXT("%s  -  %s (%s)"), *I.Id.ToString(), *I.DisplayName.ToString(), *I.Category.ToString()));
	}
}

void AMRPlayerController::MRName(const FString& Name) { if (UMRSaveGame* P = GetProfile()) { P->PlayerName = Name.Left(24); SetName(P->PlayerName); SaveProfile(); } }
void AMRPlayerController::MRPassword(const FString& Password) { if (UMRSaveGame* P = GetProfile()) { P->HostPassword = Password; SaveProfile(); } }
void AMRPlayerController::MRFriend(const FString& Name) { if (UMRSaveGame* P = GetProfile()) { if (P->FriendsList.Remove(Name) == 0) P->FriendsList.Add(Name); SaveProfile(); } }
void AMRPlayerController::MRPlayit(const FString& PublicAddress) { if (UMRSaveGame* P = GetProfile()) { P->PlayitPublicAddress = PublicAddress; SaveProfile(); } }
void AMRPlayerController::MRPort(int32 Port) { if (UMRSaveGame* P = GetProfile()) { P->HostPort = FMath::Clamp(Port, 1024, 65535); SaveProfile(); } }
void AMRPlayerController::MRMaxPlayers(int32 Count) { if (UMRSaveGame* P = GetProfile()) { P->HostMaxPlayers = FMath::Clamp(Count, 2, GetDefault<UMRNetworkSettings>()->MaxPlayersCeiling); SaveProfile(); } }
void AMRPlayerController::MRGraphics(int32 Preset) { if (UMRGameInstance* GI = GetMRGameInstance()) { GI->ApplyGraphicsPreset(Preset); GI->SaveProfile(); } }

void AMRPlayerController::MRAssist(const FString& Mode)
{
	UMRSaveGame* P = GetProfile();
	if (!P) return;
	P->WheelieAssist = Mode.StartsWith(TEXT("r")) ? 2 : (Mode.StartsWith(TEXT("b")) ? 1 : 0);
	ApplyProfileToVehicle();
	SaveProfile();
}

void AMRPlayerController::MRWeather(int32 Weather) { ServerSetEnvironment(Weather, -1.f); }
void AMRPlayerController::MRTime(float Hours) { ServerSetEnvironment(-1, Hours); }

void AMRPlayerController::MRBind(const FString& Action, const FString& Key)
{
	UMRSaveGame* P = GetProfile();
	const FKey K(*Key);
	if (!P || !K.IsValid()) { ClientMessage(TEXT("Usage: MRBind <Action> <KeyName>, e.g. MRBind Wheelie Gamepad_LeftThumbstick")); return; }
	FMRKeyBinding* B = P->KeyBindings.FindByPredicate([&](const FMRKeyBinding& E) { return E.Action == FName(*Action); });
	if (!B) { B = &P->KeyBindings.AddDefaulted_GetRef(); B->Action = FName(*Action); }
	(K.IsGamepadKey() ? B->Gamepad : B->Keyboard) = K;
	UMRInputConfig::Get()->Rebuild(P->KeyBindings, P->bInvertLookY);
	ApplyInputContexts();
	SaveProfile();
}

void AMRPlayerController::MRCompetition(const FString& Type, float Seconds)
{
	ServerStartCompetition(Type.StartsWith(TEXT("t")) ? EMRCompetition::TrickScore : EMRCompetition::LongestWheelie, Seconds > 0.f ? Seconds : 120.f);
}

#undef LOCTEXT_NAMESPACE
