#include "World/MRRouter.h"

#include "ManchesterRide.h"
#include "World/MRGeoWorld.h"

#include "Algo/Reverse.h"
#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	FCriticalSection GRouteLock;
	TSharedPtr<const FMRRouteGraph> GRoutes;
	bool GRoutesRequested = false;

	float SegDist(const FVector2f& A, const FVector2f& B, const FVector2f& P, float& OutT)
	{
		const FVector2f D = B - A;
		const float L2 = D.SizeSquared();
		OutT = L2 > 1e-6f ? FMath::Clamp(FVector2f::DotProduct(P - A, D) / L2, 0.f, 1.f) : 0.f;
		return FVector2f::Distance(A + D * OutT, P);
	}
}

TSharedPtr<const FMRRouteGraph> FMRRouteGraph::Get()
{
	FScopeLock Lock(&GRouteLock);
	if (!GRoutesRequested)
	{
		GRoutesRequested = true;
		const FString Path = FPaths::Combine(AMRGeoWorld::GeoDataDir(), TEXT("routes.bin"));
		Async(EAsyncExecution::ThreadPool, [Path]()
		{
			TSharedPtr<FMRRouteGraph> G = MakeShared<FMRRouteGraph>();
			const double T0 = FPlatformTime::Seconds();
			if (G->Load(Path))
			{
				UE_LOG(LogManchesterRide, Log, TEXT("Road network: %d junctions, %d roads (%.1f s)"), G->Nodes.Num(), G->Edges.Num(), FPlatformTime::Seconds() - T0);
				FScopeLock L(&GRouteLock);
				GRoutes = G;
			}
			else
			{
				UE_LOG(LogManchesterRide, Warning, TEXT("Road network missing (%s): run Tools/geo_pipeline/make_routes.mjs"), *Path);
			}
		});
	}
	return GRoutes;
}

bool FMRRouteGraph::Load(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 20) return false;
	int64 O = 0;
	auto U32 = [&]() -> uint32 { uint32 V = 0; if (O + 4 <= Bytes.Num()) FMemory::Memcpy(&V, Bytes.GetData() + O, 4); O += 4; return V; };
	auto I32 = [&]() -> int32 { return static_cast<int32>(U32()); };
	auto F32 = [&]() -> float { const uint32 V = U32(); float F; FMemory::Memcpy(&F, &V, 4); return F; };
	if (FMemory::Memcmp(Bytes.GetData(), "MRRT", 4) != 0) return false;
	O = 4;
	if (U32() != 1) return false;

	const uint32 NN = U32();
	if (O + int64(NN) * 8 > Bytes.Num()) return false;
	Nodes.SetNumUninitialized(NN);
	for (uint32 i = 0; i < NN; ++i) { const float X = static_cast<float>(I32()); Nodes[i] = FVector2f(X, static_cast<float>(I32())); }
	const uint32 NP = U32();
	if (O + int64(NP) * 8 > Bytes.Num()) return false;
	Points.SetNumUninitialized(NP);
	for (uint32 i = 0; i < NP; ++i) { const float X = static_cast<float>(I32()); Points[i] = FVector2f(X, static_cast<float>(I32())); }
	const uint32 NE = U32();
	if (O + int64(NE) * 28 > Bytes.Num()) return false;
	Edges.SetNumUninitialized(NE);
	for (uint32 i = 0; i < NE; ++i)
	{
		FEdge& E = Edges[i];
		E.A = I32(); E.B = I32(); E.First = I32(); E.Num = I32(); E.Len = F32(); E.Cost = F32(); E.Flags = U32();
		if (E.A < 0 || E.B < 0 || E.A >= int32(NN) || E.B >= int32(NN) || E.First < 0 || E.Num < 2 || E.First + E.Num > int32(NP)) return false;
	}

	// Adjacency (CSR)
	AdjStart.Init(0, NN + 1);
	for (const FEdge& E : Edges) { ++AdjStart[E.A + 1]; if (!(E.Flags & 1)) ++AdjStart[E.B + 1]; }
	for (uint32 i = 0; i < NN; ++i) AdjStart[i + 1] += AdjStart[i];
	Adj.SetNumUninitialized(AdjStart[NN]);
	TArray<int32> Fill = AdjStart;
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		Adj[Fill[Edges[e].A]++] = e * 2 + 1;
		if (!(Edges[e].Flags & 1)) Adj[Fill[Edges[e].B]++] = e * 2;
	}

	// Main network: the largest connected piece (private estates and car parks that never join a
	// public road are left out of snapping, so a waypoint never lands somewhere unreachable).
	TArray<int32> Parent; Parent.SetNumUninitialized(NN);
	for (uint32 i = 0; i < NN; ++i) Parent[i] = i;
	auto Find = [&](int32 X) { while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; } return X; };
	for (const FEdge& E : Edges) { const int32 Ra = Find(E.A), Rb = Find(E.B); if (Ra != Rb) Parent[Ra] = Rb; }
	TMap<int32, int32> Sizes;
	int32 Main = 0, MainSize = 0;
	for (uint32 i = 0; i < NN; ++i) { const int32 R = Find(i); const int32 S = ++Sizes.FindOrAdd(R); if (S > MainSize) { MainSize = S; Main = R; } }

	// Snapping grid
	FBox2f Box(ForceInit);
	for (const FVector2f& P : Nodes) Box += P;
	GridMin = Box.Min;
	GridW = FMath::Max(1, FMath::CeilToInt((Box.Max.X - Box.Min.X) / BucketCm) + 1);
	GridH = FMath::Max(1, FMath::CeilToInt((Box.Max.Y - Box.Min.Y) / BucketCm) + 1);
	Buckets.SetNum(GridW * GridH);
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		const FEdge& E = Edges[e];
		if (Find(E.A) != Main) continue;
		TSet<int32> Seen;
		for (int32 i = 0; i < E.Num; ++i)
		{
			const FVector2f& P = Points[E.First + i];
			const int32 X = FMath::Clamp(FMath::FloorToInt((P.X - GridMin.X) / BucketCm), 0, GridW - 1);
			const int32 Y = FMath::Clamp(FMath::FloorToInt((P.Y - GridMin.Y) / BucketCm), 0, GridH - 1);
			const int32 B = Y * GridW + X;
			if (!Seen.Contains(B)) { Seen.Add(B); Buckets[B].Add(e); }
		}
	}
	return Nodes.Num() > 0 && Edges.Num() > 0;
}

