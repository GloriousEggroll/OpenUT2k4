#include "ArenaServerGameInstance.h"
#include "../OpenUT2k4.h"
#include "ArenaDedicatedServer.h"

#include "Engine/Engine.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"

void UArenaServerGameInstance::StartGameInstance()
{
	if (!ArenaDedicatedServer::IsServerMode())
	{
		Super::StartGameInstance();
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("ConvertOnly")) || FParse::Param(FCommandLine::Get(), TEXT("ConvertFontsOnly")))
	{
		// -ConvertOnly: the converter runs once the engine is up (ArenaConverter) and quits; no match
		UE_LOG(LogOpenUT2k4, Display, TEXT("Server: converting the UT2004 install, no match"));
		BroadcastOnStart();
		return;
	}
	FString Url, Error;
	if (!ArenaDedicatedServer::PrepareFirstMatch(Url, Error))
	{
		UE_LOG(LogOpenUT2k4, Error, TEXT("Server: cannot start: %s"), *Error);
		const bool bContent = Error.Contains(TEXT("content cache")) || Error.Contains(TEXT("UT2004 install"));
		FPlatformMisc::RequestExitWithStatus(false, bContent ? ArenaDedicatedServer::ExitNoContent : ArenaDedicatedServer::ExitConfigError);
		return;
	}
	// UGameEngine::Init ran with GIsClient off (no viewport, window or local player). Back on, LoadMap does not
	// start a UE5 net driver (it listens whenever !GIsClient); our UE2 server is the only network endpoint. With a
	// real RHI (no -nullrhi) a client without a viewport would quit at once, so it stays off and the URL names no
	// net driver definition instead.
	if (!FApp::CanEverRender())
	{
		GIsClient = true;
	}
	else
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Server: started without -nullrhi; the GPU is initialised for nothing"));
		Url += TEXT("?NetDriverDef=OpenUT2k4NoNetDriver");
	}
	FURL DefaultURL;
	DefaultURL.LoadURLConfig(TEXT("DefaultPlayer"), GGameIni);
	const FURL URL(&DefaultURL, *Url, TRAVEL_Partial);
	if (GetEngine()->Browse(*WorldContext, URL, Error) == EBrowseReturnVal::Failure)
	{
		UE_LOG(LogOpenUT2k4, Error, TEXT("Server: the first match could not be opened: %s"), *Error);
		FPlatformMisc::RequestExitWithStatus(false, ArenaDedicatedServer::ExitMatchFailed);
		return;
	}
	BroadcastOnStart();
}
