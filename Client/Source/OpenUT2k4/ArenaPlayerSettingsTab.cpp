#include "ArenaPlayerSettingsTab.h"
#include "ArenaArt.h"
#include "ArenaContent.h"
#include "ArenaGUI2K4Pages.h"
#include "ArenaHUD.h"
#include "ArenaLocalMessages.h"
#include "ArenaRuntimeContent.h"
#include "ArenaSPData.h"
#include "ArenaUserSettings.h"
#include "OpenUT2k4.h"
#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "CanvasItem.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Engine/PointLight.h"
#include "Components/PointLightComponent.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"
#include "Sound/SoundBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"

using namespace ArenaGUI2K4;

namespace ArenaPlayerSettingsTabLocal
{
	const TCHAR* const Sec = TEXT("UT2K4Tab_PlayerSettings");

	FString L(const TCHAR* Key, const TCHAR* Fallback)
	{
		return ArenaGUI2K4::Loc(TEXT("GUI2K4"), Sec, Key, Fallback);
	}

	FString LA(const TCHAR* Key, int32 Index, const TCHAR* Fallback)
	{
		FString V;
		return ArenaLocalMessages::Localize(TEXT("GUI2K4"), Sec, FString::Printf(TEXT("%s[%d]"), Key, Index), V) && !V.IsEmpty() ? V : FString(Fallback);
	}

	/** A voice pack of the .int Object= lists (MetaClass=XGame.xVoicePack, like PlayerController.GetAllInt). */
	struct FVoice { FString Class; FString Name; FString Gender; };

