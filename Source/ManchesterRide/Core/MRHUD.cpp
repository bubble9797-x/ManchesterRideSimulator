#include "Core/MRHUD.h"

#include "Core/MRGameInstance.h"
#include "Core/MRGameState.h"
#include "Core/MRPlayerController.h"
#include "Core/MRPlayerState.h"
#include "Core/MRSaveGame.h"
#include "GeoProjection.h"
#include "Net/MRServerClient.h"
#include "Vehicles/MRRideVehicle.h"
#include "Vehicles/MRVehicleLibrary.h"
#include "VehicleCatalog.h"
#include "World/MRGeoWorld.h"
#include "World/MRMapData.h"
#include "CanvasItem.h"
#include "World/MRTestEnvironment.h"
#include "World/MRWorldEnvironment.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"

namespace UI
{
	const FLinearColor White(1.f, 1.f, 1.f);
	const FLinearColor Dim(0.65f, 0.67f, 0.7f);
	const FLinearColor Accent(1.f, 0.72f, 0.1f);   // bee-yellow, a nod to the Manchester worker bee
	const FLinearColor Good(0.35f, 0.9f, 0.4f);
	const FLinearColor Bad(1.f, 0.3f, 0.25f);
	const FLinearColor PanelBg(0.02f, 0.025f, 0.03f, 0.72f);
	const FLinearColor Row(1.f, 0.72f, 0.1f, 0.18f);
	const FLinearColor Route(0.f, 0.42f, 1.f);       // sat-nav line
	const FLinearColor RouteText(0.5f, 0.75f, 1.f);

	FString CompassPoint(float YawDeg)
	{
		// Unreal +X = East, +Y = South (see GeoProjection.h). Yaw 0 = East, 90 = South.
		const float Bearing = FMath::Fmod(YawDeg + 90.f + 360.f, 360.f); // 0 = North
		static const TCHAR* Names[] = { TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"), TEXT("S"), TEXT("SW"), TEXT("W"), TEXT("NW") };
		return Names[static_cast<int32>((Bearing + 22.5f) / 45.f) % 8];
	}
}

void AMRHUD::Text(const FString& Str, float X, float Y, const FLinearColor& C, float Scale, bool bRightAlign, bool bCenter, UFont* Font)
{
	if (!Font) Font = GEngine->GetMediumFont();
	float W = 0.f, H = 0.f;
	GetTextSize(Str, W, H, Font, Scale * S);
	if (bRightAlign) X -= W;
	if (bCenter) X -= W * 0.5f;
	DrawText(Str, FLinearColor(0, 0, 0, C.A * 0.8f), X + 1.5f * S, Y + 1.5f * S, Font, Scale * S); // drop shadow
	DrawText(Str, C, X, Y, Font, Scale * S);
}

void AMRHUD::Panel(float X, float Y, float W, float H, const FLinearColor& C)
{
	DrawRect(C, X, Y, W, H);
}

void AMRHUD::Bar(float X, float Y, float W, float H, float Fraction, const FLinearColor& Fill)
{
	DrawRect(FLinearColor(0, 0, 0, 0.6f), X, Y, W, H);
	DrawRect(Fill, X, Y, W * FMath::Clamp(Fraction, 0.f, 1.f), H);
}

void AMRHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas) return;
	S = Canvas->ClipY / 1080.f;
	AMRPlayerController* PC = Cast<AMRPlayerController>(PlayerOwner);
	if (!PC) return;

	AMRRideVehicle* V = PC->GetVehicle();
	if (V && PC->GetMenuScreen() != EMRMenuScreen::Garage && PC->GetMenuScreen() != EMRMenuScreen::Rider)
	{
		DrawNameTags(V);
		if (!PC->IsMenuOpen())
		{
			DrawRidingHUD(PC, V);
			DrawMinimap(V);
		}
	}
	if (PC->IsMapOpen() && !PC->IsMenuOpen())
	{
		DrawFullMap(PC);
		return;
	}
	DrawSessionInfo(PC);
	DrawCompetition();
	const UMRServerClient* SC = UMRServerClient::Get(this);
	if (PC->IsPlayerListHeld()) { if (SC && SC->IsOnline()) DrawServerPlayerList(); else DrawPlayerList(); }
	if (SC && SC->IsOnline())
	{
		DrawVoice();
		if (!PC->IsMenuOpen()) DrawChat(PC);
	}
	if (PC->IsMenuOpen()) DrawMenu(PC);
	if (SC && SC->GetState() == EMRServerState::Updating && !PC->IsMenuOpen()) DrawUpdate();
	DrawIntro();
}

void AMRHUD::DrawChat(AMRPlayerController* PC)
{
	const UMRServerClient* SC = UMRServerClient::Get(this);
	const TArray<FMRChatLine>& Chat = SC->GetChat();
	const bool bOpen = PC->IsChatOpen();
	const double Now = FPlatformTime::Seconds();
	const float X = 30.f * S, W = 760.f * S, LineH = 27.f * S;
	const float Bottom = Canvas->ClipY - 250.f * S; // above the vehicle panel
	const int32 MaxLines = bOpen ? 14 : 8;

	TArray<const FMRChatLine*> Show;
	for (int32 i = Chat.Num() - 1; i >= 0 && Show.Num() < MaxLines; --i)
	{
		if (!bOpen && Now - Chat[i].Time > 12.0) break; // closed: only recent lines, then they fade away
		Show.Insert(&Chat[i], 0);
	}
	if (bOpen) Panel(X - 10.f * S, Bottom - MaxLines * LineH - 10.f * S, W + 20.f * S, MaxLines * LineH + 58.f * S, FLinearColor(0.02f, 0.025f, 0.03f, 0.6f));
	float Y = Bottom - Show.Num() * LineH;
	for (const FMRChatLine* L : Show)
	{
		const float Age = float(Now - L->Time);
		const float A = bOpen ? 1.f : FMath::Clamp(12.f - Age, 0.f, 1.f);
		FLinearColor C = UI::White;
		FString Line;
		switch (L->Kind)
		{
		case MRNet::ChatPlayer:  Line = FString::Printf(TEXT("<%s> %s"), *L->From, *L->Text); break;
		case MRNet::ChatSystem:  Line = L->Text; C = UI::Accent; break;
		case MRNet::ChatPrivate: Line = FString::Printf(TEXT("[%s] %s"), *L->From, *L->Text); C = FLinearColor(0.85f, 0.6f, 1.f); break;
		case MRNet::ChatServer:  Line = FString::Printf(TEXT("[%s] %s"), *L->From, *L->Text); C = FLinearColor(1.f, 0.5f, 0.85f); break;
		case MRNet::ChatError:   Line = L->Text; C = UI::Bad; break;
		case MRNet::ChatAction:  Line = FString::Printf(TEXT("* %s %s"), *L->From, *L->Text); C = UI::Dim; break;
		default: Line = L->Text; break;
		}
		if (Line.Len() > 110) Line = Line.Left(107) + TEXT("...");
		C.A = A;
		Text(Line, X, Y, C, 0.85f);
		Y += LineH;
	}
	if (bOpen)
	{
		const bool bCaret = FMath::Fmod(Now, 1.0) < 0.55;
		Panel(X - 4.f * S, Bottom + 8.f * S, W + 8.f * S, 34.f * S, FLinearColor(0, 0, 0, 0.75f));
		FString Draft = PC->GetChatDraft();
		if (Draft.Len() > 100) Draft = TEXT("...") + Draft.Right(97);
		Text(TEXT("> ") + Draft + (bCaret ? TEXT("_") : TEXT("")), X + 6.f * S, Bottom + 12.f * S, UI::White, 0.9f);
		Text(TEXT("Enter send   Esc cancel   /help for commands"), X + W, Bottom + 46.f * S, UI::Dim, 0.7f, true);
	}
}

