// UT2004's in-game HUD (XInterface.HudCDeathmatch and its team subclasses) on the canvas: the widget values below are
// the stock class defaults (read from the install's XInterface.u while this was written), the drawing order and rules
// follow the classes' DrawHudPassA / DrawHudPassC / UpdateRankAndSpread / DrawWeaponBar / DrawTimer / DrawAdrenaline /
// DrawCrosshair / DrawSpectatingHud. Widget placement and scaling: ArenaHUDWidgets.h. Our own game data (character,
// player state, game state, rules) feeds the widgets.

#include "ArenaHUD.h"
#include "ArenaHUDWidgets.h"
#include "ArenaArtHUD.h"
#include "ArenaCharacter.h"
#include "ArenaGameRules.h"
#include "ArenaGameState.h"
#include "ArenaHudFonts.h"
#include "ArenaLocalMessages.h"
#include "ArenaPlayerController.h"
#include "ArenaPlayerState.h"
#include "Net/UE2NetDownload.h"
#include "Net/UE2NetJoin.h"
#include "Net/UE2NetScriptHUD.h"
#include "ArenaTypes.h"
#include "ArenaUserSettings.h"
#include "ArenaWeapon.h"
#include "ArenaTranslocator.h"

#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"


namespace ArenaHUDUTLocal
{
	using namespace ArenaHudUT;
	const FColor IconTint(255, 255, 255, 200);
	FColor Black150() { return FColor(0, 0, 0, 150); }

	// ---- XInterface.HudCDeathmatch defaultproperties ----
	struct FDMWidgets
	{
		FSpriteWidget HudBorderHealth = Spr(ETex::Hud, 0, 110, 166, 163, 0.53f, DP_LowerLeft, 0.f, 1.f, 0, 0);
		FSpriteWidget HudBorderShield = Spr(ETex::Hud, 0, 110, 166, 163, 0.53f, DP_LowerLeft, 0.f, 1.f, 0, -54);
		FSpriteWidget HudBorderAmmo = Spr(ETex::Hud, 0, 110, 166, 163, 0.53f, DP_LowerRight, 1.f, 1.f, 0, 0);
		FSpriteWidget HudBorderVehicleHealth = Spr(ETex::Hud, 0, 110, 166, 163, 0.53f, DP_LowerLeft, 0.139f, 1.f, 0, 0, FColor::White, FColor::White, EAnchor::Left);
		FSpriteWidget HudHealthALERT = Spr(ETex::HudPulse, 125, 458, 291, 511, 0.53f, DP_LowerLeft, 0.f, 1.f, 0, 0);
		FSpriteWidget HudVehicleHealthALERT = Spr(ETex::HudPulse, 125, 458, 291, 511, 0.53f, DP_LowerLeft, 0.139f, 1.f, 0, 0, FColor::White, FColor::White, EAnchor::Left);
		FSpriteWidget HudAmmoALERT = Spr(ETex::HudPulse, 125, 458, 291, 511, 0.53f, DP_LowerRight, 1.f, 1.f, 0, 0);
		FSpriteWidget HudBorderShieldIcon = Spr(ETex::Hud, 0, 248, 68, 310, 0.5f, DP_LowerLeft, 0.f, 1.f, 0, -55, IconTint, IconTint);
		FSpriteWidget HudBorderHealthIcon = Spr(ETex::Hud, 74, 165, 123, 216, 0.53f, DP_LowerLeft, 0.f, 1.f, 5, 0, IconTint, IconTint);
		FSpriteWidget HudBorderVehicleHealthIcon = Spr(ETex::Hud, 227, 406, 280, 448, 0.53f, DP_LowerLeft, 0.139f, 1.f, 5, 0, IconTint, IconTint, EAnchor::Left);
		/** WidgetTexture / TextureCoords come from the ammo class (IconMaterial, IconCoords) */
		FSpriteWidget AmmoIcon = Spr(ETex::Hud, 0, 0, 0, 0, 0.53f, DP_MiddleMiddle, 1.f, 1.f, -28, -30);
		FNumericWidget DigitsHealth = Num(0.49f, DP_MiddleRight, 0.f, 1.f, 174, -29);
		FNumericWidget DigitsVehicleHealth = Num(0.49f, DP_MiddleRight, 0.139f, 1.f, 174, -29, 0, false, EAnchor::Left);
		FNumericWidget DigitsAmmo = Num(0.49f, DP_MiddleLeft, 1.f, 1.f, -174, -29);
		FNumericWidget DigitsShield = Num(0.49f, DP_MiddleRight, 0.f, 1.f, 174, -83);
		FNumericWidget AdrenalineCount = Num(0.49f, DP_MiddleRight, 1.f, 0.f, -60, 35);
		FSpriteWidget AdrenalineIcon = Spr(ETex::Hud, 113, 38, 165, 106, 0.33f, DP_UpperRight, 1.f, 0.f, -15, 17);
		FSpriteWidget AdrenalineBackground = Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, DP_UpperRight, 1.f, 0.f, 0, 10, Black150(), Black150());
		FSpriteWidget AdrenalineBackgroundDisc = Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, DP_UpperRight, 1.f, 0.f, 0, 5);
		FSpriteWidget AdrenalineAlert = Spr(ETex::GlowPulse, 0, 0, 64, 64, 0.53f, DP_UpperRight, 1.f, 0.f, 6, 1, Colors::HighLight, Colors::HighLight);
		FSpriteWidget MyScoreBackground = Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, DP_UpperLeft, 0.f, 0.f, 0, 64, Black150(), Black150());
		FNumericWidget MyScore = Num(0.49f, DP_MiddleLeft, 0.015f, 0.f, 70, 94, 0, false, EAnchor::Left);
		FNumericWidget MyRank = Num(0.3f, DP_MiddleLeft, 0.f, 0.f, 30, 132);
		FNumericWidget MySpread = Num(0.3f, DP_MiddleLeft, 0.f, 0.f, 30, 170);
		FSpriteWidget TimerBackground = Spr(ETex::Hud, 168, 211, 334, 255, 0.4f, DP_UpperLeft, 0.f, 0.f, 60, 14, Black150(), Black150());
		FSpriteWidget TimerBackgroundDisc = Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, DP_UpperLeft, 0.f, 0.f, 0, 0);
		FSpriteWidget TimerIcon = Spr(ETex::Hud, 150, 356, 184, 389, 0.55f, DP_UpperLeft, 0.f, 0.f, 10, 9);
		FNumericWidget TimerHours = Num(0.32f, DP_MiddleLeft, 0.f, 0.f, 90, 45);
		FNumericWidget TimerMinutes = Num(0.32f, DP_MiddleLeft, 0.f, 0.f, 170, 45, 2, true);
		FNumericWidget TimerSeconds = Num(0.32f, DP_MiddleLeft, 0.f, 0.f, 250, 45, 2, true);
		FSpriteWidget TimerDigitSpacer[2] = {
			Spr(ETex::Hud, 495, 91, 503, 112, 0.4f, DP_MiddleLeft, 0.f, 0.f, 194, 36),
			Spr(ETex::Hud, 495, 91, 503, 112, 0.4f, DP_MiddleLeft, 0.f, 0.f, 130, 36),
		};
		FSpriteWidget UDamageIcon = Spr(ETex::Hud, 0, 164, 73, 246, 0.75f, DP_MiddleMiddle, 0.95f, 0.75f, 0, 0);
		FNumericWidget UDamageTime = Num(0.49f, DP_MiddleMiddle, 0.95f, 0.75f, 0, 0);
	};

	const FDMWidgets& DM()
	{
		static const FDMWidgets W;
		return W;
	}

	// ---- weapon bar: BarBorder[i] / BarBorderAmmoIndicator[i] / BarWeaponIcon[i], i = InventoryGroup - 1 ----
	constexpr int32 NumBarSlots = 9;
	const float BarPosX[NumBarSlots] = { 0.139f, 0.219f, 0.3f, 0.38f, 0.46f, 0.54f, 0.621f, 0.7f, 0.78f };
	const float BarBorderScaledPosition[NumBarSlots] = { 0.32f, 0.36f, 0.4f, 0.44f, 0.48f, 0.521f, 0.561f, 0.601f, 0.641f };
	/** default BarWeaponIcon TextureCoords and OffsetY (the stock weapon of each group: missing weapons show it in black) */
	struct FBarIcon
	{
		FIntBox Coords;
		int32 OffsetY;
	};
	const FBarIcon BarIcons[NumBarSlots] = {
		{ { 169, 39, 241, 77 }, -12 },
		{ { 245, 39, 329, 79 }, -8 },
		{ { 179, 127, 241, 175 }, 0 },
		{ { 250, 110, 330, 145 }, -12 },
		{ { 169, 78, 244, 124 }, -4 },
		{ { 246, 80, 332, 106 }, -18 },
		{ { 169, 172, 245, 208 }, -10 },
		{ { 253, 146, 333, 181 }, -13 },
		{ { 246, 182, 331, 210 }, -15 },
	};

	/** HudBase.Crosshairs: texture name and TextureScale of each stock crosshair */
	float CrosshairTextureScale(const FString& TexturePath)
	{
		if (TexturePath.Contains(TEXT("Pointer")) || TexturePath.Contains(TEXT("Bracket")))
		{
			return 0.6f;
		}
		if (TexturePath.Contains(TEXT("Triad")))
		{
			return 0.7f;
		}
		if (TexturePath.Contains(TEXT("Circle")))
		{
			return 0.4f;
		}
		return 0.75f;
	}

	int32 CurrentTeamIndex(const APlayerController* PC)
	{
		// HudCDeathmatch.UpdateHUD: TeamIndex = Clamp(team, 0, 1), 1 without a team
		const AArenaPlayerState* PS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
		return (PS && PS->Team == 0) ? 0 : 1;
	}

	/** DrawHUDAnimWidget: grows the widget for AnimTime / 2 after a pickup, then shrinks back. */
	void AnimWidget(FSpriteWidget& W, float DefaultScale, float PickupTime, float Now, float AnimTime = 0.6f, float AnimScale = 0.6f)
	{
		if (PickupTime > Now - AnimTime)
		{
			W.TextureScale = PickupTime > Now - AnimTime * 0.5f ? DefaultScale * (1.f + AnimScale * (Now - PickupTime))
				: DefaultScale * (1.f + AnimScale * (PickupTime + AnimTime - Now));
		}
		else
		{
			W.TextureScale = DefaultScale;
		}
	}

	/** DrawHUDAnimDigit: from the highlight colour back to the default after damage (per second, as in the script). */
	void AnimDigit(FNumericWidget& N, int32 TeamIndex, float PickupTime, float Now, const FColor& DefaultColor, const FColor& Highlight, float AnimTime = 0.8f)
	{
		if (PickupTime > Now - AnimTime)
		{
			const float T = Now - PickupTime;
			N.Tints[TeamIndex].R = uint8(FMath::Clamp(Highlight.R + (int32(DefaultColor.R) - Highlight.R) * T, 0.f, 255.f));
			N.Tints[TeamIndex].G = uint8(FMath::Clamp(Highlight.G + (int32(DefaultColor.G) - Highlight.G) * T, 0.f, 255.f));
			N.Tints[TeamIndex].B = uint8(FMath::Clamp(Highlight.B + (int32(DefaultColor.B) - Highlight.B) * T, 0.f, 255.f));
		}
		else
		{
			N.Tints[TeamIndex] = DefaultColor;
		}
	}

	FString UTString(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, const TCHAR* Fallback)
	{
		FString V;
		return ArenaLocalMessages::Localize(Package, Section, Key, V) ? V : FString(Fallback);
	}
}


