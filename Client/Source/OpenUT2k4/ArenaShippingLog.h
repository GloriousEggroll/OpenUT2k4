#pragma once

// Shipping builds only: OpenUT2k4.Build.cs force-includes this header in every file of the module.
//
// The installed engine is precompiled with NO_LOGGING in Shipping, so UE_LOG writes nothing and a player has no log to
// send after a problem. Here this module's UE_LOG / UE_CLOG are routed to our own log file (ArenaShippingLog.cpp:
// <Saved>/Logs/OpenUT2k4.log, OpenUT2k4Server.log for the dedicated server; the previous run's is kept as *-backup.log).
// Fatal still ends the program as before; Error, Warning, Display and Log lines are written; Verbose and finer are
// compiled out as before.

#include "CoreTypes.h"
#include "Containers/UnrealString.h"
#include "Logging/LogMacros.h"

#if NO_LOGGING

namespace ArenaShippingLog
{
	void Write(const TCHAR* Category, ELogVerbosity::Type Verbosity, const FString& Message);
}

#undef UE_LOG
#define UE_LOG(CategoryName, Verbosity, Format, ...) \
	{ \
		if constexpr ((::ELogVerbosity::Verbosity & ::ELogVerbosity::VerbosityMask) == ::ELogVerbosity::Fatal) \
		{ \
			LowLevelFatalError(Format, ##__VA_ARGS__); \
			CA_ASSUME(false); \
		} \
		else if constexpr ((::ELogVerbosity::Verbosity & ::ELogVerbosity::VerbosityMask) <= ::ELogVerbosity::Log) \
		{ \
			::ArenaShippingLog::Write(TEXT(#CategoryName), ::ELogVerbosity::Verbosity, FString::Printf(Format, ##__VA_ARGS__)); \
		} \
	}

#undef UE_CLOG
#define UE_CLOG(Condition, CategoryName, Verbosity, Format, ...) \
	{ \
		if (Condition) \
		{ \
			UE_LOG(CategoryName, Verbosity, Format, ##__VA_ARGS__); \
		} \
	}

#endif // NO_LOGGING
