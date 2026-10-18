using UnrealBuildTool;
using System.Collections.Generic;

public class OpenUT2k4Target : TargetRules
{
	public OpenUT2k4Target(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("OpenUT2k4");
	}
}
