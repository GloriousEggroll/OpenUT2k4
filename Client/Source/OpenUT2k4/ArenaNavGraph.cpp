#include "ArenaNavGraph.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "OpenUT2k4.h"
#include "ArenaTypes.h"
#include "ArenaLevelBuilder.h"
#include "ArenaGameMode.h"
#include "ArenaGameRules.h"
#include "ArenaPickup.h"
#include "ArenaJumpPad.h"
#include "ArenaMover.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "ArenaTriggers.h"
#include "ArenaWorldZones.h"
#include "ArenaUE2World.h"
#include "ArenaFlag.h"
#include "ArenaCarryable.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Algo/Reverse.h"
#include "Async/ParallelFor.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "ArenaContent.h"

static TAutoConsoleVariable<int32> CVarArenaDebugNav(
	TEXT("Arena.DebugNav"),
	0,
	TEXT("Draw the bot waypoint graph. 0 = off, 1 = nodes and links near the viewer, 2 = whole graph."),
	ECVF_Default);

namespace ArenaNavBuild
{
	// Bump when the build changes what it produces (invalidates navcache/ files).
	constexpr int32 NavCacheVersion = 3;
	constexpr float GridSpacing = 250.f;
	constexpr float NodeHeight = 90.f;             // floor -> node (capsule centre + 2 cm)
	constexpr float CapsuleRadius = ArenaTuning::CapsuleRadius;
	constexpr float CapsuleHalfHeight = ArenaTuning::CapsuleHalfHeight;
	constexpr float WalkableZ = 0.7f;
	constexpr float LinkRadius = 420.f;
	/** Pairs of the map's own UE2 navigation points are linked up to this far apart. */
	constexpr float UE2PointLinkRadius = 2000.f;
	// Long drops: running off a ledge carries the character well past LinkRadius before it lands
	// (imported maps have spawn balconies and ledges ~10 m above the floor below). Pairs farther apart than
	// LinkRadius are only tested as one-way drops, from the higher node, within the reach of a fall.
	constexpr float LongDropRadius = 800.f;
	constexpr float LongDropMinHeight = 300.f;
	constexpr float LongDropReachScale = 0.8f;    // fraction of the ballistic reach (run speed x fall time)
	constexpr float StepLen = 40.f;
	constexpr float MaxStep = ArenaTuning::MaxStepHeight;
	constexpr float MaxJumpUp = 200.f;
	constexpr float MaxJumpReach = ArenaTuning::MaxJumpHeight;
	constexpr float JumpClearance = 30.f;
	constexpr float MaxDrop = 1500.f;             // no fall damage: long drops are fine (imported maps need them)
	constexpr float MaxGapJump = 250.f;
	constexpr float RunSpeed = ArenaTuning::GroundSpeed;
	constexpr float Gravity = 980.f * ArenaTuning::GravityScale;
	constexpr float PadRadius = 90.f;
	/** Forced jumps: the most vertical launch speed allowed (about a jump plus a double jump). */
	/** (UE2 JumpSpot bDodgeUp / bForceAllowDoubleJumping spots need a dodge jump: about 1100 cm/s up, AS-Convoy) */
	constexpr float ForcedJumpMaxLift = 1100.f;
	constexpr int32 MaxSurfacesPerColumn = 16;
	constexpr float ComplexLayerGap = 20.f;         // step below a complex-collision surface before re-tracing

	// Reduced capsule used for link sweeps: its bottom sits MaxStep + 2 above the floor so that
	// steps the character can climb do not block, while the top matches the real capsule.
	constexpr float SweepRadius = CapsuleRadius - 4.f;
	constexpr float SweepHalfHeight = CapsuleHalfHeight - (MaxStep * 0.5f);
	constexpr float SweepLift = MaxStep + 2.f + SweepHalfHeight; // floor -> sweep capsule centre

	enum class ESim : uint8 { Fail, Walk, Drop, Gap };

	/** Collision helpers used during the build (WorldStatic channel, pawns / items ignored). */
	struct FContext
	{
		UWorld* World = nullptr;
		FCollisionQueryParams Params;
		FCollisionShape Capsule;
		FCollisionShape SweepCapsule;

		/** UE2 physics volumes (low gravity zones change how far drops and jumps reach). */
		TArray<const AArenaPhysicsVolume*> Volumes;
		float WorldGravityZ = -980.f;
		/** UE gravity of UE2's stock -950 (the jump and fall constants above are for it). */
		float StockGravityZ = -980.f;
		/** The level's gravity outside volumes relative to the stock one (BR-Skyline: 600 / 950). */
		float BaseRatio = 1.f;
		/** Lowest gravity of the map relative to the stock one (1 = none lower). */
		float MinGravityRatio = 1.f;

		explicit FContext(UWorld* InWorld)
			: World(InWorld)
			, Params(SCENE_QUERY_STAT(ArenaNavBuild), false)
		{
			Capsule = FCollisionShape::MakeCapsule(CapsuleRadius, CapsuleHalfHeight);
			SweepCapsule = FCollisionShape::MakeCapsule(SweepRadius, SweepHalfHeight);
			WorldGravityZ = InWorld->GetGravityZ();
			StockGravityZ = UPhysicsSettings::Get()->DefaultGravityZ;
			BaseRatio = (WorldGravityZ < 0.f && StockGravityZ < 0.f) ? FMath::Clamp(WorldGravityZ / StockGravityZ, 0.25f, 4.f) : 1.f;
			MinGravityRatio = BaseRatio;
			for (TActorIterator<AArenaPhysicsVolume> It(InWorld); It; ++It)
			{
				if (It->Shape.IsValid())
				{
					Volumes.Add(*It);
					MinGravityRatio = FMath::Min(MinGravityRatio, RatioOf(*It));
				}
			}
		}

		float RatioOf(const AArenaPhysicsVolume* V) const
		{
			// only downward gravity is modelled; volumes that blow sideways or up count as normal gravity
			const float R = StockGravityZ < 0.f ? V->GetGravityZ() / StockGravityZ : 1.f;
			return R > 0.f ? FMath::Clamp(R, 0.25f, 4.f) : BaseRatio;
		}

		/** Gravity at a point relative to the stock one (UE2 rule: the highest priority volume containing it). */
		float GravityRatioAt(const FVector& P) const
		{
			const AArenaPhysicsVolume* Best = nullptr;
			for (const AArenaPhysicsVolume* V : Volumes)
			{
				if ((!Best || V->Priority > Best->Priority) && V->EncompassesUE2(P))
				{
					Best = V;
				}
			}
			return Best ? RatioOf(Best) : BaseRatio;
		}

		float GravityAt(const FVector& P) const { return Gravity * GravityRatioAt(P); }

		bool Trace(const FVector& Start, const FVector& End, FHitResult& Hit, const FCollisionQueryParams& InParams) const
		{
			return World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, InParams);
		}

		bool TraceDown(const FVector2D& XY, float FromZ, float ToZ, FHitResult& Hit) const
		{
			return Trace(FVector(XY.X, XY.Y, FromZ), FVector(XY.X, XY.Y, ToZ), Hit, Params);
		}

		bool CapsuleFits(const FVector& Center) const
		{
			return !World->OverlapBlockingTestByChannel(Center, FQuat::Identity, ECC_WorldStatic, Capsule, Params);
		}

		bool SweepClear(const FVector& A, const FVector& B) const
		{
			if (FVector::DistSquared(A, B) < 1.f)
			{
				return !World->OverlapBlockingTestByChannel(A, FQuat::Identity, ECC_WorldStatic, SweepCapsule, Params);
			}
			return !World->SweepTestByChannel(A, B, FQuat::Identity, ECC_WorldStatic, SweepCapsule, Params);
		}

		/** Floor under a point (within Up above / Down below). Returns false if none / not walkable. */
		bool FindFloor(const FVector& Location, float Up, float Down, float& OutFloorZ) const
		{
			FHitResult Hit;
			if (Trace(Location + FVector(0, 0, Up), Location - FVector(0, 0, Down), Hit, Params) && Hit.ImpactNormal.Z >= WalkableZ)
			{
				OutFloorZ = Hit.ImpactPoint.Z;
				return true;
			}
			return false;
		}

		/**
		 * Simulate a character running from node A to node B (straight line, full speed).
		 * Walk = stays on the floor with steps <= MaxStep. Drop = leaves a ledge and lands lower.
		 * Gap = had to cross a hole and landed at a similar height (needs a jump). OutMaxGap = longest airborne span.
		 */
		ESim Simulate(const FVector& A, const FVector& B, float& OutMaxGap) const
		{
			OutMaxGap = 0.f;
			const float FloorA = A.Z - NodeHeight;
			const float FloorB = B.Z - NodeHeight;
			const FVector2D A2(A.X, A.Y);
			const FVector2D B2(B.X, B.Y);
			const float Dist = FVector2D::Distance(A2, B2);
			const int32 N = FMath::Max(1, FMath::CeilToInt(Dist / StepLen));
			const float LowLimit = FMath::Min(FloorA, FloorB) - 200.f;

			float CurFloor = FloorA;
			bool bFalling = false;
			bool bAnyGap = false;
			bool bAnyDrop = false;
			float FallStartFloor = FloorA;
			float FallStartDist = 0.f;
			float BodyZ = FloorA;
			const float G = GravityAt(A);
			FVector Prev(A.X, A.Y, FloorA + SweepLift);

			for (int32 k = 1; k <= N; ++k)
			{
				const float Alpha = float(k) / float(N);
				const FVector2D P = FMath::Lerp(A2, B2, Alpha);
				const float D = Dist * Alpha;

				if (bFalling)
				{
					const float T = (D - FallStartDist) / RunSpeed;
					BodyZ = FallStartFloor - 0.5f * G * T * T;
				}
				else
				{
					BodyZ = CurFloor;
				}

				FVector Next(P.X, P.Y, BodyZ + SweepLift);
				if (!SweepClear(Prev, Next))
				{
					return ESim::Fail;
				}

				FHitResult Hit;
				const bool bHit = TraceDown(P, BodyZ + MaxStep + 10.f, LowLimit, Hit);
				const float F = bHit ? float(Hit.ImpactPoint.Z) : -1.0e9f;
				const bool bWalkable = bHit && Hit.ImpactNormal.Z >= WalkableZ;

				if (!bFalling)
				{
					if (bHit && F >= CurFloor - MaxStep)
					{
						if (F > CurFloor + MaxStep || !bWalkable)
						{
							return ESim::Fail;
						}
						CurFloor = F;
						Next.Z = CurFloor + SweepLift;
					}
					else
					{
						// Floor fell away: we run off the edge (edge assumed half a step back)
						bFalling = true;
						FallStartFloor = CurFloor;
						FallStartDist = D - StepLen * 0.5f;
					}
				}
				else if (bHit && F >= BodyZ - 5.f)
				{
					// Landed
					if (F > BodyZ + MaxStep || !bWalkable)
					{
						return ESim::Fail;
					}
					const float Gap = D - FallStartDist;
					if (FallStartFloor - F > MaxStep)
					{
						bAnyDrop = true;
					}
					else
					{
						bAnyGap = true;
						OutMaxGap = FMath::Max(OutMaxGap, Gap);
					}
					bFalling = false;
					CurFloor = F;
					Next.Z = CurFloor + SweepLift;
				}
				Prev = Next;
			}

			if (bFalling)
			{
				// Still airborne above B: fall straight down onto B's floor
				if (FloorB > BodyZ + 5.f)
				{
					return ESim::Fail;
				}
				if (!SweepClear(Prev, FVector(B.X, B.Y, FloorB + SweepLift)))
				{
					return ESim::Fail;
				}
				if (FallStartFloor - FloorB > MaxStep)
				{
					bAnyDrop = true;
				}
				else
				{
					bAnyGap = true;
					OutMaxGap = FMath::Max(OutMaxGap, Dist - FallStartDist);
				}
				CurFloor = FloorB;
			}

			if (FMath::Abs(CurFloor - FloorB) > 25.f)
			{
				return ESim::Fail; // ended up on a different surface than B
			}
			if (FloorA - FloorB > MaxDrop * Gravity / G)
			{
				return ESim::Fail;
			}
			if (bAnyGap)
			{
				return ESim::Gap;
			}
			return bAnyDrop ? ESim::Drop : ESim::Walk;
		}

