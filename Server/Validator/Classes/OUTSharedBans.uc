//=============================================================================
// OUTSharedBans: ban lists imported from other admins, saved in
// System/OpenUT2k4ValidatorShared.ini ([OpenUT2k4Validator.OUTSharedBans]).
// Written only by Client/Tools/openut2k4_signing.py banlist-import, after it
// checked each list's Ed25519 signature against the issuer keys you trust
// (UnrealScript cannot fetch HTTPS or check Ed25519 at an affordable cost).
// Read when the map starts.
//
//   Bans=<fingerprint>|<ip>|<cidr>|<legacy id>|<date>|<reason>|<issuer>
//   Sources=<issuer>|<list date>|<entries>|<imported at>
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTSharedBans extends Object
	config(OpenUT2k4ValidatorShared);

var config array<string> Bans;
var config array<string> Sources;

defaultproperties
{
}
