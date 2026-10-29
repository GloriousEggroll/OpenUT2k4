// UScript natives of XGame classes: xUtil's character records (the System .upl player lists) and DecoText.

#include "UScriptCore.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "UScriptWorld.h"
#include "../ArenaSPData.h"

namespace USXGameNatives
{
	/**
	 * The "Player=" records of the .upl files (UE2's xUtil CachedPlayerList), read once: every record in file order, its
	 * index being its RecordIndex. Records without a DefaultName are left out.
	 */
	const TArray<TMap<FString, FString>>& PlayerRecords()
	{
		static TArray<TMap<FString, FString>> Records;
		static bool bRead = false;
		if (!bRead)
		{
			bRead = true;
			for (TMap<FString, FString>& R : ArenaSP::ReadPlayerListFields())
			{
				const FString* Name = R.Find(TEXT("defaultname"));
				if (Name && !Name->IsEmpty())
				{
					Records.Add(MoveTemp(R));
				}
			}
			UE_LOG(LogUScript, Log, TEXT("UScript: xUtil player list: %d characters from the .upl files"), Records.Num());
		}
		return Records;
	}

	FUSStruct* PlayerRecordStruct()
	{
		static FUSStruct* S = nullptr;
		if (!S)
		{
			FUSObject* O = FUSRuntime::Get().FindObject(TEXT("XGame.xUtil.PlayerRecord"));
			S = O && (O->Flags & USF_Field) ? static_cast<FUSStruct*>(O) : nullptr;
		}
		return S;
	}

	/** A material of a record (Portrait), as DynamicLoadObject gives it: None when the game does not have it. */
	FUSObject* LoadMaterial(const FString& Path)
	{
		FUSRuntime& RT = FUSRuntime::Get();
		if (Path.IsEmpty() || Path.Equals(TEXT("None"), ESearchCase::IgnoreCase) || !Path.Contains(TEXT(".")))
		{
			return nullptr;
		}
		FUSClass* Mat = RT.FindClass(TEXT("Engine.Material"));
		if (FUSObject* O = RT.FindObject(Path, Mat))
		{
			return O;
		}
		return !RT.ContentObjectExists || RT.ContentObjectExists(Path, TEXT("Material")) ? RT.GetPlaceholder(Path, Mat) : nullptr;
	}

	/**
	 * Fills a zeroed xUtil.PlayerRecord from a .upl record, as UE2's xUtil does: the .upl keys (DefaultName, species, Mesh,
	 * BodySkin, FaceSkin, Portrait, Text, VoiceClass, Sex, the bot skills, Race, FavoriteWeapon, Menu, Skeleton, Ragdoll,
	 * BotUse, UseSpecular, TeamFace, ZeroWeaponOffsets) into the struct's fields (MeshName, BodySkinName, FaceSkinName,
	 * TextName, VoiceClassName ...), Species and Portrait loaded.
	 */
	void FillRecord(FUSStruct* S, uint8* Dst, const TMap<FString, FString>& KV, int32 Index)
	{
		auto Bool = [](const FString& V)
		{
			return V.Equals(TEXT("True"), ESearchCase::IgnoreCase) || V.Equals(TEXT("Yes"), ESearchCase::IgnoreCase) || FCString::Atoi(*V) != 0;
		};
		struct FMap { const TCHAR* Prop; const TCHAR* Keys[2]; };
		static const FMap Map[] = {
			{ TEXT("DefaultName"), { TEXT("defaultname") } }, { TEXT("Species"), { TEXT("species") } },
			{ TEXT("MeshName"), { TEXT("mesh"), TEXT("meshname") } }, { TEXT("BodySkinName"), { TEXT("bodyskin"), TEXT("bodyskinname") } },
			{ TEXT("FaceSkinName"), { TEXT("faceskin"), TEXT("faceskinname") } }, { TEXT("Portrait"), { TEXT("portrait") } },
			{ TEXT("TextName"), { TEXT("text"), TEXT("textname") } }, { TEXT("VoiceClassName"), { TEXT("voiceclass"), TEXT("voiceclassname") } },
			{ TEXT("Sex"), { TEXT("sex") } }, { TEXT("Accuracy"), { TEXT("accuracy") } }, { TEXT("Aggressiveness"), { TEXT("aggressiveness") } },
			{ TEXT("StrafingAbility"), { TEXT("strafingability") } }, { TEXT("CombatStyle"), { TEXT("combatstyle") } },
			{ TEXT("Tactics"), { TEXT("tactics") } }, { TEXT("ReactionTime"), { TEXT("reactiontime") } }, { TEXT("Jumpiness"), { TEXT("jumpiness") } },
			{ TEXT("Race"), { TEXT("race") } }, { TEXT("FavoriteWeapon"), { TEXT("favoriteweapon") } }, { TEXT("Menu"), { TEXT("menu") } },
			{ TEXT("Skeleton"), { TEXT("skeleton") } }, { TEXT("Ragdoll"), { TEXT("ragdoll") } }, { TEXT("BotUse"), { TEXT("botuse") } },
			{ TEXT("UseSpecular"), { TEXT("usespecular") } }, { TEXT("TeamFace"), { TEXT("teamface") } },
			{ TEXT("ZeroWeaponOffsets"), { TEXT("zeroweaponoffsets") } },
		};
		FUSRuntime& RT = FUSRuntime::Get();
		for (const FMap& M : Map)
		{
			FUSProperty* P = S->FindPropertyByName(M.Prop);
			const FString* V = KV.Find(M.Keys[0]);
			if (!V && M.Keys[1])
			{
				V = KV.Find(M.Keys[1]);
			}
			if (!P)
			{
				continue;
			}
			uint8* Addr = Dst + P->Offset;
			if (P->Type == EUSType::Byte)
			{
				// BotUse: 1 when the record does not say
				*Addr = uint8(FMath::Clamp(V ? FCString::Atoi(**V) : 1, 0, 255));
				continue;
			}
			if (!V)
			{
				continue;
			}
			switch (P->Type)
			{
			case EUSType::Str: *reinterpret_cast<FString*>(Addr) = *V; break;
			case EUSType::Bool: *reinterpret_cast<uint32*>(Addr) = Bool(*V) ? 1u : 0u; break;
			case EUSType::Int: *reinterpret_cast<int32*>(Addr) = FCString::Atoi(**V); break;
			case EUSType::Class: *reinterpret_cast<FUSObject**>(Addr) = V->IsEmpty() ? nullptr : RT.FindClass(*V); break;
			case EUSType::Object: *reinterpret_cast<FUSObject**>(Addr) = LoadMaterial(*V); break;
			default: break;
			}
		}
		if (FUSProperty* P = S->FindPropertyByName(TEXT("RecordIndex")); P && P->Type == EUSType::Int)
		{
			*reinterpret_cast<int32*>(Dst + P->Offset) = Index;
		}
	}

