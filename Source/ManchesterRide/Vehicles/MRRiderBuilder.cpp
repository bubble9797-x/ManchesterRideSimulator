#include "Vehicles/MRRiderBuilder.h"

using RM = EMRRiderMat;
using RS = EMRRiderSeg;

namespace
{
	// ------------------------------------------------------------------ mesh helpers
	int32 AddV(FMRMeshSection& S, const FVector& P, const FVector& N)
	{
		S.V.Add(P);
		S.N.Add(N);
		return S.V.Num() - 1;
	}

	// Same convention as the vehicle builder: orient each triangle along its vertex normals.
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

	/** One cross-section of a loft: position along the axis, elliptical radii and centre offset. */
	struct FSec
	{
		float X, RY, RZ, OY, OZ;
	};

	float CatRom(float A, float B, float C, float D, float T)
	{
		return 0.5f * ((2.f * B) + (-A + C) * T + (2.f * A - 5.f * B + 4.f * C - D) * T * T + (-A + 3.f * B - 3.f * C + D) * T * T * T);
	}

	/**
	 * Smooth organic tube along Ax through elliptical sections (Catmull-Rom between them), with
	 * normals from the surface itself and capped ends. This is what shapes every limb and the torso.
	 */
	void Loft(FMRMeshSection& S, const FVector& O, const FVector& Ax, const FVector& Ay, const FVector& Az, const TArray<FSec>& In, int32 Sides = 18, int32 Sub = 3)
	{
		const int32 N = In.Num();
		if (N < 2) return;
		TArray<FSec> R;
		for (int32 i = 0; i < N - 1; ++i)
		{
			const FSec& P0 = In[FMath::Max(i - 1, 0)];
			const FSec& P1 = In[i];
			const FSec& P2 = In[i + 1];
			const FSec& P3 = In[FMath::Min(i + 2, N - 1)];
			for (int32 k = 0; k < Sub; ++k)
			{
				const float t = static_cast<float>(k) / Sub;
				R.Add({ FMath::Lerp(P1.X, P2.X, t),
					FMath::Max(0.05f, CatRom(P0.RY, P1.RY, P2.RY, P3.RY, t)), FMath::Max(0.05f, CatRom(P0.RZ, P1.RZ, P2.RZ, P3.RZ, t)),
					CatRom(P0.OY, P1.OY, P2.OY, P3.OY, t), CatRom(P0.OZ, P1.OZ, P2.OZ, P3.OZ, t) });
			}
		}
		R.Add(In.Last());
		const int32 Rn = R.Num(), W = Sides + 1;
		TArray<FVector> P;
		P.SetNum(Rn * W);
		for (int32 r = 0; r < Rn; ++r)
			for (int32 j = 0; j <= Sides; ++j)
			{
				const float A = 2.f * PI * (j % Sides) / Sides;
				P[r * W + j] = O + Ax * R[r].X + Ay * (R[r].OY + R[r].RY * FMath::Cos(A)) + Az * (R[r].OZ + R[r].RZ * FMath::Sin(A));
			}
		const int32 Base = S.V.Num();
		for (int32 r = 0; r < Rn; ++r)
		{
			const FVector Ctr = O + Ax * R[r].X + Ay * R[r].OY + Az * R[r].OZ;
			const int32 rp = FMath::Max(r - 1, 0), rn = FMath::Min(r + 1, Rn - 1);
			for (int32 j = 0; j <= Sides; ++j)
			{
				const int32 jj = j % Sides, jn = (jj + 1) % Sides, jp = (jj + Sides - 1) % Sides;
				const FVector dT = P[rn * W + jj] - P[rp * W + jj];
				const FVector dS = P[r * W + jn] - P[r * W + jp];
				FVector Nm = FVector::CrossProduct(dS, dT).GetSafeNormal();
				const FVector Rad = P[r * W + jj] - Ctr;
				if (FVector::DotProduct(Nm, Rad) < 0.f) Nm = -Nm;
				if (Nm.IsNearlyZero()) Nm = Rad.GetSafeNormal();
				AddV(S, P[r * W + j], Nm);
			}
		}
		for (int32 r = 0; r < Rn - 1; ++r)
			for (int32 j = 0; j < Sides; ++j)
				Quad(S, Base + r * W + j, Base + (r + 1) * W + j, Base + (r + 1) * W + j + 1, Base + r * W + j + 1);
		// Caps
		for (int32 End = 0; End < 2; ++End)
		{
			const int32 r = End == 0 ? 0 : Rn - 1;
			const FVector Ctr = O + Ax * R[r].X + Ay * R[r].OY + Az * R[r].OZ;
			const FVector Nc = End == 0 ? -Ax : Ax;
			const int32 C = AddV(S, Ctr, Nc);
			const int32 RingBase = S.V.Num();
			for (int32 j = 0; j <= Sides; ++j) AddV(S, P[r * W + j], Nc);
			for (int32 j = 0; j < Sides; ++j) AddTri(S, C, RingBase + j, RingBase + j + 1);
		}
	}

	FVector Unit(float AzDeg, float ElDeg)
	{
		const float A = FMath::DegreesToRadians(AzDeg), E = FMath::DegreesToRadians(ElDeg);
		return FVector(FMath::Cos(E) * FMath::Cos(A), FMath::Cos(E) * FMath::Sin(A), FMath::Sin(E));
	}

	/** Point and normal on an ellipsoid. Azimuth from +X towards +Y, elevation up from the XY plane. */
	void OnEllipsoid(const FVector& C, const FVector& R, float AzDeg, float ElDeg, FVector& OutP, FVector& OutN)
	{
		const FVector U = Unit(AzDeg, ElDeg);
		OutP = C + U * R;
		OutN = FVector(U.X / R.X, U.Y / R.Y, U.Z / R.Z).GetSafeNormal();
	}

	/**
	 * Part of an ellipsoid surface. The lower edge can rise from back to front (E0Back at the back,
	 * E0Front at the front), which is how helmet rims and hairlines run.
	 */
	void Patch(FMRMeshSection& S, const FVector& C, const FVector& R, float A0, float A1, float E0Front, float E0Back, float E1, int32 NA, int32 NE, bool bInward = false,
		TArray<FVector>* OutLowEdge = nullptr)
	{
		const int32 Base = S.V.Num();
		for (int32 e = 0; e <= NE; ++e)
			for (int32 a = 0; a <= NA; ++a)
			{
				const float Az = FMath::Lerp(A0, A1, static_cast<float>(a) / NA);
				const float Blend = 0.5f * (1.f - FMath::Cos(FMath::DegreesToRadians(Az)));
				const float El0 = FMath::Lerp(E0Front, E0Back, Blend);
				const float El = FMath::Lerp(El0, E1, static_cast<float>(e) / NE);
				FVector P, Nm;
				OnEllipsoid(C, R, Az, El, P, Nm);
				AddV(S, P, bInward ? -Nm : Nm);
				if (OutLowEdge && e == 0) OutLowEdge->Add(P);
			}
		for (int32 e = 0; e < NE; ++e)
			for (int32 a = 0; a < NA; ++a)
			{
				const int32 I = Base + e * (NA + 1) + a;
				Quad(S, I, I + NA + 1, I + NA + 2, I + 1);
			}
	}

	/** Helmet shell piece: outer surface, inner liner and the rim along the lower edge. */
	void Shell(FMRMeshSection& Outer, FMRMeshSection& Inner, const FVector& C, const FVector& R, float Thick, float A0, float A1, float E0Front, float E0Back, float E1, int32 NA, int32 NE)
	{
		TArray<FVector> Lo, Li;
		Patch(Outer, C, R, A0, A1, E0Front, E0Back, E1, NA, NE, false, &Lo);
		Patch(Inner, C, R - FVector(Thick), A0, A1, E0Front, E0Back, E1, NA, NE, true, &Li);
		const int32 Base = Outer.V.Num();
		for (int32 a = 0; a <= NA; ++a)
		{
			const FVector Down = (Lo[a] - C).GetSafeNormal() * 0.3f + FVector(0, 0, -1.f);
			AddV(Outer, Lo[a], Down.GetSafeNormal());
			AddV(Outer, Li[a], Down.GetSafeNormal());
		}
		for (int32 a = 0; a < NA; ++a) Quad(Outer, Base + 2 * a, Base + 2 * a + 1, Base + 2 * a + 3, Base + 2 * a + 2);
	}

	void Ell(FMRMeshSection& S, const FVector& C, const FVector& R, const FQuat& Q = FQuat::Identity, int32 Seg = 16, int32 Rings = 9)
	{
		MRMeshKit::Ellipsoid(S, C, R, Q, Seg, Rings);
	}

	/** Small part lying on an ellipsoid's surface (vents, pads, logos), sunk by Sink along the normal. */
	void OnSurface(FMRMeshSection& S, const FVector& C, const FVector& R, float Az, float El, const FVector& Size, float Sink, float Roll = 0.f)
	{
		FVector P, Nm;
		OnEllipsoid(C, R, Az, El, P, Nm);
		FVector Fwd = FVector::CrossProduct(Nm, FVector::CrossProduct(FVector(1, 0, 0), Nm)).GetSafeNormal();
		if (Fwd.IsNearlyZero()) Fwd = FVector(0, 1, 0);
		FQuat Q = FRotationMatrix::MakeFromZX(Nm, Fwd).ToQuat();
		if (Roll != 0.f) Q = Q * FQuat(FVector::UpVector, FMath::DegreesToRadians(Roll));
		Ell(S, P - Nm * Sink, Size, Q, 12, 6);
	}