	const TArray<FVoice>& Voices()
	{
		static TArray<FVoice> List;
		static bool bBuilt = false;
		if (bBuilt)
		{
			return List;
		}
		bBuilt = true;
		TArray<FString> Files;
		const FString Dir = ArenaContent::GetCacheDir() / TEXT("localization");
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.int")), true, false);
		for (const FString& F : Files)
		{
			TArray<FString> Lines;
			FFileHelper::LoadFileToStringArray(Lines, *(Dir / F));
			for (const FString& Line : Lines)
			{
				if (!Line.StartsWith(TEXT("Object=(")) || !Line.Contains(TEXT("MetaClass=XGame.xVoicePack"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				const FString Class = ArenaSP::StructField(Line.Mid(7), TEXT("Name"));
				FString Pkg, Cls;
				if (!Class.Split(TEXT("."), &Pkg, &Cls))
				{
					continue;
				}
				FVoice V;
				V.Class = Class;
				ArenaLocalMessages::Localize(Pkg, Cls, TEXT("VoicePackName"), V.Name);
				if (V.Name.IsEmpty())
				{
					V.Name = Cls;
				}
				V.Gender = ArenaSP::GetStockValue(Class, TEXT("VoiceGender"));
				List.Add(V);
			}
		}
		return List;
	}

	/** UT2K4Tab_PlayerSettings.PreviewVoice: a taunt of the pack (TeamVoicePack.PickCustomTauntFor(PC, true, false, 21):
	 *  from taunt 21 on, no mature ones) played on the interface slot. */
	void PreviewVoice(APlayerController* PC, const FString& VoiceClass)
	{
		if (!PC || VoiceClass.IsEmpty())
		{
			return;
		}
		const int32 NumTaunts = FCString::Atoi(*ArenaSP::GetStockValue(VoiceClass, TEXT("NumTaunts")));
		if (NumTaunts <= 0)
		{
			return;
		}
		int32 Start = 21;
		if (Start >= NumTaunts - 1)
		{
			Start = 0;
		}
		int32 Pick = -1;
		for (int32 Try = 0; Try < 8 && Pick < 0; ++Try)
		{
			const int32 I = Start + FMath::RandRange(0, NumTaunts - Start - 1);
			if (FCString::Atoi(*ArenaSP::GetStockValue(VoiceClass, TEXT("MatureTaunt"), I)) == 1)
			{
				continue;
			}
			Pick = I;
		}
		const FString Sound = ArenaSP::GetStockValue(VoiceClass, TEXT("TauntSound"), Pick < 0 ? Start : Pick);
		if (USoundBase* Snd = Sound.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadSoundByObjectPath(Sound))
		{
			UGameplayStatics::PlaySound2D(PC, Snd);
			UE_LOG(LogOpenUT2k4, Log, TEXT("Settings: voice preview %s (%s)"), *VoiceClass, *Sound);
		}
		else
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("Settings: no preview sound for %s (%s)"), *VoiceClass, *Sound);
		}
	}

	/** TeamVoicePack.VoiceMatchesGender */
	bool VoiceMatches(const FVoice& V, const FString& Sex)
	{
		if (V.Gender.Contains(TEXT("None")) || V.Gender.IsEmpty())
		{
			return true;
		}
		return Sex.StartsWith(TEXT("F"), ESearchCase::IgnoreCase) ? V.Gender.Contains(TEXT("Female")) : !V.Gender.Contains(TEXT("Female"));
	}

	/** UT2k4ModelSelect.IsHiddenCharacter / IsUnlocked: a Menu tag outside IgnoredTypes ("SP") must be unlocked. */
	bool IsUnlocked(const ArenaSP::FPlayerRecord& R)
	{
		static const FString Ignored = []() { const FString V = ArenaSP::GetStockValue(TEXT("GUI2K4.UT2k4ModelSelect"), TEXT("IgnoredTypes")); return V.IsEmpty() ? FString(TEXT("SP")) : V; }();
		if (R.Menu.IsEmpty())
		{
			return true;
		}
		TArray<FString> Tags;
		R.Menu.ParseIntoArray(Tags, TEXT(";"));
		const FString Unlocked = TEXT(";") + UArenaUserSettings::Get()->UnlockedCharacters.ToUpper() + TEXT(";");
		for (const FString& T : Tags)
		{
			if (!(TEXT(";") + Ignored.ToUpper() + TEXT(";")).Contains(TEXT(";") + T.ToUpper() + TEXT(";")) && !Unlocked.Contains(TEXT(";") + T.ToUpper() + TEXT(";")))
			{
				return false;
			}
		}
		return true;
	}

	/** UT2k4ModelSelect.InvalidTypes ("DUP"): records left out of the list. */
	bool IsListed(const ArenaSP::FPlayerRecord& R)
	{
		static const FString Invalid = []() { const FString V = ArenaSP::GetStockValue(TEXT("GUI2K4.UT2k4ModelSelect"), TEXT("InvalidTypes")); return V.IsEmpty() ? FString(TEXT("DUP")) : V; }();
		return R.Menu.IsEmpty() || !(TEXT(";") + Invalid.ToUpper() + TEXT(";")).Contains(TEXT(";") + R.Menu.ToUpper() + TEXT(";"));
	}

	FString PortraitPath(const ArenaSP::FPlayerRecord& R)
	{
		const FString Path = ArenaSP::ResolveObjectPath(R.Portrait);
		const FAnimatedImage& Img = FAnimatedImage::Get(Path);
		return Img.Items.Num() > 0 ? Img.Items[0].Texture : Path;
	}

	/** The 3D view (SpinnyWeap): a skeletal mesh actor far below the menu scene and a scene capture of only it. */
	struct FPreview
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ASkeletalMeshActor> Actor;
		TWeakObjectPtr<ASceneCapture2D> Capture;
		TStrongObjectPtr<UTextureRenderTarget2D> Target;
		/** The capture's alpha (1 = background) as a grey mask, for the two pass draw over the page */
		TStrongObjectPtr<UTextureRenderTarget2D> Mask;
		TArray<TWeakObjectPtr<APointLight>> Lights;
		FString Character;
		int32 SkinMode = -1;
		float Yaw = 0.f;
		float LastMouseX = -1.f;
		bool bValid = false;

		static FVector Origin() { return FVector(0.f, 0.f, -200000.f); }

		void Ensure(UWorld* InWorld)
		{
			if (World.Get() == InWorld && Actor.IsValid() && Capture.IsValid() && Target.IsValid())
			{
				return;
			}
			World = InWorld;
			Character.Reset();
			if (!InWorld)
			{
				return;
			}
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			P.ObjectFlags |= RF_Transient;
			Actor = InWorld->SpawnActor<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(), Origin(), FRotator::ZeroRotator, P);
			Capture = InWorld->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), Origin() + FVector(330.f, 0.f, 0.f), FRotator(0.f, 180.f, 0.f), P);
			Target.Reset(NewObject<UTextureRenderTarget2D>());
			Target->RenderTargetFormat = RTF_RGBA16f;
			Target->ClearColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
			Target->InitAutoFormat(512, 768);
			Mask.Reset(NewObject<UTextureRenderTarget2D>());
			Mask->RenderTargetFormat = RTF_RGBA8;
			Mask->ClearColor = FLinearColor::White;
			Mask->InitAutoFormat(512, 768);
			// SpinnyWeap is lit by the menu's light: a key light from the front above and a dimmer fill from the side
			struct FL { FVector At; float Intensity; };
			for (const FL& L : { FL{ FVector(300.f, 120.f, 160.f), 12000.f }, FL{ FVector(200.f, -260.f, 20.f), 4800.f }, FL{ FVector(-250.f, 0.f, 120.f), 4000.f } })
			{
				if (APointLight* Light = InWorld->SpawnActor<APointLight>(APointLight::StaticClass(), Origin() + L.At, FRotator::ZeroRotator, P))
				{
					UPointLightComponent* LC = Light->PointLightComponent;
					LC->SetMobility(EComponentMobility::Movable);
					LC->SetIntensityUnits(ELightUnits::Unitless);
					LC->SetIntensity(L.Intensity);
					LC->SetAttenuationRadius(900.f);
					LC->SetCastShadows(false);
					Lights.Add(Light);
				}
			}
			if (Capture.IsValid() && Actor.IsValid())
			{
				USceneCaptureComponent2D* C = Capture->GetCaptureComponent2D();
				C->TextureTarget = Target.Get();
				// only the model, lit, with its coverage in alpha (drawn over the page by Draw)
				C->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
				C->ShowFlags.SetPostProcessing(false);
				C->ShowFlags.SetAtmosphere(false);
				C->ShowFlags.SetFog(false);
				C->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
				C->ShowOnlyActors.Add(Actor.Get());
				C->FOVAngle = 40.f;
				C->bCaptureEveryFrame = true;
				C->bAlwaysPersistRenderingState = true;
			}
		}

		void SetCharacter(const ArenaSP::FPlayerRecord* R, int32 InSkinMode)
		{
			if (!Actor.IsValid() || !R || (Character == R->DefaultName && SkinMode == InSkinMode))
			{
				return;
			}
			Character = R->DefaultName;
			SkinMode = InSkinMode;
			bValid = false;
			USkeletalMeshComponent* Comp = Actor->GetSkeletalMeshComponent();
			const ArenaArt::FCharacterModel* Model = ArenaArt::FindCharacterModelForMesh(R->Mesh, R->BodySkin);
			USkeletalMesh* Mesh = Model ? ArenaArt::Load<USkeletalMesh>(Model->Mesh) : nullptr;
			Comp->SetSkeletalMeshAsset(Mesh);
			UE_LOG(LogOpenUT2k4, Log, TEXT("Settings: 3D view of %s: model %s, mesh %s, bounds %s"), *R->DefaultName, Model ? Model->Mesh : TEXT("none"),
				Mesh ? *Mesh->GetName() : TEXT("none"), Mesh ? *Mesh->GetBounds().ToString() : TEXT(""));
			if (!Mesh)
			{
				return;
			}
			// the stock idle (SpinnyDude.LoopAnim('Idle_Rest'))
			UAnimSequence* Idle = nullptr;
			for (const TCHAR* Name : { TEXT("Idle_Rest"), TEXT("Idle_Rifle") })
			{
				UAnimSequence* S = ArenaArt::Load<UAnimSequence>(FString(Model->AnimPrefix) + Name);
				if (S && S->GetSkeleton() == Mesh->GetSkeleton())
				{
					Idle = S;
					break;
				}
			}
			if (Idle)
			{
				Comp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
				Comp->PlayAnimation(Idle, true);
			}
			// the body skin: normal, red (_0) or blue (_1) (co_SkinPreview)
			const FString Suffix = SkinMode == 1 ? TEXT("_0") : (SkinMode == 2 ? TEXT("_1") : TEXT(""));
			if (UTexture* Skin = ArenaArt::LoadBodySkin(FString(Model->BodySkin) + Suffix))
			{
				// the slot named like the body, else slot 0 (as AArenaCharacter::ApplyBodySkin)
				int32 BodySlot = 0;
				for (int32 i = 0; i < Comp->GetNumMaterials(); ++i)
				{
					const UMaterialInterface* M = Mesh->GetMaterials().IsValidIndex(i) ? Mesh->GetMaterials()[i].MaterialInterface.Get() : nullptr;
					if (M && M->GetName().Contains(TEXT("Body")))
					{
						BodySlot = i;
						break;
					}
				}
				Comp->SetMaterial(BodySlot, nullptr);
				if (UMaterialInstanceDynamic* MI = ArenaRuntimeContent::CreateMaterialInstance(Comp->GetMaterial(BodySlot), Comp))
				{
					Comp->SetMaterial(BodySlot, MI);
					MI->SetTextureParameterValue(TEXT("Base"), Skin);
				}
			}
			// stand the model in the middle of the view (its bounds), facing the camera
			const FBoxSphereBounds B = Mesh->GetBounds();
			Actor->SetActorLocation(Origin() - FVector(0.f, 0.f, B.Origin.Z));
			if (Capture.IsValid())
			{
				const float Dist = FMath::Max(150.f, B.BoxExtent.Z * 2.8f);
				Capture->SetActorLocation(Origin() + FVector(Dist, 0.f, 0.f));
			}
			bValid = true;
		}

		void Draw(FGUI& G, const FRect& R)
		{
			if (!bValid || !Target.IsValid() || !Actor.IsValid() || !G.GetCanvas())
			{
				return;
			}
			Actor->SetActorRotation(FRotator(0.f, Yaw, 0.f));
			// the render target keeps its 2:3 aspect inside R
			const float Aspect = 512.f / 768.f;
			float W = R.W, H = R.W / Aspect;
			if (H > R.H)
			{
				H = R.H;
				W = H * Aspect;
			}
			const FVector2D Pos(R.X + (R.W - W) * 0.5f, R.Y + (R.H - H) * 0.5f);
			// the capture's alpha is 1 where nothing was drawn: as a grey mask (alpha only channel view) into Mask
			if (UWorld* W2 = World.Get())
			{
				UCanvas* MaskCanvas = nullptr;
				FVector2D Size;
				FDrawToRenderTargetContext Ctx;
				UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(W2, Mask.Get(), MaskCanvas, Size, Ctx);
				if (MaskCanvas)
				{
					FCanvasTileItem M(FVector2D::ZeroVector, Target->GetResource(), Size, FLinearColor::White);
					M.BlendMode = ESimpleElementBlendMode(SE_BLEND_RGBA_MASK_START + (1 << 3));
					MaskCanvas->DrawItem(M);
				}
				UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(W2, Ctx);
			}
			// over the page: darken where the model is (x mask), then add the model's (premultiplied) colours
			FCanvasTileItem Hold(Pos, Mask->GetResource(), FVector2D(W, H), FLinearColor::White);
			Hold.BlendMode = SE_BLEND_Modulate;
			G.GetCanvas()->DrawItem(Hold);
			FCanvasTileItem Tile(Pos, Target->GetResource(), FVector2D(W, H), FLinearColor::White);
			Tile.BlendMode = SE_BLEND_Additive;
			G.GetCanvas()->DrawItem(Tile);
		}
	};

	struct FState
	{
		bool bLoaded = false;
		bool b3DView = true;
		int32 SkinPreview = 0;
		bool bSelectOpen = false;
		int32 SelectRace = 0;
		int32 SelectIndex = -1;
		FString SelectChar;
		FPreview Preview;
		FPreview SelectPreview;
	};

	FState& State()
	{
		static FState S;
		return S;
	}

	const ArenaSP::FPlayerRecord* CurrentRecord()
	{
		UArenaUserSettings* U = UArenaUserSettings::Get();
		const ArenaSP::FPlayerRecord* R = ArenaSP::FindPlayer(U->PlayerCharacter);
		if (!R || !IsUnlocked(*R))
		{
			R = ArenaSP::FindPlayer(TEXT("Jakob"));
		}
		return R;
	}

	void SetCharacter(const FString& Name)
	{
		UArenaUserSettings* U = UArenaUserSettings::Get();
		const ArenaSP::FPlayerRecord* R = ArenaSP::FindPlayer(Name);
		if (!R || !IsUnlocked(*R))
		{
			return;
		}
		U->PlayerCharacter = R->DefaultName;
		// UpdateURL("Sex", ...)
		U->PlayerSex = R->Sex.StartsWith(TEXT("F"), ESearchCase::IgnoreCase) ? TEXT("F") : TEXT("M");
		// a voice of the other gender goes back to the default (UpdateVoiceOptions)
		for (const FVoice& V : Voices())
		{
			if (V.Class.Equals(U->VoiceType, ESearchCase::IgnoreCase) && !VoiceMatches(V, U->PlayerSex))
			{
				U->VoiceType.Reset();
			}
		}
		U->Save();
		UE_LOG(LogOpenUT2k4, Log, TEXT("Settings: character %s (%s)"), *U->PlayerCharacter, *U->PlayerSex);
	}

	void DrawModelSelect(FGUI& G)
	{
		FState& S = State();
		const ArenaSP::FData& D = ArenaSP::Get();
		// LockedFloatingWindow (0.125, 0.15, 0.74 x 0.7) "Select Character"
		const FRect P = G.Page();
		const FRect Win(P.X + 0.125f * P.W, 0.15f * G.ScreenH(), 0.74f * P.W, 0.7f * G.ScreenH());
		G.Draw().Rect(0.f, 0.f, G.ScreenW(), G.ScreenH(), FLinearColor(0.f, 0.f, 0.f, 0.35f));
		const FRect Frame = Win.Sub(0.f, 0.02f, 1.f, 0.98f);
		if (!G.Image(TEXT("2K4Menus.NewControls.Display1"), Frame, EImgStyle::Stretched, FLinearColor::White))
		{
			G.FallbackBox(Frame, EMenuState::Blurry, false);
		}
		const FRect Title = Win.Sub(0.f, 0.f, 1.f, 0.0625f);
		const FStyle& HS = GetStyle(TEXT("Header"));
		if (!G.StyleImage(HS, EMenuState::Blurry, Title))
		{
			G.FallbackBox(Title, EMenuState::Focused, true);
		}
		G.StyleText(HS, EMenuState::Blurry, EFontScale::Small, ArenaGUI2K4::Loc(TEXT("GUI2K4"), TEXT("UT2k4ModelSelect"), TEXT("WindowName"), TEXT("Select Character")), Title, EAlign::Center);
		// races (co_Race: "All" + every Race)
		TArray<FString> Races = { TEXT("All") };
		for (const ArenaSP::FPlayerRecord& R : D.Players)
		{
			if (IsListed(R) && !R.Race.IsEmpty() && !Races.Contains(R.Race.ToUpper()))
			{
				Races.Add(R.Race.ToUpper());
			}
		}
		TArray<int32> Items;
		for (int32 i = 0; i < D.Players.Num(); ++i)
		{
			if (IsListed(D.Players[i]) && (S.SelectRace == 0 || D.Players[i].Race.ToUpper() == Races[FMath::Clamp(S.SelectRace, 0, Races.Num() - 1)]))
			{
				Items.Add(i);
			}
		}
		// sb_Main (0.04, 0.075, 0.6807 x 0.5559 with the right half padded) captioned with the selection; CharList 4 x 3
		const ArenaSP::FPlayerRecord* Sel = ArenaSP::FindPlayer(S.SelectChar);
		G.Section(Win.Sub(0.04f, 0.075f, 0.480742f, 0.785859f), Sel ? Sel->DefaultName : FString(), true);
		const FRect ListR = Win.Sub(0.102888f, 0.185119f, 0.403407f, 0.658125f);
		const int32 Cols = 4, Rows = 3;
		const float CellW = ListR.W / Cols, CellH = ListR.H / Rows;
		static float Scroll = 0.f;
		const int32 TotalRows = FMath::DivideAndRoundUp(Items.Num(), Cols);
		const uint32 ListId = FGUI::Id(TEXT("ModelSelectList"));
		if (G.CustomWidget(ListId, ListR, true, FString()) != EMenuState::Blurry)
		{
			// wheel scrolling is consumed by the list widget of the GUI; use the arrows below as well
		}
		Scroll = FMath::Clamp(Scroll, 0.f, float(FMath::Max(0, TotalRows - Rows)));
		const int32 FirstRow = int32(Scroll);
		for (int32 r = 0; r < Rows; ++r)
		{
			for (int32 c = 0; c < Cols; ++c)
			{
				const int32 k = (FirstRow + r) * Cols + c;
				if (!Items.IsValidIndex(k))
				{
					continue;
				}
				const ArenaSP::FPlayerRecord& R = D.Players[Items[k]];
				const FRect Cell(ListR.X + c * CellW + CellW * 0.06f, ListR.Y + r * CellH + CellH * 0.04f, CellW * 0.88f, CellH * 0.92f);
				const bool bLocked = !IsUnlocked(R);
				// a locked character shows the LockedImage (PlayerPictures.cDefault)
				G.Image(bLocked ? FString(TEXT("PlayerPictures.cDefault")) : PortraitPath(R), Cell, EImgStyle::Scaled, FLinearColor::White);
				const uint32 Id = FGUI::Id(TEXT("ModelCell"), Items[k]);
				G.CustomWidget(Id, Cell, true, R.DefaultName);
				if (S.SelectChar.Equals(R.DefaultName, ESearchCase::IgnoreCase))
				{
					G.Draw().Outline(Cell.X, Cell.Y, Cell.W, Cell.H, FArenaDraw::UTColor(FColor(255, 200, 0)), FMath::Max(1.f, G.L(2.f)));
				}
				if (G.WasClicked(Id, Cell))
				{
					S.SelectChar = R.DefaultName;
				}
			}
		}
		if (G.Button(FGUI::Id(TEXT("ModelUp")), FRect(ListR.X - G.L(28.f), ListR.Y, G.L(24.f), G.L(24.f)), TEXT("ArrowLeft"), FString(), FString(), Scroll > 0.f))
		{
			Scroll -= 1.f;
		}
		if (G.Button(FGUI::Id(TEXT("ModelDown")), FRect(ListR.X - G.L(28.f), ListR.B() - G.L(24.f), G.L(24.f), G.L(24.f)), TEXT("ArrowRight"), FString(), FString(),
			Scroll < TotalRows - Rows))
		{
			Scroll += 1.f;
		}
		// iBK: the 3D view of the selection
		const FRect Bk = Win.Sub(0.530206f, 0.075f, 0.368945f, 0.745f);
		G.Image(TEXT("2K4Menus.Controls.buttonSquare_b"), Bk, EImgStyle::Stretched, FLinearColor(1.f, 1.f, 1.f, 0.5f));
		if (Sel && IsUnlocked(*Sel))
		{
			if (APlayerController* PC = G.GetPC())
			{
				S.SelectPreview.Ensure(PC->GetWorld());
				S.SelectPreview.SetCharacter(Sel, 0);
				S.SelectPreview.Yaw = 20.f * FMath::Sin(G.Time() * 0.5);
				S.SelectPreview.Draw(G, Bk.Inset(G.L(6.f), G.L(6.f), G.L(6.f), G.L(6.f)));
			}
		}
		const float RowH = FMath::RoundToFloat(G.ScreenH() * 0.03f);
		int32 Race = S.SelectRace;
		const FRect RaceR = Win.Sub(0.052733f, 0.88f, 0.388155f, 0.f);
		if (G.Combo(FGUI::Id(TEXT("ModelRace")), FRect(RaceR.X, RaceR.Y, RaceR.W, RowH), ArenaGUI2K4::Loc(TEXT("GUI2K4"), TEXT("UT2k4ModelSelect"), TEXT("CharRace.Caption"), TEXT("Race")),
			Race, Races, ArenaGUI2K4::Loc(TEXT("GUI2K4"), TEXT("UT2k4ModelSelect"), TEXT("CharRace.Hint"), TEXT("Filter the available characters by race.")), 0.25f))
		{
			S.SelectRace = Race;
			Scroll = 0.f;
		}
		const bool bOk = G.Button(FGUI::Id(TEXT("ModelOk")), Win.Sub(0.742188f, 0.872397f, 0.159649f, 0.06f), TEXT("SquareButton"), TEXT("OK"), FString(),
			Sel && IsUnlocked(*Sel), EFontScale::Small);
		const bool bCancel = G.Button(FGUI::Id(TEXT("ModelCancel")), Win.Sub(0.512695f, 0.872397f, 0.159649f, 0.06f), TEXT("SquareButton"), TEXT("Cancel"), FString(), true,
			EFontScale::Small) || G.BackPressed();
		if (bOk)
		{
			SetCharacter(S.SelectChar);
		}
		if (bOk || bCancel)
		{
			S.bSelectOpen = false;
			G.ResetInteraction();
		}
	}
}

