using UnrealBuildTool;

// Dedicated server target. Building it requires a source build of Unreal Engine
// (the Epic Games Launcher binary engine cannot build Server targets).
// Listen-server hosting works with the normal Game target and the launcher engine.
public class ManchesterRideServerTarget : TargetRules
{
	public ManchesterRideServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ManchesterRide");
	}
}
