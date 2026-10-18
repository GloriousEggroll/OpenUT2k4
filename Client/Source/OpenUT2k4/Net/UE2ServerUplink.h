#pragma once

#include "CoreMinimal.h"

class FUE2Server;

/**
 * "Show in Server Browsers": the UT2004 master server uplink (IpDrv.MasterServerUplink) to the OldUnreal community
 * master servers ([IpDrv.MasterServerLink] MasterServerList of the 3374 patch). Only created when the player ticks
 * the Host Game option (UArenaUserSettings::bHostAdvertise); never in tests. See Docs/UE2Net.md "Master server uplink".
 */
class FUE2MasterUplink
{
public:
	explicit FUE2MasterUplink(FUE2Server& InServer);
	~FUE2MasterUplink();

	void Tick(double Now);
	/** A client's answer to the master's CD key challenge (PlayerController.ServerValidationResponse). */
	void ClientValidationResponse(int32 ConnId, const FString& Response);
	/** A connection closed: pending challenges for it are dropped. */
	void ConnectionClosed(int32 ConnId);

private:
	struct FImpl;
	/** One link per master server group (the masters of a group share their lists). */
	TArray<TUniquePtr<FImpl>> Links;
};
