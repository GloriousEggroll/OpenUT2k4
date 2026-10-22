#include "UE2NetScriptHUD.h"

#include "../ArenaContent.h"
#include "../ArenaHUD.h"
#include "../ArenaHudFonts.h"
#include "../ArenaRuntimeContent.h"
#include "../ArenaRuntimeMap.h"
#include "../UScript/ArenaUScript.h"
#include "../UScript/UScriptBridge.h"
#include "../UScript/UScriptCanvas.h"
#include "../UScript/UScriptCore.h"
#include "../UScript/UScriptRuntime.h"
#include "../UScript/UScriptWorld.h"
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

	virtual FUSObject* GetLocalPlayerController() override
	{
		return LocalPC && !LocalPC->IsPendingKill() ? LocalPC : nullptr;
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

	virtual bool DestroyActor(FUSObject* Actor) override
	{
		if (!Actor || Actor->IsPendingKill())
		{
			return false;
		}
		CallEvent(Actor, TEXT("Destroyed"));
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

	FImpl(FUE2NetConnection& InConnection, UWorld* InWorld) : Connection(InConnection), World(InWorld)
	{
		FString M;
		if (FParse::Value(FCommandLine::Get(), TEXT("UE2ScriptHUD="), M))
		{
			Mode = M.Equals(TEXT("all"), ESearchCase::IgnoreCase) ? 2 : (M == TEXT("0") || M.Equals(TEXT("off"), ESearchCase::IgnoreCase) ? 0 : 1);
		}
		bLogFrames = FParse::Param(FCommandLine::Get(), TEXT("UE2ScriptHUDLog"));
		if (Mode == 0 || !ArenaUScript::EnsureRuntime())
		{
			return;
		}
		FUSRuntime& RT = FUSRuntime::Get();
		FUSClass* LevelClass = RT.FindClass(TEXT("Engine.LevelInfo"));
		FUSClass* CanvasClass = RT.FindClass(TEXT("Engine.Canvas"));
		FUSClass* PlayerClass = RT.FindClass(TEXT("Engine.Player"));
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
		RT.PlaceholderInit = [this](FUSObject* Obj) { Backend.InitPlaceholder(Obj); };
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
		// the server's Role is our RemoteRole and the other way round
		if (Name == TEXT("Role") || Name == TEXT("RemoteRole"))
		{
			const bool bRole = Name == TEXT("Role");
			(bRole ? USProps().RemoteRole : USProps().Role).Set<uint8>(O, uint8(V->I));
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
		CheckPlayerController();
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
		if (A.Channel == PCChannel && GRI)
		{
			SetObj(*Hit, TEXT("GameReplicationInfo"), GRI);
		}
	}

	void Close(const FUE2NetActor& A)
	{
		FUSObject* O = nullptr;
		if (!Mirrors.RemoveAndCopyValue(A.Channel, O) || !O || O == Level)
		{
			return;
		}
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
		ProcessInteractions(TEXT("Process_Tick"), nullptr, DeltaSeconds);
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
		for (const TPair<int32, FUSObject*>& KV : Mirrors)
		{
			FUSObject* O = KV.Value;
			if (!O || O == Level || !O->Data || !O->Class || !O->Class->IsChildOf(RTPawn()))
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

	FString Describe() const
	{
		return FString::Printf(TEXT("script HUD %s%s, scoreboard %s%s; %d actors mirrored (%d skipped, %d anti-cheat left out, %d map actors copied); HUD %d frames %.2f ms avg (%llu tokens avg), scoreboard alone %d frames %.2f ms avg; tick %.1f ms total; %s"),
			*HudClassName, bModHud ? (bHudFailed ? TEXT(" (failed: ours)") : TEXT(" (script)")) : TEXT(" (ours)"), *ScoreboardClassName,
			bModScoreboard ? (bScoreboardFailed ? TEXT(" (failed: ours)") : TEXT(" (script)")) : TEXT(" (ours)"), Mirrored, Skipped, Denied, MapCopies, HudFrames,
			HudFrames ? HudSeconds * 1000.0 / HudFrames : 0.0, HudFrames ? HudTokens / uint64(HudFrames) : 0ull, ScoreFrames,
			ScoreFrames ? ScoreSeconds * 1000.0 / ScoreFrames : 0.0, TickSeconds * 1000.0, *Backend.Describe());
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
	// only the HUD setup is run on our side; everything else the server calls is ours (UE2NetWorld / UE2NetPlay)
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

#endif // OPENUT2K4_USCRIPT_ENABLED
