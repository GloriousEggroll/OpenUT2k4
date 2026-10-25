#include "ArenaUE2Effects.h"
#include "OpenUT2k4.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "ArenaContent.h"
#include "ArenaCharacter.h"
#include "ArenaEmitters.h"
#include "ArenaRuntimeContent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaWeaponActors.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"
#include "Components/PointLightComponent.h"
#include "Convert/ArenaConvJson.h"
#include "Convert/ArenaConvPackage.h"
#include "Convert/ArenaConvExporter.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ArenaUE2EffectsLocal
{
	TAutoConsoleVariable<int32> CVarUE2Effects(TEXT("arena.UE2Effects"), 1,
		TEXT("Spawn UT2004's own effect classes (Redeemer / ion cannon blasts ...) from the cache's script packages (0 = our own effects)."));

	constexpr float S = ArenaUE2World::Scale;

	/** The cache's script packages, read with the converter's package reader (game thread only). */
	struct FReader
	{
		TUniquePtr<FConvPackageManager> Mgr;
		TUniquePtr<FConvExporter> Ex;
		FString Dir;
		bool bTried = false;
	};
	FReader& Reader()
	{
		static FReader R;
		return R;
	}

	TMap<FString, TSharedPtr<FArenaUE2ActorDesc>>& DescCache()
	{
		static TMap<FString, TSharedPtr<FArenaUE2ActorDesc>> C;
		return C;
	}

	TMap<FString, int32>& SpawnCounts()
	{
		static TMap<FString, int32> C;
		return C;
	}

	bool EnsureReader()
	{
		FReader& R = Reader();
		const FString Dir = FPaths::Combine(ArenaContent::GetCacheDir(), TEXT("scripts"));
		if (R.bTried && R.Dir == Dir)
		{
			return R.Mgr.IsValid();
		}
		R.bTried = true;
		R.Dir = Dir;
		R.Ex.Reset();
		R.Mgr.Reset();
		DescCache().Reset();
		if (ArenaContent::GetCacheDir().IsEmpty() || !FPaths::FileExists(FPaths::Combine(Dir, TEXT("System"), TEXT("XEffects.u"))))
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Effects: no script packages in %s: our own effects are used"), *Dir);
			return false;
		}
		R.Mgr = MakeUnique<FConvPackageManager>(Dir);
		FConvExporter::FOptions Opt;
		Opt.OutDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UE2EffectsUnused"));
		R.Ex = MakeUnique<FConvExporter>(*R.Mgr, Opt);
		return true;
	}

	TSharedPtr<FJsonObject> ToJsonObject(const FConvJson& J)
	{
		TArray<ANSICHAR> Text;
		J.Write(Text, -1);
		Text.Add(0); // Write does not terminate the text
		const FString Str(UTF8_TO_TCHAR(Text.GetData()));
		TSharedPtr<FJsonObject> Out;
		const TSharedRef<TJsonReader<>> JR = TJsonReaderFactory<>::Create(Str);
		if (!FJsonSerializer::Deserialize(JR, Out))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("UE2Effects: json error %s"), *JR->GetErrorMessage());
			return nullptr;
		}
		return Out;
	}

	FConvJson PropsJson(FConvExporter& Ex, const FConvProps& Props)
	{
		FConvJson O = FConvJson::Object();
		for (const TPair<FString, FConvProp>& KV : Props)
		{
			O.O.Emplace(KV.Key, Ex.Jsonify(KV.Value));
		}
		return O;
	}

	/** Lifetime of an emitter description: the longest particle (delay + emission + life), 0 when it never ends. */
	float EstimateLife(const FArenaUE2ActorDesc& D)
	{
		float Life = 0.f;
		if (D.IsA(TEXT("xEmitter")))
		{
			FArenaParticleParams P;
			P.LoadXEmitter(D.Props, D.Skins.Num() > 0 ? D.Skins[0] : FString(), D.DrawScale);
			return P.bRespawnDeadParticles ? 0.f : float(P.LifetimeRange.Y);
		}
		for (const TSharedPtr<FJsonValue>& V : D.Props.Array(TEXT("Emitters")))
		{
			FString Class;
			TArray<FString> Chain;
			FArenaUE2Props Props;
			if (!D.GetSubobject(FArenaUE2Props::ToString(V), Class, Chain, Props))
			{
				continue;
			}
			FArenaParticleParams P;
			P.Load(Class, Chain, Props, D.DrawScale);
			if (P.bDisabled)
			{
				continue;
			}
			if (P.bRespawnDeadParticles && !P.bAutoDestroy)
			{
				return 0.f;
			}
			const float Emit = P.ParticlesPerSecond > 0.f ? float(P.MaxParticles) / P.ParticlesPerSecond : 0.f;
			Life = FMath::Max(Life, float(FMath::Max(P.InitialDelayRange.X, P.InitialDelayRange.Y)) + Emit + float(FMath::Max(P.LifetimeRange.X, P.LifetimeRange.Y)));
		}
		return Life;
	}
}