// =============================================================================================
// Frame / state
// =============================================================================================

float AArenaHUD::UTRelativeYaw(const FVector& WorldLocation) const
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	FVector ViewLoc = FVector::ZeroVector;
	FRotator ViewRot = FRotator::ZeroRotator;
	if (PlayerOwner)
	{
		PlayerOwner->GetPlayerViewPoint(ViewLoc, ViewRot);
	}
	const FVector Dir = WorldLocation - ViewLoc;
	const float DirYaw = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
	return FMath::DegreesToRadians(FRotator::NormalizeAxis(DirYaw - ViewRot.Yaw));
}

void AArenaHUD::UpdateUTAnimState(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const float T = Now();
	if (Char != UTAnim.LastPawn)
	{
		UTAnim = FUTAnimState();
		UTAnim.LastPawn = Char;
		if (Char)
		{
			UTAnim.LastHealth = FMath::CeilToInt(Char->Health);
			UTAnim.LastShield = FMath::CeilToInt(Char->Shield);
			UTAnim.LastEnergy = FMath::FloorToInt(Char->Adrenaline);
			for (const AArenaWeapon* W : Char->GetWeapons())
			{
				const int32 G = W ? ArenaArtHUD::GetWeaponGroup(W->GetClass()) : 0;
				if (G >= 1 && G <= NumBarSlots)
				{
					UTAnim.bHasWeapon[G - 1] = true;
					UTAnim.bWeaponIconAnimDone[G - 1] = true;
				}
			}
		}
	}
	if (!Char)
	{
		return;
	}
	const int32 Health = FMath::CeilToInt(Char->Health);
	if (Health < UTAnim.LastHealth)
	{
		UTAnim.LastDamagedHealth = T;
	}
	else if (Health > UTAnim.LastHealth)
	{
		UTAnim.LastHealthPickupTime = T;
	}
	UTAnim.LastHealth = Health;
	const int32 Shield = FMath::CeilToInt(Char->Shield);
	if (Shield > UTAnim.LastShield)
	{
		UTAnim.LastArmorPickupTime = T;
	}
	UTAnim.LastShield = Shield;
	const int32 Energy = FMath::FloorToInt(Char->Adrenaline);
	if (Energy > UTAnim.LastEnergy)
	{
		UTAnim.LastAdrenalineTime = T;
	}
	UTAnim.LastEnergy = Energy;
	if (const AArenaPlayerController* PC = GetArenaPC())
	{
		float Age = 0.f;
		if (!PC->GetPickupMessage(Age).IsEmpty() && Age >= 0.f && Age < 0.5f)
		{
			UTAnim.LastPickupTime = T - Age;
		}
	}
}

// =============================================================================================
// HUD.PostRender
// =============================================================================================

