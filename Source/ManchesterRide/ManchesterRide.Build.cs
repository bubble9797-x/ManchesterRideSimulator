using UnrealBuildTool;

public class ManchesterRide : ModuleRules
{
	public ManchesterRide(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicIncludePaths.AddRange(new string[] { ModuleDirectory, System.IO.Path.Combine(ModuleDirectory, "Sim") });

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",
			"InputCore",
			"EnhancedInput",
			"PhysicsCore",
			"NetCore",
			"Sockets",
			"Networking",
			"AudioMixer",
			"SignalProcessing",
			"ApplicationCore",
			"ProceduralMeshComponent",
			"Json",
			"Slate",
			"SlateCore",
			"Voice",
		});

		// Dedicated server link (Net/MRServerLink.cpp): Winsock, Windows CNG crypto, DPAPI for the saved login.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[] { "Ws2_32.lib", "Bcrypt.lib", "Crypt32.lib" });
		}

		// Optional photoreal layer: only when Cesium for Unreal is installed in Plugins/CesiumForUnreal.
		// Cesium ships prebuilt for the Editor only and does not compile into a packaged (Shipping) game,
		// so it is compiled in for Editor targets alone; the Game target disables the plugin (see ManchesterRide.Target.cs).
		bool bCesium = Target.Type == TargetType.Editor
			&& System.IO.Directory.Exists(System.IO.Path.Combine(ModuleDirectory, "..", "..", "Plugins", "CesiumForUnreal"));
		if (bCesium) PrivateDependencyModuleNames.Add("CesiumRuntime");
		PublicDefinitions.Add("WITH_MR_CESIUM=" + (bCesium ? "1" : "0"));
	}
}
