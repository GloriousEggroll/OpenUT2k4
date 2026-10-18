#include "ArenaFolderPicker.h"
#include "OpenUT2k4.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UnrealEngine.h"
#include "Widgets/SWindow.h"

#if PLATFORM_LINUX
#include "SDL3/SDL.h"
#elif PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shobjidl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC
#include "Mac/CocoaThread.h"
#include <AppKit/AppKit.h>
#endif

namespace ArenaFolderPickerLocal
{
	using ArenaFolderPicker::FResult;

	/**
	 * One dialog at a time. The backends finish on any thread (SDL's portal reply arrives while the game pumps
	 * SDL events, zenity / kdialog / the Windows dialog on worker threads, NSOpenPanel on the Cocoa thread) and
	 * only store the result; a core ticker hands it to the callback on the game thread.
	 */
	struct FState
	{
		FCriticalSection Lock;
		bool bOpen = false;
		bool bHasResult = false;
		FResult Result;
		TFunction<void(const FResult&)> Callback;
		FString Title;
		FString StartDir;
		FTSTicker::FDelegateHandle Ticker;
		/** Window mode to restore when the dialog closes (only when it was changed). */
		bool bRestoreMode = false;
		EWindowMode::Type RestoreMode = EWindowMode::Windowed;
		int32 RestoreX = 0, RestoreY = 0;
	};
	FState GState;

	void Deliver(const FResult& R)
	{
		FScopeLock L(&GState.Lock);
		GState.Result = R;
		GState.bHasResult = true;
	}

	/** Native handle of the game window (SDL_Window* / HWND / NSWindow*), or null. */
	void* GetGameWindowHandle()
	{
		if (!GEngine || !GEngine->GameViewport)
		{
			return nullptr;
		}
		const TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
		if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
		{
			return nullptr;
		}
		return Window->GetNativeWindow()->GetOSWindowHandle();
	}

	/** Leaves fullscreen while the dialog is open (see the header). */
	void LeaveFullscreen()
	{
		const EWindowMode::Type Mode = GSystemResolution.WindowMode;
#if PLATFORM_LINUX
		const bool bLeave = Mode != EWindowMode::Windowed;
#else
		const bool bLeave = Mode == EWindowMode::Fullscreen;
#endif
		if (!bLeave || GSystemResolution.ResX <= 0 || GSystemResolution.ResY <= 0)
		{
			return;
		}
		GState.bRestoreMode = true;
		GState.RestoreMode = Mode;
		GState.RestoreX = GSystemResolution.ResX;
		GState.RestoreY = GSystemResolution.ResY;
		// a window a bit smaller than the screen, so the dialog has room next to it
		const int32 WX = FMath::Max(640, FMath::RoundToInt(GState.RestoreX * 0.75f));
		const int32 WY = FMath::Max(360, FMath::RoundToInt(GState.RestoreY * 0.75f));
		UE_LOG(LogOpenUT2k4, Log, TEXT("Folder picker: leaving fullscreen while the dialog is open (%dx%d window)"), WX, WY);
		FSystemResolution::RequestResolutionChange(WX, WY, EWindowMode::Windowed);
	}

