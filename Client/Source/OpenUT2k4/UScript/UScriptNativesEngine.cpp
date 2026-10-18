// UScript natives of Engine classes (Actor, LevelInfo, GameInfo, Controller, ...), bridged to the game world.
// Values are UE2 units; FUSGameWorld converts at the bridge.

#include "UScriptCore.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "UScriptWorld.h"
#include "UScriptBridge.h"
#include "../ArenaCharacter.h"
#include "../ArenaGameMode.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "../ArenaRuntimeContent.h"

namespace
{
	FORCEINLINE FUSObject* Live(FUSObject* O) { return (O && !O->IsPendingKill()) ? O : nullptr; }

	/** The script world of an object (FUSGameWorld when it lives in a game world). */
	FUSGameWorld* GW(FUSObject* O)
	{
		if (!O || !O->World || O->World == &FUSWorld::Transient())
		{
			return nullptr;
		}
		return static_cast<FUSGameWorld*>(O->World);
	}

	UWorld* UW(FUSObject* O)
	{
		FUSGameWorld* W = GW(O);
		return W ? W->GetUWorld() : nullptr;
	}

	bool IsClass(FUSObject* O)
	{
		return O && (O->Flags & USF_Field) && O->Class && O->Class->Name == TEXT("Class");
	}

	FUSVector Loc(FUSObject* O) { return USProps().Location.Get<FUSVector>(O); }

	/** Runs an actor iterator over a snapshot of the world's actors. */
	void IterateActors(FUSObject* Self, FUSFrame& Stack, FUSObject** Out, FUSClass* BaseClass, TFunctionRef<bool(FUSObject*)> Filter)
	{
		FUSWorld* W = Self ? Self->World : nullptr;
		TArray<FUSObject*> Snapshot;
		if (FUSGameWorld* G = GW(Self))
		{
			Snapshot = G->GetActors();
		}
		else if (W)
		{
			for (FUSObject* O : W->GetObjects())
			{
				if (O->Flags & USF_Actor)
				{
					Snapshot.Add(O);
				}
			}
		}
		int32 I = 0;
		USRunIterator(Stack, [&]() -> bool
		{
			while (I < Snapshot.Num())
			{
				FUSObject* A = Snapshot[I++];
				if (Live(A) && (!BaseClass || A->IsA(BaseClass)) && Filter(A))
				{
					*Out = A;
					return true;
				}
			}
			*Out = nullptr;
			return false;
		});
	}

	bool LineTrace(FUSObject* Self, const FUSVector& Start, const FUSVector& End, FHitResult& Hit)
	{
		UWorld* World = UW(Self);
		if (!World)
		{
			return false;
		}
		FCollisionQueryParams Params(SCENE_QUERY_STAT(UScriptTrace), true);
		if (FUSGameWorld* G = GW(Self))
		{
			if (AActor* Ignore = G->GetLinkedActor(Self))
			{
				Params.AddIgnoredActor(Ignore);
			}
		}
		return World->LineTraceSingleByChannel(Hit, USUnits::FromUE2(Start), USUnits::FromUE2(End), ECC_Visibility, Params);
	}
}

namespace USEngineNatives
{
	// ---- spawning / destruction ----
	US_NATIVE(Spawn)
	{
		USP_GET_OBJECT(ClassObj);
		USP_GET_OBJECT_OPTX(SpawnOwner, nullptr);
		USP_GET_NAME_OPTX(SpawnTag, NAME_None);
		USP_GET_VECTOR_OPTX(SpawnLocation, Loc(Self));
		USP_GET_ROTATOR_OPTX(SpawnRotation, USProps().Rotation.Get<FUSRotator>(Self));
		USP_FINISH;
		FUSObject* Spawned = nullptr;
		if (IsClass(ClassObj) && Self && Self->World)
		{
			Spawned = Self->World->SpawnActor(static_cast<FUSClass*>(ClassObj), Live(SpawnOwner), SpawnTag, &SpawnLocation, &SpawnRotation, Self);
		}
		US_RESULT(FUSObject*) = Spawned;
	}

