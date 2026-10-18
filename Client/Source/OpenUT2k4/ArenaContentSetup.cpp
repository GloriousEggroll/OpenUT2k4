#include "ArenaContentSetup.h"
#include "OpenUT2k4.h"
#include "ArenaArtHUD.h"
#include "ArenaBranding.h"
#include "ArenaContent.h"
#include "ArenaFolderPicker.h"
#include "ArenaHUD.h"
#include "ArenaLevelBuilder.h"
#include "ArenaMenuGameMode.h"
#include "ArenaRuntimeContent.h"
#include "ArenaUserSettings.h"
#include "Convert/ArenaConverter.h"
#include "Containers/Ticker.h"
#include "UnrealClient.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/Events.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SLeafWidget.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Invisible 1x1 widget in the game viewport that owns keyboard focus while the path field is edited, so typed
 * characters arrive as real text (keyboard layout, dead keys and IME handled by the platform), and forwards keys
 * and characters to the setup screen.
 */
class SArenaSetupTextSink : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SArenaSetupTextSink) {}
	SLATE_END_ARGS()

	TWeakObjectPtr<AArenaContentSetupHUD> Owner;

	void Construct(const FArguments&) {}

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1.f, 1.f); }
	virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&, int32 LayerId, const FWidgetStyle&, bool) const override
	{
		return LayerId;
	}

	virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
	{
		if (Owner.IsValid() && Owner->IsEditingPath())
		{
			Owner->OnPathKey(Event);
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnKeyUp(const FGeometry&, const FKeyEvent&) override
	{
		return Owner.IsValid() && Owner->IsEditingPath() ? FReply::Handled() : FReply::Unhandled();
	}

	virtual FReply OnKeyChar(const FGeometry&, const FCharacterEvent& Event) override
	{
		if (!Owner.IsValid() || !Owner->IsEditingPath())
		{
			return FReply::Unhandled();
		}
		if (!Event.GetModifierKeys().IsControlDown() && !Event.GetModifierKeys().IsCommandDown())
		{
			Owner->OnPathChar(Event.GetCharacter());
		}
		return FReply::Handled();
	}
};

namespace ArenaContentSetupLocal
{
	const FLinearColor White = ArenaSetupStyle::White();
	const FLinearColor Dim = ArenaSetupStyle::Dim();
	const FLinearColor Accent = ArenaSetupStyle::Accent();
	const FLinearColor Hot = ArenaSetupStyle::Hot();
	const FLinearColor Good = ArenaSetupStyle::Good();
	const FLinearColor Bad = ArenaSetupStyle::Bad();

	FLinearColor Alpha(const FLinearColor& C, float A) { return FLinearColor(C.R, C.G, C.B, C.A * A); }

	bool AutoConvert() { return FParse::Param(FCommandLine::Get(), TEXT("AutoConvert")); }

	TArray<FArenaContentSetupExtension>& Extensions()
	{
		static TArray<FArenaContentSetupExtension> List;
		return List;
	}

	bool AnyExtensionBusy()
	{
		for (const FArenaContentSetupExtension& E : Extensions())
		{
			if (E.IsBusy && E.IsBusy()) return true;
		}
		return false;
	}

	/** cache.json counts as a friendly summary line ("239 maps, 16,020 textures, ..."). */
	FString CacheSummary()
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(ArenaContent::GetCacheDir() / TEXT("cache.json"))))
		{
			return FString();
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			return FString();
		}
		const TSharedPtr<FJsonObject>* Counts = nullptr;
		if (!Root->TryGetObjectField(TEXT("counts"), Counts))
		{
			return FString();
		}
		auto N = [&Counts](const TCHAR* K) { int32 V = 0; (*Counts)->TryGetNumberField(K, V); return V; };
		auto Num = [](int32 V, const TCHAR* One, const TCHAR* Many) { return FText::AsNumber(V).ToString() + TEXT(" ") + (V == 1 ? One : Many); };
		return FString::Printf(TEXT("%s, %s, %s, %s and %s"),
			*Num(N(TEXT("data_maps")), TEXT("map"), TEXT("maps")), *Num(N(TEXT("textures")) + N(TEXT("baked_textures")), TEXT("texture"), TEXT("textures")),
			*Num(N(TEXT("staticmeshes")) + N(TEXT("skeletalmeshes")) + N(TEXT("vertmeshes")), TEXT("model"), TEXT("models")),
			*Num(N(TEXT("sounds")), TEXT("sound"), TEXT("sounds")), *Num(N(TEXT("music")), TEXT("music track"), TEXT("music tracks")));
	}

	bool HasPartialCache()
	{
		return IFileManager::Get().FileExists(*(ArenaContent::GetCacheDir() / TEXT("convert_state.json")));
	}

	FString FormatTime(double Seconds)
	{
		const int32 S = FMath::Max(0, FMath::RoundToInt(Seconds));
		return S >= 60 ? FString::Printf(TEXT("%d:%02d"), S / 60, S % 60) : FString::Printf(TEXT("%d s"), S);
	}

	FString Plural(int32 N, const TCHAR* One, const TCHAR* Many)
	{
		return FString::Printf(TEXT("%d %s"), N, N == 1 ? One : Many);
	}

	/** Shortens a path in the middle to fit Width ("/home/me/.../Unreal Tournament 2004"). */
	FString FitPath(const FArenaDraw& D, const FString& Path, float Size, float Width)
	{
		if (D.TextSize(Path, Size).X <= Width || Path.Len() < 8)
		{
			return Path;
		}
		int32 Keep = Path.Len() - 3;
		while (Keep > 6)
		{
			const int32 Head = Keep / 3, Tail = Keep - Keep / 3;
			const FString S = Path.Left(Head) + TEXT("...") + Path.Right(Tail);
			if (D.TextSize(S, Size).X <= Width)
			{
				return S;
			}
			Keep -= FMath::Max(1, Keep / 12);
		}
		return Path.Right(6);
	}

	/** Word wrap; words wider than a line (paths) are split anywhere. */
	TArray<FString> Wrap(const FArenaDraw& D, const FString& Text, float Size, float Width)
	{
		TArray<FString> Lines;
		TArray<FString> Paragraphs;
		Text.ParseIntoArray(Paragraphs, TEXT("\n"), false);
		for (const FString& Para : Paragraphs)
		{
			TArray<FString> Words;
			Para.ParseIntoArray(Words, TEXT(" "), true);
			FString Cur;
			for (FString Word : Words)
			{
				const FString Try = Cur.IsEmpty() ? Word : Cur + TEXT(" ") + Word;
				if (D.TextSize(Try, Size).X <= Width)
				{
					Cur = Try;
					continue;
				}
				if (!Cur.IsEmpty())
				{
					Lines.Add(Cur);
					Cur.Reset();
				}
				while (D.TextSize(Word, Size).X > Width && Word.Len() > 1)
				{
					int32 N = Word.Len() - 1;
					while (N > 1 && D.TextSize(Word.Left(N), Size).X > Width)
					{
						N = FMath::Max(1, N - FMath::Max(1, N / 10));
					}
					Lines.Add(Word.Left(N));
					Word.RightChopInline(N);
				}
				Cur = Word;
			}
			Lines.Add(Cur);
		}
		return Lines;
	}

	/** Case-insensitive names of the sub folders of Dir. */
	TArray<FString> SubDirs(const FString& Dir)
	{
		TArray<FString> Out;
		IFileManager::Get().IterateDirectory(*Dir, [&Out](const TCHAR* Path, bool bIsDir)
		{
			if (bIsDir)
			{
				Out.Add(FPaths::GetCleanFilename(Path));
			}
			return true;
		});
		Out.Sort([](const FString& A, const FString& B)
		{
			// folders that look like the game first
			const bool bA = A.Contains(TEXT("2004")) || A.Contains(TEXT("Tournament"));
			const bool bB = B.Contains(TEXT("2004")) || B.Contains(TEXT("Tournament"));
			return bA != bB ? bA : A < B;
		});
		return Out;
	}

	bool HasSubDir(const TArray<FString>& Names, const TCHAR* Name)
	{
		return Names.ContainsByPredicate([Name](const FString& N) { return N.Equals(Name, ESearchCase::IgnoreCase); });
	}

	/** Quotes, file:// URLs, ~ and relative paths (relative to the home folder) to an absolute folder path. */
	FString CleanPath(const FString& In)
	{
		FString S = In.TrimStartAndEnd();
		if (S.Len() >= 2 && ((S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\""))) || (S.StartsWith(TEXT("'")) && S.EndsWith(TEXT("'")))))
		{
			S = S.Mid(1, S.Len() - 2);
		}
		if (S.StartsWith(TEXT("file://"), ESearchCase::IgnoreCase))
		{
			S.RightChopInline(7);
			FString Decoded;
			for (int32 i = 0; i < S.Len(); ++i)
			{
				if (S[i] == TEXT('%') && i + 2 < S.Len() && FChar::IsHexDigit(S[i + 1]) && FChar::IsHexDigit(S[i + 2]))
				{
					Decoded.AppendChar(TCHAR(FParse::HexDigit(S[i + 1]) * 16 + FParse::HexDigit(S[i + 2])));
					i += 2;
				}
				else
				{
					Decoded.AppendChar(S[i]);
				}
			}
			S = Decoded;
		}
		const FString Home = FPlatformProcess::UserHomeDir();
		if (S == TEXT("~") || S.StartsWith(TEXT("~/")))
		{
			S = Home / S.Mid(FMath::Min(2, S.Len()));
		}
		if (S.IsEmpty())
		{
			return S;
		}
		FPaths::NormalizeDirectoryName(S);
		if (FPaths::IsRelative(S))
		{
			S = Home / S;
		}
		S = FPaths::ConvertRelativePathToFull(S);
		FPaths::CollapseRelativeDirectories(S);
		while (S.Len() > 1 && S.EndsWith(TEXT("/")) && !S.EndsWith(TEXT(":/")))
		{
			S.LeftChopInline(1);
		}
		return S;
	}

	/** Binds the main menu hook and opens the screen automatically when the game cannot play yet. */
	struct FRegister
	{
		TWeakObjectPtr<UWorld> HandledWorld;

		FRegister()
		{
			AArenaMenuHUD::OnOpenContentSetup() = [](AArenaMenuHUD* Menu)
			{
				AArenaContentSetupHUD::Open(Menu ? Menu->GetOwningPlayerController() : nullptr);
			};
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FRegister::Tick), 0.25f);
		}

		TArray<float> Shots;
		int32 NextShot = 0;
		double StartTime = 0.0;

		bool Tick(float)
		{
			bool bForceOpen = false;
#if !UE_BUILD_SHIPPING
			// tests: -AutoSetupShots=<sec>,<sec>... screenshots (Saved/Screenshots/setup_N.png), -AutoSetupQuit=<sec>,
			// -SetupOpen opens the screen even when the cache is ready
			if (StartTime == 0.0)
			{
				StartTime = FPlatformTime::Seconds();
				FString List;
				if (FParse::Value(FCommandLine::Get(), TEXT("AutoSetupShots="), List, false))
				{
					TArray<FString> Parts;
					List.ParseIntoArray(Parts, TEXT(","), true);
					for (const FString& P : Parts) Shots.Add(FCString::Atof(*P));
				}
			}
			const double Now = FPlatformTime::Seconds() - StartTime;
			if (Shots.IsValidIndex(NextShot) && Now >= Shots[NextShot])
			{
				const FString File = FPaths::ScreenShotDir() / FString::Printf(TEXT("setup_%d.png"), NextShot);
				FScreenshotRequest::RequestScreenshot(File, false, false);
				UE_LOG(LogOpenUT2k4, Log, TEXT("AutoSetupShots: %s"), *File);
				++NextShot;
			}
			float QuitAt = 0.f;
			if (FParse::Value(FCommandLine::Get(), TEXT("AutoSetupQuit="), QuitAt) && Now >= QuitAt)
			{
				FPlatformMisc::RequestExit(false, TEXT("AutoSetupQuit"));
			}
			bForceOpen = FParse::Param(FCommandLine::Get(), TEXT("SetupOpen"));
#endif
			if (!GEngine || !(bForceOpen || AArenaContentSetupHUD::IsSetupNeeded()))
			{
				return true;
			}
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				UWorld* World = Ctx.World();
				if (!World || !(Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE) || HandledWorld.Get() == World)
				{
					continue;
				}
				APlayerController* PC = World->GetFirstPlayerController();
				if (PC && Cast<AArenaMenuHUD>(PC->GetHUD()))
				{
					HandledWorld = World;
					UE_LOG(LogOpenUT2k4, Log, TEXT("Content setup: the content cache %s is not ready, opening the setup screen"), *ArenaContent::GetCacheDir());
					AArenaContentSetupHUD::Open(PC);
				}
			}
			return true;
		}
	};
	static FRegister GRegister;
}

