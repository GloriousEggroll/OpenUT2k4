#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ArenaUserSettings.generated.h"

/**
 * Persistent user preferences (stored in Game.ini under [/Script/OpenUT2k4.ArenaUserSettings]).
 * Access through UArenaUserSettings::Get(), which returns the mutable class default object.
 */
UCLASS(config = Game)
class OPENUT2K4_API UArenaUserSettings : public UObject
{
	GENERATED_BODY()

public:
	UArenaUserSettings();

	/** The mutable CDO holding the current settings. Never null. */
	static UArenaUserSettings* Get();

	virtual void PostInitProperties() override;

	/** Write the current values to the user config file. */
	void Save();

	/** Push the volume setting to the main audio device. */
	void ApplyAudio() const;

	// ---- shared adjustable settings (used by the main menu and the in-game pause menu) ----
	/** Ids of the adjustable rows, in display order: Sensitivity, InvertY, FOV, Crosshair, HudScale, HudWidth, HudWidthPercent, Brightness, RayTracing, FullRayTracing, Upscaler, UpscalerQuality, FSRSharpness, LowLatency, Volume, MusicVolume, Monitor, WindowMode, Resolution, HDR, HDRPeak, HDRPaperWhite. */
	static const TArray<FName>& GetAdjustableSettingIds();
	/** Id of the crosshair row (the HUD draws a preview next to it). */
	static FName CrosshairSettingId() { return FName(TEXT("Crosshair")); }
	/** Human readable label for an adjustable setting id. */
	static FString GetSettingLabel(FName Id);
	/** Current value as display text (e.g. "1.40", "On", "100", "85%"). */
	FString GetSettingValueText(FName Id) const;
	/**
	 * Step a setting by Dir (-1 / +1; toggles ignore the sign). Applies immediately (volume is pushed
	 * to the audio device) and saves. Returns false for unknown ids.
	 */
	bool ChangeSetting(FName Id, int32 Dir);
	/** Next mouse sensitivity: 0.1 .. 10, step 0.1 below 2 and 0.25 above. */
	static float StepSensitivity(float Current, int32 Dir);

	/**
	 * UT2004 stats account rules (UT2K4Tab_GameSettings.InvalidWarning): the username has at least 4 characters and the
	 * password at least 6. CleanStatsText makes a typed value usable in the AUTH login: spaces become underscores
	 * (GUIEditBox.bConvertSpaces), quotes and control characters are dropped, at most MaxStatsTextLen characters.
	 */
	static constexpr int32 MinStatsUsernameLen = 4;
	static constexpr int32 MinStatsPasswordLen = 6;
	static constexpr int32 MaxStatsTextLen = 32;
	static FString CleanStatsText(const FString& In);
	/** Username and password follow the UT2004 rules. */
	bool HasValidStatsLogin() const;

	static constexpr float MinSensitivity = 0.1f;
	static constexpr float MaxSensitivity = 10.f;
	static constexpr float MinFOV = 80.f;
	static constexpr float MaxFOV = 130.f;
	/** Brightness range in exposure stops (EV), stepped by BrightnessStep. */
	static constexpr float MinBrightness = -2.f;
	static constexpr float MaxBrightness = 2.f;
	static constexpr float BrightnessStep = 0.25f;
	/** HUD scale range (UT2004's HUD.HudScale, default 1), stepped by HudScaleStep. */
	static constexpr float MinHudScale = 0.5f;
	static constexpr float MaxHudScale = 1.5f;
	static constexpr float HudScaleStep = 0.05f;