void AArenaHUD::DrawUTHud(AArenaCharacter* Char, bool bAlive, bool bEnded, bool bScoreboard)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	UTFrame = FHudFrame(D, CurrentTeamIndex(PlayerOwner), Now());
	{
		// HudMutant.DrawHUD: the Mutant's HUD is tinted MutantHudTint (ModeFlags bit 0 = UArenaRules_MUT::FlagMutant)
		const AArenaGameState* MGS = GetArenaGS();
		const AArenaPlayerState* MPS = PlayerOwner ? PlayerOwner->GetPlayerState<AArenaPlayerState>() : nullptr;
		if (MGS && MPS && MGS->ModeId == TEXT("MUT") && (MPS->ModeFlags & 1) != 0)
		{
			UTFrame.HudRed = UTFrame.HudBlue = FColor(200, 0, 128, 100);
		}
	}
	// a modded UT2004 server's own HUD / scoreboard classes, drawn by their UnrealScript (Net/UE2NetScriptHUD.h); ours
	// draws what they leave out (messages, kill feed, chat, our notices) and every element they cannot draw
	FUE2NetScriptHUD::FDrawResult Script;
	if (FUE2NetScriptHUD* ScriptHud = FUE2NetScriptHUD::Get())
	{
		Script = ScriptHud->Draw(this, Canvas, bScoreboard);
	}
	if (bScoreboard)
	{
		// the HUD is not drawn under the scoreboard; ScoreBoard.bDisplayMessages shows the console messages
		if (!Script.bScoreboard)
		{
			DrawUTScoreboard();
		}
		DrawKillFeed();
		return;
	}
	const APawn* Pawn = GetOwningPawn();
	const AArenaPlayerState* MyPS = PlayerOwner ? PlayerOwner->GetPlayerState<AArenaPlayerState>() : nullptr;
	if (Script.bHud)
	{
		// the server's HUD drew the player HUD, crosshair, score pass and spectator bar; the pickup messages (its
		// LocalizedMessage list is not fed) and the join notice / bar text stay ours
		if (bAlive && !bEnded)
		{
			DrawUTPickupMessage();
		}
		else if (!bEnded)
		{
			const FString Bar = ArenaNet::GetSpectatorBarText(GetWorld());
			if (!Bar.IsEmpty())
			{
				const int32 Font = ArenaHudFonts::FontSizeIndex(D.UTClipX(), -1);
				TArray<FString> Lines;
				Bar.ParseIntoArray(Lines, TEXT("\n"));
				float Y = UTFrame.ScreenY(0.8f);
				for (const FString& Line : Lines)
				{
					const FVector2D S = D.TextSizeUT(Font, Line);
					D.TextUT(Font, Line, D.W() * 0.5f - S.X * 0.5f, Y, FLinearColor(FColor(255, 255, 255)), EArenaAlign::Left, true);
					Y += S.Y;
				}
			}
		}
	}
	else if (bAlive && !bEnded)
	{
		DrawUTDamageIndicators(Char);
		DrawUTPlayerHud(Char);
		DrawUTPickupMessage();
	}
	else if (Pawn && !Char && !bEnded)
	{
		// driving (an Onslaught vehicle pawn): the rules draw the vehicle's health; the score pass and timer stay
		UpdateUTAnimState(nullptr);
		DrawUTScorePass();
		const AArenaGameState* VGS = GetArenaGS();
		if (!(VGS && VGS->ModeId == TEXT("AS")))
		{
			DrawUTTimer();
		}
	}
	else
	{
		UpdateUTAnimState(nullptr);
		// after the match the end-of-match banner (ArenaMidGameMenu, DrawEndBanner) takes the bar's place
		if (!bEnded)
		{
			DrawUTSpectatingHud();
		}
		// HudCTeamDeathMatch.DrawSpectatingHud: team scores only for pure spectators
		const AArenaGameState* GS = GetArenaGS();
		if (GS && GS->IsTeamGame() && MyPS && MyPS->IsSpectator())
		{
			DrawUTScorePass();
		}
	}
	if (UArenaGameRules* Rules = GetRules(); Rules && !Script.bHud)
	{
		Rules->DrawHUD(this, Canvas);
	}
	DrawKillFeed();
	DrawUTCenterMessages();
	DrawLocalMessages();
	if (const FString ModStatus = ArenaNet::GetModStatusText(GetWorld()); !ModStatus.IsEmpty())
	{
		// a server mod's state (UTComp warmup and ready-up), near the top like its own overlay
		const int32 Font = ArenaHudFonts::FontSizeIndex(D.UTClipX(), 1);
		const FVector2D S = D.TextSizeUT(Font, ModStatus);
		D.TextUT(Font, ModStatus, D.W() * 0.5f - S.X * 0.5f, UTFrame.ScreenY(0.18f), FLinearColor(FColor(255, 255, 0)));
	}
}

void AArenaHUD::DrawUTCenterMessages()
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const AArenaPlayerController* PC = GetArenaPC();
	if (!PC)
	{
		return;
	}
	// Gameplay.CriticalEventPlus: DrawColor 0,160,255, FontSize 1, PosX 0.5, PosY 0.83, DP_MiddleMiddle, 3 s, fading
	const int32 Font = ArenaHudFonts::FontSizeIndex(D.UTClipX(), 1);
	const float T = Now();
	float Y = UTFrame.ScreenY(0.83f);
	for (const FArenaCenterMessage& M : PC->GetCenterMessages())
	{
		const float Age = T - M.Time;
		const float Life = FMath::Max(0.1f, M.Duration);
		if (Age < 0.f || Age > Life)
		{
			continue;
		}
		const float Alpha = FMath::Clamp((Life - Age) / Life, 0.f, 1.f);
		const FVector2D S = D.TextSizeUT(Font, M.Text);
		D.TextUT(Font, M.Text, D.W() * 0.5f - S.X * 0.5f, Y - S.Y * 0.5f, FArenaDraw::UTColor(FColor(0, 160, 255, 255), Alpha));
		Y += S.Y * HudCanvasScale;
	}
}

void AArenaHUD::DrawUTPickupMessage()
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const AArenaPlayerController* PC = GetArenaPC();
	if (!PC)
	{
		return;
	}
	// UnrealGame.PickupMessagePlus: white, FontSize 0, PosY 0.9, DP_MiddleMiddle, 3 s, fading
	float Age = 0.f;
	const FString Msg = PC->GetPickupMessage(Age);
	const float Life = 3.f;
	if (Msg.IsEmpty() || Age < 0.f || Age > Life)
	{
		return;
	}
	const int32 Font = ArenaHudFonts::FontSizeIndex(D.UTClipX(), 0);
	const FVector2D S = D.TextSizeUT(Font, Msg);
	D.TextUT(Font, Msg, D.W() * 0.5f - S.X * 0.5f, UTFrame.ScreenY(0.9f) - S.Y * 0.5f, FArenaDraw::UTColor(FColor::White, (Life - Age) / Life));
}

// =============================================================================================
// HudCDeathmatch.DrawHudPassA / DrawHudPassC for a living player
// =============================================================================================