	/** Writes record Index (or an empty record for INDEX_NONE) into a returned PlayerRecord. */
	void ReturnRecord(FUSObject* Self, FName Function, int32 Index, void* Result)
	{
		FUSStruct* S = PlayerRecordStruct();
		FUSFunction* F = Self ? USVM::FindFunction(Self, Function) : nullptr;
		FUSProperty* Ret = F ? F->ReturnProperty : nullptr;
		if (!Result || !S || !Ret || Ret->Type != EUSType::Struct || Ret->Struct != S)
		{
			return;
		}
		Ret->DestroySingle(Result);
		FMemory::Memzero(Result, Ret->ElementSize);
		if (PlayerRecords().IsValidIndex(Index))
		{
			FillRecord(S, static_cast<uint8*>(Result), PlayerRecords()[Index], Index);
		}
	}

	/** xUtil.GetPlayerList(out array<PlayerRecord>): every character of the .upl files. */
	US_NATIVE(GetPlayerList)
	{
		USP_GET_ARRAY_REF(Out, OutProp);
		USP_FINISH;
		FUSProperty* Inner = OutProp && OutProp->Type == EUSType::Array ? OutProp->Inner : nullptr;
		FUSStruct* S = Inner && Inner->Type == EUSType::Struct ? Inner->Struct : nullptr;
		if (!Out || !S)
		{
			return;
		}
		const TArray<TMap<FString, FString>>& Records = PlayerRecords();
		const int32 ES = Inner->ElementSize;
		for (int32 i = 0; i < Out->Num; ++i)
		{
			Inner->DestroySingle(Out->Data + int64(i) * ES);
		}
		Out->Data = static_cast<uint8*>(FMemory::Realloc(Out->Data, int64(FMath::Max(1, Records.Num())) * ES, 16));
		Out->Max = FMath::Max(1, Records.Num());
		Out->Num = Records.Num();
		FMemory::Memzero(Out->Data, int64(Records.Num()) * ES);
		for (int32 i = 0; i < Records.Num(); ++i)
		{
			FillRecord(S, Out->Data + int64(i) * ES, Records[i], i);
		}
	}

	/** xUtil.GetPlayerRecord(int): the record at that RecordIndex (an empty one when out of range). */
	US_NATIVE(GetPlayerRecord)
	{
		USP_GET_INT(Index);
		USP_FINISH;
		ReturnRecord(Self, TEXT("GetPlayerRecord"), Index, Result);
	}

