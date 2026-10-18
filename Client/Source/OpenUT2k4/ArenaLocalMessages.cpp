#include "ArenaLocalMessages.h"
#include "ArenaContent.h"
#include "ArenaWeapon.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"
#include "UScript/ArenaUScript.h"
#include "UScript/UScriptCore.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScript/UScriptRuntime.h"
#endif

namespace ArenaLocalMessagesLocal
{
	using ArenaLocalMessages::EStackMode;
	using ArenaLocalMessages::FStyle;

	// Class defaults of the stock message classes (LocalMessage -> CriticalEventPlus -> ...), merged down the class
	// chain. DrawPivot 8 = DP_MiddleMiddle.
	//                       source class                         DrawColor                  Size PosX  PosY    Pivot Stack              Life  Fade  Unique Special
	const FStyle Styles[] = {
		{ TEXT("Engine.LocalMessage"),            FColor(255, 255, 255, 255), 0, 0.5f, 0.83f,  8, EStackMode::None, 3.f, false, false, true },
		{ TEXT("UnrealGame.StartupMessage"),      FColor(32, 64, 255, 255),   1, 0.5f, 0.83f,  8, EStackMode::None, 3.f, true,  true,  true },
		{ TEXT("UnrealGame.TimerMessage"),        FColor(255, 255, 0, 255),   0, 0.5f, 0.10f,  8, EStackMode::Down, 1.f, true,  true,  true },
		{ TEXT("XGame.xKillerMessagePlus"),       FColor(0, 160, 255, 255),   0, 0.5f, 0.10f,  8, EStackMode::Down, 3.f, true,  true,  true },
		{ TEXT("XGame.xVictimMessage"),           FColor(255, 0, 0, 255),     0, 0.5f, 0.10f,  8, EStackMode::Down, 6.f, true,  true,  true },
		{ TEXT("XGame.MultiKillMessage"),         FColor(255, 0, 0, 255),     1, 0.5f, 0.242f, 8, EStackMode::Down, 3.f, true,  true,  true },
		{ TEXT("UnrealGame.KillingSpreeMessage"), FColor(0, 160, 255, 255),   0, 0.5f, 0.83f,  8, EStackMode::None, 3.f, true,  true,  true },
		{ TEXT("UnrealGame.FirstBloodMessage"),   FColor(255, 0, 0, 255),     1, 0.5f, 0.83f,  8, EStackMode::None, 3.f, true,  true,  true },
		{ TEXT("XGame.SpecialKillMessage"),       FColor(255, 0, 0, 255),     0, 0.5f, 0.10f,  8, EStackMode::Down, 3.f, true,  true,  true },
		{ TEXT("XGame.xDeathMessage"),            FColor(255, 0, 0, 255),     0, 0.5f, 0.83f,  8, EStackMode::None, 3.f, false, false, false },
	};
	static_assert(UE_ARRAY_COUNT(Styles) == int32(EArenaLocalMessage::Death) + 1, "one style per EArenaLocalMessage");

	// ---- localization (.int files of the install) ----

	struct FIntFile
	{
		bool bLoaded = false;
		bool bExists = false;
		double LastTry = 0.0;
		/** "section|key" (lower case) -> value */
		TMap<FString, FString> Values;
	};

