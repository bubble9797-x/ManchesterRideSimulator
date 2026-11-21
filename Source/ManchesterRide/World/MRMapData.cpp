#include "World/MRMapData.h"

#include "ManchesterRide.h"
#include "World/MRGeoWorld.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FCriticalSection GMapLock;
	TSharedPtr<const FMRMapData> GMap;
	bool GMapRequested = false;
}

TSharedPtr<const FMRMapData> FMRMapData::Get()
{
	FScopeLock Lock(&GMapLock);
	if (!GMapRequested)
	{
		GMapRequested = true;
		const FString Path = FPaths::Combine(AMRGeoWorld::GeoDataDir(), TEXT("overview.json"));
		Async(EAsyncExecution::ThreadPool, [Path]()
		{
			TSharedPtr<FMRMapData> D = MakeShared<FMRMapData>();
			const double T0 = FPlatformTime::Seconds();
			if (D->Load(Path))
			{
				D->BuildGrid();
				UE_LOG(LogManchesterRide, Log, TEXT("Map overview: %d lines, %d places, %d labels (%.1f s)"), D->Lines.Num(), D->Places.Num(), D->Labels.Num(), FPlatformTime::Seconds() - T0);
				FScopeLock L(&GMapLock);
				GMap = D;
			}
			else
			{
				UE_LOG(LogManchesterRide, Warning, TEXT("Map overview missing (%s): run Tools/geo_pipeline/make_overview.mjs"), *Path);
			}
		});
	}
	return GMap;
}

bool FMRMapData::Load(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path)) return false;
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) return false;

	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Root->TryGetArrayField(TEXT("lines"), Arr))
	{
		Lines.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>& L = V->AsArray();
			if (L.Num() < 5) continue;
			FLine Line;
			Line.Kind = static_cast<uint8>(FMath::Clamp(static_cast<int32>(L[0]->AsNumber()), 0, NumKinds - 1));
			Line.First = Points.Num();
			Line.Bounds = FBox2f(ForceInit);
			for (int32 i = 1; i + 1 < L.Num(); i += 2)
			{
				const FVector2f P(static_cast<float>(L[i]->AsNumber() * 100.0), static_cast<float>(L[i + 1]->AsNumber() * 100.0));
				Points.Add(P);
				Line.Bounds += P;
			}
			Line.Num = Points.Num() - Line.First;
			Lines.Add(Line);
		}
	}
	if (Root->TryGetArrayField(TEXT("places"), Arr))
	{
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>& P = V->AsArray();
			if (P.Num() < 4) continue;
			const FString Type = P[1]->AsString();
			const uint8 Rank = Type == TEXT("city") ? 0 : Type == TEXT("town") ? 1 : (Type == TEXT("suburb") || Type == TEXT("village")) ? 2 : 3;
			Places.Add({ P[0]->AsString(), Rank, FVector2f(static_cast<float>(P[2]->AsNumber() * 100.0), static_cast<float>(P[3]->AsNumber() * 100.0)) });
		}
	}
	if (Root->TryGetArrayField(TEXT("labels"), Arr))
	{
		Labels.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>& L = V->AsArray();
			if (L.Num() < 5) continue;
			Labels.Add({ L[0]->AsString(), FVector2f(static_cast<float>(L[1]->AsNumber() * 100.0), static_cast<float>(L[2]->AsNumber() * 100.0)),
				static_cast<float>(L[3]->AsNumber()), static_cast<uint8>(L[4]->AsNumber()) });
		}
	}
	return Lines.Num() > 0;
}

void FMRMapData::BuildGrid()
{
	FBox2f All(ForceInit);
	for (const FLine& L : Lines) All += L.Bounds;
	GridMin = All.Min;
	GridW = FMath::Max(1, FMath::CeilToInt((All.Max.X - All.Min.X) / BucketCm) + 1);
	GridH = FMath::Max(1, FMath::CeilToInt((All.Max.Y - All.Min.Y) / BucketCm) + 1);
	LineBuckets.SetNum(GridW * GridH);
	LabelBuckets.SetNum(GridW * GridH);
	auto Cell = [&](float X, float Y) { return FIntPoint(FMath::Clamp(FMath::FloorToInt((X - GridMin.X) / BucketCm), 0, GridW - 1), FMath::Clamp(FMath::FloorToInt((Y - GridMin.Y) / BucketCm), 0, GridH - 1)); };
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FIntPoint A = Cell(Lines[i].Bounds.Min.X, Lines[i].Bounds.Min.Y), B = Cell(Lines[i].Bounds.Max.X, Lines[i].Bounds.Max.Y);
		for (int32 y = A.Y; y <= B.Y; ++y) for (int32 x = A.X; x <= B.X; ++x) LineBuckets[y * GridW + x].Add(i);
	}
	for (int32 i = 0; i < Labels.Num(); ++i)
	{
		const FIntPoint C = Cell(Labels[i].P.X, Labels[i].P.Y);
		LabelBuckets[C.Y * GridW + C.X].Add(i);
	}
}

void FMRMapData::ForEachLine(const FBox2f& Area, TFunctionRef<void(const FLine&)> Fn) const
{
	if (GridW == 0) return;
	const int32 X0 = FMath::Clamp(FMath::FloorToInt((Area.Min.X - GridMin.X) / BucketCm), 0, GridW - 1), X1 = FMath::Clamp(FMath::FloorToInt((Area.Max.X - GridMin.X) / BucketCm), 0, GridW - 1);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt((Area.Min.Y - GridMin.Y) / BucketCm), 0, GridH - 1), Y1 = FMath::Clamp(FMath::FloorToInt((Area.Max.Y - GridMin.Y) / BucketCm), 0, GridH - 1);
	TBitArray<> Seen(false, Lines.Num());
	for (int32 y = Y0; y <= Y1; ++y)
		for (int32 x = X0; x <= X1; ++x)
			for (const int32 i : LineBuckets[y * GridW + x])
			{
				if (Seen[i]) continue;
				Seen[i] = true;
				if (Lines[i].Bounds.Intersect(Area)) Fn(Lines[i]);
			}
}

void FMRMapData::ForEachLabel(const FBox2f& Area, TFunctionRef<void(const FLabel&)> Fn) const
{
	if (GridW == 0) return;
	const int32 X0 = FMath::Clamp(FMath::FloorToInt((Area.Min.X - GridMin.X) / BucketCm), 0, GridW - 1), X1 = FMath::Clamp(FMath::FloorToInt((Area.Max.X - GridMin.X) / BucketCm), 0, GridW - 1);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt((Area.Min.Y - GridMin.Y) / BucketCm), 0, GridH - 1), Y1 = FMath::Clamp(FMath::FloorToInt((Area.Max.Y - GridMin.Y) / BucketCm), 0, GridH - 1);
	for (int32 y = Y0; y <= Y1; ++y)
		for (int32 x = X0; x <= X1; ++x)
			for (const int32 i : LabelBuckets[y * GridW + x])
				if (Area.IsInside(Labels[i].P)) Fn(Labels[i]);
}
