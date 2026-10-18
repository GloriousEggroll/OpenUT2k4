// Translocator links of the nav graph: UE2 ForcedPaths to JumpSpots, the routes UT2004 bots take with the translocator
// (JumpSpot.SpecialCost: 200 for a bot allowed to translocate; JumpSpot.SuggestMovePreparation -> TryTranslocator when
// the JumpSpot is bOnlyTranslocator, the jump can't be made, or bNoLowGrav in low gravity).
#include "ArenaNavGraph.h"
#include "OpenUT2k4.h"
#include "ArenaTranslocator.h"
#include "ArenaUE2World.h"
#include "Engine/World.h"

namespace ArenaTranslocatorNavLocal
{
	/** Pending links (node pairs and the JumpSpot data) between CollectTranslocatorSpots and AddTranslocatorLinks. */
	TArray<AArenaNavGraph::FTranslocLink>& Pending()
	{
		static TArray<AArenaNavGraph::FTranslocLink> P;
		return P;
	}

	/** Can the beacon fly there: AdjustToss from the start at TransBeacon.Speed, swept with the beacon's radius. */
	bool TossClear(UWorld* World, const FVector& Start, const FVector& Aim, const FVector& Target, float Radius)
	{
		const float G = ArenaTranslocator::GravityZAt(World, Start);
		const FVector V = ArenaTranslocator::AdjustToss(ArenaTranslocator::BeaconSpeedUU * 2.f, Start, Aim, G, false);
		const float Dist2D = FVector::Dist2D(Start, Aim);
		const float T = Dist2D / FMath::Max(1.f, FVector(V.X, V.Y, 0.f).Size());
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaTranslocNav), false);
		const FCollisionShape Shape = FCollisionShape::MakeSphere(ArenaTranslocator::BeaconRadiusUU * 2.f * 0.8f);
		FVector Prev = Start;
		constexpr int32 Steps = 16;
		for (int32 k = 1; k <= Steps; ++k)
		{
			const float t = T * k / Steps;
			const FVector P = Start + FVector(V.X, V.Y, 0.f) * t + FVector(0.f, 0.f, V.Z * t + 0.5f * G * t * t);
			// the beacon is caught as soon as it is over the JumpSpot's cylinder: stop checking there
			if (FVector::Dist2D(P, Target) < Radius)
			{
				return true;
			}
			if (World->SweepTestByObjectType(Prev, P, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic), Shape, Q))
			{
				return false;
			}
			Prev = P;
		}
		return true;
	}
}

void AArenaNavGraph::CollectTranslocatorSpots(TFunctionRef<int32(const FVector&)> AddNode)
{
	using namespace ArenaTranslocatorNavLocal;
	Pending().Reset();
	TranslocLinks.Reset();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	TMap<FVector, int32> NodeOf;
	auto NodeFor = [&](const FVector& L) -> int32
	{
		if (const int32* N = NodeOf.Find(L))
		{
			return *N;
		}
		const int32 N = AddNode(L);
		NodeOf.Add(L, N);
		return N;
	};
	for (const ArenaUE2World::FForcedPath& F : ArenaUE2World::GetForcedPaths(World))
	{
		if (!F.bToJumpDest || FVector::Dist2D(F.From, F.To) < 300.f)
		{
			continue;
		}
		FTranslocLink L;
		L.From = NodeFor(F.From);
		L.To = NodeFor(F.To);
		L.Target = F.To;
		L.Radius = F.TargetRadius;
		L.Height = F.TargetHeight;
		L.AimZ = F.TranslocZOffset;
		L.bOnlyTranslocator = F.bOnlyTranslocator;
		Pending().Add(L);
	}
}

