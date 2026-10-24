// The JSUI plugin's runtime module. It calls only the C API of a prebuilt
// JSUI shared library (Source/ThirdParty/JsuiLibrary, see
// integrations/unreal/scripts/stage-lib.sh), so the plugin compiles against
// any UE 5.x and only the C library is prebuilt per platform:
//
//   ThirdParty/JsuiLibrary/include/jsui/jsui.h
//   ThirdParty/JsuiLibrary/lib/libjsui.dylib      macOS (universal)
//   ThirdParty/JsuiLibrary/lib/libjsui.so         Linux
//   ThirdParty/JsuiLibrary/lib/jsui.lib           Windows import library
//   ThirdParty/JsuiLibrary/bin/jsui.dll           Windows (+ bin/icudtl.dat)
//
// The library is staged next to the game binary ($(BinaryOutputDir)).
using System.IO;
using UnrealBuildTool;

public class Jsui : ModuleRules
{
	public Jsui(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore", "UMG" });
		PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "RHI", "RenderCore" });

		string LibraryDir = Path.Combine(PluginDirectory, "Source", "ThirdParty", "JsuiLibrary");
		PublicSystemIncludePaths.Add(Path.Combine(LibraryDir, "include"));

		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			string Dylib = Path.Combine(LibraryDir, "lib", "libjsui.dylib");
			PublicAdditionalLibraries.Add(Dylib);
			RuntimeDependencies.Add("$(BinaryOutputDir)/libjsui.dylib", Dylib);

			// The GPU path: Skia Graphite on the Metal RHI's own device and
			// queue (IMetalDynamicRHI::RHIRunOnQueue). Only virtual calls into
			// the RHI, so nothing of MetalRHI is linked.
			PrivateIncludePathModuleNames.Add("MetalRHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "MetalCPP");
			PublicWeakFrameworks.Add("Metal");
			PrivateDefinitions.Add("JSUI_UE_METAL=1");
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDefinitions.Add("JSUI_SHARED=1");  // jsui.h: __declspec(dllimport)
			PublicAdditionalLibraries.Add(Path.Combine(LibraryDir, "lib", "jsui.lib"));
			PublicDelayLoadDLLs.Add("jsui.dll");
			RuntimeDependencies.Add("$(BinaryOutputDir)/jsui.dll", Path.Combine(LibraryDir, "bin", "jsui.dll"));
			// Skia's ICU reads its data from next to the executable.
			RuntimeDependencies.Add("$(BinaryOutputDir)/icudtl.dat", Path.Combine(LibraryDir, "bin", "icudtl.dat"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			string So = Path.Combine(LibraryDir, "lib", "libjsui.so");
			PublicAdditionalLibraries.Add(So);
			RuntimeDependencies.Add("$(BinaryOutputDir)/libjsui.so", So);
		}

		if (Target.Platform != UnrealTargetPlatform.Mac)
		{
			PrivateDefinitions.Add("JSUI_UE_METAL=0");
		}
	}
}
