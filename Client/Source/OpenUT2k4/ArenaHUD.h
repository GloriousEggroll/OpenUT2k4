#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ArenaHUDWidgets.h"
#include "ArenaHUD.generated.h"

class UCanvas;
class UFont;
class AArenaCharacter;
class AArenaGameState;
class AArenaPlayerController;
class AArenaPlayerState;

enum class EArenaAlign : uint8
{
	Left,
	Center,
	Right
};

/**
 * Small canvas drawing toolkit shared by the in-game HUD and the menu HUD.
 * All sizes are in screen pixels; use S() to convert from 1080p reference units.
 */
struct OPENUT2K4_API FArenaDraw
{
	UCanvas* Canvas = nullptr;
	/** Canvas height / 1080 */
	float Scale = 1.f;

	FArenaDraw() = default;
	explicit FArenaDraw(UCanvas* InCanvas);

	float S(float RefPixels) const { return RefPixels * Scale; }
	float W() const;
	float H() const;

	static UFont* GetFont();

	FVector2D TextSize(const FString& Text, float SizePx) const;
	/** Draws text with its top at Y. Returns the drawn width. */
	float Text(const FString& Text, float X, float Y, const FLinearColor& Color, float SizePx,
		EArenaAlign Align = EArenaAlign::Left, bool bShadow = true) const;
	/** Text vertically centred on Y. */
	float TextMid(const FString& Text, float X, float Y, const FLinearColor& Color, float SizePx,
		EArenaAlign Align = EArenaAlign::Left, bool bShadow = true) const;

	/**
	 * UT2004 HUD fonts (ArenaHudFonts.h: index into HudBase's FontArrayNames, 0 = largest). Drawn at the font's native
	 * pixel size like UE2's Canvas (no scaling, positions rounded to pixels). Without the font in the content cache the
	 * engine font (GetFont) is used, scaled to the same line height.
	 */
	FVector2D TextSizeUT(int32 FontIndex, const FString& Text) const;
	/**
	 * Whole multiple the UT2004 bitmap fonts are drawn at: Round(height / 1080), at least 1 (2 on 4K, also a 4K window) (UE2 drew them 1:1).
	 * The font is resampled to that size and drawn 1:1 (ArenaHudFonts::GetScaled: smooth, not blocky). Sizes returned by TextSizeUT include it.
	 */
	float UTFontScale() const;
	/**
	 * The canvas width the UT2004 font size rules (ArenaHudFonts::FontSizeIndex etc., written for ClipX) are given: the
	 * width of a 4:3 screen of the canvas height divided by UTFontScale, so the font follows the screen height on any
	 * aspect ratio and a 4K screen picks the 1080p font drawn twice as big.
	 */
	float UTClipX() const;
	/** Top-left at X, Y (before alignment). Returns the drawn width. */
	float TextUT(int32 FontIndex, const FString& Text, float X, float Y, const FLinearColor& Color,
		EArenaAlign Align = EArenaAlign::Left, bool bShadow = false) const;
	/** A UT2004 DrawColor (sRGB bytes) as the colour to pass to the drawing functions, with an alpha multiplier. */
	static FLinearColor UTColor(const FColor& Color, float AlphaScale = 1.f);

	void Rect(float X, float Y, float Width, float Height, const FLinearColor& Color) const;
	void Outline(float X, float Y, float Width, float Height, const FLinearColor& Color, float Thickness = 1.f) const;
	/** Dark translucent panel with a subtle outline and an optional coloured accent strip on the left. */
	void Panel(float X, float Y, float Width, float Height, const FLinearColor& Accent = FLinearColor::Transparent, float Alpha = 1.f) const;
	/** Horizontal bar: background + fill of Fraction (0..1). */
	void Bar(float X, float Y, float Width, float Height, float Fraction, const FLinearColor& Color, float BgAlpha = 0.35f) const;
	void Line(float X0, float Y0, float X1, float Y1, const FLinearColor& Color, float Thickness = 1.f) const;
	/** Filled ring segment. Angles in radians, 0 = up, increasing clockwise. */
	void Arc(float CX, float CY, float InnerRadius, float OuterRadius, float StartAngle, float EndAngle,
		const FLinearColor& Color, int32 Segments = 16) const;
	/** Vertical gradient made from horizontal strips. */
	void VerticalGradient(float X, float Y, float Width, float Height, const FLinearColor& Top, const FLinearColor& Bottom, int32 Steps = 24) const;
};