void AArenaHUD::DrawUTPlayerHud(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	UpdateUTAnimState(Char);
	// pass A
	// HUD_Assault overrides DrawTimer and DrawAdrenaline with nothing (its round time pod replaces the timer)
	const AArenaGameState* GS = GetArenaGS();
	const bool bAssault = GS && GS->ModeId == TEXT("AS");
	DrawUTAmmo(Char);
	DrawUTWeaponBar(Char);
	DrawUTVitals(Char);
	if (!bAssault)
	{
		DrawUTAdrenaline(Char);
	}
	DrawUTScorePass();
	DrawUTUDamage(Char);
	if (!bAssault)
	{
		DrawUTTimer();
	}
	// pass C: the crosshair (weapon infos of the stock weapons are not drawn)
	const AArenaPlayerController* PC = GetArenaPC();
	if (!(PC && PC->IsScoreboardVisible()) && !bTestScoreboard)
	{
		DrawUTCrosshair(Char);
	}
}

void AArenaHUD::DrawUTAmmo(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const FDMWidgets& Def = DM();
	const FHudFrame& F = UTFrame;
	const AArenaWeapon* Weapon = Char ? Char->GetCurrentWeapon() : nullptr;
	if (!Weapon)
	{
		return;
	}
	// HudCDeathmatch.DrawChargeBar for weapons with bShowChargingBar (the translocator): a team box at the lower right
	// and RechargeBar (HUD 0,313-170,348, SM_Left) cropped to Weapon.ChargeBar()
	if (const AWeap_Translocator* Trans = Cast<AWeap_Translocator>(Weapon))
	{
		const float SF = F.HudScale * 0.135f * F.W;
		const FColor BoxColor = F.TeamIndex == 0 ? F.HudRed : F.HudBlue;
		DrawTile(*F.D, ETex::Hud, F.W - SF - 0.0011f * F.HudScale * F.W, (1.f - 0.0975f * F.HudScale) * F.H, SF, 0.223f * SF, 0.f, 110.f, 166.f, 53.f,
			FLinearColor(BoxColor));
		FSpriteWidget Recharge = Spr(ETex::Hud, 0, 313, 170, 348, 0.5f, DP_UpperRight, 1.f, 1.f, 0, -93, FColor(255, 255, 255, 220), FColor(255, 255, 255, 220));
		Recharge.ScaleMode = SM_Left;
		Recharge.Scale = Trans->GetChargeBar();
		if (Recharge.Scale > 0.f)
		{
			DrawSprite(F, Recharge);
		}
	}
	FSpriteWidget Border = Def.HudBorderAmmo;
	Border.Tints[0] = F.HudRed;
	Border.Tints[1] = F.HudBlue;
	DrawSprite(F, Border);
	if (const ArenaArtHUD::FHudSprite* Icon = ArenaArtHUD::GetAmmoIcon(Weapon->GetClass()))
	{
		FSpriteWidget AmmoIcon = Def.AmmoIcon;
		AmmoIcon.Coords = FIntBox{ int32(Icon->X1), int32(Icon->Y1), int32(Icon->X2), int32(Icon->Y2) };
		const float Frac = Weapon->bInfiniteAmmo ? 1.f : Weapon->GetAmmoFraction();
		if (Frac < 0.15f)
		{
			FSpriteWidget Alert = Def.HudAmmoALERT;
			Alert.Tints[F.TeamIndex] = Colors::Team(F.TeamIndex);
			DrawSprite(F, Alert);
			// IconFlashMaterial (none for the stock ammo) -> HUDContent.Generic.HUDPulse
			AmmoIcon.Tex = ETex::HudPulse;
		}
		DrawSprite(F, AmmoIcon);
	}
	FNumericWidget Digits = Def.DigitsAmmo;
	Digits.Value = Weapon->bInfiniteAmmo ? 1 : Weapon->Ammo; // CalculateAmmo: 1 / 1 without an ammo count
	DrawNumeric(F, Digits);
}

void AArenaHUD::DrawUTVitals(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const FDMWidgets& Def = DM();
	const FHudFrame& F = UTFrame;
	const float T = Now();
	const int32 TI = F.TeamIndex;

	// ---- health ----
	FSpriteWidget HealthIcon = Def.HudBorderHealthIcon;
	AnimWidget(HealthIcon, Def.HudBorderHealthIcon.TextureScale, UTAnim.LastHealthPickupTime, T);
	FSpriteWidget HealthBox = Def.HudBorderHealth;
	HealthBox.Tints[0] = F.HudRed;
	HealthBox.Tints[1] = F.HudBlue;
	DrawSprite(F, HealthBox);
	const int32 CurHealth = FMath::CeilToInt(FMath::Max(0.f, Char->Health));
	if (float(CurHealth) / ArenaTuning::MaxHealth < 0.26f)
	{
		FSpriteWidget Alert = Def.HudHealthALERT;
		Alert.Tints[TI] = Colors::Team(TI);
		DrawSprite(F, Alert);
		HealthIcon.Tex = ETex::HudPulse;
	}
	DrawSprite(F, HealthIcon);
	FNumericWidget Digits = Def.DigitsHealth;
	AnimDigit(Digits, TI, UTAnim.LastDamagedHealth, T, Def.DigitsHealth.Tints[TI], Colors::HighLight);
	Digits.Value = CurHealth;
	if (CurHealth > 999)
	{
		Digits.OffsetX = 220;
		Digits.OffsetY = -35;
		Digits.TextureScale = 0.39f;
	}
	DrawNumeric(F, Digits);

	// ---- shield (only while there is one) ----
	const int32 CurShield = FMath::CeilToInt(FMath::Max(0.f, Char->Shield));
	if (CurShield > 0)
	{
		FSpriteWidget ShieldBox = Def.HudBorderShield;
		ShieldBox.Tints[0] = F.HudRed;
		ShieldBox.Tints[1] = F.HudBlue;
		DrawSprite(F, ShieldBox);
		FSpriteWidget ShieldIcon = Def.HudBorderShieldIcon;
		AnimWidget(ShieldIcon, Def.HudBorderShieldIcon.TextureScale, UTAnim.LastArmorPickupTime, T);
		DrawSprite(F, ShieldIcon);
		FNumericWidget ShieldDigits = Def.DigitsShield;
		ShieldDigits.Value = CurShield;
		DrawNumeric(F, ShieldDigits);
	}
}

void AArenaHUD::DrawUTAdrenaline(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const FDMWidgets& Def = DM();
	const FHudFrame& F = UTFrame;
	DrawSprite(F, Def.AdrenalineBackground);
	DrawSprite(F, Def.AdrenalineBackgroundDisc);
	const int32 CurEnergy = FMath::Clamp(FMath::FloorToInt(Char->Adrenaline), 0, int32(ArenaTuning::MaxAdrenaline));
	if (CurEnergy >= int32(ArenaTuning::MaxAdrenaline))
	{
		DrawSprite(F, Def.AdrenalineAlert);
	}
	FSpriteWidget Icon = Def.AdrenalineIcon;
	AnimWidget(Icon, Def.AdrenalineIcon.TextureScale, UTAnim.LastAdrenalineTime, Now());
	DrawSprite(F, Icon);
	FNumericWidget Count = Def.AdrenalineCount;
	Count.Value = CurEnergy;
	DrawNumeric(F, Count);
}