	US_NATIVE(Destroy)
	{
		USP_FINISH;
		US_RESULT(uint32) = (Self && Self->World && Self->World->DestroyActor(Self)) ? 1u : 0u;
	}

	US_NATIVE(ActorError)
	{
		USP_GET_STR(S);
		USP_FINISH;
		UE_LOG(LogUScript, Warning, TEXT("%s: Error: %s (actor destroyed)"), *Stack.Describe(), *S);
		if (Self && Self->World)
		{
			Self->World->DestroyActor(Self);
		}
	}

	US_NATIVE(SetTimer)
	{
		USP_GET_FLOAT(Rate);
		USP_GET_UBOOL(bLoop);
		USP_FINISH;
		FUSEngineProps& P = USProps();
		P.TimerCounter.Set<float>(Self, 0.f);
		P.TimerRate.Set<float>(Self, Rate);
		P.bTimerLoop.Set<uint32>(Self, bLoop ? 1u : 0u);
	}

	// ---- latent ----
	US_NATIVE(Sleep)
	{
		USP_GET_FLOAT(Seconds);
		USP_FINISH;
		if (Self && Self->StateFrame)
		{
			Self->StateFrame->LatentAction = USLatent::Sleep;
			Self->StateFrame->LatentFloat = Seconds;
		}
	}
	US_NATIVE(FinishAnim)
	{
		USP_GET_INT_OPTX(Channel, 0);
		USP_FINISH;
		if (Self && Self->StateFrame)
		{
			Self->StateFrame->LatentAction = USLatent::FinishAnim;
			Self->StateFrame->LatentFloat = 0.f;
		}
	}
	US_NATIVE(FinishInterpolation)
	{
		USP_FINISH;
		if (Self && Self->StateFrame)
		{
			Self->StateFrame->LatentAction = USLatent::FinishInterpolation;
			Self->StateFrame->LatentFloat = 0.f;
		}
	}

	// ---- movement / placement ----
	US_NATIVE(SetLocation)
	{
		USP_GET_VECTOR(NewLoc);
		USP_FINISH;
		FUSGameWorld* G = GW(Self);
		AActor* A = G ? G->GetLinkedActor(Self) : nullptr;
		bool bOk = true;
		if (A)
		{
			bOk = A->SetActorLocation(USUnits::FromUE2(NewLoc), false, nullptr, ETeleportType::TeleportPhysics);
		}
		if (bOk)
		{
			USProps().Location.Set<FUSVector>(Self, NewLoc);
		}
		US_RESULT(uint32) = bOk ? 1u : 0u;
	}
	US_NATIVE(SetRotation)
	{
		USP_GET_ROTATOR(NewRot);
		USP_FINISH;
		FUSGameWorld* G = GW(Self);
		if (AActor* A = G ? G->GetLinkedActor(Self) : nullptr)
		{
			A->SetActorRotation(USUnits::FromUE2(NewRot));
		}
		USProps().Rotation.Set<FUSRotator>(Self, NewRot);
		US_RESULT(uint32) = 1u;
	}
	US_NATIVE(Move)
	{
		USP_GET_VECTOR(Delta);
		USP_FINISH;
		FUSVector L = Loc(Self) + Delta;
		FUSGameWorld* G = GW(Self);
		if (AActor* A = G ? G->GetLinkedActor(Self) : nullptr)
		{
			A->SetActorLocation(USUnits::FromUE2(L), true);
			L = USUnits::ToUE2(A->GetActorLocation());
		}
		USProps().Location.Set<FUSVector>(Self, L);
		US_RESULT(uint32) = 1u;
	}
	US_NATIVE(SetPhysics)
	{
		USP_GET_BYTE(NewPhysics);
		USP_FINISH;
		USProps().Physics.Set<uint8>(Self, NewPhysics);
	}
	US_NATIVE(SetOwner)
	{
		USP_GET_OBJECT(NewOwner);
		USP_FINISH;
		USProps().Owner.Set<FUSObject*>(Self, NewOwner);
	}
	US_NATIVE(SetBase)
	{
		USP_GET_OBJECT(NewBase);
		USP_GET_VECTOR_OPTX(Floor, FUSVector(0, 0, 1));
		USP_FINISH;
		USProps().Base.Set<FUSObject*>(Self, NewBase);
	}
	US_NATIVE(SetCollision)
	{
		USP_GET_UBOOL_OPTX(bCol, false);
		USP_GET_UBOOL_OPTX(bBlockAct, false);
		USP_GET_UBOOL_OPTX(bBlockPl, false);
		USP_FINISH;
		if (FUSProperty* P = Self ? Self->FindProperty(TEXT("bCollideActors")) : nullptr)
		{
			*reinterpret_cast<uint32*>(Self->Data + P->Offset) = bCol ? 1u : 0u;
		}
		if (FUSProperty* P = Self ? Self->FindProperty(TEXT("bBlockActors")) : nullptr)
		{
			*reinterpret_cast<uint32*>(Self->Data + P->Offset) = bBlockAct ? 1u : 0u;
		}
	}
	US_NATIVE(SetCollisionSize)
	{
		USP_GET_FLOAT(R);
		USP_GET_FLOAT(H);
		USP_FINISH;
		USProps().CollisionRadius.Set<float>(Self, R);
		USProps().CollisionHeight.Set<float>(Self, H);
		US_RESULT(uint32) = 1u;
	}
	US_NATIVE(SetDrawScale)
	{
		USP_GET_FLOAT(S);
		USP_FINISH;
		USProps().DrawScale.Set<float>(Self, S);
	}

