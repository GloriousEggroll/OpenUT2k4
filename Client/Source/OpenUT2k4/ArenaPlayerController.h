#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ArenaTypes.h"
#include "ArenaLocalMessages.h"
#include "ArenaPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/** One line in the kill feed. */
USTRUCT()
struct FArenaKillMessage
{
	GENERATED_BODY()
	UPROPERTY() FString Killer;
	UPROPERTY() FString Victim;
	UPROPERTY() FString Weapon;
	UPROPERTY() FLinearColor KillerColor = FLinearColor::White;
	UPROPERTY() FLinearColor VictimColor = FLinearColor::White;
	UPROPERTY() bool bHeadshot = false;
	/** The UT2004 console kill line (ArenaLocalMessages::GetDeathString), drawn in the console message area. */
	UPROPERTY() FString ConsoleText;
	/** Colour of the console line (alpha 0: the kill line green). */
	UPROPERTY() FColor ConsoleColor = FColor(0, 0, 0, 0);
	float Time = 0.f;
};

/** Big centre-of-screen message ("Killing Spree!", "Red flag taken" ...). */
USTRUCT()
struct FArenaCenterMessage
{
	GENERATED_BODY()
	UPROPERTY() FString Text;
	UPROPERTY() FLinearColor Color = FLinearColor::White;
	float Time = 0.f;
	float Duration = 3.f;
	/** UT2004 local message (AddLocalMessage): its class, switch and font size; None for our own centre messages. */
	EArenaLocalMessage Kind = EArenaLocalMessage::None;
	int32 Switch = 0;
	int32 FontSize = 0;
	/** A message class we have no kind for (a UT2004 server's ONS / mod messages): its own style and class name. */
	bool bCustomStyle = false;
	ArenaLocalMessages::FStyle CustomStyle = {};
	FName CustomClass;
	const ArenaLocalMessages::FStyle& GetStyle() const { return bCustomStyle ? CustomStyle : ArenaLocalMessages::GetStyle(Kind); }
};

/**
 * In-game player controller. Builds Enhanced Input actions at runtime (no input assets),
 * forwards input to AArenaCharacter, holds HUD message queues, pause menu state.
 *
 * Functions in the "API" section are called by other systems and must keep their signatures.
 */
UCLASS()
class OPENUT2K4_API AArenaPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AArenaPlayerController();

	// ---------------- API ----------------
	/**
	 * While above 0 the message adders below keep their messages to themselves: a UT2004 server's own script HUD shows
	 * them (Net/UE2NetScriptHUD.h feeds its message lists), our HUD only when that HUD falls back.
	 */
	static int32 MessageDisplayHold;
	void AddKillMessage(APlayerState* Killer, APlayerState* Victim, const FString& WeaponName, bool bHeadshot);
	/** A kill by a killer with no player state (an automatic turret): named killer, ready-made console line. */
	void AddNamedKillMessage(const FString& KillerName, APlayerState* Victim, const FString& WeaponName, const FString& ConsoleText);
	/** A line in the console message area (UT2004 HUD.AddTextMessage: chat, server messages, kill lines from a server). */
	void AddConsoleLine(const FString& Text, const FColor& Color);
	void AddCenterMessage(const FString& Text, const FLinearColor& Color, float Duration = 3.f);
	/**
	 * A UT2004 local message (ArenaLocalMessages.h): text, colour, font size, lifetime and uniqueness as the stock
	 * message class has them. Name1 / Name2 are the class's RelatedPRI_1 / RelatedPRI_2 (may be null).
	 */
	void AddLocalMessage(EArenaLocalMessage Kind, int32 Switch = 0, const APlayerState* Name1 = nullptr, const APlayerState* Name2 = nullptr);
	/**
	 * A local message of a class without a kind of ours (Net/UE2NetWorld.h: a UT2004 server's ONS / mod message
	 * classes): its text and the class's own style (DrawColor, FontSize, PosX / PosY, Lifetime ...). A bIsUnique
	 * style replaces the message of the same ClassName.
	 */
	void AddCustomLocalMessage(const FString& Text, const ArenaLocalMessages::FStyle& Style, int32 FontSize, FName ClassName);
	/** Our shot hit an enemy (hit sound + crosshair marker). */
	void NotifyHitEnemy(float Damage, bool bHeadshot);
	/** We took damage from a direction (damage indicator). */
	void NotifyTookDamage(const FVector& FromLocation, float Damage);
	/** Short pickup message at bottom of screen. */
	void NotifyPickup(const FString& ItemName);
	/** Called when our pawn died. */
	void NotifyDied(APlayerState* Killer, const FString& WeaponName);

	const TArray<FArenaKillMessage>& GetKillMessages() const { return KillMessages; }
	const TArray<FArenaCenterMessage>& GetCenterMessages() const { return CenterMessages; }
	/** UT2004 local messages, oldest first (HudBase.LocalMessages). */
	const TArray<FArenaCenterMessage>& GetLocalMessages() const { return LocalMessages; }
	FString GetPickupMessage(float& OutAge) const;
	float GetLastHitEnemyTime() const { return LastHitEnemyTime; }
	bool WasLastHitHeadshot() const { return bLastHitHeadshot; }
	/** Damage indicators: world source locations with timestamps */
	struct FDamageIndicator { FVector From; float Time; float Damage; };
	const TArray<FDamageIndicator>& GetDamageIndicators() const { return DamageIndicators; }
	bool IsScoreboardVisible() const;
	/** Shows / hides the scoreboard as if the key were held (test hooks). */
	void SetScoreboardHeld(bool bHeld) { bScoreboardHeld = bHeld; }
	bool IsPauseMenuOpen() const { return bPauseMenuOpen; }
	int32 GetPauseMenuSelection() const { return PauseMenuSelection; }
	/**
	 * FOV for a camera component with the default 16:9 AspectRatio (vehicles): the settings or weapon zoom FOV
	 * (4:3 horizontal, UT2004 style) converted for the current viewport (ArenaDisplay::GetCameraFOV).
	 */
	float GetDesiredFOV() const;
	/** Settings FOV or weapon zoom FOV in UT2004 terms (horizontal degrees at 4:3). */
	float GetDesiredFOV43() const;

	// ---- pause menu helpers (used by AArenaHUD for mouse support) ----
	static const TArray<FString>& GetPauseMenuItems();
	void SetPauseMenuSelection(int32 Index);
	void ClickPauseItem(int32 Index);
	void SetPauseMenuOpen(bool bOpen);

	// ---- pause menu settings sub-page ----
	bool IsPauseSettingsOpen() const { return bPauseMenuOpen && bPauseSettingsOpen; }
	int32 GetPauseSettingsSelection() const { return PauseSettingsSelection; }
	/** Row ids of the settings sub-page: adjustable setting ids followed by "Back". */
	static const TArray<FName>& GetPauseSettingsRows();
	void SetPauseSettingsSelection(int32 Row);
	/** Mouse click on a settings row: Dir -1 / +1 changes the value (Back row returns to the pause list). */
	void ClickPauseSetting(int32 Row, int32 Dir);
	/** True if we have no living pawn (dead / waiting to respawn). */
	bool IsPawnDead() const;

	// ---- chat (ArenaChat.h): PlayerController.Say / TeamSay, Console.Talk / TeamTalk ----
	UFUNCTION(Exec)
	void Say(const FString& Msg);
	UFUNCTION(Exec)
	void TeamSay(const FString& Msg);
	UFUNCTION(Exec)
	void Talk();
	UFUNCTION(Exec)
	void TeamTalk();

	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;
	virtual void BeginPlay() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