void AArenaHUD::DrawUTTimer()
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const AArenaGameState* GS = GetArenaGS();
	if (!GS)
	{
		return;
	}
	const FDMWidgets& Def = DM();
	const FHudFrame& F = UTFrame;
	// GRI.TimeLimit != 0: remaining time, else the elapsed time
	int32 Seconds = GS->TimeLimit > 0 ? GS->RemainingTime : FMath::FloorToInt(GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f);
	DrawSprite(F, Def.TimerBackground);
	DrawSprite(F, Def.TimerBackgroundDisc);
	DrawSprite(F, Def.TimerIcon);
	FNumericWidget Minutes = Def.TimerMinutes;
	FNumericWidget Secs = Def.TimerSeconds;
	FSpriteWidget Spacer0 = Def.TimerDigitSpacer[0];
	FSpriteWidget Spacer1 = Def.TimerDigitSpacer[1];
	Minutes.OffsetX = Def.TimerMinutes.OffsetX - 80;
	Secs.OffsetX = Def.TimerSeconds.OffsetX - 80;
	if (Seconds > 3600)
	{
		FNumericWidget Hours = Def.TimerHours;
		Hours.Value = Seconds / 3600;
		Seconds -= Hours.Value * 3600;
		DrawNumeric(F, Hours);
		if (Hours.Value > 9)
		{
			Minutes.OffsetX = Def.TimerMinutes.OffsetX;
			Secs.OffsetX = Def.TimerSeconds.OffsetX;
		}
		else
		{
			Minutes.OffsetX = Def.TimerMinutes.OffsetX - 40;
			Secs.OffsetX = Def.TimerSeconds.OffsetX - 40;
			Spacer0.OffsetX = Def.TimerDigitSpacer[0].OffsetX - 32;
			Spacer1.OffsetX = Def.TimerDigitSpacer[1].OffsetX - 32;
		}
		DrawSprite(F, Spacer0);
	}
	DrawSprite(F, Spacer1);
	Minutes.Value = FMath::Min(Seconds / 60, 60);
	Secs.Value = FMath::Min(Seconds % 60, 60);
	DrawNumeric(F, Minutes);
	DrawNumeric(F, Secs);
}

void AArenaHUD::DrawUTUDamage(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	if (!Char || !Char->HasDoubleDamage())
	{
		return;
	}
	const FDMWidgets& Def = DM();
	const float Remaining = Char->GetDoubleDamageRemaining();
	FSpriteWidget Icon = Def.UDamageIcon;
	if (Remaining > 15.f)
	{
		Icon.TextureScale = Def.UDamageIcon.TextureScale * FMath::Min(Remaining * 0.0333f, 1.f);
	}
	DrawSprite(UTFrame, Icon);
	FNumericWidget Time = Def.UDamageTime;
	Time.Value = int32(Remaining);
	DrawNumeric(UTFrame, Time);
}

void AArenaHUD::DrawUTRankAndSpread(int32 Score, int32 Spread, int32 Rank, bool bNoBackground)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const FDMWidgets& Def = DM();
	const FHudFrame& F = UTFrame;
	if (!bNoBackground)
	{
		DrawSprite(F, Def.MyScoreBackground);
	}
	FNumericWidget S = Def.MyScore;
	S.Value = FMath::Min(Score, 999);
	DrawNumeric(F, S);
	if (F.W >= 640.f)
	{
		FNumericWidget Sp = Def.MySpread;
		Sp.Value = FMath::Min(Spread, 999);
		DrawNumeric(F, Sp);
	}
	FNumericWidget R = Def.MyRank;
	R.Value = Rank;
	DrawNumeric(F, R);
}

void AArenaHUD::DrawUTScorePass()
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	AArenaGameState* GS = GetArenaGS();
	const APlayerState* MyPS = PlayerOwner ? PlayerOwner->PlayerState : nullptr;
	if (!GS)
	{
		return;
	}
	if (UArenaGameRules* Rules = GetRules())
	{
		if (Rules->DrawScoreInfo(this, Canvas))
		{
			return;
		}
	}
	if (GS->IsTeamGame())
	{
		DrawUTTeamScores();
		return;
	}
	if (!MyPS)
	{
		return;
	}
	if (GS->ModeId == TEXT("LMS"))
	{
		// HudLMS.UpdateRankAndSpread: no capsule; rank = lives left, spread = players still in
		const AArenaPlayerState* APS = Cast<AArenaPlayerState>(MyPS);
		int32 Remaining = 0;
		for (const AArenaPlayerState* P : GS->GetSortedPlayers())
		{
			Remaining += (P && !P->bEliminated) ? 1 : 0;
		}
		DrawUTRankAndSpread(FMath::RoundToInt(MyPS->GetScore()), Remaining, APS ? FMath::Max(0, APS->Lives) : 0, true);
		return;
	}
	// HudCDeathmatch.UpdateRankAndSpread: rank in the sorted list, spread to the best other player (or the leader)
	const TArray<AArenaPlayerState*> Sorted = GS->GetSortedPlayers();
	const int32 MyScore = FMath::RoundToInt(MyPS->GetScore());
	int32 Rank = 0;
	for (int32 i = 0; i < Sorted.Num(); ++i)
	{
		if (Sorted[i] == MyPS)
		{
			Rank = i + 1;
			break;
		}
	}
	int32 Spread = 0;
	if (Sorted.Num() > 0 && Sorted[0] == MyPS)
	{
		Spread = Sorted.Num() > 1 && Sorted[1] ? MyScore - FMath::RoundToInt(Sorted[1]->GetScore()) : 0;
	}
	else if (Sorted.Num() > 0 && Sorted[0])
	{
		Spread = MyScore - FMath::RoundToInt(Sorted[0]->GetScore());
	}
	DrawUTRankAndSpread(MyScore, Spread, Rank);
}

// =============================================================================================
// HudCDeathmatch.DrawWeaponBar
// =============================================================================================

