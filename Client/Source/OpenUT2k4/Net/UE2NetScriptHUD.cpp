#include "UE2NetScriptHUD.h"

#include "../ArenaArt.h"
#include "../ArenaContent.h"
#include "../ArenaHUD.h"
#include "../ArenaHudFonts.h"
#include "../ArenaRuntimeContent.h"
#include "../ArenaRuntimeMap.h"
#include "../UScript/ArenaUScript.h"
#include "../UScript/UScriptBridge.h"
#include "../UScript/UScriptCanvas.h"
#include "../UScript/UScriptCore.h"
#include "../UScript/UScriptGUI.h"
#include "../UScript/UScriptRuntime.h"
#include "../UScript/UScriptWorld.h"
#include "../UScript/UScriptPresentation.h"
#include "Animation/AnimSequence.h"
#include "CanvasItem.h"
#include "Camera/PlayerCameraManager.h"
#include "Dom/JsonObject.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureResource.h"
#include "UE2NetConnection.h"
#include "UE2NetLog.h"
#include "UE2NetModContent.h"
#include "UE2NetPlay.h"
#include "UE2NetScriptVisuals.h"
#include "UE2NetActors.h"
#include "UE2NetModWeapons.h"
#include "UE2NetWorld.h"

#if OPENUT2K4_USCRIPT_ENABLED

namespace UE2NetScriptHUDLocal
{
	FUE2NetScriptHUD* GInstance = nullptr;

	// ERenderStyle
	constexpr uint8 STY_Normal = 1;
	constexpr uint8 STY_Masked = 2;
	constexpr uint8 STY_Translucent = 3;
	constexpr uint8 STY_Modulated = 4;
	constexpr uint8 STY_Alpha = 5;
	constexpr uint8 STY_Additive = 6;
	// ENetRole
	constexpr uint8 UE2_ROLE_SimulatedProxy = 2;
	constexpr uint8 UE2_ROLE_AutonomousProxy = 3;
	constexpr uint8 UE2_ROLE_Authority = 4;
	// ENetMode
	constexpr uint8 UE2_NM_Client = 3;

	/**
	 * Packages that are never mirrored nor run: anti-cheat and client checks (they inspect or report the client; nothing
	 * of them is emulated or answered). More with -UE2ScriptDeny=Pkg,Pkg (prefixes, case-insensitive).
	 */
	const TCHAR* const DeniedPrefixes[] = { TEXT("AntiTCC"), TEXT("itsfake"), TEXT("UTSecure"), TEXT("ACEv"), TEXT("AnthChecker"), TEXT("UTDC"),
		TEXT("SafeGame"), TEXT("TCCv"), TEXT("CheatGuard"), TEXT("AntiCheat") };

	/** Script packages of the game itself: their HUD classes are drawn by our native HUD (ArenaHUDUT). */
	const TCHAR* const StockScriptPackages[] = { TEXT("Engine"), TEXT("Core"), TEXT("XInterface"), TEXT("XGame"), TEXT("UnrealGame"), TEXT("Onslaught"),
		TEXT("OnslaughtFull"), TEXT("OnslaughtBP"), TEXT("UT2k4Assault"), TEXT("UT2k4AssaultFull"), TEXT("SkaarjPack"), TEXT("BonusPack"), TEXT("GUI2K4"),
		TEXT("Gameplay"), TEXT("XWeapons"), TEXT("XPickups"), TEXT("XEffects"), TEXT("Vehicles"), TEXT("UTClassic"), TEXT("XAdmin"), TEXT("xVoting") };