using namespace ArenaContentSetupLocal;

// =============================================================================================
// Extensions, install checks
// =============================================================================================

void AArenaContentSetupHUD::RegisterExtension(const FArenaContentSetupExtension& Extension)
{
	Extensions().RemoveAll([&Extension](const FArenaContentSetupExtension& E) { return E.Id == Extension.Id; });
	Extensions().Add(Extension);
}

bool AArenaContentSetupHUD::ContinueWithInstall(const FString& Dir)
{
	if (!ArenaContent::IsUT2004Install(Dir) || FArenaConverter::Get().IsRunning())
	{
		return false;
	}
	UArenaUserSettings* S = UArenaUserSettings::Get();
	S->UT2004Dir = FPaths::ConvertRelativePathToFull(Dir);
	S->Save();
	SetFeedback(EFeedback::None, FString());
	StartConversion(false, S->UT2004Dir);
	return FArenaConverter::Get().IsRunning();
}

bool AArenaContentSetupHUD::IsSetupNeeded()
{
	return ArenaContent::UseRuntimeContent() && !ArenaContent::IsCacheReady();
}

AArenaContentSetupHUD::FInstallCheck AArenaContentSetupHUD::CheckInstallFolder(const FString& InChosen)
{
	FInstallCheck C;
	C.Chosen = CleanPath(InChosen);
	const FString& Dir = C.Chosen;
	auto Ok = [&C](const FString& Install, const FString& Message)
	{
		C.bOk = true;
		C.Install = Install;
		C.bCorrected = Install != C.Chosen;
		C.Message = Message;
		return C;
	};
	if (Dir.IsEmpty())
	{
		C.Message = TEXT("No folder was given. Type or choose the folder where UT2004 is installed.");
		return C;
	}
	if (!IFileManager::Get().DirectoryExists(*Dir))
	{
		C.Message = IFileManager::Get().FileExists(*Dir)
			? TEXT("That is a file, not a folder. Choose the folder where UT2004 is installed.")
			: TEXT("That folder does not exist. Check the spelling, or use Browse to pick it.");
		return C;
	}
	if (ArenaContent::IsUT2004Install(Dir))
	{
		return Ok(Dir, TEXT("UT2004 found."));
	}
	if (FPaths::IsSamePath(Dir, ArenaContent::GetCacheDir()) || FPaths::IsUnderDirectory(Dir, ArenaContent::GetCacheDir()))
	{
		C.Message = TEXT("That is the game's own converted content folder. Choose the folder where UT2004 itself is installed.");
		return C;
	}
	// a folder inside the install (System, Maps, Textures...): use the install around it
	FString Up = Dir;
	for (int32 Level = 1; Level <= 2; ++Level)
	{
		const FString Parent = FPaths::GetPath(Up);
		if (Parent.IsEmpty() || Parent == Up)
		{
			break;
		}
		Up = Parent;
		if (ArenaContent::IsUT2004Install(Up))
		{
			return Ok(Up, FString::Printf(TEXT("You picked the \"%s\" folder inside UT2004, so the UT2004 folder around it is used."),
				*FPaths::GetCleanFilename(Level == 1 ? Dir : FPaths::GetPath(Dir))));
		}
	}
	// the folder above the install (e.g. "Games" or Steam's "common"): look one, then two levels down
	const TArray<FString> Children = SubDirs(Dir);
	for (const FString& Child : Children)
	{
		if (ArenaContent::IsUT2004Install(Dir / Child))
		{
			return Ok(Dir / Child, FString::Printf(TEXT("UT2004 found in the \"%s\" folder inside the one you picked."), *Child));
		}
	}
	int32 Budget = 200;
	for (const FString& Child : Children)
	{
		for (const FString& Grand : SubDirs(Dir / Child))
		{
			if (--Budget < 0)
			{
				break;
			}
			if (ArenaContent::IsUT2004Install(Dir / Child / Grand))
			{
				return Ok(Dir / Child / Grand, FString::Printf(TEXT("UT2004 found in \"%s\" inside the folder you picked."), *(Child / Grand)));
			}
		}
	}
	// explain what is wrong
	const bool bSystem = HasSubDir(Children, TEXT("System"));
	const bool bMaps = HasSubDir(Children, TEXT("Maps"));
	if (bSystem && !bMaps)
	{
		C.Message = TEXT("This folder has a System folder but no Maps folder, so the UT2004 install looks incomplete. Choose a complete UT2004 install.");
	}
	else if (bMaps && !bSystem)
	{
		C.Message = TEXT("This folder has a Maps folder but no System folder, so the UT2004 install looks incomplete. Choose a complete UT2004 install.");
	}
	else
	{
		C.Message = TEXT("This is not a UT2004 folder. Choose the folder that contains the System and Maps folders, usually called \"Unreal Tournament 2004\" or \"UT2004\".");
	}
	return C;
}

