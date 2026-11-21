#include "Core/MRRiderLook.h"

#include "Math/RandomStream.h"

#define MR_TABLE(Type, Name, ...) const TArray<Type>& FMRRiderLook::Name() { static const TArray<Type> T = { __VA_ARGS__ }; return T; }

MR_TABLE(FString, BodyNames, TEXT("Slim"), TEXT("Average"), TEXT("Broad"))
MR_TABLE(FString, SkinToneNames, TEXT("Porcelain"), TEXT("Fair"), TEXT("Light"), TEXT("Medium"), TEXT("Olive"), TEXT("Tan"),
	TEXT("Brown"), TEXT("Dark brown"), TEXT("Deep"), TEXT("Ebony"))
MR_TABLE(FLinearColor, SkinTones,
	FLinearColor(0.93f, 0.80f, 0.72f), FLinearColor(0.88f, 0.70f, 0.58f), FLinearColor(0.82f, 0.62f, 0.48f), FLinearColor(0.72f, 0.52f, 0.38f),
	FLinearColor(0.62f, 0.46f, 0.30f), FLinearColor(0.55f, 0.38f, 0.24f), FLinearColor(0.42f, 0.27f, 0.16f), FLinearColor(0.32f, 0.20f, 0.12f),
	FLinearColor(0.22f, 0.14f, 0.09f), FLinearColor(0.15f, 0.10f, 0.07f))
MR_TABLE(FString, HairStyles, TEXT("Shaved"), TEXT("Short"), TEXT("Curly"), TEXT("Long"), TEXT("Ponytail"), TEXT("Braids"), TEXT("Afro"), TEXT("Mohawk"))
MR_TABLE(FString, HairColourNames, TEXT("Black"), TEXT("Dark brown"), TEXT("Brown"), TEXT("Auburn"), TEXT("Ginger"), TEXT("Blonde"),
	TEXT("Grey"), TEXT("White"), TEXT("Blue dye"), TEXT("Pink dye"), TEXT("Green dye"))
MR_TABLE(FLinearColor, HairColours,
	FLinearColor(0.02f, 0.02f, 0.02f), FLinearColor(0.08f, 0.05f, 0.03f), FLinearColor(0.20f, 0.12f, 0.06f), FLinearColor(0.35f, 0.12f, 0.05f),
	FLinearColor(0.60f, 0.25f, 0.06f), FLinearColor(0.85f, 0.70f, 0.40f), FLinearColor(0.50f, 0.50f, 0.50f), FLinearColor(0.90f, 0.90f, 0.88f),
	FLinearColor(0.05f, 0.20f, 0.80f), FLinearColor(0.90f, 0.30f, 0.60f), FLinearColor(0.10f, 0.70f, 0.30f))
MR_TABLE(FString, FacialHairNames, TEXT("None"), TEXT("Stubble"), TEXT("Beard"))
MR_TABLE(FString, HelmetStyles, TEXT("Open-face"), TEXT("Full-face"), TEXT("Motocross"), TEXT("Skate / BMX"), TEXT("Road (aero)"))
MR_TABLE(FString, VisorNames, TEXT("Clear"), TEXT("Smoked"), TEXT("Iridium blue"), TEXT("Gold"), TEXT("Red mirror"))
MR_TABLE(FLinearColor, VisorColours,
	FLinearColor(0.60f, 0.70f, 0.75f), FLinearColor(0.05f, 0.05f, 0.06f), FLinearColor(0.10f, 0.25f, 0.80f), FLinearColor(0.80f, 0.60f, 0.10f), FLinearColor(0.80f, 0.10f, 0.08f))
MR_TABLE(FString, TopStyles, TEXT("Riding jacket"), TEXT("Hoodie"), TEXT("Jersey"), TEXT("Race leathers"), TEXT("Hi-vis vest"), TEXT("T-shirt"))
MR_TABLE(FString, BackpackNames, TEXT("None"), TEXT("Backpack"), TEXT("Courier bag"), TEXT("Hydration pack"))
MR_TABLE(FString, PaletteNames, TEXT("Red"), TEXT("Navy"), TEXT("Racing green"), TEXT("Yellow"), TEXT("Orange"), TEXT("White"),
	TEXT("Black"), TEXT("Gunmetal"), TEXT("Purple"), TEXT("Teal"), TEXT("Pink"), TEXT("Lime"), TEXT("Denim"), TEXT("Khaki"), TEXT("Brown"), TEXT("Sky blue"))
