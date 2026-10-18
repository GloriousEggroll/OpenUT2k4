#include "OpenUT2k4.h"
#include "Server/ArenaDedicatedServer.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

DEFINE_LOG_CATEGORY(LogOpenUT2k4);

namespace OpenUT2k4Local
{
#if PLATFORM_LINUX
	/**
	 * Overlay layers that run their own threads (MangoHud's logger thread with autostart_log or a manual
	 * log toggle) crash the process at exit: the Vulkan loader unloads the layer library when the RHI
	 * destroys its instance, and the still running thread wakes up in unmapped code (SIGSEGV after
	 * "Log file closed"). Marking the already loaded library RTLD_NODELETE keeps its code mapped until the
	 * process ends. Its static destructors still run at exit() and free the logger while the thread
	 * appends to it (heap corruption, abort), so once the engine has shut down completely the process
	 * ends with _exit before they run: the atexit handler is registered after the layer was loaded, so it
	 * runs before the layer's destructors. Runs before RHIExit (OnPreExit), only when such a layer is loaded.
	 */
	static const char* const ThreadedLayerLibraries[] = { "libMangoHud.so", "libMangoHud_opengl.so" };

	static int CollectLayerLibrary(dl_phdr_info* Info, size_t, void* Data)
	{
		if (Info->dlpi_name && Info->dlpi_name[0])
		{
			const char* Slash = strrchr(Info->dlpi_name, '/');
			const char* Base = Slash ? Slash + 1 : Info->dlpi_name;
			for (const char* Name : ThreadedLayerLibraries)
			{
				if (strcmp(Base, Name) == 0)
				{
					static_cast<TArray<FString>*>(Data)->Add(FString(UTF8_TO_TCHAR(Info->dlpi_name)));
				}
			}
		}
		return 0;
	}

	static bool bEngineInitComplete = false;

	static void ExitBeforeLayerDestructors()
	{
		// The engine has finished (log closed); only static destructors are left.
		uint8 Code = 0;
		FPlatformMisc::HasOverriddenReturnCode(&Code);
		fflush(nullptr);
		_exit(Code);
	}

	static void PinThreadedLayerLibraries()
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("NoOverlayPin")))
		{
			return; // diagnostics: reproduce the overlay's exit crash
		}
		TArray<FString> Paths;
		dl_iterate_phdr(&CollectLayerLibrary, &Paths);
		for (const FString& Path : Paths)
		{
			// The handle is never closed on purpose: it is the reference that keeps the library loaded.
			const bool bPinned = dlopen(TCHAR_TO_UTF8(*Path), RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != nullptr;
			UE_LOG(LogOpenUT2k4, Log, TEXT("Exit: %s overlay layer %s until process exit"), bPinned ? TEXT("keeping") : TEXT("could not keep"), *Path);
		}
		// Only after a normal run: a failed init returns its own error code through exit().
		if (Paths.Num() > 0 && bEngineInitComplete)
		{
			atexit(&ExitBeforeLayerDestructors);
		}
	}
#endif
}

class FOpenUT2k4Module : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
#if PLATFORM_LINUX
		FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([]() { OpenUT2k4Local::bEngineInitComplete = true; });
		FCoreDelegates::OnPreExit.AddStatic(&OpenUT2k4Local::PinThreadedLayerLibraries);
#endif
		// the dedicated server (OpenUT2k4Server binary or -OpenUT2k4Server): configuration, headless engine
		ArenaDedicatedServer::OnModuleStartup();
	}

	virtual void ShutdownModule() override
	{
		ArenaDedicatedServer::OnModuleShutdown();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FOpenUT2k4Module, OpenUT2k4, "OpenUT2k4");