namespace ArenaContentSetupLocal
{
	/** The GUI of the setup screen while it draws (extension blocks draw through DrawParagraph). */
	ArenaGUI2K4::FGUI* GActiveGUI = nullptr;

	/** GUIFont for a 1080p text size of the old layout */
	const TCHAR* FontForSize(float SizeRef)
	{
		return SizeRef >= 26.f ? TEXT("UT2HeaderFont") : (SizeRef >= 18.5f ? TEXT("UT2LargeFont") : (SizeRef >= 15.5f ? TEXT("UT2MenuFont") : TEXT("UT2SmallFont")));
	}
}

void AArenaContentSetupHUD::DrawProgressBar(const FArenaDraw& D, float X, float Y, float Width, float Height, float Fraction)
{
	// GUIProgressBar look: NewControls.NewStatusBar with NewStatusFill up to the progress
	ArenaGUI2K4::FGUI* G = GActiveGUI;
	if (!G || !G->Image(TEXT("2K4Menus.NewControls.NewStatusBar"), ArenaGUI2K4::FRect(X, Y, Width, Height), ArenaGUI2K4::EImgStyle::Stretched, FLinearColor::White))
	{
		D.Bar(X, Y, Width, Height, Fraction, ArenaSetupStyle::Accent());
		return;
	}
	if (Fraction > 0.f)
	{
		G->Image(TEXT("2K4Menus.NewControls.NewStatusFill"), ArenaGUI2K4::FRect(X, Y, Width * FMath::Clamp(Fraction, 0.f, 1.f), Height),
			ArenaGUI2K4::EImgStyle::Stretched, FLinearColor::White);
	}
}

float AArenaContentSetupHUD::DrawParagraph(const FArenaDraw& D, const FString& Text, float X, float Y, float Width, const FLinearColor& Color,
	float SizeRef, float GapRef)
{
	if (ArenaGUI2K4::FGUI* G = GActiveGUI)
	{
		// the menus' bitmap fonts (whole-number scaled) when the setup screen draws through its GUI
		const ArenaGUI2K4::FFontRef F = G->Font(FontForSize(SizeRef));
		for (const FString& L : G->Wrap(F, Text, Width))
		{
			G->Text(F, L, X, Y, Color, ArenaGUI2K4::EAlign::Left, true);
			Y += F.Height;
		}
		return Y + D.S(GapRef);
	}
	const float Size = D.S(SizeRef);
	for (const FString& L : Wrap(D, Text, Size, Width))
	{
		D.Text(L, X, Y, Color, Size);
		Y += Size * 1.2f;
	}
	return Y + D.S(GapRef);
}

// =============================================================================================
// Lifetime, states, actions
// =============================================================================================

void AArenaContentSetupHUD::Open(APlayerController* PC)
{
	if (!PC && GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.World() && (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE))
			{
				PC = Ctx.World()->GetFirstPlayerController();
				break;
			}
		}
	}
	if (PC)
	{
		PC->ClientSetHUD(AArenaContentSetupHUD::StaticClass());
	}
}

void AArenaContentSetupHUD::BeginPlay()
{
	Super::BeginPlay();
	InstallDir = ArenaContent::FindUT2004Install();
	Searched = ArenaContent::GetSearchLocations();
	bWasRunning = FArenaConverter::Get().IsRunning();
#if !UE_BUILD_SHIPPING
	// tests: -SetupChoose=<folder> (as if picked with Browse), -SetupConvert (then convert it), -SetupDetails,
	// -SetupTypePath=<text> (path field open with that text), -SetupBrowse [-SetupBrowseStart=<folder>] (opens the real folder dialog)
	{
		const TCHAR* Cmd = FCommandLine::Get();
		FString Value;
		if (FParse::Value(Cmd, TEXT("SetupChoose="), Value, false))
		{
			UseChosenFolder(Value, TEXT("test"));
			if (FParse::Param(Cmd, TEXT("SetupConvert")) && GetView() != EView::Converting)
			{
				StartConversion(false);
			}
		}
		bShowDetails = FParse::Param(Cmd, TEXT("SetupDetails"));
		if (FParse::Value(Cmd, TEXT("SetupTypePath="), Value, false))
		{
			BeginEditPath();
			PathText = Value;
			PathCursor = PathText.Len();
		}
		if (FParse::Param(Cmd, TEXT("SetupBrowse")))
		{
			Browse();
		}
	}
#endif
	if (AutoConvert() && !bWasRunning && IsSetupNeeded() && !InstallDir.IsEmpty() && !FArenaConverter::Get().IsRunning())
	{
		bAutoStarted = true;
		StartConversion(false);
	}
}

void AArenaContentSetupHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EndEditPath(false);
	Super::EndPlay(EndPlayReason);
}

const FArenaContentSetupExtension* AArenaContentSetupHUD::GetModalExtension() const
{
	for (const FArenaContentSetupExtension& E : Extensions())
	{
		if (E.IsModal && E.IsModal())
		{
			return &E;
		}
	}
	return nullptr;
}

AArenaContentSetupHUD::EView AArenaContentSetupHUD::GetView() const
{
	if (FArenaConverter::Get().IsRunning())
	{
		return EView::Converting;
	}
	if (GetModalExtension())
	{
		return EView::Modal;
	}
	const FArenaConvertProgress P = FArenaConverter::Get().GetProgress();
	if (bShowDone)
	{
		return P.bSucceeded ? EView::Done : (P.bCancelled ? EView::Paused : EView::Failed);
	}
	if (ArenaContent::IsCacheReady())
	{
		return EView::Ready;
	}
	return InstallDir.IsEmpty() ? EView::NoInstall : EView::InstallFound;
}