		/** Jump from A to B: up at A, across at apex height, down onto B, with gaps < MaxGapJump. */
		bool CanJump(const FVector& A, const FVector& B) const
		{
			const float FloorA = A.Z - NodeHeight;
			const float FloorB = B.Z - NodeHeight;
			const float Dz = FloorB - FloorA;
			// jump height and air time grow as gravity drops
			const float JumpScale = 1.f / GravityRatioAt(A);
			if (Dz > MaxJumpUp * JumpScale || Dz < -MaxDrop * JumpScale)
			{
				return false;
			}
			const float H = FMath::Max(FloorA, FloorB) + SweepLift + JumpClearance;
			if (H - (FloorA + SweepLift) > MaxJumpReach * JumpScale)
			{
				return false;
			}
			const FVector AUp(A.X, A.Y, H);
			const FVector BUp(B.X, B.Y, H);
			if (!SweepClear(FVector(A.X, A.Y, FloorA + SweepLift), AUp) ||
				!SweepClear(AUp, BUp) ||
				!SweepClear(BUp, FVector(B.X, B.Y, FloorB + SweepLift)))
			{
				return false;
			}

			// Holes along the way must be short enough to clear
			const FVector2D A2(A.X, A.Y);
			const FVector2D B2(B.X, B.Y);
			const float Dist = FVector2D::Distance(A2, B2);
			const int32 N = FMath::Max(1, FMath::CeilToInt(Dist / StepLen));
			const float MinFloor = FMath::Min(FloorA, FloorB) - MaxStep;
			float Gap = 0.f;
			for (int32 k = 1; k < N; ++k)
			{
				const FVector2D P = FMath::Lerp(A2, B2, float(k) / float(N));
				FHitResult Hit;
				const bool bFloor = TraceDown(P, H, MinFloor, Hit);
				if (bFloor)
				{
					Gap = 0.f;
				}
				else
				{
					Gap += Dist / N;
					if (Gap >= MaxGapJump * JumpScale)
					{
						return false;
					}
				}
			}
			return true;
		}
	};

	struct FForcedJump { int32 From; int32 To; float Speed; };
	/** Horizontal launch speed for a forced jump (0 if none up to dodge speed with a double jump's lift reaches). */
	float ForcedJumpSpeed(const FVector& From, const FVector& To, float G)
	{
		const float D = FVector::Dist2D(From, To);
		const float Dz = To.Z - From.Z;
		for (float S = RunSpeed; S <= ArenaTuning::DodgeSpeed * 1.05f; S += 50.f)
		{
			const float T = D / S;
			if ((Dz + 0.5f * G * T * T) / T <= ForcedJumpMaxLift)
			{
				return S;
			}
		}
		return 0.f;
	}

	template <typename AddNodeFn>
	void CollectForcedJumps(const FContext& Ctx, AddNodeFn AddNode, TArray<FForcedJump>& Out)
	{
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
		for (const ArenaUE2World::FForcedPath& F : ArenaUE2World::GetForcedPaths(Ctx.World))
		{
			if (!F.bToJumpDest || FVector::Dist2D(F.From, F.To) < LinkRadius)
			{
				// close ones are covered by the normal links. Translocator-only JumpSpots (bOnlyTranslocator) stay jump links
				// when a jump reaches them: ball and flag carriers can't translocate (BR-Skyline's islands)
				continue;
			}
			const float Speed = ForcedJumpSpeed(F.From, F.To, Ctx.GravityAt(F.From));
			if (Speed > 0.f)
			{
				Out.Add({ NodeFor(F.From), NodeFor(F.To), Speed });
			}
		}
	}

	float SegmentDist2D(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const float Len2 = AB.SizeSquared();
		const float T = Len2 > KINDA_SMALL_NUMBER ? FMath::Clamp(FVector2D::DotProduct(P - A, AB) / Len2, 0.f, 1.f) : 0.f;
		return FVector2D::Distance(P, A + AB * T);
	}

	float LinkCost(EArenaNavLinkType Type, const FVector& A, const FVector& B)
	{
		const float D = FVector::Dist(A, B);
		switch (Type)
		{
		case EArenaNavLinkType::Jump:    return D * 1.3f + 200.f;
		case EArenaNavLinkType::Drop:    return D * 1.1f + 20.f;
		case EArenaNavLinkType::JumpPad: return D * 0.8f + 100.f;
		case EArenaNavLinkType::Lift:    return D * 0.8f + 300.f;
		case EArenaNavLinkType::Teleport: return 150.f;
		case EArenaNavLinkType::ForcedJump: return D * 1.2f + 300.f;
		default:                         return D;
		}
	}

	/** A* heuristic scale; must stay <= the cheapest cost per cm (JumpPad 0.8). */
	constexpr float HeuristicScale = 0.8f;
}

AArenaNavGraph::AArenaNavGraph()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickInterval = 0.5f;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetCanBeDamaged(false);
	bReplicates = false;
}

FIntVector AArenaNavGraph::CellOf(const FVector& Location) const
{
	return FIntVector(
		FMath::FloorToInt32(Location.X / CellSize),
		FMath::FloorToInt32(Location.Y / CellSize),
		FMath::FloorToInt32(Location.Z / CellSize));
}

bool AArenaNavGraph::IsClearLine(const FVector& From, const FVector& To) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaNavLOS), false);
	return !World->LineTraceTestByObjectType(From, To, FCollisionObjectQueryParams(ECC_WorldStatic), Params);
}