bool ArenaUE2Effects::IsAvailable()
{
	using namespace ArenaUE2EffectsLocal;
	const bool bOk = CVarUE2Effects.GetValueOnGameThread() != 0 && ArenaContent::UseRuntimeContent() && EnsureReader();
	static bool bLogged = false;
	if (!bLogged)
	{
		bLogged = true;
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Effects: stock effect classes %s (cache %s)"), bOk ? TEXT("available") : TEXT("not available"), *ArenaContent::GetCacheDir());
	}
	return bOk;
}

void ArenaUE2Effects::AddPackageFile(const FString& File)
{
	using namespace ArenaUE2EffectsLocal;
	if (!IsAvailable() || File.IsEmpty())
	{
		return;
	}
	FReader& R = Reader();
	const FString Name = FPaths::GetBaseFilename(File);
	if (!R.Mgr.IsValid() || !R.Mgr->FindFile(Name).IsEmpty())
	{
		return;
	}
	R.Mgr->AddPackageFiles({ File });
	// classes of it that were looked for before
	for (auto It = DescCache().CreateIterator(); It; ++It)
	{
		if (!It.Value().IsValid() && It.Key().StartsWith(Name + TEXT("."), ESearchCase::IgnoreCase))
		{
			It.RemoveCurrent();
		}
	}
}

bool ArenaUE2Effects::GetClassDesc(const FString& PkgClass, FArenaUE2ActorDesc& Out)
{
	using namespace ArenaUE2EffectsLocal;
	if (!IsAvailable())
	{
		return false;
	}
	if (const TSharedPtr<FArenaUE2ActorDesc>* Hit = DescCache().Find(PkgClass))
	{
		if (Hit->IsValid())
		{
			Out = **Hit;
			return true;
		}
		return false;
	}
	DescCache().Add(PkgClass, nullptr);
	FString PkgName, ClassName;
	if (!PkgClass.Split(TEXT("."), &PkgName, &ClassName))
	{
		return false;
	}
	FReader& R = Reader();
	FConvPackage* Pkg = R.Mgr->LoadPackage(PkgName);
	const int32 Ei = Pkg ? Pkg->FindExport(ClassName, TEXT("Class")) : -1;
	if (Ei < 0)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("UE2Effects: class %s not found in the cache's script packages"), *PkgClass);
		return false;
	}
	FConvExporter& Ex = *R.Ex;
	const FConvProps& Defaults = R.Mgr->ClassDefaults(Pkg, Ei + 1);
	const TArray<FString> Chain = R.Mgr->ClassChain(Pkg, Ei + 1);

	FConvJson Record = FConvJson::Object();
	FConvJson ClassDefaultsJson = FConvJson::Object();
	Record.Set(TEXT("name"), ClassName);
	Record.Set(TEXT("class"), ClassName);
	FConvJson ChainJ = FConvJson::Array();
	for (const FString& C : Chain)
	{
		ChainJ.Add(C);
	}
	Record.Set(TEXT("class_chain"), ChainJ);
	FConvJson Props = PropsJson(Ex, Defaults);
	ClassDefaultsJson.Set(ClassName, Props);
	Record.Set(TEXT("properties"), FConvJson::Object());
	// inline subobjects (Emitter.Emitters), keyed by the reference string used in the properties
	FConvJson Subs = FConvJson::Object();
	if (const FConvProp* Em = ConvFindProp(Defaults, TEXT("Emitters")))
	{
		TArray<const FConvProp*> Refs;
		if (Em->IsArray())
		{
			for (const FConvProp& X : Em->A)
			{
				Refs.Add(&X);
			}
		}
		else
		{
			Refs.Add(Em);
		}
		for (const FConvProp* X : Refs)
		{
			if (!X->IsRef() || !X->Ref.IsSet())
			{
				continue;
			}
			FConvObject* So = X->Ref.Load();
			if (!So)
			{
				continue;
			}
			const FConvJson RefKey = Ex.Jsonify(*X);
			if (RefKey.Type != FConvJson::EType::String)
			{
				continue;
			}
			FConvJson Sub = FConvJson::Object();
			Sub.Set(TEXT("name"), So->Name());
			Sub.Set(TEXT("class"), So->ClassName());
			FConvJson SChain = FConvJson::Array();
			for (const FString& C : So->ClassChain())
			{
				SChain.Add(C);
			}
			Sub.Set(TEXT("class_chain"), SChain);
			Sub.Set(TEXT("properties"), PropsJson(Ex, So->Props()));
			Subs.Set(RefKey.S, Sub);
			if (!ClassDefaultsJson.Has(So->ClassName()))
			{
				ClassDefaultsJson.Set(So->ClassName(), PropsJson(Ex, R.Mgr->ClassDefaults(So->Pkg, So->Export().ClassIndex)));
			}
		}
	}
	Record.Set(TEXT("subobjects"), Subs);

	TSharedPtr<FArenaUE2ActorDesc> D = MakeShared<FArenaUE2ActorDesc>();
	D->Json = ToJsonObject(Record);
	D->ClassDefaults = ToJsonObject(ClassDefaultsJson);
	if (!D->Json.IsValid() || !D->ClassDefaults.IsValid())
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("UE2Effects: %s: its default properties could not be read"), *PkgClass);
		return false;
	}
	D->Name = ClassName;
	D->Class = ClassName;
	D->ClassChain = Chain;
	const TSharedPtr<FJsonObject>* Def = nullptr;
	D->ClassDefaults->TryGetObjectField(ClassName, Def);
	D->Props = FArenaUE2Props::Merge(Def ? *Def : nullptr, nullptr);
	const double Ds = D->Props.Num(TEXT("DrawScale"), 1.0);
	D->DrawScale = Ds == 0.0 ? 1.f : float(Ds);
	for (const TSharedPtr<FJsonValue>& V : D->Props.Array(TEXT("Skins")))
	{
		D->Skins.Add(FArenaUE2Props::ToString(V));
	}
	DescCache()[PkgClass] = D;
	UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Effects: %s loaded (%d emitters)"), *PkgClass, D->Props.Array(TEXT("Emitters")).Num());
	Out = *D;
	return true;
}

