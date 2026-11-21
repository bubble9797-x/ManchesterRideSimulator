#include "Vehicles/MRVehicleMeshBuilder.h"

using MRSim::EVehicleCategory;
using MRSim::ETyre;

// =====================================================================================
// Shape kit
// =====================================================================================

namespace
{
	void Basis(const FVector& D, FVector& U, FVector& W)
	{
		const FVector Up = FMath::Abs(D.Z) < 0.9f ? FVector::UpVector : FVector::ForwardVector;
		U = FVector::CrossProduct(Up, D).GetSafeNormal();
		W = FVector::CrossProduct(D, U);
	}

	int32 AddV(FMRMeshSection& S, const FVector& P, const FVector& N)
	{
		S.V.Add(P);
		S.N.Add(N);
		return S.V.Num() - 1;
	}

	// Unreal treats (P1 - P2) ^ (P0 - P2) as the front face; orient each triangle along its vertex normals.
	void AddTri(FMRMeshSection& S, int32 A, int32 B, int32 C)
	{
		const FVector F = FVector::CrossProduct(S.V[B] - S.V[C], S.V[A] - S.V[C]);
		const FVector Want = S.N[A] + S.N[B] + S.N[C];
		if (FVector::DotProduct(F, Want) < 0.f) Swap(B, C);
		S.T.Add(A); S.T.Add(B); S.T.Add(C);
	}

	void Quad(FMRMeshSection& S, int32 A, int32 B, int32 C, int32 D)
	{
		AddTri(S, A, B, C);
		AddTri(S, A, C, D);
	}

	FVector Circ(float A) { return FVector(FMath::Cos(A), 0.f, FMath::Sin(A)); } // unit vector in the XZ (wheel) plane

	/** Curved strip (mudguard) in the XZ plane around C, angles from +X towards +Z, both faces. */
	void ArcStrip(FMRMeshSection& S, const FVector& C, float R, float Width, float A0, float A1, int32 Seg = 20, float Thick = 0.4f)
	{
		for (int32 Face = 0; Face < 2; ++Face)
		{
			const float Rf = Face == 0 ? R + Thick * 0.5f : R - Thick * 0.5f;
			const float Sgn = Face == 0 ? 1.f : -1.f;
			const int32 Base = S.V.Num();
			for (int32 i = 0; i <= Seg; ++i)
			{
				const FVector Dir = Circ(FMath::Lerp(A0, A1, static_cast<float>(i) / Seg));
				AddV(S, C + Dir * Rf - FVector(0, Width * 0.5f, 0), Dir * Sgn);
				AddV(S, C + Dir * Rf + FVector(0, Width * 0.5f, 0), Dir * Sgn);
			}
			for (int32 i = 0; i < Seg; ++i) Quad(S, Base + 2 * i, Base + 2 * i + 1, Base + 2 * i + 3, Base + 2 * i + 2);
		}
	}

	void TubeRaw(FMRMeshSection& S, const FVector& A, const FVector& B, float R0, float R1, int32 Sides)
	{
		FVector D = B - A;
		const float Len = D.Size();
		if (Len < 0.01f) return;
		D /= Len;
		FVector U, W;
		Basis(D, U, W);
		const int32 Base = S.V.Num();
		for (int32 i = 0; i <= Sides; ++i)
		{
			const float Ang = 2.f * PI * i / Sides;
			const FVector Dir = U * FMath::Cos(Ang) + W * FMath::Sin(Ang);
			AddV(S, A + Dir * R0, Dir);
			AddV(S, B + Dir * R1, Dir);
		}
		for (int32 i = 0; i < Sides; ++i)
		{
			const int32 I0 = Base + 2 * i;
			Quad(S, I0, I0 + 1, I0 + 3, I0 + 2);
		}
	}
}

namespace MRMeshKit
{
	void Tube(FMRMeshSection& S, const FVector& A, const FVector& B, float R0, float R1, int32 Sides, bool bCaps)
	{
		FVector D = B - A;
		const float Len = D.Size();
		if (Len < 0.01f) return;
		D /= Len;
		TubeRaw(S, A, B, R0, R1, Sides);
		if (!bCaps) return;
		FVector U, W;
		Basis(D, U, W);
		for (int32 End = 0; End < 2; ++End)
		{
			const FVector P = End == 0 ? A : B;
			const FVector N = End == 0 ? -D : D;
			const float R = End == 0 ? R0 : R1;
			if (R <= 0.f) continue;
			const int32 C = AddV(S, P, N);
			const int32 Ring = S.V.Num();
			for (int32 i = 0; i <= Sides; ++i)
			{
				const float Ang = 2.f * PI * i / Sides;
				AddV(S, P + (U * FMath::Cos(Ang) + W * FMath::Sin(Ang)) * R, N);
			}
			for (int32 i = 0; i < Sides; ++i) AddTri(S, C, Ring + i, Ring + i + 1);
		}
	}

	void PathTube(FMRMeshSection& S, const TArray<FVector>& Pts, float R, int32 Sides, bool bCaps)
	{
		for (int32 i = 0; i + 1 < Pts.Num(); ++i) TubeRaw(S, Pts[i], Pts[i + 1], R, R, Sides);
		const int32 Rings = FMath::Max(3, Sides / 2);
		for (int32 i = 0; i < Pts.Num(); ++i)
		{
			const bool bEnd = i == 0 || i == Pts.Num() - 1;
			if (bEnd && !bCaps) continue;
			Ellipsoid(S, Pts[i], FVector(R), FQuat::Identity, Sides, Rings); // rounded joints and ends
		}
	}

	void Torus(FMRMeshSection& S, const FVector& C, const FVector& Axis, float MajorR, float MinorR, int32 Seg, int32 Sides, float MinorAxisScale)
	{
		FVector U, W;
		Basis(Axis, U, W);
		const int32 Base = S.V.Num();
		for (int32 i = 0; i <= Seg; ++i)
		{
			const float Th = 2.f * PI * i / Seg;
			const FVector Rad = U * FMath::Cos(Th) + W * FMath::Sin(Th);
			const FVector Ctr = C + Rad * MajorR;
			for (int32 j = 0; j <= Sides; ++j)
			{
				const float Ph = 2.f * PI * j / Sides;
				const float Cp = FMath::Cos(Ph), Sp = FMath::Sin(Ph);
				const FVector P = Ctr + Rad * (Cp * MinorR) + Axis * (Sp * MinorR * MinorAxisScale);
				const FVector N = (Rad * Cp + Axis * (Sp / FMath::Max(MinorAxisScale, 0.05f))).GetSafeNormal();
				AddV(S, P, N);
			}
		}
		for (int32 i = 0; i < Seg; ++i)
			for (int32 j = 0; j < Sides; ++j)
			{
				const int32 A = Base + i * (Sides + 1) + j;
				const int32 B = Base + (i + 1) * (Sides + 1) + j;
				Quad(S, A, B, B + 1, A + 1);
			}
	}

	void Disc(FMRMeshSection& S, const FVector& C, const FVector& Axis, float Rin, float Rout, float Thick, int32 Seg)
	{
		FVector U, W;
		Basis(Axis, U, W);
		const float H = Thick * 0.5f;
		auto Dir = [&](int32 i) { const float A = 2.f * PI * i / Seg; return U * FMath::Cos(A) + W * FMath::Sin(A); };
		for (int32 Face = 0; Face < 2; ++Face)
		{
			const float Sgn = Face == 0 ? 1.f : -1.f;
			const FVector N = Axis * Sgn;
			const FVector Ctr = C + Axis * (H * Sgn);
			if (Rin > 0.f)
			{
				const int32 Base = S.V.Num();
				for (int32 i = 0; i <= Seg; ++i) { AddV(S, Ctr + Dir(i) * Rin, N); AddV(S, Ctr + Dir(i) * Rout, N); }
				for (int32 i = 0; i < Seg; ++i) Quad(S, Base + 2 * i, Base + 2 * i + 1, Base + 2 * i + 3, Base + 2 * i + 2);
			}
			else
			{
				const int32 Cv = AddV(S, Ctr, N);
				const int32 Base = S.V.Num();
				for (int32 i = 0; i <= Seg; ++i) AddV(S, Ctr + Dir(i) * Rout, N);
				for (int32 i = 0; i < Seg; ++i) AddTri(S, Cv, Base + i, Base + i + 1);
			}
		}
		for (int32 Wall = 0; Wall < (Rin > 0.f ? 2 : 1); ++Wall)
		{
			const float R = Wall == 0 ? Rout : Rin;
			const float Sgn = Wall == 0 ? 1.f : -1.f;
			const int32 Base = S.V.Num();
			for (int32 i = 0; i <= Seg; ++i)
			{
				AddV(S, C + Axis * H + Dir(i) * R, Dir(i) * Sgn);
				AddV(S, C - Axis * H + Dir(i) * R, Dir(i) * Sgn);
			}
			for (int32 i = 0; i < Seg; ++i) Quad(S, Base + 2 * i, Base + 2 * i + 1, Base + 2 * i + 3, Base + 2 * i + 2);
		}
	}

	void Box(FMRMeshSection& S, const FVector& C, const FQuat& Q, const FVector& Half)
	{
		const FVector Ax[3] = { Q.GetAxisX(), Q.GetAxisY(), Q.GetAxisZ() };
		for (int32 K = 0; K < 3; ++K)
			for (int32 Sgn = -1; Sgn <= 1; Sgn += 2)
			{
				const FVector N = Ax[K] * static_cast<float>(Sgn);
				const FVector A = Ax[(K + 1) % 3] * Half[(K + 1) % 3];
				const FVector B = Ax[(K + 2) % 3] * Half[(K + 2) % 3];
				const FVector Ctr = C + N * Half[K];
				const int32 V0 = AddV(S, Ctr - A - B, N), V1 = AddV(S, Ctr + A - B, N), V2 = AddV(S, Ctr + A + B, N), V3 = AddV(S, Ctr - A + B, N);
				Quad(S, V0, V1, V2, V3);
			}
	}

	void Bar(FMRMeshSection& S, const FVector& A, const FVector& B, float Width, float Height, const FVector& UpHint)
	{
		const FVector D = B - A;
		const float Len = D.Size();
		if (Len < 0.01f) return;
		FVector Up = UpHint;
		if (FMath::Abs(FVector::DotProduct(Up.GetSafeNormal(), D / Len)) > 0.98f) Up = FVector::RightVector;
		const FQuat Q = FRotationMatrix::MakeFromXZ(D, Up).ToQuat();
		Box(S, (A + B) * 0.5f, Q, FVector(Len * 0.5f, Width * 0.5f, Height * 0.5f));
	}

	void Ellipsoid(FMRMeshSection& S, const FVector& C, const FVector& Radii, const FQuat& Q, int32 Seg, int32 Rings)
	{
		const int32 Base = S.V.Num();
		for (int32 r = 0; r <= Rings; ++r)
		{
			const float Phi = PI * r / Rings;
			for (int32 s = 0; s <= Seg; ++s)
			{
				const float Th = 2.f * PI * s / Seg;
				const FVector Unit(FMath::Sin(Phi) * FMath::Cos(Th), FMath::Sin(Phi) * FMath::Sin(Th), FMath::Cos(Phi));
				const FVector P = Unit * Radii;
				const FVector N = FVector(Unit.X / Radii.X, Unit.Y / Radii.Y, Unit.Z / Radii.Z).GetSafeNormal();
				AddV(S, C + Q.RotateVector(P), Q.RotateVector(N));
			}
		}
		for (int32 r = 0; r < Rings; ++r)
			for (int32 s = 0; s < Seg; ++s)
			{
				const int32 A = Base + r * (Seg + 1) + s;
				const int32 B = Base + (r + 1) * (Seg + 1) + s;
				Quad(S, A, B, B + 1, A + 1);
			}
	}

	void Helix(FMRMeshSection& S, const FVector& A, const FVector& B, float CoilR, float WireR, float Turns)
	{
		FVector D = B - A;
		const float Len = D.Size();
		if (Len < 0.01f) return;
		D /= Len;
		FVector U, W;
		Basis(D, U, W);
		const int32 Steps = FMath::Max(8, FMath::RoundToInt(Turns * 12.f));
		FVector Prev = A + U * CoilR;
		for (int32 i = 1; i <= Steps; ++i)
		{
			const float T = static_cast<float>(i) / Steps;
			const float Ang = 2.f * PI * Turns * T;
			const FVector P = A + D * (Len * T) + (U * FMath::Cos(Ang) + W * FMath::Sin(Ang)) * CoilR;
			TubeRaw(S, Prev, P, WireR, WireR, 6);
			Prev = P;
		}
	}
}

// =====================================================================================
// Vehicle builders
// =====================================================================================

