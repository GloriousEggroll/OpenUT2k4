// UScript VM self tests (-UScriptTest): load the stock packages and run stock UT2004 script functions with known
// results. The packages come from the player's install; nothing from UT2004 is part of the build.

#include "UScriptCore.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "UScriptWorld.h"
#include "ArenaUScript.h"
#include "HAL/PlatformTime.h"
#include "UScriptBridge.h"

namespace
{
	struct FUSTest
	{
		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bOk, const FString& Name, const FString& Detail = FString())
		{
			if (bOk)
			{
				++Passed;
				UE_LOG(LogUScript, Display, TEXT("UScriptTest PASS %s %s"), *Name, *Detail);
			}
			else
			{
				++Failed;
				UE_LOG(LogUScript, Error, TEXT("UScriptTest FAIL %s %s"), *Name, *Detail);
			}
		}

		void Eq(const FString& Name, const FString& Got, const FString& Want)
		{
			Check(Got == Want, Name, FString::Printf(TEXT("got \"%s\" want \"%s\""), *Got, *Want));
		}
		void Eq(const FString& Name, int32 Got, int32 Want)
		{
			Check(Got == Want, Name, FString::Printf(TEXT("got %d want %d"), Got, Want));
		}
		void Near(const FString& Name, float Got, float Want, float Tol = 0.001f)
		{
			Check(FMath::Abs(Got - Want) <= Tol, Name, FString::Printf(TEXT("got %f want %f"), Got, Want));
		}
	};

	FUSClass* Cls(const TCHAR* Path)
	{
		FString Err;
		FUSClass* C = FUSRuntime::Get().FindClass(Path, &Err);
		if (!C)
		{
			UE_LOG(LogUScript, Error, TEXT("UScriptTest: class %s: %s"), Path, *Err);
		}
		return C;
	}

	FUSFunction* Func(FUSClass* C, const TCHAR* Name)
	{
		return C ? C->FindFunction(Name) : nullptr;
	}

	/** Calls Class.Function on the class default object (static and simulated helper functions). */
	struct FCall
	{
		FUSClass* C;
		FUSFunction* F;
		FUSParms P;
		FCall(const TCHAR* ClassPath, const TCHAR* FuncName) : C(Cls(ClassPath)), F(Func(C, FuncName)), P(F) {}
		bool Ok() const { return C && F; }
		void Go(FUSObject* On = nullptr) { if (Ok()) { P.Call(On ? On : C->GetDefaultObject()); } }
	};
}