	FIntFile& IntFile(const FString& Package)
	{
		static TMap<FString, FIntFile> Files;
		FIntFile& F = Files.FindOrAdd(Package.ToLower());
		// a missing file is looked for again every few seconds (the startup content pass may still be copying it)
		const double Now = FPlatformTime::Seconds();
		if (F.bLoaded && (F.bExists || Now - F.LastTry < 5.0))
		{
			return F;
		}
		F.bLoaded = true;
		F.LastTry = Now;
		FString Text;
		// the cache's copy (<cache>/localization, ArenaConvLocalization.h), else the install's System folder
		const FString Path = ArenaContent::FindLocalizationFile(Package + TEXT(".int"));
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path))
		{
			return F;
		}
		F.bExists = true;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		FString Section;
		for (FString& Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT(";")))
			{
				continue;
			}
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				Section = Line.Mid(1, Line.Len() - 2).ToLower();
				continue;
			}
			FString Key, Value;
			if (!Line.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
			{
				Value = Value.Mid(1, Value.Len() - 2);
			}
			F.Values.Add(Section + TEXT("|") + Key.ToLower(), Value);
		}
		return F;
	}

	/** The .int string, "" when the package's .int exists without that key (an empty class default), else Fallback. */
	FString Loc(const TCHAR* Package, const TCHAR* Section, const FString& Key, const FString& Fallback)
	{
		FIntFile& F = IntFile(Package);
		if (!F.bExists)
		{
			return Fallback;
		}
		const FString* V = F.Values.Find(FString(Section).ToLower() + TEXT("|") + Key.ToLower());
		return V ? *V : FString();
	}

	FString Indexed(const TCHAR* Key, int32 Index)
	{
		return Index == 0 ? FString(Key) : FString::Printf(TEXT("%s[%d]"), Key, Index);
	}

	/** UnrealScript "A @ B": joined with one space. */
	FString At(const FString& A, const FString& B)
	{
		return A + TEXT(" ") + B;
	}

	// ---- our own wording (used when the install's .int files are not there) ----

	FString FallbackSpree(int32 Level)
	{
		static const TCHAR* const Notes[] = { TEXT("is on fire!"), TEXT("is relentless!"), TEXT("is unchained!"), TEXT("is an apex predator!"),
			TEXT("is legendary!"), TEXT("is beyond legendary!") };
		return Notes[FMath::Clamp(Level, 0, 5)];
	}

	FString FallbackSelfSpree(int32 Level)
	{
		static const TCHAR* const Notes[] = { TEXT("On fire!"), TEXT("Relentless!"), TEXT("Unchained!"), TEXT("Apex predator!"), TEXT("Legendary!"),
			TEXT("Beyond legendary!") };
		return Notes[FMath::Clamp(Level, 0, 5)];
	}

	FString FallbackMultiKill(int32 Level)
	{
		static const TCHAR* const Notes[] = { TEXT("Double Frag!"), TEXT("Triple Frag!"), TEXT("Quad Frag!"), TEXT("Frag Frenzy!"),
			TEXT("Frag Storm!"), TEXT("Frag Avalanche!"), TEXT("Frag Apocalypse!") };
		return Notes[FMath::Clamp(Level - 1, 0, 6)];
	}

	/** Stock damage type (localization section) of our weapons, by weapon class: the UT2004 weapon each one stands in for. */
	struct FDamageTypeMap
	{
		const TCHAR* WeaponClass;
		const TCHAR* Package;
		const TCHAR* DamageType;
	};
	const FDamageTypeMap DamageTypes[] = {
		{ TEXT("Weap_Gauntlet"),       TEXT("XWeapons"), TEXT("DamTypeShieldImpact") },
		{ TEXT("Weap_Carbine"),        TEXT("XWeapons"), TEXT("DamTypeAssaultBullet") },
		{ TEXT("Weap_GooLauncher"),    TEXT("XWeapons"), TEXT("DamTypeBioGlob") },
		{ TEXT("Weap_PulseRifle"),     TEXT("XWeapons"), TEXT("DamTypeShockBeam") },
		{ TEXT("Weap_ArcLinker"),      TEXT("XWeapons"), TEXT("DamTypeLinkPlasma") },
		{ TEXT("Weap_RotaryGun"),      TEXT("XWeapons"), TEXT("DamTypeMinigunBullet") },
		{ TEXT("Weap_ShardCannon"),    TEXT("XWeapons"), TEXT("DamTypeFlakChunk") },
		{ TEXT("Weap_RocketLauncher"), TEXT("XWeapons"), TEXT("DamTypeRocket") },
		{ TEXT("Weap_RailRifle"),      TEXT("XWeapons"), TEXT("DamTypeSniperShot") },
		{ TEXT("Weap_InstaRifle"),     TEXT("XWeapons"), TEXT("DamTypeSuperShockBeam") },
		{ TEXT("Weap_WarheadLauncher"), TEXT("XWeapons"), TEXT("DamTypeRedeemer") },
		{ TEXT("Weap_OrbitalDesignator"), TEXT("XWeapons"), TEXT("DamTypeIonBlast") },
		{ TEXT("Weap_MarksmanRifle"),  TEXT("UTClassic"), TEXT("DamTypeClassicSniper") },
		{ TEXT("Weap_SeekerLauncher"), TEXT("Onslaught"), TEXT("DamTypeONSAVRiLRocket") },
		{ TEXT("Weap_StickyLauncher"), TEXT("Onslaught"), TEXT("DamTypeONSGrenade") },
		{ TEXT("Weap_CrawlerLayer"),   TEXT("Onslaught"), TEXT("DamTypeONSMine") },
		{ TEXT("Weap_Translocator"),   TEXT("XWeapons"), TEXT("DamTypeTeleFrag") },
	};

	/** Deaths without a weapon of ours (ArenaTranslocator: UnrealPawn.gibbedBy by a pawn without the translocator / nobody). */
	const FDamageTypeMap SpecialDamageTypes[] = {
		{ TEXT("Telefragged"), TEXT("Engine"), TEXT("DamTypeTelefragged") },
		{ TEXT("Gibbed"),      TEXT("Engine"), TEXT("Gibbed") },
	};

	/**
	 * A damage type's DeathString / MaleSuicide from its class defaults in the player's script packages (the English
	 * .int files leave out the weapon damage types: their texts are only in the defaultproperties). Cached; empty when
	 * the script runtime is not available.
	 */
	bool ScriptDefaultString(const FString& Package, const FString& Class, const FString& Key, FString& Out)
	{
#if OPENUT2K4_USCRIPT_ENABLED
		static TMap<FString, FString> Cache;
		const FString CacheKey = (Package + TEXT(".") + Class + TEXT("|") + Key).ToLower();
		if (const FString* Hit = Cache.Find(CacheKey))
		{
			Out = *Hit;
			return !Out.IsEmpty();
		}
		FString Value;
		if (ArenaUScript::EnsureRuntime())
		{
			const double T0 = FPlatformTime::Seconds();
			FUSClass* C = FUSRuntime::Get().FindClass(Package + TEXT(".") + Class);
			FUSObject* DO = C ? C->GetDefaultObject() : nullptr;
			for (FUSClass* S = C; S && DO; S = S->GetSuperClass())
			{
				FUSPropRef Ref;
				if (Ref.Resolve(S, *Key) && Ref.Prop->Type == EUSType::Str)
				{
					Value = Ref.Get<FString>(DO);
					break;
				}
			}
			UE_LOG(LogTemp, Verbose, TEXT("Death message: %s.%s %s from the script defaults (%.1f ms)"), *Package, *Class, *Key, (FPlatformTime::Seconds() - T0) * 1000.0);
		}
		Cache.Add(CacheKey, Value);
		Out = Value;
		return !Out.IsEmpty();
#else
		return false;
#endif
	}

	/** (package, section) of the damage type for a weapon display name; the generic Engine DamageType when unknown. */
	TPair<FString, FString> DamageTypeFor(const FString& WeaponName, bool bHeadshot)
	{
		static TMap<FString, int32> ByName;
		static bool bBuilt = false;
		if (!bBuilt)
		{
			bBuilt = true;
			for (int32 i = 0; i < UE_ARRAY_COUNT(DamageTypes); ++i)
			{
				// display names come from the weapon defaults, so a renamed weapon keeps its damage type
				if (const UClass* Cls = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/OpenUT2k4.%s"), DamageTypes[i].WeaponClass)))
				{
					if (const AArenaWeapon* CDO = Cls->GetDefaultObject<AArenaWeapon>())
					{
						ByName.Add(CDO->WeaponName.ToLower(), i);
					}
				}
			}
		}
		const int32* Found = ByName.Find(WeaponName.ToLower());
		if (!Found)
		{
			for (const FDamageTypeMap& M : SpecialDamageTypes)
			{
				if (WeaponName.Equals(M.WeaponClass, ESearchCase::IgnoreCase))
				{
					return TPair<FString, FString>(M.Package, M.DamageType);
				}
			}
			return TPair<FString, FString>(TEXT("Engine"), TEXT("DamageType"));
		}
		const FDamageTypeMap& M = DamageTypes[*Found];
		if (bHeadshot && FCString::Strcmp(M.WeaponClass, TEXT("Weap_RailRifle")) == 0)
		{
			return TPair<FString, FString>(M.Package, TEXT("DamTypeSniperHeadShot"));
		}
		if (bHeadshot && FCString::Strcmp(M.WeaponClass, TEXT("Weap_MarksmanRifle")) == 0)
		{
			return TPair<FString, FString>(M.Package, TEXT("DamTypeClassicHeadshot"));
		}
		return TPair<FString, FString>(M.Package, M.DamageType);
	}
}