void AMRHUD::DrawVoice()
{
	const UMRServerClient* SC = UMRServerClient::Get(this);
	float Y = 60.f * S;
	if (SC->IsTransmitting())
	{
		Panel(30.f * S, Y, 210.f * S, 30.f * S, FLinearColor(0.6f, 0.05f, 0.05f, 0.75f));
		Text(TEXT("MIC ON - talking"), 42.f * S, Y + 4.f * S, UI::White, 0.85f);
		Y += 36.f * S;
	}
	for (const TPair<uint16, FMRServerRider>& P : SC->GetRiders())
	{
		if (!SC->IsRiderTalking(P.Value)) continue;
		Text(FString::Printf(TEXT("(( %s"), *P.Value.Name), 34.f * S, Y, UI::Good, 0.85f);
		Y += 26.f * S;
	}
}

void AMRHUD::DrawServerPlayerList()
{
	const UMRServerClient* SC = UMRServerClient::Get(this);
	const float W = 900.f * S, RowH = 32.f * S;
	const float X = (Canvas->ClipX - W) * 0.5f, Y = 160.f * S;
	const int32 N = SC->GetRiders().Num() + 1;
	Panel(X, Y, W, (110.f * S) + N * RowH, UI::PanelBg);
	Text(FString::Printf(TEXT("%s   %d / %d riders"), *SC->GetServerName(), N, SC->GetMaxPlayers()), X + 20.f * S, Y + 14.f * S, UI::Accent, 1.f);
	Text(TEXT("Rider"), X + 20.f * S, Y + 54.f * S, UI::Dim, 0.85f);
	Text(TEXT("Vehicle"), X + 340.f * S, Y + 54.f * S, UI::Dim, 0.85f);
	Text(TEXT("Ping"), X + W - 20.f * S, Y + 54.f * S, UI::Dim, 0.85f, true);
	float RY = Y + 88.f * S;
	auto Row = [&](const FString& Name, bool bOp, FName Vehicle, int32 Ping, bool bTalking, bool bMe)
	{
		FMRVehicleInfo VI;
		UMRVehicleLibrary::GetVehicleInfo(Vehicle, VI);
		Text(Name + (bOp ? TEXT("  [op]") : TEXT("")) + (bTalking ? TEXT("  ((talking))") : TEXT("")), X + 20.f * S, RY, bMe ? UI::Accent : UI::White, 0.85f);
		Text(VI.DisplayName.ToString(), X + 340.f * S, RY, UI::Dim, 0.85f);
		Text(FString::Printf(TEXT("%d"), Ping), X + W - 20.f * S, RY, UI::Dim, 0.85f, true);
		RY += RowH;
	};
	const AMRPlayerController* PC = Cast<AMRPlayerController>(PlayerOwner);
	const AMRRideVehicle* Me = PC ? PC->GetVehicle() : nullptr;
	Row(SC->GetMyName(), SC->IsOp(), Me ? Me->GetVehicleId() : NAME_None, SC->GetPingMs(), SC->IsTransmitting(), true);
	for (const TPair<uint16, FMRServerRider>& P : SC->GetRiders())
	{
		Row(P.Value.Name, P.Value.bOp, P.Value.VehicleId, P.Value.PingMs, SC->IsRiderTalking(P.Value), false);
	}
}

void AMRHUD::DrawUpdate()
{
	const UMRServerClient* SC = UMRServerClient::Get(this);
	const float W = 760.f * S, H = 150.f * S;
	const float X = (Canvas->ClipX - W) * 0.5f, Y = (Canvas->ClipY - H) * 0.5f;
	Panel(0, 0, Canvas->ClipX, Canvas->ClipY, FLinearColor(0, 0, 0, 0.5f));
	Panel(X, Y, W, H, UI::PanelBg);
	Panel(X, Y, W, 4.f * S, UI::Accent);
	Text(FString::Printf(TEXT("Updating to version %s"), *SC->GetUpdateVersion()), X + 30.f * S, Y + 22.f * S, UI::Accent, 1.2f);
	const float Frac = SC->GetUpdateSize() > 0 ? float(double(SC->GetUpdateReceived()) / double(SC->GetUpdateSize())) : 0.f;
	Bar(X + 30.f * S, Y + 70.f * S, W - 60.f * S, 18.f * S, Frac, UI::Good);
	Text(SC->IsVerifyingUpdate() ? FString(TEXT("Checking the download..."))
		: FString::Printf(TEXT("%.0f / %.0f MB   %.1f MB/s   (Esc: menu, to cancel)"), SC->GetUpdateReceived() / 1048576.0, SC->GetUpdateSize() / 1048576.0, SC->GetUpdateRate() / 1048576.0),
		X + 30.f * S, Y + 100.f * S, UI::White, 0.85f);
}

void AMRHUD::DrawIntro()
{
	// Opening screen: the logo over black for a few seconds, once per run of the game (not again after map travel).
	static double StartTime = -1.0;
	static bool bDone = FParse::Param(FCommandLine::Get(), TEXT("NoMenu")) || FParse::Param(FCommandLine::Get(), TEXT("NoIntro"));
	if (bDone) return;
	const double Now = FPlatformTime::Seconds();
	if (StartTime < 0.0) StartTime = Now;
	const float T = static_cast<float>(Now - StartTime);
	const float FadeIn = 0.5f, Hold = 2.6f, FadeOut = 0.9f;
	if (T > Hold + FadeOut) { bDone = true; LogoTexture = nullptr; return; }
	if (!bLogoTried)
	{
		bLogoTried = true;
		LogoTexture = FImageUtils::ImportFileAsTexture2D(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Brand"), TEXT("Logo.png")));
	}
	const float Out = T <= Hold ? 1.f : 1.f - (T - Hold) / FadeOut; // whole screen fades away at the end
	const float In = FMath::Clamp(T / FadeIn, 0.f, 1.f);              // logo fades in at the start
	const float CX = Canvas->ClipX * 0.5f, CY = Canvas->ClipY * 0.5f;
	DrawRect(FLinearColor(0.03f, 0.04f, 0.065f, Out), 0, 0, Canvas->ClipX, Canvas->ClipY);
	const float Size = 380.f * S * (0.94f + 0.06f * FMath::InterpEaseOut(0.f, 1.f, FMath::Clamp(T / 1.2f, 0.f, 1.f), 2.f));
	if (LogoTexture)
	{
		FCanvasTileItem Tile(FVector2D(CX - Size * 0.5f, CY - Size * 0.5f - 70.f * S), LogoTexture->GetResource(), FVector2D(Size, Size), FLinearColor(1.f, 1.f, 1.f, In * Out));
		Tile.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Tile);
	}
	Text(TEXT("MANCHESTER RIDE"), CX, CY + 150.f * S, FLinearColor(1.f, 1.f, 1.f, In * Out), 2.2f, false, true, GEngine->GetLargeFont());
	Text(TEXT("Open-world riding across Greater Manchester"), CX, CY + 215.f * S, FLinearColor(UI::Accent.R, UI::Accent.G, UI::Accent.B, In * Out), 1.1f, false, true);
}