void AArenaNavGraph::Build(const FBox& InBounds, AArenaLevelBuilder* Builder, const TArray<FVector>& ExtraPoints)
{
	using namespace ArenaNavBuild;

	const double StartTime = FPlatformTime::Seconds();
	Nodes.Reset();
	Cells.Reset();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FBox Bounds = InBounds;
	if (!Bounds.IsValid && Builder)
	{
		Bounds = Builder->GetArenaBounds();
	}
	if (!Bounds.IsValid)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("NavGraph: invalid bounds, graph not built"));
		return;
	}

	FContext Ctx(World);
	TArray<FBox> ScriptedDoorBoxes;
	TArray<TPair<const TCHAR*, double>> PhaseTimes;
	DeferredPoints.Reset();
	// Ignore everything that is not level geometry
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		Ctx.Params.AddIgnoredActor(*It);
	}
	for (TActorIterator<AArenaPickup> It(World); It; ++It)
	{
		Ctx.Params.AddIgnoredActor(*It);
	}
	for (TActorIterator<AArenaFlag> It(World); It; ++It)
	{
		Ctx.Params.AddIgnoredActor(*It);
	}
	// Mode actors opt out of blocking the graph with this actor tag (carryables always do).
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(ArenaNavIgnoreTag()) || It->IsA<AArenaCarryable>())
		{
			Ctx.Params.AddIgnoredActor(*It);
		}
	}
	Ctx.Params.AddIgnoredActor(this);
	// UE2 doors (R_DOOR reach specs): movers that a pawn opens by bumping them, or that a proximity Trigger next to
	// them opens (CTF-Face3's base doors: TriggerControl movers with a Trigger at the doorway), are passages, not walls.
	// Bots open them by walking into them as players do. The build sees each door in its open pose (the panels slid
	// aside still block what they block when open: AS-Glacier's base doors leave a gap narrower than the frame)
	TArray<TPair<UPrimitiveComponent*, FVector>> OpenedDoorMeshes;
	ON_SCOPE_EXIT
	{
		for (const TPair<UPrimitiveComponent*, FVector>& D : OpenedDoorMeshes)
		{
			D.Key->AddWorldOffset(-D.Value, false, nullptr, ETeleportType::TeleportPhysics);
		}
	};
	{
		TArray<const AArenaTrigger*> ProximityTriggers;
		for (TActorIterator<AArenaTrigger> It(World); It; ++It)
		{
			// proximity triggers (UT2004 TriggerType 0-3, 5 HumanPlayer, 6 LivePlayer); not 4 (Shoot)
			if (It->TriggerType != 4 && It->bInitiallyActive && It->GetUE2Event() != NAME_None)
			{
				ProximityTriggers.Add(*It);
			}
		}
		int32 NumDoors = 0, NumScripted = 0;
		const TArray<FString> NavDoorMovers = ArenaUE2World::GetNavDoorMovers(World);
		ScriptedDoors.Reset();
		const AArenaGameMode* DoorGM = World->GetAuthGameMode<AArenaGameMode>();
		const bool bScriptedDoorsPass = DoorGM && DoorGM->GetRules() && DoorGM->GetRules()->AreScriptedDoorsPassable();
		for (TActorIterator<AArenaMover> It(World); It; ++It)
		{
			const AArenaMover* M = *It;
			if (M->NumKeys < 2 || M->bRotating)
			{
				continue;
			}
			bool bDoor = M->InitialState == EArenaMoverState::BumpOpenTimed && M->BumpType <= 2;
			// a mover a UE2 Door navigation point names (Door.MyDoor): a door on the map's own paths
			const bool bNavDoor = NavDoorMovers.Contains(M->UE2Name);
			// any triggered state (TriggerControl / OpenTimed / Toggle / Pound, UT2004's TriggerAdvance ...), not lifts or loopers
			if (!bDoor && M->InitialState != EArenaMoverState::StandOpenTimed && M->InitialState != EArenaMoverState::ConstantLoop
				&& M->InitialState != EArenaMoverState::LeadInOutLooper && M->InitialState != EArenaMoverState::BumpButton)
			{
				const FVector Closed = M->GetKeyLocation(0);
				for (const AArenaTrigger* T : ProximityTriggers)
				{
					if (T->GetUE2Event() == M->Tag && FVector::Dist(T->GetActorLocation(), Closed) < 800.f)
					{
						bDoor = true;
						break;
					}
				}
			}
			// a door the map's script or objectives open (Assault: AS-Convoy's 'Code' door after the gate terminal): a passage
			// where the mode says scripted movers open as the match goes on; the bots then wait at it until it does
			// Door-like only: a thin upright slab (local mesh bounds: at least 1.5 m tall, at most 1.4 m thick) that moves less
			// than 9 m; trap doors, ramps, lifts and trucks stay floors
			bool bSlab = false;
			if (M->Mesh && M->Mesh->GetStaticMesh())
			{
				const FVector Ext = M->Mesh->GetStaticMesh()->GetBounds().BoxExtent * M->Mesh->GetComponentScale().GetAbs();
				float Travel = 0.f;
				for (int32 k = 1; k < M->NumKeys; ++k)
				{
					Travel = FMath::Max(Travel, float(FVector::Dist(M->GetKeyLocation(k), M->GetKeyLocation(0))));
				}
				bSlab = Ext.Z * 2.f >= 150.f && FMath::Min(Ext.X, Ext.Y) * 2.f <= 140.f && Travel <= 900.f;
			}
			if (!bDoor && bScriptedDoorsPass && (bSlab || bNavDoor) && M->Tag != NAME_None && M->InitialState != EArenaMoverState::StandOpenTimed
				&& M->InitialState != EArenaMoverState::ConstantLoop && M->InitialState != EArenaMoverState::LeadInOutLooper)
			{
				bDoor = true;
				++NumScripted;
				ScriptedDoors.Add(const_cast<AArenaMover*>(M));
				ScriptedDoorBoxes.Add(M->Mesh->Bounds.GetBox().ExpandBy(40.f));
			}
			if (bDoor)
			{
				const FVector OpenDelta = M->KeyPos.IsValidIndex(M->NumKeys - 1) ? M->BasePos + M->KeyPos[M->NumKeys - 1] - M->GetNetLocation() : FVector::ZeroVector;
				// sliding doors only: a door that rises or sinks is out of the way when ignored (and movers that move
				// up or down are as often collapses and blockers: AS-Convoy's CollapsePipes, CranePaths)
				if (M->Mesh && !OpenDelta.IsNearlyZero(1.f) && OpenDelta.Size() < 5000.f && FMath::Abs(OpenDelta.Z) < 0.5f * OpenDelta.Size2D())
				{
					UPrimitiveComponent* DoorMesh = const_cast<UPrimitiveComponent*>(static_cast<const UPrimitiveComponent*>(M->Mesh.Get()));
					DoorMesh->AddWorldOffset(OpenDelta, false, nullptr, ETeleportType::TeleportPhysics);
					OpenedDoorMeshes.Emplace(DoorMesh, OpenDelta);
					UE_LOG(LogOpenUT2k4, Verbose, TEXT("NavGraph: door %s (tag %s) built open, moved by %s"), *M->UE2Name, *M->Tag.ToString(), *OpenDelta.ToCompactString());
				}
				else
				{
					Ctx.Params.AddIgnoredActor(M);
				}
				++NumDoors;
			}
		}
		if (NumDoors > 0)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d door mover(s) are passable (%d opened by the map's script / objectives)"), NumDoors, NumScripted);
		}
	}

	PhaseTimes.Add(TPair<const TCHAR*, double>(TEXT("setup"), FPlatformTime::Seconds()));
	// ---------------- 0. cached graph ----------------
	CacheFile.Reset();
	TArray<FString> DoorNames;
	for (const TWeakObjectPtr<AArenaMover>& D : ScriptedDoors)
	{
		DoorNames.Add(D.IsValid() ? D->UE2Name : FString());
	}
	if (Builder && !FParse::Param(FCommandLine::Get(), TEXT("NoNavCache")) && !FParse::Param(FCommandLine::Get(), TEXT("ArenaDumpNav"))
		&& !FCString::Strifind(FCommandLine::Get(), TEXT("-NavCompare=")))
	{
		FString MapName = Builder->GetBuiltLayoutId();
		MapName.RemoveFromStart(TEXT("Imported:"));
		const FString MapJson = ArenaContent::GetCacheDir() / TEXT("maps") / MapName / TEXT("map.json");
		const FFileStatData Stat = IFileManager::Get().GetStatData(*MapJson);
		if (Stat.bIsValid && !MapName.IsEmpty())
		{
			// everything the build depends on: map data, mode (objective hints, scripted doors), bounds, gravity, code version
			const AArenaGameMode* KeyGM = World->GetAuthGameMode<AArenaGameMode>();
			FString Key = FString::Printf(TEXT("v%d|%lld|%s|%s|%s|%.1f|%d"), NavCacheVersion, Stat.FileSize, *Stat.ModificationTime.ToString(),
				KeyGM ? *KeyGM->GetModeId() : TEXT("-"), *Bounds.ToString(), Ctx.WorldGravityZ, ScriptedDoors.Num());
			for (const FVector& P : ExtraPoints)
			{
				Key += FString::Printf(TEXT("|%.0f,%.0f,%.0f"), P.X, P.Y, P.Z);
			}
			Key += FString::Join(DoorNames, TEXT(","));
			CacheFile = ArenaContent::GetCacheDir() / TEXT("navcache") / FString::Printf(TEXT("%s_%s_%08x.bin"), *MapName,
				KeyGM ? *KeyGM->GetModeId() : TEXT("-"), GetTypeHash(Key));
			if (LoadCache(CacheFile, DoorNames))
			{
				BuildParams = Ctx.Params;
				UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d nodes loaded from %s in %.1f ms"), Nodes.Num(), *FPaths::GetCleanFilename(CacheFile),
					(FPlatformTime::Seconds() - StartTime) * 1000.0);
				return;
			}
		}
	}

	// ---------------- 1. grid sampling ----------------
	const float TopZ = Bounds.Max.Z + 50.f;
	const float BottomZ = Bounds.Min.Z - 200.f;
	const int32 NumX = FMath::Max(1, FMath::FloorToInt32((Bounds.Max.X - Bounds.Min.X) / GridSpacing) + 1);
	const int32 NumY = FMath::Max(1, FMath::FloorToInt32((Bounds.Max.Y - Bounds.Min.Y) / GridSpacing) + 1);
	const float OffX = ((Bounds.Max.X - Bounds.Min.X) - (NumX - 1) * GridSpacing) * 0.5f;
	const float OffY = ((Bounds.Max.Y - Bounds.Min.Y) - (NumY - 1) * GridSpacing) * 0.5f;

	auto CapsuleFitsOnFloor = [&Ctx](const FVector2D& XY, float FloorZ, float NormalZ) -> bool
	{
		// Raise the test on slopes so the bottom hemisphere does not touch the floor
		const float Nz = FMath::Max(NormalZ, WalkableZ);
		const float TestZ = FloorZ + (CapsuleHalfHeight - CapsuleRadius) + (CapsuleRadius + 2.f) / Nz;
		return Ctx.CapsuleFits(FVector(XY.X, XY.Y, TestZ));
	};

	static const FVector2D NudgeOffsets[] = { FVector2D(70, 0), FVector2D(-70, 0), FVector2D(0, 70), FVector2D(0, -70) };

	for (int32 ix = 0; ix < NumX; ++ix)
	{
		for (int32 iy = 0; iy < NumY; ++iy)
		{
			const FVector2D XY(Bounds.Min.X + OffX + ix * GridSpacing, Bounds.Min.Y + OffY + iy * GridSpacing);
			FCollisionQueryParams ColumnParams = Ctx.Params;
			float StartZ = TopZ;

			for (int32 s = 0; s < MaxSurfacesPerColumn && StartZ > BottomZ; ++s)
			{
				FHitResult Hit;
				if (!Ctx.Trace(FVector(XY.X, XY.Y, StartZ), FVector(XY.X, XY.Y, BottomZ), Hit, ColumnParams))
				{
					break;
				}
				const float FloorZ = Hit.ImpactPoint.Z;
				if (Hit.ImpactNormal.Z >= WalkableZ)
				{
					if (CapsuleFitsOnFloor(XY, FloorZ, Hit.ImpactNormal.Z))
					{
						FArenaNavNode& Node = Nodes.AddDefaulted_GetRef();
						Node.Location = FVector(XY.X, XY.Y, FloorZ + NodeHeight);
					}
					else
					{
						// Near walls: try a few nudged positions on the same surface
						for (const FVector2D& Off : NudgeOffsets)
						{
							const FVector2D XY2 = XY + Off;
							FHitResult Hit2;
							if (Ctx.TraceDown(XY2, FloorZ + MaxStep, FloorZ - MaxStep, Hit2) &&
								Hit2.ImpactNormal.Z >= WalkableZ &&
								CapsuleFitsOnFloor(XY2, Hit2.ImpactPoint.Z, Hit2.ImpactNormal.Z))
							{
								FArenaNavNode& Node = Nodes.AddDefaulted_GetRef();
								Node.Location = FVector(XY2.X, XY2.Y, Hit2.ImpactPoint.Z + NodeHeight);
								break;
							}
						}
					}
				}
				// Continue below this surface. Convex parts (procedural boxes) are ignored from here on;
				// complex-collision meshes (imported BSP / static meshes) have more surfaces further down,
				// so the next trace starts a little below this one instead.
				UPrimitiveComponent* Comp = Hit.GetComponent();
				const UBodySetup* Body = Comp ? Comp->GetBodySetup() : nullptr;
				if (Comp && !(Body && Body->CollisionTraceFlag == CTF_UseComplexAsSimple))
				{
					ColumnParams.AddIgnoredComponent(Comp);
					StartZ = FloorZ - 1.f;
				}
				else
				{
					StartZ = FloorZ - ComplexLayerGap;
				}
			}
		}
	}
	const int32 NumGridNodes = Nodes.Num();

	PhaseTimes.Add(TPair<const TCHAR*, double>(TEXT("grid"), FPlatformTime::Seconds()));
	// ---------------- 2. special nodes ----------------
	TArray<int32> AnchorNodes;
	auto AddSpecialNode = [this, &Ctx](const FVector& Location) -> int32
	{
		float FloorZ = 0.f;
		FVector NodeLoc = Location;
		if (Ctx.FindFloor(Location, 60.f, 400.f, FloorZ))
		{
			NodeLoc.Z = FloorZ + NodeHeight;
		}
		FArenaNavNode& Node = Nodes.AddDefaulted_GetRef();
		Node.Location = NodeLoc;
		return Nodes.Num() - 1;
	};

	TArray<FForcedJump> ForcedJumps;
	TSet<int32> UE2PointNodes;
	TSet<int32> WaterNodes;
	struct FPadInfo { FVector PadLoc; int32 PadNode; int32 TargetNode; };
	TArray<FPadInfo> Pads;

	if (Builder)
	{
		for (AArenaPickup* Pickup : Builder->GetPickups())
		{
			if (Pickup)
			{
				AddSpecialNode(Pickup->GetActorLocation());
			}
		}
		for (APlayerStart* Start : Builder->GetPlayerStarts())
		{
			if (Start)
			{
				AnchorNodes.Add(AddSpecialNode(Start->GetActorLocation()));
			}
		}
		for (AArenaJumpPad* Pad : Builder->GetJumpPads())
		{
			if (Pad)
			{
				FPadInfo Info;
				Info.PadNode = AddSpecialNode(Pad->GetActorLocation() + FVector(0, 0, 10.f));
				Info.TargetNode = AddSpecialNode(Pad->GetTarget() + FVector(0, 0, 10.f));
				Info.PadLoc = Nodes[Info.PadNode].Location;
				Pads.Add(Info);
			}
		}
		for (const FVector& Point : ExtraPoints)
		{
			AddSpecialNode(Point);
		}
		// The map's own UE2 navigation points (PathNodes, InventorySpots, JumpSpots, Doors, LiftExits ...): the mapper
		// placed them where a pawn fits, e.g. in doorways narrower than the grid can sample. Each one whose spot fits
		// the (step reduced) capsule becomes a node, so every UE2 ReachSpec has nodes at both ends.
		int32 NumUE2Points = 0;
		for (const TCHAR* List : { TEXT("path_nodes"), TEXT("inventory_spots"), TEXT("jump_spots"), TEXT("other_nav_points"), TEXT("lifts") })
		{
			for (const FArenaRawMarker& Mk : Builder->GetRawMarkers(List))
			{
				if (Mk.IsA(TEXT("LiftCenter")))
				{
					continue; // rides on its lift (the lift links have their own nodes)
				}
				// UE2 paths through water (R_SWIM: AS-Glacier's flooded tunnel) are swum: the point itself is the node
				if (const UArenaWorldZoneSubsystem* WZ = UArenaWorldZoneSubsystem::Get(World))
				{
					const AArenaPhysicsVolume* V = WZ->GetPhysicsVolumeAt(Mk.Location);
					if (V && V->bWaterVolume)
					{
						if (Ctx.SweepClear(Mk.Location, Mk.Location))
						{
							FArenaNavNode& WN = Nodes.AddDefaulted_GetRef();
							WN.Location = Mk.Location;
							UE2PointNodes.Add(Nodes.Num() - 1);
							WaterNodes.Add(Nodes.Num() - 1);
							++NumUE2Points;
						}
						continue;
					}
				}
				float FloorZ = 0.f;
				bool bAdded = false;
				if (Ctx.FindFloor(Mk.Location, 60.f, 400.f, FloorZ))
				{
					const FVector P(Mk.Location.X, Mk.Location.Y, FloorZ + SweepLift);
					if (Ctx.SweepClear(P, P))
					{
						UE2PointNodes.Add(AddSpecialNode(Mk.Location));
						++NumUE2Points;
						bAdded = true;
					}
				}
				if (!bAdded)
				{
					DeferredPoints.Add(Mk.Location);
				}
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d UE2 navigation point node(s)"), NumUE2Points);
		// UE2 ForcedPaths to JumpDests (JumpSpot): long jumps the map's author linked by hand (low gravity islands such
		// as BR-Skyline). Kept when a jump at up to dodge speed with up to a double jump's lift reaches them (linked below).
		CollectForcedJumps(Ctx, [&AddSpecialNode](const FVector& L) { return AddSpecialNode(L); }, ForcedJumps);
		// the same ForcedPaths for bots with the translocator (ArenaTranslocatorNav.cpp)
		CollectTranslocatorSpots([&AddSpecialNode](const FVector& L) { return AddSpecialNode(L); });
	}
	else
	{
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			AnchorNodes.Add(AddSpecialNode(It->GetActorLocation()));
		}
	}

	// Nodes standing on a jump pad only leave through the pad
	TArray<int32> NodePad;
	NodePad.Init(INDEX_NONE, Nodes.Num());
	for (int32 p = 0; p < Pads.Num(); ++p)
	{
		const FVector& PadLoc = Pads[p].PadLoc;
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			const FVector& L = Nodes[i].Location;
			if (i != Pads[p].TargetNode && FVector::DistSquared2D(L, PadLoc) < FMath::Square(PadRadius) && FMath::Abs(L.Z - PadLoc.Z) < 60.f)
			{
				NodePad[i] = p;
			}
		}
	}

	// UE2 lifts and teleporters (native world actors on runtime maps)
	struct FLiftInfo { int32 Bottom; int32 Top; TWeakObjectPtr<AArenaMover> Mover; TArray<int32> Exits; };
	TArray<FLiftInfo> LiftInfos;
	struct FTeleInfo { int32 From; int32 To; };
	TArray<FTeleInfo> TeleInfos;
	for (TActorIterator<AArenaMover> It(World); It; ++It)
	{
		AArenaMover* M = *It;
		if (M->NumKeys < 2 || M->IsMoving() || M->KeyNum != 0 || M->bRotating)
		{
			continue;
		}
		const FVector Bottom = M->GetKeyLocation(0);
		const FVector Top = M->GetKeyLocation(M->NumKeys - 1);
		// lifts: mostly vertical travel the bots can stand on (doors move sideways)
		if (Top.Z - Bottom.Z < 150.f || FVector::Dist2D(Top, Bottom) > (Top.Z - Bottom.Z) * 0.5f)
		{
			continue;
		}
		if (M->InitialState != EArenaMoverState::StandOpenTimed && M->InitialState != EArenaMoverState::BumpOpenTimed)
		{
			continue;
		}
		FLiftInfo L;
		// UE2 LiftExits of this lift (LiftTag = the mover's Tag) near the top stop
		if (Builder)
		{
			for (const FArenaRawMarker& Mk : Builder->GetRawMarkers(TEXT("lifts")))
			{
				FString LiftTag;
				Mk.Json->TryGetStringField(TEXT("lift_tag"), LiftTag);
				const bool bTagged = !LiftTag.IsEmpty() && FName(*LiftTag) == M->Tag;
				if (Mk.IsA(TEXT("LiftExit")) && FMath::Abs(Mk.Location.Z - Top.Z) < 200.f
					&& (bTagged || FVector::Dist2D(Mk.Location, Top) < 1000.f))
				{
					L.Exits.Add(AddSpecialNode(Mk.Location + FVector(0, 0, 20.f)));
				}
			}
		}
		L.Bottom = AddSpecialNode(Bottom + FVector(0, 0, 20.f));
		// no floor at the top while the lift is down: place the node at the lift top
		FArenaNavNode& TopNode = Nodes.AddDefaulted_GetRef();
		TopNode.Location = Top + FVector(0, 0, NodeHeight);
		L.Top = Nodes.Num() - 1;
		L.Mover = M;
		Nodes[L.Bottom].Lift = M;
		LiftInfos.Add(L);
	}
	for (TActorIterator<AArenaTeleporter> It(World); It; ++It)
	{
		for (AArenaTeleporter* D : It->GetDestinations())
		{
			FTeleInfo T;
			T.From = AddSpecialNode(It->GetFloorLocation() + FVector(0, 0, 20.f));
			T.To = AddSpecialNode(D->GetFloorLocation() + FVector(0, 0, 20.f));
			// teleporters are UE2 navigation points too (reached by long ReachSpecs, e.g. up a ramp onto a pedestal)
			UE2PointNodes.Add(T.From);
			UE2PointNodes.Add(T.To);
			TeleInfos.Add(T);
		}
	}
	for (int32 i = NodePad.Num(); i < Nodes.Num(); ++i)
	{
		NodePad.Add(INDEX_NONE);
	}
	// nodes in lava / pain / kill volumes are never link targets
	TArray<bool> Hazard;
	Hazard.Init(false, Nodes.Num());
	if (const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(World))
	{
		// below its zone's KillZ (CTF-Citadel's canyon zone) a node is as deadly as lava
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			const AArenaZoneInfo* Zone = Zones->GetZoneAt(Nodes[i].Location);
			Hazard[i] = Zone && Nodes[i].Location.Z - (NodeHeight - CapsuleHalfHeight) <= Zone->KillZ + 20.f;
		}
		if (Zones->HasVolumes())
		{
			int32 NumHazard = 0;
			for (int32 i = 0; i < Nodes.Num(); ++i)
			{
				Hazard[i] = Hazard[i] || Zones->IsHazardAt(Nodes[i].Location - FVector(0, 0, NodeHeight - 20.f));
				NumHazard += Hazard[i] ? 1 : 0;
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d lift(s), %d teleporter link(s), %d node(s) in hazard volumes"), LiftInfos.Num(), TeleInfos.Num(), NumHazard);
		}
	}

	auto LinkAllowed = [&](int32 From, int32 To) -> bool
	{
		if (NodePad[From] != INDEX_NONE || (Hazard.IsValidIndex(To) && Hazard[To]))
		{
			return false;
		}
		const FVector& A = Nodes[From].Location;
		const FVector& B = Nodes[To].Location;
		for (int32 p = 0; p < Pads.Num(); ++p)
		{
			if (NodePad[To] == p)
			{
				continue;
			}
			const FVector& PadLoc = Pads[p].PadLoc;
			if (FMath::Abs(PadLoc.Z - (A.Z + B.Z) * 0.5f) < 150.f &&
				SegmentDist2D(FVector2D(PadLoc.X, PadLoc.Y), FVector2D(A.X, A.Y), FVector2D(B.X, B.Y)) < PadRadius)
			{
				return false;
			}
		}
		return true;
	};

	auto AddLink = [this](int32 From, int32 To, EArenaNavLinkType Type)
	{
		FArenaNavLink& Link = Nodes[From].Links.AddDefaulted_GetRef();
		Link.To = To;
		Link.Type = Type;
		Link.Cost = LinkCost(Type, Nodes[From].Location, Nodes[To].Location);
	};

	PhaseTimes.Add(TPair<const TCHAR*, double>(TEXT("special"), FPlatformTime::Seconds()));
	// ---------------- 3. linking ----------------
	TMap<FIntVector, TArray<int32>> BuildCells;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		BuildCells.FindOrAdd(CellOf(Nodes[i].Location)).Add(i);
	}

	// low gravity (UE2 physics volumes): falls carry further and land softer, so longer drops are safe
	const float DropRadius = FMath::Min(LongDropRadius / Ctx.MinGravityRatio, 3.f * LongDropRadius);
	const float MaxLowGravDrop = MaxDrop / Ctx.MinGravityRatio;
	const int32 CellRangeXY = FMath::CeilToInt(FMath::Max(LinkRadius, DropRadius) / CellSize);
	const TArray<int32> UE2PointList = UE2PointNodes.Array();
	const int32 CellRangeZ = FMath::CeilToInt(MaxLowGravDrop / CellSize);
	const float LinkRadiusSq = FMath::Square(LinkRadius);
	const float LongDropRadiusSq = FMath::Square(DropRadius);

	// A jump over the void (nothing to land on under its middle within a long fall, CTF-Maul's flag platforms) is a
	// risk a bot takes only when there is no other way: it costs much more than a walk around
	const float WorldKillZ = World->GetWorldSettings() ? World->GetWorldSettings()->KillZ : -1.0e6f;
	// Nodes are linked lazily, breadth first from the player starts: only nodes the starts can reach get their
	// links simulated. Everything else would be pruned below anyway, so imported maps skip the (expensive)
	// simulation for roofs, sky boxes and the solid space between rooms. Each node pair is still evaluated
	// once, in both directions, when the first of the two is expanded.
	// lift / teleporter links first, so the lazy expansion below follows them
	for (const FLiftInfo& L : LiftInfos)
	{
		AddLink(L.Bottom, L.Top, EArenaNavLinkType::Lift);
		const FVector T = Nodes[L.Top].Location;
		int32 Exits = 0;
		for (int32 E : L.Exits)
		{
			AddLink(L.Top, E, EArenaNavLinkType::Walk);
			++Exits;
		}
		for (int32 j = 0; j < Nodes.Num(); ++j)
		{
			const FVector& N = Nodes[j].Location;
			if (j != L.Top && j != L.Bottom && FVector::DistSquared2D(N, T) < FMath::Square(450.f) && FMath::Abs(N.Z - T.Z) < 70.f
				&& !Hazard[j] && IsClearLine(T, N))
			{
				AddLink(L.Top, j, EArenaNavLinkType::Walk);
				++Exits;
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: lift %s %s -> %s, %d exit link(s)"), L.Mover.IsValid() ? *L.Mover->UE2Name : TEXT("?"),
			*Nodes[L.Bottom].Location.ToCompactString(), *T.ToCompactString(), Exits);
	}
	// swim links: a water node and the UE2 navigation points around it (in the water or at its edges), wherever the
	// straight line between them is clear for the (step reduced) capsule; bots swim it in 3D
	{
		int32 NumSwim = 0;
		const TArray<int32> Points = UE2PointNodes.Array();
		for (const int32 w : WaterNodes)
		{
			const FVector A = Nodes[w].Location;
			for (const int32 j : Points)
			{
				if (j == w || (WaterNodes.Contains(j) && j < w))
				{
					continue; // water pairs once
				}
				const FVector B = Nodes[j].Location;
				if (FVector::DistSquared(A, B) > FMath::Square(UE2PointLinkRadius) || Hazard[j])
				{
					continue;
				}
				// land points: the line from their capsule centre; water points: their own spot
				const FVector BFrom = WaterNodes.Contains(j) ? B : B - FVector(0.f, 0.f, NodeHeight - SweepLift);
				if (Ctx.SweepClear(A, BFrom))
				{
					AddLink(w, j, EArenaNavLinkType::Walk);
					AddLink(j, w, EArenaNavLinkType::Walk);
					++NumSwim;
				}
			}
		}
		if (NumSwim > 0)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d swim link pair(s) through %d water node(s)"), NumSwim, WaterNodes.Num());
		}
	}
	for (const FTeleInfo& T : TeleInfos)
	{
		AddLink(T.From, T.To, EArenaNavLinkType::Teleport);
	}
	// forced jumps (collected with the special nodes): only when the arc is clear
	int32 NumForced = 0;
	for (const FForcedJump& F : ForcedJumps)
	{
		const FVector A = Nodes[F.From].Location;
		const FVector B = Nodes[F.To].Location;
		const float G = Ctx.GravityAt(A);
		const float T = FVector::Dist2D(A, B) / F.Speed;
		const float Vz = (B.Z - A.Z + 0.5f * G * T * T) / T;
		bool bClear = !Hazard[F.To];
		FVector Prev = A + FVector(0, 0, SweepLift - NodeHeight);
		for (int32 k = 1; k <= 12 && bClear; ++k)
		{
			const float t = T * k / 12.f;
			FVector P = FMath::Lerp(A, B, float(k) / 12.f);
			P.Z = A.Z + Vz * t - 0.5f * G * t * t + (SweepLift - NodeHeight);
			bClear = Ctx.SweepClear(Prev, P);
			Prev = P;
		}
		if (bClear)
		{
			AddLink(F.From, F.To, EArenaNavLinkType::ForcedJump);
			++NumForced;
		}
	}
	if (ForcedJumps.Num() > 0)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d of %d UE2 ForcedPaths jumps usable"), NumForced, ForcedJumps.Num());
	}
	AddTranslocatorLinks();

	PhaseTimes.Add(TPair<const TCHAR*, double>(TEXT("links-prep"), FPlatformTime::Seconds()));
	TArray<bool> Expanded;
	Expanded.Init(false, Nodes.Num());
	TArray<bool> Queued;
	Queued.Init(false, Nodes.Num());
	TArray<int32> Queue;
	Queue.Reserve(Nodes.Num());
	auto Enqueue = [&Queue, &Queued](int32 N)
	{
		if (!Queued[N])
		{
			Queued[N] = true;
			Queue.Add(N);
		}
	};
	if (AnchorNodes.Num() > 0)
	{
		for (const int32 A : AnchorNodes)
		{
			Enqueue(A);
		}
	}
	else
	{
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			Enqueue(i);
		}
	}

	// The expansion runs in waves: every node queued so far is simulated in parallel (scene queries only read), then
	// the links found are added and their ends queued for the next wave. A pair is still evaluated once: by the node
	// expanded first, or by the lower index when both are in the same wave.
	struct FPendingLink { int32 From; int32 To; EArenaNavLinkType Type; bool bVoidJump; };
	auto Classify = [&](int32 From, int32 To, ESim Result, float MaxGap, TArray<FPendingLink>& Out)
	{
		if (!LinkAllowed(From, To))
		{
			return;
		}
		const FVector& A = Nodes[From].Location;
		const FVector& B = Nodes[To].Location;
		bool bJump = false;
		switch (Result)
		{
		case ESim::Walk:
			Out.Add({ From, To, EArenaNavLinkType::Walk, false });
			return;
		case ESim::Drop:
			Out.Add({ From, To, EArenaNavLinkType::Drop, false });
			return;
		case ESim::Gap:
			bJump = MaxGap < MaxGapJump / Ctx.GravityRatioAt(A) && FMath::Abs(A.Z - B.Z) <= MaxStep * 2.f && Ctx.CanJump(A, B);
			break;
		default:
			bJump = B.Z - A.Z >= -MaxStep && Ctx.CanJump(A, B);
			break;
		}
		if (bJump)
		{
			// a jump over the void (nothing under its middle within a long fall) costs much more than a walk around
			const FVector Mid = (A + B) * 0.5f;
			FHitResult Hit;
			const float Bottom = FMath::Max<float>(Mid.Z - NodeHeight - MaxDrop, WorldKillZ);
			const bool bVoid = !Ctx.TraceDown(FVector2D(Mid.X, Mid.Y), Mid.Z, Bottom, Hit) || Hazard[To];
			Out.Add({ From, To, EArenaNavLinkType::Jump, bVoid });
		}
	};
	TArray<bool> InWave;
	InWave.Init(false, Nodes.Num());
	int32 Head = 0;
	while (Head < Queue.Num())
	{
		const int32 WaveEnd = Queue.Num();
		for (int32 q = Head; q < WaveEnd; ++q)
		{
			InWave[Queue[q]] = true;
		}
		TArray<TArray<FPendingLink>> Found;
		Found.SetNum(WaveEnd - Head);
		static const bool bSerial = FParse::Param(FCommandLine::Get(), TEXT("NavSerial"));
		ParallelFor(WaveEnd - Head, [&](int32 w)
		{
			const int32 i = Queue[Head + w];
			TArray<FPendingLink>& Out = Found[w];
			const FVector A = Nodes[i].Location;
			const FIntVector C = CellOf(A);
			const float RatioA = Ctx.GravityRatioAt(A);
			const float MaxDropA = MaxDrop / RatioA;
			TArray<int32> LongDropCandidates;
			auto Skip = [&](int32 j) { return Expanded[j] || (InWave[j] && j < i); };
			for (int32 dx = -CellRangeXY; dx <= CellRangeXY; ++dx)
			for (int32 dy = -CellRangeXY; dy <= CellRangeXY; ++dy)
			for (int32 dz = -CellRangeZ; dz <= CellRangeZ; ++dz)
			{
				const TArray<int32>* Cell = BuildCells.Find(C + FIntVector(dx, dy, dz));
				if (!Cell)
				{
					continue;
				}
				for (const int32 j : *Cell)
				{
					if (j == i)
					{
						continue;
					}
					const FVector B = Nodes[j].Location;
					const float D2Sq = FVector::DistSquared2D(A, B);
					if (D2Sq > LinkRadiusSq)
					{
						// Long drop candidates are only taken from the higher node, which is the one being expanded
						if (D2Sq <= LongDropRadiusSq && A.Z - B.Z >= LongDropMinHeight && A.Z - B.Z <= MaxDropA &&
							D2Sq <= FMath::Square(RunSpeed * FMath::Sqrt(2.f * (A.Z - B.Z) / (Gravity * RatioA)) * LongDropReachScale))
						{
							LongDropCandidates.Add(j);
						}
						continue;
					}
					if (Skip(j) || D2Sq < 1.f || FMath::Abs(A.Z - B.Z) > MaxLowGravDrop)
					{
						continue; // pair evaluated from the other side
					}
					float GapAB = 0.f;
					const ESim AB = Ctx.Simulate(A, B, GapAB);
					if (AB == ESim::Walk)
					{
						// Walkable steps are symmetric
						if (LinkAllowed(i, j)) { Out.Add({ i, j, EArenaNavLinkType::Walk, false }); }
						if (LinkAllowed(j, i)) { Out.Add({ j, i, EArenaNavLinkType::Walk, false }); }
						continue;
					}
					float GapBA = 0.f;
					const ESim BA = Ctx.Simulate(B, A, GapBA);
					Classify(i, j, AB, GapAB, Out);
					Classify(j, i, BA, GapBA, Out);
				}
			}
			// Two UE2 navigation points: UE2 links them over long distances where the floor is sparse for the grid
			// (ramps, narrow bridges and beams): pairs beyond LinkRadius are tested like near pairs
			if (UE2PointNodes.Contains(i))
			{
				for (const int32 j : UE2PointList)
				{
					const FVector B = Nodes[j].Location;
					const float D2Sq = FVector::DistSquared2D(A, B);
					if (j == i || Skip(j) || D2Sq <= LinkRadiusSq || D2Sq > FMath::Square(UE2PointLinkRadius) || FMath::Abs(A.Z - B.Z) > MaxLowGravDrop)
					{
						continue;
					}
					float GapAB = 0.f, GapBA = 0.f;
					const ESim AB = Ctx.Simulate(A, B, GapAB);
					const ESim BA = Ctx.Simulate(B, A, GapBA);
					if (AB == ESim::Walk || AB == ESim::Drop)
					{
						Classify(i, j, AB, GapAB, Out);
					}
					if (BA == ESim::Walk || BA == ESim::Drop)
					{
						Classify(j, i, BA, GapBA, Out);
					}
				}
			}
			// Long one-way drops, only from ledge nodes (an open floor node has 8 walk links around it). They cover
			// ledges whose landing spot is farther than LinkRadius, e.g. spawn balconies above a pit.
			if (LongDropCandidates.Num() > 0)
			{
				int32 WalkOut = 0;
				for (const FArenaNavLink& Link : Nodes[i].Links)
				{
					WalkOut += Link.Type == EArenaNavLinkType::Walk ? 1 : 0;
				}
				for (const FPendingLink& L : Out)
				{
					WalkOut += (L.From == i && L.Type == EArenaNavLinkType::Walk) ? 1 : 0;
				}
				if (WalkOut < 8)
				{
					for (const int32 j : LongDropCandidates)
					{
						float GapDrop = 0.f;
						if (LinkAllowed(i, j) && Ctx.Simulate(A, Nodes[j].Location, GapDrop) == ESim::Drop)
						{
							Out.Add({ i, j, EArenaNavLinkType::Drop, false });
						}
					}
				}
			}
		}, bSerial ? EParallelForFlags::ForceSingleThread : EParallelForFlags::None);
		// apply the wave's links, then queue what they reach
		for (const TArray<FPendingLink>& Out : Found)
		{
			for (const FPendingLink& L : Out)
			{
				AddLink(L.From, L.To, L.Type);
				if (L.bVoidJump)
				{
					FArenaNavLink& Last = Nodes[L.From].Links.Last();
					Last.Cost = Last.Cost * 3.f + 2000.f;
				}
			}
		}
		for (int32 q = Head; q < WaveEnd; ++q)
		{
			const int32 i = Queue[q];
			Expanded[i] = true;
			InWave[i] = false;
		}
		for (int32 q = Head; q < WaveEnd; ++q)
		{
			const int32 i = Queue[q];
			for (const FArenaNavLink& Link : Nodes[i].Links)
			{
				Enqueue(Link.To);
			}
			if (Pads.IsValidIndex(NodePad[i]) && i != Pads[NodePad[i]].TargetNode)
			{
				Enqueue(Pads[NodePad[i]].TargetNode);
			}
		}
		Head = WaveEnd;
	}

	PhaseTimes.Add(TPair<const TCHAR*, double>(TEXT("expand"), FPlatformTime::Seconds()));
	// Jump pads
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		if (Pads.IsValidIndex(NodePad[i]) && i != Pads[NodePad[i]].TargetNode)
		{
			Nodes[i].Links.Reset();
			AddLink(i, Pads[NodePad[i]].TargetNode, EArenaNavLinkType::JumpPad);
		}
	}

	// Physics volume streams (BR-Colossus' goal-return and side tubes): a volume that blows pawns along, ending at a
	// mid-air pad, carries whoever enters it (or a volume touching it) to that pad. Like UE2's ReachSpecs into the
	// tubes: a one-way link from every node in the stream to the pad's landing spot.
	{
		TArray<const AArenaPhysicsVolume*> Blowing;
		for (TActorIterator<AArenaPhysicsVolume> It(World); It; ++It)
		{
			const FVector& G = It->Gravity;
			if (It->Shape.IsValid() && (!It->ZoneVelocity.IsNearlyZero() || G.Z > 0.f || FVector2D(G.X, G.Y).Size() > FMath::Abs(G.Z)))
			{
				Blowing.Add(*It);
			}
		}
		int32 NumStreamLinks = 0;
		for (const FPadInfo& Pad : Pads)
		{
			// the stream ending at this pad, grown by the blowing volumes touching it
			TArray<const AArenaPhysicsVolume*> Group;
			for (const AArenaPhysicsVolume* V : Blowing)
			{
				if (V->Shape.Encompasses(Pad.PadLoc, 600.f))
				{
					Group.Add(V);
				}
			}
			for (int32 g = 0; g < Group.Num(); ++g)
			{
				for (const AArenaPhysicsVolume* V : Blowing)
				{
					if (!Group.Contains(V) && V->Shape.Bounds.ExpandBy(50.f).Intersect(Group[g]->Shape.Bounds))
					{
						Group.Add(V);
					}
				}
			}
			if (Group.Num() == 0)
			{
				continue;
			}
			for (int32 i = 0; i < Nodes.Num(); ++i)
			{
				if (i == Pad.TargetNode || i == Pad.PadNode || NodePad[i] != INDEX_NONE || Nodes[i].Links.Num() == 0)
				{
					continue;
				}
				const FVector Feet = Nodes[i].Location - FVector(0, 0, NodeHeight - 20.f);
				for (const AArenaPhysicsVolume* V : Group)
				{
					if (V->Shape.Encompasses(Feet))
					{
						AddLink(i, Pad.TargetNode, EArenaNavLinkType::JumpPad);
						++NumStreamLinks;
						break;
					}
				}
			}
		}
		if (NumStreamLinks > 0)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d stream link(s) through blowing physics volumes to their exit pads"), NumStreamLinks);
		}
	}

	// Diagnostics: pads that nobody can walk onto, or whose landing spot leads nowhere, get pruned below.
	{
		TArray<int32> Incoming;
		Incoming.Init(0, Nodes.Num());
		for (const FArenaNavNode& Node : Nodes)
		{
			for (const FArenaNavLink& L : Node.Links)
			{
				Incoming[L.To]++;
			}
		}
		for (const FPadInfo& Pad : Pads)
		{
			int32 PadIn = 0;
			for (int32 i = 0; i < Nodes.Num(); ++i)
			{
				if (Pads.IsValidIndex(NodePad[i]) && Pads[NodePad[i]].PadNode == Pad.PadNode)
				{
					PadIn += Incoming[i];
				}
			}
			UE_LOG(LogOpenUT2k4, Verbose, TEXT("NavGraph: pad at %s: %d links in; target %s: %d links out"),
				*Pad.PadLoc.ToString(), PadIn, *Nodes[Pad.TargetNode].Location.ToString(), Nodes[Pad.TargetNode].Links.Num());
			// Tube exits: a pad at the end of a physics volume that blows players along (sideways or upward gravity,
			// ZoneVelocity) is entered in flight, not on foot. UT2004's own path network has no reach specs into
			// those either (e.g. BR-Colossus' goal-return and side tubes), so no walking link in is expected.
			const AArenaPhysicsVolume* Stream = nullptr;
			if (PadIn == 0 && Nodes[Pad.TargetNode].Links.Num() > 0)
			{
				for (TActorIterator<AArenaPhysicsVolume> It(World); It && !Stream; ++It)
				{
					const FVector& G = It->Gravity;
					const bool bBlows = !It->ZoneVelocity.IsNearlyZero() || G.Z > 0.f || FVector2D(G.X, G.Y).Size() > FMath::Abs(G.Z);
					if (bBlows && It->Shape.IsValid() && It->Shape.Encompasses(Pad.PadLoc, 600.f))
					{
						Stream = *It;
					}
				}
			}
			if (Stream)
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: jump pad at %s is the exit of the %s stream (entered in flight, no walking route in); landing %s has %d links out"),
					*Pad.PadLoc.ToString(), *Stream->UE2Name, *Nodes[Pad.TargetNode].Location.ToString(), Nodes[Pad.TargetNode].Links.Num());
			}
			else if (PadIn == 0 || Nodes[Pad.TargetNode].Links.Num() == 0)
			{
				// nodes around the pad at about its height, and how many of them the walk from the starts reached
				int32 Around = 0, Reached = 0, Nearest = INDEX_NONE;
				for (int32 i = 0; i < Nodes.Num(); ++i)
				{
					const FVector& L = Nodes[i].Location;
					if (Nodes[i].Links.Num() > 0 && NodePad[i] == INDEX_NONE && i != Pad.TargetNode
						&& (Nearest == INDEX_NONE || FVector::DistSquared(L, Pad.PadLoc) < FVector::DistSquared(Nodes[Nearest].Location, Pad.PadLoc)))
					{
						Nearest = i;
					}
					if (NodePad[i] == INDEX_NONE && FVector::DistSquared2D(L, Pad.PadLoc) < FMath::Square(LinkRadius) && FMath::Abs(L.Z - Pad.PadLoc.Z) < 250.f)
					{
						++Around;
						Reached += Nodes[i].Links.Num() > 0 ? 1 : 0;
					}
				}
				UE_LOG(LogOpenUT2k4, Warning, TEXT("NavGraph: jump pad at %s is not connected (%d links in, landing %s has %d links out; %d nodes around it, %d reached; nearest reached node %s)"),
					*Pad.PadLoc.ToString(), PadIn, *Nodes[Pad.TargetNode].Location.ToString(), Nodes[Pad.TargetNode].Links.Num(), Around, Reached,
					Nearest != INDEX_NONE ? *Nodes[Nearest].Location.ToString() : TEXT("none"));
			}
		}
	}

	// links through scripted doors (checked by FindPath against the door's state)
	if (ScriptedDoorBoxes.Num() > 0)
	{
		int32 NumDoorLinks = 0;
		for (FArenaNavNode& N : Nodes)
		{
			for (FArenaNavLink& L : N.Links)
			{
				const FVector A = N.Location;
				const FVector B = Nodes[L.To].Location;
				const FVector D = B - A;
				if (D.IsNearlyZero())
				{
					continue;
				}
				for (int32 d = 0; d < ScriptedDoorBoxes.Num(); ++d)
				{
					if (ScriptedDoorBoxes[d].IsInside(A) || ScriptedDoorBoxes[d].IsInside(B) || FMath::LineBoxIntersection(ScriptedDoorBoxes[d], A, B, D))
					{
						L.Door = d;
						++NumDoorLinks;
						break;
					}
				}
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d link(s) pass %d scripted door(s)"), NumDoorLinks, ScriptedDoorBoxes.Num());
	}

	PhaseTimes.Add(TPair<const TCHAR*, double>(TEXT("pads-streams-doors"), FPlatformTime::Seconds()));
	// ---------------- 4. prune ----------------
	const int32 NumBefore = Nodes.Num();
	TArray<TArray<int32>> Reverse;
	Reverse.SetNum(NumBefore);
	TArray<bool> HasLinks;
	HasLinks.Init(false, NumBefore);
	for (int32 i = 0; i < NumBefore; ++i)
	{
		for (const FArenaNavLink& L : Nodes[i].Links)
		{
			Reverse[L.To].Add(i);
			HasLinks[i] = true;
			HasLinks[L.To] = true;
		}
	}

	auto Flood = [NumBefore](const TArray<int32>& Seeds, TFunctionRef<void(int32, TArray<int32>&)> Neighbours) -> TArray<bool>
	{
		TArray<bool> Visited;
		Visited.Init(false, NumBefore);
		TArray<int32> Stack;
		for (int32 S : Seeds)
		{
			if (!Visited[S]) { Visited[S] = true; Stack.Add(S); }
		}
		TArray<int32> Next;
		while (Stack.Num() > 0)
		{
			const int32 Cur = Stack.Pop(EAllowShrinking::No);
			Next.Reset();
			Neighbours(Cur, Next);
			for (int32 N : Next)
			{
				if (!Visited[N]) { Visited[N] = true; Stack.Add(N); }
			}
		}
		return Visited;
	};

	TArray<int32> Seeds;
	for (int32 A : AnchorNodes)
	{
		if (HasLinks[A])
		{
			Seeds.Add(A);
		}
	}

	TArray<bool> Keep;
	Keep.Init(false, NumBefore);
	int32 NonIsolated = 0;
	for (int32 i = 0; i < NumBefore; ++i)
	{
		NonIsolated += HasLinks[i] ? 1 : 0;
	}
	int32 KeepCount = 0;
	if (Seeds.Num() > 0)
	{
		const TArray<bool> Fwd = Flood(Seeds, [this](int32 Cur, TArray<int32>& Out)
		{
			for (const FArenaNavLink& L : Nodes[Cur].Links) { Out.Add(L.To); }
		});
		// Keep what the starts reach and what can get back to the main play area. The main area is the largest
		// strongly connected component reachable from the starts, not the starts themselves: spawn rooms on
		// imported maps are often left by a one-way drop and can't be re-entered. The starts stay seeds too.
		TArray<int32> Largest;
		{
			// Iterative Tarjan over the forward-reachable nodes
			TArray<int32> Index, Low;
			Index.Init(INDEX_NONE, NumBefore);
			Low.Init(0, NumBefore);
			TArray<bool> OnStack;
			OnStack.Init(false, NumBefore);
			TArray<int32> SccStack;
			struct FFrame { int32 Node; int32 Link; };
			TArray<FFrame> CallStack;
			int32 Counter = 0;
			TArray<int32> Component;
			for (int32 Root = 0; Root < NumBefore; ++Root)
			{
				if (!Fwd[Root] || Index[Root] != INDEX_NONE)
				{
					continue;
				}
				CallStack.Add({ Root, 0 });
				Index[Root] = Low[Root] = Counter++;
				SccStack.Add(Root);
				OnStack[Root] = true;
				while (CallStack.Num() > 0)
				{
					FFrame& F = CallStack.Last();
					const TArray<FArenaNavLink>& Links = Nodes[F.Node].Links;
					if (F.Link < Links.Num())
					{
						const int32 To = Links[F.Link++].To;
						if (Index[To] == INDEX_NONE)
						{
							Index[To] = Low[To] = Counter++;
							SccStack.Add(To);
							OnStack[To] = true;
							CallStack.Add({ To, 0 }); // invalidates F
						}
						else if (OnStack[To])
						{
							Low[F.Node] = FMath::Min(Low[F.Node], Index[To]);
						}
						continue;
					}
					const int32 Done = F.Node;
					CallStack.Pop(EAllowShrinking::No);
					if (CallStack.Num() > 0)
					{
						const int32 Parent = CallStack.Last().Node;
						Low[Parent] = FMath::Min(Low[Parent], Low[Done]);
					}
					if (Low[Done] == Index[Done])
					{
						Component.Reset();
						int32 W;
						do
						{
							W = SccStack.Pop(EAllowShrinking::No);
							OnStack[W] = false;
							Component.Add(W);
						} while (W != Done);
						if (Component.Num() > Largest.Num())
						{
							Largest = Component;
						}
					}
				}
			}
		}
		TArray<int32> Core = Seeds;
		Core.Append(Largest);
		const TArray<bool> Bwd = Flood(Core, [&Reverse](int32 Cur, TArray<int32>& Out)
		{
			Out.Append(Reverse[Cur]);
		});
		for (int32 i = 0; i < NumBefore; ++i)
		{
			Keep[i] = Fwd[i] && Bwd[i];
			KeepCount += Keep[i] ? 1 : 0;
		}
	}
	if (Seeds.Num() == 0 || KeepCount < NonIsolated / 4)
	{
		// Reachability prune looks wrong (no starts / poorly connected): only drop isolated nodes
		for (int32 i = 0; i < NumBefore; ++i)
		{
			Keep[i] = HasLinks[i];
		}
	}

	// Development aid: -ArenaDumpNav writes the unpruned graph to Saved/NavGraph_<Map>.csv
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaDumpNav")))
	{
		FString Csv = TEXT("kind,index,x,y,z,keep,to,type\n");
		for (int32 i = 0; i < NumBefore; ++i)
		{
			const FVector& L = Nodes[i].Location;
			Csv += FString::Printf(TEXT("node,%d,%.0f,%.0f,%.0f,%d,%d,%d\n"), i, L.X, L.Y, L.Z, Keep[i] ? 1 : 0, AnchorNodes.Contains(i) ? 1 : 0, NodePad.IsValidIndex(i) ? NodePad[i] : -1);
			for (const FArenaNavLink& Link : Nodes[i].Links)
			{
				Csv += FString::Printf(TEXT("link,%d,0,0,0,0,%d,%d\n"), i, Link.To, int32(Link.Type));
			}
		}
		const FString File = FPaths::ProjectSavedDir() / FString::Printf(TEXT("NavGraph_%s.csv"), *World->GetMapName());
		FFileHelper::SaveStringToFile(Csv, *File);
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: dumped to %s"), *File);
	}
#endif

	TArray<int32> Remap;
	Remap.Init(INDEX_NONE, NumBefore);
	TArray<FArenaNavNode> NewNodes;
	NewNodes.Reserve(NumBefore);
	for (int32 i = 0; i < NumBefore; ++i)
	{
		if (Keep[i])
		{
			Remap[i] = NewNodes.Num();
			NewNodes.Add(Nodes[i]);
		}
	}
	int32 Counts[4] = { 0, 0, 0, 0 };
	for (FArenaNavNode& Node : NewNodes)
	{
		for (int32 l = Node.Links.Num() - 1; l >= 0; --l)
		{
			const int32 NewTo = Remap[Node.Links[l].To];
			if (NewTo == INDEX_NONE)
			{
				Node.Links.RemoveAtSwap(l, EAllowShrinking::No);
			}
			else
			{
				Node.Links[l].To = NewTo;
				Counts[uint8(Node.Links[l].Type) & 3]++;
			}
		}
		Node.Links.Shrink();
	}
	Nodes = MoveTemp(NewNodes);

	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		Cells.FindOrAdd(CellOf(Nodes[i].Location)).Add(i);
	}

	const double Ms = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	{
		FString Phases;
		double Prev = StartTime;
		PhaseTimes.Add(TPair<const TCHAR*, double>(TEXT("prune"), FPlatformTime::Seconds()));
		for (const TPair<const TCHAR*, double>& Ph : PhaseTimes)
		{
			Phases += FString::Printf(TEXT(" %s %.0f ms;"), Ph.Key, (Ph.Value - Prev) * 1000.0);
			Prev = Ph.Value;
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: build phases:%s"), *Phases);
	}
	if (!FMath::IsNearlyEqual(Ctx.BaseRatio, 1.f, 0.01f))
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: level gravity x%.2f of the stock one (jumps and drops reach further)"), Ctx.BaseRatio);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d nodes (%d grid + %d special, %d pruned), %d links (walk %d, jump %d, drop %d, pad %d) in %.1f ms"),
		Nodes.Num(), NumGridNodes, NumBefore - NumGridNodes, NumBefore - Nodes.Num(),
		Counts[0] + Counts[1] + Counts[2] + Counts[3], Counts[0], Counts[1], Counts[2], Counts[3], Ms);
	BuildParams = Ctx.Params;
	if (!CacheFile.IsEmpty())
	{
		SaveCache(CacheFile, DoorNames);
	}
