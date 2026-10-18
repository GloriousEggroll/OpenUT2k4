#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaAS_Objective.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPointLightComponent;
class AArenaCharacter;

/** The four assault objective kinds. */
UENUM()
enum class EArenaASObjectiveType : uint8
{
	Touch,    // an attacker reaches the spot
	Hold,     // attackers stay in the zone for Duration seconds while no defender is inside
	Destroy,  // attackers deal MaxHealth damage to it
	Use       // an attacker holds the Use key next to it for Duration seconds
};

/**
 * One Assault objective (spawned by UArenaRules_AS). Replicates its state for the HUD; the server logic
 * runs in ServerTick, called by the rules every tick while the round is playing.
 */
UCLASS()
class OPENUT2K4_API AArenaAS_Objective : public AActor
{
	GENERATED_BODY()

public:
	AArenaAS_Objective();

	/** Server: configure once right after spawning (builds the model). */
	void Setup(EArenaASObjectiveType InType, int32 InIndex, int32 InStage, bool bInOptional, const FString& InDescription,
		float InRadius, float InDuration, float InHealth);
	/**
	 * Server, before Setup: the objective comes from a UT2004 map. UT2004's objectives are the map's own things
	 * (a DestroyableObjective_SM is the mesh itself; hold / use / reach objectives are invisible spots at the map's
	 * consoles, doors and trailers), shown with the HUD markers. MeshObjectPath (UT2004 "Pkg.Group.Name", may be
	 * empty) and its world transform are the objective's own static mesh; DestroyedMeshObjectPath replaces it when
	 * destroyed (empty: it disappears). No procedural model is built unless bInStandIn (the map's thing is not
	 * recreated, e.g. the vehicle of a DestroyVehicleObjective).
	 */
	void SetMapSource(const FString& MeshObjectPath, const FTransform& MeshTransform, const FString& DestroyedMeshObjectPath, bool bInStandIn = false);
	/**
	 * Server, before Setup, for a UT2004 DestroyVehicleObjective: the vehicle it is about (the Assault vehicle factory
	 * whose VehicleEvent is the objective's Tag, e.g. AS-MotherShip's sentinels), shown with the vehicle class's own
	 * skeletal mesh ("SkeletalMeshes/Pkg/Name") at the factory's transform instead of a stand-in; the damage box is fitted
	 * to it and it is gone (with an explosion) once destroyed.
	 */
	void SetMapVehicle(const FString& SkeletalMeshPath, const FTransform& WorldTransform) { MapVehicleMesh = SkeletalMeshPath; MapVehicleTransform = WorldTransform; }
	/** Server, after Setup: its vehicle is a real one (AArenaAS_Turret) that takes the damage: no model or damage box of its own. */
	void SetLinkedVehicle(AActor* InVehicle);
	AActor* GetLinkedVehicle() const { return LinkedVehicle.Get(); }
	bool HasLinkedVehicle() const { return bVehicleLinked; }
	/** UT2004 "Pkg.Group.Name" -> our "StaticMeshes/Pkg/Group_Name" relative path. */
	static FString MeshObjectPathToRelative(const FString& ObjectPath);
	/** Server: renumber after unplayable objectives were dropped. */
	void SetStageAndIndex(int32 InStage, int32 InIndex) { Stage = InStage; Index = InIndex; }
	/**
	 * UE2 GameObjective Tag / Event of a map objective (UArenaRules_AS): Event is fired through the UE2 event system
	 * when the objective is completed (doors, movers, scripted sequences react), and objectives completed by events
	 * (TriggeredObjective: bCompletedByEvent) complete when an event named Tag fires.
	 */
	FName UE2Tag;
	FName UE2Event;
	bool bCompletedByEvent = false;
	/** Server: completion through the UE2 event system (reported by the next ServerTick). */
	void CompleteByEvent()
	{
		if (IsActive())
		{
			bCompleted = true;
			Progress = 1.f;
			Health = 0.f;
			bPendingCompletion = true;
		}
	}
	/** Server: count as done without play (testing: ?StartStage=). */
	void ForceCompleted() { bCompleted = true; Progress = 1.f; Health = 0.f; bActive = false; }
	/** Server: clear progress / health for a new round. */
	void ResetObjective();
	/** Server: which team attacks / defends this round. */
	void SetRoles(int32 InAttackingTeam, int32 InDefendingTeam);
	/** Server: whether the objective can be worked on now. */
	void SetActive(bool bInActive);
	/**
	 * Watching a UT2004 server (Net/UE2NetWorld.h): its GameObjective state. Active = bActive and not bDisabled;
	 * Health / MaxHealth for destroyable objectives (MaxHealth <= 0 keeps ours); Progress 0..1 (SavedObjectiveProgress);
	 * bInWorked: under attack / critical.
	 */
	void SetNetState(bool bInActive, bool bInCompleted, float InHealth, float InMaxHealth, float InProgress, bool bInWorked, int32 InDefendingTeam);
	/** The UT2004 map actor this objective was made from ("HoldObjective3"; empty for generated ones). */
	FString UE2SourceName;
	/**
	 * Server: advance hold / use progress. Returns true exactly once, on the tick the objective completes
	 * (also for completion through damage).
	 */
	bool ServerTick(float DeltaSeconds, float Now);
	/** Server: Use key pressed (or held: bots call it every tick). True if this objective accepted it. */
	bool NotifyUse(AController* User, float Now);
	/** Server: damage from an attacker (weapon damage arrives through TakeDamage). */
	void ApplyAttackDamage(float Damage, AController* By);

	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- queries (replicated) ----
	EArenaASObjectiveType GetType() const { return Type; }
	int32 GetIndex() const { return Index; }
	int32 GetStage() const { return Stage; }
	bool IsOptional() const { return bOptional; }
	bool IsCompleted() const { return bCompleted; }
	bool IsActive() const { return bActive && !bCompleted; }
	const FString& GetDescription() const { return Description; }
	float GetRadius() const { return Radius; }
	float GetDuration() const { return Duration; }
	/** Hold / Use progress 0..1. */
	float GetProgress() const { return Progress; }
	float GetHealth() const { return Health; }
	float GetMaxHealth() const { return MaxHealth; }
	float GetHealthFraction() const { return MaxHealth > 0.f ? FMath::Clamp(Health / MaxHealth, 0.f, 1.f) : 0.f; }
	/** 0..1 how far the attackers got on this objective (for tie breaks / HUD). */
	float GetWorkFraction() const;
	/** Hold: defenders inside block progress. */
	bool IsContested() const { return bContested; }
	/** Someone is working on it right now (in zone / using / just hit). */
	bool IsBeingWorked() const { return bWorked; }
	int32 GetAttackingTeam() const { return AttackingTeam; }
	int32 GetDefendingTeam() const { return DefendingTeam; }
	/** World point to show on the HUD / aim at. */
	FVector GetMarkerLocation() const;
	/** Short type label for the HUD. */
	static FString TypeLabel(EArenaASObjectiveType InType);
	/** Use: is the controller close enough to use it? */
	bool IsInUseRange(const AActor* Pawn) const;
	/** Is the location inside the objective zone (2D radius + height window)? */
	bool IsInZone(const FVector& Location, float ExtraRadius = 0.f) const;
	/** Controllers that contributed to completing it (server). */
	const TArray<TWeakObjectPtr<AController>>& GetContributors() const { return Contributors; }

protected:
	virtual void BeginPlay() override;
	UFUNCTION() void OnRep_Type();

