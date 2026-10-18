#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ArenaMoveTest.generated.h"

class AActor;
class AArenaCharacter;
class AStaticMeshActor;

/**
 * Movement test hook (non-shipping builds): -MoveTest=<name>[,<name>...] or -MoveTest=all. Once the match runs, a
 * flat test rig (floor, wall, steps, slopes, a lift) is built far outside the map and the local player's pawn goes
 * through scripted UT2004 moves fed through the same entry points as the input (direction taps, jump, crouch,
 * fire). Each test logs samples ("MoveTest: <test> t=... z=... vz=... hspeed=...") and "MoveTest: RESULT <test>
 * <metric> expected=<UE2 value> measured=<value>" lines; -MoveTestShots takes screenshots at key moments,
 * -MoveTestQuit exits when done. Use with ?Bots=0.
 */
UCLASS()
class OPENUT2K4_API UArenaMoveTestSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	void BuildRig(const FVector& Origin);
	AStaticMeshActor* SpawnBox(const FVector& Center, const FVector& SizeCm, const FRotator& Rot, bool bMovable);
	void StartTest(int32 Index);
	void FinishTest();
	void ResetPawn(const FVector& FeetLocation, float Yaw);
	void Result(const TCHAR* Metric, double Expected, double Measured, const TCHAR* Unit = TEXT("cm"), double Tolerance = 0.0);
	void Note(const FString& Text);
	void Shot(const FString& Tag);
	void TickTest(float DeltaTime);
	/** Weapon / item / combo fidelity tests (ArenaMoveTestWeapons.cpp): w_* names. True when T is one of them. */
	bool TickWeaponTest(const FString& T, float DeltaTime);
	/** Translocator checks (ArenaMoveTestTransloc.cpp): w_transloc. */
	bool TickTranslocTest(float DeltaTime);
	/** View shake, terminal velocity and vehicle volume checks (ArenaMoveTestGaps.cpp): w_shake, w_fall. */
	bool TickGapsTest(const FString& T, float DeltaTime);
	/** Aim the player's view at a world point. */
	void AimAt(const FVector& Target);
	AArenaCharacter* SpawnDummy();

	TArray<FString> Tests;
	int32 TestIndex = -1;
	bool bRigBuilt = false;
	bool bDone = false;
	float WaitTime = 0.f;
	float TestTime = 0.f;
	float NextSampleTime = 0.f;
	int32 Phase = 0;
	float PhaseTime = 0.f;
	FVector Origin = FVector::ZeroVector;
	FVector StartLoc = FVector::ZeroVector;
	float StartFeetZ = 0.f;
	float MaxZ = 0.f;
	float MaxHSpeed = 0.f;
	float LandTime = -1.f;
	float TakeoffTime = -1.f;
	FVector LandLoc = FVector::ZeroVector;
	FVector Mark = FVector::ZeroVector;
	float MarkF = 0.f;
	float MarkF2 = 0.f;
	int32 Counter = 0;
	float WadeDepth = -1.f;
	FVector WadeFeet = FVector::ZeroVector;
	bool bWasFalling = false;
	float LiftSpeed = 0.f;
	TWeakObjectPtr<AArenaCharacter> Pawn;
	TWeakObjectPtr<AStaticMeshActor> Lift;
	FVector LiftHome = FVector::ZeroVector;
	TWeakObjectPtr<AActor> TrackedProjectile;
	FVector LastProjectileLoc = FVector::ZeroVector;
	TWeakObjectPtr<AArenaCharacter> Dummy;
	TArray<float> ShotTimes;
	int32 LastFireCount = 0;
	int32 SubIndex = 0;
	float SubTime = 0.f;
	int32 SubPhase = 0;
	float AmmoAtStart = 0.f;
	TWeakObjectPtr<AActor> TrackedActor;
	FVector TrackedLoc = FVector::ZeroVector;
	float TrackedSpawnTime = 0.f;
	TArray<TWeakObjectPtr<AActor>> TestActors;
	int32 Failures = 0;
	int32 Passes = 0;
};
