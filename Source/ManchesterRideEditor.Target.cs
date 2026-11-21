using UnrealBuildTool;

public class ManchesterRideEditorTarget : TargetRules
{
	public ManchesterRideEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ManchesterRide");
	}
}
