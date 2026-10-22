// A server's player agreement (DruidsPlayerAgreement.PlayerAgreementManager) on a UE2 server. See UE2NetPlayerAgreement.h.

#include "UE2NetPlayerAgreement.h"
#include "UE2NetLog.h"
#include "../UScript/UScriptCore.h"

namespace UE2NetPlayerAgreementLocal
{
	/** DruidsPlayerAgreement's PlayerAgreementManager, whatever version package it comes in. */
	bool IsAgreementManager(const FUE2NetActor& Actor)
	{
		for (const FUSClass* C = Actor.Class; C; C = C->GetSuperClass())
		{
			if (C->GetName() == TEXT("PlayerAgreementManager"))
			{
				return true;
			}
		}
		return false;
	}
}

using namespace UE2NetPlayerAgreementLocal;

void FUE2NetPlayerAgreement::ActorClosed(const FUE2NetActor& Actor)
{
	if (Actor.Channel == Channel)
	{
		// the manager is gone (answered, timed out or failed): PlayerAgreementManager.Destroyed
		Reset();
	}
}

bool FUE2NetPlayerAgreement::FunctionCall(const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	if (!Function || !IsAgreementManager(Actor))
	{
		return false;
	}
	const FName Name = Function->Name;
	if (Name == TEXT("PlayerAgreement"))
	{
		// opens PlayerAgreementPage and tells the server it shows (showPage), once
		if (Channel != Actor.Channel || !bOpen)
		{
			Reset();
			Channel = Actor.Channel;
			bOpen = true;
			Connection.SendFunction(Channel, TEXT("showPage"), {});
			UE_LOG(LogUE2Net, Log, TEXT("PlayerAgreement: the server asks us to accept its player agreement (%s)"), *Actor.ClassPath);
		}
		return true;
	}
	if (Actor.Channel != Channel)
	{
		return Name == TEXT("addToAgreement") || Name == TEXT("doneAdding") || Name == TEXT("ForceCloseMenu");
	}
	if (Name == TEXT("addToAgreement"))
	{
		// PlayerAgreementPage.appendText: "" comes as "|", a paragraph break
		Lines.Add(Params.Num() > 0 ? Params[0].Value.S : FString());
		return true;
	}
	if (Name == TEXT("doneAdding"))
	{
		// PlayerAgreementPage.doneAddingText(YesButtonEnableTime): its Timer enables "I agree!" once that has counted down
		const int32 Seconds = Params.Num() > 0 ? Params[0].Value.I : 0;
		YesAt = FPlatformTime::Seconds() + FMath::Max(0, Seconds);
		UE_LOG(LogUE2Net, Log, TEXT("PlayerAgreement: %d lines, \"I agree!\" in %d s"), Lines.Num(), Seconds);
		return true;
	}
	if (Name == TEXT("ForceCloseMenu"))
	{
		bOpen = false;
		return true;
	}
	return false;
}

int32 FUE2NetPlayerAgreement::GetSecondsUntilYes() const
{
	if (YesAt < 0.0)
	{
		return -1;
	}
	return FMath::Max(0, FMath::CeilToInt(YesAt - FPlatformTime::Seconds()));
}

bool FUE2NetPlayerAgreement::Answer(bool bYes)
{
	if (!IsOpen() || (bYes && GetSecondsUntilYes() != 0))
	{
		return false;
	}
	// PlayerAgreementPage.Yes / No: the manager's yes() / no() on the server
	if (!Connection.SendFunction(Channel, bYes ? FName(TEXT("yes")) : FName(TEXT("no")), {}))
	{
		return false;
	}
	UE_LOG(LogUE2Net, Log, TEXT("PlayerAgreement: answered %s"), bYes ? TEXT("yes") : TEXT("no"));
	bOpen = false;
	return true;
}

void FUE2NetPlayerAgreement::Reset()
{
	Channel = 0;
	bOpen = false;
	Lines.Reset();
	YesAt = -1.0;
}