	// ---- traces ----
	US_NATIVE(Trace)
	{
		USP_GET_VECTOR_REF(HitLocation);
		USP_GET_VECTOR_REF(HitNormal);
		USP_GET_VECTOR(TraceEnd);
		USP_GET_VECTOR_OPTX(TraceStart, Loc(Self));
		USP_GET_UBOOL_OPTX(bTraceActors, false);
		USP_GET_VECTOR_OPTX(Extent, FUSVector());
		USP_GET_OBJECT_REF(Material);
		USP_FINISH;
		FHitResult Hit;
		FUSObject* HitActor = nullptr;
		if (LineTrace(Self, TraceStart, TraceEnd, Hit))
		{
			*HitLocation = USUnits::ToUE2(Hit.ImpactPoint);
			*HitNormal = FUSVector(Hit.ImpactNormal);
			FUSGameWorld* G = GW(Self);
			FUSObject* Proxy = nullptr;
			if (G && Hit.GetActor())
			{
				Proxy = G->ProxyForPawn(Cast<APawn>(Hit.GetActor()), false);
			}
			HitActor = Proxy ? Proxy : (G ? G->Level : nullptr);
		}
		US_RESULT(FUSObject*) = HitActor;
	}
	US_NATIVE(FastTrace)
	{
		USP_GET_VECTOR(TraceEnd);
		USP_GET_VECTOR_OPTX(TraceStart, Loc(Self));
		USP_FINISH;
		FHitResult Hit;
		US_RESULT(uint32) = LineTrace(Self, TraceStart, TraceEnd, Hit) ? 0u : 1u;
	}