void AArenaHUD::DrawUTWeaponBar(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const FHudFrame& F = UTFrame;
	const int32 TI = F.TeamIndex;
	const float T = Now();
	const AArenaWeapon* Weapons[NumBarSlots] = {};
	const AArenaWeapon* Pending = Char->GetCurrentWeapon();
	for (const AArenaWeapon* W : Char->GetWeapons())
	{
		const int32 G = W ? ArenaArtHUD::GetWeaponGroup(W->GetClass()) : 0;
		// InventoryGroup 1..9 -> slot 0..8 (0 -> 8); others (ball launcher, group 15) are not on the bar
		const int32 Pos = G == 0 ? 8 : G - 1;
		if (W && G < 10 && ArenaArtHUD::GetWeaponIcon(W->GetClass()) && !Weapons[Pos])
		{
			Weapons[Pos] = W;
		}
	}
	if (Pending)
	{
		const int32 G = ArenaArtHUD::GetWeaponGroup(Pending->GetClass());
		if (G < 10 && ArenaArtHUD::GetWeaponIcon(Pending->GetClass()))
		{
			Weapons[G == 0 ? 8 : G - 1] = Pending;
		}
	}
	const float HudMinScale = 0.5f;
	const float HudScaleOffset = 1.f - (F.HudScale - HudMinScale) / HudMinScale;
	for (int32 i = 0; i < NumBarSlots; ++i)
	{
		const AArenaWeapon* W = Weapons[i];
		FSpriteWidget Border = Spr(ETex::Hud, 0, 39, 94, 93, 0.53f, DP_LowerLeft, 0.f, 1.f, 0, 0, F.HudRed, F.HudBlue, EAnchor::Center);
		Border.PosX = BarPosX[i] + (BarBorderScaledPosition[i] - BarPosX[i]) * HudScaleOffset;
		const float IconOffset = 94.f * 0.5f;
		FSpriteWidget Icon = Spr(ETex::Hud, BarIcons[i].Coords.X1, BarIcons[i].Coords.Y1, BarIcons[i].Coords.X2, BarIcons[i].Coords.Y2, 0.53f,
			DP_LowerMiddle, Border.PosX, 1.f, int32(IconOffset), BarIcons[i].OffsetY, FColor::White, FColor::White, EAnchor::Center);
		if (!W)
		{
			UTAnim.bHasWeapon[i] = false;
			// bShowMissingWeaponInfo: the empty slot with the group's stock icon in black
			Icon.Tints[TI] = Colors::HudBlack;
			UTAnim.bWeaponIconAnimDone[i] = false;
			DrawSprite(F, Border);
			DrawSprite(F, Icon);
			continue;
		}
		if (!UTAnim.bHasWeapon[i])
		{
			UTAnim.WeaponPickupTimer[i] = T;
			UTAnim.bHasWeapon[i] = true;
		}
		FSpriteWidget Ammo = Spr(ETex::Hud, 0, 94, 94, 109, 0.53f, DP_LowerLeft, Border.PosX, 1.f, 0, 0, IconTint, IconTint, EAnchor::Center);
		Ammo.ScaleMode = SM_Right;
		Ammo.Scale = W->bInfiniteAmmo ? 1.f : W->GetAmmoFraction(); // Weapon.AmmoStatus
		const FArenaNetStandIn& Mod = W->GetNetStandIn();
		if (Mod.bIcon)
		{
			// a mod weapon's own IconCoords on the stock HUD atlas or on its own texture (Net/UE2NetModWeapons.h)
			const FVector4f& C = Mod.IconCoords;
			Icon.Coords = FIntBox{ int32(C.X), int32(C.Y), int32(C.Z), int32(C.W) };
		}
		else if (const ArenaArtHUD::FHudSprite* WI = ArenaArtHUD::GetWeaponIcon(W->GetClass()))
		{
			Icon.Coords = FIntBox{ int32(WI->X1), int32(WI->Y1), int32(WI->X2), int32(WI->Y2) };
		}
		Icon.Tints[TI] = Colors::Normal;
		if (!UTAnim.bWeaponIconAnimDone[i])
		{
			const float Since = T - UTAnim.WeaponPickupTimer[i];
			if (Since < 0.6f)
			{
				const float K = Since < 0.3f ? Since : 0.6f - Since;
				Icon.TextureScale = 0.53f * (1.f + 1.3f * K);
				Icon.OffsetX = int32(IconOffset - IconOffset * K);
			}
			else
			{
				UTAnim.bWeaponIconAnimDone[i] = true;
			}
		}
		if (W == Pending)
		{
			Border.Tints[TI] = Colors::HighLight;
			Border.OffsetY = -10;
			Ammo.OffsetY = -10;
			Icon.OffsetY += -10;
		}
		DrawSprite(F, Border);
		DrawSprite(F, Ammo);
		UTexture2D* ModIcon = Mod.bIcon && !Mod.IconTexture.IsEmpty() ? ArenaArtHUD::GetTexture(*Mod.IconTexture) : nullptr;
		if (ModIcon && F.D)
		{
			const FRect R = SpriteRect(F, Icon);
			DrawTilePath(*F.D, *Mod.IconTexture, float(FMath::Max(1, ModIcon->GetSizeX())), float(FMath::Max(1, ModIcon->GetSizeY())), R.X, R.Y, R.W, R.H,
				float(Icon.Coords.X1), float(Icon.Coords.Y1), float(Icon.Coords.X2 - Icon.Coords.X1), float(Icon.Coords.Y2 - Icon.Coords.Y1),
				FLinearColor(Icon.Tints[F.TeamIndex]));
		}
		else
		{
			DrawSprite(F, Icon);
		}
	}
}

// =============================================================================================
// HudCDeathmatch.DrawCrosshair
// =============================================================================================

void AArenaHUD::DrawUTCrosshair(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	// CrosshairStyle (Settings > Crosshair; 0 = the first stock crosshair), CrossHairColor white, CrosshairScale 1
	const int32 Choice = FMath::Max(1, UArenaUserSettings::Get()->Crosshair);
	const FString Path = ArenaArtHUD::GetCrosshairTexture(Choice);
	if (Path.IsEmpty() || !ArenaArtHUD::GetTexture(*Path))
	{
		return;
	}
	float TS = CrosshairTextureScale(Path) * 0.5f;
	// the crosshair pulses for 0.4 s after a pickup
	const float T = Now();
	if (UTAnim.LastPickupTime > T - 0.4f)
	{
		TS *= UTAnim.LastPickupTime > T - 0.2f ? 1.f + 5.f * (T - UTAnim.LastPickupTime) : 1.f + 5.f * (UTAnim.LastPickupTime + 0.4f - T);
	}
	// drawn with HudScale 1, centred (DP_MiddleMiddle at 0.5, 0.5)
	FHudFrame F = UTFrame;
	F.HudScale = 1.f;
	F.TotalScale = HudCanvasScale * F.ResScale;
	const float Size = 65.f * TS * F.TotalScale;
	const float X = FMath::RoundToFloat(F.W * 0.5f - Size * 0.5f);
	const float Y = FMath::RoundToFloat(F.H * 0.5f - Size * 0.5f);
	DrawTilePath(D, *Path, 64.f, 64.f, X, Y, Size, Size, 0.f, 0.f, 64.f, 64.f, FLinearColor::White);
}

// =============================================================================================
// HudCTeamDeathMatch.ShowTeamScorePassA
// =============================================================================================

void AArenaHUD::DrawUTTeamScores(bool bSymbols, bool bVersus)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const AArenaGameState* GS = GetArenaGS();
	if (!GS)
	{
		return;
	}
	const FHudFrame& F = UTFrame;
	const FColor Black = Black150();
	const FSpriteWidget Bg[2] = {
		Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, DP_UpperRight, 0.5f, 0.f, -50, 10, Black, Black),
		Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, DP_UpperLeft, 0.5f, 0.f, 50, 10, Black, Black),
	};
	const FSpriteWidget Disc[2] = {
		Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, DP_UpperRight, 0.5f, 0.f, -35, 5),
		Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, DP_UpperLeft, 0.5f, 0.f, 35, 5),
	};
	DrawSprite(F, Bg[0]);
	DrawSprite(F, Bg[1]);
	DrawSprite(F, Disc[0]);
	DrawSprite(F, Disc[1]);
	if (bSymbols)
	{
		// TeamSymbols[i]: GRI.TeamSymbols[i] (256 x 256), TextureScale 0.1; Tints[i] = HudColorTeam[i] (UpdateTeamHud)
		const FColor SymbolTints[2][2] = { { Colors::HudRed, FColor(255, 32, 32, 200) }, { FColor(0, 128, 255, 200), Colors::HudBlue } };
		for (int32 Team = 0; Team < 2; ++Team)
		{
			const FString& Symbol = GS->GetTeamSymbol(Team);
			if (Symbol.IsEmpty())
			{
				continue;
			}
			const float S = 0.1f * F.TotalScale;
			const float Size = 257.f * S;
			float X = F.ScreenX(0.5f) + (Team == 0 ? -200.f : 200.f) * S;
			const float Y = F.ScreenY(0.f) + 45.f * S;
			if (Team == 0)
			{
				X -= Size; // DP_UpperRight
			}
			DrawTilePath(D, *Symbol, 256.f, 256.f, X, Y, Size, Size, 0.f, 0.f, 256.f, 256.f, FLinearColor(SymbolTints[Team][F.TeamIndex]));
		}
	}
	if (bVersus)
	{
		DrawSprite(F, Spr(ETex::Hud, 435, 0, 508, 31, 0.4f, DP_UpperMiddle, 0.5f, 0.f, 0, 30));
	}
	FNumericWidget Score[2] = { Num(0.54f, DP_MiddleRight, 0.5f, 0.f, -90, 32, 1), Num(0.54f, DP_MiddleLeft, 0.5f, 0.f, 90, 32, 1) };
	for (int32 Team = 0; Team < 2; ++Team)
	{
		Score[Team].Value = FMath::Min(GS->TeamScores[Team], 999);
		DrawNumeric(F, Score[Team]);
	}
}

