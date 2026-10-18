//=============================================================================
// OUTKeyStore: the validator's state, saved in System/OpenUT2k4ValidatorKeys.ini
// ([OpenUT2k4Validator.OUTKeyStore]). Admins may edit it while the server is
// stopped; the mutate commands change it at run time.
//
//   Keys=<fingerprint>|<first seen YYYY-MM-DD HH>|<last seen>|<last name>
//   Bans=<fingerprint>|<ip>|<cidr>|<legacy id>|<date>|<reason>|<issuer>
//   Whitelist=<fingerprint>|<comment>
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTKeyStore extends Object
	config(OpenUT2k4ValidatorKeys);

var config string ServerSecret;
var config array<string> Keys;
var config array<string> Bans;
var config array<string> Whitelist;

defaultproperties
{
}
