#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Actor.h"
#include "ArenaTypes.h"
#include "ArenaONS_PowerNode.generated.h"

class USceneComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UCapsuleComponent;
class UPointLightComponent;
class UArenaRules_ONS;

UENUM()
enum class EArenaONS_NodeState : uint8
{
	Neutral,
	Constructing,
	Active
};

/**
 * Onslaught power node or power core ("conduit" / "heart" in the HUD text). A pawn subclass only so every
 * weapon's direct hits count as damage (projectiles only damage pawns on direct impact); it is never possessed.
 *
 * Nodes start neutral; a player whose team owns a linked node or core touches the pad to start construction
 * (BuildTime seconds, faster with the Arc Linker beam). Active and constructing nodes take damage only from a
 * team that owns a linked node (the rules keep bAttackable[] up to date). Cores take damage when an adjacent node
 * is owned by the attacker.
 */
UCLASS()
class OPENUT2K4_API AArenaONS_PowerNode : public APawn
{
	GENERATED_BODY()

public:
	AArenaONS_PowerNode();

	/** Setup before / right after spawning. */
	void InitNode(UArenaRules_ONS* InRules, int32 InIndex, bool bInCore, int32 InCoreTeam, const FString& InName);

	bool IsCore() const { return bIsCore; }
	int32 GetNodeIndex() const { return NodeIndex; }
	const FString& GetNodeName() const { return NodeName; }
	int32 GetTeam() const { return Team; }
	EArenaONS_NodeState GetState() const { return State; }
	bool IsActive() const { return State == EArenaONS_NodeState::Active; }
	bool IsNeutral() const { return State == EArenaONS_NodeState::Neutral; }
	bool IsConstructing() const { return State == EArenaONS_NodeState::Constructing; }
	/** Owned (active) by Team. */
	bool IsOwnedBy(int32 InTeam) const { return State == EArenaONS_NodeState::Active && Team == InTeam; }
	float GetHealth() const { return Health; }
	float GetMaxHealth() const { return MaxHealth; }
	float GetHealthFraction() const { return MaxHealth > 0.f ? FMath::Clamp(Health / MaxHealth, 0.f, 1.f) : 0.f; }
	float GetBuildProgress() const { return BuildProgress; }
	bool IsAttackableBy(int32 InTeam) const { return (InTeam == 0 || InTeam == 1) && (AttackableMask & (1 << InTeam)) != 0; }
	bool IsBuildableBy(int32 InTeam) const { return (InTeam == 0 || InTeam == 1) && (BuildableMask & (1 << InTeam)) != 0; }
	void SetNetworkFlags(bool bAttackableByRed, bool bAttackableByBlue, bool bBuildableByRed, bool bBuildableByBlue);
	/** Last time damage was taken (HUD flashing). */
	float GetLastDamageTime() const { return LastDamageTime; }
	/** Floor location of the node (actor location). */
	FVector GetFloorLocation() const { return GetActorLocation(); }
	/** Point bots and turrets aim at. */
	FVector GetAimPoint() const;
	/** Radius of the touch pad / solid part. */
	float GetPadRadius() const { return bIsCore ? 380.f : 230.f; }
	float GetSolidRadius() const { return bIsCore ? 190.f : 85.f; }

	const TArray<TObjectPtr<AArenaONS_PowerNode>>& GetLinks() const { return Links; }
	void AddLink(AArenaONS_PowerNode* Other);
	void RemoveLink(AArenaONS_PowerNode* Other) { Links.Remove(Other); }
	void ClearLinks() { Links.Reset(); }
	bool IsLinkedTo(const AArenaONS_PowerNode* Other) const { return Links.Contains(Other); }

	/** Server: start construction for a team (touch). */
	bool StartConstruction(int32 InTeam, AController* By);
	/** Server: reset to the round start state (neutral node / full core of CoreTeam). */
	void ResetNode(int32 InCoreTeam);
	/** Server, test harness only (-ONSTestCore): make the node active for a team at once. */
	void ForceOwner(int32 InTeam);
	/** Server: heal or speed up construction (friendly Arc Linker beam). */
	void ReceiveLink(float Amount, AController* By);