	// ---- iterators ----
	US_NATIVE(AllActors)
	{
		USP_GET_OBJECT(BaseClass);
		USP_GET_OBJECT_REF(Out);
		USP_GET_NAME_OPTX(MatchTag, NAME_None);
		USP_FINISH;
		IterateActors(Self, Stack, Out, IsClass(BaseClass) ? static_cast<FUSClass*>(BaseClass) : nullptr,
			[&](FUSObject* A) { return MatchTag == NAME_None || USProps().Tag.Get<FName>(A) == MatchTag; });
	}
	US_NATIVE(DynamicActors)
	{
		USP_GET_OBJECT(BaseClass);
		USP_GET_OBJECT_REF(Out);
		USP_GET_NAME_OPTX(MatchTag, NAME_None);
		USP_FINISH;
		IterateActors(Self, Stack, Out, IsClass(BaseClass) ? static_cast<FUSClass*>(BaseClass) : nullptr,
			[&](FUSObject* A) { return !USProps().bStatic.Get<uint32>(A) && (MatchTag == NAME_None || USProps().Tag.Get<FName>(A) == MatchTag); });
	}
	US_NATIVE(ChildActors)
	{
		USP_GET_OBJECT(BaseClass);
		USP_GET_OBJECT_REF(Out);
		USP_FINISH;
		IterateActors(Self, Stack, Out, IsClass(BaseClass) ? static_cast<FUSClass*>(BaseClass) : nullptr,
			[&](FUSObject* A) { return USProps().Owner.Get<FUSObject*>(A) == Self; });
	}
	US_NATIVE(BasedActors)
	{
		USP_GET_OBJECT(BaseClass);
		USP_GET_OBJECT_REF(Out);
		USP_FINISH;
		IterateActors(Self, Stack, Out, IsClass(BaseClass) ? static_cast<FUSClass*>(BaseClass) : nullptr,
			[&](FUSObject* A) { return USProps().Base.Get<FUSObject*>(A) == Self; });
	}
	US_NATIVE(TouchingActors)
	{
		USP_GET_OBJECT(BaseClass);
		USP_GET_OBJECT_REF(Out);
		USP_FINISH;
		IterateActors(Self, Stack, Out, nullptr, [&](FUSObject* A) { return false; });
	}
	static void RadiusIter(FUSObject* Self, FUSFrame& Stack, bool bVisible, bool bColliding)
	{
		USP_GET_OBJECT(BaseClass);
		USP_GET_OBJECT_REF(Out);
		USP_GET_FLOAT_OPTX(Radius, 1000.f);
		USP_GET_VECTOR_OPTX(Center, Loc(Self));
		USP_GET_UBOOL_OPTX(bIgnoreHidden, false);
		USP_FINISH;
		const float R2 = Radius * Radius;
		IterateActors(Self, Stack, Out, IsClass(BaseClass) ? static_cast<FUSClass*>(BaseClass) : nullptr, [&](FUSObject* A)
		{
			if ((Loc(A) - Center).SizeSquared() > R2)
			{
				return false;
			}
			if (bColliding && !USProps().bStatic.Get<uint32>(A) && A->FindProperty(TEXT("bCollideActors"))
				&& !*reinterpret_cast<uint32*>(A->Data + A->FindProperty(TEXT("bCollideActors"))->Offset))
			{
				return false;
			}
			if (bIgnoreHidden && USProps().bHidden.Get<uint32>(A))
			{
				return false;
			}
			if (bVisible)
			{
				FHitResult Hit;
				return !LineTrace(Self, Center, Loc(A), Hit);
			}
			return true;
		});
	}
	US_NATIVE(RadiusActors) { RadiusIter(Self, Stack, false, false); }
	US_NATIVE(VisibleActors) { RadiusIter(Self, Stack, true, false); }
	US_NATIVE(VisibleCollidingActors) { RadiusIter(Self, Stack, true, true); }
	US_NATIVE(CollidingActors) { RadiusIter(Self, Stack, false, true); }
	US_NATIVE(ZoneActors)
	{
		USP_GET_OBJECT(BaseClass);
		USP_GET_OBJECT_REF(Out);
		USP_FINISH;
		IterateActors(Self, Stack, Out, IsClass(BaseClass) ? static_cast<FUSClass*>(BaseClass) : nullptr, [&](FUSObject* A) { return true; });
	}

	// ---- controller list ----
	US_NATIVE(AddController)
	{
		USP_FINISH;
		if (FUSGameWorld* G = GW(Self))
		{
			G->LinkController(Self, true);
		}
	}
	US_NATIVE(RemoveController)
	{
		USP_FINISH;
		if (FUSGameWorld* G = GW(Self))
		{
			G->LinkController(Self, false);
		}
	}
	US_NATIVE(LineOfSightTo)
	{
		USP_GET_OBJECT(Other);
		USP_FINISH;
		FUSObject* Pawn = USProps().ControllerPawn.Get<FUSObject*>(Self);
		FHitResult Hit;
		US_RESULT(uint32) = (Live(Other) && Pawn && !LineTrace(Pawn, Loc(Pawn), Loc(Other), Hit)) ? 1u : 0u;
	}
	US_NATIVE(LatentMoveDone)
	{
		USSkipNativeParms(Stack);
	}