#if !UE_BUILD_SHIPPING
	FString CompareFile;
	if (FParse::Value(FCommandLine::Get(), TEXT("NavCompare="), CompareFile))
	{
		RunCompareTest(CompareFile);
	}
	// -NavProbe=x|y|z|height: free horizontal distance to world geometry in 16 directions every 50 cm up from x,y,z
	// (shaft / corridor measurements)
	FString Probe;
	if (FParse::Value(FCommandLine::Get(), TEXT("NavProbe="), Probe))
	{
		TArray<FString> F;
		Probe.ParseIntoArray(F, TEXT("|"));
		if (F.Num() >= 4)
		{
			const FVector P0(FCString::Atod(*F[0]), FCString::Atod(*F[1]), FCString::Atod(*F[2]));
			FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaNavProbe), true);
			for (float Up = 0.f; Up <= FCString::Atof(*F[3]); Up += 50.f)
			{
				FString Line;
				const FVector C = P0 + FVector(0, 0, Up);
				for (int32 k = 0; k < 16; ++k)
				{
					const FVector Dir = FRotator(0.f, 22.5f * k, 0.f).Vector();
					FHitResult Hit;
					const bool bHit = World->LineTraceSingleByChannel(Hit, C, C + Dir * 1000.f, ECC_Pawn, Q);
					Line += FString::Printf(TEXT(" %4.0f"), bHit ? Hit.Distance : 1000.f);
				}
				UE_LOG(LogOpenUT2k4, Log, TEXT("NavProbe: z=%.0f%s"), C.Z, *Line);
			}
		}
	}
