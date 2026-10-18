using UnrealBuildTool;
using System;
using System.Collections.Generic;

/**
 * The OpenUT2k4 dedicated server binary (Server/Docs/DedicatedServer.md). The module is the game's own (OpenUT2k4.Build.cs:
 * OPENUT2K4_SERVER_BINARY=1, no upscaler plugin, no Vulkan HDR probe); the process is always a headless dedicated
 * server (Source/OpenUT2k4/Server/ArenaDedicatedServer.h).
 *
 * Installed (binary) engine: a Game target. Installed builds ship no precompiled server engine and cannot build
 * Type=Server targets, so the server is the game engine started headless (-nullrhi -nosound, no viewport, no local
 * player). Built with the shared build environment, like the game.
 *
 * Source engine: a real Server target (UE_SERVER=1: no renderer, audio or Slate rendering compiled in, a smaller binary)
 * with logging in Shipping builds.
 *
 * OPENUT2K4_SERVER_TARGET_TYPE=Game or =Server in the environment overrides the choice.
 */
public class OpenUT2k4ServerTarget : TargetRules
{
	public OpenUT2k4ServerTarget(TargetInfo Target) : base(Target)
	{
		string Forced = Environment.GetEnvironmentVariable("OPENUT2K4_SERVER_TARGET_TYPE") ?? "";
		bool bServerType = Forced.Equals("Server", StringComparison.OrdinalIgnoreCase)
			|| (!Forced.Equals("Game", StringComparison.OrdinalIgnoreCase) && !bIsEngineInstalled);

		Type = bServerType ? TargetType.Server : TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("OpenUT2k4");

		if (bServerType)
		{
			// headless: the upscaler plugin only serves the renderer (the module does not use it in server binaries;
			// a shared build environment, the Game target of an installed engine, cannot leave plugins out)
			DisablePlugins.Add("ArenaUpscaling");
			// a server needs its log in every configuration (needs the unique build environment of a source engine)
			BuildEnvironment = TargetBuildEnvironment.Unique;
			bUseLoggingInShipping = true;
			bWithServerCode = true;
		}
	}
}