const ArenaLocalMessages::FStyle& ArenaLocalMessages::GetStyle(EArenaLocalMessage Kind)
{
	return ArenaLocalMessagesLocal::Styles[FMath::Clamp(int32(Kind), 0, int32(EArenaLocalMessage::Death))];
}

int32 ArenaLocalMessages::GetFontSize(EArenaLocalMessage Kind, int32 Switch, bool bLocalIsName1, bool bHasName2)
{
	switch (Kind)
	{
	case EArenaLocalMessage::MultiKill:
		return Switch < 4 ? 0 : Switch == 4 ? 1 : Switch == 7 ? 3 : 2;
	case EArenaLocalMessage::KillingSpree:
		return (!bHasName2 && bLocalIsName1) ? 2 : GetStyle(Kind).FontSize;
	default:
		return GetStyle(Kind).FontSize;
	}
}

FString ArenaLocalMessages::GetString(EArenaLocalMessage Kind, int32 Switch, const FString& Name1, const FString& Name2, bool bLocalIsName1)
{
	using namespace ArenaLocalMessagesLocal;
	switch (Kind)
	{
	case EArenaLocalMessage::Startup:
	{
		// StartupMessage.GetString: Stage[Switch]
		FString Fallback;
		if (Switch >= 2 && Switch <= 4)
		{
			Fallback = FString::Printf(TEXT("Match begins in %d"), 5 - Switch);
		}
		else if (Switch == 5 || Switch == 6)
		{
			Fallback = TEXT("Fight!");
		}
		else if (Switch == 7)
		{
			Fallback = TEXT("Overtime!");
		}
		else
		{
			Fallback = TEXT("Waiting for players");
		}
		return Loc(TEXT("UnrealGame"), TEXT("StartupMessage"), Indexed(TEXT("Stage"), FMath::Clamp(Switch, 0, 7)), Fallback);
	}
	case EArenaLocalMessage::Timer:
		// TimerMessage.GetString: Switch $ CountDownTrailer
		return FString::FromInt(Switch) + Loc(TEXT("UnrealGame"), TEXT("TimerMessage"), TEXT("CountDownTrailer"), TEXT(" s"));
	case EArenaLocalMessage::Killer:
		// xKillerMessagePlus.GetString: YouKilled @ victim @ YouKilledTrailer
		if (Name1.IsEmpty())
		{
			return FString();
		}
		if (!HasLocalization(TEXT("XGame")))
		{
			return FString::Printf(TEXT("You took down %s"), *Name1);
		}
		return At(At(Loc(TEXT("XGame"), TEXT("xKillerMessagePlus"), TEXT("YouKilled"), FString()), Name1),
			Loc(TEXT("XGame"), TEXT("xKillerMessagePlus"), TEXT("YouKilledTrailer"), FString()));
	case EArenaLocalMessage::Victim:
		// xVictimMessage.GetString: YouWereKilledBy @ killer $ KilledByTrailer
		if (Name1.IsEmpty())
		{
			return FString();
		}
		if (!HasLocalization(TEXT("XGame")))
		{
			return FString::Printf(TEXT("Taken out by %s"), *Name1);
		}
		return At(Loc(TEXT("XGame"), TEXT("xVictimMessage"), TEXT("YouWereKilledBy"), FString()), Name1)
			+ Loc(TEXT("XGame"), TEXT("xVictimMessage"), TEXT("KilledByTrailer"), FString());
	case EArenaLocalMessage::MultiKill:
		// MultiKillMessage.GetString: KillString[Min(Switch, 7) - 1]
		return Loc(TEXT("XGame"), TEXT("MultiKillMessage"), Indexed(TEXT("KillString"), FMath::Clamp(Switch, 1, 7) - 1), FallbackMultiKill(Switch));
	case EArenaLocalMessage::KillingSpree:
	{
		const int32 Level = FMath::Clamp(Switch, 0, 5);
		if (Name2.IsEmpty())
		{
			// GetRelatedString for the spree owner: SelfSpreeNote[Switch]; everyone else: owner @ SpreeNote[Switch]
			if (bLocalIsName1)
			{
				return Loc(TEXT("UnrealGame"), TEXT("KillingSpreeMessage"), Indexed(TEXT("SelfSpreeNote"), Level), FallbackSelfSpree(Level));
			}
			return Name1.IsEmpty() ? FString() : At(Name1, Loc(TEXT("UnrealGame"), TEXT("KillingSpreeMessage"), Indexed(TEXT("SpreeNote"), Level), FallbackSpree(Level)));
		}
		// ended by someone: owner $ EndSpreeNote @ killer @ EndSpreeNoteTrailer
		if (!HasLocalization(TEXT("UnrealGame")))
		{
			return FString::Printf(TEXT("%s ended %s's streak"), *Name2, *Name1);
		}
		return At(At(Name1 + Loc(TEXT("UnrealGame"), TEXT("KillingSpreeMessage"), TEXT("EndSpreeNote"), FString()), Name2),
			Loc(TEXT("UnrealGame"), TEXT("KillingSpreeMessage"), TEXT("EndSpreeNoteTrailer"), FString()));
	}
	case EArenaLocalMessage::FirstBlood:
		// FirstBloodMessage.GetString: killer @ FirstBloodString
		return Name1.IsEmpty() ? FString() : At(Name1, Loc(TEXT("UnrealGame"), TEXT("FirstBloodMessage"), TEXT("FirstBloodString"), TEXT("claimed the first kill!")));
	case EArenaLocalMessage::SpecialKill:
		return Loc(TEXT("XGame"), TEXT("SpecialKillMessage"), TEXT("DecapitationString"), TEXT("Headshot!"));
	default:
		return FString();
	}
}