void AMRHUD::DrawMenu(AMRPlayerController* PC)
{
	const bool bGarage = PC->GetMenuScreen() == EMRMenuScreen::Garage || PC->GetMenuScreen() == EMRMenuScreen::Rider; // side panel so the rider preview stays visible
	const TArray<FMRMenuItem>& Items = PC->GetMenuItems();
	const float RowH = 40.f * S;
	const float W = (bGarage ? 760.f : 1000.f) * S;
	const float H = (110.f + Items.Num() * 40.f + 60.f) * S;
	const float X = bGarage ? 60.f * S : (Canvas->ClipX - W) * 0.5f; // garage panel on the left so the bike is visible
	const float Y = FMath::Max(40.f * S, (Canvas->ClipY - H) * 0.5f);

	if (!bGarage) Panel(0, 0, Canvas->ClipX, Canvas->ClipY, FLinearColor(0, 0, 0, 0.35f));
	Panel(X, Y, W, H, UI::PanelBg);
	Panel(X, Y, W, 4.f * S, UI::Accent);
	Text(PC->GetMenuTitle().ToString(), X + 30.f * S, Y + 28.f * S, UI::Accent, 1.4f, false, false, GEngine->GetLargeFont());

	float RY = Y + 100.f * S;
	for (int32 i = 0; i < Items.Num(); ++i)
	{
		const FMRMenuItem& I = Items[i];
		const bool bSel = i == PC->GetMenuSelection() && I.bEnabled;
		if (bSel) Panel(X + 16.f * S, RY - 4.f * S, W - 32.f * S, RowH, UI::Row);
		const FLinearColor LabelCol = !I.bEnabled ? UI::Dim : (bSel ? UI::Accent : UI::White);
		Text(I.Label.ToString(), X + 30.f * S, RY + 4.f * S, LabelCol, 1.05f);
		if (I.EditText)
		{
			// Text box: what you've typed (dots for passwords), a blinking cursor while it's selected.
			const float BX = X + W * 0.42f, BW = W * 0.58f - 30.f * S;
			Panel(BX, RY - 1.f * S, BW, RowH - 6.f * S, bSel ? FLinearColor(0, 0, 0, 0.75f) : FLinearColor(0, 0, 0, 0.45f));
			if (bSel) Panel(BX, RY + RowH - 9.f * S, BW, 2.f * S, UI::Accent);
			FString Shown = I.bPassword ? FString::ChrN(I.EditText->Len(), TEXT('*')) : *I.EditText;
			if (Shown.Len() > 40) Shown = TEXT("...") + Shown.Right(37);
			const bool bCaret = bSel && FMath::Fmod(FPlatformTime::Seconds(), 1.0) < 0.55;
			if (Shown.IsEmpty() && !bSel) Text(I.Value.ToString(), BX + 10.f * S, RY + 4.f * S, UI::Dim, 0.9f);
			else Text(Shown + (bCaret ? TEXT("_") : TEXT("")), BX + 10.f * S, RY + 4.f * S, UI::White, 0.95f);
		}
		else if (!I.Value.IsEmpty())
		{
			FString Value = I.Value.ToString();
			if (Value.Len() > 70) Value = Value.Left(67) + TEXT("...");
			Text(Value, X + W - 30.f * S, RY + 4.f * S, I.bEnabled ? UI::White : UI::Dim, 0.95f, true);
		}
		RY += RowH;
	}
	Text(PC->GetMenuFooter().ToString(), X + 30.f * S, Y + H - 40.f * S, UI::Dim, 0.85f);
}

void AMRHUD::DrawRidingHUD(AMRPlayerController* PC, AMRRideVehicle* V)
{
	const MRSim::FRideState& Sim = V->GetSimState();
	const MRSim::FVehicleParams& P = V->GetParams();
	const float CX = Canvas->ClipX, CY = Canvas->ClipY;

	// ---- Speedometer (bottom right) ----
	const float Mph = FMath::Abs(Sim.Speed) * 2.23694f;
	const float PX = CX - 360.f * S, PY = CY - 230.f * S;
	Panel(PX, PY, 330.f * S, 200.f * S, UI::PanelBg);
	Text(FString::Printf(TEXT("%d"), FMath::RoundToInt(Mph)), PX + 200.f * S, PY + 10.f * S, UI::White, 3.2f, true, false, GEngine->GetLargeFont());
	Text(TEXT("mph"), PX + 210.f * S, PY + 48.f * S, UI::Dim, 1.f);
	Text(FString::Printf(TEXT("%d km/h"), FMath::RoundToInt(FMath::Abs(Sim.Speed) * 3.6f)), PX + 210.f * S, PY + 75.f * S, UI::Dim, 0.85f);

	float Row = PY + 115.f * S;
	using MRSim::EPowertrain;
	if (P.Powertrain.Type == EPowertrain::Combustion)
	{
		const float RpmFrac = Sim.EngineRpm / P.Powertrain.RedlineRpm;
		Bar(PX + 20.f * S, Row, 290.f * S, 14.f * S, RpmFrac, RpmFrac > 0.9f ? UI::Bad : UI::Accent);
		Text(FString::Printf(TEXT("Gear %d   %d rpm%s"), Sim.Gear, FMath::RoundToInt(Sim.EngineRpm / 100.f) * 100, Sim.ClutchEngage < 0.5f ? TEXT("   CLUTCH") : TEXT("")),
			PX + 20.f * S, Row + 20.f * S, UI::White, 0.9f);
		const float FuelFrac = Sim.FuelLitres / FMath::Max(0.1f, P.Powertrain.FuelLitres);
		Bar(PX + 20.f * S, Row + 52.f * S, 290.f * S, 10.f * S, FuelFrac, FuelFrac < 0.15f ? UI::Bad : UI::Good);
		Text(FString::Printf(TEXT("Fuel %.1f L"), Sim.FuelLitres), PX + 310.f * S, Row + 64.f * S, UI::Dim, 0.75f, true);
	}
	else if (P.Powertrain.Type != EPowertrain::Pedal)
	{
		const float BattFrac = Sim.BatteryWh / FMath::Max(1.f, P.Powertrain.BatteryWh);
		Bar(PX + 20.f * S, Row, 290.f * S, 14.f * S, BattFrac, BattFrac < 0.15f ? UI::Bad : UI::Good);
		const UMRSaveGame* Profile = PC->GetProfile();
		static const TCHAR* Levels[] = { TEXT("ECO"), TEXT("TOUR"), TEXT("SPORT"), TEXT("TURBO") };
		const FString Mode = P.Powertrain.Type == EPowertrain::PedalAssist && Profile ? FString::Printf(TEXT("   Assist %s"), Levels[FMath::Clamp(Profile->AssistLevel, 0, 3)]) : FString();
		Text(FString::Printf(TEXT("Battery %d%%   %d W%s"), FMath::RoundToInt(BattFrac * 100.f), FMath::RoundToInt(Sim.MotorPowerW), *Mode), PX + 20.f * S, Row + 20.f * S, UI::White, 0.9f);
	}
	else
	{
		Text(TEXT("Pedal power"), PX + 20.f * S, Row + 10.f * S, UI::Dim, 0.9f);
	}

	// ---- Vehicle + wheelie (bottom left) ----
	FMRVehicleInfo Info;
	UMRVehicleLibrary::GetVehicleInfo(V->GetVehicleId(), Info);
	const float LX = 30.f * S, LY = CY - 200.f * S;
	Panel(LX, LY, 420.f * S, 170.f * S, UI::PanelBg);
	Text(Info.DisplayName.ToString(), LX + 18.f * S, LY + 12.f * S, UI::Accent, 1.1f);
	Text(Info.Category.ToString(), LX + 18.f * S, LY + 42.f * S, UI::Dim, 0.85f);
	const MRSim::FTrickTracker& T = V->GetTricks();
	if (T.IsWheelieActive())
	{
		Text(FString::Printf(TEXT("WHEELIE  %.1f m  %.1f s"), T.CurrentWheelieDistance(), T.CurrentWheelieTime()), LX + 18.f * S, LY + 72.f * S, UI::Good, 1.15f);
		const float Bp = MRSim::WheelieBalanceAngle(P);
		Bar(LX + 18.f * S, LY + 105.f * S, 380.f * S, 10.f * S, Sim.Pitch / (Bp * 1.3f), FMath::Abs(Sim.Pitch - Bp * 0.82f) < 0.08f ? UI::Good : UI::Accent);
	}
	else
	{
		Text(FString::Printf(TEXT("Best wheelie %.1f m   Score %d"), T.BestWheelieDistance(), T.TotalScore()), LX + 18.f * S, LY + 72.f * S, UI::White, 0.9f);
	}
	if (T.ComboCount() > 0)
	{
		Text(FString::Printf(TEXT("Combo x%d  %d pts"), T.ComboCount(), T.ComboPoints()), LX + 18.f * S, LY + 125.f * S, UI::Accent, 1.f);
	}

	// ---- Trick popups (top centre) ----
	float TY = 140.f * S;
	for (const FMRTrickPopup& Pop : V->GetTrickPopups())
	{
		const float Alpha = FMath::Clamp(3.f - Pop.TimeShown, 0.f, 1.f);
		const FLinearColor C = Pop.Points < 0 ? UI::Bad : UI::Accent;
		Text(FString::Printf(TEXT("%s  %+d"), *Pop.Label, Pop.Points), CX * 0.5f, TY, FLinearColor(C.R, C.G, C.B, Alpha), 1.3f, false, true);
		TY += 36.f * S;
	}

	if (Sim.bCrashed)
	{
		Text(TEXT("BAIL! Respawning..."), CX * 0.5f, CY * 0.42f, UI::Bad, 2.f, false, true, GEngine->GetLargeFont());
	}
}