	// ------------------------------------------------------------------ torso profile
	// Skin-level torso sections as fractions of the torso length: (F, half-width, half-depth, forward offset).
	struct FRow { float F, RY, RX, OX; };
	const FRow GTorso[] = {
		{ -0.22f, 4.5f, 3.5f, -2.5f }, { -0.18f, 12.5f, 9.0f, -2.5f }, { -0.10f, 16.0f, 11.0f, -2.0f }, { 0.00f, 17.0f, 11.2f, -1.0f },
		{ 0.12f, 16.0f, 10.4f, 0.0f }, { 0.27f, 14.8f, 10.0f, 0.4f }, { 0.50f, 15.8f, 10.6f, 0.6f }, { 0.72f, 17.6f, 11.4f, 0.4f },
		{ 0.88f, 18.6f, 11.2f, -0.2f }, { 0.98f, 18.0f, 9.6f, -0.8f }, { 1.05f, 13.0f, 7.4f, -1.0f }, { 1.10f, 6.5f, 5.6f, -0.8f },
	};

	struct FBodyScale
	{
		float K = 1.f;  // height / 178
		float W = 1.f;  // torso width
		float Dp = 1.f; // torso depth
		float G = 1.f;  // limb girth
		float T = 51.f; // torso length
	};

	FRow TorsoAt(const FBodyScale& B, float F)
	{
		const int32 N = UE_ARRAY_COUNT(GTorso);
		int32 i = 0;
		while (i < N - 2 && GTorso[i + 1].F < F) ++i;
		const float t = FMath::Clamp((F - GTorso[i].F) / (GTorso[i + 1].F - GTorso[i].F), 0.f, 1.f);
		return { F, FMath::Lerp(GTorso[i].RY, GTorso[i + 1].RY, t) * B.K * B.W, FMath::Lerp(GTorso[i].RX, GTorso[i + 1].RX, t) * B.K * B.Dp,
			FMath::Lerp(GTorso[i].OX, GTorso[i + 1].OX, t) * B.K };
	}

	/** Torso band from F0 to F1, grown by Grow cm (clothing layers sit over the skin-level shape). */
	void TorsoLoft(FMRMeshSection& S, const FBodyScale& B, float F0, float F1, float Grow, float Steps = 8, float HemFlare = 0.f)
	{
		TArray<FSec> Secs;
		for (int32 i = 0; i <= Steps; ++i)
		{
			const float F = FMath::Lerp(F0, F1, static_cast<float>(i) / Steps);
			const FRow Rw = TorsoAt(B, F);
			const float Flare = i == 0 ? HemFlare : 0.f;
			Secs.Add({ F * B.T, Rw.RY + Grow + Flare, Rw.RX + Grow + Flare, 0.f, Rw.OX });
		}
		// Torso frame: X forward, Y right, Z up the spine. Loft along Z; section Y = width, section "Z" = depth along X.
		Loft(S, FVector::ZeroVector, FVector(0, 0, 1), FVector(0, 1, 0), FVector(1, 0, 0), Secs, 22, 2);
	}

	/** Point on the front (Dir = +1) or back (-1) of the torso surface at fraction F. */
	FVector TorsoSurface(const FBodyScale& B, float F, float Dir, float Grow)
	{
		const FRow Rw = TorsoAt(B, F);
		return FVector(Rw.OX + Dir * (Rw.RX + Grow), 0.f, F * B.T);
	}

	// ------------------------------------------------------------------ limbs (segment frame: X along the bone)
	void LimbLoft(FMRMeshSection& S, const TArray<FSec>& Secs, int32 Sides = 16)
	{
		Loft(S, FVector::ZeroVector, FVector(1, 0, 0), FVector(0, 1, 0), FVector(0, 0, 1), Secs, Sides, 3);
	}

	TArray<FSec> Grown(const TArray<FSec>& In, float G, float X0 = -1e9f, float X1 = 1e9f)
	{
		TArray<FSec> Out;
		for (const FSec& S : In)
			if (S.X >= X0 - 0.01f && S.X <= X1 + 0.01f) Out.Add({ S.X, S.RY + G, S.RZ + G, S.OY, S.OZ });
		return Out;
	}

	void BuildHand(FMRRiderPart& P, const FMRRiderDims& D, const FBodyScale& B, float Side, bool bGauntlet)
	{
		FMRMeshSection& G = P[RM::Gloves];
		const float Hd = D.Hand, k = B.K * FMath::Lerp(1.f, B.G, 0.5f);
		// Wrist and cuff
		LimbLoft(G, { { -2.5f, 3.0f * k, 2.4f * k, 0, 0 }, { 0.f, 3.1f * k, 2.4f * k, 0, 0.2f }, { 2.5f * k, 3.7f * k, 2.2f * k, 0, 0.6f } });
		if (bGauntlet) LimbLoft(G, { { -7.f * k, 4.6f * k, 4.0f * k, 0, 0 }, { -5.f * k, 4.6f * k, 4.0f * k, 0, 0 }, { -0.5f, 3.8f * k, 3.2f * k, 0, 0 }, { 0.5f, 3.2f * k, 2.6f * k, 0, 0 } });
		// Palm / back of the hand, resting on top of and behind the grip
		Ell(G, FVector(Hd * 0.48f, 0, 1.6f * k), FVector(4.4f, 4.1f, 1.9f) * k, FQuat(FVector(0, 1, 0), FMath::DegreesToRadians(20.f)), 16, 9);
		Ell(G, FVector(Hd - 1.2f * k, 0, 2.4f * k), FVector(1.6f, 4.2f, 1.5f) * k, FQuat::Identity, 14, 8); // knuckles
		// Four fingers wrapped round the grip (bar centre at (Hd, y, 0), bar along Y)
		const float BarR = 1.75f;
		const float Ys[4] = { -2.85f, -0.95f, 0.95f, 2.85f };
		const float Rf[4] = { 0.95f, 1.0f, 0.95f, 0.8f };
		const float EndAng[4] = { -105.f, -115.f, -110.f, -95.f };
		for (int32 f = 0; f < 4; ++f)
		{
			// f = 0 is the index finger (inner side, towards the stem)
			const float Y = Ys[f] * k * Side;
			const float Wr = BarR + Rf[f] * k;
			TArray<FVector> Pts;
			Pts.Add(FVector(Hd - 1.4f * k, Y, 2.5f * k));
			for (float A = 75.f; A >= EndAng[f]; A -= 30.f)
				Pts.Add(FVector(Hd + Wr * FMath::Cos(FMath::DegreesToRadians(A)), Y, Wr * FMath::Sin(FMath::DegreesToRadians(A))));
			MRMeshKit::PathTube(G, Pts, Rf[f] * k, 8, true);
		}
		// Thumb: from the inside of the palm, under the grip
		const float Yt = -Side * 4.0f * k;
		MRMeshKit::PathTube(G, { FVector(Hd * 0.2f, Yt * 0.7f, 0.4f * k), FVector(Hd - 2.2f * k, Yt, -1.4f * k), FVector(Hd - 0.2f * k, Yt * 0.95f, -BarR - 0.9f * k), FVector(Hd + 1.2f * k, Yt * 0.85f, -BarR - 0.6f * k) }, 1.05f * k, 8, true);
	}

	void BuildFoot(FMRRiderPart& P, const FMRRiderDims& D, const FBodyScale& B, bool bBoot)
	{
		const float k = B.K;
		FMRMeshSection& Sh = P[RM::Shoes];
		const float Sole = -D.AnkleH;
		// Heel -> toe; sections sit on the sole line (Sole) with the ankle at the origin.
		TArray<FSec> Secs = {
			{ -6.8f * k, 1.6f * k, 1.6f * k, 0, Sole + 3.0f * k }, { -6.1f * k, 4.0f * k, 3.7f * k, 0, Sole + 3.8f * k },
			{ -3.5f * k, 4.7f * k, 5.6f * k, 0, Sole + 5.6f * k }, { 0.f, 4.6f * k, 5.9f * k, 0, Sole + 6.3f * k },
			{ 4.f * k, 4.75f * k, 4.5f * k, 0, Sole + 4.9f * k }, { 9.f * k, 5.0f * k, 3.4f * k, 0, Sole + 3.7f * k },
			{ 13.5f * k, 5.05f * k, 2.75f * k, 0, Sole + 3.0f * k }, { 17.f * k, 4.5f * k, 2.3f * k, 0, Sole + 2.5f * k },
			{ 19.5f * k, 3.1f * k, 1.7f * k, 0, Sole + 2.0f * k }, { 20.6f * k, 1.4f * k, 1.0f * k, 0, Sole + 1.6f * k },
		};
		LimbLoft(Sh, Secs, 18);
		// Sole and heel
		MRMeshKit::Box(P[RM::Sole], FVector(6.9f * k, 0, Sole + 0.5f * k), FQuat::Identity, FVector(13.5f * k, 4.7f * k, 0.55f * k));
		MRMeshKit::Box(P[RM::Sole], FVector(-4.f * k, 0, Sole + 0.9f * k), FQuat::Identity, FVector(2.8f * k, 4.2f * k, 0.9f * k));
		if (bBoot)
		{
			// Motorbike boot: tall shaft, toe slider and buckles
			LimbLoft(Sh, { { -5.f * k, 0.3f, 0.3f, 0, Sole + 8.f * k }, { -4.5f * k, 5.2f * k, 4.6f * k, 0, Sole + 9.f * k }, { 1.5f * k, 5.4f * k, 4.4f * k, 0, Sole + 10.f * k } });
			Ell(P[RM::Strap], FVector(15.f * k, 0, Sole + 3.f * k), FVector(4.f, 5.2f, 2.6f) * k);
			for (int32 i = 0; i < 2; ++i) MRMeshKit::Box(P[RM::Reflect], FVector((1.5f - 3.f * i) * k, 0, Sole + 7.5f * k + i * 2.5f * k), FQuat::Identity, FVector(1.2f, 5.6f, 0.4f) * k);
		}
		else
		{
			// Trainers: laces and a contrasting midsole stripe
			for (int32 i = 0; i < 4; ++i)
				MRMeshKit::Bar(P[RM::Eye], FVector((2.f + 2.4f * i) * k, -2.f * k, Sole + (8.8f - 1.05f * i) * k), FVector((2.f + 2.4f * i) * k, 2.f * k, Sole + (8.8f - 1.05f * i) * k), 0.7f * k, 0.35f * k);
			LimbLoft(P[RM::Eye], { { -6.2f * k, 3.2f * k, 0.6f * k, 0, Sole + 1.6f * k }, { 0.f, 4.75f * k, 0.6f * k, 0, Sole + 1.5f * k }, { 13.5f * k, 5.15f * k, 0.6f * k, 0, Sole + 1.4f * k }, { 19.8f * k, 2.8f * k, 0.6f * k, 0, Sole + 1.4f * k } }, 16);
		}
	}

