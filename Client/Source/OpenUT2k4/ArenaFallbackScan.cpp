#include "ArenaFallbackScan.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "UObject/UObjectIterator.h"

namespace ArenaFallbackScanPrivate
{
	struct FEntry
	{
		int32 Seen = 0;
		int32 MaxAtOnce = 0;
	};
	TMap<FString, FEntry>& Entries()
	{
		static TMap<FString, FEntry> Map;
		return Map;
	}
	bool IsEnabled()
	{
#if UE_BUILD_SHIPPING
		return false;
#else
		static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("LogProceduralShapes"));
		return bEnabled;
#endif
	}
	FAutoConsoleCommand ReportCommand(TEXT("arena.FallbackScanReport"), TEXT("Logs the procedural stand-in shapes seen so far (-LogProceduralShapes)."),
		FConsoleCommandDelegate::CreateStatic(&ArenaFallbackScan::Report));
}

void ArenaFallbackScan::Tick(UWorld* World)
{
	using namespace ArenaFallbackScanPrivate;
	if (!IsEnabled() || !World)
	{
		return;
	}
	static double NextScan = 0.0;
	const double Now = World->GetTimeSeconds();
	if (Now < NextScan)
	{
		return;
	}
	NextScan = Now + 0.5;
	const UStaticMesh* Shapes[] = { ArenaAssets::Cube(), ArenaAssets::Sphere(), ArenaAssets::Cylinder(), ArenaAssets::Cone() };
	const TCHAR* const ShapeNames[] = { TEXT("cube"), TEXT("sphere"), TEXT("cylinder"), TEXT("cone") };
	TMap<FString, int32> Now_;
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		const UStaticMeshComponent* C = *It;
		if (!C || C->GetWorld() != World || !C->IsRegistered() || !C->IsVisible() || C->bHiddenInGame)
		{
			continue;
		}
		const AActor* Owner = C->GetOwner();
		if (!Owner || Owner->IsHidden())
		{
			continue;
		}
		const UStaticMesh* Mesh = C->GetStaticMesh();
		for (int32 i = 0; i < UE_ARRAY_COUNT(Shapes); ++i)
		{
			if (Mesh && Mesh == Shapes[i])
			{
				// owner class + attach parent name (parts of one model share it) + shape
				const USceneComponent* Parent = C->GetAttachParent();
				const FString Key = FString::Printf(TEXT("%s / %s / %s"), *Owner->GetClass()->GetName(),
					Parent ? *Parent->GetName() : TEXT("-"), ShapeNames[i]);
				++Now_.FindOrAdd(Key);
				break;
			}
		}
	}
	for (const TPair<FString, int32>& P : Now_)
	{
		FEntry& E = Entries().FindOrAdd(P.Key);
		if (E.Seen == 0)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("FallbackScan: new procedural shape %s (x%d) at %.1f s"), *P.Key, P.Value, Now);
		}
		++E.Seen;
		E.MaxAtOnce = FMath::Max(E.MaxAtOnce, P.Value);
	}
}

void ArenaFallbackScan::Report()
{
	using namespace ArenaFallbackScanPrivate;
	UE_LOG(LogOpenUT2k4, Log, TEXT("FallbackScan: %d kind(s) of procedural shapes seen"), Entries().Num());
	for (const TPair<FString, FEntry>& P : Entries())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("FallbackScan:   %s - seen in %d scans, up to %d at once"), *P.Key, P.Value.Seen, P.Value.MaxAtOnce);
	}
}