AArenaEmitter* ArenaUE2Effects::SpawnEmitter(UWorld* World, const FString& PkgClass, const FVector& Location, const FRotator& Rotation, float MaxLife)
{
	using namespace ArenaUE2EffectsLocal;
	FArenaUE2ActorDesc D;
	if (!World || !GetClassDesc(PkgClass, D))
	{
		return nullptr;
	}
	D.Location = Location;
	D.Rotation = Rotation;
	const FTransform Xf(Rotation, Location);
	AArenaEmitter* E = World->SpawnActorDeferred<AArenaEmitter>(AArenaEmitter::StaticClass(), Xf, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!E)
	{
		return nullptr;
	}
	E->InitFromUE2(D);
	E->FinishSpawning(Xf);
	// UE2 Emitter.AutoDestroy (all emitters done) / the class's LifeSpan
	float Life = EstimateLife(D);
	const float ClassLife = float(D.Props.Num(TEXT("LifeSpan"), 0.0));
	if (ClassLife > 0.f)
	{
		Life = Life > 0.f ? FMath::Min(Life, ClassLife) : ClassLife;
	}
	E->SetLifeSpan(FMath::Clamp(Life > 0.f ? Life + 0.25f : MaxLife, 0.2f, MaxLife));
	++SpawnCounts().FindOrAdd(PkgClass);
	return E;
}

AActor* ArenaUE2Effects::SpawnSpriteXEmitter(UWorld* World, const FString& PkgClass, const FVector& Location)
{
	using namespace ArenaUE2EffectsLocal;
	FArenaUE2ActorDesc D;
	if (!World || !GetClassDesc(PkgClass, D))
	{
		return nullptr;
	}
	const FString Tex = SpriteTexturePath(D.Skins.Num() > 0 ? D.Skins[0] : D.Props.Str(TEXT("Texture")));
	if (Tex.IsEmpty())
	{
		return nullptr;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Location), SP);
	if (!A)
	{
		return nullptr;
	}
	USceneComponent* Root = NewObject<USceneComponent>(A);
	A->SetRootComponent(Root);
	Root->SetWorldLocation(Location);
	Root->RegisterComponent();
	const float Life = FMath::Max(0.1f, float(D.Props.Num(TEXT("LifeSpan"), 1.0)));
	const float Size = float(D.Props.Num(TEXT("mSizeRange"), 10.0) + D.Props.Num(TEXT("mSizeRange[1]"), D.Props.Num(TEXT("mSizeRange"), 10.0))) * 0.5f * S;
	ArenaArtFX::FSpriteDesc SD;
	SD.Texture = *Tex;
	// UE2 ERenderStyle: 4 Modulated, 5 Alpha -> translucent; Translucent / Additive / Particle -> additive
	const int32 Style = int32(D.Props.Num(TEXT("Style"), 3.0));
	SD.Blend = Style == 5 || Style == 4 ? ArenaArtFX::EBlend::Translucent : ArenaArtFX::EBlend::Additive;
	SD.Facing = ArenaArtFX::EFacing::Camera;
	SD.Size = FVector2D(Size, Size);
	SD.LifeTime = Life;
	SD.FadePower = D.Props.Bool(TEXT("mAttenuate"), true) ? 1.f : 0.f;
	const FColor C = D.Props.Color(TEXT("mColorRange"), FColor::White);
	SD.Tint = FLinearColor(C.R / 255.f, C.G / 255.f, C.B / 255.f);
	UArenaSpriteComponent* Comp = NewObject<UArenaSpriteComponent>(A);
	Comp->SetupAttachment(Root);
	if (!Comp->SetupSprite(SD))
	{
		A->Destroy();
		return nullptr;
	}
	Comp->RegisterComponent();
	A->SetLifeSpan(Life);
	++SpawnCounts().FindOrAdd(PkgClass);
	return A;
}