	void BuildModel();
	UStaticMeshComponent* AddPart(UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm, UMaterialInterface* Material, const FRotator& Rotation = FRotator::ZeroRotator);
	void UpdateVisuals(float DeltaSeconds);
	void AddContributor(AController* C);
	FLinearColor GetStateColor() const;

	UPROPERTY(ReplicatedUsing = OnRep_Type) EArenaASObjectiveType Type = EArenaASObjectiveType::Touch;
	UPROPERTY(Replicated) int32 Index = 0;
	UPROPERTY(Replicated) int32 Stage = 0;
	UPROPERTY(Replicated) bool bOptional = false;
	UPROPERTY(Replicated) FString Description;
	UPROPERTY(Replicated) float Radius = 200.f;
	UPROPERTY(Replicated) float Duration = 5.f;
	UPROPERTY(Replicated) float MaxHealth = 0.f;
	UPROPERTY(Replicated) float Health = 0.f;
	UPROPERTY(Replicated) float Progress = 0.f;
	UPROPERTY(Replicated) bool bCompleted = false;
	UPROPERTY(Replicated) bool bActive = false;
	UPROPERTY(Replicated) bool bContested = false;
	UPROPERTY(Replicated) bool bWorked = false;
	UPROPERTY(Replicated) int32 AttackingTeam = 0;
	UPROPERTY(Replicated) int32 DefendingTeam = 1;

	// server only
	bool bPendingCompletion = false;
	float LastHitTime = -100.f;
	TMap<TWeakObjectPtr<AController>, float> Users;
	TArray<TWeakObjectPtr<AController>> Contributors;

	// from the UT2004 map (SetMapSource)
	UPROPERTY(Replicated) bool bFromMap = false;
	UPROPERTY(Replicated) bool bMapStandIn = false;
	UPROPERTY(Replicated) FString MapMesh;
	UPROPERTY(Replicated) FString MapDestroyedMesh;
	UPROPERTY(Replicated) FTransform MapMeshTransform;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> MapMeshComp;
	UPROPERTY(Replicated) FString MapVehicleMesh;
	UPROPERTY(Replicated) FTransform MapVehicleTransform;
	UPROPERTY() TObjectPtr<class USkeletalMeshComponent> MapVehicleComp;
	bool bMapVehicleGone = false;
	TWeakObjectPtr<AActor> LinkedVehicle;
	bool bVehicleLinked = false;
	UPROPERTY() TObjectPtr<UStaticMesh> MapMeshAsset;
	UPROPERTY() TObjectPtr<UStaticMesh> MapDestroyedAsset;
	int32 MapMeshShown = -1;

	// visuals
	UPROPERTY() TObjectPtr<USceneComponent> ModelRoot;
	UPROPERTY() TObjectPtr<UBoxComponent> HitBox;
	UPROPERTY() TObjectPtr<UPointLightComponent> Light;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;
	/** Glows tinted with the state colour (team colour while active). */
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> StateGlows;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Beacon;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> ProgressPart;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> ProgressGlow;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> CorePart;
	bool bModelBuilt = false;
	int32 LastVisualState = -1;
	FVector ProgressBaseScale = FVector::OneVector;
	FVector CoreBaseScale = FVector::OneVector;
	FVector CoreBaseLocation = FVector::ZeroVector;
	float VisualTime = 0.f;
};