using namespace ArenaPlayerSettingsTabLocal;

bool ArenaPlayerSettingsTab::IsModalOpen()
{
	return State().bSelectOpen;
}

void ArenaPlayerSettingsTab::ResetDefaults()
{
	UArenaUserSettings* U = UArenaUserSettings::Get();
	U->PlayerName = TEXT("Player");
	U->PreferredTeam = 2;
	U->WeaponHand = 2;
	U->bSmallWeapons = false;
	U->VoiceType.Reset();
	U->Save();
	SetCharacter(TEXT("Jakob"));
}

void ArenaPlayerSettingsTab::ApplyTestCommand(const FString& Name, const FString& Value)
{
#if !UE_BUILD_SHIPPING
	if (Name == TEXT("PlayerChar"))
	{
		SetCharacter(Value);
	}
	else if (Name == TEXT("PlayerSelect"))
	{
		State().bSelectOpen = Value != TEXT("0");
		State().SelectChar = UArenaUserSettings::Get()->PlayerCharacter;
	}
	else if (Name == TEXT("Player3D"))
	{
		State().b3DView = Value != TEXT("0");
	}
	else if (Name == TEXT("PlayerLight"))
	{
		// test: scale the preview lights (PlayerLight=<scale>) or pick the capture source (PlayerLight=src<n>)
		FPreview& P = State().Preview;
		if (Value.StartsWith(TEXT("src")) && P.Capture.IsValid())
		{
			P.Capture->GetCaptureComponent2D()->CaptureSource = ESceneCaptureSource(FCString::Atoi(*Value + 3));
		}
		else
		{
			for (const TWeakObjectPtr<APointLight>& L : P.Lights)
			{
				if (L.IsValid())
				{
					L->PointLightComponent->SetIntensity(L->PointLightComponent->Intensity * FCString::Atof(*Value));
				}
			}
		}
	}
	else if (Name == TEXT("PlayerCapDebug"))
	{
		FPreview& P = State().Preview;
		if (P.Capture.IsValid() && P.Actor.IsValid())
		{
			P.Capture->GetCaptureComponent2D()->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
			UE_LOG(LogOpenUT2k4, Log, TEXT("Settings: capture at %s rot %s, actor at %s, comp %s visible %d registered %d"), *P.Capture->GetActorLocation().ToString(),
				*P.Capture->GetActorRotation().ToString(), *P.Actor->GetActorLocation().ToString(), *P.Actor->GetSkeletalMeshComponent()->Bounds.ToString(),
				P.Actor->GetSkeletalMeshComponent()->IsVisible(), P.Actor->GetSkeletalMeshComponent()->IsRegistered());
		}
	}
	else if (Name == TEXT("PlayerVoice"))
	{
		UArenaUserSettings::Get()->VoiceType = Value;
		if (GEngine)
		{
			for (const FWorldContext& C : GEngine->GetWorldContexts())
			{
				if (APlayerController* PC = C.World() ? C.World()->GetFirstPlayerController() : nullptr)
				{
					PreviewVoice(PC, Value);
					break;
				}
			}
		}
	}
	else if (Name == TEXT("PlayerHand"))
	{
		UArenaUserSettings::Get()->WeaponHand = FMath::Clamp(FCString::Atoi(*Value), 0, 3);
	}
	else if (Name == TEXT("PlayerSmall"))
	{
		UArenaUserSettings::Get()->bSmallWeapons = Value != TEXT("0");
	}
	else if (Name == TEXT("PlayerSkin"))
	{
		State().SkinPreview = FCString::Atoi(*Value);
	}
#endif
}

