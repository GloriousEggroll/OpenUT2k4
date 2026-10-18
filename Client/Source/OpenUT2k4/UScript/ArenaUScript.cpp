#include "ArenaUScript.h"
#include "../Convert/ArenaConvScripts.h"
#include "UScriptCore.h"
#include "Misc/CommandLine.h"
#include "Misc/DelayedAutoRegister.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"

#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "UScriptWorld.h"
#include "../ArenaContent.h"
#include "../ArenaUserContent.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

void USRunSelfTests(int32& OutPassed, int32& OutFailed);

EUSClassSupport USGetClassSupport(FUSClass* C, FString& OutReason)
{
	FUSRuntime& RT = FUSRuntime::Get();
	if (!C || !C->EnsureLoaded())
	{
		OutReason = C ? C->LoadError : FString(TEXT("class not found"));
		return EUSClassSupport::NotSupported;
	}
	if (!RT.ActorClass || !C->IsChildOf(RT.ActorClass))
	{
		return EUSClassSupport::RunsInVM; // plain objects
	}
	struct FKind { const TCHAR* Base; const TCHAR* What; };
	static const FKind Unsupported[] = {
		{ TEXT("Pawn"), TEXT("script pawns, monsters and vehicles") }, { TEXT("Controller"), TEXT("script AI and player controllers") },
		{ TEXT("Inventory"), TEXT("script weapons and inventory") }, { TEXT("WeaponPickup"), TEXT("script weapon pickups") },
		{ TEXT("Ammo"), TEXT("script ammo pickups") },
		{ TEXT("Projectile"), TEXT("script projectiles") }, { TEXT("HUD"), TEXT("script HUDs") }, { TEXT("Emitter"), TEXT("script particle effects") },
		{ TEXT("xPickUpBase"), TEXT("script pickup bases") }, { TEXT("Mover"), TEXT("script movers") },
		{ TEXT("NavigationPoint"), TEXT("script navigation points and teleporters") }, { TEXT("Brush"), TEXT("script volumes and brushes") },
		{ TEXT("Light"), TEXT("script lights") }, { TEXT("ScoreBoard"), TEXT("script scoreboards") } };
	auto ChainHas = [C](FName N)
	{
		int32 Guard = 0;
		for (FUSClass* K = C; K && Guard < 64; K = K->GetSuperClass(), ++Guard)
		{
			if (K->Name == N)
			{
				return true;
			}
		}
		return false;
	};
	for (const FKind& K : Unsupported)
	{
		if (ChainHas(FName(K.Base)))
		{
			OutReason = FString::Printf(TEXT("%s: %s are not supported yet"), *C->GetPathName(), K.What);
			return EUSClassSupport::NotSupported;
		}
	}
	return EUSClassSupport::RunsInVM;
}

namespace
{
	/**
	 * Gameplay entry points: the script functions the game calls on script actors / objects (UScriptBridge.cpp: spawn and
	 * destroy events, timers, tick, state changes, the UE2 event system, the mutator and game rules chains). Config and
	 * menu code (FillPlayInfo, GetDescriptionText, MutatorFillPlayInfo, server browser queries ...) is never called by the
	 * game, so natives that only it reaches do not stop a class from running.
	 */
	const TCHAR* const GEntryPoints[] = {
		TEXT("Spawned"), TEXT("PreBeginPlay"), TEXT("BeginPlay"), TEXT("PostBeginPlay"), TEXT("SetInitialState"), TEXT("PostNetBeginPlay"),
		TEXT("Destroyed"), TEXT("Timer"), TEXT("Tick"), TEXT("Expired"), TEXT("BeginState"), TEXT("EndState"), TEXT("Created"),
		TEXT("Trigger"), TEXT("UnTrigger"), TEXT("Touch"), TEXT("UnTouch"), TEXT("TakeDamage"),
		TEXT("AddMutator"), TEXT("CheckRelevance"), TEXT("AlwaysKeep"), TEXT("IsRelevant"), TEXT("CheckReplacement"),
		TEXT("GetInventoryClassOverride"), TEXT("GetDefaultWeapon"), TEXT("ModifyPlayer"), TEXT("ScoreKill"),
		TEXT("AddGameRules"), TEXT("NetDamage"), TEXT("GetString") };

	/** Reachability of script code from the gameplay entry points of one class (and the helper classes it spawns). */
	struct FUSReach
	{
		FUSClass* Root = nullptr;
		FUSClass* MutatorClass = nullptr;
		FUSClass* GameRulesClass = nullptr;
		/** Nodes walked / queued, with the class of the object running them. */
		TSet<FUSStruct*> Seen;
		TArray<TPair<FUSStruct*, FUSClass*>> Queue;
		TSet<FUSClass*> Roots;
		/** "Class.Function (from Class.Function)" of every missing native reached. */
		TArray<FString> Missing;
		TSet<FUSFunction*> MissingSeen;