namespace
{
	using namespace MRMeshKit;
	const FVector AxisY(0.f, 1.f, 0.f);

	float Rad(float Deg) { return FMath::DegreesToRadians(Deg); }

	struct FWheelSpec
	{
		float R = 36.f;          // outer radius (cm)
		float TyreW = 6.f;       // section width
		float TyreH = 5.5f;      // section height
		int32 Knobs = 0;         // 0 none, 1 MTB/BMX, 2 motocross
		int32 Spokes = 32;       // laced spokes (0 = cast/solid)
		int32 CastArms = 0;
		float SpokeR = 0.11f;
		float RimDepth = 2.2f;
		bool bRimBlack = true;
		float HubR = 2.2f;
		float HubHalf = 6.f;     // half the hub width
		float RotorR = 0.f;
		bool bRotorBothSides = false;
		int32 Cogs = 0;          // bicycle cassette (drive side)
		float CogMin = 2.2f, CogMax = 10.f;
		float SprocketR = 0.f;   // motorcycle rear sprocket
		float HubMotorR = 0.f;
	};

	void BuildWheel(FMRPartMesh& W, const FWheelSpec& S)
	{
		const float Sec = S.TyreH * 0.5f;
		// Tyre: torus whose section is TyreW wide and TyreH tall.
		Torus(W[EMRVisMat::Rubber], FVector::ZeroVector, AxisY, S.R - Sec, Sec, 64, 14, (S.TyreW * 0.5f) / Sec);
		if (S.Knobs > 0)
		{
			const bool bMx = S.Knobs == 2;
			const float KnobH = bMx ? 1.5f : 0.45f, KnobW = bMx ? 1.6f : 0.55f, Pitch = bMx ? 4.6f : 2.2f;
			const int32 Count = FMath::Clamp(FMath::RoundToInt(2.f * PI * S.R / Pitch), 24, 140);
			for (int32 k = 0; k < Count; ++k)
			{
				const float A = 2.f * PI * k / Count;
				const FVector Dir = Circ(A);
				const FVector Tan(-FMath::Sin(A), 0.f, FMath::Cos(A));
				const FQuat Q = FRotationMatrix::MakeFromXZ(Tan, Dir).ToQuat();
				const bool bCentre = (k % 2) == 0;
				for (int32 Side = -1; Side <= 1; ++Side)
				{
					if (bCentre != (Side == 0)) continue;
					const float Y = Side * S.TyreW * 0.34f;
					const float Rr = S.R - (Side == 0 ? 0.f : Sec * 0.25f);
					Box(W[EMRVisMat::Rubber], Dir * Rr + AxisY * Y, Q, FVector(KnobW * 0.5f, KnobW * 0.6f, KnobH * 0.5f));
				}
			}
		}
		// Rim and hub
		const float RimOut = S.R - S.TyreH + 0.6f;
		const float RimIn = FMath::Max(RimOut - S.RimDepth, S.HubR + 1.f);
		Disc(W[S.bRimBlack ? EMRVisMat::Black : EMRVisMat::Alloy], FVector::ZeroVector, AxisY, RimIn, RimOut, FMath::Max(S.TyreW * 0.72f, 1.6f), 56);
		Tube(W[EMRVisMat::Alloy], -AxisY * S.HubHalf, AxisY * S.HubHalf, S.HubR, S.HubR, 16, true);
		Tube(W[EMRVisMat::Black], -AxisY * (S.HubHalf + 1.4f), AxisY * (S.HubHalf + 1.4f), 0.7f, 0.7f, 8, true); // axle / thru-axle
		if (S.HubMotorR > 0.f) Disc(W[EMRVisMat::Black], FVector::ZeroVector, AxisY, 0.f, S.HubMotorR, S.HubHalf * 1.3f, 36);
		if (S.Spokes > 0)
		{
			const float FlangeR = S.HubMotorR > 0.f ? S.HubMotorR * 0.92f : S.HubR + 1.3f;
			const float FlangeY = S.HubMotorR > 0.f ? S.HubHalf * 0.55f : S.HubHalf * 0.8f;
			if (S.HubMotorR <= 0.f)
			{
				Disc(W[EMRVisMat::Alloy], AxisY * FlangeY, AxisY, S.HubR, FlangeR + 0.4f, 0.35f, 24);
				Disc(W[EMRVisMat::Alloy], -AxisY * FlangeY, AxisY, S.HubR, FlangeR + 0.4f, 0.35f, 24);
			}
			const float Cross = 2.f * PI * 3.f / (S.Spokes * 0.5f) * 0.5f; // 3-cross lacing offset at the hub
			for (int32 k = 0; k < S.Spokes; ++k)
			{
				const float Side = (k % 2) ? 1.f : -1.f;
				const float A = 2.f * PI * k / S.Spokes;
				const float HubA = A + (((k / 2) % 2) ? Cross : -Cross);
				const FVector HubP = Circ(HubA) * FlangeR + AxisY * (Side * FlangeY);
				const FVector RimP = Circ(A) * (RimIn + 0.2f) + AxisY * (Side * 0.35f);
				TubeRaw(W[EMRVisMat::Chrome], HubP, RimP, S.SpokeR, S.SpokeR, 4);
			}
		}
		if (S.CastArms > 0)
		{
			for (int32 k = 0; k < S.CastArms; ++k)
			{
				const float A = 2.f * PI * k / S.CastArms;
				for (const float Split : { -0.07f, 0.07f })
				{
					Bar(W[EMRVisMat::Black], Circ(A + Split * 0.4f) * (S.HubR + 0.5f), Circ(A + Split) * (RimIn + 0.3f),
						FMath::Max(S.TyreW * 0.28f, 1.2f), FMath::Max(S.R * 0.05f, 1.f), AxisY);
				}
			}
		}
		if (S.RotorR > 0.f)
		{
			for (int32 Side = -1; Side <= 1; Side += 2)
			{
				if (Side > 0 && !S.bRotorBothSides) continue;
				const FVector C = AxisY * (Side * (S.HubHalf + 0.9f));
				Disc(W[EMRVisMat::Chrome], C, AxisY, S.RotorR * 0.7f, S.RotorR, 0.22f, 44);
				for (int32 k = 0; k < 6; ++k)
				{
					const float A = 2.f * PI * k / 6.f;
					Bar(W[EMRVisMat::Black], C + Circ(A) * (S.HubR * 0.9f), C + Circ(A + 0.25f) * (S.RotorR * 0.72f), 0.25f, 1.1f, AxisY);
				}
			}
		}
		for (int32 c = 0; c < S.Cogs; ++c)
		{
			const float T = S.Cogs > 1 ? static_cast<float>(c) / (S.Cogs - 1) : 0.f;
			const float R = FMath::Lerp(S.CogMax, S.CogMin, T);
			Disc(W[(c % 4 == 0) ? EMRVisMat::Black : EMRVisMat::Chrome], AxisY * (S.HubHalf * 0.2f + c * 0.39f), AxisY, S.HubR, R, 0.2f, 40);
		}
		if (S.SprocketR > 0.f)
		{
			Disc(W[EMRVisMat::Alloy], AxisY * (S.HubHalf + 0.8f), AxisY, S.HubR + 2.f, S.SprocketR, 0.6f, 48);
		}
	}

	void Caliper(FMRMeshSection& S, const FVector& Axle, float RotorR, float Side, float AngleDeg, float Size)
	{
		const FVector P = Axle + Circ(Rad(AngleDeg)) * (RotorR * 0.86f) + AxisY * (Side * 1.6f);
		Box(S, P, FRotationMatrix::MakeFromXZ(FVector(-FMath::Sin(Rad(AngleDeg)), 0.f, FMath::Cos(Rad(AngleDeg))), Circ(Rad(AngleDeg))).ToQuat(),
			FVector(Size * 2.2f, Size * 1.1f, Size));
	}

	bool Has(const FString& Id, const TCHAR* Key) { return Id.Contains(Key, ESearchCase::IgnoreCase); }

