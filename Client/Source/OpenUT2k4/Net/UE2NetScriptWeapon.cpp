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
	if (!bEquipped || !GetViewMesh())
	{
		return;
	}
	// the animation the script plays on the weapon (WeaponFire.PlayFiring, Weapon.BringUp / PlayIdle ...)
	if (St.AnimSerial != AnimSerial)
	{
		AnimSerial = St.AnimSerial;
		if (St.bAnimStopped)
		{
			GetViewMesh()->bPauseAnims = true;
			bAnimEndPending = false;
		}
		else if (!St.Anim.IsNone())
		{
			GetViewMesh()->bPauseAnims = false;
			bAnimEndPending = PlayViewAnim(*St.Anim.ToString(), St.bAnimLoop) && !St.bAnimLoop;
			if (St.AnimRate > 0.f && St.AnimRate != 1.f)
			{
				GetViewMesh()->SetPlayRate(St.AnimRate);
			}
		}
	}
	// UE2 calls AnimEnd(0) when a non-looping animation ends (Weapon.AnimEnd: the idle animation, state code waiting)
	if (bAnimEndPending && !GetViewMesh()->IsPlaying())
	{
		bAnimEndPending = false;
		S->ScriptWeaponAnimEnd(ScriptChannel);
	}
}