/** Shared HUD colours / helpers (for game rules HUD hooks). */
namespace ArenaHudStyle
{
	OPENUT2K4_API FLinearColor TextWhite();
	OPENUT2K4_API FLinearColor TextDim();
	OPENUT2K4_API FLinearColor HealthColor();
	OPENUT2K4_API FLinearColor ShieldColor();
	OPENUT2K4_API FLinearColor AccentBlue();
	/** Team colour as an opaque tint (panels, bars). */
	OPENUT2K4_API FLinearColor TeamTint(int32 Team, float Brightness = 1.f);
	/** Lighter team colour that stays readable as text on dark panels. */
	OPENUT2K4_API FLinearColor TeamTextColor(int32 Team);
	OPENUT2K4_API FLinearColor WithAlpha(const FLinearColor& Color, float Alpha);
	/** "m:ss" */
	OPENUT2K4_API FString FormatTime(int32 Seconds);
}

/**
 * In-game HUD drawn entirely with UCanvas: vitals, ammo, weapon bar, crosshair, damage indicators,
 * kill feed, scores, timer, centre messages, scoreboard, death / end screens and the pause menu.
 */
UCLASS()
class OPENUT2K4_API AArenaHUD : public AHUD
{
	GENERATED_BODY()

public:
	AArenaHUD();

	virtual void DrawHUD() override;
	virtual void NotifyHitBoxClick(FName BoxName) override;
	virtual void NotifyHitBoxBeginCursorOver(FName BoxName) override;

	// ---- API for game rules HUD hooks (valid during DrawHUD) ----
	/** Drawing helper for the current canvas. */
	const FArenaDraw& GetDraw() const { return D; }
	/** Bottom (pixels) of the top-right score panel drawn this frame, including the goal line. */
	float GetScoreInfoBottom() const { return ScoreInfoBottom; }
	/**
	 * The standard two team score boxes (top right). ExtraHeight (1080p units) adds room under the score;
	 * DrawExtra(Team, X, Top, Width) draws into each box. GoalText is drawn under the boxes.
	 */
	void DrawTeamScorePanels(float ExtraHeight, TFunctionRef<void(int32 Team, float X, float Top, float Width)> DrawExtra, const FString& GoalText);
	/** World time in seconds (for animations). */
	float Now() const;