void AMRHUD::DrawNameTags(AMRRideVehicle* Self)
{
	const FVector Cam = PlayerOwner->PlayerCameraManager ? PlayerOwner->PlayerCameraManager->GetCameraLocation() : Self->GetActorLocation();
	const FVector CamFwd = PlayerOwner->PlayerCameraManager ? PlayerOwner->PlayerCameraManager->GetCameraRotation().Vector() : FVector::ForwardVector;
	for (TActorIterator<AMRRideVehicle> It(GetWorld()); It; ++It)
	{
		AMRRideVehicle* Other = *It;
		if (Other == Self) continue;
		FString Name = Other->GetRiderDisplayName();
		if (Name.IsEmpty()) continue;
		if (const UMRServerClient* SC = UMRServerClient::Get(this))
		{
			for (const TPair<uint16, FMRServerRider>& R : SC->GetRiders())
			{
				if (R.Value.Actor.Get() == Other && SC->IsRiderTalking(R.Value)) Name = TEXT("(( ") + Name + TEXT(" ))");
			}
		}
		const FVector Head = Other->GetActorLocation() + FVector(0, 0, 190.f);
		const float Dist = FVector::Dist(Cam, Head) / 100.f;
		if (Dist > 400.f || FVector::DotProduct(Head - Cam, CamFwd) <= 0.f) continue;
		const FVector Screen = Project(Head, false);
		const float Alpha = FMath::Clamp(1.2f - Dist / 400.f, 0.25f, 1.f);
		const float Scale = FMath::Clamp(1.2f - Dist / 300.f, 0.6f, 1.1f);
		Text(Name, Screen.X, Screen.Y, FLinearColor(1.f, 1.f, 1.f, Alpha), Scale, false, true);
		Text(FString::Printf(TEXT("%d m"), FMath::RoundToInt(Dist)), Screen.X, Screen.Y + 24.f * S * Scale, FLinearColor(0.8f, 0.8f, 0.8f, Alpha * 0.8f), Scale * 0.75f, false, true);
	}
}