MR_TABLE(FLinearColor, Palette,
	FLinearColor(0.75f, 0.06f, 0.06f), FLinearColor(0.04f, 0.10f, 0.40f), FLinearColor(0.05f, 0.35f, 0.08f), FLinearColor(0.85f, 0.65f, 0.02f),
	FLinearColor(0.90f, 0.30f, 0.02f), FLinearColor(0.90f, 0.90f, 0.90f), FLinearColor(0.02f, 0.02f, 0.02f), FLinearColor(0.30f, 0.30f, 0.32f),
	FLinearColor(0.30f, 0.05f, 0.45f), FLinearColor(0.02f, 0.40f, 0.40f), FLinearColor(0.85f, 0.25f, 0.45f), FLinearColor(0.45f, 0.85f, 0.05f),
	FLinearColor(0.10f, 0.16f, 0.30f), FLinearColor(0.40f, 0.36f, 0.22f), FLinearColor(0.25f, 0.14f, 0.07f), FLinearColor(0.35f, 0.60f, 0.90f))

#undef MR_TABLE

void FMRRiderLook::Sanitise()
{
	auto Clamp = [](uint8& V, int32 Num) { V = static_cast<uint8>(FMath::Clamp<int32>(V, 0, FMath::Max(0, Num - 1))); };
	Clamp(Body, BodyNames().Num());
	HeightCm = static_cast<uint8>(FMath::Clamp<int32>(HeightCm, 150, 200));
	Clamp(SkinTone, SkinTones().Num());
	Clamp(HairStyle, HairStyles().Num());
	Clamp(HairColour, HairColours().Num());
	Clamp(FacialHair, FacialHairNames().Num());
	Clamp(HelmetStyle, HelmetStyles().Num());
	Clamp(Visor, VisorColours().Num());
	Clamp(TopStyle, TopStyles().Num());
	Clamp(Accent, Palette().Num());
	Clamp(Legs, Palette().Num());
	Clamp(Gloves, Palette().Num());
	Clamp(Shoes, Palette().Num());
	Clamp(Backpack, BackpackNames().Num());
}

FString FMRRiderLook::Encode() const
{
	const uint8 B[] = { Body, HeightCm, SkinTone, HairStyle, HairColour, FacialHair, HelmetStyle, Visor, TopStyle, Accent, Legs, Gloves, Shoes, Backpack };
	return BytesToHex(B, UE_ARRAY_COUNT(B));
}

FMRRiderLook FMRRiderLook::Decode(const FString& Hex)
{
	FMRRiderLook L;
	TArray<uint8> B;
	B.SetNumZeroed(Hex.Len() / 2);
	if (B.Num() > 0) HexToBytes(Hex.Left(B.Num() * 2), B.GetData());
	uint8* Fields[] = { &L.Body, &L.HeightCm, &L.SkinTone, &L.HairStyle, &L.HairColour, &L.FacialHair, &L.HelmetStyle, &L.Visor, &L.TopStyle, &L.Accent, &L.Legs, &L.Gloves, &L.Shoes, &L.Backpack };
	for (int32 i = 0; i < UE_ARRAY_COUNT(Fields) && i < B.Num(); ++i) *Fields[i] = B[i]; // older/shorter strings keep defaults
	L.Sanitise();
	return L;
}

FMRRiderLook FMRRiderLook::Random(int32 Seed)
{
	FRandomStream R(Seed);
	FMRRiderLook L;
	L.Body = static_cast<uint8>(R.RandRange(0, BodyNames().Num() - 1));
	L.HeightCm = static_cast<uint8>(R.RandRange(155, 195));
	L.SkinTone = static_cast<uint8>(R.RandRange(0, SkinTones().Num() - 1));
	L.HairStyle = static_cast<uint8>(R.RandRange(0, HairStyles().Num() - 1));
	L.HairColour = static_cast<uint8>(R.RandRange(0, HairColours().Num() - 1));
	L.FacialHair = static_cast<uint8>(R.RandRange(0, FacialHairNames().Num() - 1));
	L.HelmetStyle = static_cast<uint8>(R.RandRange(0, HelmetStyles().Num() - 1));
	L.Visor = static_cast<uint8>(R.RandRange(0, VisorColours().Num() - 1));
	L.TopStyle = static_cast<uint8>(R.RandRange(0, TopStyles().Num() - 1));
	L.Accent = static_cast<uint8>(R.RandRange(0, Palette().Num() - 1));
	L.Legs = static_cast<uint8>(R.RandRange(0, Palette().Num() - 1));
	L.Gloves = static_cast<uint8>(R.RandRange(0, Palette().Num() - 1));
	L.Shoes = static_cast<uint8>(R.RandRange(0, Palette().Num() - 1));
	L.Backpack = static_cast<uint8>(R.RandRange(0, BackpackNames().Num() - 1));
	return L;
}