	void RestoreFullscreen()
	{
		if (GState.bRestoreMode)
		{
			GState.bRestoreMode = false;
			FSystemResolution::RequestResolutionChange(GState.RestoreX, GState.RestoreY, GState.RestoreMode);
		}
	}

#if PLATFORM_LINUX
	/** Absolute path of a program on $PATH, or empty. */
	FString FindProgram(const TCHAR* Name)
	{
		TArray<FString> Dirs;
		FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")).ParseIntoArray(Dirs, TEXT(":"), true);
		for (const FString& Dir : Dirs)
		{
			const FString Candidate = Dir / Name;
			if (FPaths::FileExists(Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}

	/** kdialog / zenity started directly (when SDL has no dialog backend, e.g. no portal and an old zenity). Worker thread. */
	void RunCommandLineDialog(const FString& Title, const FString& StartDir, const FString& SdlError)
	{
		struct FTool { const TCHAR* Name; FString Args; };
		const FString Start = StartDir.IsEmpty() ? FString(FPlatformProcess::UserHomeDir()) : StartDir;
		const FTool Tools[] = {
			{ TEXT("kdialog"), FString::Printf(TEXT("--title \"%s\" --getexistingdirectory \"%s\""), *Title, *Start) },
			{ TEXT("zenity"), FString::Printf(TEXT("--file-selection --directory --title=\"%s\" --filename=\"%s/\""), *Title, *Start) },
		};
		for (const FTool& Tool : Tools)
		{
			const FString Exe = FindProgram(Tool.Name);
			if (Exe.IsEmpty())
			{
				continue;
			}
			int32 Code = -1;
			FString Out, Err;
			if (!FPlatformProcess::ExecProcess(*Exe, *Tool.Args, &Code, &Out, &Err))
			{
				continue;
			}
			FResult R;
			R.Backend = Tool.Name;
			Out.TrimStartAndEndInline();
			if (Code == 0 && !Out.IsEmpty())
			{
				R.bChosen = true;
				R.Path = Out;
			}
			else
			{
				// both tools exit with 1 when the dialog is closed without a choice
				R.bCancelled = true;
			}
			Deliver(R);
			return;
		}
		FResult R;
		R.bUnavailable = true;
		R.Error = FString::Printf(TEXT("No folder dialog is available (%s; kdialog and zenity are not installed)."), *SdlError);
		Deliver(R);
	}

	void SDLCALL OnSdlDialog(void*, const char* const* FileList, int)
	{
		if (!FileList)
		{
			// no backend (or it failed): try the command line tools on a worker thread
			const FString SdlError = UTF8_TO_TCHAR(SDL_GetError());
			UE_LOG(LogOpenUT2k4, Log, TEXT("Folder picker: SDL dialog unavailable (%s), trying kdialog / zenity"), *SdlError);
			FString Title, StartDir;
			{
				FScopeLock L(&GState.Lock);
				Title = GState.Title;
				StartDir = GState.StartDir;
			}
			Async(EAsyncExecution::Thread, [Title, StartDir, SdlError]() { RunCommandLineDialog(Title, StartDir, SdlError); });
			return;
		}
		FResult R;
		const char* Driver = SDL_GetHint(SDL_HINT_FILE_DIALOG_DRIVER);
		R.Backend = Driver ? FString(UTF8_TO_TCHAR(Driver)) : FString(TEXT("xdg-desktop-portal / zenity (SDL)"));
		if (FileList[0])
		{
			R.bChosen = true;
			R.Path = UTF8_TO_TCHAR(FileList[0]);
		}
		else
		{
			R.bCancelled = true;
		}
		Deliver(R);
	}

	void OpenPlatformDialog(const FString& Title, const FString& StartDir)
	{
		SDL_Window* Parent = static_cast<SDL_Window*>(GetGameWindowHandle());
		const FTCHARToUTF8 Start(*StartDir);
		const SDL_PropertiesID Props = SDL_CreateProperties();
		SDL_SetPointerProperty(Props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, Parent);
		SDL_SetStringProperty(Props, SDL_PROP_FILE_DIALOG_TITLE_STRING, TCHAR_TO_UTF8(*Title));
		SDL_SetStringProperty(Props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Select");
		if (!StartDir.IsEmpty())
		{
			SDL_SetStringProperty(Props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, Start.Get());
		}
		SDL_SetBooleanProperty(Props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
		// SDL copies what it needs; the result arrives through OnSdlDialog (maybe right away when there is no backend)
		SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, &OnSdlDialog, nullptr, Props);
		SDL_DestroyProperties(Props);
	}

#elif PLATFORM_WINDOWS
	void OpenPlatformDialog(const FString& Title, const FString& StartDir)
	{
		const HWND Parent = static_cast<HWND>(GetGameWindowHandle());
		Async(EAsyncExecution::Thread, [Title, StartDir, Parent]()
		{
			FResult R;
			R.Backend = TEXT("Windows");
			const HRESULT Init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			IFileOpenDialog* Dialog = nullptr;
			HRESULT Hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog));
			if (SUCCEEDED(Hr) && Dialog)
			{
				DWORD Flags = 0;
				Dialog->GetOptions(&Flags);
				Dialog->SetOptions(Flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
				Dialog->SetTitle(*Title);
				if (!StartDir.IsEmpty())
				{
					IShellItem* Folder = nullptr;
					const FString Native = FPaths::ConvertRelativePathToFull(StartDir).Replace(TEXT("/"), TEXT("\\"));
					if (SUCCEEDED(SHCreateItemFromParsingName(*Native, nullptr, IID_PPV_ARGS(&Folder))) && Folder)
					{
						Dialog->SetFolder(Folder);
						Folder->Release();
					}
				}
				Hr = Dialog->Show(Parent);
				if (SUCCEEDED(Hr))
				{
					IShellItem* Item = nullptr;
					if (SUCCEEDED(Dialog->GetResult(&Item)) && Item)
					{
						PWSTR Path = nullptr;
						if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path)) && Path)
						{
							R.bChosen = true;
							R.Path = FString(Path);
							FPaths::NormalizeDirectoryName(R.Path);
							CoTaskMemFree(Path);
						}
						Item->Release();
					}
					if (!R.bChosen)
					{
						R.bUnavailable = true;
						R.Error = TEXT("The chosen folder is not on the file system.");
					}
				}
				else if (Hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
				{
					R.bCancelled = true;
				}
				else
				{
					R.bUnavailable = true;
					R.Error = FString::Printf(TEXT("The folder dialog failed (0x%08x)."), uint32(Hr));
				}
				Dialog->Release();
			}
			else
			{
				R.bUnavailable = true;
				R.Error = FString::Printf(TEXT("The folder dialog is not available (0x%08x)."), uint32(Hr));
			}
			if (SUCCEEDED(Init))
			{
				CoUninitialize();
			}
			Deliver(R);
		});
	}

#elif PLATFORM_MAC
	void OpenPlatformDialog(const FString& Title, const FString& StartDir)
	{
		NSString* NSTitle = [NSString stringWithFString:Title];
		NSString* NSStart = StartDir.IsEmpty() ? nil : [NSString stringWithFString:StartDir];
		[NSTitle retain];
		[NSStart retain];
		// AppKit lives on the Cocoa main thread (not the game thread); the panel is non-modal, so nothing blocks
		MainThreadCall(^{
			NSOpenPanel* Panel = [NSOpenPanel openPanel];
			[Panel setCanChooseFiles:NO];
			[Panel setCanChooseDirectories:YES];
			[Panel setAllowsMultipleSelection:NO];
			[Panel setCanCreateDirectories:NO];
			[Panel setMessage:NSTitle];
			[Panel setTitle:NSTitle];
			[Panel setPrompt:@"Select"];
			if (NSStart)
			{
				[Panel setDirectoryURL:[NSURL fileURLWithPath:NSStart isDirectory:YES]];
			}
			[NSTitle release];
			[NSStart release];
			[Panel beginWithCompletionHandler:^(NSModalResponse Response)
			{
				FResult R;
				R.Backend = TEXT("macOS");
				if (Response == NSModalResponseOK && [[Panel URLs] count] > 0)
				{
					R.bChosen = true;
					R.Path = FString([[[Panel URLs] objectAtIndex:0] path]);
				}
				else
				{
					R.bCancelled = true;
				}
				Deliver(R);
			}];
		}, false);
	}

#else
	void OpenPlatformDialog(const FString&, const FString&)
	{
		FResult R;
		R.bUnavailable = true;
		R.Error = TEXT("Folder dialogs are not supported on this platform.");
		Deliver(R);
	}
#endif

	/** Game thread: hands a finished dialog's result to the callback. */
	bool Tick(float)
	{
		FResult R;
		TFunction<void(const FResult&)> Callback;
		{
			FScopeLock L(&GState.Lock);
			if (!GState.bHasResult)
			{
				return true;
			}
			R = GState.Result;
			Callback = MoveTemp(GState.Callback);
			GState.bHasResult = false;
			GState.bOpen = false;
			GState.Callback = nullptr;
		}
		RestoreFullscreen();
		UE_LOG(LogOpenUT2k4, Log, TEXT("Folder picker: %s via %s%s%s"), R.bChosen ? TEXT("chosen") : R.bCancelled ? TEXT("cancelled") : TEXT("unavailable"),
			R.Backend.IsEmpty() ? TEXT("-") : *R.Backend, R.bChosen ? TEXT(": ") : (R.Error.IsEmpty() ? TEXT("") : TEXT(": ")), R.bChosen ? *R.Path : *R.Error);
		if (Callback)
		{
			Callback(R);
		}
		return true;
	}
}

bool ArenaFolderPicker::Open(const FString& Title, const FString& StartDir, TFunction<void(const FResult&)> Callback)
{
	using namespace ArenaFolderPickerLocal;
	check(IsInGameThread());
	{
		FScopeLock L(&GState.Lock);
		if (GState.bOpen)
		{
			return false;
		}
		GState.bOpen = true;
		GState.bHasResult = false;
		GState.Callback = MoveTemp(Callback);
		GState.Title = Title;
		GState.StartDir = StartDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*StartDir) ? FString() : FPaths::ConvertRelativePathToFull(StartDir);
	}
	if (!GState.Ticker.IsValid())
	{
		GState.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&ArenaFolderPickerLocal::Tick), 0.1f);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Folder picker: opening (start folder \"%s\")"), *GState.StartDir);
	LeaveFullscreen();
	OpenPlatformDialog(Title, GState.StartDir);
	return true;
}

bool ArenaFolderPicker::IsOpen()
{
	FScopeLock L(&ArenaFolderPickerLocal::GState.Lock);
	return ArenaFolderPickerLocal::GState.bOpen;
}