AActor* ArenaUE2Effects::SpawnMeshXEmitter(UWorld* World, const FString& PkgClass, const FVector& Location, const FRotator& Rotation)
{
	using namespace ArenaUE2EffectsLocal;
	FArenaUE2ActorDesc D;
	if (!World || !GetClassDesc(PkgClass, D) || !D.IsA(TEXT("xEmitter")))
	{
		return nullptr;
	}
	const FString Type = D.Props.Str(TEXT("mParticleType"));
	const bool bMesh = Type.Contains(TEXT("Mesh")) || int32(D.Props.Num(TEXT("mParticleType"), 0.0)) == 4;
	const TArray<TSharedPtr<FJsonValue>>& Nodes = D.Props.Array(TEXT("mMeshNodes"));
	FString MeshPath = Nodes.Num() > 0 ? FArenaUE2Props::ToString(Nodes[0]) : D.Props.Str(TEXT("mMeshNodes"));
	UStaticMesh* Mesh = bMesh && !MeshPath.IsEmpty() ? ArenaRuntimeContent::LoadStaticMeshByObjectPath(MeshPath) : nullptr;
	if (!Mesh)
	{
		return nullptr;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Rotation, Location), SP);
	if (!A)
	{
		return nullptr;
	}
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(A);
	A->SetRootComponent(C);
	C->SetStaticMesh(Mesh);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCastShadow(false);
	if (D.Skins.Num() > 0)
	{
		if (UMaterialInterface* M = ArenaRuntimeContent::LoadMaterialByObjectPath(D.Skins[0]))
		{
			for (int32 i = 0; i < C->GetNumMaterials(); ++i)
			{
				C->SetMaterial(i, M);
			}
		}
	}
	const FVector2D SizeRange = D.Props.Range(TEXT("mSizeRange"), FVector2D(1.0, 1.0));
	const float Size = float(FMath::FRandRange(SizeRange.X, FMath::Max(SizeRange.X, SizeRange.Y))) * D.DrawScale;
	C->SetWorldTransform(FTransform(FRotator(Rotation.Pitch, Rotation.Yaw, FMath::FRandRange(0.f, 360.f)), Location, FVector(FMath::Max(0.01f, Size))));
	C->RegisterComponent();
	const FVector2D LifeRange = D.Props.Range(TEXT("mLifeRange"), FVector2D(0.2, 0.2));
	A->SetLifeSpan(FMath::Clamp(float(LifeRange.X + LifeRange.Y) * 0.5f, 0.05f, 2.f));
	++SpawnCounts().FindOrAdd(PkgClass);
	return A;
}

void ArenaUE2Effects::Preload()
{
	if (!IsAvailable())
	{
		return;
	}
	const double T0 = FPlatformTime::Seconds();
	FArenaUE2ActorDesc D;
	for (const TCHAR* C : { TEXT("XEffects.RedeemerExplosion"), TEXT("XEffects.SmallRedeemerExplosion"), TEXT("XEffects.NewRedeemerTrail"),
		TEXT("XEffects.RedeemerTrail"), TEXT("XEffects.IonCore"), TEXT("XEffects.IonEffect"), TEXT("XEffects.IonShaft"), TEXT("XEffects.IonLightBeam"),
		TEXT("XEffects.NewIonEffect"), TEXT("Onslaught.ONSAVRiLRocketExplosion"), TEXT("Onslaught.ONSGrenadeExplosionEffect"),
		TEXT("Onslaught.ONSVehicleExplosionEffect"), TEXT("Onslaught.ONSSmallVehicleExplosionEffect"), TEXT("XWeapons.PainterBeamEffect"),
		TEXT("Onslaught.ONSMineLayerTargetBeamEffect") })
	{
		GetClassDesc(C, D);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Effects: stock effect classes read in %.2f s"), FPlatformTime::Seconds() - T0);
}

bool UArenaUE2EffectsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UArenaUE2EffectsSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	ArenaUE2Effects::Preload();
}

int32 ArenaUE2Effects::GetSpawnCount(const FString& PkgClass)
{
	const int32* N = ArenaUE2EffectsLocal::SpawnCounts().Find(PkgClass);
	return N ? *N : 0;
}

FString ArenaUE2Effects::SpriteTexturePath(const FString& UE2Path)
{
	// "Pkg.Group.Name" -> textures/<Pkg>/<Group.Name>.png, or a material's base texture (materials/<Pkg>/<Group.Name>.json)
	FString Pkg, Rest;
	if (!UE2Path.Split(TEXT("."), &Pkg, &Rest))
	{
		return FString();
	}
	const FString Cache = ArenaContent::GetCacheDir();
	FString TexPath = UE2Path;
	if (!FPaths::FileExists(FPaths::Combine(Cache, TEXT("textures"), Pkg, Rest + TEXT(".png"))))
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Cache, TEXT("materials"), Pkg, Rest + TEXT(".json"))))
		{
			return FString();
		}
		TSharedPtr<FJsonObject> J;
		const TSharedRef<TJsonReader<>> JR = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(JR, J) || !J.IsValid() || !J->TryGetStringField(TEXT("base_texture"), TexPath))
		{
			return FString();
		}
		if (!TexPath.Split(TEXT("."), &Pkg, &Rest))
		{
			return FString();
		}
	}
	return FString::Printf(TEXT("Textures/%s/%s"), *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
}