	// ------------------------------------------------------------------ head
	struct FHeadGeo
	{
		FVector SkullC, SkullR, JawC, JawR;
	};

	void BuildHead(FMRRiderPart& P, const FMRRiderLook& L, const FBodyScale& B, const FHeadGeo& H)
	{
		const float k = B.K;
		auto V = [k](float X, float Y, float Z) { return FVector(X, Y, Z) * k; };
		FMRMeshSection& Sk = P[RM::Skin];
		Ell(Sk, H.SkullC, H.SkullR, FQuat::Identity, 24, 14);
		Ell(Sk, H.JawC, H.JawR, FQuat::Identity, 20, 10);
		Ell(Sk, V(8.6f, 0, 1.2f), V(2.2f, 2.7f, 2.0f), FQuat::Identity, 14, 8);                              // chin
		for (const float S : { -1.f, 1.f })
		{
			Ell(Sk, V(6.4f, S * 4.6f, 7.0f), V(3.2f, 2.6f, 3.0f), FQuat::Identity, 14, 8);                    // cheekbones
			Ell(Sk, V(0.2f, S * 7.5f, 9.5f), V(2.4f, 0.9f, 3.1f), FQuat(FVector::UpVector, S * 0.25f), 12, 8); // ears
			Ell(P[RM::Eye], V(8.7f, S * 3.2f, 10.f), V(1.25f, 1.25f, 1.05f), FQuat::Identity, 12, 7);
			Ell(P[RM::Iris], V(9.75f, S * 3.2f, 10.f), V(0.35f, 0.62f, 0.62f), FQuat::Identity, 12, 7);
			Ell(P[RM::Hair], V(9.45f, S * 3.4f, 12.0f), V(0.55f, 1.8f, 0.42f), FQuat(FVector::ForwardVector, S * -0.12f), 10, 6); // eyebrows
			Ell(Sk, V(10.f, S * 1.35f, 6.6f), V(0.9f, 0.9f, 0.8f), FQuat::Identity, 10, 6);                    // nostril wings
		}
		Ell(Sk, V(8.4f, 0, 12.3f), V(1.8f, 6.0f, 1.3f), FQuat::Identity, 16, 8);                               // brow ridge
		Ell(Sk, V(9.6f, 0, 8.6f), V(1.4f, 1.1f, 2.6f), FQuat(FVector::RightVector, -0.25f), 12, 8);            // nose bridge
		Ell(Sk, V(10.7f, 0, 7.0f), V(1.25f, 1.3f, 1.1f), FQuat::Identity, 12, 7);                               // nose tip
		Ell(P[RM::Lips], V(9.65f, 0, 4.6f), V(0.95f, 2.3f, 0.6f), FQuat::Identity, 14, 7);
		Ell(P[RM::Lips], V(9.45f, 0, 3.8f), V(0.95f, 2.0f, 0.62f), FQuat::Identity, 14, 7);

		// Hair (what shows under the helmet)
		FMRMeshSection& Hr = P[RM::Hair];
		const FVector C = H.SkullC, R = H.SkullR;
		auto Cap = [&](float Grow, float A0, float A1, float E0F, float E0B, float E1)
		{
			Patch(Hr, C, R + FVector(Grow * k), A0, A1, E0F, E0B, E1, 28, 10);
		};
		switch (L.HairStyle)
		{
		case 1: Cap(0.35f, 38.f, 322.f, 5.f, -38.f, 90.f); Cap(0.35f, -38.f, 38.f, 42.f, 42.f, 90.f); break;                     // short
		case 2: Cap(0.9f, 36.f, 324.f, 5.f, -36.f, 90.f); Cap(0.9f, -36.f, 36.f, 40.f, 40.f, 90.f); break;                       // curly
		case 3:                                                                                                                  // long
			Cap(0.5f, 36.f, 324.f, 0.f, -40.f, 90.f); Cap(0.5f, -36.f, 36.f, 42.f, 42.f, 90.f);
			Loft(Hr, FVector::ZeroVector, FVector(0, 0, -1), FVector(0, 1, 0), FVector(1, 0, 0),
				{ { -10.f * k, 6.6f * k, 2.4f * k, 0, -7.8f * k }, { -2.f * k, 7.2f * k, 2.3f * k, 0, -9.0f * k }, { 8.f * k, 7.4f * k, 1.8f * k, 0, -10.6f * k }, { 16.f * k, 6.5f * k, 1.2f * k, 0, -12.2f * k } }, 16, 3);
			break;
		case 4:                                                                                                                  // ponytail
			Cap(0.4f, 38.f, 322.f, 5.f, -38.f, 90.f); Cap(0.4f, -38.f, 38.f, 42.f, 42.f, 90.f);
			MRMeshKit::PathTube(Hr, { V(-9.6f, 0, 12.5f), V(-13.f, 0, 9.f), V(-14.6f, 0, 2.f), V(-14.2f, 0, -6.f) }, 1.9f * k, 10, true);
			MRMeshKit::Torus(P[RM::Accent], V(-10.6f, 0, 11.8f), FVector(-0.7f, 0, 0.7f).GetSafeNormal(), 1.9f * k, 0.5f * k, 16, 6);
			break;
		case 5:                                                                                                                  // braids
			Cap(0.4f, 38.f, 322.f, 5.f, -38.f, 90.f); Cap(0.4f, -38.f, 38.f, 42.f, 42.f, 90.f);
			for (const float S : { -1.f, 1.f })
			{
				MRMeshKit::PathTube(Hr, { V(-5.5f, S * 6.f, 5.f), V(-6.5f, S * 7.2f, -4.f), V(-5.5f, S * 8.f, -13.f) }, 1.15f * k, 8, true);
				Ell(P[RM::Accent], V(-5.5f, S * 8.f, -13.5f), V(1.4f, 1.4f, 1.4f), FQuat::Identity, 10, 6);
			}
			break;
		case 6: Cap(3.4f, 30.f, 330.f, 10.f, -30.f, 90.f); Cap(3.4f, -30.f, 30.f, 45.f, 45.f, 90.f); break;                    // afro
		case 7: Cap(1.6f, 168.f, 192.f, -25.f, -25.f, 90.f); Cap(1.6f, -12.f, 12.f, 40.f, 40.f, 90.f); Cap(0.05f, 38.f, 322.f, 5.f, -38.f, 90.f); break; // mohawk + faded sides
		default: break;                                                                                                          // shaved
		}

		// Facial hair
		if (L.FacialHair > 0)
		{
			const bool bBeard = L.FacialHair == 2;
			Patch(P[RM::Beard], H.JawC, H.JawR + FVector((bBeard ? 0.9f : 0.12f) * k), -110.f, 110.f, -90.f, -90.f, bBeard ? 2.f : 14.f, 24, 10);
			if (bBeard)
			{
				Ell(P[RM::Beard], V(8.9f, 0, 0.8f), V(3.0f, 3.4f, 2.8f), FQuat::Identity, 14, 8);
				Ell(P[RM::Beard], V(10.1f, 0, 5.35f), V(0.85f, 2.7f, 0.6f), FQuat::Identity, 14, 7); // moustache
				for (const float S : { -1.f, 1.f }) Patch(P[RM::Beard], C, R + FVector(0.3f * k), S > 0 ? 62.f : -88.f, S > 0 ? 88.f : -62.f, -40.f, -40.f, 0.f, 6, 6); // sideburns
			}
		}
	}