	bool IsDeniedPackage(const FString& Package)
	{
		for (const TCHAR* P : DeniedPrefixes)
		{
			if (Package.StartsWith(P, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		static TArray<FString> Extra;
		static bool bParsed = false;
		if (!bParsed)
		{
			bParsed = true;
			FString List;
			if (FParse::Value(FCommandLine::Get(), TEXT("UE2ScriptDeny="), List))
			{
				List.ParseIntoArray(Extra, TEXT(","));
			}
		}
		for (const FString& P : Extra)
		{
			if (Package.StartsWith(P, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool IsStockPackage(const FString& Package)
	{
		for (const TCHAR* P : StockScriptPackages)
		{
			if (Package.Equals(P, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool ChainHas(const FUSClass* C, const TCHAR* Name)
	{
		int32 Guard = 0;
		for (; C && Guard < 64; C = C->GetSuperClass(), ++Guard)
		{
			if (C->Name == Name)
			{
				return true;
			}
		}
		return false;
	}

	/** "Name" or "Name[3]". */
	void SplitKey(const FString& Key, FString& OutName, int32& OutIndex)
	{
		OutIndex = 0;
		int32 Open = INDEX_NONE;
		if (Key.FindChar(TEXT('['), Open) && Key.EndsWith(TEXT("]")))
		{
			OutName = Key.Left(Open);
			OutIndex = FCString::Atoi(*Key.Mid(Open + 1));
			return;
		}
		OutName = Key;
	}

	/** Text without the UE2 colour codes (ESC R G B). */
	FString StripColourCodes(const FString& S)
	{
		FString Out;
		Out.Reserve(S.Len());
		for (int32 i = 0; i < S.Len(); ++i)
		{
			if (S[i] == 27)
			{
				i += 3;
				continue;
			}
			Out.AppendChar(S[i]);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> ReadJson(const FString& File)
	{
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!File.IsEmpty() && FFileHelper::LoadFileToString(Text, *File))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root);
		}
		return Root;
	}

	/** A material as the canvas draws it: its texture and how it is blended, turned or scaled. */
	struct FMatInfo
	{
		/** Weak: the content cache owns it (a freed texture is resolved again rather than drawn from freed memory) */
		TWeakObjectPtr<UTexture2D> Texture;
		/** UE2 size (texels of TextureCoords / U V) */
		float W = 0.f, H = 0.f;
		/** FinalBlend FrameBufferBlending (-1: none, the canvas Style decides) */
		int32 FrameBlend = -1;
		/** TexRotator: 0 fixed rotation (Yaw), 1 turning (Yaw per second), 2 oscillating */
		int32 RotType = -1;
		float RotYaw = 0.f;
		/** TexScaler / TexRotator offsets are applied around the texture centre (approximation) */
		float UScale = 1.f, VScale = 1.f;
		/** FadeColor in a Combiner: brightness pulse period (0: none) */
		float FadePeriod = 0.f;
		bool bResolved = false;
		double RetryAt = 0.0;
	};
}

using namespace UE2NetScriptHUDLocal;

// =================================================================================================
// The client script world
// =================================================================================================

/** UE2 client-side script world: mirrored actors (data) and the actors the HUD spawned (run). */
class FUE2NetMirrorWorld : public FUSWorld
{
public:
	FUSObject* Level = nullptr;
	/** Our PlayerController's mirror (Level.GetLocalPlayerController). */
	FUSObject* LocalPC = nullptr;
	/** Actors spawned on our side (HUD, scoreboard, their helpers): ticked, timers, state code. */
	TArray<FUSObject*> LocalActors;
	/** Mirrored actors whose client Timer runs like on UE2's client (the GameReplicationInfo's clock). */
	TArray<FUSObject*> TimerOnly;
	/** Set while a mod actor's client code runs: what it spawns is a client-side actor of that code. */
	bool bClientScope = false;
	TFunction<void(FUSObject*)> OnClientSpawn;
	/** The map's UE world and where the mirror's actors are shown (sounds, traces and effects of script). */
	TWeakObjectPtr<UWorld> PresentationWorld;
	TFunction<bool(FUSObject*, FVector&)> PresentationLocation;
	TFunction<bool(FUSObject*, FName, FTransform&)> PresentationBone;
	TFunction<bool(FUSObject*, FUSFunction*)> RemoteFilter;
	TFunction<void(FUSObject*, FUSFunction*, uint8*)> RemoteSend;

	virtual FUSObject* GetLocalPlayerController() override
	{
		return LocalPC && !LocalPC->IsPendingKill() ? LocalPC : nullptr;
	}

	virtual UWorld* GetPresentationWorld() override
	{
		return PresentationWorld.Get();
	}

	virtual bool GetPresentationLocation(FUSObject* Actor, FVector& OutLocation) override
	{
		return PresentationLocation && PresentationLocation(Actor, OutLocation);
	}

	virtual bool GetPresentationBone(FUSObject* Actor, FName Bone, FTransform& OutTransform) override
	{
		return PresentationBone && PresentationBone(Actor, Bone, OutTransform);
	}

	virtual bool HasPresentationAnim(FUSObject* Actor, FName Sequence) override
	{
		FUSProperty* P = Actor && Actor->Data ? Actor->FindProperty(TEXT("Mesh")) : nullptr;
		const FUSObject* Mesh = P && P->Type == EUSType::Object ? *reinterpret_cast<FUSObject* const*>(Actor->Data + P->Offset) : nullptr;
		if (!Mesh || Sequence.IsNone())
		{
			return false;
		}
		const FString Key = Mesh->GetPathName() + TEXT("|") + Sequence.ToString();
		if (const bool* Hit = HasAnimCache.Find(Key))
		{
			return *Hit;
		}
		const FString Prefix = UE2NetModWeapons::AnimPrefixForMesh(Mesh->GetPathName());
		const bool bHas = !Prefix.IsEmpty() && ArenaArt::Load<UAnimSequence>(Prefix + Sequence.ToString()) != nullptr;
		HasAnimCache.Add(Key, bHas);
		return bHas;
	}
	TMap<FString, bool> HasAnimCache;

	virtual bool IsRemoteCall(FUSObject* Actor, FUSFunction* Function) override
	{
		return RemoteFilter && RemoteFilter(Actor, Function);
	}

	virtual void CallRemote(FUSObject* Actor, FUSFunction* Function, uint8* Parms) override
	{
		if (RemoteSend)
		{
			RemoteSend(Actor, Function, Parms);
		}
	}

	virtual FUSObject* SpawnActor(FUSClass* Class, FUSObject* Owner, FName Tag, const FUSVector* Location, const FUSRotator* Rotation, FUSObject* Spawner) override
	{
		FUSRuntime& RT = FUSRuntime::Get();
		if (!Class || !Class->EnsureLoaded() || (Class->ClassFlags & USFlags::CLASS_Abstract) || !RT.ActorClass || !Class->IsChildOf(RT.ActorClass)
			|| IsDeniedPackage(Class->GetPackageName().ToString()))
		{
			return nullptr;
		}
		FUSEngineProps& P = USProps();
		FUSObject* A = ConstructObject(Class, Level, NAME_None);
		if (!A)
		{
			return nullptr;
		}
		P.Level.Set<FUSObject*>(A, Level);
		P.Owner.Set<FUSObject*>(A, Owner);
		P.Tag.Set<FName>(A, Tag != NAME_None ? Tag : Class->Name);
		P.Role.Set<uint8>(A, UE2_ROLE_Authority);
		P.RemoteRole.Set<uint8>(A, 0);
		if (Location)
		{
			P.Location.Set<FUSVector>(A, *Location);
		}
		if (Rotation)
		{
			P.Rotation.Set<FUSRotator>(A, *Rotation);
		}
		LocalActors.Add(A);
		if (bClientScope && OnClientSpawn)
		{
			OnClientSpawn(A);
		}
		// UE2 SpawnActor's events
		static const TCHAR* const Events[] = { TEXT("Spawned"), TEXT("PreBeginPlay"), TEXT("BeginPlay"), TEXT("PostBeginPlay"), TEXT("SetInitialState"),
			TEXT("PostNetBeginPlay") };
		for (const TCHAR* E : Events)
		{
			if (A->IsPendingKill())
			{
				break;
			}
			CallEvent(A, E);
		}
		return A->IsPendingKill() ? nullptr : A;
	}

	/** Called when script destroys an actor (before it is freed): the client mirror forgets it (Mirrors, ChannelOf ...). */
	TFunction<void(FUSObject*)> OnDestroyed;

	virtual bool DestroyActor(FUSObject* Actor) override
	{
		if (!Actor || Actor->IsPendingKill())
		{
			return false;
		}
		// UE2 ULevel::DestroyActor: a client cannot destroy a replicated actor it does not own the authority of (a torn off
		// one became ROLE_Authority; bNetTemporary ones are the client's to destroy)
		FUSEngineProps& P = USProps();
		if (P.Role.Get<uint8>(Actor) < UE2_ROLE_Authority)
		{
			FUSProperty* Temp = Actor->FindProperty(TEXT("bNetTemporary"));
			if (!Temp || Temp->Type != EUSType::Bool || !*reinterpret_cast<const uint32*>(Actor->Data + Temp->Offset))
			{
				return false;
			}
		}
		CallEvent(Actor, TEXT("Destroyed"));
		if (OnDestroyed)
		{
			OnDestroyed(Actor);
		}
		LocalActors.Remove(Actor);
		MarkPendingKill(Actor);
		return true;
	}

	static void CallEvent(FUSObject* Obj, const TCHAR* Name)
	{
		if (FUSFunction* F = Obj ? USVM::FindFunction(Obj, FName(Name)) : nullptr)
		{
			if (F->Parms.Num() == 0)
			{
				FUSParms Parms(F);
				Parms.Call(Obj);
			}
		}
	}
};

// =================================================================================================
// Canvas backend: our UCanvas with UT2004's fonts and the content cache's textures
// =================================================================================================

class FUE2NetCanvasBackend : public IUSCanvasBackend
{
public:
	UCanvas* Canvas = nullptr;
	APlayerController* PC = nullptr;
	float Time = 0.f;
	int32 MissingTextures = 0;

	virtual void DrawTile(FUSObject* Material, float X, float Y, float XL, float YL, float U, float V, float UL, float VL, const FColor& Color, uint8 Style) override
	{
		const FMatInfo& M = Resolve(Material);
		UTexture2D* Tex = M.Texture.Get();
		if (!Canvas || !Tex || M.W <= 0.f || M.H <= 0.f || XL == 0.f || YL == 0.f || Color.A == 0)
		{
			return;
		}
		const FTexture* Res = Tex->GetResource();
		if (!Res)
		{
			return;
		}
		// mirrored tiles: UE2 flips through a negative size or a negative UL / VL
		if (XL < 0.f)
		{
			X += XL;
			XL = -XL;
			U += UL;
			UL = -UL;
		}
		if (YL < 0.f)
		{
			Y += YL;
			YL = -YL;
			V += VL;
			VL = -VL;
		}
		FVector2D UV0(U / M.W, V / M.H), UV1((U + UL) / M.W, (V + VL) / M.H);
		if (M.UScale != 1.f || M.VScale != 1.f)
		{
			// TexScaler about the centre
			const FVector2D Mid = (UV0 + UV1) * 0.5;
			UV0 = Mid + (UV0 - Mid) / FVector2D(M.UScale, M.VScale);
			UV1 = Mid + (UV1 - Mid) / FVector2D(M.UScale, M.VScale);
		}
		FColor C = Color;
		if (M.FadePeriod > 0.f)
		{
			// a Combiner with a FadeColor (HUD pulses): the brightness goes up and down with the period
			const float S = 0.5f + 0.5f * FMath::Cos(PI * Time / M.FadePeriod);
			C = FColor(uint8(C.R * S), uint8(C.G * S), uint8(C.B * S), C.A);
		}
		FCanvasTileItem Item(FVector2D(X, Y), Res, FVector2D(XL, YL), UV0, UV1, FLinearColor(C));
		Item.BlendMode = BlendFor(M, Style);
		if (M.RotType >= 0)
		{
			float Deg = M.RotYaw * 360.f / 65536.f;
			if (M.RotType == 1)
			{
				Deg = FMath::Fmod(Time * Deg, 360.f);
			}
			else if (M.RotType == 2)
			{
				Deg = FMath::Sin(Time) * Deg;
			}
			Item.Rotation = FRotator(0.f, Deg, 0.f);
			Item.PivotPoint = FVector2D(0.5f, 0.5f);
		}
		Canvas->DrawItem(Item);
	}

	virtual void DrawText(FUSObject* Font, const FString& Text, float X, float Y, float ScaleX, float ScaleY, const FColor& Color, uint8 Style) override
	{
		UFont* F = FontFor(Font);
		if (!Canvas || !F || Text.IsEmpty() || Color.A == 0)
		{
			return;
		}
		ScaleX = ScaleX > 0.f ? ScaleX : 1.f;
		ScaleY = ScaleY > 0.f ? ScaleY : 1.f;
		const ESimpleElementBlendMode Blend = (Style == STY_Translucent || Style == STY_Additive) ? SE_BLEND_Additive : SE_BLEND_Translucent;
		float RunX = FMath::RoundToFloat(X);
		FColor RunColor = Color;
		FString Run;
		auto Flush = [&]()
		{
			if (Run.IsEmpty())
			{
				return;
			}
			FCanvasTextItem Item(FVector2D(RunX, FMath::RoundToFloat(Y)), FText::FromString(Run), F, FLinearColor(RunColor));
			Item.Scale = FVector2D(ScaleX, ScaleY);
			Item.BlendMode = Blend;
			Canvas->DrawItem(Item);
			RunX += float(F->GetStringSize(*Run)) * ScaleX;
			Run.Reset();
		};
		for (int32 i = 0; i < Text.Len(); ++i)
		{
			if (Text[i] == 27 && i + 3 < Text.Len())
			{
				Flush();
				// UE2 colour codes: ESC R G B (a 0 byte is sent as 1)
				RunColor = FColor(uint8(Text[i + 1]), uint8(Text[i + 2]), uint8(Text[i + 3]), Color.A);
				i += 3;
				continue;
			}
			Run.AppendChar(Text[i]);
		}
		Flush();
	}

	virtual void TextSize(FUSObject* Font, const FString& Text, float ScaleX, float ScaleY, float& OutXL, float& OutYL) override
	{
		OutXL = OutYL = 0.f;
		UFont* F = FontFor(Font);
		if (!F)
		{
			return;
		}
		ScaleX = ScaleX > 0.f ? ScaleX : 1.f;
		ScaleY = ScaleY > 0.f ? ScaleY : 1.f;
		const FString Plain = StripColourCodes(Text);
		OutXL = Plain.IsEmpty() ? 0.f : float(F->GetStringSize(*Plain)) * ScaleX;
		OutYL = float(FontHeight(Font, F)) * ScaleY;
	}

	virtual bool MaterialSize(FUSObject* Material, int32& OutU, int32& OutV) override
	{
		const FMatInfo& M = Resolve(Material);
		OutU = int32(M.W);
		OutV = int32(M.H);
		return M.W > 0.f && M.H > 0.f;
	}

	virtual FUSVector WorldToScreen(const FUSVector& World) override
	{
		FVector2D Screen(0.0, 0.0);
		if (!PC)
		{
			return FUSVector();
		}
		const FVector Loc = USUnits::FromUE2(World);
		FVector CamLoc;
		FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		const bool bFront = FVector::DotProduct(Loc - CamLoc, CamRot.Vector()) > 0.0;
		PC->ProjectWorldLocationToScreen(Loc, Screen, true);
		return FUSVector(float(Screen.X), float(Screen.Y), bFront ? 1.f : 0.f);
	}

	virtual void GetCamera(FUSVector& OutLocation, FUSRotator& OutRotation) override
	{
		if (!PC)
		{
			return;
		}
		FVector CamLoc;
		FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		OutLocation = USUnits::ToUE2(CamLoc);
		OutRotation = USUnits::ToUE2(CamRot);
	}

	virtual void DrawLine(float X1, float Y1, float X2, float Y2, const FColor& Color) override
	{
		if (Canvas)
		{
			FCanvasLineItem Line(FVector2D(X1, Y1), FVector2D(X2, Y2));
			Line.SetColor(FLinearColor(Color));
			Line.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(Line);
		}
	}

	virtual void DrawRect(float X, float Y, float W, float H, const FColor& Color) override
	{
		if (Canvas && W > 0.f && H > 0.f && Color.A > 0)
		{
			FCanvasTileItem Item(FVector2D(X, Y), FVector2D(W, H), FLinearColor(Color));
			Item.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(Item);
		}
	}

	/** UE2 size of a texture or material placeholder (fills Texture.USize / VSize when script reads them). */
	void InitPlaceholder(FUSObject* Obj)
	{
		if (!Obj->IsA(FName(TEXT("Material"))))
		{
			return;
		}
		const FMatInfo& M = Resolve(Obj);
		for (const TCHAR* Key : { TEXT("USize"), TEXT("UClamp") })
		{
			if (FUSProperty* P = Obj->FindProperty(FName(Key)))
			{
				*reinterpret_cast<int32*>(Obj->Data + P->Offset) = int32(M.W);
			}
		}
		for (const TCHAR* Key : { TEXT("VSize"), TEXT("VClamp") })
		{
			if (FUSProperty* P = Obj->FindProperty(FName(Key)))
			{
				*reinterpret_cast<int32*>(Obj->Data + P->Offset) = int32(M.H);
			}
		}
	}

	FString Describe() const
	{
		int32 Ok = 0;
		for (const TPair<FUSObject*, FMatInfo>& KV : Materials)
		{
			Ok += KV.Value.Texture.IsValid() ? 1 : 0;
		}
		return FString::Printf(TEXT("%d of %d materials, %d of %d fonts"), Ok, Materials.Num(), Fonts.Num() - MissingFonts, Fonts.Num());
	}

private:
	TMap<FUSObject*, FMatInfo> Materials;
	/** Weak: the fonts belong to the content cache (a freed one is looked up again). */
	TMap<FUSObject*, TPair<TWeakObjectPtr<UFont>, int32>> Fonts;
	int32 MissingFonts = 0;

	static ESimpleElementBlendMode BlendFor(const FMatInfo& M, uint8 Style)
	{
		// a FinalBlend's own frame buffer blending (FB_Overwrite 0, Modulate 1, AlphaBlend 2, AlphaModulate_MightNotFogCorrectly 3,
		// Translucent 4, Darken 5, Brighten 6, Invisible 7) decides; plain textures follow the canvas style
		switch (M.FrameBlend)
		{
		case 1: return SE_BLEND_Modulate;
		case 4: case 6: return SE_BLEND_Additive;
		case 0: case 2: case 3: case 5: return SE_BLEND_Translucent;
		default: break;
		}
		switch (Style)
		{
		case STY_Translucent: case STY_Additive: return SE_BLEND_Additive;
		case STY_Modulated: return SE_BLEND_Modulate;
		case STY_Normal: case STY_Masked: case STY_Alpha: default: return SE_BLEND_Translucent;
		}
	}

	/** The content cache's json of a texture or material (converted from its package first when only a mod has it). */
	static TSharedPtr<FJsonObject> FindJson(const FString& Path, const FString& ClassName, bool& bOutTexture, bool bConvert)
	{
		FString File = ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("textures"), Path, TEXT(".json"));
		bOutTexture = !File.IsEmpty();
		if (File.IsEmpty())
		{
			File = ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("materials"), Path, TEXT(".json"));
		}
		if (File.IsEmpty() && bConvert && UE2NetModContent::EnsureConverted(Path, ClassName))
		{
			return FindJson(Path, ClassName, bOutTexture, false);
		}
		return ReadJson(File);
	}

	const FMatInfo& Resolve(FUSObject* Mat)
	{
		static FMatInfo None;
		if (!Mat)
		{
			return None;
		}
		FMatInfo& M = Materials.FindOrAdd(Mat);
		if (M.bResolved && (M.Texture.IsValid() || (M.Texture.IsExplicitlyNull() && FPlatformTime::Seconds() < M.RetryAt)))
		{
			return M;
		}
		M = FMatInfo();
		M.bResolved = true;
		M.RetryAt = FPlatformTime::Seconds() + 10.0;
		const FString Path = Mat->GetPathName();
		const FString ClassName = Mat->Class ? Mat->Class->GetName() : FString(TEXT("Texture"));
		bool bTexture = false;
		TSharedPtr<FJsonObject> J = FindJson(Path, ClassName, bTexture, true);
		if (!J.IsValid())
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: material %s (%s) is not in the content cache"), *Path, *ClassName);
			return M;
		}
		FString TexPath = Path;
		if (bTexture)
		{
			M.W = float(J->GetNumberField(TEXT("width")));
			M.H = float(J->GetNumberField(TEXT("height")));
		}
		else
		{
			if (!J->TryGetStringField(TEXT("base_texture"), TexPath) || TexPath.IsEmpty())
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: material %s (%s) has no texture"), *Path, *ClassName);
				return M;
			}
			const TArray<TSharedPtr<FJsonValue>>* Size = nullptr;
			if (J->TryGetArrayField(TEXT("base_texture_size"), Size) && Size->Num() == 2)
			{
				M.W = float((*Size)[0]->AsNumber());
				M.H = float((*Size)[1]->AsNumber());
			}
			const TSharedPtr<FJsonObject>* Props = nullptr;
			J->TryGetObjectField(TEXT("properties"), Props);
			FString Cls;
			J->TryGetStringField(TEXT("class"), Cls);
			// the chain from the material down to its texture: FinalBlend blending, TexRotator / TexScaler, Combiner pulses
			const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
			if (J->TryGetArrayField(TEXT("resolve_chain"), Chain))
			{
				for (const TSharedPtr<FJsonValue>& Link : *Chain)
				{
					const TSharedPtr<FJsonObject> L = Link->AsObject();
					FString LinkClass, LinkPath;
					if (!L.IsValid() || !L->TryGetStringField(TEXT("class"), LinkClass) || !L->TryGetStringField(TEXT("path"), LinkPath))
					{
						continue;
					}
					bool bLinkTex = false;
					const TSharedPtr<FJsonObject> LJ = LinkPath == Path ? J : FindJson(LinkPath, LinkClass, bLinkTex, false);
					const TSharedPtr<FJsonObject>* LP = nullptr;
					if (!LJ.IsValid() || !LJ->TryGetObjectField(TEXT("properties"), LP))
					{
						continue;
					}
					if (LinkClass == TEXT("FinalBlend") && M.FrameBlend < 0)
					{
						double FB = 2.0;
						(*LP)->TryGetNumberField(TEXT("FrameBufferBlending"), FB);
						M.FrameBlend = int32(FB);
					}
					else if (LinkClass == TEXT("TexRotator") && M.RotType < 0)
					{
						double Type = 0.0;
						(*LP)->TryGetNumberField(TEXT("TexRotationType"), Type);
						M.RotType = int32(Type);
						const TSharedPtr<FJsonObject>* Rot = nullptr;
						if ((*LP)->TryGetObjectField(TEXT("Rotation"), Rot))
						{
							M.RotYaw = float((*Rot)->GetNumberField(TEXT("Yaw")));
						}
					}
					else if (LinkClass == TEXT("TexScaler"))
					{
						double US = 1.0, VS = 1.0;
						(*LP)->TryGetNumberField(TEXT("UScale"), US);
						(*LP)->TryGetNumberField(TEXT("VScale"), VS);
						M.UScale = US != 0.0 ? float(US) : 1.f;
						M.VScale = VS != 0.0 ? float(VS) : 1.f;
					}
					else if (LinkClass == TEXT("Combiner") && M.FadePeriod <= 0.f)
					{
						// Material2 a FadeColor: the pulse
						FString M2;
						if ((*LP)->TryGetStringField(TEXT("Material2"), M2))
						{
							bool bT = false;
							const TSharedPtr<FJsonObject> FJ = FindJson(M2, TEXT("FadeColor"), bT, false);
							FString FC;
							const TSharedPtr<FJsonObject>* FP = nullptr;
							if (FJ.IsValid() && FJ->TryGetStringField(TEXT("class"), FC) && FC == TEXT("FadeColor") && FJ->TryGetObjectField(TEXT("properties"), FP))
							{
								double Period = 0.0;
								(*FP)->TryGetNumberField(TEXT("FadePeriod"), Period);
								M.FadePeriod = float(Period);
							}
						}
					}
				}
			}
		}
		M.Texture = ArenaRuntimeContent::LoadTextureByObjectPath(TexPath);
		if (M.Texture.IsValid() && M.W > 0.f)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: material %s (%s): %s %.0f x %.0f%s"), *Path, *ClassName, *TexPath, M.W, M.H,
				M.FrameBlend >= 0 ? *FString::Printf(TEXT(", blending %d"), M.FrameBlend) : TEXT(""));
		}
		else
		{
			++MissingTextures;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: texture %s of %s could not be loaded"), *TexPath, *Path);
		}
		return M;
	}

	UFont* FontFor(FUSObject* Font)
	{
		if (!Font)
		{
			// UE2 logs "DrawText: No font" and draws nothing; the HUD always sets one
			return ArenaHudFonts::Get(7);
		}
		if (const TPair<TWeakObjectPtr<UFont>, int32>* Hit = Fonts.Find(Font))
		{
			if (UFont* Cached = Hit->Key.Get())
			{
				return Cached;
			}
			Fonts.Remove(Font);
		}
		const FString Path = Font->GetPathName();
		UFont* F = ArenaRuntimeContent::LoadFontByObjectPath(Path);
		if (!F && UE2NetModContent::EnsureConverted(Path, TEXT("Font")))
		{
			F = ArenaRuntimeContent::LoadFontByObjectPath(Path);
		}
		int32 Height = ArenaRuntimeContent::GetFontCellHeight(Path);
		if (!F)
		{
			++MissingFonts;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: font %s is not in the content cache (a HUD font is used)"), *Path);
			F = ArenaHudFonts::Get(6);
			Height = ArenaHudFonts::NominalHeight(6);
		}
		Fonts.Add(Font, TPair<TWeakObjectPtr<UFont>, int32>(F, Height));
		return F;
	}

	int32 FontHeight(FUSObject* Font, UFont* F)
	{
		const TPair<TWeakObjectPtr<UFont>, int32>* Hit = Font ? Fonts.Find(Font) : nullptr;
		if (Hit && Hit->Value > 0)
		{
			return Hit->Value;
		}
		return F ? FMath::RoundToInt(F->GetMaxCharHeight()) : 0;
	}
};

// =================================================================================================
// FImpl
// =================================================================================================

struct FUE2NetScriptHUD::FImpl
{
	FUE2NetConnection& Connection;
	TWeakObjectPtr<UWorld> World;
	TUniquePtr<FUE2NetMirrorWorld> Script;
	FUE2NetCanvasBackend Backend;
	bool bReady = false;
	/** -UE2ScriptHUD: 1 mod classes (default), 2 stock classes too, 0 off */
	int32 Mode = 1;

	TMap<int32, FUSObject*> Mirrors;
	/** Channel -> (owner channel, property key) of references to a channel not open yet. */
	TMultiMap<int32, TPair<int32, FString>> PendingRefs;
	TMap<uint64, FUSObject*> StaticRefs;
	FUSObject* Level = nullptr;
	FUSObject* CanvasObj = nullptr;
	FUSObject* PlayerObj = nullptr;
	FUSObject* InteractionMaster = nullptr;
	FUSObject* GRI = nullptr;
	int32 PCChannel = 0;
	bool bPCInitialised = false;
	bool bPRIsDirty = true;
	/** ClientSetHUD(HUD class, scoreboard class) waiting for the controller's mirror. */
	TArray<TPair<FName, FUE2NetValue>> PendingSetHUD;
	bool bHaveSetHUD = false;
	FString HudClassName, ScoreboardClassName;
	bool bModHud = false, bModScoreboard = false;
	/** Disabled after failures (script errors, nothing drawn, too slow): our HUD draws that element. */
	bool bHudFailed = false, bScoreboardFailed = false;
	int32 HudEmptyFrames = 0, HudErrors = 0, ScoreErrors = 0, ScoreEmptyFrames = 0;
	int32 Mirrored = 0, Skipped = 0, Denied = 0, MapCopies = 0;
	int32 HudFrames = 0, ScoreFrames = 0;
	double HudSeconds = 0.0, ScoreSeconds = 0.0, TickSeconds = 0.0;
	uint64 HudTokens = 0;
	double NextLog = 0.0;
	bool bLogFrames = false;
	float TimeSeconds = 0.f;

	// ---- mods' client-side code (Docs/UE2Net.md "Mod client-side code": phase 2) ----
	/** -UE2ScriptClient=0: the mirror stays data only (mods' simulated code is not run). */
	bool bClientCode = true;
	/** A mod actor running its client code: a mirrored (replicated) one, or one its code spawned on our side. */
	struct FClientRun
	{
		int32 Errors = 0;
		bool bOff = false;
		bool bMirrored = false;
		bool bReported = false;
	};
	TMap<FUSObject*, FClientRun> ClientRuns;
	/** Mirror -> its channel (every mirrored actor). */
	TMap<FUSObject*, int32> ChannelOf;
	TUniquePtr<FUE2NetScriptVisuals> Visuals;
	int32 ClientStarted = 0, ClientSpawned = 0, ClientStopped = 0;
	double NextSummary = FPlatformTime::Seconds() + 30.0;
	double ClientSeconds = 0.0;

	// ---- mods' menus (UScript/UScriptGUI.h; Docs/UE2Net.md "Mod menus") ----
	/** -UE2ScriptMenus=0: Player.GUIController stays None (mods' menus do not open). */
	bool bMenus = true;
	TUniquePtr<FUSGUI> Gui;
	/** Keys a mod's code bound for itself (UTComp: "set input F5 MyMenu"): key name (lower case) -> console command. */
	TMap<FString, FString> ModBinds;
	static constexpr const TCHAR* ModBindsSection = TEXT("OpenUT2k4.ModBinds");

	FImpl(FUE2NetConnection& InConnection, UWorld* InWorld) : Connection(InConnection), World(InWorld)
	{
		FString M;
		if (FParse::Value(FCommandLine::Get(), TEXT("UE2ScriptHUD="), M))
		{
			Mode = M.Equals(TEXT("all"), ESearchCase::IgnoreCase) ? 2 : (M == TEXT("0") || M.Equals(TEXT("off"), ESearchCase::IgnoreCase) ? 0 : 1);
		}
		bLogFrames = FParse::Param(FCommandLine::Get(), TEXT("UE2ScriptHUDLog"));
		{
			FString Menus;
			if (FParse::Value(FCommandLine::Get(), TEXT("UE2ScriptMenus="), Menus))
			{
				bMenus = !(Menus == TEXT("0") || Menus.Equals(TEXT("off"), ESearchCase::IgnoreCase));
			}
		}
		if (Mode == 0 || !ArenaUScript::EnsureRuntime())
		{
			return;
		}
		FUSRuntime& RT = FUSRuntime::Get();
		FUSClass* LevelClass = RT.FindClass(TEXT("Engine.LevelInfo"));
		FUSClass* CanvasClass = RT.FindClass(TEXT("Engine.Canvas"));
		// a Viewport, as the local player's is (Pawn.IsLocallyControlled: Viewport(PlayerController(Controller).Player) != None)
		FUSClass* PlayerClass = RT.FindClass(TEXT("Engine.Viewport"));
		PlayerClass = PlayerClass ? PlayerClass : RT.FindClass(TEXT("Engine.Player"));
		if (!LevelClass || !CanvasClass || !RT.ActorClass)
		{
			UE_LOG(LogUE2Net, Warning, TEXT("UE2NetScriptHUD: the stock Engine classes are missing: server HUDs are not run"));
			return;
		}
		USProps().Resolve();
		Script = MakeUnique<FUE2NetMirrorWorld>();
		Level = Script->ConstructObject(LevelClass, nullptr, FName(TEXT("LevelInfo0")));
		Level->Flags |= USF_Actor;
		Script->Level = Level;
		USProps().Level.Set<FUSObject*>(Level, Level);
		USProps().NetMode.Set<uint8>(Level, UE2_NM_Client);
		if (FUSProperty* P = Level->FindProperty(TEXT("TimeDilation")))
		{
			*reinterpret_cast<float*>(Level->Data + P->Offset) = 1.1f;
		}
		CanvasObj = Script->ConstructObject(CanvasClass, nullptr, FName(TEXT("Canvas0")));
		PlayerObj = PlayerClass ? Script->ConstructObject(PlayerClass, nullptr, FName(TEXT("Viewport0"))) : nullptr;
		if (FUSClass* IMClass = RT.FindClass(TEXT("Engine.InteractionMaster")); IMClass && PlayerObj)
		{
			InteractionMaster = Script->ConstructObject(IMClass, nullptr, FName(TEXT("InteractionMaster0")));
			SetObj(PlayerObj, TEXT("InteractionMaster"), InteractionMaster);
		}
		InitCanvasFonts();
		{
			FString C;
			if (FParse::Value(FCommandLine::Get(), TEXT("UE2ScriptClient="), C))
			{
				bClientCode = !(C == TEXT("0") || C.Equals(TEXT("off"), ESearchCase::IgnoreCase));
			}
		}
		Visuals = MakeUnique<FUE2NetScriptVisuals>(InWorld);
		Script->PresentationWorld = InWorld;
		Script->OnClientSpawn = [this](FUSObject* A)
		{
			ClientRuns.Add(A);
			++ClientSpawned;
			// UE2 gives each Emitter its own ParticleEmitters (script changes them while it plays: UpdateChargeBeam)
			if (ChainHas(A->Class, TEXT("Emitter")))
			{
				InstanceSubobjects(A, TEXT("Emitters"));
			}
		};
		Script->PresentationLocation = [this](FUSObject* A, FVector& Out)
		{
			if (USceneComponent* C = ComponentOf(A))
			{
				Out = C->GetComponentLocation();
				return true;
			}
			if (AActor* V = Visuals ? Visuals->Find(A) : nullptr)
			{
				Out = V->GetActorLocation();
				return true;
			}
			return false;
		};
		Script->PresentationBone = [this](FUSObject* A, FName Bone, FTransform& Out)
		{
			USceneComponent* C = ComponentOf(A);
			if (!C && Visuals)
			{
				if (AUE2NetScriptVisual* V = Cast<AUE2NetScriptVisual>(Visuals->Find(A)))
				{
					C = V->Skeletal ? static_cast<USceneComponent*>(V->Skeletal) : (V->Static ? static_cast<USceneComponent*>(V->Static) : V->Root.Get());
				}
			}
			if (!C)
			{
				return false;
			}
			Out = !Bone.IsNone() && C->DoesSocketExist(Bone) ? C->GetSocketTransform(Bone) : C->GetComponentTransform();
			return true;
		};
		Script->OnDestroyed = [this](FUSObject* A) { Forget(A); };
		Script->RemoteFilter = [this](FUSObject* A, FUSFunction* F) { return IsServerCall(A, F); };
		Script->RemoteSend = [this](FUSObject* A, FUSFunction* F, uint8* Parms) { SendServerCall(A, F, Parms); };
		RT.PlaceholderInit = [this](FUSObject* Obj) { Backend.InitPlaceholder(Obj); };
		RT.ConsoleCommandHook = [this](FUSObject* Self, const FString& Cmd, FString& Out) { return ScriptConsoleCommand(Self, Cmd, Out); };
		ModBinds = RT.GetUserConfigSection(ModBindsSection);
		RT.ContentObjectExists = [](const FString& Path, const FString& ClassName)
		{
			const TCHAR* Kind = ClassName == TEXT("Font") ? TEXT("fonts") : (ClassName == TEXT("Sound") ? TEXT("sounds") : nullptr);
			if (Kind)
			{
				return !ArenaRuntimeContent::FindCacheFileByObjectPath(Kind, Path, TEXT(".json")).IsEmpty();
			}
			return !ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("textures"), Path, TEXT(".json")).IsEmpty()
				|| !ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("materials"), Path, TEXT(".json")).IsEmpty();
		};
		// LevelInfo.Title (not replicated; the scoreboards show it): the converted map's title, else its name
		{
			FString Title = Connection.GetMapName();
			if (const TSharedPtr<FJsonObject> MapJson = ReadJson(ArenaRuntimeMap::GetMapDir(Connection.GetMapName()) / TEXT("map.json")))
			{
				FString T;
				const TSharedPtr<FJsonObject>* LI = nullptr;
				if (MapJson->TryGetObjectField(TEXT("level_info"), LI) && (*LI)->TryGetStringField(TEXT("title"), T) && !T.IsEmpty())
				{
					Title = T;
				}
			}
			if (FUSProperty* P = Level->FindProperty(TEXT("Title")))
			{
				*reinterpret_cast<FString*>(Level->Data + P->Offset) = Title;
			}
		}
		bReady = true;
	}

	~FImpl()
	{
		USCanvas::SetBackend(nullptr);
		if (bReady)
		{
			FUSRuntime::Get().PlaceholderInit = nullptr;
			FUSRuntime::Get().ContentObjectExists = nullptr;
			FUSRuntime::Get().ConsoleCommandHook = nullptr;
			Gui.Reset();
			ClearDefaultsRefs();
		}
		Script.Reset();
	}

	/**
	 * Script code may keep this world's objects in class defaults (default.X = ...; HUD fonts are placeholders, but a mod
	 * can store its settings object or an actor): those references are cleared before the world goes, like UE2 garbage
	 * collection would.
	 */
	void ClearDefaultsRefs()
	{
		for (FUSObject* O : Script->GetObjects())
		{
			O->Flags |= USF_PendingKill;
		}
		for (const TPair<FName, FUSLinker*>& KV : FUSRuntime::Get().Linkers)
		{
			FUSLinker* L = KV.Value;
			for (int32 Ei = 0; L && Ei < L->NumExports(); ++Ei)
			{
				FUSClass* C = L->IsTopLevelClass(Ei) ? static_cast<FUSClass*>(L->GetExport(Ei)) : nullptr;
				if (!C || !C->IsLoaded() || C->Defaults.Num() < C->PropertiesSize)
				{
					continue;
				}
				for (FUSProperty* P : C->RefProps)
				{
					for (int32 i = 0; i < P->ArrayDim; ++i)
					{
						P->CleanupRefs(C->Defaults.GetData() + P->Offset + i * P->ElementSize);
					}
				}
			}
		}
	}

	// ---- properties --------------------------------------------------------------------------------

	static void SetObj(FUSObject* O, const TCHAR* Name, FUSObject* V)
	{
		if (FUSProperty* P = O ? O->FindProperty(FName(Name)) : nullptr)
		{
			if ((P->Type == EUSType::Object || P->Type == EUSType::Class) && O->Data)
			{
				*reinterpret_cast<FUSObject**>(O->Data + P->Offset) = V;
			}
		}
	}

	static FUSObject* GetObj(FUSObject* O, const TCHAR* Name)
	{
		FUSProperty* P = O ? O->FindProperty(FName(Name)) : nullptr;
		return (P && (P->Type == EUSType::Object || P->Type == EUSType::Class) && O->Data) ? *reinterpret_cast<FUSObject**>(O->Data + P->Offset) : nullptr;
	}

	void InitCanvasFonts()
	{
		// UCanvas::Init: TinyFont / SmallFont / MedFont from the localized names; Font starts as the small font
		for (const TCHAR* Key : { TEXT("TinyFont"), TEXT("SmallFont"), TEXT("MedFont") })
		{
			FString Name;
			const FString NameKey = FString(Key) + TEXT("Name");
			if (FUSProperty* NP = CanvasObj->FindProperty(FName(*NameKey)))
			{
				Name = *reinterpret_cast<FString*>(CanvasObj->Data + NP->Offset);
			}
			if (!Name.IsEmpty())
			{
				SetObj(CanvasObj, Key, FUSRuntime::Get().FindObject(Name));
			}
		}
		if (!GetObj(CanvasObj, TEXT("SmallFont")))
		{
			SetObj(CanvasObj, TEXT("SmallFont"), FUSRuntime::Get().FindObject(TEXT("UT2003Fonts.FontEurostile9")));
		}
	}

	FUSObject* ResolveRef(const FUE2ObjectRef& R, int32 OwnerChannel, const FString& Key)
	{
		if (R.IsDynamic())
		{
			if (FUSObject* const* Hit = Mirrors.Find(R.Channel))
			{
				return *Hit;
			}
			if (Connection.GetActors().Contains(R.Channel) || OwnerChannel)
			{
				PendingRefs.Add(R.Channel, TPair<int32, FString>(OwnerChannel, Key));
			}
			return nullptr;
		}
		if (!R.IsStatic())
		{
			return nullptr;
		}
		const uint64 CacheKey = (uint64(uint32(R.PackageIndex)) << 32) | uint32(R.ExportIndex);
		if (FUSObject* const* Hit = StaticRefs.Find(CacheKey))
		{
			return *Hit;
		}
		FUE2PackageMap& PM = Connection.GetPackageMap();
		const FString Path = PM.GetObjectPath(R);
		const FString ClassName = PM.GetObjectClassName(R);
		FUSObject* Result = nullptr;
		if (ClassName == TEXT("Class"))
		{
			Result = PM.GetClass(R);
		}
		else
		{
			// a map actor that has a channel (bNoDelete actors placed in the map)
			for (const TPair<int32, FUSObject*>& KV : Mirrors)
			{
				const FUE2NetActor* A = Connection.GetActors().Find(KV.Key);
				if (A && A->bStatic && A->Archetype == R)
				{
					Result = KV.Value;
					break;
				}
			}
			FString Package;
			Path.Split(TEXT("."), &Package, nullptr);
			if (!Result && !Path.IsEmpty() && Package.Equals(Connection.GetMapName(), ESearchCase::IgnoreCase))
			{
				// a map actor without a channel (PRI.PlayerZone / PlayerLocation ...): a data copy of it, with its saved
				// properties from the map package (LocationName ...)
				Result = MapActorCopy(R);
			}
			else if (!Result && !Path.IsEmpty())
			{
				// textures, fonts, sounds, meshes ...: a placeholder of the right class (the same object script constants name)
				FUSRuntime& RT = FUSRuntime::Get();
				FUSClass* C = RT.FindLoadedClass(FName(*ClassName));
				Result = RT.GetPlaceholder(Path, C ? C : RT.GetIntrinsicClass(FName(*ClassName)));
			}
		}
		StaticRefs.Add(CacheKey, Result);
		return Result;
	}

	FUSObject* MapActorCopy(const FUE2ObjectRef& R)
	{
		const TArray<FUE2PackageInfo>& List = Connection.GetPackageMap().GetList();
		const FString File = List.IsValidIndex(R.PackageIndex) ? List[R.PackageIndex].LocalPath : FString();
		FUSLinker* L = File.IsEmpty() ? nullptr : FUSRuntime::Get().LoadPackageFile(File);
		FUSClass* C = L ? L->GetExportClass(R.ExportIndex) : nullptr;
		if (!C || !C->EnsureLoaded() || !RTActor() || !C->IsChildOf(RTActor()) || IsDeniedPackage(C->GetPackageName().ToString()))
		{
			return nullptr;
		}
		FUSObject* O = Script->ConstructObject(C, Level, FName(*L->GetExportName(R.ExportIndex)));
		if (O)
		{
			L->LoadInstanceProperties(R.ExportIndex, O);
			USProps().Level.Set<FUSObject*>(O, Level);
			++MapCopies;
		}
		return O;
	}

	bool WriteValue(FUSProperty* P, uint8* Addr, const FUE2NetValue& V, int32 OwnerChannel, const FString& Key)
	{
		using EK = FUE2NetValue::EKind;
		switch (P->Type)
		{
		case EUSType::Byte: *Addr = uint8(V.Kind == EK::Float ? int32(V.F) : V.I); return true;
		case EUSType::Int: *reinterpret_cast<int32*>(Addr) = V.Kind == EK::Float ? int32(V.F) : V.I; return true;
		case EUSType::Bool: *reinterpret_cast<uint32*>(Addr) = V.I != 0 ? 1u : 0u; return true;
		case EUSType::Float: *reinterpret_cast<float*>(Addr) = V.Kind == EK::Float ? V.F : float(V.I); return true;
		case EUSType::Name: *reinterpret_cast<FName*>(Addr) = V.Kind == EK::Name ? V.N : FName(*V.S); return true;
		case EUSType::Str: *reinterpret_cast<FString*>(Addr) = V.S; return true;
		case EUSType::Object:
		case EUSType::Class:
			*reinterpret_cast<FUSObject**>(Addr) = ResolveRef(V.Obj, OwnerChannel, Key);
			return true;
		case EUSType::Struct:
		{
			if (V.Kind == EK::Vector && P->ElementSize >= int32(sizeof(FUSVector)))
			{
				*reinterpret_cast<FUSVector*>(Addr) = FUSVector(V.V.X, V.V.Y, V.V.Z);
				return true;
			}
			if (V.Kind == EK::Rotator && P->ElementSize >= int32(sizeof(FUSRotator)))
			{
				*reinterpret_cast<FUSRotator*>(Addr) = FUSRotator(V.R.X, V.R.Y, V.R.Z);
				return true;
			}
			if (V.Kind == EK::Plane && P->ElementSize >= int32(sizeof(FUSPlane)))
			{
				FUSPlane& Pl = *reinterpret_cast<FUSPlane*>(Addr);
				Pl.X = V.P.X;
				Pl.Y = V.P.Y;
				Pl.Z = V.P.Z;
				Pl.W = V.P.W;
				return true;
			}
			if (V.Kind == EK::Struct && P->Struct)
			{
				for (const TPair<FName, FUE2NetValue>& M : V.Members)
				{
					FString Name;
					int32 Index = 0;
					SplitKey(M.Key.ToString(), Name, Index);
					FUSProperty* MP = P->Struct->FindPropertyByName(FName(*Name));
					if (MP && Index >= 0 && Index < MP->ArrayDim)
					{
						WriteValue(MP, Addr + MP->Offset + Index * MP->ElementSize, M.Value, 0, FString());
					}
				}
				return true;
			}
			return false;
		}
		default:
			return false;
		}
	}

	void WriteKey(FUSObject* O, const FUE2NetActor& A, const FString& Key)
	{
		const FUE2NetValue* V = A.Properties.Find(Key);
		if (!V || !O->Data)
		{
			return;
		}
		FString Name;
		int32 Index = 0;
		SplitKey(Key, Name, Index);
		// UE2's server swaps Role and RemoteRole before replicating an actor (UActorChannel::ReplicateActor), so the wire
		// already holds the client's roles: our pawn and controller ROLE_AutonomousProxy, the rest ROLE_SimulatedProxy
		if (Name == TEXT("Role") || Name == TEXT("RemoteRole"))
		{
			(Name == TEXT("Role") ? USProps().Role : USProps().RemoteRole).Set<uint8>(O, uint8(V->I));
			return;
		}
		FUSProperty* P = O->FindProperty(FName(*Name));
		if (!P || Index < 0 || Index >= P->ArrayDim)
		{
			return;
		}
		WriteValue(P, O->Data + P->Offset + Index * P->ElementSize, *V, A.Channel, Key);
	}

	// ---- mirror ------------------------------------------------------------------------------------

	void Open(const FUE2NetActor& A)
	{
		if (!bReady || A.bBroken || !A.Class || Mirrors.Contains(A.Channel))
		{
			return;
		}
		if (IsDeniedPackage(A.Class->GetPackageName().ToString()))
		{
			++Denied;
			return;
		}
		if (!A.Class->EnsureLoaded() || !RTActor() || !A.Class->IsChildOf(RTActor()))
		{
			++Skipped;
			return;
		}
		FUSObject* O = nullptr;
		if (ChainHas(A.Class, TEXT("LevelInfo")))
		{
			O = Level; // the level's replicated values go into our Level
		}
		else
		{
			O = Script->ConstructObject(A.Class, Level, NAME_None);
		}
		if (!O)
		{
			++Skipped;
			return;
		}
		++Mirrored;
		Mirrors.Add(A.Channel, O);
		FUSEngineProps& P = USProps();
		if (O != Level)
		{
			P.Level.Set<FUSObject*>(O, Level);
			P.Role.Set<uint8>(O, UE2_ROLE_SimulatedProxy);
			P.RemoteRole.Set<uint8>(O, UE2_ROLE_Authority);
			if (!A.bStatic)
			{
				P.Location.Set<FUSVector>(O, FUSVector(A.SpawnLocation.X, A.SpawnLocation.Y, A.SpawnLocation.Z));
				P.Rotation.Set<FUSRotator>(O, FUSRotator(A.SpawnRotation.X, A.SpawnRotation.Y, A.SpawnRotation.Z));
			}
		}
		for (const TPair<FString, FUE2NetValue>& KV : A.Properties)
		{
			WriteKey(O, A, KV.Key);
		}
		// references to this channel that arrived before it opened
		TArray<TPair<int32, FString>> Waiting;
		PendingRefs.MultiFind(A.Channel, Waiting);
		PendingRefs.Remove(A.Channel);
		for (const TPair<int32, FString>& W : Waiting)
		{
			FUSObject* const* Owner = Mirrors.Find(W.Key);
			const FUE2NetActor* OA = Connection.GetActors().Find(W.Key);
			if (Owner && OA)
			{
				WriteKey(*Owner, *OA, W.Value);
			}
		}
		if (ChainHas(A.Class, TEXT("Weapon")))
		{
			// Weapon.InitWeaponFires (native, run by PostBeginPlay on every client): the fire mode objects, which the HUD
			// asks for the ammo class (Weapon.GetAmmoClass) and the charge bar
			InitWeaponFires(O);
		}
		if (ChainHas(A.Class, TEXT("GameReplicationInfo")))
		{
			// GameReplicationInfo.PostBeginPlay: Level.GRI; PostNetBeginPlay: the local controller's GameReplicationInfo
			GRI = O;
			P.GRI.Set<FUSObject*>(Level, O);
			// GameReplicationInfo.PostBeginPlay: SetTimer(Level.TimeDilation, true); its client Timer counts ElapsedTime and
			// RemainingTime (replicated once), which scoreboards show
			P.TimerRate.Set<float>(O, 1.1f);
			P.bTimerLoop.Set<uint32>(O, 1u);
			P.TimerCounter.Set<float>(O, 0.f);
			Script->TimerOnly.AddUnique(O);
		}
		if (ChainHas(A.Class, TEXT("PlayerReplicationInfo")))
		{
			bPRIsDirty = true;
		}
		ChannelOf.Add(O, A.Channel);
		if (bClientCode && O != Level && ShouldRunClient(A.Class))
		{
			// what a UE2 client runs for a new replicated actor: SpawnActor's events (simulated ones only, the role rule
			// in the VM), then PostNetBeginPlay once the initial properties are in
			ClientRuns.Add(O).bMirrored = true;
			++ClientStarted;
			SyncClientLocations();
			for (const TCHAR* E : { TEXT("Spawned"), TEXT("PreBeginPlay"), TEXT("BeginPlay"), TEXT("PostBeginPlay"), TEXT("SetInitialState"), TEXT("PostNetBeginPlay") })
			{
				CallClientEvent(O, E);
			}
		}
		CheckPlayerController();
	}

	// ---- mods' client-side code -----------------------------------------------------------------------

	/**
	 * Mod actors that run their client code: what a UE2 client runs of a replicated actor (simulated functions, its
	 * Tick / Timer / simulated state code) for classes of a mod's package (not UT2004's own: our native code shows those),
	 * except Info (replication infos, mutators: their client parts are done in UE2NetModClient / the HUD), controllers and
	 * HUDs (run by the script HUD).
	 */
	static bool ShouldRunClient(FUSClass* C)
	{
		const FString Pkg = C ? C->GetPackageName().ToString() : FString();
		return C && !IsStockPackage(Pkg) && !IsDeniedPackage(Pkg) && !ChainHas(C, TEXT("Info")) && !ChainHas(C, TEXT("Controller")) && !ChainHas(C, TEXT("HUD"));
	}

	/** The native component that shows a mirrored actor (its channel's), else null. */
	USceneComponent* ComponentOf(FUSObject* O) const
	{
		const int32* Ch = O ? ChannelOf.Find(O) : nullptr;
		FUE2NetWorld* NW = Ch ? FUE2NetWorld::Get() : nullptr;
		return NW ? NW->GetChannelComponent(*Ch) : nullptr;
	}

	/**
	 * Runs Fn for a client-code actor within its budget: an actor whose code errs three times (script errors, a million
	 * tokens in one call) stops running (UE2's data mirror and our native parts stay; the per-element fallback).
	 */
	bool RunGuarded(FUSObject* O, TFunctionRef<void()> Fn)
	{
		FClientRun* R = ClientRuns.Find(O);
		if (!R || R->bOff || !O || O->IsPendingKill())
		{
			return false;
		}
		const uint64 Tokens = USVM::GStatTokens, Aborts = USVM::GStatAborts;
		const double T0 = FPlatformTime::Seconds();
		const bool bWasClient = Script->bClientScope;
		Script->bClientScope = true;
		Fn();
		Script->bClientScope = bWasClient;
		ClientSeconds += FPlatformTime::Seconds() - T0;
		R = ClientRuns.Find(O);
		if (!R)
		{
			return true;
		}
		R->Errors += USVM::GStatAborts != Aborts ? 1 : 0;
		R->Errors += USVM::GStatTokens - Tokens > 1000000 ? 3 : 0;
		if (R->Errors >= 3 && !R->bOff)
		{
			R->bOff = true;
			++ClientStopped;
			UE_LOG(LogUE2Net, Warning, TEXT("UE2NetScriptHUD: the client code of %s (%s) stopped after %d script errors or overlong calls: its data mirror and our native parts stay"),
				*O->GetName(), *O->Class->GetPathName(), R->Errors);
		}
		return true;
	}

	void CallClientEvent(FUSObject* O, const TCHAR* Name)
	{
		RunGuarded(O, [&]() { FUE2NetMirrorWorld::CallEvent(O, Name); });
	}

	/** Forgets client-code actors that are gone (before the world frees them). */
	/**
	 * A mirrored actor goes (script destroyed it, its channel closed): every map that keeps it by pointer lets go before
	 * CleanupDestroyed frees it. A destroyed replicated actor is detached from its channel, as UE2 detaches torn off and
	 * temporary actors; the channel's close later finds nothing.
	 */
	void Forget(FUSObject* O)
	{
		if (!O)
		{
			return;
		}
		if (const int32* Ch = ChannelOf.Find(O))
		{
			FUSObject* const* M = Mirrors.Find(*Ch);
			if (M && *M == O)
			{
				Mirrors.Remove(*Ch);
				if (*Ch == PCChannel)
				{
					PCChannel = 0;
					bPCInitialised = false;
					Script->LocalPC = nullptr;
				}
			}
			ChannelOf.Remove(O);
		}
		if (O == GRI)
		{
			GRI = nullptr;
			USProps().GRI.Set<FUSObject*>(Level, nullptr);
		}
		for (auto It = StaticRefs.CreateIterator(); It; ++It)
		{
			if (It.Value() == O)
			{
				It.RemoveCurrent();
			}
		}
		Script->TimerOnly.Remove(O);
		Script->LocalActors.Remove(O);
		ClientRuns.Remove(O);
		bPRIsDirty = true;
	}

	/** Before the world frees its pending kill objects: nothing of ours may point at them. */
	void ForgetDestroyed()
	{
		TArray<FUSObject*> Dead;
		for (const TPair<int32, FUSObject*>& KV : Mirrors)
		{
			if (KV.Value && KV.Value->IsPendingKill())
			{
				Dead.Add(KV.Value);
			}
		}
		for (const TPair<FUSObject*, int32>& KV : ChannelOf)
		{
			if (KV.Key && KV.Key->IsPendingKill())
			{
				Dead.AddUnique(KV.Key);
			}
		}
		for (FUSObject* O : Dead)
		{
			Forget(O);
		}
		PurgeClientRuns();
	}

	void PurgeClientRuns()
	{
		for (auto It = ClientRuns.CreateIterator(); It; ++It)
		{
			if (!It.Key() || It.Key()->IsPendingKill())
			{
				It.RemoveCurrent();
			}
		}
	}

	/** UE2 moves the replicated actors itself: their mirrors get where ours show them. */
	void SyncClientLocations()
	{
		for (const TPair<FUSObject*, FClientRun>& KV : ClientRuns)
		{
			if (!KV.Value.bMirrored || KV.Value.bOff)
			{
				continue;
			}
			USceneComponent* C = ComponentOf(KV.Key);
			if (!C)
			{
				continue;
			}
			AActor* Owner = C->GetOwner();
			// a pawn's or vehicle's own place (not its mesh's); a gun's component
			const bool bActorPlace = Owner && (C == Owner->GetRootComponent() || (C->IsA<USkeletalMeshComponent>() && Cast<ACharacter>(Owner)));
			const FVector L = bActorPlace ? Owner->GetActorLocation() : C->GetComponentLocation();
			const FRotator R = bActorPlace ? Owner->GetActorRotation() : C->GetComponentRotation();
			USProps().Location.Set<FUSVector>(KV.Key, USUnits::ToUE2(L));
			USProps().Rotation.Set<FUSRotator>(KV.Key, USUnits::ToUE2(R));
		}
	}

	// ---- calls to the server (UE2's replicated functions) ----
	TMap<FName, int32> ServerCallsSent;

	/**
	 * A replicated function client script calls on an actor our connection owns (our controller, our pawn, our weapons and
	 * the rest of our inventory: their Owner chain reaches our controller) goes to the server, as UE2's
	 * ProcessRemoteFunction sends it; the server-to-client ones (ClientXxx by UE2's naming, and any with a return value)
	 * run here. UE2 decides by the function's replication condition; the naming stands in for it.
	 */
	/**
	 * UE2 AActor::ProcessRemoteFunction on a client: a replicated function goes to the server when the actor belongs to our
	 * controller (its owner chain reaches it: GetTopPlayerController) and the function's replication condition holds here,
	 * with our roles (`reliable if (Role < ROLE_Authority) ServerStartFire` holds; `reliable if (Role == ROLE_Authority)
	 * ClientSetHUD, ReceiveNewAim, StopZoom` does not: those run here). A call the server just sent us is never sent back.
	 */
	bool IsServerCall(FUSObject* A, FUSFunction* F) const
	{
		if (!bClientCode || !A || !F || F->ReturnProperty || !ChannelOf.Contains(A))
		{
			return false;
		}
		if (A == DeliveringActor && DeliveringFunction && F->Name == DeliveringFunction->Name)
		{
			return false;
		}
		FUSObject* PC = Script ? Script->GetLocalPlayerController() : nullptr;
		bool bOurs = false;
		int32 Guard = 0;
		for (FUSObject* O = A; O && Guard < 8 && !bOurs; O = USProps().Owner.Get<FUSObject*>(O), ++Guard)
		{
			bOurs = O == PC;
		}
		return bOurs && USVM::EvalNetCondition(A, F);
	}

	/** The server's call being run here (RunClientFunction / RunMenuFunction): never sent back. */
	FUSObject* DeliveringActor = nullptr;
	FUSFunction* DeliveringFunction = nullptr;

	void SendServerCall(FUSObject* A, FUSFunction* F, uint8* Parms)
	{
		const int32* Ch = ChannelOf.Find(A);
		if (!Ch)
		{
			return;
		}
		TArray<FUE2NetValue> Values;
		for (FUSProperty* P : F->Parms)
		{
			if (!P || P == F->ReturnProperty)
			{
				continue;
			}
			const uint8* Addr = Parms + P->Offset;
			FUE2NetValue V;
			switch (P->Type)
			{
			case EUSType::Byte: V.Kind = FUE2NetValue::EKind::Byte; V.I = *Addr; break;
			case EUSType::Int: V.Kind = FUE2NetValue::EKind::Int; V.I = *reinterpret_cast<const int32*>(Addr); break;
			case EUSType::Bool: V.Kind = FUE2NetValue::EKind::Bool; V.I = *reinterpret_cast<const uint32*>(Addr) ? 1 : 0; break;
			case EUSType::Float: V.Kind = FUE2NetValue::EKind::Float; V.F = *reinterpret_cast<const float*>(Addr); break;
			case EUSType::Name: V.Kind = FUE2NetValue::EKind::Name; V.N = *reinterpret_cast<const FName*>(Addr); break;
			case EUSType::Str: V.Kind = FUE2NetValue::EKind::Str; V.S = *reinterpret_cast<const FString*>(Addr); break;
			case EUSType::Object:
			case EUSType::Class:
			{
				FUSObject* O = *reinterpret_cast<FUSObject* const*>(Addr);
				const int32* OC = O ? ChannelOf.Find(O) : nullptr;
				V.Kind = FUE2NetValue::EKind::Object;
				V.Obj.Channel = OC ? *OC : 0;
				break;
			}
			case EUSType::Struct:
			{
				const FString SN = P->Struct ? P->Struct->Name.ToString() : FString();
				if (SN == TEXT("Vector"))
				{
					const FUSVector& Vec = *reinterpret_cast<const FUSVector*>(Addr);
					V.Kind = FUE2NetValue::EKind::Vector;
					V.V = FVector3f(Vec.X, Vec.Y, Vec.Z);
				}
				else if (SN == TEXT("Rotator"))
				{
					const FUSRotator& Rot = *reinterpret_cast<const FUSRotator*>(Addr);
					V.Kind = FUE2NetValue::EKind::Rotator;
					V.R = FIntVector(Rot.Pitch, Rot.Yaw, Rot.Roll);
				}
				break; // other structs: not sent by our encoder (None)
			}
			default:
				break;
			}
			Values.Add(V);
		}
		Connection.SendFunction(*Ch, F->Name, Values);
		int32& N = ServerCallsSent.FindOrAdd(F->Name);
		if (N++ == 0)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: client script calls %s on the server (%s, %d parameters)"), *F->Name.ToString(), *A->GetName(), Values.Num());
		}
	}

	/**
	 * The objects of an actor's subobject array (inline "Begin Object" defaults, e.g. an Emitter's ParticleEmitters) as
	 * copies of its own, with their saved properties: the class defaults hold shared shells.
	 */
	void InstanceSubobjects(FUSObject* A, const TCHAR* ArrayName)
	{
		FUSProperty* P = A ? A->FindProperty(FName(ArrayName)) : nullptr;
		if (!P || P->Type != EUSType::Array || !P->Inner || P->Inner->Type != EUSType::Object)
		{
			return;
		}
		FUSScriptArray& Arr = *reinterpret_cast<FUSScriptArray*>(A->Data + P->Offset);
		FUSRuntime& RT = FUSRuntime::Get();
		for (int32 i = 0; i < Arr.Num && Arr.Data; ++i)
		{
			FUSObject*& E = *reinterpret_cast<FUSObject**>(Arr.Data + i * P->Inner->ElementSize);
			if (!E || !E->Class || E->World == Script.Get())
			{
				continue;
			}
			FUSObject* Copy = Script->ConstructObject(E->Class, A, NAME_None);
			if (!Copy)
			{
				continue;
			}
			if (E->Data)
			{
				for (FUSProperty* CP : E->Class->AllProps)
				{
					CP->CopyComplete(Copy->Data + CP->Offset, E->Data + CP->Offset);
				}
			}
			else
			{
				FString Pkg, Rel;
				E->GetPathName().Split(TEXT("."), &Pkg, &Rel);
				FUSLinker* const* Linker = RT.Linkers.Find(FName(*Pkg.ToLower()));
				Linker = Linker ? Linker : RT.Linkers.Find(FName(*Pkg));
				const int32 Ei = Linker && *Linker ? (*Linker)->FindExport(Rel, E->Class->GetName()) : INDEX_NONE;
				if (Ei != INDEX_NONE)
				{
					(*Linker)->LoadInstanceProperties(Ei, Copy);
				}
			}
			E = Copy;
		}
	}

	/** One frame of the mirrored client-code actors (the ones spawned on our side tick with the local actors). */
	void TickMirroredClients(float DeltaSeconds)
	{
		FUSEngineProps& P = USProps();
		TArray<FUSObject*> Run;
		for (const TPair<FUSObject*, FClientRun>& KV : ClientRuns)
		{
			if (KV.Value.bMirrored && !KV.Value.bOff)
			{
				Run.Add(KV.Key);
			}
		}
		for (FUSObject* A : Run)
		{
			RunGuarded(A, [&]()
			{
				TickActor(A, DeltaSeconds, P);
			});
		}
	}

	/** An actor's timer, Tick and state code (UE2 client). */
	void TickActor(FUSObject* A, float DeltaSeconds, FUSEngineProps& P)
	{
		// Actor.LifeSpan of our own client-side actors (a replicated actor's is the server's)
		if (const FClientRun* R = ClientRuns.Find(A); R && !R->bMirrored)
		{
			if (float* Life = P.LifeSpan.Ptr<float>(A); Life && *Life > 0.f)
			{
				*Life -= DeltaSeconds;
				if (*Life <= 0.f)
				{
					*Life = 0.f;
					if (USVM::FindFunction(A, TEXT("Expired")))
					{
						FUE2NetMirrorWorld::CallEvent(A, TEXT("Expired"));
					}
					if (!A->IsPendingKill())
					{
						Script->DestroyActor(A);
					}
					return;
				}
			}
		}
		float* Rate = P.TimerRate.Ptr<float>(A);
		float* Counter = P.TimerCounter.Ptr<float>(A);
		if (Rate && Counter && *Rate > 0.f)
		{
			*Counter += DeltaSeconds;
			if (*Counter >= *Rate)
			{
				const int32 Ticks = FMath::Max(1, int32(*Counter / *Rate));
				*Counter -= *Rate * Ticks;
				if (!P.bTimerLoop.Get<uint32>(A))
				{
					*Rate = 0.f;
				}
				FUE2NetMirrorWorld::CallEvent(A, TEXT("Timer"));
			}
		}
		if (A->IsPendingKill())
		{
			return;
		}
		if (A->Class->bHasTick)
		{
			if (FUSFunction* F = USVM::FindFunction(A, TEXT("Tick")))
			{
				FUSParms Parms(F);
				Parms.Float(DeltaSeconds);
				Parms.Call(A);
			}
		}
		USVM::ProcessState(A, DeltaSeconds);
	}

	/** Skins of mirrored mod pawns their client code set, by object (paths last applied). */
	TMap<FUSObject*, TArray<FString>> PawnSkins;

	/**
	 * A mod pawn's client code setting its Skins to a mod's own materials (a bright skin, a mod's team skins): shown on our
	 * pawn. Materials made at run time by script (Freon's frozen Combiners) and UT2004's own skins (the character's, which
	 * ours already show) are left out.
	 */
	void SyncPawnSkins()
	{
		FUE2NetWorld* NW = FUE2NetWorld::Get();
		for (const TPair<FUSObject*, FClientRun>& KV : ClientRuns)
		{
			FUSObject* O = KV.Key;
			if (!NW || !KV.Value.bMirrored || !O || O->IsPendingKill() || !RTPawn() || !O->Class->IsChildOf(RTPawn()))
			{
				continue;
			}
			TArray<FString> Paths;
			if (FUSProperty* P = O->FindProperty(TEXT("Skins")); P && P->Type == EUSType::Array && P->Inner && P->Inner->Type == EUSType::Object)
			{
				const FUSScriptArray& Arr = *reinterpret_cast<const FUSScriptArray*>(O->Data + P->Offset);
				for (int32 i = 0; i < Arr.Num && Arr.Data; ++i)
				{
					const FUSObject* M = *reinterpret_cast<FUSObject* const*>(Arr.Data + i * P->Inner->ElementSize);
					const FString Path = M ? M->GetPathName() : FString();
					FString Pkg;
					Paths.Add(Path.Split(TEXT("."), &Pkg, nullptr) && !IsStockPackage(Pkg) && !UE2NetModWeapons::IsStockClass(Path) ? Path : FString());
				}
			}
			TArray<FString>& Last = PawnSkins.FindOrAdd(O);
			if (Paths == Last)
			{
				continue;
			}
			Last = Paths;
			const int32* Ch = ChannelOf.Find(O);
			AUE2NetPawn* Pawn = Ch ? Cast<AUE2NetPawn>(NW->GetNativeActor(*Ch)) : nullptr;
			if (Pawn)
			{
				Pawn->SetScriptSkins(UE2NetModWeapons::LoadSkins(Paths));
			}
		}
		for (auto It = PawnSkins.CreateIterator(); It; ++It)
		{
			if (!ClientRuns.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
	}

	/** The client-side actors of mod code that show something: drawn natively. */
	void SyncClientVisuals()
	{
		SyncPawnSkins();
		if (!Visuals)
		{
			return;
		}
		TArray<FUSObject*> Spawned;
		for (const TPair<FUSObject*, FClientRun>& KV : ClientRuns)
		{
			if (!KV.Value.bMirrored && KV.Key && !KV.Key->IsPendingKill())
			{
				Spawned.Add(KV.Key);
			}
		}
		Visuals->Sync(Spawned, [this](FUSObject* O) { return ComponentOf(O); });
		for (FUSObject* Done : Visuals->TakeFinished())
		{
			Script->DestroyActor(Done);
		}
		// what client code attaches to a replicated vehicle weapon replaces our class-defaults stand-in for it
		FUE2NetWorld* NW = FUE2NetWorld::Get();
		for (TPair<FUSObject*, FClientRun>& KV : ClientRuns)
		{
			FUSObject* Base = KV.Value.bMirrored || KV.Value.bReported ? nullptr : USProps().Base.Get<FUSObject*>(KV.Key);
			const int32* Ch = Base ? ChannelOf.Find(Base) : nullptr;
			if (Ch && NW && Visuals->Find(KV.Key) && NW->RetireClientAttachmentStandIn(*Ch, KV.Key->Class->GetPathName()))
			{
				KV.Value.bReported = true;
			}
		}
	}

	int32 MessagesFed = 0, MessagesFailed = 0;

	bool FeedMessage(int32 Channel, const FString& Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
	{
		CheckPlayerController();
		FUSObject* PC = GetPC();
		FUSObject* Hud = GetHud();
		if (!bReady || !bModHud || bHudFailed || Channel != PCChannel || !PC || !Hud)
		{
			return false;
		}
		auto Param = [&](const TCHAR* Key) -> const FUE2NetValue*
		{
			for (const TPair<FName, FUE2NetValue>& P : Params)
			{
				if (P.Key == Key)
				{
					return &P.Value;
				}
			}
			return nullptr;
		};
		auto Obj = [&](const TCHAR* Key) -> FUSObject*
		{
			const FUE2NetValue* V = Param(Key);
			return V ? ResolveRef(V->Obj, 0, FString()) : nullptr;
		};
		const uint64 Aborts = USVM::GStatAborts;
		if (Function == TEXT("ReceiveLocalizedMessage"))
		{
			// PlayerController.ReceiveLocalizedMessage: Message.static.ClientReceive(Self, Switch, RelatedPRI_1, RelatedPRI_2,
			// OptionalObject); LocalMessage.ClientReceive puts it in the HUD's LocalizedMessage list (a mod's message
			// class may do more: sounds, its own HUD calls)
			FUSObject* C = Obj(TEXT("Message"));
			FUSClass* MC = C && (C->Flags & USF_Field) ? static_cast<FUSClass*>(C) : nullptr;
			FUSObject* DO = MC ? MC->GetDefaultObject() : nullptr;
			FUSFunction* F = DO ? USVM::FindFunction(DO, TEXT("ClientReceive")) : nullptr;
			if (!F)
			{
				return false;
			}
			const FUE2NetValue* Switch = Param(TEXT("Switch"));
			FUSParms P(F);
			P.Object(PC).Int(Switch ? Switch->I : 0).Object(Obj(TEXT("RelatedPRI_1"))).Object(Obj(TEXT("RelatedPRI_2"))).Object(Obj(TEXT("OptionalObject")));
			P.Call(DO);
		}
		else if (Function == TEXT("ClientMessage") || Function == TEXT("TeamMessage"))
		{
			// PlayerController.ClientMessage -> TeamMessage(PlayerReplicationInfo, S, Type) -> myHUD.Message(PRI, S, Type)
			FUSFunction* F = USVM::FindFunction(Hud, TEXT("Message"));
			const FUE2NetValue* S = Param(TEXT("S"));
			const FUE2NetValue* Type = Param(TEXT("Type"));
			if (!F || !S)
			{
				return false;
			}
			FUSObject* PRI = Function == TEXT("TeamMessage") ? Obj(TEXT("PRI")) : GetObj(PC, TEXT("PlayerReplicationInfo"));
			FUSParms P(F);
			P.Object(PRI).Str(S->S).Name(Type ? (Type->Kind == FUE2NetValue::EKind::Name ? Type->N : FName(*Type->S)) : NAME_None);
			P.Call(Hud);
		}
		else
		{
			return false;
		}
		if (USVM::GStatAborts != Aborts)
		{
			// a script error: this one is ours (the HUD may still draw others)
			++MessagesFailed;
			return false;
		}
		++MessagesFed;
		return true;
	}

	// ---- the local player's script weapons ----------------------------------------------------------

	int32 HeldScriptWeapon = 0;
	TSet<FUSObject*> FireModesStarted;

	FUSObject* MirrorOfChannel(int32 Channel) const
	{
		FUSObject* const* Hit = Channel ? Mirrors.Find(Channel) : nullptr;
		return Hit && *Hit && !(*Hit)->IsPendingKill() ? *Hit : nullptr;
	}

	bool IsScriptWeaponImpl(int32 Channel) const
	{
		FUSObject* W = MirrorOfChannel(Channel);
		const FClientRun* R = W ? ClientRuns.Find(W) : nullptr;
		return R && !R->bOff && ChainHas(W->Class, TEXT("Weapon")) && !ChainHas(W->Class, TEXT("WeaponAttachment"));
	}

	static FUSObject* FireModeOf(FUSObject* W, int32 Fm)
	{
		FUSProperty* P = W ? W->FindProperty(TEXT("FireMode")) : nullptr;
		if (!P || P->Type != EUSType::Object || Fm >= P->ArrayDim)
		{
			return nullptr;
		}
		FUSObject* F = *reinterpret_cast<FUSObject**>(W->Data + P->Offset + Fm * P->ElementSize);
		return F && F->Data ? F : nullptr;
	}

	template <typename T>
	static T* PropPtr(FUSObject* O, const TCHAR* Name)
	{
		FUSProperty* P = O && O->Data ? O->FindProperty(FName(Name)) : nullptr;
		return P && P->ElementSize >= int32(sizeof(T)) ? reinterpret_cast<T*>(O->Data + P->Offset) : nullptr;
	}

	/** Calls a script function of Obj with up to one int / float / object parameter (events and plain functions). */
	static bool CallWith(FUSObject* Obj, const TCHAR* Name, TFunctionRef<void(FUSParms&)> Fill)
	{
		FUSFunction* F = Obj ? USVM::FindFunction(Obj, FName(Name)) : nullptr;
		if (!F)
		{
			return false;
		}
		FUSParms P(F);
		Fill(P);
		P.Call(Obj);
		return true;
	}

	/** What UE2's client sets up for a weapon of ours (ClientWeaponSet, the fire modes' PostBeginPlay). */
	void PrepareWeapon(FUSObject* W)
	{
		FUSObject* Pawn = GetObj(W, TEXT("Instigator"));
		Pawn = Pawn ? Pawn : GetObj(W, TEXT("Owner"));
		if (!GetObj(W, TEXT("Instigator")) && Pawn)
		{
			SetObj(W, TEXT("Instigator"), Pawn);
		}
		for (int32 Fm = 0; Fm < 2; ++Fm)
		{
			FUSObject* F = FireModeOf(W, Fm);
			if (!F)
			{
				continue;
			}
			SetObj(F, TEXT("Instigator"), Pawn);
			SetObj(F, TEXT("Owner"), W);
			SetObj(F, TEXT("Level"), Level);
			if (!FireModesStarted.Contains(F))
			{
				FireModesStarted.Add(F);
				CallWith(F, TEXT("PostBeginPlay"), [](FUSParms&) {});
			}
		}
	}

	void ScriptWeaponCommandImpl(int32 Channel, FUE2NetScriptHUD::EWeaponCommand Command, uint8 Fm)
	{
		FUSObject* W = MirrorOfChannel(Channel);
		if (!W || !IsScriptWeaponImpl(Channel))
		{
			return;
		}
		RunGuarded(W, [&]()
		{
			PrepareWeapon(W);
			switch (Command)
			{
			case FUE2NetScriptHUD::EWeaponCommand::BringUp:
				HeldScriptWeapon = Channel;
				if (FUSObject* Pawn = GetObj(W, TEXT("Instigator")))
				{
					SetObj(Pawn, TEXT("Weapon"), W);
				}
				CallWith(W, TEXT("BringUp"), [](FUSParms& P) { P.Object(nullptr); });
				break;
			case FUE2NetScriptHUD::EWeaponCommand::PutDown:
				if (HeldScriptWeapon == Channel)
				{
					HeldScriptWeapon = 0;
				}
				CallWith(W, TEXT("PutDown"), [](FUSParms&) {});
				break;
			case FUE2NetScriptHUD::EWeaponCommand::StartFire:
				CallWith(W, TEXT("ClientStartFire"), [Fm](FUSParms& P) { P.Int(Fm); });
				break;
			case FUE2NetScriptHUD::EWeaponCommand::StopFire:
				CallWith(W, TEXT("ClientStopFire"), [Fm](FUSParms& P) { P.Int(Fm); });
				break;
			}
		});
	}

	bool GetScriptWeaponStateImpl(int32 Channel, FUE2NetScriptHUD::FScriptWeaponState& Out) const
	{
		FUSObject* W = MirrorOfChannel(Channel);
		if (!W)
		{
			return false;
		}
		// (AmmoAmount / MaxAmmo are simulated script functions: cheap, no side effects)
		auto IntCall = [W](const TCHAR* Name)
		{
			FUSFunction* F = USVM::FindFunction(W, FName(Name));
			if (!F)
			{
				return 0;
			}
			FUSParms P(F);
			P.Int(0);
			P.Call(W);
			return P.ReturnInt();
		};
		Out.Ammo = IntCall(TEXT("AmmoAmount"));
		Out.MaxAmmo = IntCall(TEXT("MaxAmmo"));
		if (const USPresentation::FAnim* A = USPresentation::FindAnim(W))
		{
			Out.Anim = A->Sequence;
			Out.AnimRate = A->Rate;
			Out.bAnimLoop = A->bLoop;
			Out.bAnimStopped = A->bStopped;
			Out.AnimSerial = A->Serial;
		}
		return true;
	}

	/**
	 * What a mod weapon's own HUD code shows as its ammo (HudCDeathmatch: Weapon.GetAmmoCount; BW: the magazine, and
	 * GetHUDAmmoText for magazine and reserve): only for weapon classes of a mod that override them.
	 */
	bool GetModWeaponAmmoImpl(int32 Channel, int32& OutCur, int32& OutMax, FString& OutText) const
	{
		FUSObject* W = MirrorOfChannel(Channel);
		if (!W || W->IsPendingKill() || !ClientRuns.Contains(W))
		{
			return false;
		}
		auto ModOverride = [W](const TCHAR* Name) -> FUSFunction*
		{
			FUSFunction* F = USVM::FindFunction(W, FName(Name));
			FUSClass* C = F ? F->GetOwnerClass() : nullptr;
			return C && !IsStockPackage(C->GetPackageName().ToString()) ? F : nullptr;
		};
		FUSFunction* Count = ModOverride(TEXT("GetAmmoCount"));
		FUSFunction* Text = ModOverride(TEXT("GetHUDAmmoText"));
		if (!Count && !Text)
		{
			return false;
		}
		if (Count && Count->Parms.Num() >= 2)
		{
			FUSParms P(Count);
			P.Float(0.f).Float(0.f);
			P.Call(W);
			OutMax = FMath::RoundToInt(P.GetFloat(0));
			OutCur = FMath::RoundToInt(P.GetFloat(1));
		}
		if (Text && Text->Parms.Num() >= 1)
		{
			FUSParms P(Text);
			P.Int(0);
			P.Call(W);
			OutText = P.ReturnStr();
		}
		return true;
	}

	void ScriptWeaponAnimEndImpl(int32 Channel)
	{
		FUSObject* W = MirrorOfChannel(Channel);
		if (W && IsScriptWeaponImpl(Channel))
		{
			RunGuarded(W, [&]() { CallWith(W, TEXT("AnimEnd"), [](FUSParms& P) { P.Int(0); }); });
		}
	}

	/**
	 * One frame of a fire mode as UE2's native weapon code runs it on the owning client: ModeTick, the fire mode's own timer
	 * (WeaponFire.SetTimer: TimerInterval / NextTimerPop), and while the weapon is ready (WS_ReadyToFire) the shots:
	 * held modes fire each time NextFireTime passes (bWaitForRelease: once per press); charged modes (bFireOnRelease) start
	 * holding (ModeHoldFire) and fire when released.
	 */
	void TickFireMode(FUSObject* W, FUSObject* F, float DeltaSeconds, bool bShots)
	{
		CallWith(F, TEXT("ModeTick"), [DeltaSeconds](FUSParms& P) { P.Float(DeltaSeconds); });
		const float Now = TimeSeconds;
		if (float* Interval = PropPtr<float>(F, TEXT("TimerInterval")); Interval && *Interval > 0.f)
		{
			float* Pop = PropPtr<float>(F, TEXT("NextTimerPop"));
			if (Pop && Now >= *Pop)
			{
				const uint32* Loop = PropPtr<uint32>(F, TEXT("bTimerLoop"));
				if (Loop && *Loop)
				{
					*Pop += *Interval;
				}
				else
				{
					*Interval = 0.f;
				}
				CallWith(F, TEXT("Timer"), [](FUSParms&) {});
			}
		}
		const uint8* ClientState = PropPtr<uint8>(W, TEXT("ClientState"));
		if (!bShots || !ClientState || *ClientState != 4) // EWeaponClientState WS_ReadyToFire
		{
			return;
		}
		uint32* bIsFiring = PropPtr<uint32>(F, TEXT("bIsFiring"));
		uint32* bFireOnRelease = PropPtr<uint32>(F, TEXT("bFireOnRelease"));
		uint32* bWaitForRelease = PropPtr<uint32>(F, TEXT("bWaitForRelease"));
		uint32* bNowWaiting = PropPtr<uint32>(F, TEXT("bNowWaiting"));
		uint32* bInstantStop = PropPtr<uint32>(F, TEXT("bInstantStop"));
		float* NextFireTime = PropPtr<float>(F, TEXT("NextFireTime"));
		float* HoldTime = PropPtr<float>(F, TEXT("HoldTime"));
		if (!bIsFiring || !NextFireTime || !HoldTime)
		{
			return;
		}
		if (bFireOnRelease && *bFireOnRelease)
		{
			if (*bIsFiring)
			{
				if (Now >= *NextFireTime)
				{
					if (*HoldTime <= 0.f)
					{
						CallWith(F, TEXT("ModeHoldFire"), [](FUSParms&) {});
					}
					*HoldTime += DeltaSeconds;
				}
			}
			else if ((*HoldTime > 0.f || (bInstantStop && *bInstantStop)) && Now >= *NextFireTime)
			{
				CallWith(F, TEXT("ModeDoFire"), [](FUSParms&) {});
				if (bInstantStop)
				{
					*bInstantStop = 0;
				}
			}
			return;
		}
		if (*bIsFiring)
		{
			const bool bWaiting = bWaitForRelease && *bWaitForRelease && bNowWaiting && *bNowWaiting;
			if (!bWaiting && Now >= *NextFireTime)
			{
				UE_LOG(LogUE2Net, Verbose, TEXT("UE2NetScriptHUD: %s.ModeDoFire at %.2f"), *F->GetName(), Now);
				CallWith(F, TEXT("ModeDoFire"), [](FUSParms&) {});
				if (bWaitForRelease && *bWaitForRelease && bNowWaiting)
				{
					*bNowWaiting = 1;
				}
			}
		}
		else if (bNowWaiting && bWaitForRelease && *bWaitForRelease)
		{
			*bNowWaiting = 0;
		}
	}

	/** The held script weapon's frame: Weapon.WeaponTick, then its fire modes. */
	void TickHeldScriptWeapon(float DeltaSeconds)
	{
		// the weapon in our hands (the server's ClientWeaponSet brings a script weapon up through its own code, not only
		// through our BringUp)
		if (const FUE2NetPlay* Play = FUE2NetPlay::Get())
		{
			const int32 Cur = Play->GetCurrentWeaponChannel();
			if (Cur && Cur != HeldScriptWeapon && IsScriptWeaponImpl(Cur))
			{
				HeldScriptWeapon = Cur;
			}
		}
		FUSObject* W = MirrorOfChannel(HeldScriptWeapon);
		if (!W || !IsScriptWeaponImpl(HeldScriptWeapon))
		{
			return;
		}
		RunGuarded(W, [&]()
		{
			CallWith(W, TEXT("WeaponTick"), [DeltaSeconds](FUSParms& P) { P.Float(DeltaSeconds); });
			for (int32 Fm = 0; Fm < 2; ++Fm)
			{
				if (FUSObject* F = FireModeOf(W, Fm))
				{
					TickFireMode(W, F, DeltaSeconds, true);
				}
			}
		});
	}

	void TickStandInWeaponImpl(int32 Channel, bool bHeld0, bool bHeld1, float DeltaSeconds)
	{
		FUSObject* W = MirrorOfChannel(Channel);
		const FClientRun* R = W ? ClientRuns.Find(W) : nullptr;
		if (!R || R->bOff)
		{
			return;
		}
		RunGuarded(W, [&]()
		{
			PrepareWeapon(W);
			for (int32 Fm = 0; Fm < 2; ++Fm)
			{
				if (FUSObject* F = FireModeOf(W, Fm))
				{
					if (uint32* bIsFiring = PropPtr<uint32>(F, TEXT("bIsFiring")))
					{
						*bIsFiring = (Fm == 0 ? bHeld0 : bHeld1) ? 1u : 0u;
					}
					TickFireMode(W, F, DeltaSeconds, false); // (our stand-in plays the shots)
				}
			}
		});
	}

	void StandInFiredImpl(int32 Channel, uint8 Fm)
	{
		FUSObject* W = MirrorOfChannel(Channel);
		FUSObject* F = W ? FireModeOf(W, Fm) : nullptr;
		if (F && ClientRuns.Contains(W))
		{
			RunGuarded(W, [&]() { CallWith(F, TEXT("ModeDoFire"), [](FUSParms&) {}); });
		}
	}

	float GetScriptFireRateImpl(int32 Channel, uint8 Fm) const
	{
		FUSObject* W = MirrorOfChannel(Channel);
		FUSObject* F = W ? FireModeOf(W, Fm) : nullptr;
		const float* Rate = F ? PropPtr<float>(F, TEXT("FireRate")) : nullptr;
		return Rate ? *Rate : -1.f;
	}

	/** UE2's console: an exec function of the viewport's interactions' owners: our controller, then its pawn and weapon. */
	bool ConsoleExecImpl(const FString& Command, bool bModOnly = false)
	{
		FString Name, Args;
		if (!Command.TrimStartAndEnd().Split(TEXT(" "), &Name, &Args))
		{
			Name = Command.TrimStartAndEnd();
		}
		FUSObject* PC = GetPC();
		FUSObject* Pawn = GetObj(PC, TEXT("Pawn"));
		FUSObject* Weapon = GetObj(Pawn, TEXT("Weapon"));
		for (FUSObject* Target : { PC, Pawn, Weapon })
		{
			FUSFunction* F = Target ? USVM::FindFunction(Target, FName(*Name)) : nullptr;
			if (!F || !(F->FunctionFlags & USFlags::FUNC_Exec))
			{
				continue;
			}
			if (bModOnly && (!F->GetOwnerClass() || IsStockPackage(F->GetOwnerClass()->GetPackageName().ToString())))
			{
				continue;
			}
			TArray<FString> Words;
			Args.ParseIntoArrayWS(Words);
			FUSParms P(F);
			int32 i = 0;
			for (FUSProperty* Parm : F->Parms)
			{
				if (Parm == F->ReturnProperty)
				{
					continue;
				}
				// the last string parameter takes the rest of the line, as UE2's exec parser does
				const FString W = Words.IsValidIndex(i) ? Words[i] : FString();
				switch (Parm->Type)
				{
				case EUSType::Str:
				{
					FString Rest;
					for (int32 j = i; j < Words.Num(); ++j)
					{
						Rest += (j > i ? TEXT(" ") : TEXT("")) + Words[j];
					}
					P.Str(Parm == F->Parms.Last() ? Rest : W);
					break;
				}
				case EUSType::Int: P.Int(FCString::Atoi(*W)); break;
				case EUSType::Float: P.Float(FCString::Atof(*W)); break;
				case EUSType::Byte: P.Byte(uint8(FCString::Atoi(*W))); break;
				case EUSType::Bool: P.Bool(W == TEXT("1") || W.Equals(TEXT("true"), ESearchCase::IgnoreCase)); break;
				case EUSType::Name: P.Name(FName(*W)); break;
				default: P.Skip(); break;
				}
				++i;
			}
			if (ClientRuns.Contains(Target))
			{
				RunGuarded(Target, [&]() { P.Call(Target); });
			}
			else
			{
				P.Call(Target);
			}
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: console %s ran %s.%s"), *Command, *Target->Class->GetName(), *Name);
			return true;
		}
		return false;
	}

	/**
	 * A function the server calls on a mod actor that runs client code (ClientWeaponSet, ClientForceAmmoUpdate, a mod's own
	 * client calls): it runs in the mirror, as on UE2's client.
	 */
	bool RunClientFunction(const FUE2NetActor& A, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
	{
		FUSObject* O = MirrorOfChannel(A.Channel);
		const FClientRun* R = O ? ClientRuns.Find(O) : nullptr;
		FUSFunction* F = R && !R->bOff ? USVM::FindFunction(O, Function->Name) : nullptr;
		UE_LOG(LogUE2Net, Verbose, TEXT("UE2NetScriptHUD: server call %s on %s: %s"), *Function->GetName(), O ? *O->GetName() : TEXT("(no mirror)"),
			F ? TEXT("run") : (R ? TEXT("no such function") : TEXT("not running client code")));
		if (!F)
		{
			return false;
		}
		const int32 Size = FMath::Max(F->PropertiesSize, 8);
		uint8* Block = static_cast<uint8*>(FMemory::Malloc(Size, 16));
		FMemory::Memzero(Block, Size);
		for (FUSProperty* P : F->Parms)
		{
			if (P == F->ReturnProperty)
			{
				continue;
			}
			for (const TPair<FName, FUE2NetValue>& V : Params)
			{
				if (V.Key == P->Name)
				{
					WriteValue(P, Block + P->Offset, V.Value, A.Channel, P->Name.ToString());
					break;
				}
			}
		}
		{
			TGuardValue<FUSObject*> GA(DeliveringActor, O);
			TGuardValue<FUSFunction*> GF(DeliveringFunction, F);
			RunGuarded(O, [&]() { USVM::ProcessEvent(O, F, Block); });
		}
		F->DestroyValue(Block);
		FMemory::Free(Block);
		return true;
	}

	/** True when a mirrored actor's own client code shows an attachment of that class (Net/UE2NetModWeapons stand-in). */
	bool HasClientAttachment(int32 Channel, const FString& ClassPath) const
	{
		for (const TPair<FUSObject*, FClientRun>& KV : ClientRuns)
		{
			FUSObject* Base = KV.Value.bMirrored ? nullptr : USProps().Base.Get<FUSObject*>(KV.Key);
			const int32* Ch = Base ? ChannelOf.Find(Base) : nullptr;
			if (Ch && *Ch == Channel && KV.Key->Class->GetPathName().Equals(ClassPath, ESearchCase::IgnoreCase) && Visuals && Visuals->Find(KV.Key))
			{
				return true;
			}
		}
		return false;
	}

	static FUSClass* RTActor() { return FUSRuntime::Get().ActorClass; }
	static FUSClass* RTPawn() { return FUSRuntime::Get().PawnClass; }

	void InitWeaponFires(FUSObject* Weapon)
	{
		FUSProperty* Classes = Weapon->FindProperty(TEXT("FireModeClass"));
		FUSProperty* ModeProp = Weapon->FindProperty(TEXT("FireMode"));
		if (!Classes || !ModeProp || Classes->Type != EUSType::Class || ModeProp->Type != EUSType::Object)
		{
			return;
		}
		for (int32 i = 0; i < FMath::Min(Classes->ArrayDim, ModeProp->ArrayDim); ++i)
		{
			FUSObject* C = *reinterpret_cast<FUSObject**>(Weapon->Data + Classes->Offset + i * Classes->ElementSize);
			FUSObject*& FireObj = *reinterpret_cast<FUSObject**>(Weapon->Data + ModeProp->Offset + i * ModeProp->ElementSize);
			if (FireObj || !C || !(C->Flags & USF_Field))
			{
				continue;
			}
			FireObj = Script->ConstructObject(static_cast<FUSClass*>(C), Weapon, NAME_None);
			SetObj(FireObj, TEXT("Weapon"), Weapon);
			SetObj(FireObj, TEXT("Level"), Level);
			if (FUSProperty* TM = FireObj ? FireObj->FindProperty(TEXT("ThisModeNum")) : nullptr)
			{
				FireObj->Data[TM->Offset] = uint8(i);
			}
			// InitWeaponFires also gives the weapon its ammo classes (Weapon.AmmoClass[i] = FireMode[i].AmmoClass)
			FUSProperty* WAmmo = Weapon->FindProperty(TEXT("AmmoClass"));
			FUSObject* FireAmmo = GetObj(FireObj, TEXT("AmmoClass"));
			if (WAmmo && WAmmo->Type == EUSType::Class && i < WAmmo->ArrayDim && FireAmmo)
			{
				FUSObject*& Slot = *reinterpret_cast<FUSObject**>(Weapon->Data + WAmmo->Offset + i * WAmmo->ElementSize);
				Slot = Slot ? Slot : FireAmmo;
			}
		}
	}

	void Update(const FUE2NetActor& A, const TArray<FString>& Changed)
	{
		FUSObject* const* Hit = Mirrors.Find(A.Channel);
		if (!Hit)
		{
			Open(A);
			return;
		}
		for (const FString& Key : Changed)
		{
			WriteKey(*Hit, A, Key);
		}
		if (ClientRuns.Contains(*Hit))
		{
			// UE2's replication callbacks: ClientTrigger when bClientTrigger flips, the weapon effects when FlashCount
			// changes (ONSWeapon.FlashMuzzleFlash, WeaponAttachment.ThirdPersonEffects), PostNetReceive with bNetNotify
			FUSObject* O = *Hit;
			if (Changed.Contains(TEXT("bClientTrigger")))
			{
				CallClientEvent(O, TEXT("ClientTrigger"));
			}
			if (Changed.Contains(TEXT("FlashCount")))
			{
				CallClientEvent(O, USVM::FindFunction(O, TEXT("FlashMuzzleFlash")) ? TEXT("FlashMuzzleFlash") : TEXT("ThirdPersonEffects"));
			}
			if (FUSProperty* NN = O->FindProperty(TEXT("bNetNotify")); NN && NN->Type == EUSType::Bool && *reinterpret_cast<uint32*>(O->Data + NN->Offset))
			{
				CallClientEvent(O, TEXT("PostNetReceive"));
			}
		}
		if (A.Channel == PCChannel && GRI)
		{
			SetObj(*Hit, TEXT("GameReplicationInfo"), GRI);
		}
		if (Changed.Contains(TEXT("bTearOff")))
		{
			FUSObject* O = *Hit;
			FUSProperty* T = O->FindProperty(TEXT("bTearOff"));
			if (T && T->Type == EUSType::Bool && *reinterpret_cast<const uint32*>(O->Data + T->Offset))
			{
				TearOff(A.Channel, O);
			}
		}
	}

	/**
	 * UE2 bTearOff: the client's copy leaves the channel and becomes its own (ROLE_Authority), then gets TornOff (a
	 * dying pawn, BW's M46 sticky mine). One running client code keeps running as a client-side actor; the channel's close
	 * no longer touches it.
	 */
	void TearOff(int32 Channel, FUSObject* O)
	{
		FClientRun* R = ClientRuns.Find(O);
		if (!R || O == GetPC())
		{
			return;
		}
		USProps().Role.Set<uint8>(O, UE2_ROLE_Authority);
		USProps().RemoteRole.Set<uint8>(O, 0);
		Mirrors.Remove(Channel);
		ChannelOf.Remove(O);
		R->bMirrored = false;
		Script->LocalActors.AddUnique(O);
		CallClientEvent(O, TEXT("TornOff"));
	}

	void Close(const FUE2NetActor& A)
	{
		FUSObject* O = nullptr;
		if (!Mirrors.RemoveAndCopyValue(A.Channel, O) || !O || O == Level)
		{
			return;
		}
		if (ClientRuns.Contains(O))
		{
			CallClientEvent(O, TEXT("Destroyed"));
			ClientRuns.Remove(O);
		}
		ChannelOf.Remove(O);
		if (O == GRI)
		{
			GRI = nullptr;
			USProps().GRI.Set<FUSObject*>(Level, nullptr);
		}
		if (A.Channel == PCChannel)
		{
			PCChannel = 0;
			bPCInitialised = false;
			Script->LocalPC = nullptr;
		}
		bPRIsDirty = bPRIsDirty || ChainHas(O->Class, TEXT("PlayerReplicationInfo"));
		// a static map actor's channel closing leaves cached references to it
		for (auto It = StaticRefs.CreateIterator(); It; ++It)
		{
			if (It.Value() == O)
			{
				It.RemoveCurrent();
			}
		}
		Script->TimerOnly.Remove(O);
		Script->LocalActors.Remove(O);
		Script->MarkPendingKill(O);
		ForgetDestroyed();
		Script->CleanupDestroyed();
	}

	FUSObject* GetPC() const
	{
		FUSObject* const* Hit = PCChannel ? Mirrors.Find(PCChannel) : nullptr;
		return Hit ? *Hit : nullptr;
	}

	void CheckPlayerController()
	{
		if (PCChannel)
		{
			return;
		}
		const int32 Ch = Connection.FindPlayerControllerChannel();
		FUSObject* const* Hit = Ch ? Mirrors.Find(Ch) : nullptr;
		if (!Hit)
		{
			return;
		}
		PCChannel = Ch;
		FUSObject* PC = *Hit;
		Script->LocalPC = PC;
		USProps().Role.Set<uint8>(PC, UE2_ROLE_AutonomousProxy);
		SetObj(PC, TEXT("Player"), PlayerObj);
		SetObj(PC, TEXT("ViewTarget"), PC);
		if (GRI)
		{
			SetObj(PC, TEXT("GameReplicationInfo"), GRI);
		}
		if (PlayerObj)
		{
			SetObj(PlayerObj, TEXT("Actor"), PC);
		}
		SetObj(CanvasObj, TEXT("Viewport"), PlayerObj);
		// Player.GUIController before the controller's own code runs (mods open their menus through it)
		EnsureGui();
		if (!bPCInitialised)
		{
			bPCInitialised = true;
			// the client's own PlayerController.PostBeginPlay (UTComp: its client settings objects, which its HUD reads)
			const double T0 = FPlatformTime::Seconds();
			const uint64 Aborts = USVM::GStatAborts;
			FUE2NetMirrorWorld::CallEvent(PC, TEXT("PostBeginPlay"));
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: %s PostBeginPlay ran on our controller (%.1f ms%s)"), *PC->Class->GetPathName(),
				(FPlatformTime::Seconds() - T0) * 1000.0, USVM::GStatAborts != Aborts ? TEXT(", script error") : TEXT(""));
			// PostBeginPlay spawned a default HUD (SpawnDefaultHUD): the server's ClientSetHUD replaces it
			AddModInteractions(PC);
		}
		if (PendingSetHUD.Num())
		{
			TArray<TPair<FName, FUE2NetValue>> Params = MoveTemp(PendingSetHUD);
			PendingSetHUD.Reset();
			RunSetHUD(Params);
		}
	}

	/**
	 * Interactions a mod's client code adds for the local player and keeps in a controller property (UTComp's mutator Tick:
	 * BS_xPlayer.Overlay = InteractionMaster.AddInteraction("UTComp_Overlay", Player), the team / powerup overlay): a
	 * controller property typed as an Interaction subclass that is still None gets that interaction, through the stock
	 * InteractionMaster.AddInteraction.
	 */
	void AddModInteractions(FUSObject* PC)
	{
		FUSRuntime& RT = FUSRuntime::Get();
		FUSClass* InteractionClass = RT.FindClass(TEXT("Engine.Interaction"));
		FUSFunction* Add = InteractionMaster ? USVM::FindFunction(InteractionMaster, TEXT("AddInteraction")) : nullptr;
		if (!InteractionClass || !Add)
		{
			return;
		}
		for (FUSProperty* P : PC->Class->AllProps)
		{
			if (P->Type != EUSType::Object || !P->PropertyClass || P->PropertyClass == InteractionClass || !P->PropertyClass->IsChildOf(InteractionClass)
				|| IsStockPackage(P->PropertyClass->GetPackageName().ToString()) || IsDeniedPackage(P->PropertyClass->GetPackageName().ToString())
				|| *reinterpret_cast<FUSObject**>(PC->Data + P->Offset))
			{
				continue;
			}
			FUSParms Parms(Add);
			Parms.Str(P->PropertyClass->GetPathName()).Object(PlayerObj);
			Parms.Call(InteractionMaster);
			FUSObject* I = Parms.ReturnObject();
			*reinterpret_cast<FUSObject**>(PC->Data + P->Offset) = I;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: interaction %s added for %s.%s%s"), *P->PropertyClass->GetPathName(), *PC->Class->GetName(),
				*P->GetName(), I ? TEXT("") : TEXT(" (failed)"));
		}
	}

	/**
	 * A mod's info actor its client code finds for itself in its PlayerTick (UTComp: BS_xPlayer.UTCompPRI, the
	 * UTComp_PRI of our player, which its menu and HUD read; the mirrored controller does not tick): a controller
	 * property typed as a mod's ReplicationInfo subclass that is still None gets the mirrored actor of that class owned by
	 * our controller or our PlayerReplicationInfo (or the only one there is).
	 */
	double NextInfoFill = 0.0;
	void FillModInfoRefs(FUSObject* PC)
	{
		const double Now = FPlatformTime::Seconds();
		if (!PC || Now < NextInfoFill)
		{
			return;
		}
		NextInfoFill = Now + 0.5;
		FUSClass* RIClass = FUSRuntime::Get().FindClass(TEXT("Engine.ReplicationInfo"));
		FUSObject* MyPRI = GetObj(PC, TEXT("PlayerReplicationInfo"));
		for (FUSProperty* P : PC->Class->AllProps)
		{
			if (!RIClass || P->Type != EUSType::Object || P->ArrayDim != 1 || !P->PropertyClass || !P->PropertyClass->IsChildOf(RIClass)
				|| IsStockPackage(P->PropertyClass->GetPackageName().ToString()) || *reinterpret_cast<FUSObject**>(PC->Data + P->Offset))
			{
				continue;
			}
			FUSObject* Found = nullptr;
			int32 Count = 0;
			for (const TPair<int32, FUSObject*>& KV : Mirrors)
			{
				FUSObject* O = KV.Value;
				if (!O || O->IsPendingKill() || !O->Class->IsChildOf(P->PropertyClass))
				{
					continue;
				}
				++Count;
				FUSObject* Owner = USProps().Owner.Get<FUSObject*>(O);
				// a LinkedReplicationInfo of our PRI (PlayerReplicationInfo.CustomReplicationInfo -> NextReplicationInfo ...)
				bool bLinked = false;
				int32 Guard = 0;
				for (FUSObject* L = GetObj(MyPRI, TEXT("CustomReplicationInfo")); L && Guard < 16 && !bLinked; L = GetObj(L, TEXT("NextReplicationInfo")), ++Guard)
				{
					bLinked = L == O;
				}
				if (bLinked)
				{
					Found = O;
					Count = 1;
					break;
				}
				if ((Owner && (Owner == PC || Owner == MyPRI)) || (MyPRI && GetObj(O, TEXT("PRI")) == MyPRI))
				{
					Found = O;
				}
				else if (!Found && Count == 1)
				{
					Found = O;
				}
			}
			if (Found && (Count == 1 || USProps().Owner.Get<FUSObject*>(Found) == PC || USProps().Owner.Get<FUSObject*>(Found) == MyPRI || GetObj(Found, TEXT("PRI")) == MyPRI))
			{
				*reinterpret_cast<FUSObject**>(PC->Data + P->Offset) = Found;
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: %s.%s is %s"), *PC->Class->GetName(), *P->GetName(), *Found->GetName());
			}
		}
	}

	/** InteractionMaster.Process_PostRender / Process_Tick over the viewport's interactions. */
	void ProcessInteractions(const TCHAR* Event, FUSObject* CanvasArg, float DeltaSeconds)
	{
		FUSProperty* Local = PlayerObj ? PlayerObj->FindProperty(TEXT("LocalInteractions")) : nullptr;
		FUSFunction* F = InteractionMaster ? USVM::FindFunction(InteractionMaster, FName(Event)) : nullptr;
		if (!Local || !F || reinterpret_cast<FUSScriptArray*>(PlayerObj->Data + Local->Offset)->Num == 0 || F->Parms.Num() < 2)
		{
			return;
		}
		FUSParms Parms(F);
		Local->CopyComplete(Parms.ParmAddr(0), PlayerObj->Data + Local->Offset);
		if (CanvasArg)
		{
			*reinterpret_cast<FUSObject**>(Parms.ParmAddr(1)) = CanvasArg;
		}
		else
		{
			*reinterpret_cast<float*>(Parms.ParmAddr(1)) = DeltaSeconds;
		}
		Parms.Call(InteractionMaster);
	}

	void RebuildPRIArray()
	{
		if (!bPRIsDirty || !GRI)
		{
			return;
		}
		bPRIsDirty = false;
		FUSProperty* Arr = GRI->FindProperty(TEXT("PRIArray"));
		if (!Arr || Arr->Type != EUSType::Array || !Arr->Inner)
		{
			return;
		}
		TArray<TPair<int32, FUSObject*>> PRIs;
		for (const TPair<int32, FUSObject*>& KV : Mirrors)
		{
			if (KV.Value && !KV.Value->IsPendingKill() && ChainHas(KV.Value->Class, TEXT("PlayerReplicationInfo")))
			{
				PRIs.Add(KV);
			}
		}
		PRIs.Sort([](const TPair<int32, FUSObject*>& A, const TPair<int32, FUSObject*>& B) { return A.Key < B.Key; });
		FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(GRI->Data + Arr->Offset);
		const int32 ES = Arr->Inner->ElementSize;
		if (PRIs.Num() > A->Max)
		{
			A->Data = static_cast<uint8*>(FMemory::Realloc(A->Data, int64(PRIs.Num()) * ES, 16));
			A->Max = PRIs.Num();
		}
		A->Num = PRIs.Num();
		for (int32 i = 0; i < PRIs.Num(); ++i)
		{
			*reinterpret_cast<FUSObject**>(A->Data + int64(i) * ES) = PRIs[i].Value;
		}
	}

	// ---- the HUD -----------------------------------------------------------------------------------

	void RunSetHUD(const TArray<TPair<FName, FUE2NetValue>>& Params)
	{
		FUSObject* PC = GetPC();
		FUSFunction* F = PC ? USVM::FindFunction(PC, TEXT("ClientSetHUD")) : nullptr;
		if (!F)
		{
			PendingSetHUD = Params;
			return;
		}
		bHaveSetHUD = true;
		FUSParms Parms(F);
		for (int32 i = 0; i < F->Parms.Num(); ++i)
		{
			FUSProperty* P = F->Parms[i];
			for (const TPair<FName, FUE2NetValue>& KV : Params)
			{
				if (KV.Key == P->Name)
				{
					WriteValue(P, Parms.ParmAddr(i), KV.Value, 0, FString());
				}
			}
		}
		FUSClass* HudClass = static_cast<FUSClass*>(Parms.GetObject(0));
		FUSClass* ScoreClass = static_cast<FUSClass*>(Parms.GetObject(1));
		HudClassName = HudClass ? HudClass->GetPathName() : FString(TEXT("None"));
		ScoreboardClassName = ScoreClass ? ScoreClass->GetPathName() : FString(TEXT("None"));
		bModHud = HudClass && (Mode == 2 || !IsStockPackage(HudClass->GetPackageName().ToString()));
		bModScoreboard = ScoreClass && (Mode == 2 || !IsStockPackage(ScoreClass->GetPackageName().ToString()));
		bHudFailed = bScoreboardFailed = false;
		HudEmptyFrames = ScoreEmptyFrames = HudErrors = ScoreErrors = 0;
		if (!bModHud && !bModScoreboard)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: the server's HUD %s and scoreboard %s are stock: our HUD draws them"), *HudClassName, *ScoreboardClassName);
			return;
		}
		const double T0 = FPlatformTime::Seconds();
		const uint64 Aborts = USVM::GStatAborts;
		Parms.Call(PC);
		FUSObject* Hud = GetObj(PC, TEXT("myHUD"));
		FUSObject* Score = Hud ? GetObj(Hud, TEXT("ScoreBoard")) : nullptr;
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: ClientSetHUD(%s, %s) ran (%.1f ms%s): HUD %s, scoreboard %s; script draws the %s"), *HudClassName,
			*ScoreboardClassName, (FPlatformTime::Seconds() - T0) * 1000.0, USVM::GStatAborts != Aborts ? TEXT(", script error") : TEXT(""),
			Hud ? *Hud->Class->GetPathName() : TEXT("none"), Score ? *Score->Class->GetPathName() : TEXT("none"),
			bModHud && bModScoreboard ? TEXT("HUD and scoreboard") : (bModHud ? TEXT("HUD") : TEXT("scoreboard")));
	}

	FUSObject* GetHud() const
	{
		FUSObject* Hud = GetObj(GetPC(), TEXT("myHUD"));
		return Hud && !Hud->IsPendingKill() && Hud->Data ? Hud : nullptr;
	}

	void Tick(float DeltaSeconds)
	{
		if (!bReady)
		{
			return;
		}
		const double T0 = FPlatformTime::Seconds();
		CheckPlayerController();
		RebuildPRIArray();
		TimeSeconds += DeltaSeconds * 1.f;
		USProps().TimeSeconds.Set<float>(Level, TimeSeconds);
		{
			// LevelInfo's clock (UE2 sets it from the system time every tick; scoreboards show the date)
			const FDateTime Now = FDateTime::Now();
			const TPair<const TCHAR*, int32> Clock[] = { { TEXT("Year"), Now.GetYear() }, { TEXT("Month"), Now.GetMonth() }, { TEXT("Day"), Now.GetDay() },
				{ TEXT("DayOfWeek"), int32(Now.GetDayOfWeek() == EDayOfWeek::Sunday ? 0 : int32(Now.GetDayOfWeek()) + 1) }, { TEXT("Hour"), Now.GetHour() },
				{ TEXT("Minute"), Now.GetMinute() }, { TEXT("Second"), Now.GetSecond() }, { TEXT("Millisecond"), Now.GetMillisecond() } };
			for (const TPair<const TCHAR*, int32>& KV : Clock)
			{
				if (FUSProperty* P = Level->FindProperty(FName(KV.Key)); P && P->Type == EUSType::Int)
				{
					*reinterpret_cast<int32*>(Level->Data + P->Offset) = KV.Value;
				}
			}
		}
		Script->TimeSeconds = TimeSeconds;
		Backend.Time = TimeSeconds;
		FUSEngineProps& P = USProps();
		SyncClientLocations();
		TArray<FUSObject*> Actors = Script->LocalActors;
		const int32 NumLocal = Actors.Num();
		Actors.Append(Script->TimerOnly);
		for (int32 Ai = 0; Ai < Actors.Num(); ++Ai)
		{
			FUSObject* A = Actors[Ai];
			if (A->IsPendingKill())
			{
				continue;
			}
			if (Ai < NumLocal && ClientRuns.Contains(A))
			{
				// spawned by a mod's client code: within its budget
				RunGuarded(A, [&]() { TickActor(A, DeltaSeconds, P); });
				continue;
			}
			float* Rate = P.TimerRate.Ptr<float>(A);
			float* Counter = P.TimerCounter.Ptr<float>(A);
			if (Rate && Counter && *Rate > 0.f)
			{
				*Counter += DeltaSeconds;
				if (*Counter >= *Rate)
				{
					const int32 Ticks = FMath::Max(1, int32(*Counter / *Rate));
					*Counter -= *Rate * Ticks;
					if (!P.bTimerLoop.Get<uint32>(A))
					{
						*Rate = 0.f;
					}
					FUE2NetMirrorWorld::CallEvent(A, TEXT("Timer"));
				}
			}
			if (A->IsPendingKill() || Ai >= NumLocal)
			{
				continue;
			}
			if (A->Class->bHasTick)
			{
				if (FUSFunction* F = USVM::FindFunction(A, TEXT("Tick")))
				{
					FUSParms Parms(F);
					Parms.Float(DeltaSeconds);
					Parms.Call(A);
				}
			}
			USVM::ProcessState(A, DeltaSeconds);
		}
		TickMirroredClients(DeltaSeconds);
		TickHeldScriptWeapon(DeltaSeconds);
		FillModInfoRefs(GetPC());
		if (FUSObject* PC = GetPC())
		{
			SyncPawnWeapon(PC);
		}
		if (Gui)
		{
			Gui->Tick(DeltaSeconds);
		}
		if (FPlatformTime::Seconds() >= NextSummary)
		{
			NextSummary = FPlatformTime::Seconds() + 30.0;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD summary: %s"), *Describe());
		}
		ProcessInteractions(TEXT("Process_Tick"), nullptr, DeltaSeconds);
		SyncClientVisuals();
		ForgetDestroyed();
		Script->CleanupDestroyed();
		TickSeconds += FPlatformTime::Seconds() - T0;
	}

	/** The pawn we look at (followed player or our own), as the HUD's PlayerOwner.ViewTarget. */
	void UpdateViewTarget(FUSObject* PC, APlayerController* UPC)
	{
		FUSObject* Target = nullptr;
		FUE2NetWorld* NW = FUE2NetWorld::Get();
		if (UPC && NW)
		{
			if (const int32 Ch = NW->FindChannelOf(UPC->GetViewTarget()))
			{
				FUSObject* const* Hit = Mirrors.Find(Ch);
				Target = Hit ? *Hit : nullptr;
			}
		}
		if (!Target)
		{
			FUSObject* Pawn = GetObj(PC, TEXT("Pawn"));
			Target = Pawn && !Pawn->IsPendingKill() ? Pawn : PC;
		}
		SetObj(PC, TEXT("ViewTarget"), Target);
		SyncPawnWeapon(PC);
	}

	void SyncPawnWeapon(FUSObject* PC)
	{
		// Pawn.Weapon is not replicated: UE2's client sets it when the weapon comes up (Weapon.ClientWeaponSet / BringUp),
		// which our play layer does (FUE2NetPlay)
		FUSObject* Pawn = GetObj(PC, TEXT("Pawn"));
		if (Pawn && !Pawn->IsPendingKill())
		{
			const FUE2NetPlay* Play = FUE2NetPlay::Get();
			FUSObject* const* Wep = Play && Play->GetCurrentWeaponChannel() ? Mirrors.Find(Play->GetCurrentWeaponChannel()) : nullptr;
			SetObj(Pawn, TEXT("Weapon"), Wep ? *Wep : nullptr);
		}
	}

	void PrepareCanvas(UCanvas* Canvas)
	{
		FUSObject* C = CanvasObj;
		const float W = float(Canvas->SizeX), H = float(Canvas->SizeY);
		auto SetF = [C](const TCHAR* N, float V) { if (FUSProperty* P = C->FindProperty(FName(N))) { *reinterpret_cast<float*>(C->Data + P->Offset) = V; } };
		auto SetI = [C](const TCHAR* N, int32 V) { if (FUSProperty* P = C->FindProperty(FName(N))) { *reinterpret_cast<int32*>(C->Data + P->Offset) = V; } };
		FUE2NetMirrorWorld::CallEvent(C, TEXT("Reset"));
		SetI(TEXT("SizeX"), int32(W));
		SetI(TEXT("SizeY"), int32(H));
		SetF(TEXT("ClipX"), W);
		SetF(TEXT("ClipY"), H);
		SetF(TEXT("OrgX"), 0.f);
		SetF(TEXT("OrgY"), 0.f);
		SetF(TEXT("CurX"), 0.f);
		SetF(TEXT("CurY"), 0.f);
		SetF(TEXT("FontScaleX"), 1.f);
		SetF(TEXT("FontScaleY"), 1.f);
		if (FUSProperty* P = C->FindProperty(TEXT("ColorModulate")))
		{
			FUSPlane& M = *reinterpret_cast<FUSPlane*>(C->Data + P->Offset);
			M.X = M.Y = M.Z = M.W = 1.f;
		}
		if (FUSProperty* P = C->FindProperty(TEXT("Style")))
		{
			C->Data[P->Offset] = STY_Normal;
		}
		if (FUSProperty* P = C->FindProperty(TEXT("DrawColor")))
		{
			*reinterpret_cast<FUSColor*>(C->Data + P->Offset) = FUSColor{ 255, 255, 255, 255 };
		}
		if (!GetObj(C, TEXT("Font")))
		{
			SetObj(C, TEXT("Font"), GetObj(C, TEXT("SmallFont")));
		}
	}

	/**
	 * UE2's renderer, for each pawn on screen with bScriptPostRender: Pawn.PostRender2D(Canvas, ScreenLocX, ScreenLocY)
	 * in place of the native team beacon (Pawn's: the HUD's DrawCustomBeacon). A Freeze Tag pawn sets bScriptPostRender in
	 * its client Tick once its replicated bFrozen is set (Freon_Pawn: the snowflake beacon with the thaw bar), which
	 * the mirror does not run: that line is done here.
	 */
	void PawnPostRender2D()
	{
		FUE2NetWorld* NW = FUE2NetWorld::Get();
		if (!NW || !RTPawn())
		{
			return;
		}
		// a copy: PostRender2D is script, which may destroy actors (Forget changes Mirrors)
		const TArray<TPair<int32, FUSObject*>> Pawns = Mirrors.Array();
		for (const TPair<int32, FUSObject*>& KV : Pawns)
		{
			FUSObject* O = KV.Value;
			if (!O || O == Level || O->IsPendingKill() || !O->Data || !O->Class || !O->Class->IsChildOf(RTPawn()))
			{
				continue;
			}
			FUSProperty* Post = O->FindProperty(FName(TEXT("bScriptPostRender")));
			if (!Post || Post->Type != EUSType::Bool)
			{
				continue;
			}
			uint32& bPost = *reinterpret_cast<uint32*>(O->Data + Post->Offset);
			if (FUSProperty* Frozen = O->FindProperty(FName(TEXT("bFrozen"))); Frozen && Frozen->Type == EUSType::Bool)
			{
				bPost = *reinterpret_cast<const uint32*>(O->Data + Frozen->Offset) != 0 ? 1u : bPost;
			}
			AActor* Native = bPost ? NW->GetNativeActor(KV.Key) : nullptr;
			if (!Native || Native->IsHidden())
			{
				continue;
			}
			const FUSVector Loc = USUnits::ToUE2(Native->GetActorLocation());
			USProps().Location.Set<FUSVector>(O, Loc);
			const FUSVector Screen = Backend.WorldToScreen(Loc);
			if (Screen.Z <= 0.f)
			{
				continue;
			}
			if (FUSFunction* F = USVM::FindFunction(O, TEXT("PostRender2D")))
			{
				FUSParms Parms(F);
				Parms.Object(CanvasObj);
				Parms.Float(Screen.X);
				Parms.Float(Screen.Y);
				Parms.Call(O);
			}
		}
	}

	FDrawResult Draw(AHUD* Hud, UCanvas* Canvas, bool bShowScores)
	{
		FDrawResult R;
		const bool bWantHud = bModHud && !bHudFailed;
		const bool bWantScores = bShowScores && bModScoreboard && !bScoreboardFailed;
		FUSObject* PC = GetPC();
		FUSObject* ScriptHud = GetHud();
		if (!bReady || !Canvas || !PC || !ScriptHud || (!bWantHud && !bWantScores))
		{
			return R;
		}
		APlayerController* UPC = Hud ? Hud->PlayerOwner.Get() : nullptr;
		RebuildPRIArray();
		UpdateViewTarget(PC, UPC);
		PrepareCanvas(Canvas);
		Backend.Canvas = Canvas;
		Backend.PC = UPC;
		USCanvas::SetBackend(&Backend);
		USCanvas::ResetDrawCount();
		const uint64 Aborts = USVM::GStatAborts;
		const uint64 Tokens = USVM::GStatTokens;
		const double T0 = FPlatformTime::Seconds();
		if (bWantHud)
		{
			// HUD.PostRender: the scoreboard (bShowScoreBoard) or the HUD, the HUD's overlays and the controller's overlays
			if (FUSProperty* P = ScriptHud->FindProperty(TEXT("bShowScoreBoard")))
			{
				*reinterpret_cast<uint32*>(ScriptHud->Data + P->Offset) = bWantScores ? 1u : 0u;
			}
			PawnPostRender2D();
			if (FUSFunction* F = USVM::FindFunction(ScriptHud, TEXT("PostRender")))
			{
				FUSParms Parms(F);
				Parms.Object(CanvasObj);
				Parms.Call(ScriptHud);
			}
			// the viewport's interactions draw after the HUD (UTComp's team overlay)
			ProcessInteractions(TEXT("Process_PostRender"), CanvasObj, 0.f);
			const int32 Draws = USCanvas::GetDrawCount();
			const bool bError = USVM::GStatAborts != Aborts;
			HudErrors += bError ? 1 : 0;
			HudEmptyFrames = Draws == 0 ? HudEmptyFrames + 1 : 0;
			++HudFrames;
			HudSeconds += FPlatformTime::Seconds() - T0;
			HudTokens += USVM::GStatTokens - Tokens;
			R.bHud = Draws > 0 && !bError;
			R.bMessages = R.bHud && MessagesFed > 0;
			R.bScoreboard = bWantScores && R.bHud;
			if (HudErrors >= 5 || HudEmptyFrames >= 120 || USVM::GStatTokens - Tokens > 5000000)
			{
				bHudFailed = true;
				UE_LOG(LogUE2Net, Warning, TEXT("UE2NetScriptHUD: the server's HUD %s is left to our HUD (%d script errors, %d frames without drawing, %llu tokens)"),
					*HudClassName, HudErrors, HudEmptyFrames, USVM::GStatTokens - Tokens);
			}
		}
		else if (bWantScores)
		{
			// a stock HUD with a mod scoreboard: HUD.LinkActors, then ScoreBoard.DrawScoreboard as HUD.PostRender calls it
			FUSObject* Score = GetObj(ScriptHud, TEXT("ScoreBoard"));
			FUE2NetMirrorWorld::CallEvent(ScriptHud, TEXT("LinkActors"));
			if (FUSFunction* F = Score ? USVM::FindFunction(Score, TEXT("DrawScoreboard")) : nullptr)
			{
				FUSParms Parms(F);
				Parms.Object(CanvasObj);
				Parms.Call(Score);
			}
			const int32 Draws = USCanvas::GetDrawCount();
			const bool bError = USVM::GStatAborts != Aborts;
			ScoreErrors += bError ? 1 : 0;
			ScoreEmptyFrames = Draws == 0 ? ScoreEmptyFrames + 1 : 0;
			++ScoreFrames;
			ScoreSeconds += FPlatformTime::Seconds() - T0;
			R.bScoreboard = Draws > 0 && !bError;
			if (ScoreErrors >= 5 || ScoreEmptyFrames >= 60)
			{
				bScoreboardFailed = true;
				UE_LOG(LogUE2Net, Warning, TEXT("UE2NetScriptHUD: the server's scoreboard %s is left to our scoreboard (%d script errors, %d frames without drawing)"),
					*ScoreboardClassName, ScoreErrors, ScoreEmptyFrames);
			}
		}
		USCanvas::SetBackend(nullptr);
		Backend.Canvas = nullptr;
		ForgetDestroyed();
		Script->CleanupDestroyed();
		const double Now = FPlatformTime::Seconds();
		if (bLogFrames && Now >= NextLog)
		{
			NextLog = Now + 5.0;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: %s"), *Describe());
			FUSObject* Pawn = GetObj(PC, TEXT("Pawn"));
			FUSObject* VT = GetObj(PC, TEXT("ViewTarget"));
			FUSObject* Wep = GetObj(Pawn, TEXT("Weapon"));
			FUSObject* Inv = GetObj(Pawn, TEXT("Inventory"));
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: view %s, pawn %s (weapon %s, inventory %s), HUD PawnOwner %s"), VT ? *VT->GetName() : TEXT("None"),
				Pawn ? *Pawn->GetName() : TEXT("None"), Wep ? *Wep->GetName() : TEXT("None"), Inv ? *Inv->GetName() : TEXT("None"),
				GetObj(ScriptHud, TEXT("PawnOwner")) ? *GetObj(ScriptHud, TEXT("PawnOwner"))->GetName() : TEXT("None"));
		}
		return R;
	}