// ---------------------------------------------------------------------------------------------
// AArenaIonEffect (XEffects.IonEffect)
// ---------------------------------------------------------------------------------------------

AArenaIonEffect::AArenaIonEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetCastShadows(false);
	Light->SetIntensityUnits(ELightUnits::Unitless);
	// IonEffect: LightHue 160 / LightSaturation 140 (blue), LightBrightness 255, LightRadius 12
	Light->SetLightColor(FLinearColor(0.35f, 0.5f, 1.f));
	Light->SetIntensity(80000.f);
	Light->SetAttenuationRadius(2400.f);
}

bool AArenaIonEffect::AimAt(const FVector& InHitLocation, const FVector& InHitNormal)
{
	using namespace ArenaUE2EffectsLocal;
	UWorld* World = GetWorld();
	FArenaUE2ActorDesc Ion, CoreD, ShaftD, BeamD;
	if (!World || !ArenaUE2Effects::GetClassDesc(TEXT("XEffects.IonEffect"), Ion) || !ArenaUE2Effects::GetClassDesc(TEXT("XEffects.IonCore"), CoreD)
		|| !ArenaUE2Effects::GetClassDesc(TEXT("XEffects.IonShaft"), ShaftD) || !ArenaUE2Effects::GetClassDesc(TEXT("XEffects.IonLightBeam"), BeamD))
	{
		return false;
	}
	Top = GetActorLocation();
	HitLocation = InHitLocation;
	HitNormal = InHitNormal;
	DropTime = FMath::Max(0.05f, float(Ion.Props.Num(TEXT("DropTime"), 0.5)));
	SetLifeSpan(float(Ion.Props.Num(TEXT("LifeSpan"), 2.0)));
	StartTime = World->GetTimeSeconds();

	auto Layer = [this](const FArenaUE2ActorDesc& D, ArenaArtFX::EFacing Facing, float Width, float Length) -> UArenaSpriteComponent*
	{
		const FString Tex = ArenaUE2Effects::SpriteTexturePath(D.Skins.Num() > 0 ? D.Skins[0] : FString());
		if (Tex.IsEmpty())
		{
			return nullptr;
		}
		ArenaArtFX::FSpriteDesc SD;
		SD.Texture = *Tex;
		SD.Blend = ArenaArtFX::EBlend::Additive;
		SD.Facing = Facing;
		SD.Size = FVector2D(Length, Width);
		const FColor C = D.Props.Color(TEXT("mColorRange"), FColor::White);
		SD.Tint = D.Props.Has(TEXT("mColorRange")) ? FLinearColor(C.R / 255.f, C.G / 255.f, C.B / 255.f) * 4.f : FLinearColor::White;
		SD.FadePower = 0.f;
		SD.bRandomRoll = Facing == ArenaArtFX::EFacing::Camera;
		SD.TileLength = Facing == ArenaArtFX::EFacing::Beam ? float(D.Props.Num(TEXT("mRegenDist"), 128.0)) * S * 2.f : 0.f;
		UArenaSpriteComponent* C2 = NewObject<UArenaSpriteComponent>(this);
		C2->SetupAttachment(RootComponent);
		C2->SetUsingAbsoluteLocation(true);
		C2->SetUsingAbsoluteRotation(true);
		C2->SetUsingAbsoluteScale(true);
		if (!C2->SetupSprite(SD))
		{
			C2->DestroyComponent();
			return nullptr;
		}
		C2->RegisterComponent();
		return C2;
	};
	// IonCore (xEmitter sprite, mSizeRange 300 UU), IonShaft (4 beams, mSizeRange 50..90 UU), IonLightBeam (1 beam, 22 UU x DrawScale 5)
	const float CoreSize = float(CoreD.Props.Num(TEXT("mSizeRange"), 300.0)) * S;
	Core = Layer(CoreD, ArenaArtFX::EFacing::Camera, CoreSize, CoreSize);
	if (Core)
	{
		Core->SetWorldLocation(Top);
	}
	const int32 NumShaft = FMath::Clamp(int32(ShaftD.Props.Num(TEXT("mMaxParticles"), 4.0)), 1, 8);
	const float ShaftMin = float(ShaftD.Props.Num(TEXT("mSizeRange"), 50.0)) * S;
	const float ShaftMax = float(ShaftD.Props.Num(TEXT("mSizeRange[1]"), 90.0)) * S;
	for (int32 i = 0; i < NumShaft; ++i)
	{
		const float W = FMath::Lerp(ShaftMin, ShaftMax, NumShaft > 1 ? float(i) / float(NumShaft - 1) : 0.5f);
		if (UArenaSpriteComponent* C = Layer(ShaftD, ArenaArtFX::EFacing::Beam, W, 100.f))
		{
			Shaft.Add(C);
		}
	}
	const float BeamW = float(BeamD.Props.Num(TEXT("mSizeRange[1]"), 22.0)) * S * FMath::Max(1.f, BeamD.DrawScale) * 0.5f;
	if (UArenaSpriteComponent* C = Layer(BeamD, ArenaArtFX::EFacing::Beam, BeamW, 100.f))
	{
		LightBeam.Add(C);
	}
	FadeTime = float(ShaftD.Props.Num(TEXT("mLifeRange"), 1.3));
	Tick(0.f);
	return Core != nullptr || Shaft.Num() > 0;
}

void AArenaIonEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float T = World->GetTimeSeconds() - StartTime;
	// state Drop: the core falls from the cannon to the mark, the shaft follows it from the top
	const float Delta = FMath::Min(T / DropTime, 1.f);
	const FVector CoreLoc = Top * (1.f - Delta) + HitLocation * Delta;
	if (Core)
	{
		Core->SetWorldLocation(CoreLoc);
	}
	const FVector ShaftEnd = bLanded ? CoreLoc : CoreLoc;
	for (int32 i = 0; i < Shaft.Num(); ++i)
	{
		// the xEmitter beam strands wander a little around the axis
		const float Ph = T * 7.f + i * 1.9f;
		const FVector Wobble(FMath::Sin(Ph) * 12.f, FMath::Cos(Ph * 1.3f) * 12.f, 0.f);
		Shaft[i]->SetBeam(Top + Wobble, ShaftEnd - Wobble);
	}
	for (UArenaSpriteComponent* C : LightBeam)
	{
		C->SetBeam(Top, HitLocation);
	}
	if (!bLanded && T >= DropTime)
	{
		// Timer: the core goes, the beams attenuate, NewIonEffect 100 UU above the mark, Rot(0, 16384, 0)
		bLanded = true;
		LandTime = T;
		if (Core)
		{
			Core->DestroyComponent();
			Core = nullptr;
		}
		ArenaUE2Effects::SpawnEmitter(World, TEXT("XEffects.NewIonEffect"), HitLocation + FVector(0.f, 0.f, 100.f * ArenaUE2World::Scale), ArenaUE2Effects::UE2Rot(0, 16384, 0));
	}
	if (bLanded)
	{
		const float Fade = FMath::Clamp(1.f - (T - LandTime) / FMath::Max(FadeTime, 0.1f), 0.f, 1.f);
		for (UArenaSpriteComponent* C : Shaft)
		{
			C->SetIntensityScale(Fade);
		}
		for (UArenaSpriteComponent* C : LightBeam)
		{
			C->SetIntensityScale(Fade);
		}
		Light->SetIntensity(80000.f * Fade);
	}
}

// ---------------------------------------------------------------------------------------------
// Test hook: -UE2FXTest
// ---------------------------------------------------------------------------------------------

bool UArenaUE2FXTestSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	return FCString::Strifind(FCommandLine::Get(), TEXT("-UE2FXTest")) != nullptr;
#endif
}

bool UArenaUE2FXTestSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UArenaUE2FXTestSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UArenaUE2FXTestSubsystem, STATGROUP_Tickables);
}