	// ---- info ----
	US_NATIVE(GetURLMap)
	{
		USP_FINISH;
		FUSGameWorld* G = GW(Self);
		US_RESULT(FString) = G ? G->MapName : FString();
	}
	US_NATIVE(GetLocalURL)
	{
		USP_FINISH;
		FUSGameWorld* G = GW(Self);
		US_RESULT(FString) = G ? FString::Printf(TEXT("%s?Game=%s"), *G->MapName, G->Game ? *G->Game->Class->GetPathName() : TEXT("")) : FString();
	}
	US_NATIVE(GetAddressURL) { USP_FINISH; US_RESULT(FString) = TEXT("127.0.0.1:7777"); }
	US_NATIVE(ConsoleCommand)
	{
		USP_GET_STR(Cmd);
		USP_GET_UBOOL_OPTX(bLog, false);
		USP_FINISH;
		UE_LOG(LogUScript, Log, TEXT("%s: ConsoleCommand \"%s\" (ignored)"), *Stack.Describe(), *Cmd);
		US_RESULT(FString) = FString();
	}
	US_NATIVE(ParseKillMessage)
	{
		USP_GET_STR(KillerName);
		USP_GET_STR(VictimName);
		USP_GET_STR(Message);
		USP_FINISH;
		US_RESULT(FString) = Message.Replace(TEXT("%k"), *KillerName).Replace(TEXT("%o"), *VictimName);
	}
	US_NATIVE(GetNetworkNumber) { USP_FINISH; US_RESULT(FString) = TEXT("127.0.0.1"); }
	US_NATIVE(GetPhysicsVolume)
	{
		USP_GET_VECTOR(L);
		USP_FINISH;
		FUSGameWorld* G = GW(Self);
		US_RESULT(FUSObject*) = G ? G->DefaultPhysicsVolume : nullptr;
	}

	// ---- colours ----
	static FUSColor ClampColor(int32 R, int32 G, int32 B, int32 A)
	{
		FUSColor C;
		C.R = uint8(FMath::Clamp(R, 0, 255)); C.G = uint8(FMath::Clamp(G, 0, 255)); C.B = uint8(FMath::Clamp(B, 0, 255)); C.A = uint8(FMath::Clamp(A, 0, 255));
		return C;
	}
	US_NATIVE(Add_ColorColor) { USP_GET_STRUCT(FUSColor, A); USP_GET_STRUCT(FUSColor, B); USP_FINISH; US_RESULT(FUSColor) = ClampColor(A.R + B.R, A.G + B.G, A.B + B.B, A.A + B.A); }
	US_NATIVE(Subtract_ColorColor) { USP_GET_STRUCT(FUSColor, A); USP_GET_STRUCT(FUSColor, B); USP_FINISH; US_RESULT(FUSColor) = ClampColor(A.R - B.R, A.G - B.G, A.B - B.B, A.A - B.A); }
	US_NATIVE(Multiply_ColorFloat) { USP_GET_STRUCT(FUSColor, A); USP_GET_FLOAT(F); USP_FINISH; US_RESULT(FUSColor) = ClampColor(int32(A.R * F), int32(A.G * F), int32(A.B * F), int32(A.A * F)); }
	US_NATIVE(Multiply_FloatColor) { USP_GET_FLOAT(F); USP_GET_STRUCT(FUSColor, A); USP_FINISH; US_RESULT(FUSColor) = ClampColor(int32(A.R * F), int32(A.G * F), int32(A.B * F), int32(A.A * F)); }

	// ---- sound ----
	US_NATIVE(PlaySound)
	{
		USP_GET_OBJECT(Sound);
		USP_GET_BYTE_OPTX(Slot, 0);
		USP_GET_FLOAT_OPTX(Volume, 1.f);
		USP_GET_UBOOL_OPTX(bNoOverride, false);
		USP_GET_FLOAT_OPTX(Radius, 0.f);
		USP_GET_FLOAT_OPTX(Pitch, 1.f);
		USP_GET_UBOOL_OPTX(bAttenuate, true);
		USP_FINISH;
		UWorld* World = UW(Self);
		if (!Sound || !World)
		{
			return;
		}
		if (USoundBase* S = ArenaRuntimeContent::LoadSoundByObjectPath(Sound->GetPathName()))
		{
			FUSGameWorld* G = GW(Self);
			AActor* A = G ? G->GetLinkedActor(Self) : nullptr;
			const FVector L = A ? A->GetActorLocation() : USUnits::FromUE2(Loc(Self));
			UGameplayStatics::PlaySoundAtLocation(World, S, L, FMath::Clamp(Volume, 0.f, 2.f), FMath::Clamp(Pitch, 0.25f, 4.f));
		}
	}