		/** The class running Node's code: the analysed class when it inherits the code, else the code's owner. */
		FUSClass* SelfFor(FUSClass* Owner, FUSClass* Self) const
		{
			return (Owner && Self && Self->IsChildOf(Owner)) ? Self : Owner;
		}

		void Add(FUSFunction* F, FUSClass* Self, FUSStruct* From)
		{
			if (!F)
			{
				return;
			}
			if (F->FunctionFlags & USFlags::FUNC_Native)
			{
				if (!F->Func && !MissingSeen.Contains(F))
				{
					MissingSeen.Add(F);
					FUSClass* Owner = F->GetOwnerClass();
					Missing.Add(FString::Printf(TEXT("%s.%s (from %s)"), Owner ? *Owner->GetName() : TEXT("?"), *F->GetName(),
						From ? *From->GetPathName() : TEXT("?")));
				}
				return;
			}
			if (!Seen.Contains(F))
			{
				Seen.Add(F);
				Queue.Add({ F, SelfFor(F->GetOwnerClass(), Self) });
			}
		}

		/** Every function Name may find on an object of class K (class scope and every state). */
		void AddByName(FUSClass* K, FName Name, FUSStruct* From)
		{
			if (!K || Name == NAME_None || !K->EnsureLoaded())
			{
				return;
			}
			Add(K->FindFunction(Name), K, From);
			for (const TPair<FName, FUSState*>& KV : K->StateMap)
			{
				if (FUSFunction* const* F = KV.Value->FuncMap.Find(Name))
				{
					Add(*F, K, From);
				}
			}
		}

		/** Classes the code names (class'X' constants, class defaults): helper mutators, game rules and the package's own classes run too. */
		void NoteClass(FUSObject* O)
		{
			if (!O || !(O->Flags & USF_Field) || !O->Class || O->Class->Name != TEXT("Class"))
			{
				return;
			}
			FUSClass* K = static_cast<FUSClass*>(O);
			if (Roots.Contains(K) || !K->EnsureLoaded())
			{
				return;
			}
			const bool bHelper = (MutatorClass && K->IsChildOf(MutatorClass)) || (GameRulesClass && K->IsChildOf(GameRulesClass))
				|| K->GetPackageName() == Root->GetPackageName();
			FString Ignored;
			if (bHelper && USGetClassSupport(K, Ignored) == EUSClassSupport::RunsInVM)
			{
				AddRoot(K);
			}
		}

		void AddRoot(FUSClass* K)
		{
			Roots.Add(K);
			for (const TCHAR* Entry : GEntryPoints)
			{
				AddByName(K, FName(Entry), K);
			}
			// state code runs every tick
			for (const TPair<FName, FUSState*>& KV : K->StateMap)
			{
				for (FUSStruct* S = KV.Value; S; S = S->GetSuperStruct())
				{
					if (!Seen.Contains(S))
					{
						Seen.Add(S);
						Queue.Add({ S, K });
					}
				}
			}
			// class references in the defaults (e.g. a game rules class spawned by its property)
			for (FUSProperty* P : K->AllProps)
			{
				if (P->Type == EUSType::Class && K->Defaults.Num() >= P->Offset + P->GetSize())
				{
					for (int32 i = 0; i < P->ArrayDim; ++i)
					{
						FUSObject* Ref = nullptr;
						FMemory::Memcpy(&Ref, K->Defaults.GetData() + P->Offset + i * P->ElementSize, sizeof(Ref));
						NoteClass(Ref);
					}
				}
			}
		}

		void Run(FUSClass* InRoot)
		{
			FUSRuntime& RT = FUSRuntime::Get();
			Root = InRoot;
			MutatorClass = RT.FindClass(TEXT("Engine.Mutator"));
			GameRulesClass = RT.FindClass(TEXT("Engine.GameRules"));
			AddRoot(Root);
			for (int32 Qi = 0; Qi < Queue.Num() && Qi < 20000; ++Qi)
			{
				FUSStruct* Node = Queue[Qi].Key;
				FUSClass* Self = Queue[Qi].Value;
				TArray<USVM::FCallSite> Calls;
				TArray<FUSObject*> Objects;
				USVM::ForEachCall(Node, Self, [&Calls](const USVM::FCallSite& Site) { Calls.Add(Site); },
					[&Objects](FUSObject* O) { Objects.Add(O); });
				for (const USVM::FCallSite& Site : Calls)
				{
					if (Site.NativeIndex != INDEX_NONE && !Site.Function)
					{
						Missing.AddUnique(FString::Printf(TEXT("native %d (from %s)"), Site.NativeIndex, *Node->GetPathName()));
						continue;
					}
					if (Site.Name == NAME_None)
					{
						Add(Site.Function, Self, Node);
						continue;
					}
					// virtual call: whatever the name finds on the context's class, and on the analysed class when it is one
					AddByName(Site.ContextClass, Site.Name, Node);
					for (FUSClass* R : Roots)
					{
						if (Site.ContextClass && R != Site.ContextClass && R->IsChildOf(Site.ContextClass))
						{
							AddByName(R, Site.Name, Node);
						}
					}
				}
				for (FUSObject* O : Objects)
				{
					NoteClass(O);
				}
			}
		}
	};