#endif
}

int32 AArenaNavGraph::FindNearestNode(const FVector& Location, bool bRequireVisible) const
{
	if (Nodes.Num() == 0)
	{
		return INDEX_NONE;
	}

	// Distance metric: vertical differences count triple so we prefer the same floor
	auto Score = [&Location](const FVector& L)
	{
		const FVector D = L - Location;
		return D.X * D.X + D.Y * D.Y + 9.f * D.Z * D.Z;
	};

	TArray<TPair<float, int32>, TInlineAllocator<64>> Candidates;
	const FIntVector C = CellOf(Location);
	for (int32 dx = -2; dx <= 2; ++dx)
	for (int32 dy = -2; dy <= 2; ++dy)
	for (int32 dz = -1; dz <= 1; ++dz)
	{
		if (const TArray<int32>* Cell = Cells.Find(C + FIntVector(dx, dy, dz)))
		{
			for (const int32 Idx : *Cell)
			{
				Candidates.Emplace(Score(Nodes[Idx].Location), Idx);
			}
		}
	}

	if (Candidates.Num() == 0)
	{
		int32 Best = INDEX_NONE;
		float BestScore = TNumericLimits<float>::Max();
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			const float S = Score(Nodes[i].Location);
			if (S < BestScore)
			{
				BestScore = S;
				Best = i;
			}
		}
		return Best;
	}

	Candidates.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
	if (bRequireVisible)
	{
		const int32 MaxTests = FMath::Min(Candidates.Num(), 8);
		for (int32 i = 0; i < MaxTests; ++i)
		{
			if (IsClearLine(Location, Nodes[Candidates[i].Value].Location))
			{
				return Candidates[i].Value;
			}
		}
	}
	return Candidates[0].Value;
}