void AArenaContentSetupHUD::StartConversion(bool bForce, const FString& Dir)
{
	InstallDir = !Dir.IsEmpty() ? Dir : (!InstallDir.IsEmpty() && ArenaContent::IsUT2004Install(InstallDir) ? InstallDir : ArenaContent::FindUT2004Install());
	if (InstallDir.IsEmpty())
	{
		return;
	}
	UArenaUserSettings* S = UArenaUserSettings::Get();
	if (S->UT2004Dir != InstallDir)
	{
		S->UT2004Dir = InstallDir;
		S->Save();
	}
	FArenaConverter::FOptions Opt;
	Opt.InstallDir = InstallDir;
	Opt.bForce = bForce;
	FString Maps;
	if (FParse::Value(FCommandLine::Get(), TEXT("ConvertMaps="), Maps, false))
	{
		Maps.ParseIntoArray(Opt.Maps, TEXT(","), true);
	}
	bShowDone = false;
	Selection = 0;
	SetFeedback(EFeedback::None, FString());
	if (FArenaConverter::Get().Start(Opt))
	{
		bWasRunning = true;
		UE_LOG(LogOpenUT2k4, Log, TEXT("Content setup: converting %s into %s"), *InstallDir, *ArenaContent::GetCacheDir());
	}
}

void AArenaContentSetupHUD::Close()
{
	EndEditPath(false);
	if (APlayerController* PC = GetOwningPlayerController())
	{
		PC->ClientSetHUD(AArenaMenuHUD::StaticClass());
	}
}

void AArenaContentSetupHUD::SetFeedback(EFeedback Kind, const FString& Text, const FString& Detail)
{
	FeedbackKind = Kind;
	FeedbackText = Text;
	FeedbackDetail = Detail;
}

void AArenaContentSetupHUD::Browse()
{
	if (bNoDialog)
	{
		BeginEditPath();
		return;
	}
	TWeakObjectPtr<AArenaContentSetupHUD> Weak(this);
	FString Start = !InstallDir.IsEmpty() ? InstallDir : FString(FPlatformProcess::UserHomeDir());
#if !UE_BUILD_SHIPPING
	FParse::Value(FCommandLine::Get(), TEXT("SetupBrowseStart="), Start, false); // tests: folder the dialog opens in
#endif
	const bool bOpened = ArenaFolderPicker::Open(TEXT("Choose your Unreal Tournament 2004 folder"), Start, [Weak](const ArenaFolderPicker::FResult& R)
	{
		AArenaContentSetupHUD* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		if (R.bChosen)
		{
			Self->UseChosenFolder(R.Path, TEXT("folder dialog"));
		}
		else if (R.bCancelled)
		{
			Self->SetFeedback(EFeedback::None, FString());
		}
		else
		{
			Self->bNoDialog = true;
			Self->SetFeedback(EFeedback::Warn, TEXT("This system cannot show a folder window, so please type the path of your UT2004 folder instead."), R.Error);
			Self->BeginEditPath();
		}
	});
	if (bOpened)
	{
		SetFeedback(EFeedback::None, FString());
	}
}

void AArenaContentSetupHUD::UseChosenFolder(const FString& Chosen, const TCHAR* Source)
{
	const FInstallCheck C = CheckInstallFolder(Chosen);
	UE_LOG(LogOpenUT2k4, Log, TEXT("Content setup: %s chose \"%s\" -> %s%s (%s)"), Source, *Chosen, C.bOk ? TEXT("install ") : TEXT("not an install"),
		*C.Install, *C.Message);
	if (!C.bOk)
	{
		SetFeedback(EFeedback::Bad, C.Message, C.Chosen);
		return;
	}
	InstallDir = C.Install;
	UArenaUserSettings* S = UArenaUserSettings::Get();
	S->UT2004Dir = InstallDir;
	S->Save();
	bShowDone = false;
	const bool bReady = ArenaContent::IsCacheReady();
	SetFeedback(EFeedback::Good, C.Message + (bReady ? TEXT(" Choose Convert again to rebuild your content from it.") : TEXT(" Choose Convert to continue.")),
		C.bCorrected ? FString::Printf(TEXT("You picked: %s"), *C.Chosen) : FString());
	SelectButton(bReady ? FName(TEXT("Rebuild")) : FName(TEXT("Convert")));
}

void AArenaContentSetupHUD::FocusTextSink(bool bFocus)
{
	if (!FSlateApplication::IsInitialized() || !GEngine || !GEngine->GameViewport)
	{
		return;
	}
	if (bFocus)
	{
		if (!TextSink.IsValid())
		{
			TextSink = SNew(SArenaSetupTextSink);
			TextSink->Owner = this;
			TextSinkHolder = SNew(SBox).HAlign(HAlign_Left).VAlign(VAlign_Top).WidthOverride(1.f).HeightOverride(1.f)[TextSink.ToSharedRef()];
			GEngine->GameViewport->AddViewportWidgetContent(TextSinkHolder.ToSharedRef(), 1000);
		}
		FSlateApplication::Get().SetAllUserFocus(TextSink, EFocusCause::SetDirectly);
	}
	else if (TextSink.IsValid())
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(TextSinkHolder.ToSharedRef());
		TextSink.Reset();
		TextSinkHolder.Reset();
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
}

void AArenaContentSetupHUD::BeginEditPath()
{
	if (bEditingPath)
	{
		return;
	}
	bEditingPath = true;
	PathText = !InstallDir.IsEmpty() ? InstallDir : (FeedbackKind == EFeedback::Bad && !FeedbackDetail.IsEmpty() ? FeedbackDetail : FString());
	PathCursor = PathText.Len();
	PathEditStart = FPlatformTime::Seconds();
	Selection = 0;
	FocusTextSink(true);
}

void AArenaContentSetupHUD::EndEditPath(bool bAccept)
{
	if (!bEditingPath)
	{
		return;
	}
	bEditingPath = false;
	Selection = 0;
	FocusTextSink(false);
	if (bAccept)
	{
		UseChosenFolder(PathText, TEXT("path field"));
		if (FeedbackKind == EFeedback::Bad)
		{
			// keep the text for correcting it
			bEditingPath = true;
			FocusTextSink(true);
		}
	}
}

void AArenaContentSetupHUD::CompletePath()
{
	// Tab: complete the last path segment from the folders that exist
	const FString Clean = PathText.Contains(TEXT("/")) || PathText.Contains(TEXT("\\")) || PathText.StartsWith(TEXT("~")) ? PathText : FString();
	if (Clean.IsEmpty())
	{
		return;
	}
	FString Typed = PathText.Replace(TEXT("\\"), TEXT("/"));
	if (Typed.StartsWith(TEXT("~")))
	{
		Typed = FString(FPlatformProcess::UserHomeDir()) / Typed.Mid(FMath::Min(2, Typed.Len()));
	}
	int32 Slash = INDEX_NONE;
	Typed.FindLastChar(TEXT('/'), Slash);
	const FString Base = Typed.Left(Slash + 1);
	const FString Prefix = Typed.Mid(Slash + 1);
	TArray<FString> Matches;
	for (const FString& Name : SubDirs(Base.IsEmpty() ? FString(TEXT("/")) : Base))
	{
		if (Name.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			Matches.Add(Name);
		}
	}
	if (Matches.Num() == 0)
	{
		return;
	}
	FString Common = Matches[0];
	for (const FString& M : Matches)
	{
		int32 N = 0;
		while (N < Common.Len() && N < M.Len() && FChar::ToLower(Common[N]) == FChar::ToLower(M[N])) ++N;
		Common.LeftInline(N);
	}
	PathText = Base + (Common.Len() >= Prefix.Len() ? Common : Prefix) + (Matches.Num() == 1 ? TEXT("/") : TEXT(""));
	PathCursor = PathText.Len();
}