	void BuildHelmet(FMRRiderPart& P, const FMRRiderLook& L, const FBodyScale& B, const FHeadGeo& H, bool bMoto)
	{
		const float k = B.K;
		auto V = [k](float X, float Y, float Z) { return FVector(X, Y, Z) * k; };
		FMRMeshSection& Sh = P[RM::Helmet];
		FMRMeshSection& In = P[RM::Strap];
		const FVector Hc = H.SkullC;
		auto ChinStraps = [&](const FVector& ShC, const FVector& ShR, float El)
		{
			for (const float S : { -1.f, 1.f })
			{
				FVector Top, N;
				OnEllipsoid(ShC, ShR, S * 82.f, El, Top, N);
				MRMeshKit::PathTube(P[RM::Strap], { Top, V(3.f, S * 6.6f, 2.5f), V(5.f, S * 4.6f, -1.6f), V(5.6f, 0, -2.6f) }, 0.32f * k, 6, true);
			}
		};
		switch (L.HelmetStyle)
		{
		case 1: // full-face: shell with a face port, visor, vents, rear spoiler
		{
			const FVector C = Hc + V(0.6f, 0, -0.6f), R = V(14.2f, 11.8f, 14.8f);
			Shell(Sh, In, C, R, 2.f * k, 58.f, 302.f, -62.f, -62.f, 90.f, 40, 14);
			Shell(Sh, In, C, R, 2.f * k, -58.f, 58.f, 20.f, 20.f, 90.f, 20, 8);
			Shell(Sh, In, C, R, 2.f * k, -58.f, 58.f, -62.f, -62.f, -19.f, 20, 6);
			Patch(P[RM::Visor], C, R * 1.035f, -63.f, 63.f, -22.f, -22.f, 24.f, 22, 8);
			for (const float S : { -1.f, 1.f }) OnSurface(P[RM::Strap], C, R * 1.035f, S * 64.f, 1.f, V(1.6f, 1.6f, 0.6f), 0.f); // visor pivots
			OnSurface(P[RM::Strap], C, R, 0.f, -40.f, V(2.6f, 1.2f, 0.5f), 0.15f);   // chin vent
			OnSurface(P[RM::Strap], C, R, 0.f, 48.f, V(2.2f, 3.0f, 0.5f), 0.15f);    // top vent
			OnSurface(Sh, C, R, 180.f, 20.f, V(3.f, 7.f, 2.2f), 0.4f);              // spoiler
			OnSurface(P[RM::Accent], C, R, 0.f, 70.f, V(5.f, 1.2f, 0.3f), 0.1f);
			break;
		}
		case 2: // motocross: big chin bar, peak, goggles
		{
			const FVector C = Hc + V(0.6f, 0, 0.4f), R = V(14.f, 11.6f, 14.4f);
			Shell(Sh, In, C, R, 2.f * k, 62.f, 298.f, -56.f, -56.f, 90.f, 40, 14);
			Shell(Sh, In, C, R, 2.f * k, -62.f, 62.f, 16.f, 16.f, 90.f, 20, 8);
			Shell(Sh, In, C, R, 2.f * k, -62.f, 62.f, -56.f, -56.f, -26.f, 20, 6);
			Ell(Sh, Hc + V(13.6f, 0, -2.6f), V(5.2f, 6.6f, 4.0f), FQuat(FVector::RightVector, 0.35f), 16, 8); // chin bar snout
			for (const float S : { -1.f, 1.f }) OnSurface(P[RM::Strap], Hc + V(13.6f, 0, -2.6f), V(5.2f, 6.6f, 4.0f), S * 25.f, -5.f, V(1.4f, 0.8f, 0.4f), 0.1f);
			MRMeshKit::Box(P[RM::Accent], Hc + V(13.f, 0, 12.6f), FQuat(FVector::RightVector, FMath::DegreesToRadians(14.f)), V(8.5f, 9.5f, 0.45f)); // peak
			// Goggles: frame, lens and strap round the helmet
			Patch(P[RM::Strap], Hc, V(11.6f, 9.6f, 12.4f), -58.f, 58.f, -9.f, -9.f, 15.f, 18, 4);
			Patch(P[RM::Visor], Hc, V(11.9f, 9.8f, 12.6f), -50.f, 50.f, -6.f, -6.f, 12.f, 16, 4);
			Patch(P[RM::Accent], C, R * 1.012f, 62.f, 298.f, -2.f, -2.f, 6.f, 30, 1);
			break;
		}
		case 3: // skate / BMX: deep round shell, a few round vents
		{
			const FVector C = Hc + V(-0.4f, 0, 1.2f), R = V(12.6f, 10.6f, 12.8f);
			Shell(Sh, In, C, R, 1.8f * k, -180.f, 180.f, 9.f, -34.f, 90.f, 48, 14);
			for (const float A : { -18.f, 18.f }) OnSurface(P[RM::Strap], C, R, A, 42.f, V(1.3f, 1.3f, 0.4f), 0.1f);
			for (const float A : { 150.f, 180.f, 210.f }) OnSurface(P[RM::Strap], C, R, A, 30.f, V(1.2f, 1.2f, 0.4f), 0.1f);
			ChinStraps(C, R * 0.99f, -14.f);
			break;
		}
		case 4: // road aero: elongated, vents, wraparound glasses
		{
			const FVector C = Hc + V(-2.2f, 0, 2.6f), R = V(15.4f, 10.9f, 11.6f);
			Shell(Sh, In, C, R, 1.6f * k, -180.f, 180.f, 6.f, -12.f, 90.f, 48, 14);
			for (int32 i = -2; i <= 2; ++i) OnSurface(P[RM::Strap], C, R, i * 17.f, 24.f, V(3.2f, 0.85f, 0.5f), 0.2f, -90.f);
			for (int32 i = -2; i <= 2; ++i) if (i != 0) OnSurface(P[RM::Strap], C, R, i * 26.f, 56.f, V(3.8f, 0.9f, 0.5f), 0.2f, -90.f);
			for (const float A : { 160.f, 180.f, 200.f }) OnSurface(P[RM::Strap], C, R, A, 22.f, V(2.6f, 0.8f, 0.5f), 0.2f, -90.f);
			OnSurface(P[RM::Accent], C, R, 0.f, 75.f, V(6.f, 1.3f, 0.3f), 0.05f, -90.f);
			Patch(P[RM::Visor], Hc, V(10.9f, 8.7f, 11.6f), -56.f, 56.f, -7.f, -7.f, 9.f, 18, 3); // glasses
			ChinStraps(C, R * 0.99f, -4.f);
			break;
		}
		default: // open-face (3/4) with a flip-down visor strip
		{
			const FVector C = Hc + V(-0.3f, 0, 1.0f), R = V(12.4f, 10.3f, 12.7f);
			Shell(Sh, In, C, R, 1.8f * k, 62.f, 298.f, -50.f, -50.f, 90.f, 40, 14);
			Shell(Sh, In, C, R, 1.8f * k, -62.f, 62.f, 24.f, 24.f, 90.f, 20, 8);
			Patch(P[RM::Visor], C, R * 1.05f, -58.f, 58.f, 2.f, 2.f, 27.f, 20, 5);
			for (const float S : { -1.f, 1.f }) OnSurface(P[RM::Strap], C, R * 1.05f, S * 60.f, 12.f, V(1.4f, 1.4f, 0.6f), 0.f);
			ChinStraps(C, R * 0.99f, -40.f);
			break;
		}
		}
		(void)bMoto;
	}

	// ------------------------------------------------------------------ IK
	FVector TwoBone(const FVector& A, const FVector& Target, float L1, float L2, const FVector& Pole, FVector& OutEnd)
	{
		FVector D = Target - A;
		float Dist = D.Size();
		const FVector Dir = Dist > 0.01f ? D / Dist : FVector(1, 0, 0);
		Dist = FMath::Clamp(Dist, FMath::Abs(L1 - L2) + 0.5f, (L1 + L2) * 0.999f);
		OutEnd = A + Dir * Dist;
		const float Along = (L1 * L1 - L2 * L2 + Dist * Dist) / (2.f * Dist);
		const float Out = FMath::Sqrt(FMath::Max(0.f, L1 * L1 - Along * Along));
		FVector PoleDir = Pole - Dir * FVector::DotProduct(Pole, Dir);
		if (!PoleDir.Normalize()) PoleDir = FVector::CrossProduct(Dir, FVector(0, 1, 0)).GetSafeNormal();
		return A + Dir * Along + PoleDir * Out;
	}

	FTransform Bone(const FVector& From, const FVector& To)
	{
		const FVector X = (To - From).GetSafeNormal();
		return FTransform(FRotationMatrix::MakeFromXY(X, FVector(0, 1, 0)).ToQuat(), From);
	}
}

// =====================================================================================

