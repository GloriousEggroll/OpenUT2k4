#include "UE2NetScriptWeapon.h"

#include "Components/SkeletalMeshComponent.h"
#include "UE2NetScriptHUD.h"

AWeap_UE2Script::AWeap_UE2Script()
{
	WeaponName = TEXT("Mod weapon");
	bInfiniteAmmo = false;
	MaxAmmo = 1;
}

void AWeap_UE2Script::Equip()
{
	Super::Equip();
	if (FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get())
	{
		S->ScriptWeaponCommand(ScriptChannel, FUE2NetScriptHUD::EWeaponCommand::BringUp, 0);
	}
}

void AWeap_UE2Script::Unequip()
{
	if (FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get())
	{
		S->ScriptWeaponCommand(ScriptChannel, FUE2NetScriptHUD::EWeaponCommand::PutDown, 0);
	}
	Super::Unequip();
}

void AWeap_UE2Script::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
	FUE2NetScriptHUD::FScriptWeaponState St;
	if (!S || !S->GetScriptWeaponState(ScriptChannel, St))
	{
		return;
	}
	// the script's ammo (Weapon.AmmoAmount / MaxAmmo of the first mode) for our HUD
	Ammo = FMath::Max(0, St.Ammo);
	MaxAmmo = FMath::Max(1, St.MaxAmmo);
	// the animation the script plays on the weapon (WeaponFire.PlayFiring, Weapon.BringUp / PlayIdle, BW's cocking ...)
	const double Now = FPlatformTime::Seconds();
	if (St.AnimSerial != AnimSerial)
	{
		AnimSerial = St.AnimSerial;
		AnimEndFallback = 0.0;
		bAnimEndPending = false;
		USkeletalMeshComponent* View = bEquipped ? GetViewMesh() : nullptr;
		if (St.bAnimStopped)
		{
			if (View)
			{
				View->bPauseAnims = true;
			}
		}
		else if (!St.Anim.IsNone())
		{
			bool bPlaying = false;
			if (View)
			{
				View->bPauseAnims = false;
				bPlaying = PlayViewAnim(*St.Anim.ToString(), St.bAnimLoop);
				if (bPlaying && St.AnimRate > 0.f && St.AnimRate != 1.f)
				{
					View->SetPlayRate(St.AnimRate);
				}
			}
			if (!St.bAnimLoop)
			{
				bAnimEndPending = true;
				// an animation our view mesh cannot play (not converted, not in its set, not shown): UE2's AnimEnd still
				// comes, or script waiting for it (a reload's or a weapon's cocking state) would wait forever
				AnimEndFallback = bPlaying ? 0.0 : Now + 0.5 / FMath::Max(0.1f, St.AnimRate > 0.f ? St.AnimRate : 1.f);
			}
		}
	}
	// UE2 calls AnimEnd(0) when a non-looping animation ends (Weapon.AnimEnd: the idle animation, state code waiting)
	if (bAnimEndPending)
	{
		USkeletalMeshComponent* View = bEquipped ? GetViewMesh() : nullptr;
		const bool bEnded = AnimEndFallback > 0.0 ? Now >= AnimEndFallback : (!View || !View->IsPlaying());
		if (bEnded)
		{
			bAnimEndPending = false;
			AnimEndFallback = 0.0;
			S->ScriptWeaponAnimEnd(ScriptChannel);
		}
	}
}