void AArenaContentSetupHUD::OnPathKey(const FKeyEvent& Event)
{
	const FKey Key = Event.GetKey();
	const bool bCtrl = Event.GetModifierKeys().IsControlDown() || Event.GetModifierKeys().IsCommandDown();
	PathEditStart = FPlatformTime::Seconds(); // caret visible while typing
	if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom)
	{
		EndEditPath(true);
	}
	else if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right)
	{
		EndEditPath(false);
	}
	else if (Key == EKeys::BackSpace && PathCursor > 0)
	{
		PathText.RemoveAt(PathCursor - 1);
		--PathCursor;
	}
	else if (Key == EKeys::Delete && PathCursor < PathText.Len())
	{
		PathText.RemoveAt(PathCursor);
	}
	else if (Key == EKeys::Left)
	{
		PathCursor = FMath::Max(0, PathCursor - 1);
	}
	else if (Key == EKeys::Right)
	{
		PathCursor = FMath::Min(PathText.Len(), PathCursor + 1);
	}
	else if (Key == EKeys::Home)
	{
		PathCursor = 0;
	}
	else if (Key == EKeys::End)
	{
		PathCursor = PathText.Len();
	}
	else if (Key == EKeys::Tab)
	{
		CompletePath();
	}
	else if (bCtrl && Key == EKeys::V)
	{
		FString Clip;
		FPlatformApplicationMisc::ClipboardPaste(Clip);
		Clip.ReplaceInline(TEXT("\r"), TEXT(""));
		Clip.ReplaceInline(TEXT("\n"), TEXT(""));
		PathText.InsertAt(PathCursor, Clip);
		PathCursor += Clip.Len();
	}
	else if (bCtrl && (Key == EKeys::U || Key == EKeys::BackSpace))
	{
		PathText.Reset();
		PathCursor = 0;
	}
	else if (Key == EKeys::Up || Key == EKeys::Down)
	{
		// lets keyboard players reach the buttons under the field
		Selection = Selection == 0 ? 1 : 0;
	}
}

void AArenaContentSetupHUD::OnPathChar(TCHAR Char)
{
	if (Char < 32 || Char == 127 || PathText.Len() >= 1024)
	{
		return;
	}
	PathText.InsertAt(PathCursor, Char);
	++PathCursor;
}

void AArenaContentSetupHUD::BuildButtons(TArray<FButton>& Out) const
{
	Out.Reset();
	auto Add = [&Out](const TCHAR* Id, const FString& Label, bool bEnabled = true) { Out.Add({ FName(Id), Label, bEnabled }); };
	const FString DetailsLabel = bShowDetails ? TEXT("Hide details") : TEXT("Show details");
	if (bEditingPath)
	{
		Add(TEXT("PathOk"), TEXT("Use this folder"), !PathText.TrimStartAndEnd().IsEmpty());
		Add(TEXT("PathCancel"), TEXT("Cancel"));
		return;
	}
	const bool bBusy = AnyExtensionBusy();
	const bool bPicker = ArenaFolderPicker::IsOpen();
	const FString BrowseLabel = bNoDialog ? TEXT("Type the folder path...") : TEXT("Browse...");
	switch (GetView())
	{
	case EView::Converting:
		Add(TEXT("Cancel"), TEXT("Cancel"));
		Add(TEXT("Details"), DetailsLabel);
		return;
	case EView::Modal:
	{
		const FArenaContentSetupExtension* Modal = GetModalExtension();
		AddExtensionButtons(Out, Modal->Group.IsNone() ? Modal->Id : Modal->Group);
		Add(TEXT("Back"), TEXT("Back"));
		return;
	}
	case EView::Done:
		Add(TEXT("Continue"), TEXT("Continue"));
		if (FArenaConverter::Get().GetProgress().ErrorCount > 0)
		{
			Add(TEXT("Details"), DetailsLabel);
		}
		return;
	case EView::Ready:
		Add(TEXT("Continue"), TEXT("Continue"));
		Add(TEXT("Browse"), bNoDialog ? BrowseLabel : FString(TEXT("Use another folder...")), !bPicker && !bBusy);
		if (!bNoDialog)
		{
			Add(TEXT("Type"), TEXT("Type a path..."), !bBusy);
		}
		Add(TEXT("Rebuild"), TEXT("Convert again"), !InstallDir.IsEmpty() && !bBusy);
		AddExtensionButtons(Out);
		Add(TEXT("Details"), DetailsLabel);
		return;
	case EView::NoInstall:
		Add(TEXT("Browse"), bNoDialog ? BrowseLabel : FString(TEXT("Browse for UT2004...")), !bPicker && !bBusy);
		if (!bNoDialog)
		{
			Add(TEXT("Type"), TEXT("Type a path..."), !bBusy);
		}
		AddExtensionButtons(Out);
		Add(TEXT("Rescan"), TEXT("Search again"), !bBusy);
		Add(TEXT("Details"), DetailsLabel);
		Add(TEXT("Back"), TEXT("Back"));
		return;
	case EView::InstallFound:
	case EView::Failed:
	case EView::Paused:
	default:
	{
		const bool bResume = HasPartialCache();
		const FString ConvertLabel = GetView() == EView::Failed ? TEXT("Try again") : (bResume ? TEXT("Resume converting") : TEXT("Convert"));
		Add(TEXT("Convert"), ConvertLabel, !InstallDir.IsEmpty() && !bBusy);
		Add(TEXT("Browse"), bNoDialog ? BrowseLabel : FString(TEXT("Use another folder...")), !bPicker && !bBusy);
		if (!bNoDialog)
		{
			Add(TEXT("Type"), TEXT("Type a path..."), !bBusy);
		}
		AddExtensionButtons(Out);
		Add(TEXT("Details"), DetailsLabel);
		Add(TEXT("Back"), TEXT("Back"));
		return;
	}
	}
}

void AArenaContentSetupHUD::AddExtensionButtons(TArray<FButton>& Out, FName OnlyGroup) const
{
	// ---- extension buttons (FArenaContentSetupExtension, e.g. "Download from OldUnreal") ----
	for (const FArenaContentSetupExtension& E : Extensions())
	{
		if (!OnlyGroup.IsNone() && (E.Group.IsNone() ? E.Id : E.Group) != OnlyGroup)
		{
			continue;
		}
		const FString Label = E.GetLabel ? E.GetLabel() : FString();
		if (!Label.IsEmpty())
		{
			Out.Add({ FName(*(TEXT("Ext_") + E.Id.ToString())), Label, !E.IsEnabled || E.IsEnabled() });
		}
	}
}

void AArenaContentSetupHUD::SelectButton(FName Id)
{
	TArray<FButton> Buttons;
	BuildButtons(Buttons);
	const int32 Index = Buttons.IndexOfByPredicate([Id](const FButton& B) { return B.Id == Id; });
	if (Index != INDEX_NONE)
	{
		Selection = Index;
	}
}

void AArenaContentSetupHUD::Activate(FName Id)
{
	const FString IdStr = Id.ToString();
	if (IdStr.StartsWith(TEXT("Ext_")))
	{
		for (const FArenaContentSetupExtension& E : Extensions())
		{
			if (E.Id.ToString() == IdStr.Mid(4) && E.OnActivate)
			{
				SetFeedback(EFeedback::None, FString());
				E.OnActivate(this);
				break;
			}
		}
		return;
	}
	if (Id == TEXT("Cancel"))
	{
		FArenaConverter::Get().Cancel();
	}
	else if (Id == TEXT("Convert"))
	{
		StartConversion(false);
	}
	else if (Id == TEXT("Rebuild"))
	{
		StartConversion(true);
	}
	else if (Id == TEXT("Browse"))
	{
		Browse();
	}
	else if (Id == TEXT("Type"))
	{
		BeginEditPath();
	}
	else if (Id == TEXT("PathOk"))
	{
		EndEditPath(true);
	}
	else if (Id == TEXT("PathCancel"))
	{
		EndEditPath(false);
	}
	else if (Id == TEXT("Details"))
	{
		bShowDetails = !bShowDetails;
	}
	else if (Id == TEXT("Rescan"))
	{
		InstallDir = ArenaContent::FindUT2004Install();
		Searched = ArenaContent::GetSearchLocations();
		SetFeedback(InstallDir.IsEmpty() ? EFeedback::Warn : EFeedback::Good,
			InstallDir.IsEmpty() ? TEXT("Still no UT2004 install in the usual places. Use Browse to show where it is.") : TEXT("UT2004 found."));
	}
	else if (Id == TEXT("Continue") || Id == TEXT("Back"))
	{
		Close();
	}
}

void AArenaContentSetupHUD::NotifyHitBoxClick(FName BoxName)
{
	// input goes through the GUI2K4 widgets (ArenaGUI2K4.h)
	Super::NotifyHitBoxClick(BoxName);
}