FMRRiderMesh MRBuildRiderMesh(const FMRRiderLook& Look, bool bMotoGear)
{
	FMRRiderMesh M;
	FMRRiderDims& D = M.D;
	const float H = FMath::Clamp(static_cast<float>(Look.HeightCm), 140.f, 205.f);
	FBodyScale B;
	B.K = H / 178.f;
	B.W = Look.WidthScale();
	B.Dp = 1.f + (B.W - 1.f) * 0.6f;
	B.G = 1.f + (B.W - 1.f) * 0.55f;
	const float k = B.K, G = B.K * B.G;

	// Segment lengths as fractions of standing height (adult anthropometry).
	D.Height = H;
	D.Torso = 0.288f * H;
	D.ShoulderHalf = 0.1f * H * (0.92f + 0.08f * B.W);
	D.HipHalf = 0.05f * H * (0.94f + 0.06f * B.W);
	D.Neck = 0.05f * H;
	D.UpperArm = 0.172f * H;
	D.Forearm = 0.157f * H;
	D.Hand = 0.046f * H;
	D.Thigh = 0.245f * H;
	D.Shin = 0.246f * H;
	D.AnkleH = 0.0425f * H;
	D.FootBall = 0.074f * H;
	B.T = D.Torso;

	const int32 Top = static_cast<int32>(Look.TopStyle);
	const bool bJacket = Top == 0, bHoodie = Top == 1, bJersey = Top == 2, bLeathers = Top == 3, bVest = Top == 4, bTee = Top == 5;
	const RM LegMat = bLeathers ? RM::Top : RM::Legs;

	// ---------------- torso
	FMRRiderPart& To = M.Seg[static_cast<int32>(RS::Torso)];
	TorsoLoft(To[LegMat], B, -0.22f, 0.16f, 0.5f, 7);                       // trousers / seat of the leathers
	TorsoLoft(To[RM::Top], B, 0.04f, 1.10f, bTee || bJersey ? 0.5f : 0.95f, 12, bTee || bJersey ? 0.3f : 1.1f);
	for (const float S : { -1.f, 1.f })
	{
		const FVector Sj(-0.5f * k, S * D.ShoulderHalf, D.Torso);
		Ell(To[RM::Top], Sj + FVector(0, S * 0.4f * k, 0.4f * k), FVector(6.8f, 6.6f, 6.6f) * G, FQuat::Identity, 18, 10); // deltoids
		Ell(To[RM::Top], FVector(-6.f * k, S * 9.f * k * B.W, D.Torso * 0.86f), FVector(5.f * k, 7.5f * k * B.W, 7.f * k), FQuat::Identity, 14, 8); // shoulder blades / traps
		Ell(To[LegMat], FVector(-5.f * k, S * 7.f * k * B.W, -0.1f * D.Torso), FVector(6.2f, 7.f * B.W, 6.f) * k, FQuat::Identity, 16, 9); // seat
	}
	// Belt line
	TorsoLoft(To[RM::Strap], B, 0.035f, 0.065f, 0.75f, 2);

	if (bJacket)
	{
		Loft(To[RM::Top], FVector::ZeroVector, FVector(0, 0, 1), FVector(0, 1, 0), FVector(1, 0, 0),
			{ { D.Torso + 1.5f * k, 8.6f * k, 7.6f * k, 0, -0.8f * k }, { D.Torso + 5.f * k, 7.6f * k, 6.8f * k, 0, -0.4f * k }, { D.Torso + 8.f * k, 7.2f * k, 6.6f * k, 0, 0.2f * k } }, 18, 2); // collar
		const FVector Z0 = TorsoSurface(B, 0.06f, 1.f, 0.95f), Z1 = TorsoSurface(B, 0.92f, 1.f, 0.95f), Z2 = TorsoSurface(B, 0.5f, 1.f, 0.95f);
		MRMeshKit::PathTube(To[RM::Strap], { Z0 + FVector(0.2f, 0, 0), Z2 + FVector(0.2f, 0, 0), Z1 + FVector(0.2f, 0, 0) }, 0.35f * k, 6, true); // zip
		TorsoLoft(To[RM::Accent], B, 0.70f, 0.73f, 1.15f, 2);                                                                                   // chest piping
		for (const float S : { -1.f, 1.f }) Ell(To[RM::Accent], FVector(-0.5f * k, S * (D.ShoulderHalf + 1.2f * k), D.Torso + 2.5f * k), FVector(5.f, 5.f, 3.f) * G, FQuat::Identity, 14, 7); // shoulder armour
	}
	if (bHoodie)
	{
		Ell(To[RM::Top], FVector(-8.5f * k, 0, D.Torso + 3.5f * k), FVector(6.f, 10.f * B.W, 5.5f) * k, FQuat(FVector::RightVector, 0.3f), 16, 9); // hood, bunched behind the neck
		Loft(To[RM::Top], FVector::ZeroVector, FVector(0, 0, 1), FVector(0, 1, 0), FVector(1, 0, 0),
			{ { D.Torso + 1.f * k, 9.f * k, 8.f * k, 0, -1.5f * k }, { D.Torso + 4.f * k, 8.f * k, 7.2f * k, 0, -1.2f * k } }, 18, 2);
		const FVector Pk = TorsoSurface(B, 0.26f, 1.f, 0.95f);
		Ell(To[RM::Top], Pk + FVector(-0.6f * k, 0, 0), FVector(1.6f, 10.f * B.W, 6.5f) * k, FQuat::Identity, 16, 8); // kangaroo pocket
		for (const float S : { -1.f, 1.f })
		{
			const FVector A = TorsoSurface(B, 1.0f, 1.f, 1.f) + FVector(0.5f * k, S * 2.6f * k, 0), Bt = TorsoSurface(B, 0.78f, 1.f, 1.f) + FVector(0.5f * k, S * 3.0f * k, 0);
			MRMeshKit::Tube(To[RM::Eye], A, Bt, 0.3f * k, 0.3f * k, 6, true); // drawstrings
		}
	}
	if (bJersey)
	{
		TorsoLoft(To[RM::Accent], B, 0.58f, 0.76f, 0.75f, 3);   // chest band
		const FVector Z0 = TorsoSurface(B, 0.08f, 1.f, 0.5f), Z1 = TorsoSurface(B, 1.0f, 1.f, 0.5f);
		MRMeshKit::Tube(To[RM::Strap], Z0 + FVector(0.25f, 0, 0), Z1 + FVector(0.25f, 0, 0), 0.25f * k, 0.25f * k, 6, true);
		for (int32 i = -1; i <= 1; ++i)
		{
			const FVector Pk = TorsoSurface(B, 0.16f, -1.f, 0.5f);
			Ell(To[RM::Top], Pk + FVector(-0.3f * k, i * 7.f * k * B.W, 0), FVector(1.4f, 3.2f, 4.6f) * k, FQuat::Identity, 12, 6); // rear pockets
		}
	}
	if (bLeathers)
	{
		Ell(To[RM::Top], TorsoSurface(B, 0.98f, -1.f, 0.f) + FVector(-1.f * k, 0, 2.f * k), FVector(6.5f, 8.f, 13.f) * k, FQuat(FVector::RightVector, -0.2f), 16, 9); // aero hump
		for (const float S : { -1.f, 1.f })
			MRMeshKit::PathTube(To[RM::Accent], { FVector(0, S * (TorsoAt(B, 0.1f).RY + 1.0f), 0.1f * D.Torso), FVector(0, S * (TorsoAt(B, 0.5f).RY + 1.0f), 0.5f * D.Torso), FVector(-1.f * k, S * (TorsoAt(B, 0.9f).RY + 1.f), 0.9f * D.Torso) }, 0.9f * k, 6, true);
		Loft(To[RM::Top], FVector::ZeroVector, FVector(0, 0, 1), FVector(0, 1, 0), FVector(1, 0, 0),
			{ { D.Torso + 1.f * k, 8.2f * k, 7.4f * k, 0, -0.8f * k }, { D.Torso + 4.5f * k, 7.f * k, 6.5f * k, 0, -0.4f * k } }, 18, 2);
	}
	if (bVest)
	{
		TorsoLoft(To[RM::HiVis], B, 0.10f, 0.97f, 1.75f, 8, 0.2f);
		TorsoLoft(To[RM::Reflect], B, 0.30f, 0.36f, 1.95f, 2);
		TorsoLoft(To[RM::Reflect], B, 0.50f, 0.56f, 1.95f, 2);
	}

	// Bags
	switch (Look.Backpack)
	{
	case 1: // backpack
	case 3: // hydration pack
	{
		const bool bHyd = Look.Backpack == 3;
		const float F0 = bHyd ? 0.42f : 0.28f, F1 = 0.9f;
		const FVector Bk0 = TorsoSurface(B, F0, -1.f, 1.f), Bk1 = TorsoSurface(B, F1, -1.f, 1.f);
		const float Dep = (bHyd ? 3.2f : 6.5f) * k, Wd = (bHyd ? 9.f : 13.f) * k;
		const float X0 = static_cast<float>(Bk0.X), Z0 = static_cast<float>(Bk0.Z), X1 = static_cast<float>(Bk1.X), Z1 = static_cast<float>(Bk1.Z);
		Loft(To[RM::Bag], FVector::ZeroVector, FVector(0, 0, 1), FVector(0, 1, 0), FVector(1, 0, 0),
			{ { Z0, Wd * 0.7f, Dep * 0.6f, 0, X0 - Dep * 0.6f }, { Z0 + 2.f * k, Wd, Dep, 0, X0 - Dep }, { Z1 - 3.f * k, Wd, Dep, 0, X1 - Dep * 0.9f }, { Z1, Wd * 0.75f, Dep * 0.55f, 0, X1 - Dep * 0.5f } }, 16, 3);
		if (!bHyd) Ell(To[RM::Accent], FVector(Bk0.X - Dep * 1.9f, 0, FMath::Lerp(Bk0.Z, Bk1.Z, 0.35f)), FVector(1.5f * k, Wd * 0.8f, 6.f * k), FQuat::Identity, 14, 7); // front pocket
		for (const float S : { -1.f, 1.f })
		{
			const FVector Tp(-6.f * k, S * 9.f * k * B.W, D.Torso + 3.5f * k);
			const FVector Fr = TorsoSurface(B, 0.8f, 1.f, 1.2f) + FVector(0, S * 9.5f * k * B.W, 0);
			const FVector Lo = TorsoSurface(B, 0.45f, 1.f, 1.2f) + FVector(0, S * 11.f * k * B.W, 0);
			MRMeshKit::PathTube(To[RM::Strap], { Bk1 + FVector(-0.5f, S * 8.f * k, -2.f * k), Tp, Fr, Lo }, 0.9f * k, 6, true); // shoulder straps
		}
		if (bHyd) MRMeshKit::PathTube(To[RM::Strap], { Bk1 + FVector(-1.f, -5.f * k, 0), FVector(-2.f * k, -12.f * k, D.Torso + 4.f * k), TorsoSurface(B, 0.8f, 1.f, 2.f) + FVector(0, -8.f * k, 0) }, 0.6f * k, 6, true); // drinking hose
		break;
	}
	case 2: // courier bag on the hip, strap across the chest
	{
		const FVector Hip = TorsoSurface(B, 0.18f, -1.f, 1.f) + FVector(1.f * k, 12.f * k * B.W, 0);
		MRMeshKit::Box(To[RM::Bag], Hip, FQuat::Identity, FVector(6.f, 4.f, 13.f) * k);
		MRMeshKit::Box(To[RM::Bag], Hip + FVector(-0.4f * k, 0, 9.f * k), FQuat(FVector::RightVector, 0.15f), FVector(6.6f, 4.5f, 4.5f) * k); // flap
		MRMeshKit::PathTube(To[RM::Strap], { Hip + FVector(0, 0, 12.f * k), TorsoSurface(B, 0.6f, 1.f, 2.4f) + FVector(0, 4.f * k, 0), FVector(-2.f * k, -11.f * k, D.Torso + 3.5f * k),
			TorsoSurface(B, 0.8f, -1.f, 2.4f) + FVector(0, -4.f * k, 0), Hip + FVector(-3.f * k, 0, 12.f * k) }, 0.9f * k, 6, true);
		break;
	}
	default: break;
	}

	// ---------------- neck (frame: Z up the neck)
	FMRRiderPart& Nk = M.Seg[static_cast<int32>(RS::Neck)];
	Loft(Nk[RM::Skin], FVector::ZeroVector, FVector(0, 0, 1), FVector(0, 1, 0), FVector(1, 0, 0),
		{ { -4.f * k, 6.f * G, 5.6f * G, 0, -0.6f * k }, { 0.f, 5.5f * G, 5.1f * G, 0, 0 }, { D.Neck * 0.6f, 5.0f * G, 4.8f * G, 0, 0.3f * k }, { D.Neck + 2.f * k, 4.8f * G, 4.6f * G, 0, 0.8f * k }, { D.Neck + 4.f * k, 4.2f * G, 4.2f * G, 0, 1.f * k } }, 18, 3);
	Ell(Nk[RM::Skin], FVector(2.8f * k, 0, D.Neck * 0.55f), FVector(1.1f, 1.3f, 1.6f) * k, FQuat::Identity, 10, 6); // Adam's apple / throat

	// ---------------- head (frame: origin at the top of the neck, X where the face looks, Z up)
	FMRRiderPart& Hd = M.Seg[static_cast<int32>(RS::Head)];
	FHeadGeo HG;
	HG.SkullC = FVector(0.5f, 0, 10.f) * k;
	HG.SkullR = FVector(9.6f, 7.7f, 11.4f) * k;
	HG.JawC = FVector(4.5f, 0, 3.f) * k;
	HG.JawR = FVector(5.8f, 5.9f, 4.6f) * k;
	BuildHead(Hd, Look, B, HG);
	BuildHelmet(Hd, Look, B, HG, bMotoGear);

	// ---------------- arms
	for (int32 s = 0; s < 2; ++s)
	{
		const float Side = s == 0 ? -1.f : 1.f;
		FMRRiderPart& Ua = M.Seg[static_cast<int32>(s == 0 ? RS::UpperArmL : RS::UpperArmR)];
		FMRRiderPart& Fa = M.Seg[static_cast<int32>(s == 0 ? RS::ForearmL : RS::ForearmR)];
		FMRRiderPart& Hn = M.Seg[static_cast<int32>(s == 0 ? RS::HandL : RS::HandR)];
		const float UA = D.UpperArm, FA = D.Forearm;
		const TArray<FSec> UpperSkin = { { -3.f * k, 3.0f * G, 3.0f * G, 0, 0 }, { 0.f, 5.4f * G, 5.4f * G, 0, 0 }, { 0.3f * UA, 4.6f * G, 5.0f * G, 0, 0.35f * G },
			{ 0.65f * UA, 4.1f * G, 4.4f * G, 0, 0.2f * G }, { 0.93f * UA, 3.9f * G, 3.8f * G, 0, 0 }, { UA + 1.5f * k, 3.4f * G, 3.4f * G, 0, 0 }, { UA + 3.f * k, 1.5f * G, 1.5f * G, 0, 0 } };
		const TArray<FSec> ForeSkin = { { -2.5f * k, 2.8f * G, 2.8f * G, 0, 0 }, { 0.f, 4.0f * G, 4.0f * G, 0, 0 }, { 0.25f * FA, 4.4f * G, 4.0f * G, 0, 0.2f * G },
			{ 0.65f * FA, 3.4f * G, 2.9f * G, 0, 0 }, { FA, 2.8f * G, 2.2f * G, 0, 0 }, { FA + 1.f * k, 2.2f * G, 1.8f * G, 0, 0 } };
		if (bTee)
		{
			LimbLoft(Ua[RM::Skin], UpperSkin);
			LimbLoft(Fa[RM::Skin], ForeSkin);
			TArray<FSec> Sleeve = Grown(UpperSkin, 0.7f * k, -10.f, 0.3f * UA);
			Sleeve.Add({ 0.45f * UA, 5.3f * G, 5.6f * G, 0, 0.3f * G });
			LimbLoft(Ua[RM::Top], Sleeve);
		}
		else
		{
			const float Cl = bLeathers || bJersey ? 0.45f * k : 0.9f * k;
			LimbLoft(Ua[RM::Top], Grown(UpperSkin, Cl));
			TArray<FSec> Fs = Grown(ForeSkin, Cl);
			if (bJacket || bHoodie) Fs[Fs.Num() - 2].RY += 0.5f * k, Fs[Fs.Num() - 2].RZ += 0.6f * k; // loose cuff
			LimbLoft(Fa[RM::Top], Fs);
			if (bJacket || bLeathers) Ell(Fa[bLeathers ? RM::Strap : RM::Accent], FVector(-0.5f * k, 0, -2.2f * G), FVector(4.f, 3.6f, 1.5f) * G, FQuat::Identity, 14, 7); // elbow armour / slider
			if (bJacket) LimbLoft(Fa[RM::Accent], Grown(ForeSkin, Cl + 0.25f * k, FA - 4.f * k, FA));                            // cuff trim
			if (bJersey) LimbLoft(Ua[RM::Accent], Grown(UpperSkin, 0.7f * k, 0.55f * UA, 0.93f * UA));                            // sleeve band
		}
		BuildHand(Hn, D, B, Side, bMotoGear);
	}

	// ---------------- legs
	for (int32 s = 0; s < 2; ++s)
	{
		const float Side = s == 0 ? -1.f : 1.f;
		FMRRiderPart& Th = M.Seg[static_cast<int32>(s == 0 ? RS::ThighL : RS::ThighR)];
		FMRRiderPart& Sn = M.Seg[static_cast<int32>(s == 0 ? RS::ShinL : RS::ShinR)];
		FMRRiderPart& Ft = M.Seg[static_cast<int32>(s == 0 ? RS::FootL : RS::FootR)];
		const float TL = D.Thigh, SL = D.Shin;
		const float Cl = bLeathers ? 0.5f * k : 0.9f * k;
		// Thigh: Z is the front of the leg (quads), the hip joint sits inside the pelvis.
		LimbLoft(Th[LegMat], { { -6.f * k, 6.f * G, 6.f * G, 0, 0 }, { -2.f * k, 7.6f * G + Cl, 7.8f * G + Cl, 0, -0.5f * G }, { 0.25f * TL, 7.5f * G + Cl, 7.9f * G + Cl, 0, 0.5f * G },
			{ 0.6f * TL, 6.7f * G + Cl, 6.9f * G + Cl, 0, 0.4f * G }, { 0.9f * TL, 5.2f * G + Cl, 5.3f * G + Cl, 0, 0.1f * G }, { TL, 5.0f * G + Cl, 5.5f * G + Cl, 0, 0.5f * G }, { TL + 3.f * k, 3.2f * G, 3.4f * G, 0, 0.3f * G } }, 18);
		if (bLeathers)
		{
			Ell(Th[RM::Strap], FVector(TL - 1.f * k, Side * (5.2f * G + Cl), 0.5f * k), FVector(3.6f, 1.2f, 3.0f) * k, FQuat::Identity, 14, 7); // knee slider
			MRMeshKit::PathTube(Th[RM::Accent], { FVector(0.1f * TL, Side * (8.4f * G + Cl), 0), FVector(0.8f * TL, Side * (6.2f * G + Cl), 0) }, 0.8f * k, 6, true);
		}
		else if (!bJersey && !bVest) Ell(Th[RM::Strap], FVector(0.55f * TL, Side * (7.0f * G + Cl), 0.f), FVector(6.f, 0.6f, 0.4f) * k, FQuat::Identity, 10, 5); // side seam
		// Shin: calf behind (-Z)
		LimbLoft(Sn[LegMat], { { -3.f * k, 4.0f * G, 4.0f * G, 0, 0 }, { 0.f, 5.0f * G + Cl, 5.2f * G + Cl, 0, 0 }, { 0.28f * SL, 5.0f * G + Cl, 5.7f * G + Cl, 0, -1.1f * G },
			{ 0.62f * SL, 4.1f * G + Cl, 4.3f * G + Cl, 0, -0.4f * G }, { 0.9f * SL, 3.4f * G + Cl, 3.4f * G + Cl, 0, 0 }, { SL + 1.f * k, 3.4f * G + Cl, 3.6f * G + Cl, 0, 0 }, { SL + 2.5f * k, 2.f * G, 2.f * G, 0, 0 } }, 18);
		if (bMotoGear)
		{
			// Boot shaft over the lower shin, with buckles and a shin plate
			LimbLoft(Sn[RM::Shoes], { { 0.52f * SL, 4.9f * G + Cl, 5.1f * G + Cl, 0, -0.4f * G }, { 0.6f * SL, 5.2f * G + Cl, 5.4f * G + Cl, 0, -0.4f * G }, { SL - 2.f * k, 4.6f * G + Cl, 4.7f * G + Cl, 0, 0 }, { SL + 2.f * k, 4.6f * G + Cl, 4.7f * G + Cl, 0, 0 } }, 18);
			for (const float F : { 0.68f, 0.82f }) LimbLoft(Sn[RM::Reflect], { { F * SL, 5.3f * G + Cl, 5.5f * G + Cl, 0, 0 }, { F * SL + 1.4f * k, 5.25f * G + Cl, 5.45f * G + Cl, 0, 0 } }, 18);
			Ell(Sn[RM::Accent], FVector(0.75f * SL, 0, 4.6f * G + Cl), FVector(9.f, 3.5f, 1.4f) * k, FQuat::Identity, 14, 7);
		}
		BuildFoot(Ft, D, B, bMotoGear);
	}
	return M;
}

