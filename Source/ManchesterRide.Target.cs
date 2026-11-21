using UnrealBuildTool;

public class ManchesterRideTarget : TargetRules
{
	public ManchesterRideTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ManchesterRide");
		// Cesium (the optional photoreal map) is restricted to Editor targets in ManchesterRide.uproject
		// (TargetAllowList), because it is prebuilt for the Editor only and will not compile into a
		// packaged game. The procedural world, riding, map, routing and multiplayer do not depend on it.
	}
}