// =============================================================================================
// HudCDeathmatch.DrawSpectatingHud: the bar at the bottom (dead, spectating, match over)
// =============================================================================================

void AArenaHUD::DrawUTSpectatingHud()
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const AArenaGameState* GS = GetArenaGS();
	AArenaPlayerController* PC = GetArenaPC();
	const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	const int32 MedFont = ArenaHudFonts::MediumFontIndex(D.UTClipX());
	const int32 ConFont = ArenaHudFonts::ConsoleFontIndex(D.UTClipX());
	const float MedH = D.TextSizeUT(MedFont, TEXT("W")).Y;
	float SmallH = D.TextSizeUT(ConFont, TEXT("W")).Y;
	const float Full = MedH + SmallH;
	const float Px = float(UIPixelScale(D.H()));
	const float Top = D.H() - 8.f * Px - Full;
	const float Scale = (Full + 16.f * Px) / 128.f;
	// InterfaceContent.Menu.SquareBoxA across the screen with ColorModulate (255, 255, 0): its dark blue fill turns black,
	// its light frame yellow
	// across the HUD region (the whole width unless Settings > HUD Width narrows it)
	const float BarX = UTFrame.RegionX0, BarW = UTFrame.RegionW;
	DrawTileStretched(D, TEXT("Textures/InterfaceContent/Menu_SquareBoxA"), BarX, Top, BarW, Full, FLinearColor(1.f, 1.f, 0.f, 1.f));
	const float TextTop = Top + 4.f * Px;
	// a UT2004 server join's notice (packages left out: Net/UE2NetDownload.h), above the bar in every spectating view
	if (GetWorld() && GetWorld()->URL.HasOption(TEXT("UE2Net")))
	{
		if (const FString& Notice = UE2NetDownload::GetJoinNotice(); !Notice.IsEmpty())
		{
			// the medium font when the line fits the screen, else the console font; on a dark band so it reads over the world
			const int32 NoticeFont = D.TextSizeUT(MedFont, Notice).X <= D.W() * 0.96f ? MedFont : ConFont;
			const FVector2D NS = D.TextSizeUT(NoticeFont, Notice);
			const float NoticeY = Top - 6.f * Px - 2.f * NS.Y;
			const float NoticeX = FMath::Max(BarX, D.W() * 0.5f - NS.X * 0.5f);
			D.Rect(NoticeX - 6.f * Px, NoticeY - 2.f * Px, NS.X + 12.f * Px, NS.Y + 4.f * Px, FLinearColor(0.f, 0.f, 0.f, 0.6f));
			D.TextUT(NoticeFont, Notice, NoticeX, NoticeY, FLinearColor(FColor(255, 200, 64)));
		}
	}
	// the LastManStanding logo at the left end of the bar is not drawn (logos are never used); its room is kept
	const float TextX = BarX + 256.f * Scale * 0.75f;

	const bool bEnded = GS && GS->Phase == EArenaMatchPhase::Ended;
	const APawn* ViewPawn = PC ? Cast<APawn>(PC->GetViewTarget()) : nullptr;
	const APlayerState* ViewPS = ViewPawn ? ViewPawn->GetPlayerState() : nullptr;
	if (bEnded)
	{
		// bDisplayWinner / bDisplayLoser
		FString Info;
		const bool bWon = GS->IsTeamGame() ? (MyPS && MyPS->Team == GS->WinningTeam) : (MyPS && GS->WinningPlayer == MyPS);
		if (bWon)
		{
			Info = UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("YouveWonTheMatch"), TEXT("You've won the match!"));
		}
		else if (!GS->IsTeamGame() && GS->WinningPlayer)
		{
			Info = GS->WinningPlayer->GetPlayerName() + UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("WonMatchPostFix"), TEXT(" won the match!"));
		}
		else
		{
			Info = UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("YouveLostTheMatch"), TEXT("You've lost the match."));
		}
		const FVector2D S = D.TextSizeUT(MedFont, Info);
		D.TextUT(MedFont, Info, D.W() * 0.5f - S.X * 0.5f, Top + Full * 0.5f - S.Y * 0.5f, FLinearColor::White);
		return;
	}
	if (ViewPS && ViewPS != MyPS)
	{
		// spectating another player: "Now viewing" (console font, green) and the name (medium font, yellow), score right
		if (D.W() < 640.f)
		{
			SmallH = 0.f;
		}
		else
		{
			D.TextUT(ConFont, UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("NowViewing"), TEXT("Now viewing")), TextX, TextTop,
				FLinearColor(FColor(32, 255, 32)));
			const FString ScoreText = UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("ScoreText"), TEXT("Score"));
			D.TextUT(ConFont, ScoreText, BarX + BarW - 5.f * Px, TextTop, FLinearColor(FColor(32, 255, 32)), EArenaAlign::Right);
		}
		D.TextUT(MedFont, ViewPS->GetPlayerName(), TextX, TextTop + SmallH, FLinearColor(FColor(255, 255, 0)));
		D.TextUT(MedFont, FString::FromInt(FMath::RoundToInt(ViewPS->GetScore())), BarX + BarW - 5.f * Px, TextTop + SmallH,
			FLinearColor(FColor(255, 255, 0)), EArenaAlign::Right);
		// following a player on a UT2004 server: what the server replicates about the pawn to us (Pawn.Health; its
		// ammo, weapons and adrenaline go to its owner only), in the bar's style left of the score
		const AArenaCharacter* ViewChar = Cast<AArenaCharacter>(ViewPawn);
		if (ViewChar && GetWorld() && GetWorld()->URL.HasOption(TEXT("UE2Net")) && ViewChar->Health > 0.f)
		{
			const float HX = BarX + BarW - 5.f * Px - D.TextSizeUT(MedFont, TEXT("00000")).X - 24.f * Px;
			if (SmallH > 0.f)
			{
				D.TextUT(ConFont, TEXT("Health"), HX, TextTop, FLinearColor(FColor(32, 255, 32)), EArenaAlign::Right);
			}
			D.TextUT(MedFont, FString::FromInt(FMath::CeilToInt(ViewChar->Health)), HX, TextTop + SmallH,
				FLinearColor(ViewChar->Health < 26.f ? FColor(255, 64, 32) : FColor(255, 255, 0)), EArenaAlign::Right);
		}
		const FString Tag = UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("InitialViewingString"), TEXT("Press Fire to View a different Player"));
		const FVector2D TS = D.TextSizeUT(ConFont, Tag);
		D.TextUT(ConFont, Tag, D.W() * 0.5f - TS.X * 0.5f, Top - 3.f * Px - TS.Y, FLinearColor::White);
		return;
	}
	// HudCDeathmatch.GetInfoString
	FString Info;
	if (const FString NetInfo = ArenaNet::GetSpectatorBarText(GetWorld()); !NetInfo.IsEmpty())
	{
		// watching a UT2004 server's map (Net/UE2NetJoin.h); the bar has one line (a join notice is drawn above it)
		NetInfo.Split(TEXT("\n"), &Info, nullptr);
		if (Info.IsEmpty())
		{
			Info = NetInfo;
		}
	}
	else if (MyPS && MyPS->bEliminated)
	{
		Info = UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("OutFireText"), TEXT("   You are out. Fire to watch the others."));
	}
	else if (GS && GS->Phase == EArenaMatchPhase::Countdown && !MyPS)
	{
		Info = UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("WaitingToSpawn"), TEXT("Press [Fire] to join the match!"));
	}
	else
	{
		Info = UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("Restart"), TEXT("   You died. Press Fire to respawn!"));
	}
	const FVector2D S = D.TextSizeUT(MedFont, Info);
	D.TextUT(MedFont, Info, D.W() * 0.5f - S.X * 0.5f, Top + Full * 0.5f - S.Y * 0.5f, FLinearColor::White);
}