void AMRHUD::DrawMinimap(AMRRideVehicle* Self)
{
	const float Size = 280.f * S;
	const float MX = Canvas->ClipX - Size - 30.f * S, MY = 30.f * S;
	const float RangeCm = 30000.f; // half-width shown: 300 m
	Panel(MX, MY, Size, Size, UI::PanelBg);
	const FVector Me = Self->GetActorLocation();
	auto ToMap = [&](const FVector2D& W) { return FVector2D(MX + Size * 0.5f + (W.X - Me.X) / RangeCm * Size * 0.5f, MY + Size * 0.5f + (W.Y - Me.Y) / RangeCm * Size * 0.5f); };

	for (TActorIterator<AMRTestEnvironment> It(GetWorld()); It; ++It)
	{
		for (const AMRTestEnvironment::FArea& A : It->GetAreas())
		{
			const FVector2D Min = ToMap(A.Center - A.Size * 0.5f), Max = ToMap(A.Center + A.Size * 0.5f);
			const float X0 = FMath::Clamp(Min.X, MX, MX + Size), Y0 = FMath::Clamp(Min.Y, MY, MY + Size);
			const float X1 = FMath::Clamp(Max.X, MX, MX + Size), Y1 = FMath::Clamp(Max.Y, MY, MY + Size);
			if (X1 > X0 && Y1 > Y0) DrawRect(FLinearColor(A.Color.R * 2.f, A.Color.G * 2.f, A.Color.B * 2.f, 0.55f), X0, Y0, X1 - X0, Y1 - Y0);
		}
	}
	// Real map: roads, paths, water, rail and building outlines from the streamed geo cells.
	auto Clip = [&](FVector2D& A, FVector2D& B) -> bool // Liang-Barsky against the map square
	{
		const FVector2D D = B - A;
		double T0 = 0.0, T1 = 1.0;
		const double P[4] = { -D.X, D.X, -D.Y, D.Y };
		const double Q[4] = { A.X - MX, MX + Size - A.X, A.Y - MY, MY + Size - A.Y };
		for (int32 i = 0; i < 4; ++i)
		{
			if (FMath::IsNearlyZero(P[i])) { if (Q[i] < 0.0) return false; continue; }
			const double R = Q[i] / P[i];
			if (P[i] < 0.0) T0 = FMath::Max(T0, R); else T1 = FMath::Min(T1, R);
			if (T0 > T1) return false;
		}
		const FVector2D A0 = A;
		A = A0 + D * T0; B = A0 + D * T1;
		return true;
	};
	for (TActorIterator<AMRGeoWorld> It(GetWorld()); It; ++It)
	{
		const FBox2f View(FVector2f(Me.X - RangeCm, Me.Y - RangeCm), FVector2f(Me.X + RangeCm, Me.Y + RangeCm));
		// Draw in layers so roads sit on top of buildings and paths.
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			It->ForEachMapLine(View, [&](const AMRGeoWorld::FMapLine& L)
			{
				using EML = AMRGeoWorld::EMapLine;
				FLinearColor Col; float W; int32 LinePass;
				switch (L.Kind)
				{
				case EML::Building:  Col = FLinearColor(0.55f, 0.52f, 0.48f, 0.55f); W = 1.f; LinePass = 0; break;
				case EML::Water:     Col = FLinearColor(0.25f, 0.55f, 0.95f, 0.9f);  W = 3.f; LinePass = 0; break;
				case EML::Path:      Col = FLinearColor(0.75f, 0.70f, 0.60f, 0.6f);  W = 1.f; LinePass = 1; break;
				case EML::Rail:      Col = FLinearColor(0.60f, 0.45f, 0.70f, 0.8f);  W = 1.5f; LinePass = 1; break;
				case EML::Cycle:     Col = FLinearColor(0.95f, 0.35f, 0.30f, 0.9f);  W = 2.f; LinePass = 1; break;
				case EML::MinorRoad: Col = FLinearColor(0.92f, 0.92f, 0.92f, 0.9f);  W = 2.5f; LinePass = 2; break;
				default:             Col = FLinearColor(1.00f, 0.80f, 0.30f, 1.f);   W = 4.f; LinePass = 2; break;
				}
				if (LinePass != Pass) return;
				for (int32 i = 0; i + 1 < L.Pts.Num(); ++i)
				{
					FVector2D A = ToMap(FVector2D(L.Pts[i])), B = ToMap(FVector2D(L.Pts[i + 1]));
					if (Clip(A, B)) DrawLine(A.X, A.Y, B.X, B.Y, Col, W * S);
				}
			});
		}
	}
	// Waypoint route (blue) and the waypoint itself, pinned to the edge when it is off the minimap.
	const AMRPlayerController* NavPC = Cast<AMRPlayerController>(GetOwningPlayerController());
	if (NavPC && NavPC->HasWaypoint())
	{
		const TArray<FVector2f>& R = NavPC->GetRoute();
		if (R.Num() >= 2)
		{
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				FVector2D Prev = ToMap(FVector2D(NavPC->GetRouteProgressPoint()));
				for (int32 i = NavPC->GetRouteProgressSeg() + 1; i < R.Num(); ++i)
				{
					const FVector2D Next = ToMap(FVector2D(R[i]));
					FVector2D A = Prev, B = Next;
					if (Clip(A, B)) DrawLine(A.X, A.Y, B.X, B.Y, Pass == 0 ? FLinearColor(1, 1, 1, 0.8f) : UI::Route, (Pass == 0 ? 7.f : 4.f) * S);
					Prev = Next;
				}
			}
		}
		FVector2D Wp = ToMap(NavPC->GetWaypoint());
		Wp.X = FMath::Clamp(Wp.X, MX + 6.f * S, MX + Size - 6.f * S);
		Wp.Y = FMath::Clamp(Wp.Y, MY + 6.f * S, MY + Size - 6.f * S);
		DrawRect(FLinearColor(1, 1, 1), Wp.X - 6.f * S, Wp.Y - 6.f * S, 12.f * S, 12.f * S);
		DrawRect(UI::Route, Wp.X - 4.f * S, Wp.Y - 4.f * S, 8.f * S, 8.f * S);
	}
	for (TActorIterator<AMRRideVehicle> It(GetWorld()); It; ++It)
	{
		const FVector2D P = ToMap(FVector2D(It->GetActorLocation()));
		if (P.X < MX || P.X > MX + Size || P.Y < MY || P.Y > MY + Size) continue;
		const bool bMe = *It == Self;
		DrawRect(bMe ? UI::Accent : FLinearColor(0.3f, 0.6f, 1.f), P.X - 4.f * S, P.Y - 4.f * S, 8.f * S, 8.f * S);
	}
	// Heading tick for the player
	const FVector Fwd = Self->GetActorForwardVector();
	const FVector2D C = ToMap(FVector2D(Me));
	DrawLine(C.X, C.Y, C.X + Fwd.X * 18.f * S, C.Y + Fwd.Y * 18.f * S, UI::Accent, 2.f * S);
	Text(TEXT("N"), MX + Size * 0.5f, MY + 4.f * S, UI::Dim, 0.8f, false, true);

	// GPS readout: real WGS84 coordinates from the shared projection.
	const MRGeo::FLatLon LL = MRGeo::ToLatLon(MRGeo::UnrealToLocal(Me.X, Me.Y));
	FString AreaName = TEXT("Open ground");
	for (TActorIterator<AMRTestEnvironment> It(GetWorld()); It; ++It)
	{
		for (const AMRTestEnvironment::FArea& A : It->GetAreas())
		{
			if (A.Size.X >= 59000.f) continue; // skip the base area
			if (FMath::Abs(Me.X - A.Center.X) < A.Size.X * 0.5f && FMath::Abs(Me.Y - A.Center.Y) < A.Size.Y * 0.5f) AreaName = A.Name;
		}
	}
	for (TActorIterator<AMRGeoWorld> It(GetWorld()); It; ++It)
	{
		const FString Near = It->GetNearestPlaceName(Me);
		AreaName = Near.IsEmpty() ? TEXT("Greater Manchester") : FString::Printf(TEXT("Near %s"), *Near);
	}
	Text(FString::Printf(TEXT("%.5f%c %.5f%c   %s"), FMath::Abs(LL.Lat), LL.Lat >= 0 ? 'N' : 'S', FMath::Abs(LL.Lon), LL.Lon >= 0 ? 'E' : 'W',
		*UI::CompassPoint(Self->GetActorRotation().Yaw)), MX + Size, MY + Size + 8.f * S, UI::White, 0.8f, true);
	Text(AreaName, MX + Size, MY + Size + 32.f * S, UI::Dim, 0.8f, true);
	if (NavPC)
	{
		auto Dist = [](float Cm) { return Cm >= 100000.f ? FString::Printf(TEXT("%.1f km"), Cm / 100000.f) : FString::Printf(TEXT("%.0f m"), FMath::RoundToFloat(Cm / 1000.f) * 10.f); };
		if (NavPC->HasWaypoint())
		{
			const FString Msg = NavPC->IsRouteFailed() ? FString(TEXT("Waypoint: no road route, head for the marker"))
				: NavPC->GetRoute().Num() < 2 ? FString(TEXT("Waypoint: finding a route..."))
				: FString::Printf(TEXT("Waypoint  %s"), *Dist(NavPC->GetRouteRemainingCm()));
			Text(Msg, MX + Size, MY + Size + 56.f * S, UI::RouteText, 0.9f, true);
		}
		else if (GetWorld()->GetTimeSeconds() - NavPC->GetArrivedTime() < 4.f)
		{
			Text(TEXT("You have arrived"), MX + Size, MY + Size + 56.f * S, UI::Accent, 1.f, true);
		}
	}
}