protected:
	void CreateInputActions();
	// input handlers
	void OnMove(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnJumpPressed();
	void OnJumpReleased();
	void OnFirePressed();
	void OnFireReleased();
	void OnAltFirePressed();
	void OnAltFireReleased();
	void OnTapForward();
	void OnTapBack();
	void OnTapLeft();
	void OnTapRight();
	void OnNextWeapon();
	void OnPrevWeapon();
	void OnSlot(int32 Slot);
	/** UT2004 "SwitchWeapon 10" (Q): the translocator; pressed again while held: its beacon camera (TransLauncher.Reselect). */
	void OnTranslocatorKey();
	void OnScoreboardPressed();
	void OnScoreboardReleased();
	void OnPausePressed();
	void OnMenuUp();
	void OnMenuDown();
	void OnMenuAccept();
	void OnSuicide();
	void OnUse();
	void OnCrouchPressed();
	void OnCrouchReleased();
	void OnWalkPressed();
	void OnWalkReleased();
	void OnDodgePressed();
	void ActivatePauseItem(int32 Index);
	void OnMenuLeft();
	void OnMenuRight();
	void OnSensitivityDown();
	void OnSensitivityUp();
	void ChangePauseSetting(int32 Row, int32 Dir);
	void OpenPauseSettings(bool bOpen);
	void AdjustSensitivityHotkey(int32 Dir);
	void OnGamepadLook(const FInputActionValue& Value);
	/** True if pawn input should be ignored (pause menu open / match over). */
	bool IsPawnInputBlocked() const;
	class AArenaCharacter* GetArenaCharacter() const;
	void AddMappingContextToSubsystem();
	void ExpireMessages();
	float CurrentFOV = 0.f;

	UPROPERTY() TObjectPtr<UInputMappingContext> MappingContext;
	UPROPERTY() TMap<FName, TObjectPtr<UInputAction>> Actions;

	TArray<FArenaKillMessage> KillMessages;
	TArray<FArenaCenterMessage> CenterMessages;
	TArray<FArenaCenterMessage> LocalMessages;
	/** Match start: the countdown second / phase last seen (UT2004 StartupMessage stages are shown client side). */
	int32 LastCountdownSeen = -1;
	EArenaMatchPhase LastPhaseSeen = EArenaMatchPhase::Countdown;
	bool bPhaseSeen = false;
	void UpdateStartupMessages();
	/** Non-shipping test hook -AutoLocalMessages=<sec>: a kill and a death of this player at that world time. */
	void TickTestMessages();
	int32 TestMessageStep = 0;
	TArray<FDamageIndicator> DamageIndicators;
	FString PickupMessage;
	float PickupMessageTime = -100.f;
	float LastHitEnemyTime = -100.f;
	bool bLastHitHeadshot = false;
	bool bScoreboardHeld = false;
	bool bPauseMenuOpen = false;
	int32 PauseMenuSelection = 0;
	bool bPauseSettingsOpen = false;
	int32 PauseSettingsSelection = 0;
};