void AArenaContentSetupHUD::NotifyHitBoxBeginCursorOver(FName BoxName)
{
	Super::NotifyHitBoxBeginCursorOver(BoxName);
}

void AArenaContentSetupHUD::HandleKeys(int32 NumButtons)
{
	// Up / Down / Enter move and activate through the GUI focus; Esc cancels a conversion or goes back
	if (!GUI || bEditingPath || !GUI->BackPressed())
	{
		return;
	}
	if (FArenaConverter::Get().IsRunning())
	{
		FArenaConverter::Get().Cancel();
	}
	else
	{
		Close();
	}
}

// =============================================================================================
// Drawing
// =============================================================================================

void AArenaContentSetupHUD::DrawPathField(const FArenaDraw& D, float X, float& Y, float Width)
{
	using namespace ArenaGUI2K4;
	Y = DrawParagraph(D, TEXT("Type the full path of your UT2004 folder (the one with System and Maps inside):"), X, Y, Width, White, 18.f, 6.f);
	// a GUIEditBox (STY2EditBox) holding the path
	const FFontRef F = GUI ? GUI->Font(TEXT("UT2MenuFont")) : FFontRef();
	const float H = GUI ? FMath::RoundToFloat(F.Height * 1.5f) : D.S(44.f);
	const float Pad = D.S(12.f);
	const FRect Box(X, Y, Width, H);
	const uint32 FieldId = FGUI::Id(TEXT("SetupPath"));
	const EMenuState St = GUI ? GUI->CustomWidget(FieldId, Box, true, TEXT("Click to type the folder path"), false) : EMenuState::Blurry;
	if (!GUI || !GUI->StyleImage(GetStyle(TEXT("EditBox")), bEditingPath ? EMenuState::Pressed : St, Box))
	{
		D.Rect(X, Y, Width, H, FLinearColor(0.f, 0.01f, 0.03f, 0.85f));
		D.Outline(X, Y, Width, H, bEditingPath ? Accent : Alpha(Accent, 0.4f), FMath::Max(1.f, D.S(1.5f)));
	}
	if (GUI && !bEditingPath && GUI->WasClicked(FieldId, Box))
	{
		BeginEditPath();
	}
	// show the end of long paths (where the caret usually is)
	auto TW = [&](const FString& T) { return GUI ? GUI->TextSize(F, T).X : D.TextSize(T, D.S(20.f)).X; };
	const float Avail = Width - Pad * 2.f;
	FString Before = PathText.Left(PathCursor);
	const FString After = PathText.Mid(PathCursor);
	int32 Skip = 0;
	while (Skip < Before.Len() && TW(Before.Mid(Skip)) > Avail * 0.9f)
	{
		Skip += FMath::Max(1, (Before.Len() - Skip) / 16);
	}
	Before = Before.Mid(Skip);
	FString Shown = Before + After;
	while (Shown.Len() > Before.Len() && TW(Shown) > Avail)
	{
		Shown.LeftChopInline(1);
	}
	const FString Display = PathText.IsEmpty() ? TEXT("e.g. ") + FString(FPlatformProcess::UserHomeDir()) / TEXT("Games/UT2004") : Shown;
	const FLinearColor TextColor = PathText.IsEmpty() ? Alpha(Dim, 0.6f) : White;
	if (GUI)
	{
		GUI->Text(F, Display, X + Pad, Y + (H - F.Height) * 0.5f, TextColor);
	}
	else
	{
		D.TextMid(Display, X + Pad, Y + H * 0.5f, TextColor, D.S(20.f));
	}
	if (bEditingPath && FMath::Fmod(FPlatformTime::Seconds() - PathEditStart, 1.0) < 0.6)
	{
		const float CX = X + Pad + (Before.IsEmpty() ? 0.f : TW(Before));
		D.Rect(CX, Y + H * 0.2f, FMath::Max(1.f, D.S(2.f)), H * 0.6f, White);
	}
	Y += H + D.S(8.f);
	Y = DrawParagraph(D, TEXT("Enter to confirm, Esc to cancel, Tab completes folder names, Ctrl+V pastes."), X, Y, Width, Alpha(Dim, 0.85f), 15.f, 14.f);
}

