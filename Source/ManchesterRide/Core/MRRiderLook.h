#pragma once

#include "CoreMinimal.h"
#include "MRRiderLook.generated.h"

/**
 * Character editor data: everything about how your rider looks (jacket and helmet colours stay
 * in their own fields for compatibility). All options are free and available from the start.
 *
 * Stored as small indices so it replicates in ~14 bytes and fits in a login URL option
 * (?look=<hex>). Every value is clamped on receipt, so a modified client can't crash others.
 */
USTRUCT(BlueprintType)
struct FMRRiderLook
{
	GENERATED_BODY()

	UPROPERTY() uint8 Body = 1;        // 0 slim, 1 average, 2 broad
	UPROPERTY() uint8 HeightCm = 178;  // 150..200
	UPROPERTY() uint8 SkinTone = 3;    // index into SkinTones
	UPROPERTY() uint8 HairStyle = 1;   // index into HairStyles (shows under the helmet)
	UPROPERTY() uint8 HairColour = 1;  // index into HairColours
	UPROPERTY() uint8 FacialHair = 0;  // 0 none, 1 stubble, 2 beard
	UPROPERTY() uint8 HelmetStyle = 0; // index into HelmetStyles
	UPROPERTY() uint8 Visor = 0;       // index into VisorColours
	UPROPERTY() uint8 TopStyle = 0;    // index into TopStyles
	UPROPERTY() uint8 Accent = 5;      // stripes / trim, index into the shared palette
	UPROPERTY() uint8 Legs = 12;       // trousers, palette index
	UPROPERTY() uint8 Gloves = 6;      // palette index
	UPROPERTY() uint8 Shoes = 6;       // palette index
	UPROPERTY() uint8 Backpack = 0;    // 0 none, 1 backpack, 2 courier bag, 3 hydration pack

	bool operator==(const FMRRiderLook& O) const { return Encode() == O.Encode(); }

	/** Clamp every field to a valid option. */
	void Sanitise();
	FString Encode() const;
	static FMRRiderLook Decode(const FString& Hex);
	static FMRRiderLook Random(int32 Seed);

	float HeightScale() const { return static_cast<float>(HeightCm) / 178.f; }
	float WidthScale() const { return Body == 0 ? 0.85f : (Body == 2 ? 1.2f : 1.f); }

	// ---- Option tables (names shown in the editor) ----
	static const TArray<FString>& BodyNames();
	static const TArray<FString>& SkinToneNames();
	static const TArray<FLinearColor>& SkinTones();
	static const TArray<FString>& HairStyles();
	static const TArray<FString>& HairColourNames();
	static const TArray<FLinearColor>& HairColours();
	static const TArray<FString>& FacialHairNames();
	static const TArray<FString>& HelmetStyles();
	static const TArray<FString>& VisorNames();
	static const TArray<FLinearColor>& VisorColours();
	static const TArray<FString>& TopStyles();
	static const TArray<FString>& BackpackNames();
	/** Shared clothing palette: the 12 garage colours plus denim, khaki, brown, sky blue. */
	static const TArray<FString>& PaletteNames();
	static const TArray<FLinearColor>& Palette();
};