	// ---- controls / view ----
	UPROPERTY(config) float MouseSensitivity = 1.f;
	UPROPERTY(config) bool bInvertY = false;
	/** UT2004 GameInfo.bWeaponShouldViewShake ("Weapons Shake View"): firing weapons shake the shooter's view
	 * (PlayerController.WeaponShakeView). Explosions, damage and vehicle blasts shake the view regardless, as in UT2004. */
	UPROPERTY(config) bool bWeaponViewShake = true;
	/**
	 * Field of view the UT2004 way: horizontal degrees on a 4:3 screen (UT2004's DefaultFOV 90). Wider screens keep
	 * the vertical extent and see more to the sides (Hor+, ArenaDisplay::GetHorizontalFOV): 90 is about 106 at 16:9,
	 * 121 at 21:9 and 139 at 32:9.
	 */
	UPROPERTY(config) float FOV = 90.f;
	/** 0 = FOV was saved as horizontal degrees at 16:9 (before the 4:3 definition); converted once on load. */
	UPROPERTY(config) int32 FOVVersion = 0;
	/**
	 * Display brightness in exposure stops (EV), -2 .. +2, 0 = neutral. Added to the exposure bias of whatever
	 * post process is active in game views (FArenaBrightnessViewExtension), so it works on every map.
	 */
	UPROPERTY(config) float Brightness = 0.f;
	/**
	 * Hardware ray traced lighting (Lumen): 0 = Auto (on with a discrete GPU that supports it), 1 = On, 2 = Off.
	 * Applied right away by ArenaRayTracing::Apply; unsupported hardware always uses software Lumen.
	 */
	UPROPERTY(config) int32 RayTracing = 0;
	/**
	 * Full Ray Tracing on top of ray traced Lumen (ArenaRayTracing::IsFullActive): 0 = Off, 1 = On. Only takes
	 * effect (and can only be switched) while ray traced Lumen is active; applied right away.
	 */
	UPROPERTY(config) int32 FullRayTracing = 0;
	/**
	 * Temporal upscaler (ArenaUpscalerSettings): 0 = TSR, 1 = AMD FSR 3.1, 2 = Off (native resolution, FXAA).
	 * Applied right away; FSR falls back to TSR where it is unsupported.
	 */
	UPROPERTY(config) int32 Upscaler = 0;
	/** Render resolution of TSR / FSR: 0 = Auto, 1 = Native AA, 2 = Quality, 3 = Balanced, 4 = Performance, 5 = Ultra Performance. */
	UPROPERTY(config) int32 UpscalerQuality = 0;
	/** FSR RCAS sharpening 0..1 (0 = off). */
	UPROPERTY(config) float FSRSharpness = 0.2f;
	/**
	 * Low latency: 0 = Off, 1 = On. AMD Anti-Lag (VK_AMD_anti_lag) where the Vulkan driver provides it, engine frame
	 * pacing (r.OneFrameThreadLag 0) elsewhere. Applied right away (ArenaUpscalerSettings).
	 */
	UPROPERTY(config) int32 LowLatency = 0;
	UPROPERTY(config) FString PlayerName = TEXT("Player");
	/** Master volume 0..1 */
	UPROPERTY(config) float Volume = 1.f;
	/** Music volume 0..1, on top of the master volume (UArenaMusicSubsystem reads it every frame) */
	UPROPERTY(config) float MusicVolume = 0.6f;
	/** Crosshair choice: 0 = built-in, 1.. = crosshair textures (ArenaArtHUD::GetCrosshairName). */
	UPROPERTY(config) int32 Crosshair = 0;
	/**
	 * HUD.HudScale of UT2004 (1 = stock size): multiplies every HUD widget (ArenaHUDWidgets.h). The HUD already follows
	 * the screen height, this is the player's extra size preference.
	 */
	UPROPERTY(config) float HudScale = 1.f;
	/**
	 * HUD width (ArenaHudUT::HudRegion): the edge anchored HUD widgets are laid out in a centred region of the screen
	 * width; 0 = full width, 1 = 21:9, 2 = 16:9 (of the screen height; never wider than the screen), 3 = HudWidthPercent.
	 */
	UPROPERTY(config) int32 HudWidth = 0;
	/** Region width in percent of the screen width for HudWidth 3 (50 .. 100). */
	UPROPERTY(config) float HudWidthPercent = 100.f;
	/** Fullscreen monitor: 0 = automatic (display at the desktop origin), 1.. = display (ArenaDisplay). */
	UPROPERTY(config) int32 Monitor = 0;
	/** Window mode: 0 = Fullscreen (renders at ResolutionX x ResolutionY, scaled to the display), 1 = Borderless (desktop size), 2 = Windowed. */
	UPROPERTY(config) int32 WindowMode = 1;
	/** Fullscreen / windowed resolution (ArenaDisplay::GetResolutions); 0 = the display's desktop size. */
	UPROPERTY(config) int32 ResolutionX = 0;
	UPROPERTY(config) int32 ResolutionY = 0;
	/** HDR output: 0 = Off, 1 = On (ArenaHDR; only when the display, compositor and swapchain support HDR10). Applied right away. */
	UPROPERTY(config) int32 HDR = 0;
	/** HDR peak brightness in nits for the tonemapper (0 = automatic: the display's own, else 1000). */
	UPROPERTY(config) int32 HDRPeakNits = 0;
	/** HDR paper white in nits: the brightness of SDR white for the scene, the HUD and the menus. */
	UPROPERTY(config) int32 HDRPaperWhiteNits = 200;
	/** UT2004 install chosen by the player (ArenaContent::FindUT2004Install); empty = search automatically. */
	UPROPERTY(config) FString UT2004Dir;
	/**
	 * When the player agreed to the Epic Games Terms of Service before downloading UT2004 from OldUnreal
	 * (FArenaContentDownload, ISO 8601 UTC); empty = never agreed.
	 */
	UPROPERTY(config) FString AcceptedEpicTermsUtc;