void AMRHUD::DrawFullMap(AMRPlayerController* PC)
{
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	DrawRect(FLinearColor(0.93f, 0.92f, 0.89f, 1.f), 0, 0, W, H); // paper
	const FVector2D C = PC->GetMapCentre();
	const float Cpp = PC->GetMapCmPerPixel(); // cm per screen pixel
	const float Mpp = Cpp / 100.f;
	auto ToScreen = [&](const FVector2f& P) { return FVector2D(W * 0.5f + (P.X - C.X) / Cpp, H * 0.5f + (P.Y - C.Y) / Cpp); };
	const FBox2f View(FVector2f(C.X - W * 0.5f * Cpp, C.Y - H * 0.5f * Cpp), FVector2f(C.X + W * 0.5f * Cpp, C.Y + H * 0.5f * Cpp));

	const TSharedPtr<const FMRMapData> Map = FMRMapData::Get();
	if (!Map.IsValid())
	{
		Text(TEXT("Loading the map of Greater Manchester..."), W * 0.5f, H * 0.5f, FLinearColor(0.1f, 0.1f, 0.12f), 1.4f, false, true);
		return;
	}

	// Close-up: building footprints from the streamed cells (the real outlines), under the roads.
	if (Mpp < 2.5f)
	{
		for (TActorIterator<AMRGeoWorld> It(GetWorld()); It; ++It)
		{
			It->ForEachMapLine(View, [&](const AMRGeoWorld::FMapLine& L)
			{
				if (L.Kind != AMRGeoWorld::EMapLine::Building) return;
				for (int32 i = 0; i + 1 < L.Pts.Num(); ++i)
				{
					const FVector2D A = ToScreen(L.Pts[i]), B = ToScreen(L.Pts[i + 1]);
					DrawLine(A.X, A.Y, B.X, B.Y, FLinearColor(0.62f, 0.56f, 0.5f), 1.5f);
				}
			});
		}
	}

	// Roads, rail and water: casing first, then fill, so junctions read cleanly. Streets appear as you zoom in.
	struct FStyle { FLinearColor Fill; float WidthM; float MinPx; float MaxMpp; };
	const FStyle Styles[FMRMapData::NumKinds] = {
		{ FLinearColor(0.16f, 0.38f, 0.82f), 22.f, 2.5f, 1e9f }, // motorway (UK blue)
		{ FLinearColor(0.97f, 0.62f, 0.25f), 14.f, 1.6f, 120.f }, // A/B roads
		{ FLinearColor(1.f, 1.f, 1.f), 9.f, 1.f, 9.f },           // streets
		{ FLinearColor(0.35f, 0.33f, 0.4f), 4.f, 1.2f, 60.f },    // rail
		{ FLinearColor(0.55f, 0.3f, 0.6f), 4.f, 1.2f, 30.f },     // Metrolink tram
		{ FLinearColor(0.45f, 0.65f, 0.9f), 18.f, 1.6f, 200.f },  // rivers
		{ FLinearColor(0.45f, 0.65f, 0.9f), 10.f, 1.2f, 60.f },   // canals
	};
	static const uint8 Order[] = { FMRMapData::River, FMRMapData::Canal, FMRMapData::Rail, FMRMapData::Tram, FMRMapData::Street, FMRMapData::MajorRoad, FMRMapData::Motorway };
	for (const uint8 K : Order)
	{
		const FStyle& St = Styles[K];
		if (Mpp > St.MaxMpp) continue;
		const float Px = FMath::Max(St.WidthM / Mpp, St.MinPx);
		const bool bCased = K <= FMRMapData::Street && Px > 2.5f;
		for (int32 Pass = bCased ? 0 : 1; Pass < 2; ++Pass)
		{
			const FLinearColor Col = Pass == 0 ? FLinearColor(0.55f, 0.5f, 0.45f) : St.Fill;
			const float Wd = Pass == 0 ? Px + 2.f : Px;
			Map->ForEachLine(View, [&](const FMRMapData::FLine& L)
			{
				if (L.Kind != K) return;
				const FVector2D Size = FVector2D(L.Bounds.GetSize()) / Cpp;
				if (Size.X < 1.5f && Size.Y < 1.5f) return; // sub-pixel
				FVector2D Prev = ToScreen(Map->Points[L.First]);
				for (int32 i = 1; i < L.Num; ++i)
				{
					const FVector2D P = ToScreen(Map->Points[L.First + i]);
					DrawLine(Prev.X, Prev.Y, P.X, P.Y, Col, Wd);
					Prev = P;
				}
			});
		}
	}

	// Labels, decluttered on a coarse screen grid so names never pile up.
	TSet<FIntPoint> Used;
	auto Claim = [&](const FVector2D& P, float HalfW, float HalfH)
	{
		const int32 X0 = FMath::FloorToInt((P.X - HalfW) / 40.f), X1 = FMath::FloorToInt((P.X + HalfW) / 40.f);
		const int32 Y0 = FMath::FloorToInt((P.Y - HalfH) / 20.f), Y1 = FMath::FloorToInt((P.Y + HalfH) / 20.f);
		for (int32 y = Y0; y <= Y1; ++y) for (int32 x = X0; x <= X1; ++x) if (Used.Contains(FIntPoint(x, y))) return false;
		for (int32 y = Y0; y <= Y1; ++y) for (int32 x = X0; x <= X1; ++x) Used.Add(FIntPoint(x, y));
		return true;
	};
	UFont* Font = GEngine->GetMediumFont();
	// Places: cities and towns always, suburbs/villages when closer, neighbourhoods close up.
	for (int32 Rank = 0; Rank <= 3; ++Rank)
	{
		const float MaxMpp = Rank == 0 ? 1e9f : Rank == 1 ? 120.f : Rank == 2 ? 30.f : 8.f;
		if (Mpp > MaxMpp) continue;
		const float Scale = Rank == 0 ? 1.6f : Rank == 1 ? 1.3f : Rank == 2 ? 1.05f : 0.85f;
		for (const FMRMapData::FPlace& Pl : Map->Places)
		{
			if (Pl.Rank != Rank || !View.IsInside(Pl.P)) continue;
			const FVector2D P = ToScreen(Pl.P);
			float Tw = 0.f, Th = 0.f;
			GetTextSize(Pl.Name, Tw, Th, Font, Scale * S);
			if (!Claim(P, Tw * 0.5f, Th * 0.5f)) continue;
			Text(Pl.Name.ToUpper(), P.X, P.Y - Th * 0.5f, Rank <= 1 ? FLinearColor(0.12f, 0.12f, 0.15f) : FLinearColor(0.3f, 0.3f, 0.35f), Scale, false, true, Font);
		}
	}
	// Street names along the road.
	if (Mpp < 6.f)
	{
		Map->ForEachLabel(View, [&](const FMRMapData::FLabel& L)
		{
			if (L.Kind == FMRMapData::Street && Mpp > 2.5f) return;
			const FVector2D P = ToScreen(L.P);
			float Tw = 0.f, Th = 0.f;
			GetTextSize(L.Name, Tw, Th, Font, 0.7f * S);
			if (!Claim(P, Tw * 0.5f, Th * 0.5f)) return;
			// Canvas text can't rotate, so names sit level, centred on the road; a light halo keeps them readable.
			DrawRect(FLinearColor(0.97f, 0.96f, 0.93f, 0.8f), P.X - Tw * 0.5f - 3.f, P.Y - Th * 0.5f, Tw + 6.f, Th);
			DrawText(L.Name, FLinearColor(0.15f, 0.15f, 0.18f), P.X - Tw * 0.5f, P.Y - Th * 0.5f, Font, 0.7f * S);
		});
	}

	// Waypoint route: a blue line along the roads from where you are, with a white edge so it reads over any road colour.
	if (PC->HasWaypoint())
	{
		const TArray<FVector2f>& R = PC->GetRoute();
		if (R.Num() >= 2)
		{
			const float Margin = 50.f;
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				const FLinearColor Col = Pass == 0 ? FLinearColor(1.f, 1.f, 1.f, 0.9f) : UI::Route;
				const float Wd = (Pass == 0 ? 14.f : 8.f) * S;
				FVector2D Prev = ToScreen(PC->GetRouteProgressPoint());
				for (int32 i = PC->GetRouteProgressSeg() + 1; i < R.Num(); ++i)
				{
					const FVector2D P = ToScreen(R[i]);
					const bool bOff = (Prev.X < -Margin && P.X < -Margin) || (Prev.X > W + Margin && P.X > W + Margin) || (Prev.Y < -Margin && P.Y < -Margin) || (Prev.Y > H + Margin && P.Y > H + Margin);
					if (!bOff) DrawLine(Prev.X, Prev.Y, P.X, P.Y, Col, Wd);
					Prev = P;
				}
			}
		}
		// Pin
		const FVector2D Wp = ToScreen(FVector2f(PC->GetWaypoint()));
		DrawLine(Wp.X, Wp.Y, Wp.X, Wp.Y - 22.f * S, FLinearColor(0.05f, 0.1f, 0.25f), 3.f * S);
		DrawRect(FLinearColor(1, 1, 1), Wp.X - 10.f * S, Wp.Y - 34.f * S, 20.f * S, 20.f * S);
		DrawRect(UI::Route, Wp.X - 8.f * S, Wp.Y - 32.f * S, 16.f * S, 16.f * S);
	}
	// Crosshair: Enter / X drops the waypoint here.
	DrawLine(W * 0.5f - 10.f * S, H * 0.5f, W * 0.5f + 10.f * S, H * 0.5f, FLinearColor(0.1f, 0.1f, 0.1f, 0.7f), 1.5f);
	DrawLine(W * 0.5f, H * 0.5f - 10.f * S, W * 0.5f, H * 0.5f + 10.f * S, FLinearColor(0.1f, 0.1f, 0.1f, 0.7f), 1.5f);

	// Riders: you (arrow) and everyone else (dots with names).
	for (TActorIterator<AMRRideVehicle> It(GetWorld()); It; ++It)
	{
		const FVector2D P = ToScreen(FVector2f(FVector2D(It->GetActorLocation())));
		const bool bMe = *It == PC->GetVehicle();
		if (bMe)
		{
			const FVector F = It->GetActorForwardVector();
			const FVector2D Fw(F.X, F.Y), Rt(-F.Y, F.X);
			const FVector2D Tip = P + Fw * 16.f * S, L = P - Fw * 10.f * S + Rt * 9.f * S, R = P - Fw * 10.f * S - Rt * 9.f * S;
			for (int32 k = 0; k < 2; ++k)
			{
				const FLinearColor Col = k == 0 ? FLinearColor(0, 0, 0) : UI::Accent;
				const float Th = k == 0 ? 6.f * S : 3.f * S;
				DrawLine(Tip.X, Tip.Y, L.X, L.Y, Col, Th); DrawLine(L.X, L.Y, R.X, R.Y, Col, Th); DrawLine(R.X, R.Y, Tip.X, Tip.Y, Col, Th);
			}
		}
		else
		{
			DrawRect(FLinearColor(0.2f, 0.45f, 1.f), P.X - 5.f * S, P.Y - 5.f * S, 10.f * S, 10.f * S);
			const FString Who = It->GetRiderDisplayName();
			if (!Who.IsEmpty()) Text(Who, P.X, P.Y + 8.f * S, FLinearColor(0.1f, 0.2f, 0.6f), 0.75f, false, true);
		}
	}

	// Frame: title, where you are, scale bar, north arrow, controls.
	Panel(0, 0, W, 70.f * S, FLinearColor(0.02f, 0.025f, 0.03f, 0.85f));
	Text(TEXT("GREATER MANCHESTER"), 30.f * S, 16.f * S, UI::Accent, 1.5f);
	if (const AMRRideVehicle* Me = PC->GetVehicle())
	{
		FString Near;
		for (TActorIterator<AMRGeoWorld> It(GetWorld()); It; ++It) Near = It->GetNearestPlaceName(Me->GetActorLocation());
		const MRGeo::FLatLon LL = MRGeo::ToLatLon(MRGeo::UnrealToLocal(Me->GetActorLocation().X, Me->GetActorLocation().Y));
		Text(FString::Printf(TEXT("You are near %s   %.5f N  %.5f W"), Near.IsEmpty() ? TEXT("Manchester") : *Near, LL.Lat, -LL.Lon), W - 30.f * S, 22.f * S, UI::White, 0.95f, true);
	}
	// Scale bar: a round distance about 150 px long.
	{
		const float Target = 150.f * S * Mpp;
		const float Steps[] = { 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000 };
		float Len = Steps[0];
		for (const float St : Steps) if (St <= Target) Len = St;
		const float Px = Len / Mpp;
		const float X0 = 30.f * S, Y0 = H - 50.f * S;
		DrawRect(FLinearColor(0.1f, 0.1f, 0.1f), X0, Y0, Px, 4.f * S);
		Text(Len >= 1000.f ? FString::Printf(TEXT("%.0f km"), Len / 1000.f) : FString::Printf(TEXT("%.0f m"), Len), X0 + Px + 10.f * S, Y0 - 10.f * S, FLinearColor(0.1f, 0.1f, 0.1f), 0.9f);
	}
	Text(TEXT("N ^"), W - 50.f * S, 90.f * S, FLinearColor(0.1f, 0.1f, 0.1f), 1.1f, false, true);
	if (PC->HasWaypoint())
	{
		const float Km = PC->GetRouteRemainingCm() / 100000.f;
		const FString Msg = PC->IsRouteFailed() ? FString(TEXT("No road route to that spot: try a point nearer a road"))
			: PC->GetRoute().Num() < 2 ? FString(TEXT("Finding a route..."))
			: FString::Printf(TEXT("Waypoint: %.1f km by road  (about %d min by bike at 20 km/h)"), Km, FMath::Max(1, FMath::RoundToInt(Km / 20.f * 60.f)));
		Panel(20.f * S, 80.f * S, 620.f * S, 34.f * S, FLinearColor(0.02f, 0.025f, 0.03f, 0.8f));
		Text(Msg, 32.f * S, 86.f * S, UI::RouteText, 0.9f);
	}
	Panel(0, H - 28.f * S, W, 28.f * S, FLinearColor(0.02f, 0.025f, 0.03f, 0.75f));
	Text(TEXT("WASD / arrows: move     Wheel, Q / E: zoom     Space / A: centre on you     Click / Enter / X: set waypoint     T / L3: teleport to it     Right-click / Backspace / Y: clear     M / Esc: close"),
		W * 0.5f, H - 24.f * S, UI::Dim, 0.75f, false, true);
	Text(TEXT("Map data (c) OpenStreetMap contributors"), W - 20.f * S, H - 52.f * S, FLinearColor(0.3f, 0.3f, 0.3f), 0.65f, true);
}