FString ArenaLocalMessages::GetDeathString(const FString& Killer, const FString& Victim, const FString& WeaponName, bool bHeadshot)
{
	using namespace ArenaLocalMessagesLocal;
	const FString VictimName = Victim.IsEmpty() ? Loc(TEXT("XGame"), TEXT("xDeathMessage"), TEXT("SomeoneString"), TEXT("a player")) : Victim;
	const TPair<FString, FString> DamageType = DamageTypeFor(WeaponName, bHeadshot);
	FString Message;
	if (Killer.IsEmpty())
	{
		// xDeathMessage switch 1: DamageType.SuicideMessage (MaleSuicide / FemaleSuicide; we have no gender)
		if (!Localize(DamageType.Key, DamageType.Value, TEXT("MaleSuicide"), Message)
			&& !ScriptDefaultString(DamageType.Key, DamageType.Value, TEXT("MaleSuicide"), Message))
		{
			Localize(TEXT("Engine"), TEXT("DamageType"), TEXT("MaleSuicide"), Message);
		}
		if (Message.IsEmpty())
		{
			Message = TEXT("%o eliminated themselves");
		}
	}
	else
	{
		if (!Localize(DamageType.Key, DamageType.Value, TEXT("DeathString"), Message)
			&& !ScriptDefaultString(DamageType.Key, DamageType.Value, TEXT("DeathString"), Message))
		{
			Localize(TEXT("Engine"), TEXT("DamageType"), TEXT("DeathString"), Message);
		}
		if (Message.IsEmpty())
		{
			Message = WeaponName.IsEmpty() ? FString(TEXT("%k took down %o")) : FString::Printf(TEXT("%%k took down %%o [%s]"), *WeaponName);
		}
	}
	// GameInfo.ParseKillMessage
	Message.ReplaceInline(TEXT("%k"), *Killer, ESearchCase::CaseSensitive);
	Message.ReplaceInline(TEXT("%o"), *VictimName, ESearchCase::CaseSensitive);
	return Message;
}

bool ArenaLocalMessages::Localize(const FString& Package, const FString& Section, const FString& Key, FString& OutValue)
{
	ArenaLocalMessagesLocal::FIntFile& F = ArenaLocalMessagesLocal::IntFile(Package);
	const FString* V = F.Values.Find(Section.ToLower() + TEXT("|") + Key.ToLower());
	if (!V)
	{
		return false;
	}
	OutValue = *V;
	return true;
}

bool ArenaLocalMessages::HasLocalization(const FString& Package)
{
	return ArenaLocalMessagesLocal::IntFile(Package).bExists;
}