void UArenaUE2FXTestSubsystem::Fire(bool bIonStrike)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	// the viewer survives the blast (screenshots of the whole effect)
	if (AArenaCharacter* Me = Cast<AArenaCharacter>(PC->GetPawn()))
	{
		Me->Health = 100000.f;
	}
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	const FRotator View = PC->PlayerCameraManager->GetCameraRotation();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaUE2FXTest), false, PC->GetPawn());
	// ground points in front of the view (yaw -60 .. 60, 20 .. 50 m); the ion strike needs open sky above the point
	bool bFound = false;
	for (float Dist : { 3000.f, 2200.f, 4000.f, 5000.f })
	{
		for (float Yaw : { 0.f, -20.f, 20.f, -40.f, 40.f, -60.f, 60.f })
		{
			const FVector Dir = FRotator(0.f, View.Yaw + Yaw, 0.f).Vector();
			FHitResult Wall;
			const FVector Ahead = World->LineTraceSingleByChannel(Wall, Eye, Eye + Dir * Dist, ECC_Visibility, Q) ? Wall.Location - Dir * 100.f : Eye + Dir * Dist;
			FHitResult Ground;
			if (!World->LineTraceSingleByChannel(Ground, Ahead, Ahead - FVector(0.f, 0.f, 4000.f), ECC_Visibility, Q))
			{
				continue;
			}
			Point = Ground.ImpactPoint;
			if (!bIonStrike)
			{
				bFound = FVector::Dist(Point, Eye) > 1500.f;
			}
			else if (AArenaIonCannon* C = AArenaIonCannon::FindOrSpawn(World, Point))
			{
				bFound = C->CheckMark(nullptr, Point, false, FString());
			}
			if (bFound)
			{
				break;
			}
		}
		if (bFound)
		{
			break;
		}
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (!bIonStrike && Kind.Equals(TEXT("warhead"), ESearchCase::IgnoreCase))
	{
		// an unguided warhead (no instigator: team 255) coming in high from 80 m behind the point, towards it
		const FVector From = Point + FVector(0.f, 0.f, 600.f) + FRotator(0.f, View.Yaw, 0.f).Vector() * 6000.f;
		AArenaWarheadShell* W = World->SpawnActor<AArenaWarheadShell>(AArenaWarheadShell::StaticClass(), From, (Point - From).Rotation(), SP);
		if (W)
		{
			W->Launch(nullptr, TEXT("Warhead Launcher"), Point - From);
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2FXTest: incoming warhead from %s to %s"), *From.ToCompactString(), *Point.ToCompactString());
		return;
	}
	if (!bIonStrike)
	{
		AArenaWarheadShell* W = World->SpawnActor<AArenaWarheadShell>(AArenaWarheadShell::StaticClass(), Point + FVector(0.f, 0.f, 400.f), FRotator(-60.f, 0.f, 0.f), SP);
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2FXTest: warhead %s"), W ? *W->GetName() : TEXT("not spawned"));
		if (W)
		{
			W->Launch(nullptr, TEXT("Warhead Launcher"), FVector(0.5f, 0.f, -1.f));
			W->BlowUp(Point);
		}
	}
	else if (AArenaIonCannon* C = AArenaIonCannon::FindOrSpawn(World, Point))
	{
		const bool bOk = C->CheckMark(nullptr, Point, true, TEXT("Orbital Designator"));
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2FXTest: ion cannon at %s marks %s: %s"), *C->GetActorLocation().ToCompactString(), *Point.ToCompactString(), bOk ? TEXT("firing") : TEXT("no clear sky"));
	}
	int32 Live = 0;
	for (TActorIterator<AArenaEmitter> It(World); It; ++It)
	{
		++Live;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("UE2FXTest: %s at %s (view %s): RedeemerExplosion %d, IonCore %d, emitters in the world %d"), bIonStrike ? TEXT("ion strike") : TEXT("nuke"),
		*Point.ToCompactString(), *Eye.ToCompactString(), ArenaUE2Effects::GetSpawnCount(TEXT("XEffects.RedeemerExplosion")), ArenaUE2Effects::GetSpawnCount(TEXT("XEffects.IonCore")), Live);
}

void UArenaUE2FXTestSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay() || Step < 0)
	{
		return;
	}
	if (Kind.IsEmpty())
	{
		FParse::Value(FCommandLine::Get(), TEXT("-UE2FXTest="), Kind);
		if (Kind.IsEmpty())
		{
			Kind = TEXT("both");
		}
		FParse::Value(FCommandLine::Get(), TEXT("-UE2FXTestTime="), StartTime);
		NextTime = StartTime;
	}
	const float Now = World->GetTimeSeconds();
	if (Kind.Equals(TEXT("walkinto"), ESearchCase::IgnoreCase))
	{
		// -UE2FXWalk=x,y,z (UE2 units; with -UE2FXWalkDir=yaw degrees, else towards the nearest surface): the player's
		// pawn is put there at -UE2FXTestTime and walks forward at full input for 4 s; logs its path in UE2 units
		APlayerController* PC = World->GetFirstPlayerController();
		AArenaCharacter* Me = PC ? Cast<AArenaCharacter>(PC->GetPawn()) : nullptr;
		if (!Me || Now < StartTime)
		{
			return;
		}
		if (Step == 0)
		{
			FString P;
			FParse::Value(FCommandLine::Get(), TEXT("-UE2FXWalk="), P, false);
			TArray<FString> Parts;
			P.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() < 3)
			{
				Step = -1;
				return;
			}
			const FVector At(FCString::Atof(*Parts[0]) * 2.f, FCString::Atof(*Parts[1]) * 2.f, FCString::Atof(*Parts[2]) * 2.f);
			Me->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
			Me->GetCharacterMovement()->Velocity = FVector::ZeroVector;
			float Yaw = 0.f;
			if (!FParse::Value(FCommandLine::Get(), TEXT("-UE2FXWalkDir="), Yaw))
			{
				float Best = 1e9f;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaWalkInto), false, Me);
				for (int32 i = 0; i < 32; ++i)
				{
					const FVector Dir = FRotator(0.f, i * 11.25f, 0.f).Vector();
					FHitResult H;
					if (World->LineTraceSingleByChannel(H, At, At + Dir * 300.f, ECC_Pawn, Q) && H.Distance < Best)
					{
						Best = H.Distance;
						Yaw = (-H.ImpactNormal).GetSafeNormal2D().Rotation().Yaw;
						UE_LOG(LogOpenUT2k4, Log, TEXT("UE2FXTest: walkinto: surface %.0f UU away, normal %s (%s)"), H.Distance / 2.f, *H.ImpactNormal.ToCompactString(), *GetNameSafe(H.GetComponent()));
					}
				}
			}
			WalkYaw = Yaw;
			PC->SetControlRotation(FRotator(0.f, Yaw, 0.f));
			NextTime = Now;
			Step = 1;
			MarkTime = Now;
			return;
		}
		if (Step == 1)
		{
			Me->AddMovementInput(FRotator(0.f, WalkYaw, 0.f).Vector(), 1.f);
			if (Now >= NextTime)
			{
				NextTime = Now + 0.1f;
				const FVector L = Me->GetActorLocation() / 2.f;
				const FFindFloorResult& Fl = Me->GetCharacterMovement()->CurrentFloor;
				UE_LOG(LogOpenUT2k4, Log, TEXT("UE2FXTest: walkinto t=%.1f at (%.1f, %.1f, %.1f) UU vel %s mode %d floor %d dist %.1f normal %s impact %s line %d on %s"), Now - MarkTime, L.X, L.Y, L.Z,
					*(Me->GetVelocity() / 2.f).ToCompactString(), int32(Me->GetCharacterMovement()->MovementMode.GetValue()), Fl.IsWalkableFloor() ? 1 : 0,
					Fl.FloorDist, *Fl.HitResult.ImpactNormal.ToCompactString(), *(Fl.HitResult.ImpactPoint / 2.f).ToCompactString(), Fl.bLineTrace ? 1 : 0,
					*GetNameSafe(Fl.HitResult.GetComponent()));
			}
			if (Now - MarkTime > 4.f)
			{
				Step = -1;
			}
		}
		return;
	}
	if (Kind.Equals(TEXT("vehrest"), ESearchCase::IgnoreCase))
	{
		// every 10 s: how far each empty vehicle moved since the last check (parked vehicles must not creep)
		if (Now >= NextTime)
		{
			NextTime = Now + 10.f;
			for (TActorIterator<AArenaONS_Vehicle> It(World); It; ++It)
			{
				FVector* Last = RestPositions.Find(It->GetFName());
				if (Last && !It->HasDriver())
				{
					UE_LOG(LogOpenUT2k4, Log, TEXT("UE2FXTest: %s %s moved %.1f cm in 10 s"), *It->GetVehicleName(), *It->GetName(), FVector::Dist(*Last, It->GetActorLocation()));
				}
				RestPositions.Add(It->GetFName(), It->GetActorLocation());
			}
		}
		return;
	}
	if (Kind.Equals(TEXT("bighead"), ESearchCase::IgnoreCase))
	{
		// follow the character with the biggest head from behind (Big Head mutator checks with -AutoScreenshot)
		APlayerController* PC = World->GetFirstPlayerController();
		if (PC && Now >= NextTime)
		{
			NextTime = Now + 3.f;
			AArenaCharacter* Best = nullptr;
			for (TActorIterator<AArenaCharacter> It(World); It; ++It)
			{
				if (*It != PC->GetPawn() && It->IsAlive() && (!Best || It->HeadScale > Best->HeadScale))
				{
					Best = *It;
				}
			}
			if (Best)
			{
				PC->SetViewTarget(Best);
				if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("arena.BehindView")))
				{
					CV->Set(1, ECVF_SetByCode);
				}
				UE_LOG(LogOpenUT2k4, Log, TEXT("UE2FXTest: viewing %s, head scale %.2f"), *Best->GetName(), Best->HeadScale);
			}
		}
		return;
	}
	if (Now < NextTime)
	{
		return;
	}
	// steps: fire, then shots at +0.15, 0.4, 0.8, 1.3, 2.0 s; "both": the ion strike 6 s after the nuke
	static const float ShotDelays[] = { 0.15f, 0.25f, 0.4f, 0.5f, 0.7f };
	if (Step == 0)
	{
		bIon = Kind.Equals(TEXT("ion"), ESearchCase::IgnoreCase);
		Fire(bIon);
		ShotIndex = Kind.Equals(TEXT("warhead"), ESearchCase::IgnoreCase) ? ShotIndex : 0;
		// the ion beam comes 0.5 s after the strike starts
		NextTime = Now + (bIon ? 0.55f : 0.f) + ShotDelays[0];
		Step = 1;
		return;
	}
	if (Step >= 1 && Step <= 5)
	{
		FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("ue2fx_%s_%d"), bIon ? TEXT("ion") : *Kind.ToLower(), ShotIndex++), false, false);
		NextTime = Now + (Step < 5 ? ShotDelays[Step] : 0.f);
		++Step;
		return;
	}
	if (Kind.Equals(TEXT("warhead"), ESearchCase::IgnoreCase) && ++ShotIndex < 30)
	{
		// five incoming warheads, 6 s apart (ShotIndex counts 5 shots per warhead)
		Step = 0;
		NextTime = Now + 6.f;
		return;
	}
	if (!bIon && Kind.Equals(TEXT("both"), ESearchCase::IgnoreCase))
	{
		bIon = true;
		Step = 0;
		Kind = TEXT("ion");
		NextTime = Now + 5.f;
		return;
	}
	Step = -1;
}
