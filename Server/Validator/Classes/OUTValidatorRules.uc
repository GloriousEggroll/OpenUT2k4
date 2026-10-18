//=============================================================================
// OUTValidatorRules: passes instant hit damage to the aim statistics of
// OUTPlayerCheck. Changes nothing.
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTValidatorRules extends GameRules;

var OUTValidator Validator;

function int NetDamage(int OriginalDamage, int Damage, Pawn Injured, Pawn InstigatedBy, vector HitLocation, out vector Momentum, class<DamageType> DamageType)
{
	if (Validator != None && InstigatedBy != None && Injured != None && Injured != InstigatedBy && DamageType != None
		&& (DamageType.default.bInstantHit || DamageType.default.bFastInstantHit) && PlayerController(InstigatedBy.Controller) != None)
		Validator.NotifyInstantHit(PlayerController(InstigatedBy.Controller));
	return Super.NetDamage(OriginalDamage, Damage, Injured, InstigatedBy, HitLocation, Momentum, DamageType);
}

defaultproperties
{
}