void AMRHUD::DrawSessionInfo(AMRPlayerController* PC)
{
	const UMRGameInstance* GI = PC->GetMRGameInstance();
	FString Line;
	const UMRServerClient* SC = UMRServerClient::Get(this);
	if (SC && SC->IsOnline())
	{
		Line = FString::Printf(TEXT("%s   %d/%d riders   ping %d ms   T chat  V talk  Tab riders"), *SC->GetServerName(), SC->GetRiders().Num() + 1, SC->GetMaxPlayers(), SC->GetPingMs());
	}
	else if (GI && GI->IsInMultiplayer())
	{
		const AMRGameState* GS = GetWorld()->GetGameState<AMRGameState>();
		const APlayerState* PS = PC->PlayerState;
		Line = FString::Printf(TEXT("%s   %d/%d riders%s"), GS ? *GS->SessionName : TEXT(""), GS ? GS->PlayerArray.Num() : 0, GS ? GS->MaxPlayers : 0,
			(PS && !GI->IsHosting()) ? *FString::Printf(TEXT("   ping %d ms"), FMath::RoundToInt(PS->GetPingInMilliseconds())) : TEXT("   (host)"));
	}
	else
	{
		Line = TEXT("Solo free roam - Esc / Menu for multiplayer");
	}
	if (const AMRWorldEnvironment* Env = AMRWorldEnvironment::Get(this))
	{
		const float H = Env->GetTimeOfDayHours();
		Line += FString::Printf(TEXT("   |   %02d:%02d  %s%s"), FMath::FloorToInt(H), FMath::FloorToInt(FMath::Frac(H) * 60.f),
			*AMRWorldEnvironment::WeatherName(Env->GetWeather()).ToString(),
			Env->GetRoadWetness() > 0.05f ? *FString::Printf(TEXT("  (roads %d%% wet)"), FMath::RoundToInt(Env->GetRoadWetness() * 100.f)) : TEXT(""));
	}
	Text(Line, 30.f * S, 24.f * S, UI::Dim, 0.85f);
	if (GI && !PC->IsMenuOpen() && GI->GetLastError() != EMRConnectError::None)
	{
		Text(GI->GetLastErrorText().ToString(), Canvas->ClipX * 0.5f, 60.f * S, UI::Bad, 0.9f, false, true);
	}
	else if (SC && !PC->IsMenuOpen() && SC->GetState() == EMRServerState::Failed && !SC->GetErrorText().IsEmpty())
	{
		Text(SC->GetErrorText().ToString().Left(150), Canvas->ClipX * 0.5f, 60.f * S, UI::Bad, 0.9f, false, true);
	}
}

