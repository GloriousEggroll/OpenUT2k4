#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Controller.h"
#include "UE2ServerPlayer.generated.h"

/**
 * A player on our UE2 server (Net/UE2Server.h): the native controller of a UT2004 or OpenUT2k4 client that joined
 * the hosted match. It has a player state like any human player (scores, team, kills) and possesses an
 * AArenaCharacter like the local player and the bots; its input comes over the network (ServerMove, fire, use ...),
 * applied by FUE2ServerWorld. The pawn's movement component does not tick by itself: every client move is one
 * physics step (UE2 MoveAutonomous).
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AUE2RemoteController : public AController
{
	GENERATED_BODY()

public:
	AUE2RemoteController();

	/** FUE2ServerConnection id of the client. */
	int32 ConnId = 0;
	/** Joined with ?SpectatorOnly=1 (never gets a pawn). */
	bool bRemoteSpectator = false;

	virtual void GetPlayerViewPoint(FVector& OutLocation, FRotator& OutRotation) const override;

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
};