	// ---- last used instant action setup ----
	/** Registered game mode id ("DM", "TDM", "CTF", or any mode registered with ARENA_REGISTER_RULES) */
	UPROPERTY(config) FString LastMode = TEXT("DM");
	/** Layout id ("Imported:<Map>"); empty = the mode's default map. */
	UPROPERTY(config) FString LastLayout;
	UPROPERTY(config) int32 LastBots = 7;
	UPROPERTY(config) int32 LastSkill = 3;
	UPROPERTY(config) int32 LastGoal = 25;
	/** Minutes, 0 = none */
	UPROPERTY(config) int32 LastTimeLimit = 15;
	UPROPERTY(config) bool bLastInstagib = false;
	/** Instant Action "Translocator" per game mode ("CTF=1", "DM=0"; UT2004 keeps bAllowTrans per game class). Modes not
	 *  listed use their default (UArenaGameRules::GetDefaultTranslocator: CTF and Bombing Run on). */
	UPROPERTY(config) TArray<FString> LastTranslocator;
	UPROPERTY(config) int32 LastTeam = 0;
	/** Mutators chosen on the Instant Action Mutators tab ("Package.Class,Package.Class", the ?Mutator= option). */
	UPROPERTY(config) FString LastMutators;
	/** Instant Action map list: only maps of the UT2004 install ("Only Official Maps"), no custom maps. */
	UPROPERTY(config) bool bLastOfficialMapsOnly = false;

	// ---- Settings > Player (UT2K4Tab_PlayerSettings, ArenaPlayerSettingsTab.h) ----
	/** The player's character (a .upl DefaultName); sent as ?Character= / ?Sex= like UT2004's URL options. */
	UPROPERTY(config) FString PlayerCharacter = TEXT("Jakob");
	/** "M" / "F", from the character's record. */
	UPROPERTY(config) FString PlayerSex = TEXT("M");
	/** Preferred team: 0 red, 1 blue, 2 none (UT2004 Team 255). */
	UPROPERTY(config) int32 PreferredTeam = 2;
	/** Voice pack class ("XGame.MercMaleVoice"); empty = the character's default. */
	UPROPERTY(config) FString VoiceType;
	/** Weapon hand: 0 left, 1 center, 2 right, 3 hidden (UT2004 Handedness + 1). */
	UPROPERTY(config) int32 WeaponHand = 2;
	/** PlayerController.bSmallWeapons */
	UPROPERTY(config) bool bSmallWeapons = false;

	// ---- single player (ArenaSinglePlayer.h) ----
	/**
	 * Characters unlocked by finishing the tournament (UT2K4MainPage.TotalUnlockedCharacters): the Menu tags of their
	 * .upl records ("MALCOLM", "CLANLORD", "XAN"), separated by ";".
	 */
	UPROPERTY(config) FString UnlockedCharacters;

	// ---- Host Game (server rules tab, ArenaGameSession.h) ----
	UPROPERTY(config) FString HostServerName = TEXT("OpenUT2k4 Server");
	UPROPERTY(config) int32 HostMaxPlayers = 16;
	UPROPERTY(config) int32 HostMaxSpectators = 2;
	/** Game password players must give to join (empty = open). */
	UPROPERTY(config) FString HostPassword;
	/** UDP game port; the query port is this + 1 (as UT2004). */
	UPROPERTY(config) int32 HostPort = 7777;
	/** UT2K4Tab_ServerRulesPanel.bLANServer: LAN play (more bandwidth per client). */
	UPROPERTY(config) bool bHostLANServer = false;
	/** "Show in Server Browsers": the UE2 server registers with the OldUnreal master servers (off by default). LAN queries are always answered. */
	UPROPERTY(config) bool bHostAdvertise = false;
	/** Players banned from servers you host (Host Game admin tab): "<player id>|<name>"; the id is the one UT2004 servers
	 *  derive from a client's CD key hash (Net/UE2Server.h), so a ban holds across names. */
	UPROPERTY(config) TArray<FString> HostBannedPlayers;

	// ---- server browser (Net/ArenaServerBrowser*) ----
	/** Favourite UT2004 servers: "host:port" (game port; host may be a name), in the order added. */
	UPROPERTY(config) TArray<FString> ServerFavourites;
	/** Server browser filters: hide servers without players / full servers. */
	UPROPERTY(config) bool bBrowserHideEmpty = false;
	UPROPERTY(config) bool bBrowserHideFull = false;
	/** Last server browser tab. */
	UPROPERTY(config) int32 BrowserTab = 2;
	/**
	 * UT2004 master servers ("host:port[,group]"). Empty = the community masters the OldUnreal 3374 patch lists in its
	 * Default.ini; one master of each group is asked (the masters of a group share their lists).
	 */
	UPROPERTY(config) TArray<FString> MasterServers;

	// ---- UT2004 stats (Net/UE2Net*): sent in AUTH when joining a server. Defaults match UT2004: off and blank. ----
	/** "Track Stats": send the stats account to stats-enabled servers. */
	UPROPERTY(config) bool bTrackStats = false;
	/** UT2004 stats account (sent only when bTrackStats). The password is never logged. */
	UPROPERTY(config) FString StatsUsername;
	UPROPERTY(config) FString StatsPassword;
	/**
	 * A CD key generated for an install that has none and cannot be written (Net/UE2NetCDKey). Normally the generated
	 * key goes into the install itself. Never logged.
	 */
	UPROPERTY(config) FString GeneratedCDKey;
};