	// ---- UT2004 HUD (ArenaHUDUT.cpp, ArenaHUDUTModes.cpp, ArenaHUDScoreboard.cpp; ArenaHUDWidgets.h) ----
	/** True while the UT2004 HUD is drawn this frame (its art is available); rules then use the DrawUT* functions. */
	bool IsUTHud() const { return bUTHud; }
	/** Widget frame of this canvas (valid during DrawHUD). */
	const ArenaHudUT::FHudFrame& GetUTFrame() const { return UTFrame; }
	/** Yaw of a world location relative to the view, radians, 0 = straight ahead, positive = to the right. */
	float UTRelativeYaw(const FVector& WorldLocation) const;
	/** HudCDeathmatch.UpdateRankAndSpread: score (with its capsule unless bNoBackground), spread and rank widgets, top left. */
	void DrawUTRankAndSpread(int32 Score, int32 Spread, int32 Rank, bool bNoBackground = false);
	/** HudCTeamDeathMatch.ShowTeamScorePassA: the two team score capsules, team symbols, versus icon and scores (top centre). */
	void DrawUTTeamScores(bool bSymbols = true, bool bVersus = true);
	/** HudCCaptureTheFlag.ShowTeamScorePassA. FlagState per team: 0 home, 1 held by its team, 2 held by the enemy, 3 dropped. */
	void DrawUTCaptureTheFlag(const uint8 FlagState[2], const FVector* BaseLocation[2]);
	/** HudCBombingRun.ShowTeamScorePassA: the own team's score left. BallState as a flag state seen by the viewer's team. */
	void DrawUTBombingRun(uint8 BallState, const FVector* BallLocation);
	/** HudCDoubleDomination.ShowTeamScorePassA. PointState: 0 neutral, 1 the viewer's team (red for spectators), 2 the other, 3 disabled. */
	void DrawUTDoubleDomination(const uint8 PointState[2], const FVector* PointLocation[2]);
	/** One node of the Onslaught HUD (ONSHUDOnslaught). Stage as ONSPowerCore.CoreStage: 0 active, 2 constructing, 4 neutral. */
	struct FUTPowerNode
	{
		FVector Location = FVector::ZeroVector;
		int32 Team = 255;
		uint8 Stage = 4;
		bool bFinalCore = false;
		bool bUnderAttack = false;
		bool bSevered = false;
		/** PowerCoreAttackable: the viewer's team can attack (or build) it */
		bool bAttackable = false;
		int32 NodeNum = 0;
		float Health = 0.f, MaxHealth = 1.f;
		/** height of the health bar above Location (cm) */
		float BarHeight = 0.f;
	};
	/** ONSHUDOnslaught.ShowTeamScorePassA (core widgets and health) plus the radar map (ShowTeamScorePassC) and the in-world node health bars. */
	void DrawUTOnslaught(const TArray<FUTPowerNode>& Nodes, const TArray<TPair<int32, int32>>& Links, int32 ViewerTeam);
	/** HUD_Assault.ShowTeamScorePassA rows (top left): round time, reinforcements (seconds; < 0 = none), objective progress. */
	void DrawUTAssault(int32 RoundTimeLeft, int32 ReinforceSeconds, bool bShowRoundTime);
	/**
	 * HudMutant.DrawHudPassA: while another player is the Mutant, the centre radar ring with a dot towards it (coloured by
	 * the height difference) and range dots, and its name in a box at the top; the bottom feeder icon for the bottom feeder.
	 */
	void DrawUTMutant(const FString& MutantName, const FVector* MutantLocation, bool bBottomFeeder);
	/** HudCDeathmatch.DrawHudPassA in a vehicle: the driver's health, the vehicle's health box (PosX 0.139), adrenaline, crosshair. */
	void DrawUTVehicleHud(const AArenaCharacter* Driver, int32 VehicleHealth, float VehicleHealthMax);
	/**
	 * HUD_Assault.DrawVisibleObjective: an objective's type icon (Type 0 touch, 1 hold, 2 destroy, 3 use) in the defender's
	 * colour where it is in sight (shown 6 s, hidden for 1 s, as OBJ_FadedOutTime / OBJ_ReAppearTime), with its health /
	 * progress bar above while Progress > 0. Key identifies the objective for the fade timing.
	 */
	void DrawUTObjectiveMarker(const FVector& Location, int32 Type, int32 DefenderTeam, float Progress, int32 Key);
	/** HudBase.DrawTargeting: the turning reticle (HUDContent.Reticles.rotReticle001, additive) on a world location (ball pass target). */
	void DrawUTTargeting(const FVector& WorldLocation);
	/** HudInvasion.ShowTeamScorePassA / C: the radar (instead of team scores) with the monsters around the viewer. */
	void DrawUTInvasionRadar(const TArray<FVector>& Monsters);

protected:
	void DrawVitals(const AArenaCharacter* Char);
	void DrawAmmo(const AArenaCharacter* Char);
	void DrawWeaponBar(const AArenaCharacter* Char);
	void DrawCrosshair(const AArenaCharacter* Char);
	void DrawDamageIndicators(const AArenaCharacter* Char);
	void DrawPowerups(const AArenaCharacter* Char);
	void DrawKillFeed();
	void DrawScoreInfo();
	void DrawTimer();
	void DrawCenterMessages();
	void DrawPickupMessage();
	/** UT2004 local messages (ArenaLocalMessages.h, HudBase.DisplayLocalMessages): the 3-2-1 match start, kill / death, sprees ... */
	void DrawLocalMessages();
	void DrawScoreboard();
	void DrawDeadScreen();
	void DrawEndBanner();
	void DrawPauseMenu();
	void DrawPauseSettings();