void AMRHUD::DrawCompetition()
{
	const AMRGameState* GS = GetWorld()->GetGameState<AMRGameState>();
	if (!GS) return;
	if (GS->Competition == EMRCompetition::None)
	{
		return;
	}
	TArray<const AMRPlayerState*> Ranked;
	for (APlayerState* P : GS->PlayerArray) if (const AMRPlayerState* MP = Cast<AMRPlayerState>(P)) Ranked.Add(MP);
	Ranked.Sort([](const AMRPlayerState& A, const AMRPlayerState& B) { return A.CompetitionScore > B.CompetitionScore; });
	const float X = Canvas->ClipX * 0.5f - 220.f * S, Y = 70.f * S;
	Panel(X, Y, 440.f * S, (60.f + FMath::Min(5, Ranked.Num()) * 28.f) * S, UI::PanelBg);
	const int32 Left = FMath::CeilToInt(GS->GetCompetitionTimeLeft());
	Text(FString::Printf(TEXT("%s  %d:%02d"), *AMRGameState::CompetitionName(GS->Competition).ToString(), Left / 60, Left % 60), X + 220.f * S, Y + 10.f * S, UI::Accent, 1.1f, false, true);
	for (int32 i = 0; i < Ranked.Num() && i < 5; ++i)
	{
		const bool bWheelie = GS->Competition == EMRCompetition::LongestWheelie;
		Text(FString::Printf(TEXT("%d. %s"), i + 1, *Ranked[i]->GetPlayerName()), X + 20.f * S, Y + (44.f + i * 28.f) * S, UI::White, 0.9f);
		Text(bWheelie ? FString::Printf(TEXT("%.1f m"), Ranked[i]->CompetitionScore) : FString::Printf(TEXT("%d"), FMath::RoundToInt(Ranked[i]->CompetitionScore)),
			X + 420.f * S, Y + (44.f + i * 28.f) * S, UI::White, 0.9f, true);
	}
}

void AMRHUD::DrawPlayerList()
{
	const AMRGameState* GS = GetWorld()->GetGameState<AMRGameState>();
	if (!GS) return;
	const float W = 900.f * S, RowH = 32.f * S;
	const float X = (Canvas->ClipX - W) * 0.5f, Y = 160.f * S;
	Panel(X, Y, W, (70.f * S) + GS->PlayerArray.Num() * RowH, UI::PanelBg);
	Text(TEXT("Rider"), X + 20.f * S, Y + 16.f * S, UI::Accent, 0.9f);
	Text(TEXT("Vehicle"), X + 300.f * S, Y + 16.f * S, UI::Accent, 0.9f);
	Text(TEXT("Best wheelie"), X + 620.f * S, Y + 16.f * S, UI::Accent, 0.9f, true);
	Text(TEXT("Score"), X + 760.f * S, Y + 16.f * S, UI::Accent, 0.9f, true);
	Text(TEXT("Ping"), X + W - 20.f * S, Y + 16.f * S, UI::Accent, 0.9f, true);
	float RY = Y + 54.f * S;
	for (APlayerState* P : GS->PlayerArray)
	{
		const AMRPlayerState* MP = Cast<AMRPlayerState>(P);
		if (!MP) continue;
		FMRVehicleInfo VI;
		UMRVehicleLibrary::GetVehicleInfo(MP->VehicleId, VI);
		Text(MP->GetPlayerName(), X + 20.f * S, RY, UI::White, 0.85f);
		Text(VI.DisplayName.ToString(), X + 300.f * S, RY, UI::Dim, 0.85f);
		Text(FString::Printf(TEXT("%.1f m"), MP->BestWheelieDistance), X + 620.f * S, RY, UI::White, 0.85f, true);
		Text(FString::Printf(TEXT("%d"), MP->TrickScore), X + 760.f * S, RY, UI::White, 0.85f, true);
		Text(FString::Printf(TEXT("%d"), FMath::RoundToInt(MP->GetPingInMilliseconds())), X + W - 20.f * S, RY, UI::Dim, 0.85f, true);
		RY += RowH;
	}
	if (!GS->LastWinner.IsEmpty())
	{
		Text(GS->LastWinner, X + W * 0.5f, RY + 6.f * S, UI::Accent, 0.85f, false, true);
	}
}