	// ------------------------------------------------------------------ bicycles / e-bikes / BMX
	void BuildBicycle(FMRVehicleVisual& V, const MRSim::FVehicleParams& P, const FString& Id)
	{
		const float L = P.Wheelbase * 100.f, RF = P.WheelRadiusFront * 100.f, RR = P.WheelRadiusRear * 100.f;
		const float TF = P.Front.Travel * 100.f, TR = P.Rear.Travel * 100.f;
		const bool bSusF = TF > 4.f, bSusR = TR > 4.f;
		const bool bBmx = P.Category == EVehicleCategory::BMX;
		const bool bDh = P.Category == EVehicleCategory::DownhillBike;
		const bool bEbike = P.Category == EVehicleCategory::EBike;
		const bool bRoadCat = P.Category == EVehicleCategory::RoadBike;
		const bool bFlatRoad = Has(Id, TEXT("hybrid")) || Has(Id, TEXT("city"));
		const bool bDrop = (bRoadCat && !bFlatRoad) || (bEbike && (Has(Id, TEXT("road")) || Has(Id, TEXT("gravel"))));
		const bool bCommuter = bFlatRoad || Has(Id, TEXT("tourer")) || Has(Id, TEXT("trekking")) || Has(Id, TEXT("step")) || Has(Id, TEXT("cargo"))
			|| Has(Id, TEXT("longtail")) || Has(Id, TEXT("fold")) || Has(Id, TEXT("hub_e")) || Has(Id, TEXT("commut"));
		const bool bFat = Has(Id, TEXT("fat"));
		const bool bCargo = Has(Id, TEXT("cargo")) || Has(Id, TEXT("longtail"));
		const bool bStepThrough = Has(Id, TEXT("step"));
		const bool bSingleSpeed = bBmx || Has(Id, TEXT("fixie"));
		const bool bKnobbly = P.Tyre == ETyre::Knobbly;
		const bool bMtbLike = !bBmx && !bDrop && !bCommuter && (bKnobbly || bDh || P.Category == EVehicleCategory::MountainBike);
		const bool bHubMotor = bEbike && P.Powertrain.MotorPeakForceN <= 135.f;
		const bool bCoil = bDh || Has(Id, TEXT("park")) || Has(Id, TEXT("freeride"));
		const float K = FMath::Clamp(RR / (bBmx ? 25.5f : 36.f), 0.62f, 1.12f); // kids' bikes scale down

		// --- frame geometry (real-world numbers for the type)
		const float HA = bBmx ? 75.f : bDrop ? (Has(Id, TEXT("gravel")) || Has(Id, TEXT("cross")) ? 71.f : 72.5f) : bCommuter ? 70.f
			: bDh ? 63.5f : 69.5f - FMath::Max(TF - 10.f, 0.f) * 0.55f;
		const float STA = bBmx ? 71.f : bCommuter ? 72.f : 74.f;
		const float CS = bCargo ? L * 0.42f : (bBmx ? 34.f : bDrop ? 41.f : bDh ? 44.5f : 43.5f) * K;
		const float Drop = (bBmx ? -2.f : bDrop || bCommuter ? 7.f : bDh ? 1.f : 3.5f) * K;
		const FVector BB(FMath::Sqrt(FMath::Max(CS * CS - Drop * Drop, 1.f)), 0.f, RR - Drop);
		const FVector RearAxle(0.f, 0.f, RR), FrontAxle(L, 0.f, RF);
		const FVector D(-FMath::Cos(Rad(HA)), 0.f, FMath::Sin(Rad(HA)));   // steering axis, pointing up
		const FVector SDir(-FMath::Cos(Rad(STA)), 0.f, FMath::Sin(Rad(STA)));
		const float AC = bSusF ? RF + 4.f + TF + 3.f : (bBmx ? RF + 6.f : RF + 3.5f);
		const FVector Crown = FrontAxle + D * AC;
		const float HT = (bBmx ? 13.f : bDrop ? 15.f : bCommuter ? 16.f : 11.5f) * K;
		const FVector HeadLow = Crown + D * 1.f, HeadTop = HeadLow + D * HT;
		const float ST = (bBmx ? 22.f : bDrop ? 52.f : bDh ? 38.f : bCommuter ? 48.f : 43.f) * K;
		const FVector SeatCluster = BB + SDir * ST;
		const float SH = (bBmx ? 32.f : bDh ? 63.f : bDrop ? 73.f : 72.f) * K;
		const FVector SaddleP = BB + SDir * SH;
		const float RTube = bBmx ? 1.4f : bDh ? 2.5f : bDrop ? 1.8f : 2.1f;
		const float RDown = bEbike ? 3.3f : bBmx ? 1.6f : bDh ? 3.f : bDrop ? 2.1f : 2.5f;
		const float HubHalf = bDrop || bCommuter ? 5.f : bBmx ? 5.5f : 7.4f;

		FMRPartMesh& B = V.Body;
		// Main triangle
		Tube(B[EMRVisMat::Paint], HeadLow - D * 1.5f, HeadTop + D * 1.5f, 2.3f, 2.3f, 16, true);                  // head tube
		Disc(B[EMRVisMat::Black], HeadTop + D * 1.6f, D, 1.6f, 2.5f, 0.8f, 20);                                   // headset cup
		Tube(B[EMRVisMat::Paint], HeadLow + D * 1.5f, BB + FVector(1.f, 0, 1.f), RDown, RDown * 0.95f, 14, false); // down tube
		Tube(B[EMRVisMat::Paint], BB - SDir * 1.f, SeatCluster + SDir * 1.f, RTube, RTube, 14, true);             // seat tube
		if (bStepThrough)
		{
			PathTube(B[EMRVisMat::Paint], { HeadTop - D * 3.f, HeadLow + FVector(-14.f, 0, -8.f), BB + SDir * 14.f }, RTube, 14, false);
		}
		else
		{
			Tube(B[EMRVisMat::Paint], HeadTop - D * 1.5f, SeatCluster - SDir * 1.5f, RTube, RTube * 0.92f, 14, false); // top tube
		}
		Tube(B[EMRVisMat::Paint], BB - AxisY * 3.5f, BB + AxisY * 3.5f, 2.2f, 2.2f, 16, true);                   // BB shell
		if (bBmx) Bar(B[EMRVisMat::Paint], HeadLow + D * 3.f - FVector(0, 0, 0), BB + (HeadLow - BB) * 0.35f, 1.f, 1.2f); // gusset

		// Rear triangle or full-suspension swingarm
		const FVector Bp = RearAxle + (SeatCluster - RearAxle) * 0.62f; // shock yoke on single-pivot swingarms
		if (bSusR)
		{
			const FVector MP = BB + FVector(-3.f, 0, 8.f) * K;
			V.bHasSwingarm = true;
			V.SwingPivot = MP;
			FMRPartMesh& Sw = V.Swingarm;
			auto Loc = [&](const FVector& X) { return X - MP; };
			for (const float Side : { -1.f, 1.f })
			{
				Bar(Sw[EMRVisMat::Paint], Loc(MP + AxisY * (Side * 4.5f)), Loc(RearAxle + AxisY * (Side * (HubHalf + 1.f))), 2.2f, 3.4f, FVector(0, 0, 1));
				Bar(Sw[EMRVisMat::Paint], Loc(RearAxle + AxisY * (Side * (HubHalf + 1.f))), Loc(Bp + AxisY * (Side * 3.f)), 1.8f, 2.6f, FVector(1, 0, 0));
				Bar(Sw[EMRVisMat::Paint], Loc(MP + AxisY * (Side * 4.f) + FVector(0, 0, 2.f)), Loc(Bp + AxisY * (Side * 3.f)), 1.8f, 2.8f, FVector(1, 0, 0));
				Box(Sw[EMRVisMat::Alloy], Loc(RearAxle + AxisY * (Side * (HubHalf + 1.2f))), FQuat::Identity, FVector(2.2f, 0.5f, 2.2f)); // dropout
			}
			Tube(Sw[EMRVisMat::Black], Loc(MP - AxisY * 5.f), Loc(MP + AxisY * 5.f), 1.4f, 1.4f, 12, true); // pivot axle
			// Shock: frame mount on the down tube to the swingarm yoke.
			const FVector ShockFront = BB + (HeadLow - BB) * 0.36f + FVector(0, 0, 2.f);
			const FVector ShockRear = Bp + FVector(1.5f, 0, -1.f);
			const FVector Mid = FMath::Lerp(ShockFront, ShockRear, 0.55f);
			Tube(B[EMRVisMat::Black], ShockFront, Mid, 2.2f, 2.2f, 14, true);                                 // body / reservoir
			Tube(B[EMRVisMat::Chrome], Mid, ShockRear, 1.1f, 1.1f, 10, true);                                // shaft
			if (bCoil) Helix(B[EMRVisMat::Gold], FMath::Lerp(ShockFront, ShockRear, 0.25f), FMath::Lerp(ShockFront, ShockRear, 0.9f), 2.6f, 0.45f, 6.f);
			else Tube(B[EMRVisMat::Black], FMath::Lerp(ShockFront, ShockRear, 0.3f), FMath::Lerp(ShockFront, ShockRear, 0.62f), 2.7f, 2.7f, 14, true); // air can
		}
		else
		{
			for (const float Side : { -1.f, 1.f })
			{
				Tube(B[EMRVisMat::Paint], BB + AxisY * (Side * 4.f), RearAxle + AxisY * (Side * (HubHalf + 0.8f)), 1.25f * (bBmx ? 1.1f : 1.f), 0.9f, 10, false); // chainstay
				Tube(B[EMRVisMat::Paint], RearAxle + AxisY * (Side * (HubHalf + 0.8f)), SeatCluster - SDir * 2.f + AxisY * (Side * 1.8f), 0.85f, 1.0f, 10, false); // seatstay
				Box(B[EMRVisMat::Alloy], RearAxle + AxisY * (Side * (HubHalf + 1.1f)), FQuat::Identity, FVector(2.f, 0.5f, 2.f));
			}
		}

		// Seatpost and saddle
		Tube(B[EMRVisMat::Black], SeatCluster - SDir * 6.f, SaddleP - SDir * 3.f, 1.55f, 1.55f, 12, false);
		Disc(B[EMRVisMat::Black], SeatCluster + SDir * 0.6f, SDir, 1.5f, 2.3f, 1.2f, 16);
		{
			const FVector Sc = SaddleP + FVector(-1.5f, 0, 2.f);
			const FQuat Q = FQuat(FVector::RightVector, Rad(bBmx ? -12.f : 0.f));
			const float Wd = bCommuter ? 1.3f : 1.f;
			Ellipsoid(B[EMRVisMat::Seat], Sc + Q.RotateVector(FVector(-5.f, 0, 0)), FVector(8.f, 7.f * Wd, 2.6f), Q, 18, 8);
			Ellipsoid(B[EMRVisMat::Seat], Sc + Q.RotateVector(FVector(7.f, 0, -0.3f)), FVector(9.f, 3.2f * Wd, 2.f), Q, 16, 8);
			for (const float Side : { -1.f, 1.f })
				Tube(B[EMRVisMat::Chrome], Sc + Q.RotateVector(FVector(-9.f, Side * 2.5f, -2.2f)), Sc + Q.RotateVector(FVector(9.f, Side * 1.2f, -2.f)), 0.3f, 0.3f, 6, false);
		}

		// Drivetrain (drive side is +Y)
		const int32 RingT = bBmx ? 25 : bDrop ? 50 : bDh ? 36 : bEbike ? 36 : 32;
		const float RingR = RingT * 0.2021f, RingY = bDrop ? 4.4f : 5.3f;
		Disc(B[EMRVisMat::Black], BB + AxisY * RingY, AxisY, RingR - 1.7f, RingR, 0.45f, 56);
		if (bDrop && !bSingleSpeed) Disc(B[EMRVisMat::Black], BB + AxisY * (RingY - 0.6f), AxisY, 34 * 0.2021f - 1.5f, 34 * 0.2021f, 0.4f, 48);
		for (int32 k = 0; k < 4; ++k) Bar(B[EMRVisMat::Black], BB + AxisY * RingY, BB + AxisY * RingY + Circ(PI * 0.5f * k + 0.4f) * (RingR - 1.5f), 1.2f, 0.5f, AxisY);
		const float CrankL = (bBmx ? 17.5f : 17.f) * K, CrankA = Rad(-25.f);
		Tube(B[EMRVisMat::Alloy], BB - AxisY * 7.2f, BB + AxisY * 7.2f, 1.2f, 1.2f, 10, true); // spindle
		(void)CrankA;
		V.bHasCranks = true;
		V.BB = BB;
		V.CrankLength = CrankL;
		V.PedalY = 7.2f + (bDrop ? 4.f : 6.f);
		V.GearRatio = bBmx ? 2.4f : bDrop ? 3.1f : bCommuter ? 2.6f : 2.2f;
		for (const float Side : { -1.f, 1.f })
		{
			// Crank arm in the crank's own frame (origin BB, angle 0); the actor turns it.
			const FVector Dir = Circ(Side > 0 ? 0.f : PI);
			const FVector End = Dir * CrankL + AxisY * (Side * 7.2f);
			Bar(V.Crank[EMRVisMat::Black], AxisY * (Side * 7.2f), End, 1.4f, 2.6f, AxisY);
			Tube(V.Crank[EMRVisMat::Alloy], End, End + AxisY * (Side * 1.6f), 0.6f, 0.6f, 8, true); // pedal spindle
			FMRPartMesh& Pd = V.Pedal[Side > 0 ? 1 : 0];
			const FVector Pc = AxisY * (Side * (V.PedalY - 7.2f)); // pedal centre relative to the crank end
			if (bDrop)
			{
				Box(Pd[EMRVisMat::Black], Pc, FQuat::Identity, FVector(4.5f, 3.5f, 0.9f)); // clipless pedal
				Box(Pd[EMRVisMat::Chrome], Pc + FVector(0, 0, 0.9f), FQuat::Identity, FVector(2.6f, 2.6f, 0.15f));
			}
			else
			{
				Box(Pd[EMRVisMat::Black], Pc, FQuat::Identity, FVector(5.f, 5.f, 0.8f)); // flat pedal
				for (int32 Pin = 0; Pin < 4; ++Pin)
					Tube(Pd[EMRVisMat::Chrome], Pc + FVector((Pin < 2 ? -4.f : 4.f), (Pin % 2 ? -4.f : 4.f), 0.7f),
						Pc + FVector((Pin < 2 ? -4.f : 4.f), (Pin % 2 ? -4.f : 4.f), 1.4f), 0.2f, 0.2f, 4, false);
			}
		}
		const float CogR = bSingleSpeed ? 1.9f : bDrop ? 3.4f : 4.4f;
		{
			const FVector RingTop = BB + FVector(0, RingY, RingR), RingBot = BB + FVector(0, RingY, -RingR);
			const FVector CogTop = RearAxle + FVector(0, RingY, CogR), CogBot = RearAxle + FVector(0, RingY, -CogR);
			PathTube(B[EMRVisMat::Chrome], { RingTop, CogTop }, 0.32f, 5, false);
			if (bSingleSpeed)
			{
				PathTube(B[EMRVisMat::Chrome], { RingBot, CogBot }, 0.32f, 5, false);
			}
			else
			{
				const FVector P1 = RearAxle + FVector(0.8f, RingY + 0.8f, -CogR - 2.6f);
				const FVector P2 = RearAxle + FVector(-1.f, RingY + 0.8f, -CogR - 10.f);
				PathTube(B[EMRVisMat::Chrome], { RingBot, P2, P1, CogBot }, 0.32f, 5, false);
				Bar(B[EMRVisMat::Black], P1 + AxisY * 0.8f, P2 + AxisY * 0.8f, 0.6f, 2.4f, AxisY);       // derailleur cage
				Disc(B[EMRVisMat::Black], P1, AxisY, 0.f, 1.6f, 0.5f, 16);
				Disc(B[EMRVisMat::Black], P2, AxisY, 0.f, 1.6f, 0.5f, 16);
				Box(B[EMRVisMat::Black], RearAxle + FVector(1.f, RingY + 2.2f, -1.5f), FQuat::Identity, FVector(2.2f, 1.2f, 2.f));
			}
		}

		// E-bike motor and battery
		if (bEbike)
		{
			const FVector DtDir = (BB - HeadLow).GetSafeNormal();
			const FVector Under = FVector::CrossProduct(DtDir, AxisY).GetSafeNormal() * -1.f;
			Bar(B[EMRVisMat::Black], FMath::Lerp(HeadLow, BB, 0.18f) + Under * 1.2f, FMath::Lerp(HeadLow, BB, 0.8f) + Under * 1.2f, 7.4f, 7.4f, Under);
			if (!bHubMotor)
			{
				Ellipsoid(B[EMRVisMat::Black], BB + FVector(-1.f, 0, 1.5f), FVector(11.f, 7.f, 10.f), FQuat::Identity, 20, 10);
				Disc(B[EMRVisMat::Alloy], BB + AxisY * 3.6f, AxisY, 0.f, 6.5f, 0.6f, 24);
			}
			Box(B[EMRVisMat::Black], HeadTop + D * 4.f + FVector(4.f, 0, 1.f), FQuat::Identity, FVector(3.f, 2.5f, 1.f)); // display
		}

		// Brakes
		const float RotorR = bBmx || Has(Id, TEXT("fixie")) ? 0.f : (bDh || Has(Id, TEXT("enduro")) || bEbike ? 10.15f : bDrop ? 8.f : 9.f) * FMath::Max(K, 0.8f);
		if (RotorR > 0.f) Caliper(B[EMRVisMat::Black], RearAxle, RotorR, -(HubHalf + 0.9f), 145.f, 1.4f);

		// Commuter kit: mudguards, rack, lights
		if (bCommuter || bCargo)
		{
			ArcStrip(B[EMRVisMat::Black], RearAxle, RR + 2.6f, 5.f, Rad(10.f), Rad(165.f), 22);
			const float RackZ = RR * 2.f + 5.f;
			const float RackX1 = bCargo ? -L * 0.12f : -16.f, RackX0 = bCargo ? L * 0.3f : 6.f;
			for (const float Side : { -1.f, 1.f })
			{
				Tube(B[EMRVisMat::Black], FVector(RackX1, Side * 7.f, RackZ), FVector(RackX0, Side * 7.f, RackZ), 0.55f, 0.55f, 8, true);
				Tube(B[EMRVisMat::Black], FVector(RackX1 + 3.f, Side * 7.f, RackZ), RearAxle + AxisY * (Side * (HubHalf + 1.2f)), 0.5f, 0.5f, 8, false);
				Tube(B[EMRVisMat::Black], FVector(RackX0, Side * 7.f, RackZ), SeatCluster - SDir * 3.f + AxisY * (Side * 2.f), 0.45f, 0.45f, 8, false);
			}
			if (bCargo) Box(B[EMRVisMat::Black], FVector((RackX0 + RackX1) * 0.5f, 0, RackZ + 0.8f), FQuat::Identity, FVector((RackX0 - RackX1) * 0.5f, 9.f, 0.8f));
			Box(B[EMRVisMat::RedLens], FVector(RackX1 - 1.f, 0, RackZ - 1.f), FQuat::Identity, FVector(1.f, 3.f, 1.2f)); // rear light
		}
		if (bBmx)
		{
			for (const float Side : { -1.f, 1.f })
				Tube(B[EMRVisMat::Chrome], RearAxle + AxisY * (Side * (HubHalf + 1.6f)), RearAxle + AxisY * (Side * (HubHalf + 12.f)), 1.9f, 1.9f, 12, true); // pegs
		}

		// --- Steering: fork, stem, bars (SteerPivot space: front contact at origin)
		const FVector SteerOff(L, 0.f, 0.f);
		auto Sv = [&](const FVector& X) { return X - SteerOff; };
		FMRPartMesh& St = V.Steer;
		FMRPartMesh& Lw = V.FrontLowers;
		auto Lv = [&](const FVector& X) { return X - FrontAxle; };
		const float LegY = bBmx ? 5.f : bDrop || bCommuter ? 5.2f : 6.2f;
		const FVector SteererTop = HeadTop + D * (2.f + 2.f);
		Tube(St[EMRVisMat::Black], Sv(Crown), Sv(SteererTop), 1.6f, 1.6f, 12, true); // steerer + spacers
		if (bSusF)
		{
			const float StR = bDh ? 1.9f : 1.75f;
			const float StanchionLen = TF + 12.f;
			Bar(St[EMRVisMat::Black], Sv(Crown - AxisY * (LegY + 2.f)), Sv(Crown + AxisY * (LegY + 2.f)), 4.f, 3.f, D);
			const FVector TopOfStanchion = bDh ? HeadTop + D * 2.f : Crown + D * 1.5f;
			if (bDh) Bar(St[EMRVisMat::Black], Sv(TopOfStanchion - AxisY * (LegY + 2.f)), Sv(TopOfStanchion + AxisY * (LegY + 2.f)), 4.f, 2.5f, D);
			for (const float Side : { -1.f, 1.f })
			{
				Tube(St[bDh || bMtbLike ? EMRVisMat::Gold : EMRVisMat::Chrome], Sv(TopOfStanchion + AxisY * (Side * LegY)),
					Sv(Crown - D * StanchionLen + AxisY * (Side * LegY)), StR, StR, 14, true);
				// Lowers ride on the axle
				Tube(Lw[EMRVisMat::Black], Lv(FrontAxle - D * 2.f + AxisY * (Side * LegY)), Lv(FrontAxle + D * (AC - TF - 6.f) + AxisY * (Side * LegY)), 2.4f, 2.3f, 14, true);
				Box(Lw[EMRVisMat::Black], Lv(FrontAxle + AxisY * (Side * LegY)), FQuat::Identity, FVector(2.2f, 0.8f, 2.2f));
			}
			const FVector ArchC = FrontAxle + D * (AC - TF - 9.f) + FVector(3.f, 0, 0);
			PathTube(Lw[EMRVisMat::Black], { Lv(ArchC - AxisY * LegY), Lv(ArchC + FVector(2.5f, 0, 2.f)), Lv(ArchC + AxisY * LegY) }, 1.2f, 8, false);
		}
		else
		{
			Bar(St[EMRVisMat::Paint], Sv(Crown - AxisY * (LegY + 1.f)), Sv(Crown + AxisY * (LegY + 1.f)), 3.f, 2.6f, D);
			for (const float Side : { -1.f, 1.f })
				Tube(St[bBmx ? EMRVisMat::Chrome : EMRVisMat::Paint], Sv(FrontAxle + AxisY * (Side * LegY)), Sv(Crown + AxisY * (Side * LegY)), 1.1f, 1.6f, 12, false);
		}
		if (RotorR > 0.f) Caliper(Lw[EMRVisMat::Black], FVector::ZeroVector, RotorR, -(HubHalf + 0.9f), 110.f, 1.4f);
		if (bCommuter || bCargo) ArcStrip(Lw[EMRVisMat::Black], FVector::ZeroVector, RF + 2.6f, 5.f, Rad(20.f), Rad(150.f), 18);

		// Stem and bars
		const float StemL = (bDrop ? 10.f : bCommuter ? 8.f : bBmx ? 5.f : 4.5f) * FMath::Max(K, 0.85f);
		const FVector StemDir = FVector(FMath::Cos(Rad(bCommuter ? 20.f : -6.f)), 0, FMath::Sin(Rad(bCommuter ? 20.f : -6.f)));
		const FVector StemBase = HeadTop + D * 3.f;
		const FVector StemEnd = StemBase + StemDir * StemL;
		Bar(St[EMRVisMat::Black], Sv(StemBase - StemDir * 2.f), Sv(StemEnd + StemDir * 1.f), 3.8f, 3.6f);
		const FVector BarC = StemEnd + FVector(0, 0, bMtbLike ? 1.f : 0.f);
		float BarW = (bBmx ? 72.f : bDrop ? 42.f : bCommuter ? 62.f : bDh ? 80.f : 78.f) * FMath::Max(K, 0.82f);
		if (bBmx)
		{
			for (const float Side : { -1.f, 1.f })
			{
				PathTube(St[EMRVisMat::Chrome], { Sv(BarC + FVector(0, Side * 11.f, 0)), Sv(BarC + FVector(-4.f, Side * 13.f, 21.f * K)), Sv(BarC + FVector(-6.f, Side * BarW * 0.5f, 22.f * K)) }, 1.1f, 10, true);
				Tube(St[EMRVisMat::Rubber], Sv(BarC + FVector(-5.4f, Side * (BarW * 0.5f - 13.f), 21.8f * K)), Sv(BarC + FVector(-6.f, Side * BarW * 0.5f, 22.f * K)), 1.6f, 1.6f, 10, true);
			}
			Tube(St[EMRVisMat::Chrome], Sv(BarC + FVector(0, -11.f, 0)), Sv(BarC + FVector(0, 11.f, 0)), 1.1f, 1.1f, 10, false);
			Tube(St[EMRVisMat::Chrome], Sv(BarC + FVector(-2.5f, -12.5f, 13.f * K)), Sv(BarC + FVector(-2.5f, 12.5f, 13.f * K)), 0.9f, 0.9f, 10, false); // crossbar
			V.Bars = BarC + FVector(-6.f, 0, 22.f * K);
			V.BarHalfWidth = BarW * 0.5f - 7.f;
		}
		else if (bDrop)
		{
			for (const float Side : { -1.f, 1.f })
			{
				const float Y = Side * BarW * 0.5f;
				PathTube(St[EMRVisMat::Rubber], { Sv(BarC + FVector(0, Side * 3.f, 0)), Sv(BarC + FVector(0, Y * 0.95f, 0)), Sv(BarC + FVector(7.f, Y, -0.5f)),
					Sv(BarC + FVector(9.5f, Y, -6.f)), Sv(BarC + FVector(7.f, Y, -12.f)), Sv(BarC + FVector(0.f, Y, -13.5f)), Sv(BarC + FVector(-4.f, Y, -12.8f)) }, 1.25f, 10, true);
				Box(St[EMRVisMat::Black], Sv(BarC + FVector(9.f, Y, 1.5f)), FQuat(FVector::RightVector, Rad(-15.f)), FVector(3.5f, 1.4f, 2.f)); // hood
				Bar(St[EMRVisMat::Black], Sv(BarC + FVector(9.5f, Y, 0.f)), Sv(BarC + FVector(10.5f, Y, -10.f)), 1.f, 1.6f, FVector(1, 0, 0));    // lever blade
			}
			V.Bars = BarC + FVector(9.f, 0, 1.f);
			V.BarHalfWidth = BarW * 0.5f;
		}
		else
		{
			const float H = BarW * 0.5f;
			TArray<FVector> Pts = { Sv(BarC + FVector(-2.5f, -H, 1.8f)), Sv(BarC + FVector(-1.2f, -H * 0.35f, 1.f)), Sv(BarC + FVector(0, -H * 0.12f, 0)),
				Sv(BarC + FVector(0, H * 0.12f, 0)), Sv(BarC + FVector(-1.2f, H * 0.35f, 1.f)), Sv(BarC + FVector(-2.5f, H, 1.8f)) };
			PathTube(St[EMRVisMat::Black], Pts, 1.1f, 10, true);
			for (const float Side : { -1.f, 1.f })
			{
				const FVector End = BarC + FVector(-2.5f, Side * H, 1.8f);
				const FVector In = BarC + FVector(-2.1f, Side * (H - 13.f), 1.6f);
				Tube(St[EMRVisMat::Rubber], Sv(In), Sv(End + FVector(0, Side * 0.5f, 0)), 1.65f, 1.65f, 12, true);
				Box(St[EMRVisMat::Black], Sv(In - FVector(0, Side * 1.5f, 0)), FQuat::Identity, FVector(1.6f, 1.2f, 1.8f));     // brake master
				Bar(St[EMRVisMat::Alloy], Sv(In - FVector(-1.f, Side * 1.5f, 0)), Sv(In + FVector(3.f, Side * 7.f, -1.5f)), 0.8f, 1.2f); // lever
			}
			if (!bSingleSpeed) Box(St[EMRVisMat::Black], Sv(BarC + FVector(-1.f, H - 16.f, -2.5f)), FQuat::Identity, FVector(2.f, 1.4f, 1.4f)); // shifter
			V.Bars = BarC + FVector(-2.f, 0, 1.5f);
			V.BarHalfWidth = H - 7.f;
		}
		// --- Real-life details
		{
			const FVector DtA = HeadLow + D * 2.f, DtB = BB + FVector(4.f, 0, 0);
			const FVector DtUnder = FVector::CrossProduct((DtB - DtA).GetSafeNormal(), AxisY).GetSafeNormal() * -(RDown + 0.4f);
			const float LeverY = bDrop ? BarW * 0.5f : BarW * 0.5f - 14.f;
			// Brake hoses: front from the left lever down the fork leg; rear along the down tube to the rear caliper.
			if (RotorR > 0.f)
			{
				PathTube(St[EMRVisMat::Black], { Sv(BarC + FVector(bDrop ? 8.f : -1.f, -LeverY, bDrop ? 0.f : 1.f)), Sv(StemBase + FVector(7.f, -5.f, 3.f)),
					Sv(Crown + FVector(4.f, -(LegY + 2.6f), 1.f)), Sv(Crown - D * (AC * 0.45f) + FVector(3.f, -(LegY + 2.7f), 0.f)) }, 0.26f, 5, false);
				PathTube(B[EMRVisMat::Black], { HeadTop + FVector(5.f, 3.f, 1.f), DtA + DtUnder + FVector(0, 1.2f, 0), DtB + DtUnder + FVector(0, 1.2f, 0),
					RearAxle + FVector(16.f, -(HubHalf + 1.6f), 2.f), RearAxle + FVector(5.f, -(HubHalf + 1.8f), RotorR * 0.6f) }, 0.26f, 5, false);
			}
			// Gear cable to the rear derailleur.
			if (!bSingleSpeed)
				PathTube(B[EMRVisMat::Black], { HeadTop + FVector(5.f, -3.f, 1.f), DtA + DtUnder - FVector(0, 1.2f, 0), DtB + DtUnder - FVector(0, 1.2f, 0),
					RearAxle + FVector(12.f, RingY + 1.5f, 1.f), RearAxle + FVector(2.f, RingY + 2.2f, -1.5f) }, 0.22f, 5, false);
			// Red rear reflector under the saddle (fitted to every new bike sold in the UK).
			Box(B[EMRVisMat::RedLens], SaddleP - SDir * 9.f + FVector(-2.5f, 0, 0), FQuat(FVector::RightVector, Rad(-15.f)), FVector(0.6f, 2.5f, 1.6f));
			// Bottle and cage on the down tube (road, gravel, XC and trail bikes).
			if (!bBmx && !bCommuter && !bEbike && !bDh && !bCargo)
			{
				const FVector Over = DtUnder.GetSafeNormal() * (RDown + 3.8f); // DtUnder points to the top side of the down tube
				const FVector C0 = FMath::Lerp(DtB, DtA, 0.3f) + Over, C1 = FMath::Lerp(DtB, DtA, 0.58f) + Over;
				Tube(B[EMRVisMat::Black], C0 - Over * 0.35f, C1 - Over * 0.35f, 0.25f, 0.25f, 4, false); // cage rail
				Tube(B[EMRVisMat::Alloy], C0, C1, 3.4f, 3.2f, 14, true);                              // bottle
				Tube(B[EMRVisMat::Black], C1, C1 + (C1 - C0).GetSafeNormal() * 2.5f, 1.4f, 1.0f, 10, true); // cap
			}
			// Kickstand folded along the left chainstay (commuters, e-bikes, cargo).
			if (bCommuter || bEbike || bCargo)
				Bar(B[EMRVisMat::Black], BB + FVector(-5.f, -6.f, -3.f), RearAxle + FVector(14.f, -7.5f, 0.f), 1.6f, 2.2f, FVector(0, 1, 0));
			// Front white reflector on the bars for commuters.
			if (bCommuter) Box(St[EMRVisMat::Alloy], Sv(BarC + FVector(3.f, 0, -3.f)), FQuat::Identity, FVector(0.6f, 2.5f, 1.6f));
		}
		if (bCommuter || bEbike)
		{
			V.FrontLight = Crown + FVector(4.f, 0, 2.f);
			V.bHasFrontLight = true;
			Ellipsoid(St[EMRVisMat::Black], Sv(V.FrontLight), FVector(3.f, 3.2f, 2.6f), FQuat::Identity, 14, 8);
			Disc(St[EMRVisMat::Lens], Sv(V.FrontLight + FVector(2.6f, 0, 0)), FVector(1, 0, 0), 0.f, 2.2f, 0.5f, 16);
		}

		// --- Wheels
		FWheelSpec W;
		W.TyreW = bFat ? 11.5f : bDrop ? (Has(Id, TEXT("gravel")) || Has(Id, TEXT("tour")) || Has(Id, TEXT("cross")) ? 3.8f : 2.8f) : bCommuter ? 4.2f
			: bBmx ? 5.6f : bDh ? 6.4f : 6.1f;
		W.TyreH = W.TyreW * (bFat ? 0.9f : 0.92f);
		W.Knobs = bKnobbly ? 1 : 0;
		W.Spokes = bBmx ? 36 : bDrop ? 24 : 32;
		W.SpokeR = 0.11f;
		W.RimDepth = Has(Id, TEXT("aero")) ? 5.5f : bFat ? 3.f : 2.2f;
		W.bRimBlack = !bBmx;
		W.HubHalf = HubHalf;
		W.RotorR = RotorR;
		FWheelSpec Wf = W, Wr = W;
		Wf.R = RF; Wr.R = RR;
		Wr.Cogs = bSingleSpeed ? 1 : (bDrop ? 11 : 12);
		Wr.CogMin = bSingleSpeed ? CogR : 2.1f;
		Wr.CogMax = bSingleSpeed ? CogR : (bDrop ? 6.2f : 10.2f);
		if (bHubMotor) { Wr.HubMotorR = 7.5f; Wr.Spokes = 32; }
		BuildWheel(V.FrontWheel, Wf);
		BuildWheel(V.RearWheel, Wr);

		// --- Rider contact points
		V.Foot = BB + FVector(2.f, 0, -6.f);
		V.Seat = bBmx ? BB + FVector(-6.f, 0, 62.f * K) : SaddleP + FVector(-1.f, 0, 8.f);
		V.Head = FVector(L * (bBmx ? 0.45f : 0.5f), 0, V.Seat.Z + (bDrop ? 52.f : bCommuter ? 72.f : 66.f) * FMath::Max(K, 0.8f));
	}

