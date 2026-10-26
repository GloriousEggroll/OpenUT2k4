#pragma once

#include "CoreMinimal.h"
#include "../ArenaWeapon.h"
#include "UE2NetScriptWeapon.generated.h"

/**
 * A mod weapon with no stock relative (Ballistic Weapons ...) held by our player on a UT2004 server: the weapon runs as
 * the mod's own UnrealScript in the client mirror (Net/UE2NetScriptHUD: its Weapon, WeaponFire objects and ammo; its
 * calls to the server go out as UE2's replicated functions), this actor shows it. The view model is the mod's
 * first-person mesh and placement (UE2NetModWeapons::FindWeapon with Engine.Weapon as the base); it plays the
 * animations the script plays (PlayAnim on the weapon), reports their end (AnimEnd), and shows the script's ammo.
 * Fire presses go to the script's ClientStartFire / ClientStopFire; nothing is fired natively.
 */
UCLASS(NotPlaceable)
class OPENUT2K4_API AWeap_UE2Script : public AArenaWeapon
{
	GENERATED_BODY()

public:
	AWeap_UE2Script();

	/** The server's weapon channel this actor shows. */
	void SetScriptChannel(int32 InChannel) { ScriptChannel = InChannel; }
	int32 GetScriptChannel() const { return ScriptChannel; }

	virtual void Equip() override;
	virtual void Unequip() override;
	virtual void StartFire(uint8 Mode) override {}
	virtual void StopFire(uint8 Mode) override {}
	virtual bool HasAmmo() const override { return true; }
	virtual bool HasAmmoFor(uint8 Mode) const override { return true; }

protected:
	virtual void Tick(float DeltaSeconds) override;

	int32 ScriptChannel = 0;
	uint32 AnimSerial = 0;
	bool bAnimEndPending = false;
	/** Platform time to send AnimEnd for an animation our view mesh does not play (0: none). */
	double AnimEndFallback = 0.0;
};