	bool CanRunClass(FUSClass* C, FString& OutReason)
	{
		if (!C->EnsureLoaded())
		{
			OutReason = C->LoadError.IsEmpty() ? FString::Printf(TEXT("%s cannot be loaded"), *C->GetPathName()) : C->LoadError;
			return false;
		}
		if (USGetClassSupport(C, OutReason) != EUSClassSupport::RunsInVM)
		{
			return false;
		}
		// only natives that gameplay can reach count; a call to a missing native at run time still logs once and
		// returns zero / None (UScriptVM.cpp, USCallMissingNative)
		FUSReach Reach;
		Reach.Run(C);
		if (Reach.Missing.Num())
		{
			TArray<FString> Shown(Reach.Missing.GetData(), FMath::Min(Reach.Missing.Num(), 4));
			OutReason = FString::Printf(TEXT("unimplemented natives reached from gameplay code: %s%s"), *FString::Join(Shown, TEXT(", ")),
				Reach.Missing.Num() > Shown.Num() ? *FString::Printf(TEXT(" and %d more"), Reach.Missing.Num() - Shown.Num()) : TEXT(""));
			return false;
		}
		return true;
	}

	FDelayedAutoRegisterHelper GUScriptTestHook(EDelayedRegisterRunPhase::EndOfEngineInit, []()
	{
		// custom content browser: maps whose script classes the VM can run are no longer marked unsupported
		ArenaUserContent::ScriptClassSupport() = [](const FString& ClassPath) { return ArenaUScript::CanRun(ClassPath); };
		ArenaUserContent::RefreshMapStatus();

		if (FParse::Param(FCommandLine::Get(), TEXT("UScriptCanRunReport")))
		{
			ArenaUScript::LogMutatorSupport();
		}
		if (!FParse::Param(FCommandLine::Get(), TEXT("UScriptTest")))
		{
			return;
		}
		int32 Passed = 0, Failed = 0;
		ArenaUScript::RunSelfTests(Passed, Failed);
		UE_LOG(LogUScript, Display, TEXT("UScriptTest: %d passed, %d failed"), Passed, Failed);
		if (FParse::Param(FCommandLine::Get(), TEXT("UScriptTestQuit")))
		{
			FPlatformMisc::RequestExit(false, TEXT("UScriptTestQuit"));
		}
	});
}

bool ArenaUScript::IsAvailable()
{
	return true;
}

bool ArenaUScript::EnsureRuntime(FString* OutError)
{
	FUSRuntime& RT = FUSRuntime::Get();
	if (RT.IsInitialized())
	{
		return true;
	}
	FString Err;
	// the content cache's copy of the script packages (ArenaConvScripts.h, laid out like an install) first; the install,
	// when there, adds what the cache does not have (texture / sound packages script defaults may refer to)
	const FString CacheScripts = ArenaConvScripts::GetDir(ArenaContent::GetCacheDir());
	const FString Install = ArenaContent::FindUT2004Install();
	const bool bFromCache = ArenaConvScripts::HasScripts(ArenaContent::GetCacheDir());
	if (!RT.Initialize(bFromCache ? CacheScripts : Install, Err))
	{
		if (OutError)
		{
			*OutError = Err;
		}
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogUScript, Warning, TEXT("UScript runtime unavailable: %s"), *Err);
		}
		return false;
	}
	if (bFromCache && !Install.IsEmpty())
	{
		RT.AddPackageFolder(Install);
	}
	TArray<FString> Files;
	for (const TPair<FString, FString>& KV : ArenaUserContent::GetScriptPackages())
	{
		Files.Add(KV.Value);
	}
	RT.AddPackageFiles(Files);
	return true;
}