	/** xUtil.FindUPLPlayerRecord(string): the first record whose DefaultName is the name (any case), else an empty one. */
	US_NATIVE(FindUPLPlayerRecord)
	{
		USP_GET_STR(CharName);
		USP_FINISH;
		const int32 Index = PlayerRecords().IndexOfByPredicate([&CharName](const TMap<FString, FString>& R)
		{
			return R.FindChecked(TEXT("defaultname")).Equals(CharName, ESearchCase::IgnoreCase);
		});
		ReturnRecord(Self, TEXT("FindUPLPlayerRecord"), CharName.IsEmpty() ? INDEX_NONE : Index, Result);
	}

	/**
	 * xUtil.LoadDecoText(PackageName, DecoTextName, optional ColumnCount): a new DecoText with the localized
	 * [DecoText] DecoTextName of <PackageName>.int ("XPlayers.JuggMaleAA"), its "|"-separated rows in Rows (rows wrapped at
	 * ColumnCount characters when given); None when the text is not localized.
	 */
	US_NATIVE(LoadDecoText)
	{
		USP_GET_STR(PackageName);
		USP_GET_STR(DecoTextName);
		USP_GET_INT_OPTX(ColumnCount, 0);
		USP_FINISH;
		US_RESULT(FUSObject*) = nullptr;
		FUSRuntime& RT = FUSRuntime::Get();
		FString Text;
		FUSClass* DecoClass = RT.FindClass(TEXT("XGame.DecoText"));
		if (DecoTextName.IsEmpty() || !DecoClass || !RT.GetLocalized(PackageName, TEXT("DecoText"), DecoTextName, Text))
		{
			return;
		}
		FUSWorld* W = Self && Self->World ? Self->World : &FUSWorld::Transient();
		FUSObject* Deco = W->ConstructObject(DecoClass);
		if (!Deco || !Deco->Data)
		{
			return;
		}
		TArray<FString> Rows;
		Text.ParseIntoArray(Rows, TEXT("|"), false);
		if (ColumnCount > 0)
		{
			TArray<FString> Wrapped;
			for (const FString& Row : Rows)
			{
				FString Rest = Row;
				while (Rest.Len() > ColumnCount)
				{
					int32 Cut = ColumnCount;
					for (int32 i = ColumnCount; i > 0; --i)
					{
						if (Rest[i] == TEXT(' '))
						{
							Cut = i;
							break;
						}
					}
					Wrapped.Add(Rest.Left(Cut));
					Rest = Rest.Mid(Cut).TrimStart();
				}
				Wrapped.Add(Rest);
			}
			Rows = MoveTemp(Wrapped);
		}
		if (FUSProperty* P = Deco->FindProperty(TEXT("TextName")); P && P->Type == EUSType::Str)
		{
			*reinterpret_cast<FString*>(Deco->Data + P->Offset) = DecoTextName;
		}
		if (FUSProperty* P = Deco->FindProperty(TEXT("Rows")); P && P->Type == EUSType::Array && P->Inner && P->Inner->Type == EUSType::Str)
		{
			FUSScriptArray& A = *reinterpret_cast<FUSScriptArray*>(Deco->Data + P->Offset);
			P->DestroySingle(&A);
			A.Data = static_cast<uint8*>(FMemory::Malloc(int64(FMath::Max(1, Rows.Num())) * P->Inner->ElementSize, 16));
			FMemory::Memzero(A.Data, int64(FMath::Max(1, Rows.Num())) * P->Inner->ElementSize);
			A.Num = A.Max = Rows.Num();
			for (int32 i = 0; i < Rows.Num(); ++i)
			{
				*reinterpret_cast<FString*>(A.Data + int64(i) * P->Inner->ElementSize) = Rows[i];
			}
		}
		US_RESULT(FUSObject*) = Deco;
	}
}

void USNatives::RegisterXGame()
{
	using namespace USXGameNatives;
	static const FUSNativeEntry Entries[] = {
		{ TEXT("xUtil"), TEXT("GetPlayerList"), &GetPlayerList }, { TEXT("xUtil"), TEXT("GetPlayerRecord"), &GetPlayerRecord },
		{ TEXT("xUtil"), TEXT("FindUPLPlayerRecord"), &FindUPLPlayerRecord }, { TEXT("xUtil"), TEXT("LoadDecoText"), &LoadDecoText },
	};
	Register(Entries, UE_ARRAY_COUNT(Entries));
}

#endif // OPENUT2K4_USCRIPT_ENABLED