	// ---- mods' menus ---------------------------------------------------------------------------------

	/** The viewport's GUIController (UT2004.ini: GUI2K4.UT2K4GUIController), its styles and fonts. */
	void EnsureGui()
	{
		if (Gui || !bMenus || !PlayerObj || !Script)
		{
			return;
		}
		const double T0 = FPlatformTime::Seconds();
		Gui = MakeUnique<FUSGUI>(Script.Get());
		FString Error;
		if (!Gui->Init(PlayerObj, InteractionMaster, Error))
		{
			UE_LOG(LogUE2Net, Warning, TEXT("UE2NetScriptHUD: mods' menus are off (%s)"), *Error);
			Gui.Reset();
			bMenus = false;
			return;
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: mods' menus ready (%.1f ms)"), (FPlatformTime::Seconds() - T0) * 1000.0);
	}

	/** xVoting's pages stay ours (Net/UE2NetMapVote: the mid-game menu's Map Voting tab). */
	static bool IsNativeMenu(const FString& MenuClass)
	{
		return MenuClass.StartsWith(TEXT("xVoting."), ESearchCase::IgnoreCase);
	}

	/**
	 * PlayerController.ClientOpenMenu / ClientReplaceMenu / ClientCloseMenu from the server: run on our controller's mirror,
	 * which opens the page through Player.GUIController as UE2's client does.
	 */
	bool RunMenuFunction(const FUE2NetActor& A, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
	{
		const FName N = Function ? Function->Name : NAME_None;
		if (N != TEXT("ClientOpenMenu") && N != TEXT("ClientReplaceMenu") && N != TEXT("ClientCloseMenu"))
		{
			return false;
		}
		CheckPlayerController();
		FUSObject* PC = GetPC();
		if (!PC || A.Channel != PCChannel || !Gui)
		{
			return false;
		}
		FUSFunction* F = USVM::FindFunction(PC, N);
		if (!F)
		{
			return false;
		}
		const int32 Size = FMath::Max(F->PropertiesSize, 8);
		uint8* Block = static_cast<uint8*>(FMemory::Malloc(Size, 16));
		FMemory::Memzero(Block, Size);
		FString Menu;
		for (FUSProperty* P : F->Parms)
		{
			if (P == F->ReturnProperty)
			{
				continue;
			}
			for (const TPair<FName, FUE2NetValue>& V : Params)
			{
				if (V.Key == P->Name)
				{
					WriteValue(P, Block + P->Offset, V.Value, A.Channel, P->Name.ToString());
					if (P->Type == EUSType::Str && Menu.IsEmpty())
					{
						Menu = *reinterpret_cast<const FString*>(Block + P->Offset);
					}
					break;
				}
			}
		}
		const bool bRun = !IsNativeMenu(Menu);
		if (bRun)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: the server's %s %s"), *N.ToString(), *Menu);
			TGuardValue<FUSObject*> GA(DeliveringActor, PC);
			TGuardValue<FUSFunction*> GF(DeliveringFunction, F);
			USVM::ProcessEvent(PC, F, Block);
		}
		F->DestroyValue(Block);
		FMemory::Free(Block);
		return bRun;
	}

	/** Script's ConsoleCommand: what a mod's code (or a menu) asks of the console. True when handled. */
	bool ScriptConsoleCommand(FUSObject* Self, const FString& InCmd, FString& Out)
	{
		FString Verb, Rest;
		const FString Cmd = InCmd.TrimStartAndEnd();
		if (!Cmd.Split(TEXT(" "), &Verb, &Rest))
		{
			Verb = Cmd;
		}
		Rest.TrimStartAndEndInline();
		auto FindClassNamed = [](const FString& Name) -> FUSClass*
		{
			FUSRuntime& RT = FUSRuntime::Get();
			FUSClass* C = Name.Contains(TEXT(".")) ? RT.FindClass(Name) : RT.FindLoadedClass(FName(*Name));
			return C && C->EnsureLoaded() && C->Defaults.Num() >= C->PropertiesSize ? C : nullptr;
		};
		if (Verb.Equals(TEXT("set"), ESearchCase::IgnoreCase) || Verb.Equals(TEXT("setnopersist"), ESearchCase::IgnoreCase))
		{
			FString Target, Tail, PropName, Value;
			Rest.Split(TEXT(" "), &Target, &Tail);
			Tail.TrimStartAndEndInline();
			if (!Tail.Split(TEXT(" "), &PropName, &Value))
			{
				PropName = Tail;
			}
			Value.TrimStartAndEndInline();
			if (Target.Equals(TEXT("input"), ESearchCase::IgnoreCase))
			{
				// a key bind (UE2's User.ini [Engine.Input]): kept in our per-user ini, run while playing on a UT2004 server
				if (!PropName.IsEmpty())
				{
					ModBinds.Add(PropName.ToLower(), Value);
					FUSRuntime::Get().SetUserConfigValue(ModBindsSection, PropName, Value);
					UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD: the server's mod binds %s to \"%s\""), *PropName, *Value);
				}
				return true;
			}
			if (FUSClass* C = FindClassNamed(Target))
			{
				if (FUSProperty* P = C->FindPropertyByName(FName(*PropName)); P && P->Type != EUSType::Array)
				{
					const TCHAR* In = *Value;
					P->ImportText(In, C->Defaults.GetData() + P->Offset);
				}
			}
			return true;
		}
		if (Verb.Equals(TEXT("get"), ESearchCase::IgnoreCase))
		{
			FString Target, PropName;
			if (Rest.Split(TEXT(" "), &Target, &PropName))
			{
				if (FUSClass* C = FindClassNamed(Target))
				{
					if (FUSProperty* P = C->FindPropertyByName(FName(*PropName.TrimStartAndEnd())); P && P->Type != EUSType::Array)
					{
						P->ExportText(Out, C->Defaults.GetData() + P->Offset, true);
					}
				}
			}
			return true;
		}
		if (Verb.Equals(TEXT("getcurrentres"), ESearchCase::IgnoreCase))
		{
			Out = Gui ? FString::Printf(TEXT("%dx%d"), int32(Gui->ResX), int32(Gui->ResY)) : FString(TEXT("1024x768"));
			return true;
		}
		if (Verb.Equals(TEXT("mutate"), ESearchCase::IgnoreCase))
		{
			if (FUE2NetPlay* P = FUE2NetPlay::Get())
			{
				P->Mutate(Rest);
			}
			return true;
		}
		if (Verb.Equals(TEXT("say"), ESearchCase::IgnoreCase) || Verb.Equals(TEXT("teamsay"), ESearchCase::IgnoreCase))
		{
			if (FUE2NetPlay* P = FUE2NetPlay::Get(); P && !Rest.IsEmpty())
			{
				P->Say(Rest, Verb.Equals(TEXT("teamsay"), ESearchCase::IgnoreCase));
			}
			return true;
		}
		if (Verb.Equals(TEXT("toggleime"), ESearchCase::IgnoreCase) || Verb.Equals(TEXT("setmouse"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		// an exec function of the mod's own code (a menu button that runs "Ready", "mymenu" ...)
		static int32 Depth = 0;
		if (!Self || Depth > 2)
		{
			return false;
		}
		TGuardValue<int32> Guard(Depth, Depth + 1);
		return ConsoleExecImpl(Cmd);
	}

	bool IsMenuOpenImpl() const
	{
		return Gui && Gui->IsOpen();
	}

	bool DrawMenusImpl(AHUD* Hud, UCanvas* Canvas)
	{
		if (!Gui || !Gui->IsOpen() || !Canvas)
		{
			return false;
		}
		PrepareCanvas(Canvas);
		Backend.Canvas = Canvas;
		Backend.PC = Hud ? Hud->PlayerOwner.Get() : nullptr;
		USCanvas::SetBackend(&Backend);
		Gui->Draw(CanvasObj, float(Canvas->SizeX), float(Canvas->SizeY));
		USCanvas::SetBackend(nullptr);
		Backend.Canvas = nullptr;
		ForgetDestroyed();
		Script->CleanupDestroyed();
		return true;
	}

	FString ModBindFor(const FString& KeyName) const
	{
		const FString* Hit = ModBinds.Find(KeyName.ToLower());
		return Hit ? *Hit : FString();
	}

	FString Describe() const
	{
		return FString::Printf(TEXT("script HUD %s%s, scoreboard %s%s; %d actors mirrored (%d skipped, %d anti-cheat left out, %d map actors copied); HUD %d frames %.2f ms avg (%llu tokens avg), scoreboard alone %d frames %.2f ms avg; tick %.1f ms total; %s"),
			*HudClassName, bModHud ? (bHudFailed ? TEXT(" (failed: ours)") : TEXT(" (script)")) : TEXT(" (ours)"), *ScoreboardClassName,
			bModScoreboard ? (bScoreboardFailed ? TEXT(" (failed: ours)") : TEXT(" (script)")) : TEXT(" (ours)"), Mirrored, Skipped, Denied, MapCopies, HudFrames,
			HudFrames ? HudSeconds * 1000.0 / HudFrames : 0.0, HudFrames ? HudTokens / uint64(HudFrames) : 0ull, ScoreFrames,
			ScoreFrames ? ScoreSeconds * 1000.0 / ScoreFrames : 0.0, TickSeconds * 1000.0, *Backend.Describe())
			+ FString::Printf(TEXT("; messages fed to the script HUD %d (%d script errors: ours)"), MessagesFed, MessagesFailed)
			+ FString::Printf(TEXT("; mods' client code: %s, %d replicated actors started, %d client-side actors spawned, %d stopped, %.1f ms total; visuals %s"),
				bClientCode ? TEXT("run") : TEXT("off"), ClientStarted, ClientSpawned, ClientStopped, ClientSeconds * 1000.0, Visuals ? *Visuals->Describe() : TEXT("none"))
			+ FString::Printf(TEXT("; %s"), Gui ? *Gui->Describe() : TEXT("menus off"));
	}
};

// =================================================================================================
// FUE2NetScriptHUD
// =================================================================================================

FUE2NetScriptHUD::FUE2NetScriptHUD(FUE2NetConnection& InConnection, UWorld* InWorld)
	: Impl(MakeUnique<FImpl>(InConnection, InWorld))
{
	GInstance = this;
}

FUE2NetScriptHUD::~FUE2NetScriptHUD()
{
	if (Impl->bReady || Impl->HudFrames || Impl->ScoreFrames)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetScriptHUD summary: %s"), *Impl->Describe());
	}
	if (GInstance == this)
	{
		GInstance = nullptr;
	}
}

FUE2NetScriptHUD* FUE2NetScriptHUD::Get()
{
	return GInstance;
}

void FUE2NetScriptHUD::ActorOpened(const FUE2NetActor& Actor)
{
	Impl->Open(Actor);
}

void FUE2NetScriptHUD::ActorUpdated(const FUE2NetActor& Actor, const TArray<FString>& Changed)
{
	if (Impl->bReady)
	{
		Impl->Update(Actor, Changed);
	}
}

void FUE2NetScriptHUD::ActorClosed(const FUE2NetActor& Actor)
{
	if (Impl->bReady)
	{
		Impl->Close(Actor);
	}
}

void FUE2NetScriptHUD::FunctionCall(const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	if (!Impl->bReady || !Function)
	{
		return;
	}
	// the HUD setup, the menu calls on our controller (PlayerController.ClientOpenMenu ...: mods' menus), and what the
	// server calls on a mod actor running client code (its client functions); everything else the server calls is ours
	// (UE2NetWorld / UE2NetPlay)
	if (Impl->RunMenuFunction(Actor, Function, Params))
	{
		return;
	}
	if (Function->Name != TEXT("ClientSetHUD"))
	{
		Impl->RunClientFunction(Actor, Function, Params);
		return;
	}
	if (Function->Name == TEXT("ClientSetHUD"))
	{
		Impl->CheckPlayerController();
		if (Impl->GetPC() && Actor.Channel == Impl->PCChannel)
		{
			Impl->RunSetHUD(Params);
		}
		else
		{
			Impl->PendingSetHUD = Params;
		}
	}
}

void FUE2NetScriptHUD::Tick(float DeltaSeconds)
{
	Impl->Tick(DeltaSeconds);
}

FUE2NetScriptHUD::FDrawResult FUE2NetScriptHUD::Draw(AHUD* Hud, UCanvas* Canvas, bool bShowScores)
{
	return Impl->Draw(Hud, Canvas, bShowScores);
}

FString FUE2NetScriptHUD::Describe() const
{
	return Impl->Describe();
}

bool FUE2NetScriptHUD::HasClientAttachment(int32 Channel, const FString& ClassPath) const
{
	return Impl->HasClientAttachment(Channel, ClassPath);
}

bool FUE2NetScriptHUD::FeedMessage(int32 Channel, const FString& Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	return Impl->FeedMessage(Channel, Function, Params);
}

bool FUE2NetScriptHUD::IsScriptWeapon(int32 Channel) const { return Impl->bReady && Impl->IsScriptWeaponImpl(Channel); }
void FUE2NetScriptHUD::ScriptWeaponCommand(int32 Channel, EWeaponCommand Command, uint8 Mode) { if (Impl->bReady) Impl->ScriptWeaponCommandImpl(Channel, Command, Mode); }
bool FUE2NetScriptHUD::GetScriptWeaponState(int32 Channel, FScriptWeaponState& Out) const { return Impl->bReady && Impl->GetScriptWeaponStateImpl(Channel, Out); }
void FUE2NetScriptHUD::ScriptWeaponAnimEnd(int32 Channel) { if (Impl->bReady) Impl->ScriptWeaponAnimEndImpl(Channel); }
void FUE2NetScriptHUD::TickStandInWeapon(int32 Channel, bool bHeld0, bool bHeld1, float DeltaSeconds) { if (Impl->bReady) Impl->TickStandInWeaponImpl(Channel, bHeld0, bHeld1, DeltaSeconds); }
void FUE2NetScriptHUD::StandInFired(int32 Channel, uint8 Mode) { if (Impl->bReady) Impl->StandInFiredImpl(Channel, Mode); }
float FUE2NetScriptHUD::GetScriptFireRate(int32 Channel, uint8 Mode) const { return Impl->bReady ? Impl->GetScriptFireRateImpl(Channel, Mode) : -1.f; }
bool FUE2NetScriptHUD::ConsoleExec(const FString& Command, bool bModOnly) { return Impl->bReady && Impl->ConsoleExecImpl(Command, bModOnly); }
bool FUE2NetScriptHUD::IsMenuOpen() const { return Impl->bReady && Impl->IsMenuOpenImpl(); }
FString FUE2NetScriptHUD::GetMenuName() const { return Impl->bReady && Impl->Gui ? Impl->Gui->TopMenuName() : FString(); }
bool FUE2NetScriptHUD::DrawMenus(AHUD* Hud, UCanvas* Canvas) { return Impl->bReady && Impl->DrawMenusImpl(Hud, Canvas); }
bool FUE2NetScriptHUD::MenuKey(uint8 Key, uint8 Action, float Delta) { return Impl->bReady && Impl->Gui && Impl->Gui->KeyEvent(Key, Action, Delta); }
bool FUE2NetScriptHUD::MenuType(uint8 Key, const FString& Unicode) { return Impl->bReady && Impl->Gui && Impl->Gui->KeyType(Key, Unicode); }
void FUE2NetScriptHUD::MenuMouse(float X, float Y) { if (Impl->bReady && Impl->Gui) { Impl->Gui->MouseMove(X, Y); } }
void FUE2NetScriptHUD::CloseMenus() { if (Impl->bReady && Impl->Gui) { Impl->Gui->CloseAll(); } }
bool FUE2NetScriptHUD::OpenMenu(const FString& MenuClass)
{
	FUSObject* C = Impl->bReady && Impl->Gui ? Impl->Gui->GetController() : nullptr;
	FUSFunction* F = C ? USVM::FindFunction(C, TEXT("OpenMenu")) : nullptr;
	if (!F)
	{
		return false;
	}
	FUSParms P(F);
	P.Str(MenuClass);
	P.Call(C);
	return P.ReturnBool();
}
FString FUE2NetScriptHUD::GetModBind(const FString& KeyName) const { return Impl->bReady ? Impl->ModBindFor(KeyName) : FString(); }
void FUE2NetScriptHUD::SetModBind(const FString& KeyName, const FString& Command)
{
	if (Impl->bReady && !KeyName.IsEmpty())
	{
		if (Command.IsEmpty())
		{
			Impl->ModBinds.Remove(KeyName.ToLower());
		}
		else
		{
			Impl->ModBinds.Add(KeyName.ToLower(), Command);
		}
		FUSRuntime::Get().SetUserConfigValue(FImpl::ModBindsSection, KeyName, Command);
	}
}
FString FUE2NetScriptHUD::DumpMenus() const { return Impl->bReady && Impl->Gui ? Impl->Gui->DumpTop() : FString(); }
bool FUE2NetScriptHUD::GetModWeaponAmmo(int32 Channel, int32& OutCur, int32& OutMax, FString& OutText) const
{
	return Impl->bReady && Channel && Impl->GetModWeaponAmmoImpl(Channel, OutCur, OutMax, OutText);
}
FString FUE2NetScriptHUD::DescribePCProperty(const FString& Path) const
{
	FUSObject* O = Impl->bReady ? Impl->GetPC() : nullptr;
	TArray<FString> Parts;
	Path.ParseIntoArray(Parts, TEXT("."));
	FString Out = TEXT("None");
	for (int32 i = 0; O && i < Parts.Num(); ++i)
	{
		if (Parts[i] == TEXT("@state"))
		{
			return O->StateFrame && O->StateFrame->StateNode ? O->StateFrame->StateNode->GetName() : FString(TEXT("(none)"));
		}
		FUSProperty* P = O->Data ? O->FindProperty(FName(*Parts[i])) : nullptr;
		if (!P)
		{
			return FString::Printf(TEXT("(no %s in %s)"), *Parts[i], *O->Class->GetName());
		}
		if (i + 1 < Parts.Num())
		{
			O = P->Type == EUSType::Object ? *reinterpret_cast<FUSObject**>(O->Data + P->Offset) : nullptr;
			continue;
		}
		Out.Reset();
		P->ExportText(Out, O->Data + P->Offset, true);
	}
	return Out;
}
bool FUE2NetScriptHUD::ClickMenu(const FString& Name) { return Impl->bReady && Impl->Gui && Impl->Gui->ClickNamed(Name); }

#else // OPENUT2K4_USCRIPT_ENABLED

struct FUE2NetScriptHUD::FImpl {};
FUE2NetScriptHUD::FUE2NetScriptHUD(FUE2NetConnection&, UWorld*) {}
FUE2NetScriptHUD::~FUE2NetScriptHUD() {}
FUE2NetScriptHUD* FUE2NetScriptHUD::Get() { return nullptr; }
void FUE2NetScriptHUD::ActorOpened(const FUE2NetActor&) {}
void FUE2NetScriptHUD::ActorUpdated(const FUE2NetActor&, const TArray<FString>&) {}
void FUE2NetScriptHUD::ActorClosed(const FUE2NetActor&) {}
void FUE2NetScriptHUD::FunctionCall(const FUE2NetActor&, FUSFunction*, const TArray<TPair<FName, FUE2NetValue>>&) {}
void FUE2NetScriptHUD::Tick(float) {}
FUE2NetScriptHUD::FDrawResult FUE2NetScriptHUD::Draw(AHUD*, UCanvas*, bool) { return FDrawResult(); }
FString FUE2NetScriptHUD::Describe() const { return FString(); }
bool FUE2NetScriptHUD::HasClientAttachment(int32, const FString&) const { return false; }
bool FUE2NetScriptHUD::FeedMessage(int32, const FString&, const TArray<TPair<FName, FUE2NetValue>>&) { return false; }
bool FUE2NetScriptHUD::IsScriptWeapon(int32) const { return false; }
void FUE2NetScriptHUD::ScriptWeaponCommand(int32, EWeaponCommand, uint8) {}
bool FUE2NetScriptHUD::GetScriptWeaponState(int32, FScriptWeaponState&) const { return false; }
void FUE2NetScriptHUD::ScriptWeaponAnimEnd(int32) {}
void FUE2NetScriptHUD::TickStandInWeapon(int32, bool, bool, float) {}
void FUE2NetScriptHUD::StandInFired(int32, uint8) {}
float FUE2NetScriptHUD::GetScriptFireRate(int32, uint8) const { return -1.f; }
bool FUE2NetScriptHUD::ConsoleExec(const FString&, bool) { return false; }
bool FUE2NetScriptHUD::IsMenuOpen() const { return false; }
FString FUE2NetScriptHUD::GetMenuName() const { return FString(); }
bool FUE2NetScriptHUD::DrawMenus(AHUD*, UCanvas*) { return false; }
bool FUE2NetScriptHUD::MenuKey(uint8, uint8, float) { return false; }
bool FUE2NetScriptHUD::MenuType(uint8, const FString&) { return false; }
void FUE2NetScriptHUD::MenuMouse(float, float) {}
void FUE2NetScriptHUD::CloseMenus() {}
bool FUE2NetScriptHUD::OpenMenu(const FString&) { return false; }
FString FUE2NetScriptHUD::GetModBind(const FString&) const { return FString(); }
void FUE2NetScriptHUD::SetModBind(const FString&, const FString&) {}
FString FUE2NetScriptHUD::DumpMenus() const { return FString(); }
bool FUE2NetScriptHUD::GetModWeaponAmmo(int32, int32&, int32&, FString&) const { return false; }
FString FUE2NetScriptHUD::DescribePCProperty(const FString&) const { return FString(); }
bool FUE2NetScriptHUD::ClickMenu(const FString&) { return false; }

#endif // OPENUT2K4_USCRIPT_ENABLED