void AArenaNavGraph::AddTranslocatorLinks()
{
	using namespace ArenaTranslocatorNavLocal;
	UWorld* World = GetWorld();
	int32 NumOnly = 0, NumBlocked = 0, NumJump = 0, NumNoFloor = 0;
	// the forced jumps' own nodes sit at the same spots (AddSpecialNode of the same ForcedPaths ends)
	TSet<TPair<FIntVector, FIntVector>> JumpPairs;
	auto Key = [](const FVector& V) { return FIntVector(FMath::RoundToInt32(V.X), FMath::RoundToInt32(V.Y), FMath::RoundToInt32(V.Z)); };
	for (const FArenaNavNode& N : Nodes)
	{
		for (const FArenaNavLink& K : N.Links)
		{
			if (K.Type == EArenaNavLinkType::ForcedJump && Nodes.IsValidIndex(K.To))
			{
				JumpPairs.Add(TPair<FIntVector, FIntVector>(Key(N.Location), Key(Nodes[K.To].Location)));
			}
		}
	}
	for (const FTranslocLink& L : Pending())
	{
		if (!Nodes.IsValidIndex(L.From) || !Nodes.IsValidIndex(L.To))
		{
			continue;
		}
		// a forced jump link already covers the pair: the bot jumps (SuggestMovePreparation tries the jump first unless the
		// JumpSpot is bOnlyTranslocator)
		const bool bJumpLink = JumpPairs.Contains(TPair<FIntVector, FIntVector>(Key(Nodes[L.From].Location), Key(Nodes[L.To].Location)));
		if (bJumpLink && !L.bOnlyTranslocator)
		{
			++NumJump;
			continue;
		}
		const FVector Start = Nodes[L.From].Location;
		// the spot must have floor under it (the translocation ends there; a JumpSpot over a gap is a jump target)
		{
			FHitResult Floor;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaTranslocFloor), false);
			FCollisionObjectQueryParams Obj(ECC_WorldStatic);
			Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
			const bool bHit = World->LineTraceSingleByObjectType(Floor, L.Target, L.Target - FVector(0.f, 0.f, 5000.f), Obj, Q);
			const bool bFloor = bHit && Floor.ImpactNormal.Z >= 0.7f && L.Target.Z - Floor.ImpactPoint.Z < L.Height + 200.f;
			if (!bFloor)
			{
				++NumNoFloor;
				UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: no floor under the translocator spot %s (from %s; first hit %s %.0f cm below, %s): link skipped"),
					*L.Target.ToCompactString(), *Start.ToCompactString(), bHit ? *GetNameSafe(Floor.GetActor()) : TEXT("nothing"),
					bHit ? L.Target.Z - Floor.ImpactPoint.Z : -1.f, bHit ? *Floor.ImpactNormal.ToCompactString() : TEXT("-"));
				continue;
			}
		}
		if (!TossClear(World, Start, L.Target + FVector(0.f, 0.f, L.AimZ), L.Target, L.Radius))
		{
			++NumBlocked;
			continue;
		}
		FArenaNavLink& Link = Nodes[L.From].Links.AddDefaulted_GetRef();
		Link.To = L.To;
		Link.Type = EArenaNavLinkType::Translocate;
		// UE2 path cost: the distance plus JumpSpot.SpecialCost 200 UU
		Link.Cost = FVector::Dist(Start, Nodes[L.To].Location) + 400.f;
		FTranslocLink& Added = TranslocLinks.Add_GetRef(L);
		Added.FromLoc = Start;
		Added.ToLoc = Nodes[L.To].Location;
		NumOnly += L.bOnlyTranslocator ? 1 : 0;
	}
	if (Pending().Num() > 0)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d translocator link(s) from %d ForcedPaths to JumpSpots (%d bOnlyTranslocator, %d left to forced jumps, %d with the throw blocked, %d without floor)"),
			TranslocLinks.Num(), Pending().Num(), NumOnly, NumJump, NumBlocked, NumNoFloor);
	}
	Pending().Reset();
}

const AArenaNavGraph::FTranslocLink* AArenaNavGraph::FindTranslocLink(const FVector& From, const FVector& To) const
{
	const FTranslocLink* Best = nullptr;
	float BestD = FMath::Square(120.f) * 2.f;
	for (const FTranslocLink& L : TranslocLinks)
	{
		const float D = FVector::DistSquared(L.FromLoc, From) + FVector::DistSquared(L.ToLoc, To);
		if (D < BestD)
		{
			BestD = D;
			Best = &L;
		}
	}
	return Best;
}