	/**
	 * Watching a UT2004 server (Net/UE2NetWorld.h): the server's ONSPowerCore state. No local construction, touch or
	 * damage from then on; construction finished / node destroyed effects play on the state changes. bDamaged: the
	 * server's health went down (HUD flashing).
	 */
	void SetNetState(EArenaONS_NodeState NewState, int32 NewTeam, float NewHealth, float NewMaxHealth, bool bDamaged);
	bool IsNetDriven() const { return bNetDriven; }
	/** The UT2004 map actor this node was made from ("ONSPowerNodeNeutral12"; empty for generated nodes). */
	FString UE2SourceName;

	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Spawn spots around the node (filled by the rules, server). */
	TArray<FTransform> SpawnSpots;
	/** Ring points for bots: inner (on the pad, for building) and outer (attack / defend standoff). */
	TArray<FVector> InnerPoints;
	TArray<FVector> OuterPoints;

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCapsuleComponent> Solid;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> CrystalRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> HealthBarRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;

protected:
	virtual void BeginPlay() override;

	void BuildModel();
	UStaticMeshComponent* AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& Loc, const FRotator& Rot, const FVector& SizeCm, UMaterialInterface* Material, bool bShadow = true);
	void UpdateLook();
	void UpdateCollision();
	void SetState(EArenaONS_NodeState NewState, int32 NewTeam);
	void TickTouch();

	UFUNCTION() void OnRep_Look();

	UPROPERTY(ReplicatedUsing = OnRep_Look) int32 Team = ArenaTeam::None;
	UPROPERTY(ReplicatedUsing = OnRep_Look) EArenaONS_NodeState State = EArenaONS_NodeState::Neutral;
	UPROPERTY(Replicated) float Health = 0.f;
	UPROPERTY(Replicated) float MaxHealth = 1000.f;
	UPROPERTY(Replicated) float BuildProgress = 0.f;
	UPROPERTY(ReplicatedUsing = OnRep_Look) uint8 AttackableMask = 0;
	UPROPERTY(Replicated) uint8 BuildableMask = 0;
	UPROPERTY(Replicated) bool bIsCore = false;
	UPROPERTY(Replicated) int32 NodeIndex = 0;
	UPROPERTY(Replicated) FString NodeName;
	UPROPERTY(Replicated) TArray<TObjectPtr<AArenaONS_PowerNode>> Links;
	UPROPERTY(Replicated) float LastDamageTime = -100.f;

	TWeakObjectPtr<UArenaRules_ONS> Rules;
	/** Driven by a UT2004 server's state (SetNetState). */
	bool bNetDriven = false;
	TWeakObjectPtr<AController> Builder;
	float BuildRateBonusUntil = 0.f;
	float BuildRateBonus = 0.f;
	float NextTouchCheck = 0.f;
	float NextCoreDamageLog = 0.f;
	float AccumulatedLogDamage = 0.f;
	TWeakObjectPtr<AController> LastAttacker;

	// ---- visuals ----
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> TeamGlow;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> ActiveOnly;
	UPROPERTY() TArray<TObjectPtr<USceneComponent>> Pylons;
	UPROPERTY() TArray<TObjectPtr<USceneComponent>> Rings;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> ShieldPart;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> HealthFill;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> HealthBack;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> CrystalPart;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> HealthFillMat;
	bool bModelBuilt = false;
	float VisualTime = 0.f;

	// ---- imported art (ArenaArtItems::GetOnslaughtArt); procedural parts are skipped when it is built ----
	/** Builds the imported node / core model. False = keep the procedural one. */
	bool BuildArtModel();
	void UpdateArtLook();
	bool bArtModel = false;
	UPROPERTY() TObjectPtr<class USkeletalMeshComponent> ArtFrame;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> ArtEnergy;
	UPROPERTY() TArray<TObjectPtr<class UArenaSpriteComponent>> ArtFlares;
	FVector ArtFrameScale = FVector::OneVector;
	FVector ArtFrameLocation = FVector::ZeroVector;
	/** Frame bounds bottom below its origin (cm, at full scale). */
	float ArtFrameBottom = 0.f;
};

/** Glowing beam between two linked nodes (team coloured when both ends belong to one team). */
UCLASS()
class OPENUT2K4_API AArenaONS_PowerLink : public AActor
{
	GENERATED_BODY()

public:
	AArenaONS_PowerLink();
	void InitLink(AArenaONS_PowerNode* InA, AArenaONS_PowerNode* InB);
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	AArenaONS_PowerNode* GetA() const { return A; }
	AArenaONS_PowerNode* GetB() const { return B; }

protected:
	UPROPERTY(Replicated) TObjectPtr<AArenaONS_PowerNode> A;
	UPROPERTY(Replicated) TObjectPtr<AArenaONS_PowerNode> B;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Beam;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Core;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> BeamMat;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> CoreMat;
	float Time = 0.f;
	int32 LastColorKey = -100;
	/** Imported-art beam (textured quads turned to the viewer) instead of the glow cylinders. */
	UPROPERTY() TArray<TObjectPtr<class UArenaSpriteComponent>> ArtBeams;
	bool bArtTried = false;
};