	// ---- StreamBase (music player paths) ----
	US_NATIVE(GetPathSeparator) { USP_FINISH; US_RESULT(FString) = TEXT("/"); }
	US_NATIVE(IsCaseSensitive) { USP_FINISH; US_RESULT(uint32) = PLATFORM_LINUX ? 1u : 0u; }
	US_NATIVE(Multiply_StrStr) { USP_GET_STR(A); USP_GET_STR(B); USP_FINISH; US_RESULT(FString) = A.IsEmpty() ? B : (A.EndsWith(TEXT("/")) ? A + B : A + TEXT("/") + B); }
	US_NATIVE(MultiplyEqual_StrStr)
	{
		USP_GET_STR_REF(A); USP_GET_STR(B); USP_FINISH;
		*A = A->IsEmpty() ? B : (A->EndsWith(TEXT("/")) ? *A + B : *A + TEXT("/") + B);
		US_RESULT(FString) = *A;
	}

	// ---- presentation (no visual effect for script actors yet): consume parameters quietly ----
	US_NATIVE(Quiet) { USSkipNativeParms(Stack); }
	US_NATIVE(ReturnFalse) { USSkipNativeParms(Stack); if (Result) { US_RESULT(uint32) = 0; } }
	US_NATIVE(SetBoneScale)
	{
		USP_GET_INT(Slot);
		USP_GET_FLOAT_OPTX(Scale, 1.f);
		USP_GET_NAME_OPTX(Bone, NAME_None);
		USP_FINISH;
		if (Bone == FName(TEXT("head")))
		{
			USProps().HeadScale.Set<float>(Self, Scale);
		}
	}
}

void USRegisterBridgeOverrides();