namespace ArenaNavCacheLocal
{
	constexpr uint32 Magic = 0x4E415643; // "NAVC"
}

void AArenaNavGraph::SaveCache(const FString& File, const TArray<FString>& DoorNames) const
{
	TArray<uint8> Data;
	FMemoryWriter Ar(Data);
	uint32 Magic = ArenaNavCacheLocal::Magic;
	int32 Version = ArenaNavBuild::NavCacheVersion;
	Ar << Magic << Version;
	TArray<FString> Doors = DoorNames;
	Ar << Doors;
	int32 NumNodes = Nodes.Num();
	Ar << NumNodes;
	for (const FArenaNavNode& N : Nodes)
	{
		FVector L = N.Location;
		FString LiftName = N.Lift.IsValid() ? N.Lift->UE2Name : FString();
		int32 NumLinks = N.Links.Num();
		Ar << L << LiftName << NumLinks;
		for (const FArenaNavLink& K : N.Links)
		{
			int32 To = K.To;
			uint8 Type = uint8(K.Type);
			float Cost = K.Cost;
			int32 Door = K.Door;
			Ar << To << Type << Cost << Door;
		}
	}
	TArray<FVector> Deferred = DeferredPoints;
	Ar << Deferred;
	int32 NumTrans = TranslocLinks.Num();
	Ar << NumTrans;
	for (const FTranslocLink& T : TranslocLinks)
	{
		FTranslocLink C = T;
		Ar << C.From << C.To << C.FromLoc << C.ToLoc << C.Target << C.Radius << C.Height << C.AimZ << C.bOnlyTranslocator;
	}
	if (FFileHelper::SaveArrayToFile(Data, *File))
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: cached to %s (%d KB)"), *FPaths::GetCleanFilename(File), Data.Num() / 1024);
	}
}