// =====================================================================================

void MRSolveRiderPose(const FMRRiderDims& D, const FMRRiderTargets& T, FMRRiderPose& Out)
{
	const FVector Up(0, 0, 1), Y0(0, 1, 0);
	const FVector Hip = T.Hip;
	// Torso roll about the hips uses the same sense as the bike's lean (FRotator roll, + = right).
	const FQuat RollQ = FRotator(0.f, 0.f, FMath::RadiansToDegrees(T.BodyRoll)).Quaternion();
	const FVector Y = RollQ.RotateVector(Y0);
	auto Spine = [&RollQ](float Th) { return RollQ.RotateVector(FVector(FMath::Sin(Th), 0.f, FMath::Cos(Th))); };
	const FVector HintDir = (T.HeadHint - Hip).GetSafeNormal();
	const float Th0 = FMath::Clamp(FMath::Atan2(HintDir.X, FMath::Max(0.05f, HintDir.Z)) + T.TorsoBias, -0.3f, 1.3f);

	// Wrist sits behind and above the grip; the hand comes down onto the bar.
	const FVector HandBack = FVector(-0.8f, 0.f, 0.6f).GetSafeNormal() * D.Hand;
	const FVector Wrist[2] = { T.Grip[0] + HandBack, T.Grip[1] + HandBack };
	const float Reach = D.UpperArm + D.Forearm, Ideal = 0.9f * Reach;

	// Lean the torso from the hips until the hands reach the grips with slightly bent elbows.
	float BestTh = Th0, BestCost = 1e30f;
	for (float Th = Th0 - 0.4f; Th <= 1.35f; Th += 0.015f)
	{
		const FVector Sc = Hip + Spine(Th) * D.Torso;
		float Cost = 0.f;
		for (int32 s = 0; s < 2; ++s)
		{
			const FVector Sj = Sc + Y * ((s == 0 ? -1.f : 1.f) * D.ShoulderHalf);
			const float Dd = FVector::Dist(Sj, Wrist[s]) - Ideal;
			Cost += Dd * Dd * (Dd > 0.f ? 3.f : 0.15f); // over-reaching is far worse than bent elbows
		}
		Cost += 600.f * FMath::Square(Th - Th0); // stay close to the vehicle's riding position
		if (Cost < BestCost) { BestCost = Cost; BestTh = Th; }
	}
	const FVector Sp = Spine(BestTh);
	const FVector Fwd = RollQ.RotateVector(FVector(FMath::Cos(BestTh), 0.f, -FMath::Sin(BestTh)));
	const FVector Sc = Hip + Sp * D.Torso;
	Out.Seg[static_cast<int32>(EMRRiderSeg::Torso)] = FTransform(FRotationMatrix::MakeFromZX(Sp, Fwd).ToQuat(), Hip);

	// Neck bends up from the shoulders, head comes back towards level so the rider looks ahead.
	const FVector NeckBase = Sc + Sp * (0.012f * D.Height) + Fwd * (-0.008f * D.Height);
	const FVector NeckDir = (Sp * 0.45f + RollQ.RotateVector(Up) * 0.55f + FVector(0.12f, 0, 0)).GetSafeNormal();
	Out.Seg[static_cast<int32>(EMRRiderSeg::Neck)] = FTransform(FRotationMatrix::MakeFromZX(NeckDir, FVector(1, 0, 0)).ToQuat(), NeckBase);
	const FVector HeadO = NeckBase + NeckDir * D.Neck;
	const float HeadPitch = FMath::Clamp(BestTh * 0.22f + T.HeadPitch, -0.5f, 0.6f);
	const FQuat HeadRollQ = FRotator(0.f, 0.f, FMath::RadiansToDegrees(T.BodyRoll + T.HeadRoll)).Quaternion();
	const FQuat HeadQ = HeadRollQ * FQuat(Up, T.HeadYaw) * FQuat(Y0, HeadPitch);
	Out.Seg[static_cast<int32>(EMRRiderSeg::Head)] = FTransform(HeadQ, HeadO);
	const float Sk = D.Height / 178.f;
	Out.Eye = HeadO + HeadQ.RotateVector(FVector(9.8f, 0.f, 10.f) * Sk);

	auto J = [&Out](EMRRiderJoint Jt) -> FVector& { return Out.Joint[static_cast<int32>(Jt)]; };
	J(EMRRiderJoint::Pelvis) = Hip;
	J(EMRRiderJoint::Chest) = Sc;
	J(EMRRiderJoint::Neck) = NeckBase;
	J(EMRRiderJoint::Head) = HeadO;
	J(EMRRiderJoint::HeadTop) = HeadO + HeadQ.RotateVector(FVector(0.f, 0.f, 16.f) * Sk);
	J(EMRRiderJoint::HeadFwd) = HeadO + HeadQ.RotateVector(FVector(11.f, 0.f, 6.f) * Sk);

	// Arms
	for (int32 s = 0; s < 2; ++s)
	{
		const float Side = s == 0 ? -1.f : 1.f;
		const FVector Sj = Sc + Y * (Side * D.ShoulderHalf);
		FVector WristAt;
		const FVector Elbow = TwoBone(Sj, Wrist[s], D.UpperArm, D.Forearm, RollQ.RotateVector(FVector(-0.3f, Side * 0.75f, -0.6f)), WristAt);
		Out.Seg[static_cast<int32>(s == 0 ? EMRRiderSeg::UpperArmL : EMRRiderSeg::UpperArmR)] = Bone(Sj, Elbow);
		Out.Seg[static_cast<int32>(s == 0 ? EMRRiderSeg::ForearmL : EMRRiderSeg::ForearmR)] = Bone(Elbow, WristAt);
		Out.Seg[static_cast<int32>(s == 0 ? EMRRiderSeg::HandL : EMRRiderSeg::HandR)] = Bone(WristAt, WristAt + (T.Grip[s] - Wrist[s]));
		J(s == 0 ? EMRRiderJoint::ShL : EMRRiderJoint::ShR) = Sj;
		J(s == 0 ? EMRRiderJoint::ElbowL : EMRRiderJoint::ElbowR) = Elbow;
		J(s == 0 ? EMRRiderJoint::WristL : EMRRiderJoint::WristR) = WristAt;
		J(s == 0 ? EMRRiderJoint::HandL : EMRRiderJoint::HandR) = WristAt + (T.Grip[s] - Wrist[s]);
	}

	// Legs: ball of the foot on the target; point the toes down further if the leg can't reach (ankling).
	const float LegMax = (D.Thigh + D.Shin) * 0.985f;
	for (int32 s = 0; s < 2; ++s)
	{
		const float Side = s == 0 ? -1.f : 1.f;
		const FVector Hj = Hip + Y0 * (Side * D.HipHalf);
		const FVector BallLocal(D.FootBall, 0.f, -D.AnkleH);
		float Pitch = T.FootPitch[s];
		FVector Ankle;
		for (int32 i = 0; i < 40; ++i)
		{
			Ankle = T.FootBall[s] - FQuat(Y0, Pitch).RotateVector(BallLocal);
			if (FVector::Dist(Ankle, Hj) <= LegMax || Pitch > 1.0f) break;
			Pitch += 0.03f;
		}
		FVector AnkleAt;
		const FVector Knee = TwoBone(Hj, Ankle, D.Thigh, D.Shin, FVector(1.f, Side * (T.KneeOut + T.KneeOutExtra[s]), 0.2f), AnkleAt);
		Out.Seg[static_cast<int32>(s == 0 ? EMRRiderSeg::ThighL : EMRRiderSeg::ThighR)] = Bone(Hj, Knee);
		Out.Seg[static_cast<int32>(s == 0 ? EMRRiderSeg::ShinL : EMRRiderSeg::ShinR)] = Bone(Knee, AnkleAt);
		Out.Seg[static_cast<int32>(s == 0 ? EMRRiderSeg::FootL : EMRRiderSeg::FootR)] = FTransform(FQuat(Y0, Pitch), AnkleAt);
		J(s == 0 ? EMRRiderJoint::HipL : EMRRiderJoint::HipR) = Hj;
		J(s == 0 ? EMRRiderJoint::KneeL : EMRRiderJoint::KneeR) = Knee;
		J(s == 0 ? EMRRiderJoint::AnkleL : EMRRiderJoint::AnkleR) = AnkleAt;
		J(s == 0 ? EMRRiderJoint::ToeL : EMRRiderJoint::ToeR) = AnkleAt + FQuat(Y0, Pitch).RotateVector(BallLocal);
	}
}