void USRunSelfTests(int32& OutPassed, int32& OutFailed)
{
	FUSTest T;
	FString Err;
	const double T0 = FPlatformTime::Seconds();
	T.Check(ArenaUScript::EnsureRuntime(&Err), TEXT("runtime"), Err);
	if (!FUSRuntime::Get().IsInitialized())
	{
		OutPassed = T.Passed;
		OutFailed = T.Failed;
		return;
	}
	FUSRuntime& RT = FUSRuntime::Get();
	UE_LOG(LogUScript, Display, TEXT("UScriptTest: runtime up in %.1f ms"), (FPlatformTime::Seconds() - T0) * 1000.0);

	// ---- layout of the core structs (natives rely on it) ----
	{
		FUSStruct* V = static_cast<FUSStruct*>(RT.FindObject(TEXT("Core.Object.Vector")));
		FUSStruct* R = static_cast<FUSStruct*>(RT.FindObject(TEXT("Core.Object.Rotator")));
		FUSStruct* Col = static_cast<FUSStruct*>(RT.FindObject(TEXT("Core.Object.Color")));
		T.Check(V && V->PropertiesSize == 12 && V->FindPropertyByName(TEXT("Y")) && V->FindPropertyByName(TEXT("Y"))->Offset == 4
			&& V->FindPropertyByName(TEXT("Z"))->Offset == 8, TEXT("layout.Vector"));
		T.Check(R && R->PropertiesSize == 12 && R->FindPropertyByName(TEXT("Yaw")) && R->FindPropertyByName(TEXT("Yaw"))->Offset == 4, TEXT("layout.Rotator"));
		T.Check(Col && Col->PropertiesSize == 4 && Col->FindPropertyByName(TEXT("R")) && Col->FindPropertyByName(TEXT("R"))->Offset == 2, TEXT("layout.Color"));
	}

	// ---- Core natives called directly (C++ -> native, NativeParm frames) ----
	{
		FCall Abs(TEXT("Core.Object"), TEXT("Abs"));
		Abs.P.Float(-3.5f);
		Abs.Go();
		T.Near(TEXT("native.Abs"), Abs.P.ReturnFloat(), 3.5f);

		FCall Mid(TEXT("Core.Object"), TEXT("Mid"));
		Mid.P.Str(TEXT("Unreal Tournament")).Int(7).Int(4);
		Mid.Go();
		T.Eq(TEXT("native.Mid"), Mid.P.ReturnStr(), TEXT("Tour"));

		FCall InStr(TEXT("Core.Object"), TEXT("InStr"));
		InStr.P.Str(TEXT("DM-Rankin")).Str(TEXT("-"));
		InStr.Go();
		T.Eq(TEXT("native.InStr"), InStr.P.ReturnInt(), 2);

		FCall Divide(TEXT("Core.Object"), TEXT("Divide"));
		Divide.P.Str(TEXT("Key=Value")).Str(TEXT("="));
		Divide.Go();
		T.Check(Divide.P.ReturnBool() && Divide.P.GetStr(2) == TEXT("Key") && Divide.P.GetStr(3) == TEXT("Value"), TEXT("native.Divide(out)"),
			Divide.P.GetStr(2) + TEXT("|") + Divide.P.GetStr(3));

		FCall Repl(TEXT("Core.Object"), TEXT("Repl"));
		Repl.P.Str(TEXT("a.B.c")).Str(TEXT("b")).Str(TEXT("x"));
		Repl.Go();
		T.Eq(TEXT("native.Repl(case-insensitive)"), Repl.P.ReturnStr(), TEXT("a.x.c"));

		FCall VSize(TEXT("Core.Object"), TEXT("VSize"));
		VSize.P.Vector(FUSVector(3, 4, 12));
		VSize.Go();
		T.Near(TEXT("native.VSize"), VSize.P.ReturnFloat(), 13.f);
	}

	// ---- stock script functions (bytecode) ----
	{
		FCall C(TEXT("Core.Object"), TEXT("GetItemName"));
		C.P.Str(TEXT("XGame.xPawn"));
		C.Go();
		T.Eq(TEXT("Object.GetItemName"), C.P.ReturnStr(), TEXT("xPawn"));
	}
	{
		FCall C(TEXT("Core.Object"), TEXT("ReplaceText"));
		C.P.Str(TEXT("one-two-three")).Str(TEXT("-")).Str(TEXT(" + "));
		C.Go();
		T.Eq(TEXT("Object.ReplaceText(out)"), C.P.GetStr(0), TEXT("one + two + three"));
	}
	{
		FCall C(TEXT("Core.Object"), TEXT("RandRange"));
		C.P.Float(7.f).Float(7.f);
		C.Go();
		T.Near(TEXT("Object.RandRange"), C.P.ReturnFloat(), 7.f);
	}
	const TCHAR* Url = TEXT("?Game=XGame.xDeathMatch?Map=DM-Rankin?Mutator=XGame.MutInstaGib?NumBots=4");
	{
		FCall C(TEXT("Engine.GameInfo"), TEXT("ParseOption"));
		C.P.Str(Url).Str(TEXT("mutator"));
		C.Go();
		T.Eq(TEXT("GameInfo.ParseOption"), C.P.ReturnStr(), TEXT("XGame.MutInstaGib"));
	}
	{
		FCall C(TEXT("Engine.GameInfo"), TEXT("HasOption"));
		C.P.Str(Url).Str(TEXT("NumBots"));
		C.Go();
		const bool bHas = C.P.ReturnBool();
		FCall D(TEXT("Engine.GameInfo"), TEXT("HasOption"));
		D.P.Str(Url).Str(TEXT("Team"));
		D.Go();
		T.Check(bHas && !D.P.ReturnBool(), TEXT("GameInfo.HasOption"));
	}
	{
		FCall C(TEXT("Engine.GameInfo"), TEXT("GrabOption"));
		C.P.Str(TEXT("?Map=DM-Rankin?NumBots=4"));
		C.Go();
		T.Check(C.P.ReturnBool() && C.P.GetStr(1) == TEXT("Map=DM-Rankin") && C.P.GetStr(0) == TEXT("?NumBots=4"), TEXT("GameInfo.GrabOption(out,out)"),
			C.P.GetStr(0) + TEXT(" | ") + C.P.GetStr(1));
	}
	{
		FCall C(TEXT("Engine.GameInfo"), TEXT("GetKeyValue"));
		C.P.Str(TEXT("NumBots=4"));
		C.Go();
		T.Check(C.P.GetStr(1) == TEXT("NumBots") && C.P.GetStr(2) == TEXT("4"), TEXT("GameInfo.GetKeyValue"), C.P.GetStr(1) + TEXT("=") + C.P.GetStr(2));
	}
	{
		FCall C(TEXT("Engine.GameInfo"), TEXT("ParseKillMessage"));
		C.P.Str(TEXT("Alice")).Str(TEXT("Bob")).Str(TEXT("%o was fragged by %k"));
		C.Go();
		T.Eq(TEXT("GameInfo.ParseKillMessage"), C.P.ReturnStr(), TEXT("Bob was fragged by Alice"));
	}
	{
		FCall C(TEXT("Engine.StreamBase"), TEXT("RevInStr"));
		C.P.Str(TEXT("Music.Song.ogg")).Str(TEXT("."));
		C.Go();
		T.Eq(TEXT("StreamBase.RevInStr"), C.P.ReturnInt(), 10);
	}
	{
		FCall C(TEXT("Engine.LocalMessage"), TEXT("GetLifeTime"));
		C.P.Int(0);
		C.Go();
		T.Near(TEXT("LocalMessage.GetLifeTime(defaults)"), C.P.ReturnFloat(), 3.f);
	}
	{
		// localized default from XGame.int, read through the VM
		FUSClass* C = Cls(TEXT("XGame.MutInstaGib"));
		FUSProperty* P = C ? C->FindPropertyByName(TEXT("FriendlyName")) : nullptr;
		FString Want;
		RT.GetLocalized(TEXT("XGame"), TEXT("MutInstaGib"), TEXT("FriendlyName"), Want);
		const FString Got = (P && C) ? *reinterpret_cast<const FString*>(C->Defaults.GetData() + P->Offset) : FString();
		T.Check(!Got.IsEmpty() && Got == Want, TEXT("localized MutInstaGib.FriendlyName"), Got);
		FUSClass* LG = Cls(TEXT("UnrealGame.MutLowGrav"));
		FUSProperty* G = LG ? LG->FindPropertyByName(TEXT("GravityZ")) : nullptr;
		const float Grav = G ? *reinterpret_cast<const float*>(LG->Defaults.GetData() + G->Offset) : 0.f;
		T.Check(Grav < 0.f, TEXT("defaults MutLowGrav.GravityZ"), FString::Printf(TEXT("%f"), Grav));
	}

	// ---- dynamic arrays on class defaults (XGame.TeamRedConfigured: FindCharIndex / AddCharacter / RemoveCharacter) ----
	{
		auto Find = [](const TCHAR* Name) { FCall C(TEXT("XGame.TeamRedConfigured"), TEXT("FindCharIndex")); C.P.Str(Name); C.Go(); return C.P.ReturnInt(); };
		const int32 Before = Find(TEXT("ScriptTestBot"));
		{
			FCall Add(TEXT("XGame.TeamRedConfigured"), TEXT("AddCharacter"));
			Add.P.Str(TEXT("ScriptTestBot"));
			Add.Go();
		}
		const int32 Added = Find(TEXT("ScriptTestBot"));
		{
			FCall Rem(TEXT("XGame.TeamRedConfigured"), TEXT("RemoveCharacter"));
			Rem.P.Int(Added).Int(1);
			Rem.Go();
		}
		const int32 After = Find(TEXT("ScriptTestBot"));
		T.Check(Before == -1 && Added >= 0 && After == -1, TEXT("dynarray add/find/remove"), FString::Printf(TEXT("%d %d %d"), Before, Added, After));
	}
	{
		FCall C(TEXT("Engine.StreamBase"), TEXT("ParseDirectories"));
		C.P.Str(TEXT("Music/Rock/Live/song.ogg"));
		C.Go();
		const FUSScriptArray* A = reinterpret_cast<const FUSScriptArray*>(C.P.ReturnAddr());
		FString Parts;
		for (int32 i = 0; A && i < A->Num; ++i)
		{
			Parts += reinterpret_cast<const FString*>(A->Data)[i] + TEXT("|");
		}
		T.Check(A && A->Num >= 3, TEXT("StreamBase.ParseDirectories(array return, insert)"), Parts);
	}
	{
		// switch on strings + localized text
		FCall C(TEXT("XGame.MutInstaGib"), TEXT("GetDescriptionText"));
		C.P.Str(TEXT("bAllowTranslocator"));
		C.Go();
		const FString Got = C.P.ReturnStr();
		T.Check(!Got.IsEmpty() && Got != TEXT("bAllowTranslocator"), TEXT("MutInstaGib.GetDescriptionText(switch)"), Got);
	}
	{
		int32 Total = 0;
		FString Lens;
		for (int32 Ladder = 0; Ladder < 6; ++Ladder)
		{
			FCall C(TEXT("XGame.UT2K4LadderInfo"), TEXT("LengthOfLadder"));
			C.P.Int(Ladder);
			C.Go();
			Lens += FString::Printf(TEXT("%d "), C.P.ReturnInt());
			Total += C.P.ReturnInt();
		}
		T.Check(Total > 10, TEXT("UT2K4LadderInfo.LengthOfLadder(switch, class context, dynamic arrays)"), Lens);
	}
	{
		FCall C(TEXT("XGame.UT2K4GameProfile"), TEXT("MoneyToString"));
		C.P.Int(1234567);
		C.Go();
		T.Check(C.P.ReturnStr().Contains(TEXT("234")), TEXT("UT2K4GameProfile.MoneyToString"), C.P.ReturnStr());
	}

	// ---- actors: spawn, events, Tag/Event chain (Gameplay.Counter x2, Actor.TriggerEvent) ----
	{
		FUSGameWorld W(nullptr, nullptr);
		FString WErr;
		const bool bInit = W.Init(TEXT("DM"), WErr);
		FUSClass* CounterClass = Cls(TEXT("Gameplay.Counter"));
		FUSObject* A = (bInit && CounterClass) ? W.SpawnActor(CounterClass, nullptr, TEXT("CountA"), nullptr, nullptr, nullptr) : nullptr;
		FUSObject* B = (bInit && CounterClass) ? W.SpawnActor(CounterClass, nullptr, TEXT("CountB"), nullptr, nullptr, nullptr) : nullptr;
		FUSProperty* Num = CounterClass ? CounterClass->FindPropertyByName(TEXT("NumToCount")) : nullptr;
		if (A && B && Num)
		{
			USProps().Event.Set<FName>(A, TEXT("CountB"));
			*reinterpret_cast<uint8*>(A->Data + Num->Offset) = 2;
			*reinterpret_cast<uint8*>(B->Data + Num->Offset) = 3;
			FUSFunction* Trig = USVM::FindFunction(A, TEXT("Trigger"));
			for (int32 i = 0; i < 2 && Trig; ++i)
			{
				FUSParms P(Trig);
				P.Object(nullptr).Object(nullptr);
				P.Call(A);
			}
			W.Tick(0.1f);
			const int32 NA = *reinterpret_cast<uint8*>(A->Data + Num->Offset);
			const int32 NB = *reinterpret_cast<uint8*>(B->Data + Num->Offset);
			T.Check(NA == 0 && NB == 2, TEXT("actors: Counter A -> TriggerEvent -> Counter B"), FString::Printf(TEXT("A=%d B=%d"), NA, NB));
		}
		else
		{
			T.Check(false, TEXT("actors: spawn Gameplay.Counter"), WErr);
		}
	}

	// ---- bulk load of the stock script packages ----
	{
		const double L0 = FPlatformTime::Seconds();
		int32 Classes = 0, Failed = 0;
		const TCHAR* Pkgs[] = { TEXT("Core"), TEXT("Engine"), TEXT("UnrealGame"), TEXT("XGame"), TEXT("XWeapons"), TEXT("XPickups"),
			TEXT("XEffects"), TEXT("Onslaught"), TEXT("UT2k4Assault"), TEXT("XInterface"), TEXT("GUI2K4"), TEXT("Gameplay"), TEXT("Vehicles") };
		for (const TCHAR* PkgName : Pkgs)
		{
			FUSLinker* L = RT.LoadPackage(PkgName);
			if (!L)
			{
				continue;
			}
			for (int32 Ei = 0; Ei < L->NumExports(); ++Ei)
			{
				if (L->IsTopLevelClass(Ei))
				{
					FUSClass* C = static_cast<FUSClass*>(L->GetExport(Ei));
					++Classes;
					if (!C || !C->EnsureLoaded())
					{
						++Failed;
						if (Failed < 10)
						{
							UE_LOG(LogUScript, Warning, TEXT("UScriptTest: load failed %s: %s"), C ? *C->GetPathName() : TEXT("?"), C ? *C->LoadError : TEXT(""));
						}
					}
				}
			}
		}
		TArray<FString> Gameplay;
		for (const FString& M : RT.GetMissingNatives())
		{
			for (const TCHAR* K : { TEXT("Actor."), TEXT("Pawn."), TEXT("Controller."), TEXT("GameInfo."), TEXT("LevelInfo."), TEXT("Mutator."), TEXT("Inventory."), TEXT("Weapon.") })
			{
				if (M.StartsWith(K))
				{
					Gameplay.Add(M);
				}
			}
		}
		Gameplay.Sort();
		UE_LOG(LogUScript, Display, TEXT("UScriptTest: gameplay natives not implemented yet (%d): %s"), Gameplay.Num(), *FString::Join(Gameplay, TEXT(" ")));
		T.Check(Failed == 0 && Classes > 1000, TEXT("bulk load"), FString::Printf(TEXT("%d classes, %d failed, %.0f ms, %d natives not implemented yet"),
			Classes, Failed, (FPlatformTime::Seconds() - L0) * 1000.0, RT.GetMissingNatives().Num()));
	}

	// ---- interpreter speed ----
	{
		FUSClass* C = Cls(TEXT("Core.Object"));
		FUSFunction* F = Func(C, TEXT("GetItemName"));
		if (F)
		{
			const int32 N = 20000;
			const uint64 Tok0 = USVM::GStatTokens;
			const double S0 = FPlatformTime::Seconds();
			for (int32 i = 0; i < N; ++i)
			{
				FUSParms P(F);
				P.Str(TEXT("Engine.GameInfo.Something"));
				P.Call(C->GetDefaultObject());
			}
			const double Dt = FPlatformTime::Seconds() - S0;
			const uint64 Toks = USVM::GStatTokens - Tok0;
			T.Check(Dt / N < 0.0002, TEXT("perf.GetItemName"), FString::Printf(TEXT("%.2f us per call, %.1f M tokens/s"), Dt / N * 1e6, Toks / Dt / 1e6));
		}
	}

	OutPassed = T.Passed;
	OutFailed = T.Failed;
}

#endif // OPENUT2K4_USCRIPT_ENABLED