void AArenaContentSetupHUD::DrawBody(const FArenaDraw& D, EView View, float X, float Y, float Width, float Bottom)
{
	auto Para = [&](const FString& Text, const FLinearColor& Color, float Size = 19.f, float Gap = 10.f)
	{
		if (Y < Bottom)
		{
			Y = DrawParagraph(D, Text, X, Y, Width, Color, Size, Gap);
		}
	};
	auto Heading = [&](const FString& Text, const FLinearColor& Color)
	{
		Y = DrawParagraph(D, Text, X, Y, Width, Color, 30.f, 12.f);
	};
	/** The install path in a framed box. */
	auto PathBox = [&](const FString& Label, const FString& Path)
	{
		Y = DrawParagraph(D, Label, X, Y, Width, Dim, 15.f, 2.f);
		if (GUI)
		{
			// a read-only GUIEditBox (STY2EditBox) with the folder
			const ArenaGUI2K4::FFontRef F = GUI->Font(TEXT("UT2MenuFont"));
			const ArenaGUI2K4::FRect Box(X, Y, Width, FMath::RoundToFloat(F.Height * 1.5f));
			if (!GUI->StyleImage(ArenaGUI2K4::GetStyle(TEXT("EditBox")), ArenaGUI2K4::EMenuState::Blurry, Box))
			{
				D.Rect(Box.X, Box.Y, Box.W, Box.H, FLinearColor(0.f, 0.01f, 0.03f, 0.6f));
			}
			GUI->TextIn(F, Path, Box.Inset(D.S(10.f), 0.f, D.S(10.f), 0.f), White, ArenaGUI2K4::EAlign::Left);
			Y += Box.H + D.S(14.f);
			return;
		}
		const float H = D.S(38.f);
		D.Rect(X, Y, Width, H, FLinearColor(0.f, 0.01f, 0.03f, 0.6f));
		D.Rect(X, Y, D.S(3.f), H, Accent);
		D.TextMid(FitPath(D, Path, D.S(18.f), Width - D.S(24.f)), X + D.S(14.f), Y + H * 0.5f, White, D.S(18.f));
		Y += H + D.S(14.f);
	};
	auto DetailLine = [&](const FString& Text, const FLinearColor& Color = Dim)
	{
		if (Y < Bottom)
		{
			Y = DrawParagraph(D, Text, X + D.S(14.f), Y, Width - D.S(14.f), Color, 15.f, 2.f);
		}
	};
	auto DetailsHeader = [&](const FString& Text)
	{
		Y += D.S(6.f);
		D.Rect(X, Y, Width, FMath::Max(1.f, D.S(1.f)), Alpha(Accent, 0.35f));
		Y += D.S(10.f);
		Y = DrawParagraph(D, Text, X, Y, Width, Accent, 16.f, 4.f);
	};
	const FArenaConvertProgress P = FArenaConverter::Get().GetProgress();
	const FString ErrLog = ArenaContent::GetCacheDir() / TEXT("convert_errors.log");

	// result of Browse / the path field
	if (FeedbackKind != EFeedback::None && View != EView::Converting && View != EView::Modal)
	{
		const FLinearColor C = FeedbackKind == EFeedback::Good ? Good : (FeedbackKind == EFeedback::Warn ? Hot : Bad);
		const float Top = Y;
		const float Inner = Width - D.S(28.f);
		float TY = DrawParagraph(D, FeedbackText, X + D.S(18.f), Y + D.S(10.f), Inner, White, 18.f, 2.f);
		if (!FeedbackDetail.IsEmpty())
		{
			TY = DrawParagraph(D, FeedbackDetail, X + D.S(18.f), TY, Inner, Dim, 15.f, 2.f);
		}
		const float BoxH = TY - Top + D.S(8.f);
		D.Rect(X, Top, Width, BoxH, Alpha(C, 0.12f));
		D.Rect(X, Top, D.S(4.f), BoxH, C);
		Y = Top + BoxH + D.S(16.f);
	}
	if (ArenaFolderPicker::IsOpen())
	{
		Para(TEXT("A folder window is open: choose your UT2004 folder there."), Accent, 19.f, 14.f);
	}
	if (bEditingPath)
	{
		DrawPathField(D, X, Y, Width);
	}

	switch (View)
	{
	case EView::Converting:
	{
		Heading(TEXT("Converting your UT2004 content"), White);
		Para(TEXT("This happens once. Your UT2004 folder is only read, never changed."), Dim, 17.f, 18.f);
		Para(P.Stage.IsEmpty() ? FString(TEXT("Starting...")) : P.Stage, Accent, 20.f, 8.f);
		const float BarH = D.S(22.f);
		DrawProgressBar(D, X, Y, Width, BarH, P.Fraction);
		Y += BarH + D.S(10.f);
		FString Stats = FString::Printf(TEXT("%d%%   -   %s elapsed"), FMath::RoundToInt(P.Fraction * 100.f), *FormatTime(P.ElapsedSeconds));
		if (P.Fraction > 0.05f && P.ElapsedSeconds > 10.0)
		{
			Stats += FString::Printf(TEXT("   -   about %s left"), *FormatTime(P.ElapsedSeconds * (1.0 - P.Fraction) / P.Fraction));
		}
		Para(Stats, White, 18.f, 14.f);
		if (P.ErrorCount > 0)
		{
			Para(FString::Printf(TEXT("%s could not be converted so far. The game works without them."), *Plural(P.ErrorCount, TEXT("item"), TEXT("items"))), Hot, 17.f, 8.f);
		}
		Para(TEXT("You can cancel at any time: converting again continues where it stopped."), Dim, 16.f, 8.f);
		if (bShowDetails)
		{
			DetailsHeader(TEXT("Details"));
			DetailLine(FString::Printf(TEXT("Item: %s"), *P.Item));
			DetailLine(FString::Printf(TEXT("%d of %d packages and maps"), P.UnitsDone, P.UnitsTotal));
			DetailLine(FString::Printf(TEXT("From: %s"), *InstallDir));
			DetailLine(FString::Printf(TEXT("To: %s"), *ArenaContent::GetCacheDir()));
			for (const FString& E : P.RecentErrors)
			{
				DetailLine(E.Replace(TEXT("\t"), TEXT(":  ")), Alpha(Bad, 0.9f));
			}
		}
		return;
	}
	case EView::Modal:
		if (const FArenaContentSetupExtension* Modal = GetModalExtension())
		{
			if (Modal->DrawInfo)
			{
				Modal->DrawInfo(D, X, Y, Width);
			}
		}
		return;
	case EView::Done:
		Heading(TEXT("All set! Your UT2004 content is ready."), Good);
		if (const FString Summary = CacheSummary(); !Summary.IsEmpty())
		{
			Para(Summary + TEXT("."), White, 19.f, 8.f);
		}
		Para(FString::Printf(TEXT("Converted in %s."), *FormatTime(P.ElapsedSeconds)), Dim, 17.f, 14.f);
		if (P.ErrorCount > 0)
		{
			Para(FString::Printf(TEXT("%s could not be converted - details in %s. The game works without them."), *Plural(P.ErrorCount, TEXT("item"), TEXT("items")),
				*FPaths::GetCleanFilename(ErrLog)), Hot, 17.f, 14.f);
		}
		Para(TEXT("Choose Continue to pick a map and play."), White, 19.f, 10.f);
		if (bShowDetails)
		{
			DetailsHeader(TEXT("Details"));
			DetailLine(FString::Printf(TEXT("Problem list: %s"), *ErrLog));
			DetailLine(FString::Printf(TEXT("Content folder: %s"), *ArenaContent::GetCacheDir()));
			for (const FString& E : P.RecentErrors)
			{
				DetailLine(E.Replace(TEXT("\t"), TEXT(":  ")), Alpha(Bad, 0.9f));
			}
		}
		return;
	case EView::Failed:
		Heading(TEXT("The conversion did not finish"), Bad);
		Para(P.FatalError.IsEmpty() ? FString(TEXT("Something went wrong while converting.")) : P.FatalError, White, 19.f, 10.f);
		Para(TEXT("Check that the UT2004 folder can be read and that there is enough free disk space (about 6 GB), then try again."), Dim, 17.f, 14.f);
		if (!InstallDir.IsEmpty())
		{
			PathBox(TEXT("UT2004 folder"), InstallDir);
		}
		if (bShowDetails)
		{
			DetailsHeader(TEXT("Details"));
			DetailLine(FString::Printf(TEXT("Content folder: %s"), *ArenaContent::GetCacheDir()));
			DetailLine(FString::Printf(TEXT("Problem list: %s"), *ErrLog));
			for (const FString& E : P.RecentErrors)
			{
				DetailLine(E.Replace(TEXT("\t"), TEXT(":  ")), Alpha(Bad, 0.9f));
			}
		}
		return;
	case EView::Paused:
		Heading(TEXT("Conversion paused"), Hot);
		Para(TEXT("Nothing is lost: Resume converting continues where it stopped."), White, 19.f, 14.f);
		PathBox(TEXT("UT2004 folder"), InstallDir);
		break;
	case EView::Ready:
		Heading(TEXT("Your UT2004 content is ready"), Good);
		if (const FString Summary = CacheSummary(); !Summary.IsEmpty())
		{
			Para(Summary + TEXT("."), White, 19.f, 14.f);
		}
		if (!InstallDir.IsEmpty())
		{
			PathBox(TEXT("UT2004 folder"), InstallDir);
		}
		Para(TEXT("Convert again if you updated your UT2004 install or want to use another copy. Continue returns to the menu."), Dim, 17.f, 12.f);
		break;
	case EView::InstallFound:
		Heading(TEXT("UT2004 found"), Good);
		PathBox(TEXT("UT2004 folder"), InstallDir);
		if (HasPartialCache())
		{
			Para(TEXT("A previous conversion was interrupted. Resume converting continues where it stopped."), Hot, 19.f, 10.f);
		}
		else
		{
			Para(TEXT("One more step: the game converts the maps, characters, textures and sounds of your copy once. This takes a few minutes and needs about 6 GB of free disk space."),
				White, 19.f, 10.f);
		}
		Para(TEXT("Not the right copy? Choose Use another folder."), Dim, 17.f, 14.f);
		break;
	case EView::NoInstall:
	default:
		Heading(TEXT("UT2004 was not found on this computer"), Hot);
		Para(TEXT("OpenUT2k4 is played with the maps, characters, textures and sounds of your own copy of Unreal Tournament 2004. They are converted once; your UT2004 folder is not changed."),
			White, 19.f, 10.f);
		Para(TEXT("Choose Browse and select the folder where UT2004 is installed: the one that contains the System and Maps folders, often called \"Unreal Tournament 2004\" or \"UT2004\"."),
			White, 19.f, 14.f);
		break;
	}

	// ---- extension blocks (FArenaContentSetupExtension, e.g. the OldUnreal download offer) ----
	for (const FArenaContentSetupExtension& E : Extensions())
	{
		if (E.DrawInfo && Y < Bottom)
		{
			E.DrawInfo(D, X, Y, Width);
		}
	}

	if (bShowDetails)
	{
		DetailsHeader(TEXT("Details"));
		if (View == EView::NoInstall)
		{
			DetailLine(TEXT("Places searched automatically:"), White);
			for (const FString& S : Searched)
			{
				DetailLine(TEXT("  ") + S);
			}
			DetailLine(TEXT("Other ways to set the folder: start the game with -UT2004Dir=\"<folder>\", set the environment variable OPENUT2K4_UT2004_DIR, or set UT2004Dir in the [/Script/OpenUT2k4.ArenaUserSettings] section of:"), White);
			DetailLine(TEXT("  ") + FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GeneratedConfigDir(), ANSI_TO_TCHAR(FPlatformProperties::PlatformName()), TEXT("Game.ini"))));
		}
		DetailLine(FString::Printf(TEXT("Content folder: %s"), *ArenaContent::GetCacheDir()));
		DetailLine(ArenaContent::IsCacheReady() ? TEXT("Status: complete")
			: HasPartialCache() ? TEXT("Status: partly converted (can be resumed)") : TEXT("Status: not converted yet"));
	}
}

