#pragma once

#include "CoreMinimal.h"

/**
 * The UT2004 local message classes our HUD reproduces (match start countdown, kill messages, sprees ...).
 * Each kind maps to one stock class; ArenaLocalMessages::GetStyle has its look and ArenaLocalMessages::GetString
 * its text.
 */
enum class EArenaLocalMessage : uint8
{
	None,
	/** UnrealGame.StartupMessage: Switch = startup stage (2..4 = "about to begin" 3/2/1, 5 = the match has begun). */
	Startup,
	/** UnrealGame.TimerMessage: Switch = seconds left. */
	Timer,
	/** XGame.xKillerMessagePlus ("you killed <victim>"): Name1 = victim. */
	Killer,
	/** XGame.xVictimMessage ("you were killed by <killer>"): Name1 = killer. */
	Victim,
	/** XGame.MultiKillMessage: Switch = multi kill level (1 = double kill). */
	MultiKill,
	/** UnrealGame.KillingSpreeMessage: Switch = spree level (0 = 5 kills); Name1 = spree owner, Name2 = who ended it. */
	KillingSpree,
	/** UnrealGame.FirstBloodMessage: Name1 = killer. */
	FirstBlood,
	/** XGame.SpecialKillMessage (head shot). */
	SpecialKill,
	/** XGame.xDeathMessage: the console kill line (drawn by the console message area, not as a local message). */
	Death,
};

/**
 * Looks, lifetimes and texts of UT2004's local messages, and the HUD layout constants that place them.
 *
 * Values were derived once from the stock classes of the player's own install (class defaults and the bytecode of
 * the functions named below); they are numbers and rules, so they live here as a table. Message texts are never
 * copied: GetString reads them at runtime from the install's localization files (System/<Package>.int) and uses
 * our own wording when the install (or the key) is not there.
 *
 * Drawing (XInterface.HudBase.LocalizedMessage / LayoutMessage / DisplayLocalMessages / DrawMessage /
 * GetScreenCoords):
 *  - a bIsUnique message replaces the one of the same class (it keeps its place in the list); at most 8 messages;
 *  - font = HUD.GetFontSizeIndex(Canvas, GetFontSize(...)) (ArenaHudFonts::FontSizeIndex), text size from that font;
 *  - screen position = Pos * HudCanvasScale * Clip + (1 - HudCanvasScale) / 2 * Clip, then the DrawPivot offset
 *    (all of ours: 8 = middle-middle, centred on both axes);
 *  - messages with the same PosX, PosY, DrawPivot and StackMode are drawn as one stack, oldest first: SM_Down moves
 *    each next message down by its text height (as a fraction of the canvas, so the step is height * HudCanvasScale);
 *    SM_None messages are drawn at the same spot;
 *  - bFadeMessage: alpha = DrawColor.A * (time left / lifetime), linear over the whole lifetime;
 *  - plain Canvas.DrawTextClipped: no shadow, no outline.
 * Console lines (Engine.HUD.AddTextMessage / DisplayMessages; xDeathMessage is not bIsSpecial so its text goes
 * there): ConsoleMessageCount lines in GetConsoleFont, colour GetConsoleColor (xDeathMessage: HUD GreenColor), each
 * line lives the message class's Lifetime, no fade; position see ConsoleMessagePosX / Y below.
 */
namespace ArenaLocalMessages
{
	enum class EStackMode : uint8
	{
		None = 0,
		Up = 1,
		Down = 2,
	};

	struct FStyle
	{
		/** Stock class this style comes from ("Package.Class"). */
		const TCHAR* SourceClass;
		/** DrawColor (GetColor returns it for every class we use). sRGB bytes like UE2's Canvas colours. */
		FColor DrawColor;
		/** LocalMessage.FontSize (GetFontSize may change it per switch, see GetFontSize). */
		int32 FontSize;
		float PosX;
		float PosY;
		/** EDrawPivot (8 = DP_MiddleMiddle). */
		uint8 DrawPivot;
		EStackMode StackMode;
		/** Seconds (GetLifeTime returns Lifetime for every class we use). */
		float Lifetime;
		bool bFadeMessage;
		bool bIsUnique;
		/** False: the text goes to the console message lines instead of the local message list. */
		bool bIsSpecial;
	};

	// ---- HUD layout (XInterface.HudBase / HudCDeathmatch defaults) ----
	/** HUD.HudCanvasScale: the HUD is laid out in the middle 95% of the canvas. */
	constexpr float HudCanvasScale = 0.95f;
	/** Console message area (HUD.ConsoleMessagePosX default, HudCDeathmatch.ConsoleMessagePosY). */
	constexpr float ConsoleMessagePosX = 0.f;
	constexpr float ConsoleMessagePosY = 0.87f;
	/** HUD.ConsoleMessageCount. */
	constexpr int32 ConsoleMessageCount = 4;
	/** HudBase.LocalMessages array size. */
	constexpr int32 MaxLocalMessages = 8;
	/** xDeathMessage.GetConsoleColor = HUD.default.GreenColor. */
	inline FColor ConsoleKillColor() { return FColor(0, 255, 0, 255); }

	OPENUT2K4_API const FStyle& GetStyle(EArenaLocalMessage Kind);

	/**
	 * LocalMessage.GetFontSize with the per-class overrides: MultiKillMessage (switch < 4: 0, 4: 1, 7: 3, else 2) and
	 * KillingSpreeMessage (2 for the spree owner's own "spree" message, else 0).
	 */
	OPENUT2K4_API int32 GetFontSize(EArenaLocalMessage Kind, int32 Switch, bool bLocalIsName1, bool bHasName2);

	/**
	 * The message text, as the class's GetString / GetRelatedString builds it, with the strings of the install's .int
	 * files (our wording without them). bLocalIsName1: the local player is Name1 (UT2004 then uses GetRelatedString).
	 */
	OPENUT2K4_API FString GetString(EArenaLocalMessage Kind, int32 Switch, const FString& Name1, const FString& Name2, bool bLocalIsName1);

	/**
	 * The console kill line (xDeathMessage.GetString: the damage type's DeathString / MaleSuicide with %k and %o
	 * replaced). WeaponName is our weapon's display name (or a cause like "the void"); it picks the stock damage type of
	 * the matching UT2004 weapon. Killer empty = suicide / environment death.
	 */
	OPENUT2K4_API FString GetDeathString(const FString& Killer, const FString& Victim, const FString& WeaponName, bool bHeadshot);

	/** A string of System/<Package>.int ([Section] Key=...), quotes removed. False when the file or key is missing. */
	OPENUT2K4_API bool Localize(const FString& Package, const FString& Section, const FString& Key, FString& OutValue);
	/** True when the install's System/<Package>.int can be read. */
	OPENUT2K4_API bool HasLocalization(const FString& Package);
}