bool AArenaNavGraph::LoadCache(const FString& File, const TArray<FString>& DoorNames)
{
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *File, FILEREAD_Silent))
	{
		return false;
	}
	FMemoryReader Ar(Data);
	uint32 Magic = 0;
	int32 Version = 0;
	Ar << Magic << Version;
	TArray<FString> Doors;
	Ar << Doors;
	if (Magic != ArenaNavCacheLocal::Magic || Version != ArenaNavBuild::NavCacheVersion || Doors != DoorNames || Ar.IsError())
	{
		return false;
	}
	TMap<FString, AArenaMover*> Movers;
	for (TActorIterator<AArenaMover> It(GetWorld()); It; ++It)
	{
		Movers.Add(It->UE2Name, *It);
	}
	int32 NumNodes = 0;
	Ar << NumNodes;
	if (NumNodes < 0 || NumNodes > 10000000)
	{
		return false;
	}
	TArray<FArenaNavNode> Loaded;
	Loaded.SetNum(NumNodes);
	for (FArenaNavNode& N : Loaded)
	{
		FString LiftName;
		int32 NumLinks = 0;
		Ar << N.Location << LiftName << NumLinks;
		if (Ar.IsError() || NumLinks < 0 || NumLinks > 100000)
		{
			return false;
		}
		if (!LiftName.IsEmpty())
		{
			N.Lift = Movers.FindRef(LiftName);
		}
		N.Links.SetNum(NumLinks);
		for (FArenaNavLink& K : N.Links)
		{
			uint8 Type = 0;
			Ar << K.To << Type << K.Cost << K.Door;
			K.Type = EArenaNavLinkType(Type);
			if (K.To < 0 || K.To >= NumNodes)
			{
				return false;
			}
		}
	}
	TArray<FVector> Deferred;
	Ar << Deferred;
	int32 NumTrans = 0;
	Ar << NumTrans;
	TArray<FTranslocLink> Trans;
	Trans.SetNum(FMath::Max(0, NumTrans));
	for (FTranslocLink& C : Trans)
	{
		Ar << C.From << C.To << C.FromLoc << C.ToLoc << C.Target << C.Radius << C.Height << C.AimZ << C.bOnlyTranslocator;
	}
	if (Ar.IsError())
	{
		return false;
	}
	Nodes = MoveTemp(Loaded);
	DeferredPoints = MoveTemp(Deferred);
	TranslocLinks = MoveTemp(Trans);
	Cells.Reset();
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		Cells.FindOrAdd(CellOf(Nodes[i].Location)).Add(i);
	}
	return true;
}

void AArenaNavGraph::RefreshAround(const FBox& Region, const UPrimitiveComponent* MoverFloor)
{
	using namespace ArenaNavBuild;
	UWorld* World = GetWorld();
	if (!World || Nodes.Num() == 0 || (DeferredPoints.Num() == 0 && !MoverFloor))
	{
		return;
	}
	FContext Ctx(World);
	Ctx.Params = BuildParams;
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		Ctx.Params.AddIgnoredActor(*It);
	}
	const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(World);
	auto Link = [this](int32 From, int32 To, EArenaNavLinkType Type)
	{
		for (const FArenaNavLink& L : Nodes[From].Links)
		{
			if (L.To == To)
			{
				return;
			}
		}
		FArenaNavLink& L = Nodes[From].Links.AddDefaulted_GetRef();
		L.To = To;
		L.Type = Type;
		L.Cost = LinkCost(Type, Nodes[From].Location, Nodes[To].Location);
	};
	auto Connect = [&](int32 From, int32 To, ESim R, float Gap)
	{
		const FVector& A = Nodes[From].Location;
		const FVector& B = Nodes[To].Location;
		if (R == ESim::Walk)
		{
			Link(From, To, EArenaNavLinkType::Walk);
		}
		else if (R == ESim::Drop)
		{
			Link(From, To, EArenaNavLinkType::Drop);
		}
		else if (B.Z - A.Z >= -MaxStep && Ctx.CanJump(A, B) && (R != ESim::Gap || Gap < MaxGapJump))
		{
			Link(From, To, EArenaNavLinkType::Jump);
		}
	};
	int32 Added = 0, NewLinks = 0;
	// candidates: the map's navigation points without a floor so far, and a grid over the mover's own top (a spawn trailer
	// at the end of its run, AS-Convoy)
	TArray<FVector> Candidates;
	for (const FVector& Pt : DeferredPoints)
	{
		if (Region.IsInsideOrOn(Pt))
		{
			Candidates.Add(Pt);
		}
	}
	const int32 NumDeferredCands = Candidates.Num();
	if (MoverFloor)
	{
		const FBox MB = MoverFloor->Bounds.GetBox();
		for (float X = MB.Min.X + 60.f; X <= MB.Max.X - 60.f; X += GridSpacing * 0.8f)
		{
			for (float Y = MB.Min.Y + 60.f; Y <= MB.Max.Y - 60.f; Y += GridSpacing * 0.8f)
			{
				FHitResult Hit;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaNavMoverTop), true);
				if (const_cast<UPrimitiveComponent*>(MoverFloor)->LineTraceComponent(Hit, FVector(X, Y, MB.Max.Z + 50.f), FVector(X, Y, MB.Min.Z - 10.f), Q) && Hit.ImpactNormal.Z >= WalkableZ)
				{
					bool bNear = false;
					for (const int32 Idx : Cells.FindRef(CellOf(Hit.ImpactPoint + FVector(0.f, 0.f, NodeHeight))))
					{
						bNear |= FVector::DistSquared(Nodes[Idx].Location, Hit.ImpactPoint + FVector(0.f, 0.f, NodeHeight)) < FMath::Square(120.f);
					}
					if (!bNear)
					{
						Candidates.Add(Hit.ImpactPoint + FVector(0.f, 0.f, NodeHeight));
					}
				}
			}
		}
	}
	for (int32 c = 0; c < Candidates.Num(); ++c)
	{
		const FVector Pt = Candidates[c];
		float FloorZ = 0.f;
		if (!Ctx.FindFloor(Pt, 60.f, 400.f, FloorZ) || !Ctx.SweepClear(FVector(Pt.X, Pt.Y, FloorZ + SweepLift), FVector(Pt.X, Pt.Y, FloorZ + SweepLift)))
		{
			continue;
		}
		if (Zones && Zones->HasVolumes() && Zones->IsHazardAt(FVector(Pt.X, Pt.Y, FloorZ + 20.f)))
		{
			continue;
		}
		if (c < NumDeferredCands)
		{
			DeferredPoints.RemoveSingleSwap(Pt);
		}
		FArenaNavNode& N = Nodes.AddDefaulted_GetRef();
		N.Location = FVector(Pt.X, Pt.Y, FloorZ + NodeHeight);
		const int32 NewIdx = Nodes.Num() - 1;
		Cells.FindOrAdd(CellOf(N.Location)).Add(NewIdx);
		++Added;
		// link with every node around it (the other new ones included), both ways
		const FVector A = Nodes[NewIdx].Location;
		for (int32 j = 0; j < NewIdx; ++j)
		{
			const FVector B = Nodes[j].Location;
			const float D2 = FVector::DistSquared2D(A, B);
			if (D2 > FMath::Square(UE2PointLinkRadius) || FMath::Abs(A.Z - B.Z) > MaxDrop || D2 < 1.f)
			{
				continue;
			}
			const int32 Before = Nodes[NewIdx].Links.Num() + Nodes[j].Links.Num();
			float GapAB = 0.f, GapBA = 0.f;
			Connect(NewIdx, j, Ctx.Simulate(A, B, GapAB), GapAB);
			Connect(j, NewIdx, Ctx.Simulate(B, A, GapBA), GapBA);
			NewLinks += Nodes[NewIdx].Links.Num() + Nodes[j].Links.Num() - Before;
		}
	}
	if (Added > 0)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavGraph: %d node(s) and %d link(s) added around a mover that came to rest at %s"), Added, NewLinks, *Region.GetCenter().ToCompactString());
	}
}