bool ArenaUScript::CanRun(const FString& PackageOrClass, FString* OutReason)
{
	FString Reason;
	bool bOk = false;
	if (EnsureRuntime(&Reason))
	{
		FUSRuntime& RT = FUSRuntime::Get();
		if (PackageOrClass.Contains(TEXT(".")))
		{
			FUSClass* C = RT.FindClass(PackageOrClass, &Reason);
			bOk = C && CanRunClass(C, Reason);
		}
		else if (FUSLinker* L = RT.LoadPackage(FName(*PackageOrClass), &Reason))
		{
			bOk = true;
			for (int32 Ei = 0; Ei < L->NumExports() && bOk; ++Ei)
			{
				if (L->IsTopLevelClass(Ei))
				{
					FUSClass* C = static_cast<FUSClass*>(L->GetExport(Ei));
					bOk = C && CanRunClass(C, Reason);
				}
			}
		}
	}
	if (OutReason)
	{
		*OutReason = bOk ? FString() : Reason;
	}
	return bOk;
}

void ArenaUScript::RunSelfTests(int32& OutPassed, int32& OutFailed)
{
	USRunSelfTests(OutPassed, OutFailed);
}

void ArenaUScript::LogMutatorSupport()
{
	if (!EnsureRuntime())
	{
		return;
	}
	FUSRuntime& RT = FUSRuntime::Get();
	FUSClass* MutatorClass = RT.FindClass(TEXT("Engine.Mutator"));
	TArray<FString> Files;
	const FString Dir = ArenaConvScripts::GetDir(ArenaContent::GetCacheDir()) / TEXT("System");
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.u")), true, false);
	Files.Sort();
	int32 Num = 0, NumOk = 0;
	for (const FString& File : Files)
	{
		FUSLinker* L = MutatorClass ? RT.LoadPackage(FName(*FPaths::GetBaseFilename(File))) : nullptr;
		for (int32 Ei = 0; L && Ei < L->NumExports(); ++Ei)
		{
			FUSClass* C = L->IsTopLevelClass(Ei) ? static_cast<FUSClass*>(L->GetExport(Ei)) : nullptr;
			if (!C || C == MutatorClass || !C->EnsureLoaded() || !C->IsChildOf(MutatorClass) || (C->ClassFlags & USFlags::CLASS_Abstract))
			{
				continue;
			}
			FString Reason;
			const bool bOk = CanRun(C->GetPathName(), &Reason);
			++Num;
			NumOk += bOk ? 1 : 0;
			UE_LOG(LogUScript, Display, TEXT("UScriptCanRun: %s: %s"), *C->GetPathName(), bOk ? TEXT("ok") : *Reason);
		}
	}
	UE_LOG(LogUScript, Display, TEXT("UScriptCanRun: %d of %d mutator classes can run"), NumOk, Num);
}

#else // OPENUT2K4_USCRIPT_ENABLED

bool ArenaUScript::IsAvailable() { return false; }
bool ArenaUScript::EnsureRuntime(FString* OutError) { if (OutError) { *OutError = TEXT("the script runtime is not part of this build"); } return false; }
bool ArenaUScript::CanRun(const FString& PackageOrClass, FString* OutReason) { if (OutReason) { *OutReason = TEXT("the script runtime is not part of this build"); } return false; }
void ArenaUScript::RunSelfTests(int32& OutPassed, int32& OutFailed) { OutPassed = 0; OutFailed = 0; }
void ArenaUScript::LogMutatorSupport() {}
void ArenaUScriptHooks::InitGame(AArenaGameMode*, const FString&) {}
void ArenaUScriptHooks::LevelBuilt(AArenaGameMode*) {}
bool ArenaUScriptHooks::FilterPickup(UWorld*, const FVector&, EArenaPickupType&, TSubclassOf<AArenaWeapon>&) { return true; }
void ArenaUScriptHooks::ModifyDefaultInventory(AController*, TArray<TSubclassOf<AArenaWeapon>>&) {}
void ArenaUScriptHooks::PawnSpawned(AArenaCharacter*) {}
float ArenaUScriptHooks::ModifyDamage(float Damage, AController*, AArenaCharacter*) { return Damage; }
void ArenaUScriptHooks::Killed(AController*, AController*, AArenaCharacter*) {}
bool ArenaUScriptHooks::IsActive(const UWorld*) { return false; }
void ArenaUScriptHooks::ScriptActorEvent(AActor*, AActor*, APawn*, int32) {}
void ArenaUScriptHooks::ScriptActorDamage(AActor*, float, AController*, AActor*) {}

#endif // OPENUT2K4_USCRIPT_ENABLED

FString ArenaUScript::GetUnsupportedReason(const FString& PackageOrClass)
{
	FString Reason;
	CanRun(PackageOrClass, &Reason);
	return Reason;
}