bool FMRRouteGraph::Snap(const FVector2D& P, float MaxCm, FSnap& Out) const
{
	if (GridW == 0) return false;
	const FVector2f Pf(P);
	const int32 CX = FMath::FloorToInt((Pf.X - GridMin.X) / BucketCm), CY = FMath::FloorToInt((Pf.Y - GridMin.Y) / BucketCm);
	const int32 R = FMath::CeilToInt(MaxCm / BucketCm);
	float Best = MaxCm;
	bool bFound = false;
	TSet<int32> Tried;
	for (int32 Y = CY - R; Y <= CY + R; ++Y)
	{
		if (Y < 0 || Y >= GridH) continue;
		for (int32 X = CX - R; X <= CX + R; ++X)
		{
			if (X < 0 || X >= GridW) continue;
			for (const int32 e : Buckets[Y * GridW + X])
			{
				if (Tried.Contains(e)) continue;
				Tried.Add(e);
				const FEdge& E = Edges[e];
				float Along = 0.f;
				for (int32 i = 0; i + 1 < E.Num; ++i)
				{
					const FVector2f& A = Points[E.First + i];
					const FVector2f& B = Points[E.First + i + 1];
					float T;
					const float D = SegDist(A, B, Pf, T);
					if (D < Best)
					{
						Best = D; bFound = true;
						Out.Edge = e; Out.Seg = i; Out.T = T; Out.P = A + (B - A) * T;
						Out.Along = Along + FVector2f::Distance(A, B) * T;
					}
					Along += FVector2f::Distance(A, B);
				}
			}
		}
	}
	return bFound;
}