	// ------------------------------------------------------------------ motorcycles / dirt bikes
	void BuildMotorcycle(FMRVehicleVisual& V, const MRSim::FVehicleParams& P, const FString& Id)
	{
		const float L = P.Wheelbase * 100.f, RF = P.WheelRadiusFront * 100.f, RR = P.WheelRadiusRear * 100.f;
		const float TF = P.Front.Travel * 100.f;
		const bool bDirt = P.Category == EVehicleCategory::DirtBike;
		const bool bElectric = P.Powertrain.Type != MRSim::EPowertrain::Combustion;
		const bool bCruiser = Has(Id, TEXT("cruiser"));
		const bool bRetro = Has(Id, TEXT("retro"));
		const bool bTrials = Has(Id, TEXT("trials"));
		const bool bSupermoto = Has(Id, TEXT("supermoto"));
		const bool bAdventure = Has(Id, TEXT("adventure"));
		const bool bFaired = !bDirt && (Has(Id, TEXT("sport")) || Has(Id, TEXT("superbike")) || Has(Id, TEXT("tourer")));
		const bool bTwoStroke = Has(Id, TEXT("2t"));
		const bool bLightE = Has(Id, TEXT("lemx")); // Sur-Ron / Talaria style: beam frame, battery between the knees, no tank
		const bool bVarg = Has(Id, TEXT("varg"));   // full-size electric MX: radiators and shrouds like a petrol bike
		const bool bAirCooled = bCruiser || bRetro || Has(Id, TEXT("trail")) || Has(Id, TEXT("pit")) || Has(Id, TEXT("mini")) || Has(Id, TEXT("125_naked"));
		const bool bOffroadStyle = bDirt || bSupermoto || bAdventure;
		const bool bUsd = !bCruiser && !bRetro && !Has(Id, TEXT("125"));
		const float Ks = FMath::Clamp(RR / 33.f, 0.62f, 1.12f);
		int32 Cyl = 1;
		if (!bDirt && !bElectric)
		{
			const float Tq = P.Powertrain.PeakTorqueNm;
			Cyl = bCruiser ? 2 : Has(Id, TEXT("single")) || Has(Id, TEXT("supermoto")) || Tq < 20.f ? 1 : Has(Id, TEXT("triple")) ? 3 : (Has(Id, TEXT("superbike")) || Has(Id, TEXT("1300")) || Has(Id, TEXT("900"))) ? 4 : 2;
		}

		const float Rake = bCruiser ? 32.f : bDirt || bAdventure || bSupermoto ? 27.f : bTrials ? 23.f : 24.5f;
		const FVector D(-FMath::Sin(Rad(Rake)), 0.f, FMath::Cos(Rad(Rake)));
		const FVector FrontAxle(L, 0.f, RF), RearAxle(0.f, 0.f, RR);
		const float ClampZ = (bDirt ? 98.f : bTrials ? 88.f : bAdventure || bSupermoto ? 92.f : bCruiser ? 72.f : bFaired ? 74.f : 78.f) * Ks;
		const float AC = (ClampZ - RF) / FMath::Cos(Rad(Rake));
		const FVector LowerClamp = FrontAxle + D * AC, UpperClamp = LowerClamp + D * (bDirt ? 9.f : 7.f);
		const float SeatZ = (bLightE ? 84.f : bDirt ? 95.f : bTrials ? 78.f : bSupermoto ? 90.f : bAdventure ? 86.f : bCruiser ? 68.f : bFaired ? 82.f : 80.f) * Ks;
		const FVector SP(L * (bDirt ? 0.4f : 0.41f), 0.f, RR + (bDirt ? 12.f : 6.f) * Ks);
		const FVector EC(L * 0.55f, 0.f, RR + (bDirt ? 17.f : 9.f) * Ks);
		const float ForkY = (bDirt ? 11.f : bCruiser ? 11.5f : 9.5f) * FMath::Max(Ks, 0.8f);

		FMRPartMesh& B = V.Body;
		// Steering head
		Tube(B[EMRVisMat::Black], LowerClamp - D * 1.f, UpperClamp + D * 1.f, 3.2f, 3.2f, 14, true);

		// Frame
		const FVector HeadMid = FMath::Lerp(LowerClamp, UpperClamp, 0.5f);
		const FVector TankC = bDirt ? FVector(L * 0.63f, 0, SeatZ - 6.f * Ks) : FVector(HeadMid.X - 26.f * Ks, 0, UpperClamp.Z - 4.f * Ks);
		if (bLightE)
		{
			// Forged alloy beam frame: twin flat plates from the head to the swingarm pivot, a down beam to the motor.
			for (const float Side : { -1.f, 1.f })
			{
				Bar(B[EMRVisMat::Alloy], HeadMid + FVector(0, Side * 3.5f, 0), SP + FVector(6.f, Side * 7.f, 16.f * Ks), 2.f, 9.f * Ks, FVector(0, 0, 1));
				PathTube(B[EMRVisMat::Alloy], { SP + FVector(4.f, Side * 7.f, 14.f * Ks), FVector(L * 0.12f, Side * 6.f, SeatZ - 7.f * Ks) }, 1.4f * Ks, 8, true); // subframe
			}
			Bar(B[EMRVisMat::Alloy], LowerClamp - D * 2.f, SP + FVector(18.f, 0, -6.f * Ks), 6.f * Ks, 3.f * Ks, FVector(0, 1, 0));
			// Battery pack in the frame triangle, with painted side covers
			const FVector Bat(L * 0.5f, 0, SeatZ - 26.f * Ks);
			Box(B[EMRVisMat::Black], Bat, FQuat(FVector::RightVector, Rad(-12.f)), FVector(15.f, 8.f, 17.f) * Ks);
			for (const float Side : { -1.f, 1.f })
				Box(B[EMRVisMat::Paint], Bat + FVector(0, Side * 8.6f * Ks, 1.f), FQuat(FVector::RightVector, Rad(-12.f)), FVector(16.f, 0.6f, 15.f) * Ks);
			Ellipsoid(B[EMRVisMat::Paint], FVector(L * 0.66f, 0, SeatZ - 9.f * Ks), FVector(13.f, 9.f, 6.f) * Ks, FQuat(FVector::RightVector, Rad(14.f)), 16, 8); // front cover / tank shroud
		}
		else if (bDirt || bSupermoto || bTrials)
		{
			for (const float Side : { -1.f, 1.f })
			{
				PathTube(B[EMRVisMat::Alloy], { HeadMid + AxisY * (Side * 3.f), FVector(L * 0.56f, Side * 9.f, SeatZ - 14.f * Ks), SP + FVector(5.f, Side * 9.f, 9.f * Ks), SP + FVector(3.f, Side * 9.f, -8.f * Ks) }, 2.1f * Ks, 10, true);
				PathTube(B[EMRVisMat::Alloy], { SP + FVector(4.f, Side * 9.f, 9.f * Ks), FVector(L * 0.14f, Side * 8.f, SeatZ - 8.f * Ks) }, 1.3f * Ks, 8, true); // subframe
			}
			PathTube(B[EMRVisMat::Alloy], { LowerClamp - D * 2.f, EC + FVector(17.f, 0, -4.f) * Ks, EC + FVector(4.f, 0, -18.f) * Ks, SP + FVector(0.f, 0, -10.f * Ks) }, 2.f * Ks, 10, true);
			Bar(B[EMRVisMat::Black], EC + FVector(14.f, 0, -21.f) * Ks, SP + FVector(-2.f, 0, -14.f * Ks), 18.f * Ks, 1.f); // skid plate
		}
		else if (bCruiser)
		{
			PathTube(B[EMRVisMat::Black], { HeadMid, FVector(L * 0.45f, 0, SeatZ + 2.f), FVector(L * 0.12f, 0, SeatZ - 6.f) }, 2.4f, 10, true);
			for (const float Side : { -1.f, 1.f })
				PathTube(B[EMRVisMat::Black], { LowerClamp - D * 1.5f + AxisY * (Side * 3.f), EC + FVector(20.f, Side * 9.f, -14.f), SP + FVector(0, Side * 10.f, -10.f), RearAxle + FVector(14.f, Side * 11.f, 12.f) }, 1.9f, 10, true);
		}
		else
		{
			for (const float Side : { -1.f, 1.f })
				Bar(B[EMRVisMat::Alloy], HeadMid + AxisY * (Side * 6.f), SP + FVector(6.f, Side * 12.f, 9.f * Ks), 3.6f, 9.f * Ks, FVector(0, 0, 1));
			for (const float Side : { -1.f, 1.f })
				Bar(B[EMRVisMat::Black], SP + FVector(2.f, Side * 7.f, 12.f * Ks), FVector(L * 0.06f, Side * 6.f, SeatZ - 4.f), 1.8f, 2.4f, FVector(0, 0, 1)); // subframe
		}

		// Engine / motor
		if (bLightE)
		{
			// Mid motor ahead of the pivot, primary reduction case on the left
			Tube(B[EMRVisMat::Black], SP + FVector(14.f, -9.f, -4.f) * Ks, SP + FVector(14.f, 9.f, -4.f) * Ks, 8.f * Ks, 8.f * Ks, 24, true);
			Disc(B[EMRVisMat::Alloy], SP + FVector(14.f, 9.5f, -4.f) * Ks, AxisY, 0.f, 6.5f * Ks, 0.8f * Ks, 24);
			Bar(B[EMRVisMat::Black], SP + FVector(18.f, -10.f, -4.f) * Ks, SP + FVector(2.f, -10.f, 2.f) * Ks, 3.f * Ks, 9.f * Ks, FVector(0, 1, 0));
		}
		else if (bElectric)
		{
			Box(B[EMRVisMat::Black], EC + FVector(2.f, 0, 4.f) * Ks, FQuat::Identity, FVector(22.f, 14.f, 17.f) * Ks);
			Disc(B[EMRVisMat::Alloy], SP + FVector(8.f, 0, 2.f) * Ks, AxisY, 0.f, 9.f * Ks, 18.f * Ks, 32);
			for (int32 Fin = 0; Fin < 6; ++Fin)
				Box(B[EMRVisMat::Alloy], EC + FVector(-16.f + Fin * 6.f, 0, -13.f) * Ks, FQuat::Identity, FVector(1.f, 14.5f, 3.f) * Ks); // cooling ribs
			if (bVarg)
				for (const float Side : { -1.f, 1.f })
				{
					Box(B[EMRVisMat::Black], FVector(EC.X + 17.f * Ks, Side * 12.f * Ks, EC.Z + 22.f * Ks), FQuat(FVector::RightVector, Rad(-20.f)), FVector(2.f, 6.5f, 11.f) * Ks); // radiators
					Box(B[EMRVisMat::Paint], FVector(EC.X + 15.f * Ks, Side * 14.f * Ks, EC.Z + 26.f * Ks), FQuat(FVector::RightVector, Rad(-20.f)), FVector(8.f, 1.2f, 12.f) * Ks); // shrouds
				}
		}
		else
		{
			const float BlockW = (Cyl >= 3 ? 40.f : Cyl == 2 ? (bCruiser ? 24.f : 32.f) : 22.f) * Ks;
			Box(B[EMRVisMat::Black], EC, FQuat::Identity, FVector(15.f, BlockW * 0.5f, 13.f) * Ks);                         // crankcase
			Disc(B[EMRVisMat::Alloy], EC + FVector(-2.f, BlockW * 0.5f + 1.f, -2.f) * Ks, AxisY, 0.f, 10.f * Ks, 2.f * Ks, 28); // clutch cover
			Disc(B[EMRVisMat::Alloy], EC + FVector(2.f, -BlockW * 0.5f - 1.f, -1.f) * Ks, AxisY, 0.f, 8.5f * Ks, 2.f * Ks, 28); // stator cover
			auto Cylinder = [&](const FVector& Base, const FVector& Dir, float Len, float R)
			{
				Tube(B[EMRVisMat::Alloy], Base, Base + Dir * Len, R, R * 0.95f, 16, true);
				if (bAirCooled)
					for (int32 Fin = 1; Fin <= 6; ++Fin)
						Disc(B[EMRVisMat::Alloy], Base + Dir * (Len * Fin / 7.f), Dir, R, R * 1.45f, 0.4f, 20);
				Box(B[EMRVisMat::Black], Base + Dir * (Len + 3.f * Ks), FRotationMatrix::MakeFromZ(Dir).ToQuat(), FVector(R * 1.1f, R * 1.3f, 3.5f * Ks)); // head
			};
			if (bCruiser)
			{
				Cylinder(EC + FVector(4.f, 0, 10.f), FVector(FMath::Sin(Rad(30.f)), 0, FMath::Cos(Rad(30.f))), 20.f, 7.5f);
				Cylinder(EC + FVector(-4.f, 0, 10.f), FVector(-FMath::Sin(Rad(30.f)), 0, FMath::Cos(Rad(30.f))), 20.f, 7.5f);
			}
			else
			{
				const FVector CylDir = FVector(FMath::Sin(Rad(bDirt ? 12.f : 30.f)), 0, FMath::Cos(Rad(bDirt ? 12.f : 30.f)));
				for (int32 c = 0; c < Cyl; ++c)
				{
					const float Y = (c - (Cyl - 1) * 0.5f) * 9.f * Ks;
					Cylinder(EC + FVector(5.f, Y, 10.f) * FVector(Ks, 1.f, Ks), CylDir, (bDirt ? 16.f : 14.f) * Ks, (Cyl > 2 ? 4.4f : 6.f) * Ks);
				}
			}
			if (!bAirCooled)
			{
				if (bDirt)
				{
					for (const float Side : { -1.f, 1.f })
					{
						Box(B[EMRVisMat::Black], FVector(EC.X + 17.f * Ks, Side * 12.f * Ks, EC.Z + 22.f * Ks), FQuat(FVector::RightVector, Rad(-20.f)), FVector(2.f, 6.5f, 11.f) * Ks); // radiators
						Box(B[EMRVisMat::Paint], FVector(EC.X + 15.f * Ks, Side * 14.f * Ks, EC.Z + 26.f * Ks), FQuat(FVector::RightVector, Rad(-20.f)), FVector(8.f, 1.2f, 12.f) * Ks); // shrouds
					}
				}
				else
				{
					Box(B[EMRVisMat::Black], FVector(LowerClamp.X - 12.f * Ks, 0, EC.Z + 18.f * Ks), FQuat(FVector::RightVector, Rad(-15.f)), FVector(2.5f, 17.f, 14.f) * Ks);
				}
			}
			// Exhaust (right side)
			const FVector Port = EC + FVector(14.f, 0, 18.f) * Ks;
			if (bCruiser)
			{
				for (const float Z : { 0.f, -7.f })
					PathTube(B[EMRVisMat::Chrome], { Port + FVector(0, 4.f, Z), Port + FVector(6.f, 14.f, -16.f + Z), FVector(L * 0.45f, 15.f, RR - 2.f + Z * 0.5f), FVector(-L * 0.08f, 15.f, RR + 4.f + Z * 0.5f) }, 3.f, 12, true);
			}
			else if (bTwoStroke)
			{
				PathTube(B[EMRVisMat::Black], { Port, Port + FVector(10.f, 4.f, -14.f) * Ks, EC + FVector(4.f, 9.f, -16.f) * Ks, SP + FVector(-4.f, 11.f, 8.f) * Ks }, 4.f * Ks, 12, true); // expansion chamber
				Tube(B[EMRVisMat::Alloy], SP + FVector(-6.f, 12.f, 10.f) * Ks, FVector(L * 0.12f, 12.f * Ks, SeatZ - 14.f * Ks), 3.6f * Ks, 3.4f * Ks, 14, true);
			}
			else if (bDirt || bSupermoto)
			{
				PathTube(B[EMRVisMat::Chrome], { Port, Port + FVector(6.f, 6.f, -12.f) * Ks, EC + FVector(-2.f, 12.f, 2.f) * Ks, SP + FVector(-6.f, 13.f, 14.f) * Ks }, 2.2f * Ks, 12, true);
				Tube(B[EMRVisMat::Alloy], SP + FVector(-6.f, 13.f, 14.f) * Ks, FVector(L * 0.08f, 13.f * Ks, SeatZ - 12.f * Ks), 4.8f * Ks, 4.4f * Ks, 16, true);
			}
			else
			{
				for (int32 c = 0; c < Cyl; ++c)
				{
					const float Y = (c - (Cyl - 1) * 0.5f) * 9.f * Ks;
					PathTube(B[EMRVisMat::Chrome], { Port + FVector(0, Y, 0), Port + FVector(8.f, Y, -16.f) * Ks, EC + FVector(4.f, Y * 0.3f, -16.f) * Ks }, 2.f * Ks, 10, true);
				}
				PathTube(B[EMRVisMat::Chrome], { EC + FVector(4.f, 0, -16.f) * Ks, SP + FVector(-4.f, 10.f, -6.f) * Ks }, 2.6f * Ks, 10, true);
				Tube(B[EMRVisMat::Alloy], SP + FVector(-4.f, 12.f, -4.f) * Ks, SP + FVector(-30.f, 13.f, 8.f) * Ks, 5.5f * Ks, 4.8f * Ks, 16, true); // silencer
			}
		}

		// Tank, seat, tail, bodywork
		if (!bTrials && !bLightE) Ellipsoid(B[EMRVisMat::Paint], TankC, (bDirt ? FVector(17.f, 13.f, 9.f) : bCruiser ? FVector(24.f, 15.f, 10.f) : FVector(24.f, 17.f, 12.f)) * Ks, FQuat(FVector::RightVector, Rad(bDirt ? 6.f : -4.f)), 22, 12);
		if (bDirt || bSupermoto)
		{
			Bar(B[EMRVisMat::Seat], FVector(L * 0.66f, 0, SeatZ - 4.f * Ks), FVector(L * 0.14f, 0, SeatZ - 1.f * Ks), 21.f * Ks, 7.f * Ks);
			Bar(B[EMRVisMat::Paint], FVector(L * 0.16f, 0, SeatZ - 7.f * Ks), FVector(-L * 0.06f, 0, SeatZ + 3.f * Ks), 18.f * Ks, 1.f, FVector(0, 0, 1)); // rear fender
			for (const float Side : { -1.f, 1.f })
				Box(B[EMRVisMat::Paint], FVector(L * 0.3f, Side * 11.f * Ks, SeatZ - 14.f * Ks), FQuat(FVector::RightVector, Rad(8.f)), FVector(12.f, 1.f, 9.f) * Ks); // side plates
		}
		else if (bTrials)
		{
			Box(B[EMRVisMat::Seat], FVector(L * 0.25f, 0, SeatZ - 2.f), FQuat::Identity, FVector(8.f, 7.f, 2.f));
			Bar(B[EMRVisMat::Paint], FVector(L * 0.25f, 0, SeatZ - 4.f), FVector(-L * 0.04f, 0, SeatZ + 2.f), 16.f, 1.f, FVector(0, 0, 1));
			Ellipsoid(B[EMRVisMat::Paint], FVector(L * 0.6f, 0, SeatZ - 4.f), FVector(14.f, 11.f, 6.f), FQuat::Identity, 16, 8);
		}
		else
		{
			const FVector SeatC(L * (bCruiser ? 0.3f : 0.36f), 0, SeatZ - 3.f * Ks);
			Ellipsoid(B[EMRVisMat::Seat], SeatC, FVector(bCruiser ? 20.f : 17.f, bCruiser ? 17.f : 13.f, 4.5f) * Ks, FQuat::Identity, 20, 8);
			Ellipsoid(B[EMRVisMat::Seat], SeatC + FVector(-24.f, 0, 4.f) * Ks, FVector(13.f, 11.f, 4.f) * Ks, FQuat::Identity, 18, 8);   // pillion
			if (!bCruiser)
			{
				Ellipsoid(B[EMRVisMat::Paint], SeatC + FVector(-30.f, 0, 1.f) * Ks, FVector(22.f, 11.f, 7.f) * Ks, FQuat(FVector::RightVector, Rad(-10.f)), 18, 8); // tail unit
				Box(B[EMRVisMat::RedLens], SeatC + FVector(-52.f, 0, 2.f) * Ks, FQuat::Identity, FVector(1.f, 5.f, 1.5f) * Ks);
				Bar(B[EMRVisMat::Black], SeatC + FVector(-46.f, 0, -2.f) * Ks, SeatC + FVector(-56.f, 0, -18.f) * Ks, 10.f * Ks, 1.f); // hugger / plate holder
				Box(B[EMRVisMat::Plate], SeatC + FVector(-56.f, 0, -21.f) * Ks, FQuat(FVector::RightVector, Rad(-10.f)), FVector(0.5f, 10.f, 6.f) * Ks); // UK rear number plate (yellow)
			}
			else
			{
				ArcStrip(B[EMRVisMat::Paint], RearAxle, RR + 4.f, 18.f, Rad(15.f), Rad(150.f), 22, 0.8f);
			}
		}
		if (bFaired)
		{
			const FVector Nose = FVector(UpperClamp.X + 10.f * Ks, 0, UpperClamp.Z - 8.f * Ks);
			Ellipsoid(B[EMRVisMat::Paint], Nose, FVector(26.f, 20.f, 18.f) * Ks, FQuat(FVector::RightVector, Rad(-12.f)), 24, 12);
			Ellipsoid(B[EMRVisMat::Black], Nose + FVector(-4.f, 0, 17.f) * Ks, FVector(16.f, 14.f, 2.f) * Ks, FQuat(FVector::RightVector, Rad(-40.f)), 20, 8); // screen
			for (const float Side : { -1.f, 1.f })
			{
				Box(B[EMRVisMat::Paint], FVector(EC.X + 6.f * Ks, Side * 18.f * Ks, EC.Z + 10.f * Ks), FQuat(FVector::RightVector, Rad(-14.f)), FVector(24.f, 1.5f, 15.f) * Ks); // side panels
				Box(B[EMRVisMat::Lens], Nose + FVector(22.f, Side * 7.f, -2.f) * Ks, FQuat(FVector::RightVector, Rad(-12.f)), FVector(1.f, 4.5f, 2.f) * Ks);
			}
			V.FrontLight = Nose + FVector(24.f, 0, -2.f) * Ks;
			V.bHasFrontLight = true;
		}
		if (bAdventure)
		{
			for (const float Side : { -1.f, 1.f })
				Box(B[EMRVisMat::Black], FVector(L * 0.1f, Side * 24.f, RR + 22.f), FQuat::Identity, FVector(20.f, 7.f, 16.f)); // panniers
		}

		// Swingarm + chain (rotates about SP)
		V.bHasSwingarm = true;
		V.SwingPivot = SP;
		{
			FMRPartMesh& Sw = V.Swingarm;
			auto Loc = [&](const FVector& X) { return X - SP; };
			for (const float Side : { -1.f, 1.f })
			{
				Bar(Sw[EMRVisMat::Alloy], Loc(SP + AxisY * (Side * 9.f * Ks)), Loc(RearAxle + AxisY * (Side * 11.f * Ks)), 3.f * Ks, (bDirt ? 6.f : 8.f) * Ks, FVector(0, 0, 1));
				Box(Sw[EMRVisMat::Black], Loc(RearAxle + AxisY * (Side * 11.5f * Ks)), FQuat::Identity, FVector(3.f, 1.f, 2.f) * Ks); // adjusters
			}
			Tube(Sw[EMRVisMat::Black], Loc(SP - AxisY * 12.f * Ks), Loc(SP + AxisY * 12.f * Ks), 1.6f * Ks, 1.6f * Ks, 12, true);
			if (!bElectric || bDirt)
			{
				const FVector Fs = SP + FVector(9.f, 13.f, 2.f) * Ks;
				const float FsR = 4.f * Ks, RsR = (bDirt ? 11.f : 10.f) * Ks;
				PathTube(Sw[EMRVisMat::Black], { Loc(Fs + FVector(0, 0, FsR)), Loc(RearAxle + FVector(0, 13.f * Ks, RsR)) }, 0.55f * Ks, 6, false);
				PathTube(Sw[EMRVisMat::Black], { Loc(Fs - FVector(0, 0, FsR)), Loc(RearAxle + FVector(0, 13.f * Ks, -RsR)) }, 0.55f * Ks, 6, false);
				Disc(Sw[EMRVisMat::Black], Loc(Fs), AxisY, 0.f, FsR, 0.6f, 20);
			}
			if (!bCruiser) ArcStrip(Sw[EMRVisMat::Black], Loc(RearAxle), RR + 3.f, 14.f * Ks, Rad(70.f), Rad(130.f), 10, 0.6f); // hugger
		}
		// Rear shock(s)
		if (bCruiser)
		{
			for (const float Side : { -1.f, 1.f })
			{
				const FVector A = RearAxle + FVector(10.f, Side * 12.f, 3.f), Bt = FVector(L * 0.14f, Side * 12.f, SeatZ - 8.f);
				Tube(B[EMRVisMat::Chrome], A, Bt, 1.6f, 1.6f, 10, true);
				Helix(B[EMRVisMat::Chrome], FMath::Lerp(A, Bt, 0.15f), FMath::Lerp(A, Bt, 0.85f), 3.f, 0.5f, 7.f);
			}
		}
		else
		{
			const FVector A = SP + FVector(-10.f, 0, -2.f) * Ks, Bt = SP + FVector(-4.f, 0, 30.f) * Ks;
			Tube(B[EMRVisMat::Black], A, Bt, 2.f * Ks, 2.f * Ks, 12, true);
			Helix(B[bDirt ? EMRVisMat::Gold : EMRVisMat::Paint], FMath::Lerp(A, Bt, 0.12f), FMath::Lerp(A, Bt, 0.82f), 3.2f * Ks, 0.6f * Ks, 7.f);
		}
		// Footpegs and levers
		for (const float Side : { -1.f, 1.f })
		{
			Tube(B[EMRVisMat::Black], SP + FVector(6.f, Side * 9.f, -2.f) * Ks, SP + FVector(6.f, Side * 19.f, -2.f) * Ks, 1.4f * Ks, 1.4f * Ks, 8, true);
			Bar(B[EMRVisMat::Alloy], SP + FVector(6.f, Side * 15.f, -2.f) * Ks, SP + FVector(20.f, Side * 15.f, 0.f) * Ks, 1.f, 1.6f);
		}

		// --- Steering (SteerPivot space)
		const FVector SteerOff(L, 0.f, 0.f);
		auto Sv = [&](const FVector& X) { return X - SteerOff; };
		auto Lv = [&](const FVector& X) { return X - FrontAxle; };
		FMRPartMesh& St = V.Steer;
		FMRPartMesh& Lw = V.FrontLowers;
		Bar(St[EMRVisMat::Alloy], Sv(LowerClamp - AxisY * (ForkY + 3.f)), Sv(LowerClamp + AxisY * (ForkY + 3.f)), 5.f * Ks, 3.f * Ks, D);
		Bar(St[EMRVisMat::Alloy], Sv(UpperClamp - AxisY * (ForkY + 3.f)), Sv(UpperClamp + AxisY * (ForkY + 3.f)), 5.f * Ks, 2.5f * Ks, D);
		const float Split = AC * 0.5f;
		for (const float Side : { -1.f, 1.f })
		{
			const FVector Top = UpperClamp + D * 2.f + AxisY * (Side * ForkY);
			if (bUsd)
			{
				Tube(St[EMRVisMat::Black], Sv(Top), Sv(LowerClamp - D * (AC - Split - 4.f) + AxisY * (Side * ForkY)), 3.1f * Ks, 3.1f * Ks, 16, true);   // outer tubes
				Tube(Lw[bDirt ? EMRVisMat::Gold : EMRVisMat::Chrome], Lv(FrontAxle + D * 2.f + AxisY * (Side * ForkY)), Lv(FrontAxle + D * (Split + 4.f) + AxisY * (Side * ForkY)), 2.4f * Ks, 2.4f * Ks, 14, true); // inner stanchions
			}
			else
			{
				Tube(St[EMRVisMat::Chrome], Sv(Top), Sv(LowerClamp - D * (AC - Split - 4.f) + AxisY * (Side * ForkY)), 2.f * Ks, 2.f * Ks, 14, true);
				Tube(Lw[EMRVisMat::Alloy], Lv(FrontAxle - D * 2.f + AxisY * (Side * ForkY)), Lv(FrontAxle + D * (Split + 2.f) + AxisY * (Side * ForkY)), 2.8f * Ks, 2.6f * Ks, 14, true);
			}
			Box(Lw[EMRVisMat::Black], Lv(FrontAxle + AxisY * (Side * ForkY)), FQuat::Identity, FVector(3.f, 1.4f, 3.f) * Ks); // axle lug
		}
		// Bars
		const FVector BarBase = UpperClamp + D * 3.f;
		if (bFaired && !Has(Id, TEXT("tourer")))
		{
			for (const float Side : { -1.f, 1.f })
			{
				const FVector A = UpperClamp - D * 3.f + AxisY * (Side * ForkY);
				const FVector Bt = A + FVector(-12.f, Side * 14.f, -3.f) * Ks;
				Tube(St[EMRVisMat::Black], Sv(A), Sv(Bt), 1.1f * Ks, 1.1f * Ks, 10, true);
				Tube(St[EMRVisMat::Rubber], Sv(FMath::Lerp(A, Bt, 0.35f)), Sv(Bt), 1.7f * Ks, 1.7f * Ks, 10, true);
			}
			V.Bars = UpperClamp - D * 3.f + FVector(-8.f, 0, -2.f) * Ks;
			V.BarHalfWidth = (ForkY + 12.f) * Ks;
		}
		else
		{
			const float H = (bCruiser ? 44.f : bOffroadStyle || bTrials ? 41.f : 38.f) * FMath::Max(Ks, 0.75f);
			const FVector Rise = bCruiser ? FVector(-22.f, 0, 22.f) : FVector(-3.f, 0, (bDirt || bTrials ? 8.f : 5.f)) * Ks;
			const FVector Cb = BarBase + Rise;
			for (const float Side : { -1.f, 1.f }) // risers from the top clamp up to the bar
				PathTube(St[bCruiser ? EMRVisMat::Chrome : EMRVisMat::Black], { Sv(UpperClamp + AxisY * (Side * 4.f)), Sv(Cb + FVector(0, Side * H * 0.15f, -2.f)) }, 1.6f * Ks, 10, true);
			TArray<FVector> Pts = { Sv(Cb + FVector(-6.f, -H, 2.f)), Sv(Cb + FVector(-2.f, -H * 0.45f, 0)), Sv(Cb + FVector(0, -H * 0.15f, -2.f)),
				Sv(Cb + FVector(0, H * 0.15f, -2.f)), Sv(Cb + FVector(-2.f, H * 0.45f, 0)), Sv(Cb + FVector(-6.f, H, 2.f)) };
			PathTube(St[bCruiser ? EMRVisMat::Chrome : EMRVisMat::Black], Pts, 1.4f * Ks, 10, true);
			if (bDirt || bTrials) Tube(St[EMRVisMat::Black], Sv(Cb + FVector(-1.f, -H * 0.4f, 4.f)), Sv(Cb + FVector(-1.f, H * 0.4f, 4.f)), 1.f * Ks, 1.f * Ks, 8, true); // crossbar pad
			for (const float Side : { -1.f, 1.f })
			{
				const FVector End = Cb + FVector(-6.f, Side * H, 2.f);
				Tube(St[EMRVisMat::Rubber], Sv(End - FVector(-1.2f, Side * 13.f, 0.5f)), Sv(End), 1.8f * Ks, 1.8f * Ks, 12, true);
				Bar(St[EMRVisMat::Alloy], Sv(End - FVector(-1.f, Side * 15.f, 0)), Sv(End + FVector(5.f, Side * -2.f, -1.f)), 1.f, 1.4f); // levers
				if (!bOffroadStyle && !bTrials)
				{
					Tube(St[EMRVisMat::Black], Sv(End - FVector(0, Side * 8.f, -1.f)), Sv(End - FVector(-4.f, Side * 4.f, -16.f)), 0.5f, 0.5f, 6, false); // mirror stalk
					Ellipsoid(St[EMRVisMat::Black], Sv(End - FVector(-4.f, Side * 4.f, -18.f)), FVector(2.f, 6.f, 3.5f), FQuat::Identity, 12, 6);
				}
			}
			V.Bars = Cb + FVector(-5.f, 0, 1.f);
			V.BarHalfWidth = H - 7.f;
		}
		if (!bDirt && !bTrials && !bFaired)
		{
			const FVector Lamp = LowerClamp + FVector(12.f, 0, 4.f) * Ks;
			Ellipsoid(St[bRetro || bCruiser ? EMRVisMat::Chrome : EMRVisMat::Black], Sv(Lamp), FVector(8.f, 9.f, 9.f) * Ks, FQuat::Identity, 18, 10);
			Disc(St[EMRVisMat::Lens], Sv(Lamp + FVector(7.f, 0, 0) * Ks), FVector(1, 0, 0), 0.f, 7.f * Ks, 0.6f, 24);
			Box(St[EMRVisMat::Black], Sv(UpperClamp + D * 5.f + FVector(2.f, 0, 2.f)), FQuat(FVector::RightVector, Rad(-25.f)), FVector(3.f, 9.f, 6.f) * Ks); // clocks
			V.FrontLight = Lamp + FVector(8.f, 0, 0);
			V.bHasFrontLight = true;
		}
		if (bDirt || bSupermoto || bTrials)
		{
			Box(St[EMRVisMat::Paint], Sv(UpperClamp + FVector(6.f, 0, -2.f) * Ks), FQuat(FVector::RightVector, Rad(-(90.f - Rake) + 90.f - 20.f)), FVector(1.f, 12.f, 11.f) * Ks); // number plate
			Bar(St[EMRVisMat::Paint], Sv(LowerClamp + FVector(0, 0, -3.f) * Ks), Sv(LowerClamp + FVector(38.f, 0, -14.f) * Ks), 16.f * Ks, 0.8f, FVector(0, 0, 1)); // high fender
		}
		else
		{
			ArcStrip(Lw[EMRVisMat::Paint], FVector::ZeroVector, RF + 3.f, 14.f * Ks, Rad(25.f), Rad(140.f), 18, 0.6f); // low front mudguard
		}

		// --- Wheels
		FWheelSpec Wf, Wr;
		Wf.R = RF; Wr.R = RR;
		Wf.TyreW = (bDirt ? 8.f : bCruiser ? 13.f : bFaired ? 12.f : 11.f) * Ks;
		Wr.TyreW = (bDirt ? 11.f : bCruiser ? 18.f : bFaired ? 18.f : 16.f) * Ks;
		Wf.TyreH = Wf.TyreW * (bDirt ? 1.f : 0.75f);
		Wr.TyreH = Wr.TyreW * (bDirt ? 0.95f : 0.62f);
		Wf.Knobs = Wr.Knobs = bDirt ? 2 : 0;
		const bool bLaced = bDirt || bAdventure || bRetro || bCruiser || bTrials;
		Wf.Spokes = Wr.Spokes = bLaced ? 36 : 0;
		Wf.SpokeR = Wr.SpokeR = 0.22f * Ks;
		Wf.CastArms = Wr.CastArms = bLaced ? 0 : 5;
		Wf.RimDepth = Wr.RimDepth = bLaced ? 2.5f : 3.f;
		Wf.bRimBlack = Wr.bRimBlack = !bRetro && !bCruiser;
		Wf.HubR = Wr.HubR = 5.f * Ks;
		Wf.HubHalf = 6.f * Ks; Wr.HubHalf = 8.f * Ks;
		Wf.RotorR = (bDirt ? 13.f : bFaired || bSupermoto ? 16.f : 14.5f) * Ks;
		Wf.bRotorBothSides = !bDirt && !Has(Id, TEXT("125")) && !bTrials;
		Wr.RotorR = (bDirt ? 11.f : 11.f) * Ks;
		Wr.SprocketR = (bElectric && !bDirt) ? 0.f : (bDirt ? 11.f : 10.f) * Ks;
		BuildWheel(V.FrontWheel, Wf);
		BuildWheel(V.RearWheel, Wr);
		Caliper(Lw[EMRVisMat::Black], FVector::ZeroVector, Wf.RotorR, -(Wf.HubHalf + 0.9f), 120.f, 2.f * Ks);
		if (Wf.bRotorBothSides) Caliper(Lw[EMRVisMat::Black], FVector::ZeroVector, Wf.RotorR, Wf.HubHalf + 0.9f, 120.f, 2.f * Ks);

		// Rider
		V.Foot = SP + FVector(6.f, 0, -2.f) * Ks;
		V.FootHalfWidth = 15.f * Ks;
		V.Seat = bTrials ? FVector(L * 0.4f, 0, SeatZ + 26.f) : FVector(L * (bDirt ? 0.46f : bCruiser ? 0.3f : 0.37f), 0, SeatZ + 3.f * Ks);
		V.Head = V.Seat + (bFaired ? FVector(26.f, 0, 52.f) : bCruiser ? FVector(-2.f, 0, 74.f) : FVector(12.f, 0, 70.f)) * FMath::Max(Ks, 0.8f);
	}