int32 AArenaNavGraph::RunPadPathTest() const
{
	TArray<TArray<int32>> Reverse;
	Reverse.SetNum(Nodes.Num());
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		for (const FArenaNavLink& L : Nodes[i].Links)
		{
			Reverse[L.To].Add(i);
		}
	}
	int32 Pads = 0, Through = 0;
	for (int32 PadNode = 0; PadNode < Nodes.Num(); ++PadNode)
	{
		const FArenaNavLink* PadLink = Nodes[PadNode].Links.FindByPredicate([](const FArenaNavLink& L) { return L.Type == EArenaNavLinkType::JumpPad; });
		if (!PadLink)
		{
			continue;
		}
		++Pads;
		// walk back from the pad up to 6 links (breadth first) and start from the farthest node found
		TMap<int32, int32> Depth;
		TArray<int32> Queue = { PadNode };
		Depth.Add(PadNode, 0);
		int32 Start = INDEX_NONE;
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			const int32 N = Queue[Head];
			const int32 D = Depth[N];
			if (D >= 2 && (Start == INDEX_NONE || FVector::DistSquared(Nodes[N].Location, Nodes[PadNode].Location) > FVector::DistSquared(Nodes[Start].Location, Nodes[PadNode].Location)))
			{
				Start = N;
			}
			if (D >= 6)
			{
				continue;
			}
			for (const int32 Prev : Reverse[N])
			{
				if (!Depth.Contains(Prev))
				{
					Depth.Add(Prev, D + 1);
					Queue.Add(Prev);
				}
			}
		}
		const FVector Landing = Nodes[PadLink->To].Location;
		TArray<FVector> Path;
		TArray<EArenaNavLinkType> Links;
		const bool bPath = Start != INDEX_NONE && FindPath(Nodes[Start].Location, Landing, Path, Links);
		const bool bUsesPad = bPath && Links.Contains(EArenaNavLinkType::JumpPad);
		Through += bUsesPad ? 1 : 0;
		UE_LOG(LogOpenUT2k4, Log, TEXT("NavPadTest: pad at %s -> %s: %s"), *Nodes[PadNode].Location.ToCompactString(), *Landing.ToCompactString(),
			Start == INDEX_NONE ? TEXT("no node leads onto the pad") : bUsesPad ? *FString::Printf(TEXT("path from %s (%d points) goes through the pad"), *Nodes[Start].Location.ToCompactString(), Path.Num())
			: bPath ? TEXT("path found, but not through the pad") : TEXT("no path"));
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("NavPadTest: %d of %d pad links are on the planned path"), Through, Pads);
	return Through;
}

int32 AArenaNavGraph::RunCompareTest(const FString& File) const
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *File))
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("NavCompare: can't read %s"), *File);
		return -1;
	}
	int32 Num = 0, Failed = 0, Detour = 0;
	for (const FString& Line : Lines)
	{
		TArray<FString> F;
		Line.ParseIntoArrayWS(F);
		if (F.Num() < 9)
		{
			continue;
		}
		const FVector A(FCString::Atod(*F[0]), FCString::Atod(*F[1]), FCString::Atod(*F[2]));
		const FVector B(FCString::Atod(*F[3]), FCString::Atod(*F[4]), FCString::Atod(*F[5]));
		++Num;
		TArray<FVector> Path;
		TArray<EArenaNavLinkType> Links;
		if (!FindPath(A, B, Path, Links))
		{
			++Failed;
			UE_LOG(LogOpenUT2k4, Log, TEXT("NavCompare: NO PATH %s -> %s (flags %s) %s -> %s"), *F[7], *F[8], *F[6], *A.ToCompactString(), *B.ToCompactString());
			continue;
		}
		float Len = 0.f;
		FVector Prev = A;
		for (const FVector& P : Path)
		{
			Len += FVector::Dist(Prev, P);
			Prev = P;
		}
		const float Direct = FVector::Dist(A, B);
		if (Len > Direct * 2.5f + 2500.f)
		{
			++Detour;
			UE_LOG(LogOpenUT2k4, Log, TEXT("NavCompare: DETOUR %s -> %s (flags %s) %s -> %s: %.0f vs %.0f"), *F[7], *F[8], *F[6], *A.ToCompactString(), *B.ToCompactString(), Len, Direct);
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("NavCompare: %d specs, %d without a path, %d long detours"), Num, Failed, Detour);
	return Failed;
}

bool AArenaNavGraph::FindPath(const FVector& Start, const FVector& End, TArray<FVector>& OutPath, TArray<EArenaNavLinkType>& OutLinkTypes,
	bool bCanTranslocate, bool bDoorsOpen) const
{
	using namespace ArenaNavBuild;

	OutPath.Reset();
	OutLinkTypes.Reset();

	const int32 S = FindNearestNode(Start, true);
	const int32 E = FindNearestNode(End, true);
	if (S == INDEX_NONE || E == INDEX_NONE)
	{
		return false;
	}

	if (S == E)
	{
		OutPath.Add(End);
		OutLinkTypes.Add(EArenaNavLinkType::Walk);
		return true;
	}

	const int32 N = Nodes.Num();
	TArray<float> G;
	G.Init(TNumericLimits<float>::Max(), N);
	TArray<int32> Parent;
	Parent.Init(INDEX_NONE, N);
	TArray<EArenaNavLinkType> ParentType;
	ParentType.Init(EArenaNavLinkType::Walk, N);
	TArray<bool> Closed;
	Closed.Init(false, N);

	struct FOpenEntry
	{
		float F;
		int32 Node;
	};
	auto HeapPred = [](const FOpenEntry& A, const FOpenEntry& B) { return A.F < B.F; };

	const FVector Goal = Nodes[E].Location;
	TArray<FOpenEntry> Open;
	Open.Reserve(256);
	G[S] = 0.f;
	Open.HeapPush({ FVector::Dist(Nodes[S].Location, Goal) * HeuristicScale, S }, HeapPred);

	bool bFound = false;
	while (Open.Num() > 0)
	{
		FOpenEntry Cur;
		Open.HeapPop(Cur, HeapPred, EAllowShrinking::No);
		if (Closed[Cur.Node])
		{
			continue;
		}
		if (Cur.Node == E)
		{
			bFound = true;
			break;
		}
		Closed[Cur.Node] = true;

		for (const FArenaNavLink& Link : Nodes[Cur.Node].Links)
		{
			if (Closed[Link.To] || (Link.Type == EArenaNavLinkType::Translocate && !bCanTranslocate))
			{
				continue;
			}
			if (!bDoorsOpen && Link.Door != INDEX_NONE && ScriptedDoors.IsValidIndex(Link.Door))
			{
				const AArenaMover* DoorMover = ScriptedDoors[Link.Door].Get();
				if (DoorMover && DoorMover->IsClosed())
				{
					continue; // UE2 BlockedPath: shut until the script opens it
				}
			}
			const float NewG = G[Cur.Node] + Link.Cost;
			if (NewG < G[Link.To])
			{
				G[Link.To] = NewG;
				Parent[Link.To] = Cur.Node;
				ParentType[Link.To] = Link.Type;
				Open.HeapPush({ NewG + FVector::Dist(Nodes[Link.To].Location, Goal) * HeuristicScale, Link.To }, HeapPred);
			}
		}
	}

	if (!bFound)
	{
		return false;
	}

	TArray<int32> NodePath;
	for (int32 Cur = E; Cur != INDEX_NONE; Cur = Parent[Cur])
	{
		NodePath.Add(Cur);
		if (Cur == S)
		{
			break;
		}
	}
	Algo::Reverse(NodePath);

	for (int32 i = 0; i < NodePath.Num(); ++i)
	{
		OutPath.Add(Nodes[NodePath[i]].Location);
		OutLinkTypes.Add(i == 0 ? EArenaNavLinkType::Walk : ParentType[NodePath[i]]);
	}

	// Skip the first node if we can head straight for the second one
	if (OutPath.Num() >= 2 && OutLinkTypes[1] == EArenaNavLinkType::Walk &&
		FVector::Dist2D(Start, OutPath[1]) < FVector::Dist2D(OutPath[0], OutPath[1]) + 50.f &&
		FMath::Abs(Start.Z - OutPath[0].Z) < 120.f &&
		IsClearLine(Start, OutPath[1]))
	{
		OutPath.RemoveAt(0);
		OutLinkTypes.RemoveAt(0);
	}

	if (FVector::Dist(OutPath.Last(), End) > 50.f)
	{
		OutPath.Add(End);
		OutLinkTypes.Add(EArenaNavLinkType::Walk);
	}
	return true;
}

FVector AArenaNavGraph::GetRandomNodeLocation(FRandomStream& Rng) const
{
	if (Nodes.Num() == 0)
	{
		return GetActorLocation();
	}
	return Nodes[Rng.RandHelper(Nodes.Num())].Location;
}

void AArenaNavGraph::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (CVarArenaDebugNav.GetValueOnGameThread() > 0)
	{
		DebugDraw(PrimaryActorTick.TickInterval + 0.05f);
	}
}

void AArenaNavGraph::DebugDraw(float Duration) const
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FVector ViewLoc = FVector::ZeroVector;
	bool bLimitRange = CVarArenaDebugNav.GetValueOnGameThread() < 2;
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		FRotator ViewRot;
		PC->GetPlayerViewPoint(ViewLoc, ViewRot);
	}
	else
	{
		bLimitRange = false;
	}
	const float RangeSq = FMath::Square(4000.f);

	for (const FArenaNavNode& Node : Nodes)
	{
		if (bLimitRange && FVector::DistSquared(Node.Location, ViewLoc) > RangeSq)
		{
			continue;
		}
		DrawDebugPoint(World, Node.Location, 8.f, FColor(80, 200, 255), false, Duration);
		for (const FArenaNavLink& Link : Node.Links)
		{
			if (!Nodes.IsValidIndex(Link.To))
			{
				continue;
			}
			const FVector& To = Nodes[Link.To].Location;
			switch (Link.Type)
			{
			case EArenaNavLinkType::Walk:
				DrawDebugLine(World, Node.Location, To, FColor(40, 220, 40), false, Duration, 0, 1.5f);
				break;
			case EArenaNavLinkType::Jump:
				DrawDebugDirectionalArrow(World, Node.Location, To, 30.f, FColor(255, 230, 0), false, Duration, 0, 2.f);
				break;
			case EArenaNavLinkType::Drop:
				DrawDebugDirectionalArrow(World, Node.Location, To, 30.f, FColor(255, 120, 0), false, Duration, 0, 2.f);
				break;
			case EArenaNavLinkType::JumpPad:
				DrawDebugDirectionalArrow(World, Node.Location, To, 60.f, FColor(255, 0, 255), false, Duration, 0, 3.f);
				break;
			case EArenaNavLinkType::Lift:
				DrawDebugDirectionalArrow(World, Node.Location, To, 60.f, FColor(0, 200, 255), false, Duration, 0, 3.f);
				break;
			case EArenaNavLinkType::Teleport:
				DrawDebugDirectionalArrow(World, Node.Location, To, 60.f, FColor(255, 255, 255), false, Duration, 0, 3.f);
				break;
			case EArenaNavLinkType::ForcedJump:
				DrawDebugDirectionalArrow(World, Node.Location, To, 60.f, FColor(255, 160, 160), false, Duration, 0, 3.f);
				break;
			case EArenaNavLinkType::Translocate:
				DrawDebugDirectionalArrow(World, Node.Location, To, 60.f, FColor(80, 140, 255), false, Duration, 0, 3.f);
				break;
			}
		}
	}
#endif
}
