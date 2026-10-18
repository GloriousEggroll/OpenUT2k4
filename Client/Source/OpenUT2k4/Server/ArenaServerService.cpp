#include "ArenaDedicatedServer.h"
#include "../OpenUT2k4.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#include <atomic>

/**
 * The server as a native Windows service (Server/Docs/DedicatedServer.md "Windows service"). Installed by
 * Server/Package/windows/install-service.ps1 with the binary path and "-Service=<name>" in its command line.
 *
 * The engine owns the main thread, so the service control dispatcher runs on a helper thread (StartServiceCtrlDispatcher
 * may be called from any thread; it must happen within 30 s of the start, long before the first map). ServiceMain
 * reports RUNNING at once; STOP, SHUTDOWN and PRESHUTDOWN request a normal engine exit (the same path as Ctrl+C:
 * the UE2 server tells its clients, the log is flushed) and report STOP_PENDING with a wait hint; the module's
 * shutdown reports STOPPED with the exit code, so a failure exit (non-zero) triggers the service's recovery actions.
 */
namespace ArenaServerServiceLocal
{
	FString ServiceName = TEXT("OpenUT2k4Server");
	SERVICE_STATUS_HANDLE StatusHandle = nullptr;
	SERVICE_STATUS SvcStatus = {};
	HANDLE StoppedEvent = nullptr;
	HANDLE DispatcherThread = nullptr;
	std::atomic<bool> bService{ false };
	std::atomic<DWORD> CheckPoint{ 1 };
	FCriticalSection StatusLock;

	void SetState(DWORD State, DWORD ExitCode = 0, DWORD WaitHintMs = 0)
	{
		FScopeLock L(&StatusLock);
		if (!StatusHandle)
		{
			return;
		}
		SvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		SvcStatus.dwCurrentState = State;
		SvcStatus.dwControlsAccepted = State == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN) : 0;
		SvcStatus.dwWin32ExitCode = ExitCode ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
		SvcStatus.dwServiceSpecificExitCode = ExitCode;
		SvcStatus.dwWaitHint = WaitHintMs;
		SvcStatus.dwCheckPoint = (State == SERVICE_RUNNING || State == SERVICE_STOPPED) ? 0 : CheckPoint++;
		SetServiceStatus(StatusHandle, &SvcStatus);
	}

	DWORD WINAPI HandlerEx(DWORD Control, DWORD, LPVOID, LPVOID)
	{
		switch (Control)
		{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:
		case SERVICE_CONTROL_PRESHUTDOWN:
			SetState(SERVICE_STOP_PENDING, 0, 30000);
			// read by the game thread's loop (IsEngineExitRequested); the same flag the console's Ctrl+C handler sets
			RequestEngineExit(TEXT("Windows service stop"));
			return NO_ERROR;
		case SERVICE_CONTROL_INTERROGATE:
			return NO_ERROR;
		default:
			return ERROR_CALL_NOT_IMPLEMENTED;
		}
	}

	void WINAPI ServiceMain(DWORD, LPWSTR*)
	{
		StatusHandle = RegisterServiceCtrlHandlerExW(*ServiceName, &HandlerEx, nullptr);
		if (!StatusHandle)
		{
			return;
		}
		bService = true;
		SetState(SERVICE_RUNNING);
		WaitForSingleObject(StoppedEvent, INFINITE);
	}

	DWORD WINAPI DispatcherMain(LPVOID)
	{
		SERVICE_TABLE_ENTRYW Table[] = {
			{ const_cast<LPWSTR>(*ServiceName), &ServiceMain },
			{ nullptr, nullptr },
		};
		if (!StartServiceCtrlDispatcherW(Table))
		{
			const DWORD Err = GetLastError();
			// ERROR_FAILED_SERVICE_CONTROLLER_CONNECT: started from a console with -Service, not by the SCM
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Server: -Service given but the service control manager did not start this process (error %u); running normally"), Err);
		}
		return 0;
	}
}

void ArenaServerService::Start()
{
	using namespace ArenaServerServiceLocal;
	FString Name;
	if (!FParse::Param(FCommandLine::Get(), TEXT("Service")) && !FParse::Value(FCommandLine::Get(), TEXT("Service="), Name))
	{
		return;
	}
	if (!Name.IsEmpty())
	{
		ServiceName = Name;
	}
	StoppedEvent = CreateEventW(nullptr, true, false, nullptr);
	DispatcherThread = CreateThread(nullptr, 0, &DispatcherMain, nullptr, 0, nullptr);
	UE_LOG(LogOpenUT2k4, Log, TEXT("Server: Windows service %s"), *ServiceName);
}

void ArenaServerService::Stop(int32 ExitCode)
{
	using namespace ArenaServerServiceLocal;
	if (!DispatcherThread)
	{
		return;
	}
	if (bService)
	{
		SetState(SERVICE_STOPPED, DWORD(ExitCode));
	}
	SetEvent(StoppedEvent);
	// the dispatcher returns once the SCM has the STOPPED state
	WaitForSingleObject(DispatcherThread, 5000);
	CloseHandle(DispatcherThread);
	DispatcherThread = nullptr;
}

bool ArenaServerService::IsRunningAsService()
{
	FString Name;
	return FParse::Param(FCommandLine::Get(), TEXT("Service")) || FParse::Value(FCommandLine::Get(), TEXT("Service="), Name);
}

#else

void ArenaServerService::Start()
{
}

void ArenaServerService::Stop(int32)
{
}

bool ArenaServerService::IsRunningAsService()
{
	// systemd / launchd: stdin is /dev/null, the stdin reader ends by itself
	return false;
}

#endif