	// ------------------------------------------------------------------ e-scooters
	void BuildScooter(FMRVehicleVisual& V, const MRSim::FVehicleParams& P, const FString& Id)
	{
		const float L = P.Wheelbase * 100.f, RF = P.WheelRadiusFront * 100.f, RR = P.WheelRadiusRear * 100.f;
		const bool bOff = Has(Id, TEXT("offroad")) || P.Tyre == ETyre::Knobbly;
		const bool bSprung = P.Front.Travel > 0.025f;
		const bool bDual = Has(Id, TEXT("dual")) || bOff;
		const float DeckZ = RR + (bSprung ? 9.f : 6.f);
		const float DeckW = bOff ? 22.f : bDual ? 20.f : 16.f;
		const FVector RearAxle(0, 0, RR), FrontAxle(L, 0, RF);
		const float HA = bOff ? 72.f : 76.f;
		const FVector D(-FMath::Cos(Rad(HA)), 0, FMath::Sin(Rad(HA)));
		const float X0 = RR * 0.9f, X1 = L * 0.74f;

		FMRPartMesh& B = V.Body;
		Box(B[EMRVisMat::Paint], FVector((X0 + X1) * 0.5f, 0, DeckZ - 2.5f), FQuat::Identity, FVector((X1 - X0) * 0.5f, DeckW * 0.5f, 2.5f)); // deck shell
		Box(B[EMRVisMat::Rubber], FVector((X0 + X1) * 0.5f, 0, DeckZ + 0.15f), FQuat::Identity, FVector((X1 - X0) * 0.5f - 1.f, DeckW * 0.5f - 1.f, 0.2f)); // grip tape
		for (const float Side : { -1.f, 1.f })
			Bar(B[EMRVisMat::Black], FVector(X0, Side * (DeckW * 0.5f - 1.f), DeckZ - 2.f), RearAxle + AxisY * (Side * 5.f), 2.f, 3.f); // rear fork
		ArcStrip(B[EMRVisMat::Black], RearAxle, RR + 2.4f, DeckW * 0.6f, Rad(35.f), Rad(175.f), 18, 0.6f);
		Box(B[EMRVisMat::RedLens], RearAxle + FVector(-RR - 2.f, 0, RR * 0.6f), FQuat::Identity, FVector(0.8f, 3.f, 1.f));
		const FVector Crown = FrontAxle + D * (RF + 9.f);
		const FVector Neck = Crown + D * 8.f;
		Bar(B[EMRVisMat::Paint], FVector(X1 - 4.f, 0, DeckZ - 2.f), Neck - D * 3.f, 7.f, 5.f, FVector(0, 0, 1));            // neck
		Tube(B[EMRVisMat::Black], Neck - D * 4.f, Neck + D * 4.f, 2.4f, 2.4f, 14, true);                                    // head tube
		if (bSprung)
		{
			for (const float Side : { -1.f, 1.f })
				Helix(B[EMRVisMat::Gold], FVector(X0 + 2.f, Side * 4.f, DeckZ - 4.f), RearAxle + FVector(4.f, Side * 4.f, 3.f), 1.4f, 0.3f, 6.f);
		}

		const FVector SteerOff(L, 0, 0);
		auto Sv = [&](const FVector& X) { return X - SteerOff; };
		auto Lv = [&](const FVector& X) { return X - FrontAxle; };
		FMRPartMesh& St = V.Steer;
		FMRPartMesh& Lw = V.FrontLowers;
		const float StemTopZ = 108.f;
		const float StemLen = (StemTopZ - Neck.Z) / D.Z;
		const FVector StemTop = Neck + D * StemLen;
		Tube(St[EMRVisMat::Paint], Sv(Crown), Sv(StemTop), 2.f, 1.8f, 14, true);                                      // stem
		Box(St[EMRVisMat::Black], Sv(Neck + D * 6.f + FVector(2.f, 0, 0)), FQuat::Identity, FVector(3.f, 3.f, 4.f)); // fold latch
		Bar(St[EMRVisMat::Black], Sv(Crown - AxisY * 6.f), Sv(Crown + AxisY * 6.f), 3.f, 3.f, D);
		for (const float Side : { -1.f, 1.f })
		{
			Tube(St[EMRVisMat::Black], Sv(Crown + AxisY * (Side * 5.f)), Sv(Crown - D * 6.f + AxisY * (Side * 5.f)), 1.3f, 1.3f, 10, true);
			Tube(Lw[EMRVisMat::Black], Lv(FrontAxle + AxisY * (Side * 5.f)), Lv(FrontAxle + D * (RF + 2.f) + AxisY * (Side * 5.f)), 1.3f, 1.3f, 10, true);
			if (bSprung) Helix(Lw[EMRVisMat::Gold], Lv(FrontAxle + D * 4.f + AxisY * (Side * 5.f)), Lv(FrontAxle + D * (RF + 2.f) + AxisY * (Side * 5.f)), 1.6f, 0.3f, 5.f);
		}
		const float H = bOff ? 31.f : 26.f;
		PathTube(St[EMRVisMat::Black], { Sv(StemTop + FVector(0, -H, 0)), Sv(StemTop + FVector(0, H, 0)) }, 1.1f, 10, true);
		for (const float Side : { -1.f, 1.f })
		{
			Tube(St[EMRVisMat::Rubber], Sv(StemTop + FVector(0, Side * (H - 11.f), 0)), Sv(StemTop + FVector(0, Side * (H + 0.5f), 0)), 1.6f, 1.6f, 12, true);
			Bar(St[EMRVisMat::Alloy], Sv(StemTop + FVector(0, Side * (H - 12.f), 0.5f)), Sv(StemTop + FVector(5.f, Side * (H - 4.f), -1.f)), 0.8f, 1.2f);
		}
		Box(St[EMRVisMat::Black], Sv(StemTop + FVector(1.f, 0, 2.f)), FQuat(FVector::RightVector, Rad(-20.f)), FVector(4.f, 4.f, 1.2f)); // display
		Box(St[EMRVisMat::Black], Sv(StemTop + FVector(0, H - 13.f, -1.f)), FQuat::Identity, FVector(1.5f, 1.5f, 1.5f));                 // throttle
		V.FrontLight = FMath::Lerp(Crown, StemTop, 0.55f) + FVector(2.5f, 0, 0);
		V.bHasFrontLight = true;
		Ellipsoid(St[EMRVisMat::Black], Sv(V.FrontLight), FVector(2.f, 3.5f, 2.f), FQuat::Identity, 12, 6);
		Disc(St[EMRVisMat::Lens], Sv(V.FrontLight + FVector(1.8f, 0, 0)), FVector(1, 0, 0), 0.f, 1.8f, 0.4f, 16);
		ArcStrip(Lw[EMRVisMat::Black], FVector::ZeroVector, RF + 2.4f, DeckW * 0.55f, Rad(40.f), Rad(140.f), 14, 0.6f);

		FWheelSpec W;
		W.TyreW = (bOff ? 9.f : 5.5f) * FMath::Max(RR / 12.5f, 0.8f);
		W.TyreH = W.TyreW * 0.95f;
		W.Knobs = bOff ? 1 : 0;
		W.Spokes = 0;
		W.CastArms = 5;
		W.RimDepth = 1.5f;
		W.HubR = 2.5f;
		W.HubHalf = 3.5f;
		W.RotorR = bDual || bOff ? 6.5f : 0.f;
		FWheelSpec Wf = W, Wr = W;
		Wf.R = RF; Wr.R = RR;
		Wf.HubMotorR = RF * 0.48f;
		if (bDual) Wr.HubMotorR = RR * 0.48f;
		else Wr.RotorR = 5.5f;
		BuildWheel(V.FrontWheel, Wf);
		BuildWheel(V.RearWheel, Wr);

		V.Bars = StemTop;
		V.BarHalfWidth = H - 6.f;
		V.Foot = FVector(L * 0.42f, 0, DeckZ + 3.f);
		V.FootHalfWidth = FMath::Min(DeckW * 0.5f - 4.f, 7.f);
		V.bStanding = true;
		V.Seat = FVector(L * 0.4f, 0, DeckZ + 95.f);
		V.Head = FVector(L * 0.5f, 0, DeckZ + 158.f);
	}
}

FMRVehicleVisual MRBuildVehicleVisual(const MRSim::FVehicleParams& P, const FString& StyleId)
{
	FMRVehicleVisual V;
	switch (P.Category)
	{
	case EVehicleCategory::EScooter: BuildScooter(V, P, StyleId); break;
	case EVehicleCategory::DirtBike:
	case EVehicleCategory::Motorcycle: BuildMotorcycle(V, P, StyleId); break;
	default: BuildBicycle(V, P, StyleId); break;
	}
	return V;
}