void USNatives::RegisterEngine()
{
	USRegisterBridgeOverrides();
	using namespace USEngineNatives;
	static const FUSNativeEntry Entries[] = {
		{ TEXT("Actor"), TEXT("Spawn"), &Spawn }, { TEXT("Actor"), TEXT("Destroy"), &Destroy }, { TEXT("Actor"), TEXT("Error"), &ActorError },
		{ TEXT("Actor"), TEXT("SetTimer"), &SetTimer }, { TEXT("Actor"), TEXT("Sleep"), &Sleep }, { TEXT("Actor"), TEXT("FinishAnim"), &FinishAnim },
		{ TEXT("Actor"), TEXT("FinishInterpolation"), &FinishInterpolation },
		{ TEXT("Actor"), TEXT("SetLocation"), &SetLocation }, { TEXT("Actor"), TEXT("SetRotation"), &SetRotation }, { TEXT("Actor"), TEXT("Move"), &Move },
		{ TEXT("Actor"), TEXT("SetPhysics"), &SetPhysics }, { TEXT("Actor"), TEXT("SetOwner"), &SetOwner }, { TEXT("Actor"), TEXT("SetBase"), &SetBase },
		{ TEXT("Actor"), TEXT("SetCollision"), &SetCollision }, { TEXT("Actor"), TEXT("SetCollisionSize"), &SetCollisionSize },
		{ TEXT("Actor"), TEXT("SetDrawScale"), &SetDrawScale },
		{ TEXT("Actor"), TEXT("Trace"), &Trace }, { TEXT("Actor"), TEXT("FastTrace"), &FastTrace },
		{ TEXT("Actor"), TEXT("AllActors"), &AllActors }, { TEXT("Actor"), TEXT("DynamicActors"), &DynamicActors },
		{ TEXT("Actor"), TEXT("ChildActors"), &ChildActors }, { TEXT("Actor"), TEXT("BasedActors"), &BasedActors },
		{ TEXT("Actor"), TEXT("TouchingActors"), &TouchingActors }, { TEXT("Actor"), TEXT("RadiusActors"), &RadiusActors },
		{ TEXT("Actor"), TEXT("VisibleActors"), &VisibleActors }, { TEXT("Actor"), TEXT("VisibleCollidingActors"), &VisibleCollidingActors },
		{ TEXT("Actor"), TEXT("CollidingActors"), &CollidingActors }, { TEXT("ZoneInfo"), TEXT("ZoneActors"), &ZoneActors },
		{ TEXT("Controller"), TEXT("AddController"), &AddController }, { TEXT("Controller"), TEXT("RemoveController"), &RemoveController },
		{ TEXT("Controller"), TEXT("LineOfSightTo"), &LineOfSightTo }, { TEXT("Controller"), TEXT("MoveTo"), &LatentMoveDone },
		{ TEXT("Controller"), TEXT("MoveToward"), &LatentMoveDone }, { TEXT("Controller"), TEXT("FinishRotation"), &LatentMoveDone },
		{ TEXT("Controller"), TEXT("WaitForLanding"), &LatentMoveDone },
		{ TEXT("Actor"), TEXT("GetURLMap"), &GetURLMap }, { TEXT("LevelInfo"), TEXT("GetLocalURL"), &GetLocalURL },
		{ TEXT("LevelInfo"), TEXT("GetAddressURL"), &GetAddressURL }, { TEXT("Actor"), TEXT("ConsoleCommand"), &ConsoleCommand },
		{ TEXT("PlayerController"), TEXT("ConsoleCommand"), &ConsoleCommand },
		{ TEXT("GameInfo"), TEXT("ParseKillMessage"), &ParseKillMessage }, { TEXT("GameInfo"), TEXT("GetNetworkNumber"), &GetNetworkNumber },
		{ TEXT("LevelInfo"), TEXT("GetPhysicsVolume"), &GetPhysicsVolume },
		{ TEXT("Actor"), TEXT("Add_ColorColor"), &Add_ColorColor }, { TEXT("Actor"), TEXT("Subtract_ColorColor"), &Subtract_ColorColor },
		{ TEXT("Actor"), TEXT("Multiply_ColorFloat"), &Multiply_ColorFloat }, { TEXT("Actor"), TEXT("Multiply_FloatColor"), &Multiply_FloatColor },
		{ TEXT("Actor"), TEXT("SetBoneScale"), &SetBoneScale },
		// presentation / physics details without an effect on script logic yet
		{ TEXT("Actor"), TEXT("PlaySound"), &PlaySound }, { TEXT("Actor"), TEXT("PlayOwnedSound"), &PlaySound }, { TEXT("Actor"), TEXT("DemoPlaySound"), &Quiet },
		{ TEXT("Actor"), TEXT("MakeNoise"), &Quiet }, { TEXT("Actor"), TEXT("PlayAnim"), &ReturnFalse }, { TEXT("Actor"), TEXT("LoopAnim"), &ReturnFalse },
		{ TEXT("Actor"), TEXT("TweenAnim"), &ReturnFalse }, { TEXT("Actor"), TEXT("IsAnimating"), &ReturnFalse }, { TEXT("Actor"), TEXT("HasAnim"), &ReturnFalse },
		{ TEXT("Actor"), TEXT("StopAnimating"), &Quiet }, { TEXT("Actor"), TEXT("AnimBlendParams"), &Quiet }, { TEXT("Actor"), TEXT("AnimBlendToAlpha"), &Quiet },
		{ TEXT("Actor"), TEXT("SetDrawType"), &Quiet }, { TEXT("Actor"), TEXT("SetStaticMesh"), &Quiet }, { TEXT("Actor"), TEXT("LinkMesh"), &Quiet },
		{ TEXT("Actor"), TEXT("LinkSkelAnim"), &Quiet }, { TEXT("Actor"), TEXT("SetDrawScale3D"), &Quiet }, { TEXT("Actor"), TEXT("SetBoneRotation"), &Quiet },
		{ TEXT("Actor"), TEXT("SetBoneDirection"), &Quiet }, { TEXT("Actor"), TEXT("SetBoneLocation"), &Quiet }, { TEXT("Actor"), TEXT("Clock"), &Quiet },
		{ TEXT("Actor"), TEXT("UnClock"), &Quiet }, { TEXT("Actor"), TEXT("DrawDebugLine"), &Quiet }, { TEXT("Actor"), TEXT("DrawDebugSphere"), &Quiet },
		{ TEXT("Actor"), TEXT("DrawStayingDebugLine"), &Quiet }, { TEXT("Actor"), TEXT("ClearStayingDebugLines"), &Quiet },
		{ TEXT("Actor"), TEXT("KAddImpulse"), &Quiet }, { TEXT("Actor"), TEXT("KWake"), &Quiet }, { TEXT("Actor"), TEXT("StopAllMusic"), &Quiet },
		{ TEXT("Actor"), TEXT("PlayFeedbackEffect"), &Quiet }, { TEXT("Actor"), TEXT("StopFeedbackEffect"), &Quiet },
		{ TEXT("Actor"), TEXT("PlayerCanSeeMe"), &ReturnFalse }, { TEXT("Actor"), TEXT("AttachToBone"), &ReturnFalse },
		{ TEXT("Actor"), TEXT("DetachFromBone"), &ReturnFalse }, { TEXT("Actor"), TEXT("AutonomousPhysics"), &Quiet }, { TEXT("Actor"), TEXT("MoveSmooth"), &ReturnFalse },
		{ TEXT("Actor"), TEXT("SetRelativeLocation"), &ReturnFalse }, { TEXT("Actor"), TEXT("SetRelativeRotation"), &ReturnFalse },
		{ TEXT("Actor"), TEXT("AddToPackageMap"), &Quiet }, { TEXT("Actor"), TEXT("ResetStaticFilterState"), &Quiet },
		{ TEXT("LevelInfo"), TEXT("IsEntry"), &ReturnFalse }, { TEXT("LevelInfo"), TEXT("IsDemoBuild"), &ReturnFalse },
		{ TEXT("LevelInfo"), TEXT("DetailChange"), &Quiet }, { TEXT("LevelInfo"), TEXT("IsSoftwareRendering"), &ReturnFalse },
		{ TEXT("LevelInfo"), TEXT("IsPendingConnection"), &ReturnFalse }, { TEXT("LevelInfo"), TEXT("UpdateDistanceFogLOD"), &Quiet },
		{ TEXT("LevelInfo"), TEXT("ForceLoadTexture"), &Quiet }, { TEXT("Pickup"), TEXT("AddToNavigation"), &Quiet },
		{ TEXT("Pickup"), TEXT("RemoveFromNavigation"), &Quiet }, { TEXT("Weapon"), TEXT("InitWeaponFires"), &Quiet },
		{ TEXT("Volume"), TEXT("Encompasses"), &ReturnFalse }, { TEXT("Pawn"), TEXT("ReachedDestination"), &ReturnFalse },
		{ TEXT("Controller"), TEXT("CanSee"), &ReturnFalse }, { TEXT("Controller"), TEXT("InLatentExecution"), &ReturnFalse },
		{ TEXT("Controller"), TEXT("StopWaiting"), &Quiet }, { TEXT("PlayerController"), TEXT("ClientTravel"), &Quiet },
		{ TEXT("PlayerController"), TEXT("ResetKeyboard"), &Quiet }, { TEXT("PlayerController"), TEXT("SetViewTarget"), &Quiet },
		{ TEXT("GameInfo"), TEXT("SavePackage"), &ReturnFalse },
		{ TEXT("StreamBase"), TEXT("GetPathSeparator"), &GetPathSeparator }, { TEXT("StreamBase"), TEXT("IsCaseSensitive"), &IsCaseSensitive },
		{ TEXT("StreamBase"), TEXT("Multiply_StrStr"), &Multiply_StrStr }, { TEXT("StreamBase"), TEXT("MultiplyEqual_StrStr"), &MultiplyEqual_StrStr },
	};
	Register(Entries, UE_ARRAY_COUNT(Entries));
}

#endif // OPENUT2K4_USCRIPT_ENABLED
