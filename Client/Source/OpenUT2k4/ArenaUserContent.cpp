#include "ArenaUserContent.h"
#include "OpenUT2k4.h"
#include "ArenaContent.h"
#include "ArenaContentSetup.h"
#include "ArenaHUD.h"
#include "ArenaLevelBuilder.h"
#include "ArenaMenuGameMode.h"
#include "ArenaRuntimeContent.h"
#include "Convert/ArenaConverter.h"
#include "Convert/ArenaConvJson.h"
#include "Convert/ArenaConvUserContent.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace ArenaUserContentLocal
{
	using namespace ArenaUserContent;

	struct FJob
	{
		FCriticalSection Lock;
		FScanStatus Status;
		TAtomic<bool> bCancel{ false };
		TAtomic<bool> bBusy{ false };
		/** A scan finished and the game thread has not picked up its results yet. */
		TAtomic<bool> bUnhandled{ false };
		TFuture<void> Future;
	};

	FJob& Job()
	{
		static FJob J;
		return J;
	}

	struct FResults
	{
		bool bLoaded = false;
		TArray<FMapInfo> Maps;
		TArray<FProblem> Problems;
		TMap<FString, FString> ScriptPackages;
	};

	FResults& Results()
	{
		static FResults R;
		return R;
	}

	bool NoUserContent()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("NoUserContent"));
	}

	ArenaConvUserContent::FOptions MakeOptions(bool bForce)
	{
		ArenaConvUserContent::FOptions Opt;
		Opt.InstallDir = ArenaContent::FindUT2004Install();
		Opt.CacheDir = ArenaContent::GetCacheDir();
		Opt.UserDir = ArenaContent::GetUserContentDir();
		Opt.bForce = bForce;
		return Opt;
	}

	/** One scan on the calling thread; updates Job().Status. */
	void RunScan(const ArenaConvUserContent::FOptions& Opt)
	{
		FJob& J = Job();
		const double T0 = FPlatformTime::Seconds();
		ArenaConvUserContent::FStats Stats;
		const bool bOk = ArenaConvUserContent::Run(Opt, [&J, T0](const ArenaConvUserContent::FProgress& P)
		{
			FScopeLock L(&J.Lock);
			J.Status.Stage = P.Stage;
			J.Status.Item = P.Item;
			J.Status.Fraction = P.Fraction;
			J.Status.Done = P.Done;
			J.Status.Total = P.Total;
			J.Status.ElapsedSeconds = FPlatformTime::Seconds() - T0;
		}, &J.bCancel, Stats);
		FString Stages;
		for (const TPair<FString, double>& KV : Stats.StageSeconds)
		{
			Stages += FString::Printf(TEXT(" %s %.2fs"), *KV.Key, KV.Value);
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Custom content: scan %s in %.2fs: %d converted (%s), %d removed, %d unchanged, %d cannot be played, %d errors, %d older maps updated; stages:%s"),
			bOk ? TEXT("finished") : Stats.bCancelled ? TEXT("cancelled") : TEXT("FAILED"), FPlatformTime::Seconds() - T0, Stats.ConvertedMaps.Num(),
			*FString::Join(Stats.ConvertedMaps, TEXT(", ")), Stats.RemovedMaps.Num(), Stats.UnchangedMaps, Stats.UnplayableMaps, Stats.Errors, Stats.UpdatedMaps.Num(), *Stages);
		{
			FScopeLock L(&J.Lock);
			J.Status.bRunning = false;
			J.Status.bFinished = true;
			J.Status.bOk = bOk;
			J.Status.bCancelled = Stats.bCancelled;
			J.Status.FatalError = Stats.FatalError;
			J.Status.ElapsedSeconds = FPlatformTime::Seconds() - T0;
			J.Status.Fraction = bOk ? 1.f : J.Status.Fraction;
			J.Status.ConvertedMaps = Stats.ConvertedMaps;
			J.Status.RemovedMaps = Stats.RemovedMaps;
			J.Status.UpdatedMaps = Stats.UpdatedMaps;
			J.Status.UnchangedMaps = Stats.UnchangedMaps;
		}
		J.bBusy = false;
		J.bUnhandled = true;
	}

	void LoadResults()
	{
		FResults& R = Results();
		R = FResults();
		R.bLoaded = true;
		FConvJson State;
		if (!FConvJson::LoadFile(ArenaConvUserContent::StatePath(ArenaContent::GetCacheDir()), State))
		{
			return;
		}
		// a state left over from another custom content folder does not describe this one
		if (State.GetString(TEXT("user_dir")) != FPaths::ConvertRelativePathToFull(ArenaContent::GetUserContentDir()))
		{
			return;
		}
		const TFunction<bool(const FString&)>& Support = ScriptClassSupport();
		for (const TPair<FString, FConvJson>& KV : State.Get(TEXT("maps")).O)
		{
			const FConvJson& M = KV.Value;
			FMapInfo I;
			I.Name = KV.Key;
			I.Title = M.GetString(TEXT("title"), KV.Key);
			I.Author = M.GetString(TEXT("author"));
			I.Description = M.GetString(TEXT("description"));
			const FConvJson& Pl = M.Get(TEXT("players"));
			if (Pl.Num() == 2)
			{
				I.PlayersMin = int32(Pl.A[0].AsNumber());
				I.PlayersMax = int32(Pl.A[1].AsNumber());
			}
			I.Screenshot = M.GetString(TEXT("screenshot"));
			I.File = M.GetString(TEXT("file"));
			I.Archive = M.GetString(TEXT("archive"));
			for (const FConvJson& P : M.Get(TEXT("problems")).A)
			{
				I.Problems.Add(P.S);
			}
			for (const FConvJson& P : M.Get(TEXT("missing")).A)
			{
				I.MissingPackages.Add(P.S);
			}
			TArray<FString> Unsupported;
			for (const FConvJson& C : M.Get(TEXT("script_classes")).A)
			{
				const FString Class = C.GetString(TEXT("class"));
				const int32 Count = int32(C.GetNumber(TEXT("count"), 1));
				I.ScriptClasses.Emplace(Class, Count);
				if (!Support || !Support(Class))
				{
					Unsupported.Add(Count > 1 ? FString::Printf(TEXT("%s (x%d)"), *Class, Count) : Class);
				}
			}
			if (M.GetString(TEXT("status")) != TEXT("ok"))
			{
				I.Status = EMapStatus::Unplayable;
			}
			else
			{
				if (Unsupported.Num())
				{
					TArray<FString> Shown;
					for (int32 i = 0; i < Unsupported.Num() && i < 4; ++i)
					{
						Shown.Add(Unsupported[i]);
					}
					I.Problems.Insert(FString::Printf(TEXT("Uses custom scripts (not supported yet): %s%s. These parts are left out."), *FString::Join(Shown, TEXT(", ")),
						Unsupported.Num() > 4 ? *FString::Printf(TEXT(" and %d more"), Unsupported.Num() - 4) : TEXT("")), 0);
				}
				I.Status = I.Problems.Num() ? EMapStatus::Limited : EMapStatus::Ready;
			}
			R.Maps.Add(MoveTemp(I));
		}
		R.Maps.Sort([](const FMapInfo& A, const FMapInfo& B) { return A.Name < B.Name; });
		for (const FConvJson& P : State.Get(TEXT("problems")).A)
		{
			FProblem Pr;
			Pr.File = P.GetString(TEXT("file"));
			Pr.Reason = P.GetString(TEXT("reason"));
			Pr.bError = P.GetString(TEXT("severity")) != TEXT("info");
			R.Problems.Add(Pr);
		}
		for (const TPair<FString, FConvJson>& KV : State.Get(TEXT("script_packages")).O)
		{
			R.ScriptPackages.Add(KV.Key, KV.Value.GetString(TEXT("file")));
			if (!KV.Value.GetString(TEXT("int")).IsEmpty())
			{
				R.ScriptPackages.Add(KV.Key + TEXT(".int"), KV.Value.GetString(TEXT("int")));
			}
		}
	}

	FString Plural(int32 N, const TCHAR* One, const TCHAR* Many)
	{
		return FString::Printf(TEXT("%d %s"), N, N == 1 ? One : Many);
	}

	// ---- setup screen page ----

	struct FPage
	{
		/** The custom content page is open (modal block of the setup screen). */
		bool bShowList = false;
		/** Scans started in this session (for the "last scan" line). */
		bool bScannedHere = false;
	};
	FPage GPage;

	bool PageModal()
	{
		return GPage.bShowList || IsScanning();
	}

	void DrawPage(const FArenaDraw& D, float X, float& Y, float Width)
	{
		using namespace ArenaSetupStyle;
		if (!ArenaContent::UseRuntimeContent() || !ArenaContent::IsCacheReady() || NoUserContent())
		{
			return;
		}
		auto Para = [&](const FString& Text, const FLinearColor& Color, float Size, float Gap)
		{
			Y = AArenaContentSetupHUD::DrawParagraph(D, Text, X, Y, Width, Color, Size, Gap);
		};
		const TArray<FMapInfo>& Maps = GetMaps();
		int32 NReady = 0, NLimited = 0, NBad = 0;
		for (const FMapInfo& M : Maps)
		{
			NReady += M.Status == EMapStatus::Ready;
			NLimited += M.Status == EMapStatus::Limited;
			NBad += M.Status == EMapStatus::Unplayable;
		}
		const FScanStatus St = GetStatus();
		if (!PageModal())
		{
			// summary under the screen's own text
			D.Rect(X, Y, Width, FMath::Max(1.f, D.S(1.f)), FLinearColor(Accent().R, Accent().G, Accent().B, 0.35f));
			Y += D.S(14.f);
			Para(TEXT("Custom content"), White(), 21.f, 4.f);
			if (Maps.Num() == 0)
			{
				Para(TEXT("Add community maps (a .ut2 file with the packages it comes with, or a .umod / .zip) to the custom content folder, then choose Rescan custom content."),
					Dim(), 17.f, 6.f);
			}
			else
			{
				FString Line = FString::Printf(TEXT("%s ready"), *Plural(NReady + NLimited, TEXT("custom map"), TEXT("custom maps")));
				if (NBad)
				{
					Line += FString::Printf(TEXT(", %d cannot be played (see Custom content)"), NBad);
				}
				Para(Line + TEXT("."), NBad ? Hot() : Good(), 17.f, 6.f);
			}
			Para(ArenaContent::GetUserContentDir(), Dim(), 15.f, 10.f);
			return;
		}
		if (St.bRunning)
		{
			if (St.Stage.StartsWith(TEXT("Updating")))
			{
				Para(TEXT("Updating converted maps..."), White(), 30.f, 12.f);
				Para(TEXT("This version of the game reads more from UT2004 maps (zones, skyboxes, movers). Maps converted earlier are updated once; the rest of your content is kept."),
					Dim(), 17.f, 18.f);
			}
			else
			{
				Para(TEXT("Converting custom content"), White(), 30.f, 12.f);
				Para(TEXT("Only new and changed maps are converted; your UT2004 content stays as it is."), Dim(), 17.f, 18.f);
			}
			Para(St.Stage.IsEmpty() ? FString(TEXT("Starting")) : St.Stage, Accent(), 20.f, 8.f);
			const float BarH = D.S(22.f);
			AArenaContentSetupHUD::DrawProgressBar(D, X, Y, Width, BarH, St.Fraction);
			Y += BarH + D.S(10.f);
			FString Stats = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(St.Fraction * 100.f));
			if (St.Total > 0)
			{
				Stats += FString::Printf(TEXT("   -   %d of %d"), FMath::Min(St.Done + 1, St.Total), St.Total);
			}
			Stats += FString::Printf(TEXT("   -   %.0f s"), St.ElapsedSeconds);
			Para(Stats, White(), 18.f, 8.f);
			if (!St.Item.IsEmpty())
			{
				Para(St.Item, Dim(), 15.f, 10.f);
			}
			return;
		}
		Para(TEXT("Custom content"), White(), 30.f, 8.f);
		Para(FString::Printf(TEXT("Folder: %s"), *ArenaContent::GetUserContentDir()), Dim(), 15.f, 10.f);
		if (GPage.bScannedHere && St.bFinished)
		{
			if (!St.FatalError.IsEmpty())
			{
				Para(St.FatalError, Bad(), 17.f, 8.f);
			}
			else if (St.bCancelled)
			{
				Para(TEXT("The scan was stopped. Rescan continues with what is left."), Hot(), 17.f, 8.f);
			}
			else
			{
				Para(FString::Printf(TEXT("Scanned in %.1f s: %s converted, %d removed, %d unchanged."), St.ElapsedSeconds,
					*Plural(St.ConvertedMaps.Num(), TEXT("map"), TEXT("maps")), St.RemovedMaps.Num(), St.UnchangedMaps), Good(), 17.f, 8.f);
				if (St.UpdatedMaps.Num())
				{
					Para(FString::Printf(TEXT("%s converted earlier were updated for this version of the game."), *Plural(St.UpdatedMaps.Num(), TEXT("UT2004 map"), TEXT("UT2004 maps"))),
						Good(), 17.f, 8.f);
				}
			}
		}
		if (Maps.Num() == 0 && GetProblems().Num() == 0)
		{
			Para(TEXT("No custom maps yet. Put community maps in this folder: the .ut2 map with the packages it comes with (.utx, .usx, .uax, .ukx, .u, .ogg), loose or in the UT2004 folder layout, or the .umod / .zip it was downloaded as. Then choose Rescan custom content."),
				White(), 17.f, 8.f);
			return;
		}
		Para(FString::Printf(TEXT("%s ready, %d with limits, %d cannot be played."), *Plural(NReady, TEXT("map"), TEXT("maps")), NLimited, NBad), White(), 18.f, 10.f);
		// one line per map, then the reasons
		const float MaxY = D.H() - D.S(150.f);
		int32 Shown = 0;
		TArray<const FMapInfo*> Sorted;
		for (const FMapInfo& M : Maps)
		{
			Sorted.Add(&M);
		}
		Sorted.StableSort([](const FMapInfo& A, const FMapInfo& B) { return int32(A.Status) > int32(B.Status); });
		for (const FMapInfo* M : Sorted)
		{
			if (Y > MaxY)
			{
				break;
			}
			const FLinearColor Col = M->Status == EMapStatus::Ready ? Good() : M->Status == EMapStatus::Limited ? Hot() : Bad();
			FString Head = M->Title.Equals(M->Name) || M->Title.IsEmpty() ? M->Name : FString::Printf(TEXT("%s  (%s)"), *M->Title, *M->Name);
			if (!M->Author.IsEmpty())
			{
				Head += TEXT("  by ") + M->Author;
			}
			Head += M->Status == EMapStatus::Ready ? TEXT("  -  ready") : M->Status == EMapStatus::Limited ? TEXT("  -  plays with limits") : TEXT("  -  cannot be played");
			Para(Head, Col, 17.f, 2.f);
			for (const FString& P : M->Problems)
			{
				Y = AArenaContentSetupHUD::DrawParagraph(D, P, X + D.S(18.f), Y, Width - D.S(18.f), Dim(), 15.f, 2.f);
			}
			Y += D.S(6.f);
			++Shown;
		}
		int32 ShownProblems = 0;
		for (const FProblem& P : GetProblems())
		{
			if (Y > MaxY)
			{
				break;
			}
			Para(FString::Printf(TEXT("%s %s"), *P.File, *P.Reason), P.bError ? Bad() : Dim(), 15.f, 4.f);
			++ShownProblems;
		}
		const int32 Hidden = (Sorted.Num() - Shown) + (GetProblems().Num() - ShownProblems);
		if (Hidden > 0)
		{
			Para(FString::Printf(TEXT("... and %d more (full list: %s)"), Hidden, *ArenaConvUserContent::StatePath(ArenaContent::GetCacheDir())), Dim(), 15.f, 4.f);
		}
	}

	struct FRegister
	{
		bool bStartupDone = false;
		bool bOpenScreen = false;
		bool bConverterWasRunning = false;
		bool bTestHook = false;

		FRegister()
		{
			const FName Group(TEXT("UserContent"));
			FArenaContentSetupExtension Page;
			Page.Id = FName(TEXT("UserContentPage"));
			Page.Group = Group;
			Page.GetLabel = []() -> FString
			{
				if (!ArenaContent::IsCacheReady() || NoUserContent() || IsScanning())
				{
					return FString();
				}
				return GPage.bShowList ? FString(TEXT("Done")) : FString(TEXT("Custom content..."));
			};
			Page.OnActivate = [](AArenaContentSetupHUD*) { GPage.bShowList = !GPage.bShowList; };
			Page.DrawInfo = [](const FArenaDraw& D, float X, float& Y, float Width) { DrawPage(D, X, Y, Width); };
			Page.IsBusy = []() { return IsScanning(); };
			Page.IsModal = []() { return PageModal(); };
			Page.GetModalStatus = []() -> FString
			{
				const FScanStatus S = GetStatus();
				if (S.bRunning)
				{
					return FString::Printf(TEXT("%s  %d%%"), S.Stage.StartsWith(TEXT("Updating")) ? TEXT("UPDATING MAPS") : TEXT("CUSTOM CONTENT"), FMath::RoundToInt(S.Fraction * 100.f));
				}
				return TEXT("CUSTOM CONTENT");
			};
			AArenaContentSetupHUD::RegisterExtension(Page);

			FArenaContentSetupExtension Rescan;
			Rescan.Id = FName(TEXT("UserContentRescan"));
			Rescan.Group = Group;
			Rescan.GetLabel = []() -> FString
			{
				if (!ArenaContent::IsCacheReady() || NoUserContent())
				{
					return FString();
				}
				return IsScanning() ? FString(TEXT("Stop")) : FString(TEXT("Rescan custom content"));
			};
			Rescan.IsEnabled = []() { return IsScanning() || !FArenaConverter::Get().IsRunning(); };
			Rescan.OnActivate = [](AArenaContentSetupHUD*)
			{
				if (IsScanning())
				{
					CancelScan();
					return;
				}
				GPage.bShowList = true;
				GPage.bScannedHere = true;
				StartScan(false);
			};
			AArenaContentSetupHUD::RegisterExtension(Rescan);

			FArenaContentSetupExtension Open;
			Open.Id = FName(TEXT("UserContentOpen"));
			Open.Group = Group;
			Open.GetLabel = []() -> FString { return GPage.bShowList && !IsScanning() ? FString(TEXT("Open the folder")) : FString(); };
			Open.OnActivate = [](AArenaContentSetupHUD*)
			{
				const FString Dir = ArenaContent::GetUserContentDir();
				IFileManager::Get().MakeDirectory(*Dir, true);
				FPlatformProcess::ExploreFolder(*Dir);
			};
			AArenaContentSetupHUD::RegisterExtension(Open);

			FCoreDelegates::OnFEngineLoopInitComplete.AddRaw(this, &FRegister::OnEngineReady);
			FCoreDelegates::OnPreExit.AddLambda([]()
			{
				FJob& J = Job();
				J.bCancel = true;
				if (J.Future.IsValid())
				{
					J.Future.Wait();
				}
			});
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FRegister::Tick), 0.25f);
		}

		void OnEngineReady()
		{
			const TCHAR* Cmd = FCommandLine::Get();
			if (!ArenaContent::UseRuntimeContent() || NoUserContent() || FParse::Param(Cmd, TEXT("ConvertOnly")))
			{
				return;
			}
			// the folder exists from the first start on, so players can find it
			const FString Dir = ArenaContent::GetUserContentDir();
			if (!IFileManager::Get().DirectoryExists(*Dir))
			{
				for (const TCHAR* Sub : { TEXT("Maps"), TEXT("Textures"), TEXT("StaticMeshes"), TEXT("Sounds"), TEXT("Music"), TEXT("System"), TEXT("Animations"), TEXT("KarmaData") })
				{
					IFileManager::Get().MakeDirectory(*(Dir / Sub), true);
				}
			}
			if (FParse::Param(Cmd, TEXT("UserContentScan")))
			{
				// headless: scan once on this thread, then quit
				bool bOk = false;
				if (CanScan())
				{
					FJob& J = Job();
					J.bBusy = true;
					{
						FScopeLock L(&J.Lock);
						J.Status = FScanStatus();
						J.Status.bRunning = true;
					}
					RunScan(MakeOptions(FParse::Param(Cmd, TEXT("UserContentForce"))));
					bOk = GetStatus().bOk;
				}
				else
				{
					UE_LOG(LogOpenUT2k4, Error, TEXT("UserContentScan: no ready content cache or UT2004 install"));
				}
				LoadResults();
				for (const FMapInfo& M : Results().Maps)
				{
					UE_LOG(LogOpenUT2k4, Display, TEXT("UserContentScan: %s [%s] \"%s\" by \"%s\" players %d-%d shot=%s%s%s"), *M.Name,
						M.Status == EMapStatus::Ready ? TEXT("ready") : M.Status == EMapStatus::Limited ? TEXT("limited") : TEXT("unplayable"),
						*M.Title, *M.Author, M.PlayersMin, M.PlayersMax, *M.Screenshot, M.Problems.Num() ? TEXT(" - ") : TEXT(""), *FString::Join(M.Problems, TEXT(" | ")));
				}
				for (const FProblem& P : Results().Problems)
				{
					UE_LOG(LogOpenUT2k4, Display, TEXT("UserContentScan: file %s %s"), *P.File, *P.Reason);
				}
				FPlatformMisc::RequestExitWithStatus(true, bOk ? 0 : 1);
				return;
			}
			GPage.bShowList = FParse::Param(Cmd, TEXT("UserContentShowList"));
			if (CanScan() && HasChanges() && StartScan(false))
			{
				GPage.bScannedHere = true;
				bOpenScreen = true;
			}
		}

		bool Tick(float)
		{
			FJob& J = Job();
			if (J.bUnhandled.Exchange(false))
			{
				const FScanStatus S = GetStatus();
				if (S.ConvertedMaps.Num() || S.RemovedMaps.Num() || S.UpdatedMaps.Num())
				{
					ArenaRuntimeContent::ResetCacheIndex();
				}
				Results().bLoaded = false;
				AArenaLevelBuilder::RefreshImportedLayouts();
			}
			// a finished full conversion: bring the custom content up to date
			const bool bConv = FArenaConverter::Get().IsRunning();
			if (bConverterWasRunning && !bConv && FArenaConverter::Get().GetProgress().bSucceeded && CanScan() && HasChanges() && StartScan(false))
			{
				GPage.bScannedHere = true;
			}
			bConverterWasRunning = bConv;
			// a startup scan shows its progress on the setup screen
			if (bOpenScreen && GEngine)
			{
				for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
				{
					UWorld* World = Ctx.World();
					APlayerController* PC = World && (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE) ? World->GetFirstPlayerController() : nullptr;
					if (PC && Cast<AArenaMenuHUD>(PC->GetHUD()))
					{
						bOpenScreen = false;
						const FScanStatus S = GetStatus();
						if (S.bRunning || S.ConvertedMaps.Num() || S.RemovedMaps.Num() || S.UpdatedMaps.Num())
						{
							GPage.bShowList = true;
							AArenaContentSetupHUD::Open(PC);
						}
						break;
					}
				}
			}
			return true;
		}
	};
	static FRegister GRegister;
}