bool FMRRouteGraph::FindRoute(const FVector2D& From, const FVector2D& To, TArray<FVector2f>& OutPts, float& OutLenCm, float MaxSnapCm) const
{
	OutPts.Reset();
	OutLenCm = 0.f;
	FSnap S, G;
	if (!Snap(From, MaxSnapCm, S) || !Snap(To, MaxSnapCm, G)) return false;
	const FEdge& SE = Edges[S.Edge];
	const FEdge& GE = Edges[G.Edge];
	const bool bSOneWay = (SE.Flags & 1) != 0, bGOneWay = (GE.Flags & 1) != 0;

	// Same road piece: ride straight along it if the direction is allowed.
	if (S.Edge == G.Edge && (G.Along >= S.Along || !bSOneWay))
	{
		OutPts.Add(S.P);
		if (G.Along >= S.Along) for (int32 i = S.Seg + 1; i <= G.Seg; ++i) OutPts.Add(Points[SE.First + i]);
		else for (int32 i = S.Seg; i > G.Seg; --i) OutPts.Add(Points[SE.First + i]);
		OutPts.Add(G.P);
		OutLenCm = FMath::Abs(G.Along - S.Along);
		return true;
	}

	// A* over junctions. Cost = length x road cost factor; heuristic = straight line x the cheapest factor.
	const int32 NN = Nodes.Num();
	TArray<float> Gc; Gc.Init(TNumericLimits<float>::Max(), NN);
	TArray<int32> Prev; Prev.Init(INDEX_NONE, NN); // edge*2+dir used to arrive (or -2 for a start node)
	struct FOpen { float F; int32 N; };
	TArray<FOpen> Heap;
	auto Less = [](const FOpen& A, const FOpen& B) { return A.F < B.F; };
	const FVector2f Goal = G.P;
	constexpr float MinFactor = 0.9f;
	auto H = [&](int32 N) { return FVector2f::Distance(Nodes[N], Goal) * MinFactor; };
	auto Push = [&](int32 N, float Cost, int32 Via)
	{
		if (Cost >= Gc[N]) return;
		Gc[N] = Cost; Prev[N] = Via;
		Heap.HeapPush({ Cost + H(N), N }, Less);
	};
	Push(SE.B, (SE.Len - S.Along) * SE.Cost, -2);            // leave forward, towards B
	if (!bSOneWay) Push(SE.A, S.Along * SE.Cost, -3);         // leave backward, towards A
	float Best = TNumericLimits<float>::Max();
	int32 BestNode = INDEX_NONE;
	// Finishing: from GE.A ride forward to the goal point; from GE.B ride backward (two-way only).
	auto Finish = [&](int32 N) -> float
	{
		float C = TNumericLimits<float>::Max();
		if (N == GE.A) C = FMath::Min(C, G.Along * GE.Cost);
		if (N == GE.B && !bGOneWay) C = FMath::Min(C, (GE.Len - G.Along) * GE.Cost);
		return C;
	};
	int32 Expanded = 0;
	while (Heap.Num() > 0)
	{
		FOpen Top;
		Heap.HeapPop(Top, Less);
		if (Top.F >= Best) break;
		const int32 N = Top.N;
		if (Top.F > Gc[N] + H(N) + 1.f) continue; // stale
		if (++Expanded > 2000000) break;
		const float Fin = Finish(N);
		if (Fin < TNumericLimits<float>::Max() && Gc[N] + Fin < Best) { Best = Gc[N] + Fin; BestNode = N; }
		for (int32 k = AdjStart[N]; k < AdjStart[N + 1]; ++k)
		{
			const int32 Code = Adj[k];
			const FEdge& E = Edges[Code >> 1];
			const int32 To2 = (Code & 1) ? E.B : E.A;
			Push(To2, Gc[N] + E.Len * E.Cost, Code);
		}
	}
	if (BestNode == INDEX_NONE) return false;

	// Walk back to the start, then emit points forwards.
	TArray<int32> Chain; // edge codes from the first junction to BestNode
	int32 N = BestNode;
	int32 StartVia = -2;
	for (int32 Guard = 0; Guard < NN; ++Guard)
	{
		const int32 Via = Prev[N];
		if (Via < 0) { StartVia = Via; break; }
		Chain.Add(Via);
		const FEdge& E = Edges[Via >> 1];
		N = (Via & 1) ? E.A : E.B;
	}
	Algo::Reverse(Chain);

	OutPts.Add(S.P);
	if (StartVia == -2) for (int32 i = S.Seg + 1; i < SE.Num; ++i) OutPts.Add(Points[SE.First + i]);
	else for (int32 i = S.Seg; i >= 0; --i) OutPts.Add(Points[SE.First + i]);
	for (const int32 Code : Chain)
	{
		const FEdge& E = Edges[Code >> 1];
		if (Code & 1) for (int32 i = 1; i < E.Num; ++i) OutPts.Add(Points[E.First + i]);
		else for (int32 i = E.Num - 2; i >= 0; --i) OutPts.Add(Points[E.First + i]);
	}
	if (BestNode == GE.A) // finish forward along the goal road
		for (int32 i = 1; i <= G.Seg; ++i) OutPts.Add(Points[GE.First + i]);
	else
		for (int32 i = GE.Num - 2; i > G.Seg; --i) OutPts.Add(Points[GE.First + i]);
	OutPts.Add(G.P);
	for (int32 i = 0; i + 1 < OutPts.Num(); ++i) OutLenCm += FVector2f::Distance(OutPts[i], OutPts[i + 1]);
	return true;
}

float MRDistanceToPolyline(const TArray<FVector2f>& Pts, const FVector2D& P, int32 FirstSeg, int32 LastSeg, int32& OutSeg, float& OutT)
{
	float Best = TNumericLimits<float>::Max();
	OutSeg = FMath::Clamp(FirstSeg, 0, FMath::Max(0, Pts.Num() - 2));
	OutT = 0.f;
	const FVector2f Pf(P);
	for (int32 i = FMath::Max(0, FirstSeg); i <= FMath::Min(LastSeg, Pts.Num() - 2); ++i)
	{
		float T;
		const float D = SegDist(Pts[i], Pts[i + 1], Pf, T);
		if (D < Best) { Best = D; OutSeg = i; OutT = T; }
	}
	return Best;
}