void ArenaPlayerSettingsTab::Draw(FGUI& G, const FRect& Panel)
{
	FState& S = State();
	UArenaUserSettings* U = UArenaUserSettings::Get();
	const ArenaSP::FPlayerRecord* Rec = CurrentRecord();
	if (S.bSelectOpen)
	{
		// the Change Character window is modal: only it takes input
		DrawModelSelect(G);
		return;
	}

	// PlayerBK1 "3D View" / "Portrait"
	const FRect BK1 = G.Section(Panel.Sub(0.004063f, 0.017969f, 0.446758f, 0.963631f), S.b3DView ? L(TEXT("Show3DViewCaption"), TEXT("3D View")) : L(TEXT("ShowBioCaption"), TEXT("Portrait")));
	(void)BK1;
	if (S.b3DView)
	{
		const FRect View = Panel.Sub(0.013071f, 0.154426f, 0.427141f, 0.738132f);
		if (APlayerController* PC = G.GetPC())
		{
			S.Preview.Ensure(PC->GetWorld());
			S.Preview.SetCharacter(Rec, S.SkinPreview);
			// DropTarget: drag to turn the model
			const uint32 DragId = FGUI::Id(TEXT("PlayerSpin"));
			const EMenuState St = G.CustomWidget(DragId, View, true, FString());
			if (St == EMenuState::Pressed)
			{
				if (S.Preview.LastMouseX >= 0.f)
				{
					S.Preview.Yaw += (G.Mouse().X - S.Preview.LastMouseX) * 0.6f;
				}
				S.Preview.LastMouseX = G.Mouse().X;
			}
			else
			{
				S.Preview.LastMouseX = -1.f;
			}
			S.Preview.Draw(G, View);
		}
		// co_SkinPreview: normal / red / blue skin
		TArray<FString> Skins = { LA(TEXT("Previews"), 0, TEXT("View Normal Skin")), LA(TEXT("Previews"), 1, TEXT("View Red Skin")), LA(TEXT("Previews"), 2, TEXT("View Blue Skin")) };
		const FRect SkR = Panel.Sub(0.053531f, 0.11147f, 0.346258f, 0.f);
		int32 Skin = S.SkinPreview;
		if (G.Combo(FGUI::Id(TEXT("PlayerSkinPreview")), FRect(SkR.X, SkR.Y, SkR.W, FMath::RoundToFloat(G.ScreenH() * 0.03f)), FString(), Skin, Skins,
			L(TEXT("SkinPreview.Hint"), TEXT("Show how the model looks using the selected skin.")), 0.f))
		{
			S.SkinPreview = Skin;
		}
	}
	else if (Rec)
	{
		G.Image(PortraitPath(*Rec), Panel.Sub(0.057016f, 0.094895f, 0.334368f, 0.798132f), EImgStyle::Scaled, FLinearColor::White);
	}
	if (G.Button(FGUI::Id(TEXT("Player3DView")), Panel.Sub(0.043685f, 0.901559f, 0.13072f, 0.05f), TEXT("SquareButton"),
		S.b3DView ? L(TEXT("ShowBioCaption"), TEXT("Portrait")) : L(TEXT("Show3DViewCaption"), TEXT("3D View")),
		L(TEXT("Player3DView.Hint"), TEXT("Toggle between 3D view and portrait of character.")), true, EFontScale::Small))
	{
		S.b3DView = !S.b3DView;
	}
	if (G.Button(FGUI::Id(TEXT("PlayerPick")), Panel.Sub(0.177174f, 0.901559f, 0.233399f, 0.05f), TEXT("SquareButton"), L(TEXT("bPickModel.Caption"), TEXT("Change Character")),
		L(TEXT("bPickModel.Hint"), TEXT("Select a new Character.")), true, EFontScale::Small))
	{
		S.bSelectOpen = true;
		S.SelectChar = Rec ? Rec->DefaultName : FString();
	}

	// PlayerBK2 "Misc.": name, small weapons, preferred team, weapon hand, voice type, FOV (ManageComponent order)
	const FRect BK2 = G.Section(Panel.Sub(0.463047f, 0.017969f, 0.531719f, 0.573006f), L(TEXT("PlayerBK2.Caption"), TEXT("Misc.")));
	const float RowH = FMath::RoundToFloat(G.ScreenH() * 0.03f), Step = FMath::RoundToFloat(G.ScreenH() * 0.045f);
	float Y = BK2.Y + G.L(6.f);
	const float X = BK2.X + BK2.W * 0.05f, W = BK2.W * 0.9f;
	auto Row = [&]() { const FRect R(X, Y, W, RowH); Y += Step; return R; };
	FString Name = U->PlayerName;
	if (G.EditBox(FGUI::Id(TEXT("PlayerNameEdit")), Row(), L(TEXT("PlayerName.Caption"), TEXT("Name")), Name, L(TEXT("PlayerName.Hint"), TEXT("Changes the alias you play as.")), 0.5f, 16))
	{
		U->PlayerName = Name.Replace(TEXT("\""), TEXT("")).Replace(TEXT(" "), TEXT("_"));
		U->Save();
	}
	bool bSmall = U->bSmallWeapons;
	if (G.CheckBox(FGUI::Id(TEXT("PlayerSmallWeap")), Row(), L(TEXT("PlayerSmallWeap.Caption"), TEXT("Small Weapons")), bSmall,
		L(TEXT("PlayerSmallWeap.Hint"), TEXT("Makes your first person weapon smaller.")), 0.94f))
	{
		U->bSmallWeapons = bSmall;
		U->Save();
	}
	TArray<FString> Teams = { LA(TEXT("TeamNames"), 0, TEXT("Red")), LA(TEXT("TeamNames"), 1, TEXT("Blue")), LA(TEXT("TeamNames"), 2, TEXT("None")) };
	int32 Team = FMath::Clamp(U->PreferredTeam, 0, 2);
	if (G.Combo(FGUI::Id(TEXT("PlayerTeam")), Row(), L(TEXT("PlayerTeam.Caption"), TEXT("Preferred Team")), Team, Teams,
		L(TEXT("PlayerTeam.Hint"), TEXT("Changes the team you will play on by default.")), 0.7f))
	{
		U->PreferredTeam = Team;
		U->Save();
	}
	TArray<FString> Hands = { LA(TEXT("HandNames"), 0, TEXT("Left")), LA(TEXT("HandNames"), 1, TEXT("Center")), LA(TEXT("HandNames"), 2, TEXT("Right")), LA(TEXT("HandNames"), 3, TEXT("Hidden")) };
	int32 Hand = FMath::Clamp(U->WeaponHand, 0, 3);
	if (G.Combo(FGUI::Id(TEXT("PlayerHand")), Row(), L(TEXT("PlayerHand.Caption"), TEXT("Weapon Hand")), Hand, Hands,
		L(TEXT("PlayerHand.Hint"), TEXT("Changes whether your weapon is visible.")), 0.7f))
	{
		U->WeaponHand = Hand;
		U->Save();
	}
	// voice types matching the character's gender (UpdateVoiceOptions), "Default" first
	TArray<FString> VoiceItems = { L(TEXT("DefaultText"), TEXT("Default")) };
	TArray<FString> VoiceClasses = { FString() };
	for (const FVoice& V : Voices())
	{
		if (VoiceMatches(V, Rec ? Rec->Sex : U->PlayerSex))
		{
			VoiceItems.Add(V.Name);
			VoiceClasses.Add(V.Class);
		}
	}
	int32 Voice = FMath::Max(0, VoiceClasses.IndexOfByPredicate([U](const FString& C) { return C.Equals(U->VoiceType, ESearchCase::IgnoreCase); }));
	if (G.Combo(FGUI::Id(TEXT("PlayerVoice")), Row(), L(TEXT("VoiceType.Caption"), TEXT("Voice Type")), Voice, VoiceItems,
		L(TEXT("VoiceType.Hint"), TEXT("Choose how your character's voice will sound in the game.")), 0.5f) && VoiceClasses.IsValidIndex(Voice))
	{
		U->VoiceType = VoiceClasses[Voice];
		U->Save();
		PreviewVoice(G.GetPC(), U->VoiceType);
	}
	// Default FOV (our 4:3 field of view setting)
	const FName FovId(TEXT("FOV"));
	const int32 Dir = G.Spinner(FGUI::Id(TEXT("PlayerFOV")), Row(), L(TEXT("PlayerFOV.Caption"), TEXT("Default FOV")), U->GetSettingValueText(FovId),
		L(TEXT("PlayerFOV.Hint"), TEXT("This value will change your field of view while playing.")), 0.7f);
	if (Dir != 0)
	{
		U->ChangeSetting(FovId, Dir);
	}

	// PlayerBK3 "Biography": the character's DecoText
	const FRect BK3 = G.Section(Panel.Sub(0.463047f, 0.610417f, 0.531719f, 0.372811f), L(TEXT("PlayerBK3.Caption"), TEXT("Biography")));
	if (Rec)
	{
		G.ScrollText(FGUI::Id(TEXT("PlayerBio")), BK3.Inset(G.L(4.f), G.L(2.f), G.L(4.f), G.L(2.f)), ArenaSP::CharacterText(*Rec).Replace(TEXT("|"), TEXT("\n")));
	}

}
