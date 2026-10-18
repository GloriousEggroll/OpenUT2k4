#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ArenaPadShaftTest.generated.h"

class AArenaJumpPad;
class ACharacter;

/**
 * Jump pad flight test hook (non-shipping builds): -PadShaftTest=<UE2 pad name>[,<trials>]. Use with ?Bots=1. The
 * bot's AI is switched off; for each trial the pawn is put on the floor 3.5 m from the pad and runs straight at the
 * pad's centre from a different direction (trials spread over the full circle). The flight is logged:
 * "PadShaftTest: trial <n> from <deg>: touched at <xy>, apex <z>, landed <dist> from the target: OK|FAIL" and at the
 * end "PadShaftTest: <ok> of <n> flights reached the target".
 */
UCLASS()
class OPENUT2K4_API UArenaPadShaftTestSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	void StartTrial();

	TWeakObjectPtr<AArenaJumpPad> Pad;
	TWeakObjectPtr<ACharacter> Pawn;
	FString PadName;
	int32 NumTrials = 16;
	int32 Trial = -1;
	int32 NumOk = 0;
	/** 0 waiting, 1 running at the pad, 2 in flight, 3 done */
	int32 Phase = 0;
	float PhaseTime = 0.f;
	float WaitTime = 0.f;
	float MaxZ = 0.f;
	FVector TouchLoc = FVector::ZeroVector;
	FVector RunDir = FVector::ZeroVector;
};