	// ---- UT2004 HUD (ArenaHUDUT.cpp); used when its art is available, the procedural HUD above otherwise ----
	/** HUD.PostRender with the UT2004 HUD: scoreboard, player HUD or spectating bar, messages. */
	void DrawUTHud(AArenaCharacter* Char, bool bAlive, bool bEnded, bool bScoreboard);
	/** Centre messages of the game (flag taken, ...) as UT2004's CriticalEventPlus, the pickup message as PickupMessagePlus. */
	void DrawUTCenterMessages();
	void DrawUTPickupMessage();
	/** HudBase.DrawHUD passes A and C for a living player (HudCDeathmatch), and the mode's team / score pass. */
	void DrawUTPlayerHud(const AArenaCharacter* Char);
	void DrawUTVitals(const AArenaCharacter* Char);
	void DrawUTAmmo(const AArenaCharacter* Char);
	void DrawUTWeaponBar(const AArenaCharacter* Char);
	void DrawUTAdrenaline(const AArenaCharacter* Char);
	void DrawUTTimer();
	void DrawUTUDamage(const AArenaCharacter* Char);
	void DrawUTCrosshair(const AArenaCharacter* Char);
	/** HudBase.DisplayHit / DrawDamageIndicators: red gradients along the screen edges towards the damage. */
	void DrawUTDamageIndicators(const AArenaCharacter* Char);
	/** HUD.DrawSpectatingHud of HudCDeathmatch: the bar at the bottom while dead, spectating or after the match. */
	void DrawUTSpectatingHud();
	/** Score pass of the mode (rules' DrawScoreInfo, else TDM team scores or the DM score / rank / spread). */
	void DrawUTScorePass();
	/** ScoreBoardDeathMatch / ScoreBoardTeamDeathMatch (ArenaHUDScoreboard.cpp). */
	void DrawUTScoreboard();
	void DrawUTScoreboardDM();
	void DrawUTScoreboardTeams();
	/** ONS world health bars (ONSHUDOnslaught.DrawHealthBar) */
	void DrawUTNodeHealthBar(const FUTPowerNode& Node);

	bool bUTHud = false;
	ArenaHudUT::FHudFrame UTFrame;
	/** HudCDeathmatch animation state */
	struct FUTAnimState
	{
		int32 LastHealth = 0;
		float LastDamagedHealth = -100.f;
		float LastHealthPickupTime = -100.f;
		int32 LastShield = 0;
		float LastArmorPickupTime = -100.f;
		int32 LastEnergy = 0;
		float LastAdrenalineTime = -100.f;
		float LastPickupTime = -100.f;
		bool bHasWeapon[9] = {};
		float WeaponPickupTimer[9] = {};
		bool bWeaponIconAnimDone[9] = {};
		const AActor* LastPawn = nullptr;
	};
	FUTAnimState UTAnim;
	void UpdateUTAnimState(const AArenaCharacter* Char);

	/** Draws one team / all-player scoreboard column. Returns the height used. */
	float DrawScoreTable(const TArray<AArenaPlayerState*>& Players, float X, float Y, float Width, const FString& Title,
		const FLinearColor& Accent, int32 TeamScore, int32 Team = -1);

	AArenaPlayerController* GetArenaPC() const;
	AArenaGameState* GetArenaGS() const;
	class UArenaGameRules* GetRules() const;

	FArenaDraw D;
	float ScoreInfoBottom = 0.f;

	/** Non-shipping test hook (-AutoHudShow=Scoreboard:<from>-<to>,Pause:...,Settings:...): page forced on this frame. */
	bool bTestScoreboard = false;
	bool bTestPause = false;
	bool bTestSettings = false;
	void UpdateTestPages();
};
