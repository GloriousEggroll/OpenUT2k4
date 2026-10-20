using UnrealBuildTool;

public class OpenUT2k4 : ModuleRules
{
	public OpenUT2k4(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Files use same-named helpers in their own anonymous / local namespaces (Tube, Le32, C...), which collide when
		// unity builds merge them. Each file is compiled on its own.
		bUseUnity = false;

		// The dedicated server binary as a real Server target (Source/OpenUT2k4Server.Target.cs on a source engine) leaves
		// out what only a renderer uses: the upscaler plugin and the Vulkan HDR probe. As a Game target (installed engine)
		// it shares this module's build products with the game (one shared build environment), so the module is compiled
		// the same way and the server mode is chosen at run time from the executable's name (Server/ArenaDedicatedServer).
		bool bServerBinary = Target.Type == TargetType.Server;
		PrivateDefinitions.Add("OPENUT2K4_SERVER_BINARY=" + (bServerBinary ? "1" : "0"));

		// Shipping: the installed engine is precompiled without logging (NO_LOGGING), so UE_LOG writes nothing. This module's
		// UE_LOG / UE_CLOG go to our own log file instead (ArenaShippingLog.h, <Saved>/Logs/OpenUT2k4.log) so players can send one.
		if (Target.Configuration == UnrealTargetConfiguration.Shipping)
		{
			ForceIncludeFiles.Add(System.IO.Path.Combine(ModuleDirectory, "ArenaShippingLog.h"));
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "AIModule", "Slate", "SlateCore", "Json"
		});

		// Runtime content: PNG decoding (ArenaRuntimeMap terrain heightmaps, runtime textures); PhysicsCore for
		// flushing the physics scene after building a map at runtime (FChaosScene::Flush).
		PrivateDependencyModuleNames.AddRange(new string[] { "ImageWrapper", "PhysicsCore" });
		// Runtime skeletal meshes fill FSkeletalMeshLODRenderData (vertex buffers) directly (ArenaRuntimeSkeletal).
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI" });
		// Runtime content provider (ArenaRuntimeContent): static meshes from mesh descriptions, ogg music.
		PrivateDependencyModuleNames.AddRange(new string[] { "MeshDescription", "StaticMeshDescription", "VorbisAudioDecoder", "AudioExtensions" });
		// ArenaAudioOutput: stereo fold and peak limiter on the main submix (audio mixer device, submix effect buffers)
		PrivateDependencyModuleNames.AddRange(new string[] { "AudioMixer", "AudioMixerCore", "SignalProcessing" });
		// FSoundQualityInfo (a plain struct used by the engine's runtime decoders) lives in a TargetPlatform header: include path only.
		PrivateIncludePathModuleNames.Add("TargetPlatform");

		// ArenaContentDownload / Download/: downloading the UT2004 disc image from OldUnreal (engine HTTP module)
		PrivateDependencyModuleNames.Add("HTTP");

		// Net/: the UT2004 server browser (master server TCP, UDP server queries) and the UE2 client use engine sockets
		if (!PrivateDependencyModuleNames.Contains("Sockets"))
		{
			PrivateDependencyModuleNames.Add("Sockets");
		}

		// Upscaler / low latency settings (Plugins/ArenaUpscaling: FSR 3.1, VK_AMD_anti_lag)
		if (!bServerBinary)
		{
			PrivateDependencyModuleNames.Add("ArenaUpscaling");
		}
		PrivateDefinitions.Add("ARENA_WITH_UPSCALING=" + (bServerBinary ? "0" : "1"));

		// Server/ArenaDedicatedServer: the dedicated server's game instance class (UGameMapsSettings)
		PrivateDependencyModuleNames.Add("EngineSettings");

		// Convert/: the in-game UT2004 content converter writes PNGs with zlib (also used by Download/ArenaDlCab for InstallShield cabinets)
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

		// ArenaContentSetup: clipboard paste in the path field (FPlatformApplicationMisc). ArenaFolderPicker: native folder dialogs.
		PrivateDependencyModuleNames.Add("ApplicationCore");
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[] { "ole32.lib", "shell32.lib" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicFrameworks.Add("AppKit");
		}

		// ArenaHDR: probes the game window's Vulkan surface for HDR10 colour spaces (Vulkan RHI on Linux and Windows; Mac uses Metal).
		bool bVulkan = (Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.Win64) && !bServerBinary;
		if (bVulkan)
		{
			PrivateDependencyModuleNames.Add("VulkanRHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "Vulkan");
		}
		PrivateDefinitions.Add("ARENA_WITH_VULKAN=" + (bVulkan ? "1" : "0"));

		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			// ArenaDisplay (fullscreen monitor choice) and ArenaFolderPicker (SDL_ShowFileDialogWithProperties) call SDL3. Headers only: the engine's ApplicationCore
			// already links SDL3 and exports it, so linking the static library here would duplicate SDL's state.
			PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/ThirdParty/SDL3/SDL-gui-backend/include"));
			PrivateDefinitions.Add("SDL_WITH_EPIC_EXTENSIONS=1");
		}
	}
}