// =====================================================================================
// Crash ragdoll

namespace
{
	using RJ = EMRRiderJoint;
	constexpr uint8 J8(RJ X) { return static_cast<uint8>(X); }
	FQuat Basis(const FVector& A, const FVector& B) { return FRotationMatrix::MakeFromXZ(A, B).ToQuat(); }
}

void FMRRiderRagdoll::Init(const FMRRiderPose& W, const FMRRiderDims& D, const FVector& Vel, const FVector& Spin)
{
	const float H = D.Height;
	for (int32 i = 0; i < NJ; ++i) P[i] = W.Joint[i];
	const float R[NJ] = { 0.06f, 0.05f, 0.05f, 0.065f, 0.045f, 0.045f, 0.035f, 0.06f, 0.025f, 0.025f,
		0.025f, 0.025f, 0.02f, 0.02f, 0.015f, 0.015f, 0.03f, 0.03f, 0.025f, 0.025f, 0.015f, 0.015f };
	for (int32 i = 0; i < NJ; ++i) Radius[i] = R[i] * H;

	Links.Reset();
	auto Link = [this](RJ A, RJ B, float Scale = 1.f, bool bMin = false)
	{
		Links.Add({ J8(A), J8(B), static_cast<float>(FVector::Dist(P[J8(A)], P[J8(B)])) * Scale, bMin });
	};
	auto Rigid = [&Link](std::initializer_list<RJ> Set)
	{
		const RJ* B = Set.begin();
		for (size_t i = 0; i < Set.size(); ++i) for (size_t k = i + 1; k < Set.size(); ++k) Link(B[i], B[k]);
	};
	// Torso and shoulders move as one block; the head is a rigid block hinged at the neck.
	Rigid({ RJ::Pelvis, RJ::HipL, RJ::HipR, RJ::Chest, RJ::ShL, RJ::ShR, RJ::Neck });
	Rigid({ RJ::Neck, RJ::Head, RJ::HeadTop, RJ::HeadFwd });
	Link(RJ::Head, RJ::Chest, 0.92f, true); // the neck can't fold the head into the chest
	Link(RJ::HeadFwd, RJ::Chest, 0.85f, true);
	for (int32 s = 0; s < 2; ++s)
	{
		const bool L = s == 0;
		const RJ Sh = L ? RJ::ShL : RJ::ShR, El = L ? RJ::ElbowL : RJ::ElbowR, Wr = L ? RJ::WristL : RJ::WristR, Hd = L ? RJ::HandL : RJ::HandR;
		const RJ Hp = L ? RJ::HipL : RJ::HipR, Kn = L ? RJ::KneeL : RJ::KneeR, An = L ? RJ::AnkleL : RJ::AnkleR, To = L ? RJ::ToeL : RJ::ToeR;
		Link(Sh, El); Link(El, Wr); Rigid({ Wr, Hd, El });
		Link(Hp, Kn); Link(Kn, An); Rigid({ An, To, Kn });
		// Joint limits, roughly: elbows and knees can't fold flat, legs can't fold into the chest.
		Links.Add({ J8(Sh), J8(Wr), 0.42f * (D.UpperArm + D.Forearm), true });
		Links.Add({ J8(Hp), J8(An), 0.5f * (D.Thigh + D.Shin), true });
		Links.Add({ J8(RJ::Chest), J8(Kn), 0.75f * D.Torso, true });
		Links.Add({ J8(RJ::Pelvis), J8(An), 0.45f * (D.Thigh + D.Shin), true });
	}

	// Each segment: origin joint plus two joints that fix its rotation.
	auto Ref = [this, &W](RS S, RJ O, RJ A, RJ B)
	{
		FSegRef& F = Seg[static_cast<int32>(S)];
		F.O = J8(O); F.A = J8(A); F.B = J8(B);
		const FQuat Q = W.Seg[static_cast<int32>(S)].GetRotation();
		const FQuat L = Basis(Q.UnrotateVector(P[F.A] - P[F.O]), Q.UnrotateVector(P[F.B] - P[F.O]));
		F.LocalBasisInv = L.Inverse();
	};
	Ref(RS::Torso, RJ::Pelvis, RJ::Chest, RJ::HipR);
	Ref(RS::Neck, RJ::Neck, RJ::Head, RJ::ShR);
	Ref(RS::Head, RJ::Head, RJ::HeadTop, RJ::HeadFwd);
	for (int32 s = 0; s < 2; ++s)
	{
		const bool L = s == 0;
		Ref(L ? RS::UpperArmL : RS::UpperArmR, L ? RJ::ShL : RJ::ShR, L ? RJ::ElbowL : RJ::ElbowR, RJ::Chest);
		Ref(L ? RS::ForearmL : RS::ForearmR, L ? RJ::ElbowL : RJ::ElbowR, L ? RJ::WristL : RJ::WristR, L ? RJ::HandL : RJ::HandR);
		Ref(L ? RS::HandL : RS::HandR, L ? RJ::WristL : RJ::WristR, L ? RJ::HandL : RJ::HandR, L ? RJ::ElbowL : RJ::ElbowR);
		Ref(L ? RS::ThighL : RS::ThighR, L ? RJ::HipL : RJ::HipR, L ? RJ::KneeL : RJ::KneeR, L ? RJ::HipR : RJ::HipL);
		Ref(L ? RS::ShinL : RS::ShinR, L ? RJ::KneeL : RJ::KneeR, L ? RJ::AnkleL : RJ::AnkleR, L ? RJ::ToeL : RJ::ToeR);
		Ref(L ? RS::FootL : RS::FootR, L ? RJ::AnkleL : RJ::AnkleR, L ? RJ::ToeL : RJ::ToeR, L ? RJ::KneeL : RJ::KneeR);
	}
	const FTransform& HeadXf = W.Seg[static_cast<int32>(RS::Head)];
	EyeLocal = HeadXf.InverseTransformPositionNoScale(W.Eye);

	// Initial velocity: the body carries on at the vehicle's speed, plus any tumble.
	constexpr float Hs = 1.f / 120.f;
	const FVector C = P[J8(RJ::Pelvis)];
	for (int32 i = 0; i < NJ; ++i) Prev[i] = P[i] - (Vel + FVector::CrossProduct(Spin, P[i] - C)) * Hs;
	Accum = 0.f;
	Time = 0.f;
}

