using UnrealBuildTool;

public class ArenaUpscaling : ModuleRules
{
	public ArenaUpscaling(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
		// Renderer: ITemporalUpscaler / ScreenPass (public headers). Projects: shader directory of this plugin.
		PrivateDependencyModuleNames.AddRange(new string[] { "CoreUObject", "Engine", "RenderCore", "Renderer", "RHI", "Projects" });

		// Low latency (VK_AMD_anti_lag) needs the Vulkan RHI, which exists on Linux and Windows only (Mac uses Metal).
		bool bVulkan = Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.Win64;
		if (bVulkan)
		{
			PrivateDependencyModuleNames.Add("VulkanRHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "Vulkan");
		}
		PrivateDefinitions.Add("ARENA_UPSCALING_WITH_VULKAN=" + (bVulkan ? "1" : "0"));
	}
}