// =============================================================================================
// HudCDeathmatch.DrawHudPassA in a vehicle
// =============================================================================================

void AArenaHUD::DrawUTVehicleHud(const AArenaCharacter* Driver, int32 VehicleHealth, float VehicleHealthMax)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTLocal;
	const FDMWidgets& Def = DM();
	const FHudFrame& F = UTFrame;
	const int32 TI = F.TeamIndex;
	if (Driver)
	{
		DrawUTVitals(Driver);
		DrawUTAdrenaline(Driver);
	}
	FSpriteWidget Box = Def.HudBorderVehicleHealth;
	Box.Tints[0] = F.HudRed;
	Box.Tints[1] = F.HudBlue;
	DrawSprite(F, Box);
	FSpriteWidget Icon = Def.HudBorderVehicleHealthIcon;
	if (VehicleHealthMax > 0.f && float(VehicleHealth) / VehicleHealthMax < 0.26f)
	{
		FSpriteWidget Alert = Def.HudVehicleHealthALERT;
		Alert.Tints[TI] = Colors::Team(TI);
		DrawSprite(F, Alert);
		Icon.Tex = ETex::HudPulse;
	}
	DrawSprite(F, Icon);
	FNumericWidget Digits = Def.DigitsVehicleHealth;
	Digits.Value = FMath::Max(0, VehicleHealth);
	if (VehicleHealth > 999)
	{
		Digits.OffsetX = 220;
		Digits.OffsetY = -35;
		Digits.TextureScale = 0.39f;
	}
	DrawNumeric(F, Digits);
	DrawUTCrosshair(Driver);
}

// =============================================================================================
// HudBase.DisplayHit / DrawDamageIndicators
// =============================================================================================

void AArenaHUD::DrawUTDamageIndicators(const AArenaCharacter* Char)
{
	using namespace ArenaHudUT;
	const AArenaPlayerController* PC = GetArenaPC();
	if (!PC || !Char)
	{
		return;
	}
	// DisplayHit: the sides facing the hit flash (index 0 front / top, 1 back / bottom, 2 left, 3 right) with DamageTime =
	// 5 * Clamp(Damage, 20, 30), more than 0.7 towards a side emphasises it; Tick takes 120 a second off; DamageTime is the alpha
	float DamageTime[4] = { 0.f, 0.f, 0.f, 0.f };
	bool Emphasized[4] = { false, false, false, false };
	const float T = Now();
	const FRotator LookDir(0.f, Char->GetActorRotation().Yaw, 0.f);
	const FVector X = LookDir.Vector();
	const FVector Y = FRotationMatrix(LookDir).GetScaledAxis(EAxis::Y);
	for (const AArenaPlayerController::FDamageIndicator& Ind : PC->GetDamageIndicators())
	{
		const float Age = T - Ind.Time;
		const float Start = 5.f * FMath::Clamp(Ind.Damage, 20.f, 30.f);
		const float Left = Start - 120.f * Age;
		if (Age < 0.f || Left < 1.f)
		{
			continue;
		}
		FVector HitDir = Ind.From - Char->GetActorLocation();
		HitDir.Z = 0.f;
		HitDir = HitDir.GetSafeNormal();
		const float Forward = FVector::DotProduct(HitDir, X);
		const float Side = FVector::DotProduct(HitDir, Y);
		bool Ignore[4] = { false, false, false, false };
		bool Emph[4] = { false, false, false, false };
		if (Forward > 0.f) { Emph[0] = Forward > 0.7f; Ignore[1] = true; }
		else { Emph[1] = Forward < -0.7f; Ignore[0] = true; }
		if (Side > 0.f) { Emph[3] = Side > 0.7f; Ignore[2] = true; }
		else { Emph[2] = Side < -0.7f; Ignore[3] = true; }
		for (int32 i = 0; i < 4; ++i)
		{
			if (!Ignore[i] && Left > DamageTime[i])
			{
				DamageTime[i] = Left;
				Emphasized[i] = Emph[i];
			}
		}
	}
	const float W = D.W(), H = D.H();
	auto Col = [](float A) { return FLinearColor(FColor(255, 0, 0, uint8(FMath::Clamp(A, 0.f, 255.f)))); };
	if (DamageTime[0] > 0.f)
	{
		DrawTile(D, ETex::Hud, 0.f, 0.f, W, (Emphasized[0] ? 0.15f : 0.05f) * H, 395.f, 219.f, 21.f, -10.f, Col(DamageTime[0]));
	}
	if (DamageTime[1] > 0.f)
	{
		const float Hh = Emphasized[1] ? 0.15f : 0.1f;
		DrawTile(D, ETex::Hud, 0.f, (1.f - Hh) * H, W, Hh * H, 395.f, 209.f, 21.f, 10.f, Col(DamageTime[1]));
	}
	if (DamageTime[2] > 0.f)
	{
		DrawTile(D, ETex::Hud, 0.f, 0.f, (Emphasized[2] ? 0.15f : 0.05f) * W, H, 404.f, 182.f, 12.f, 21.f, Col(DamageTime[2]));
	}
	if (DamageTime[3] > 0.f)
	{
		const float Ww = Emphasized[3] ? 0.15f : 0.05f;
		DrawTile(D, ETex::Hud, (1.f - Ww) * W, 0.f, Ww * W, H, 416.f, 182.f, -12.f, 21.f, Col(DamageTime[3]));
	}
}