void FMRRiderRagdoll::Step(float Dt, const float* GroundZ)
{
	constexpr float Hs = 1.f / 120.f;
	constexpr float Gravity = -980.f;
	constexpr float SlideDecel = 0.55f * 980.f; // sliding friction on tarmac, cm/s^2
	Accum += FMath::Min(Dt, 0.1f);
	Time += FMath::Min(Dt, 0.1f);
	while (Accum >= Hs)
	{
		Accum -= Hs;
		for (int32 i = 0; i < NJ; ++i)
		{
			const FVector V = (P[i] - Prev[i]) * 0.999f;
			Prev[i] = P[i];
			P[i] += V + FVector(0.f, 0.f, Gravity * Hs * Hs);
		}
		for (int32 It = 0; It < 8; ++It)
		{
			for (const FLink& L : Links)
			{
				const FVector Dv = P[L.B] - P[L.A];
				const float Len = Dv.Size();
				if (Len < 1e-3f || (L.bMinOnly && Len >= L.Len)) continue;
				const FVector Corr = Dv * (0.5f * (Len - L.Len) / Len);
				P[L.A] += Corr;
				P[L.B] -= Corr;
			}
			for (int32 i = 0; i < NJ; ++i) P[i].Z = FMath::Max(P[i].Z, GroundZ[i] + Radius[i]);
		}
		// Ground contact: small bounce, and sliding friction that brings the body to a stop.
		for (int32 i = 0; i < NJ; ++i)
		{
			const float G = GroundZ[i] + Radius[i];
			if (P[i].Z > G + 0.5f) continue;
			const float Vz = P[i].Z - Prev[i].Z;
			if (Vz < 0.f) Prev[i].Z = P[i].Z + Vz * 0.15f;
			const FVector M(P[i].X - Prev[i].X, P[i].Y - Prev[i].Y, 0.f);
			const float Mag = M.Size(), Cut = SlideDecel * Hs * Hs;
			if (Mag <= Cut) { Prev[i].X = P[i].X; Prev[i].Y = P[i].Y; }
			else { const FVector Keep = M * ((Mag - Cut) / Mag); Prev[i].X = P[i].X - Keep.X; Prev[i].Y = P[i].Y - Keep.Y; }
		}
	}
}

void FMRRiderRagdoll::GetPose(FMRRiderPose& Out) const
{
	for (int32 s = 0; s < NS; ++s)
	{
		const FSegRef& F = Seg[s];
		const FQuat Wq = Basis(P[F.A] - P[F.O], P[F.B] - P[F.O]);
		Out.Seg[s] = FTransform((Wq * F.LocalBasisInv).GetNormalized(), P[F.O]);
	}
	for (int32 i = 0; i < NJ; ++i) Out.Joint[i] = P[i];
	Out.Eye = Out.Seg[static_cast<int32>(RS::Head)].TransformPositionNoScale(EyeLocal);
}
