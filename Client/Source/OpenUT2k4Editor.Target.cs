using UnrealBuildTool;
using System.Collections.Generic;

public class OpenUT2k4EditorTarget : TargetRules
{
	public OpenUT2k4EditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("OpenUT2k4");
	}
}
