#include "ArenaHudFonts.h"
#include "ArenaRuntimeContent.h"

#include "Engine/Font.h"
#include "UObject/WeakObjectPtr.h"

namespace ArenaHudFontsLocal
{
	struct FHudFont
	{
		const TCHAR* Path;
		/** Tallest glyph cell (VSize) of the font in the stock package. */
		int32 Height;
	};
	// XInterface.HudBase FontArrayNames (largest first) with the cell heights of those fonts
	const FHudFont Fonts[ArenaHudFonts::NumFonts] = {
		{ TEXT("UT2003Fonts.FontEurostile37"), 63 },
		{ TEXT("UT2003Fonts.FontEurostile29"), 50 },
		{ TEXT("UT2003Fonts.FontEurostile24"), 42 },
		{ TEXT("UT2003Fonts.FontEurostile21"), 37 },
		{ TEXT("UT2003Fonts.FontEurostile17"), 30 },
		{ TEXT("UT2003Fonts.FontEurostile14"), 25 },
		{ TEXT("UT2003Fonts.FontEurostile12"), 21 },
		{ TEXT("UT2003Fonts.FontEurostile9"), 16 },
		{ TEXT("UT2003Fonts.FontSmallText"), 8 },
	};
	// HudBase FontScreenWidthMedium
	const float MediumWidths[8] = { 2048.f, 1600.f, 1280.f, 1024.f, 800.f, 640.f, 512.f, 400.f };
	constexpr int32 ConsoleFontSize = 5;
}

const TCHAR* ArenaHudFonts::FontPath(int32 Index)
{
	return ArenaHudFontsLocal::Fonts[FMath::Clamp(Index, 0, NumFonts - 1)].Path;
}

int32 ArenaHudFonts::NominalHeight(int32 Index)
{
	return ArenaHudFontsLocal::Fonts[FMath::Clamp(Index, 0, NumFonts - 1)].Height;
}

UFont* ArenaHudFonts::Get(int32 Index)
{
	if (!ArenaRuntimeContent::IsActive())
	{
		return nullptr;
	}
	// the provider keeps fonts for the session (and rate-limits retries of missing ones); this saves the lookups
	static TWeakObjectPtr<UFont> Cached[NumFonts];
	TWeakObjectPtr<UFont>& Slot = Cached[FMath::Clamp(Index, 0, NumFonts - 1)];
	if (UFont* Font = Slot.Get())
	{
		return Font;
	}
	UFont* Font = ArenaRuntimeContent::LoadFontByObjectPath(FontPath(Index));
	Slot = Font;
	return Font;
}

UFont* ArenaHudFonts::GetScaled(int32 Index, float Scale)
{
	if (!ArenaRuntimeContent::IsActive())
	{
		return nullptr;
	}
	const FString Path = FontPath(Index);
	const int32 Cell = ArenaRuntimeContent::GetFontCellHeight(Path);
	return Cell > 0 ? ArenaRuntimeContent::LoadFontAtHeight(Path, FMath::RoundToInt(float(Cell) * FMath::Max(Scale, 0.25f))) : nullptr;
}

int32 ArenaHudFonts::FontSizeIndex(float ClipX, int32 FontSize)
{
	for (const float W : { 512.f, 640.f, 800.f, 1024.f, 1280.f, 1600.f })
	{
		if (ClipX >= W)
		{
			++FontSize;
		}
	}
	return FMath::Clamp(8 - FontSize, 0, 8);
}

int32 ArenaHudFonts::ConsoleFontIndex(float ClipX)
{
	int32 FontSize = ArenaHudFontsLocal::ConsoleFontSize;
	for (const float W : { 640.f, 800.f, 1024.f, 1280.f, 1600.f })
	{
		if (ClipX < W)
		{
			++FontSize;
		}
	}
	return FMath::Min(8, FontSize);
}

int32 ArenaHudFonts::MediumFontIndex(float ClipX)
{
	for (int32 i = 0; i < 8; ++i)
	{
		if (ArenaHudFontsLocal::MediumWidths[i] <= ClipX)
		{
			return i;
		}
	}
	return 8;
}