void AArenaContentSetupHUD::DrawButtons(const FArenaDraw& D, const TArray<FButton>& Buttons, float X, float Y, float Width, float Time)
{
	using namespace ArenaGUI2K4;
	if (!GUI)
	{
		return;
	}
	// GUIButtons (STY2RoundButton, like UE2's default button style), one per row
	const float RowH = FMath::RoundToFloat(GUI->ScreenH() * 0.05f), Gap = FMath::RoundToFloat(GUI->ScreenH() * 0.012f);
	// focus follows Selection when the code chose a button
	if (FocusedSelection != Selection && Buttons.IsValidIndex(Selection))
	{
		GUI->SetFocus(FGUI::Id(*Buttons[Selection].Id.ToString()));
		FocusedSelection = Selection;
	}
	for (int32 i = 0; i < Buttons.Num(); ++i)
	{
		const FButton& B = Buttons[i];
		const uint32 Id = FGUI::Id(*B.Id.ToString());
		if (GUI->Button(Id, FRect(X, Y, Width, RowH), TEXT("RoundButton"), B.Label, FString(), B.bEnabled))
		{
			Selection = FocusedSelection = i;
			Activate(B.Id);
		}
		if (GUI->IsFocused(Id))
		{
			Selection = FocusedSelection = i;
		}
		Y += RowH + Gap;
	}
}

void AArenaContentSetupHUD::DrawHUD()
{
	using namespace ArenaGUI2K4;
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const bool bRunning = FArenaConverter::Get().IsRunning();
	if (bWasRunning && !bRunning)
	{
		bWasRunning = false;
		bShowDone = true;
		Selection = 0;
		FinishedAt = FPlatformTime::Seconds();
		const FArenaConvertProgress P = FArenaConverter::Get().GetProgress();
		if (P.bSucceeded)
		{
			// the provider indexes the cache lazily; the map list comes from data/
			ArenaRuntimeContent::ResetCacheIndex();
			AArenaLevelBuilder::RefreshImportedLayouts();
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Content setup: conversion %s (%.1fs, %d problems)"),
			P.bSucceeded ? TEXT("finished") : P.bCancelled ? TEXT("cancelled") : TEXT("failed"), P.ElapsedSeconds, P.ErrorCount);
	}
	if (bAutoStarted && bShowDone && FPlatformTime::Seconds() - FinishedAt > 1.0)
	{
		bAutoStarted = false;
		if (FArenaConverter::Get().GetProgress().bSucceeded)
		{
			Close();
			return;
		}
	}
	if (!GUI)
	{
		GUI = MakeUnique<FGUI>();
	}
	FGUI& G = *GUI;
	FGUI::SetExternalTextInput(bEditingPath);
	G.Begin(Canvas, GetOwningPlayerController(), true);
	GActiveGUI = &G;
	const FArenaDraw& D = G.Draw();
	const float W = G.ScreenW(), H = G.ScreenH();
	const float Time = GetWorld() ? GetWorld()->GetRealTimeSeconds() : 0.f;
	const EView View = GetView();
	const FRect Page = G.Page();

	// UT2k4MainPage backdrop (the menu's procedural scene before any content exists)
	if (G.HasArt())
	{
		G.PageBackground();
	}
	else
	{
		AArenaMenuHUD::DrawMenuBackdrop(D, Time, 2);
	}

	// header: "UT2004 Content | <state>"
	FString Chip;
	switch (View)
	{
	case EView::NoInstall:    Chip = TEXT("Not found"); break;
	case EView::InstallFound: Chip = HasPartialCache() ? TEXT("Incomplete") : TEXT("Ready to convert"); break;
	case EView::Ready:        Chip = TEXT("Ready"); break;
	case EView::Converting:   Chip = FString::Printf(TEXT("Converting %d%%"), FMath::RoundToInt(FArenaConverter::Get().GetProgress().Fraction * 100.f)); break;
	case EView::Done:         Chip = TEXT("Done"); break;
	case EView::Failed:       Chip = TEXT("Failed"); break;
	case EView::Paused:       Chip = TEXT("Paused"); break;
	case EView::Modal:
	{
		const FArenaContentSetupExtension* Modal = GetModalExtension();
		Chip = Modal && Modal->GetModalStatus ? Modal->GetModalStatus() : FString();
		break;
	}
	}
	const FRect HeaderR = G.Header(Chip.IsEmpty() ? FString(TEXT("UT2004 Content")) : FString(TEXT("UT2004 Content | ")) + Chip);

	// the UT2004 logo of the player's own content when converted, the game title otherwise
	const float LogoH = FMath::RoundToFloat(H * 0.13f);
	{
		const FRect LogoR(W * 0.5f - LogoH, HeaderR.B() + G.L(4.f), LogoH * 2.f, LogoH);
		if (!G.Image(TEXT("2K4Menus.MainMenu.2K4Logo"), LogoR, EImgStyle::Scaled, FLinearColor::White))
		{
			const FFontRef F = G.Font(TEXT("UT2MainMenuFont"));
			G.Text(F, ArenaBranding::GameTitle(), W * 0.5f, LogoR.Y + (LogoH - F.Height) * 0.5f, FArenaDraw::UTColor(FColor(244, 237, 205)), EAlign::Center, true);
		}
	}

	// window: a section frame over the page, the text on the left, the buttons in an AltSectionBackground on the right
	const FRect Footer = G.Footer(G.GetHint());
	const FRect Win(Page.X + Page.W * 0.01f, HeaderR.B() + LogoH + G.L(10.f), Page.W * 0.98f, Footer.Y - (HeaderR.B() + LogoH + G.L(10.f)) - G.L(30.f));
	const FRect Client = G.Section(Win, TEXT("UT2004 Content"));
	const float BW = Client.W * 0.3f;
	const FRect BtnSec(Client.R() - BW - G.L(4.f), Client.Y, BW, Client.H);
	const FRect BtnClient = G.Section(BtnSec, FString(), true);
	const FRect Body(Client.X + G.L(12.f), Client.Y + G.L(6.f), BtnSec.X - Client.X - G.L(30.f), Client.H - G.L(12.f));
	DrawBody(D, View, Body.X, Body.Y, Body.W, Body.B());

	TArray<FButton> Buttons;
	BuildButtons(Buttons);
	Selection = FMath::Clamp(Selection, 0, FMath::Max(0, Buttons.Num() - 1));
	if (Buttons.IsValidIndex(Selection) && !Buttons[Selection].bEnabled)
	{
		// keep the highlight on something usable
		for (int32 i = 0; i < Buttons.Num(); ++i)
		{
			if (Buttons[i].bEnabled) { Selection = i; break; }
		}
	}
	DrawButtons(D, Buttons, BtnClient.X + G.L(8.f), BtnClient.Y + G.L(8.f), BtnClient.W - G.L(16.f), Time);
	if (bEditingPath && TextSink.IsValid() && !TextSink->HasKeyboardFocus())
	{
		FSlateApplication::Get().SetAllUserFocus(TextSink, EFocusCause::SetDirectly);
	}
	HandleKeys(Buttons.Num());

	// our disclaimer and version above the footer
	{
		const FFontRef F = G.Font(TEXT("UT2SmallFont"));
		const float Y = Footer.Y - F.Height - G.L(4.f);
		G.Text(F, ArenaBranding::Disclaimer(), W * 0.5f, Y, Alpha(Dim, 0.6f), EAlign::Center, true);
		G.Text(F, ArenaBranding::VersionTag(), W - G.L(8.f), Y - F.Height, Alpha(Dim, 0.5f), EAlign::Right, true);
	}
	GActiveGUI = nullptr;
	G.End();
}