using namespace ArenaUserContentLocal;

namespace ArenaUserContent
{
	bool CanScan()
	{
		return ArenaContent::UseRuntimeContent() && !NoUserContent() && ArenaContent::IsCacheReady() && !ArenaContent::FindUT2004Install().IsEmpty();
	}

	bool StartScan(bool bForce)
	{
		FJob& J = Job();
		if (J.bBusy.Load() || FArenaConverter::Get().IsRunning() || !CanScan())
		{
			return false;
		}
		const ArenaConvUserContent::FOptions Opt = MakeOptions(bForce);
		J.bBusy = true;
		J.bCancel = false;
		{
			FScopeLock L(&J.Lock);
			J.Status = FScanStatus();
			J.Status.bRunning = true;
			J.Status.Stage = TEXT("Starting");
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Custom content: scanning %s"), *Opt.UserDir);
		J.Future = Async(EAsyncExecution::Thread, [Opt]() { RunScan(Opt); });
		return true;
	}

	void CancelScan()
	{
		Job().bCancel = true;
	}

	bool IsScanning()
	{
		return Job().bBusy.Load();
	}

	FScanStatus GetStatus()
	{
		FJob& J = Job();
		FScopeLock L(&J.Lock);
		return J.Status;
	}

	bool HasChanges()
	{
		return ArenaConvUserContent::HasChanges(MakeOptions(false));
	}

	const TArray<FMapInfo>& GetMaps()
	{
		if (!Results().bLoaded)
		{
			LoadResults();
		}
		return Results().Maps;
	}

	const FMapInfo* FindMap(const FString& MapName)
	{
		for (const FMapInfo& M : GetMaps())
		{
			if (M.Name.Equals(MapName, ESearchCase::IgnoreCase))
			{
				return &M;
			}
		}
		return nullptr;
	}

	const TArray<FProblem>& GetProblems()
	{
		GetMaps();
		return Results().Problems;
	}

	TMap<FString, FString> GetScriptPackages()
	{
		GetMaps();
		return Results().ScriptPackages;
	}

	TFunction<bool(const FString& ClassPath)>& ScriptClassSupport()
	{
		static TFunction<bool(const FString&)> Hook;
		return Hook;
	}

	void RefreshMapStatus()
	{
		Results().bLoaded = false;
		AArenaLevelBuilder::RefreshImportedLayouts();
	}
}
