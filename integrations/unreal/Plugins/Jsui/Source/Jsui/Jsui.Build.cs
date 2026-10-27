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
		PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "RHI", "RenderCore", "HTTP" });

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

			// The GPU paths: Skia Ganesh on the D3D12 RHI's device and queue
			// (ID3D12DynamicRHI::RHIRunOnQueue), Ganesh on a D3D12 device next
			// to the D3D11 RHI's (ID3D11DynamicRHI::RHIGetDevice), Skia
			// Graphite on the Vulkan RHI's (-vulkan; IVulkanDynamicRHI). Only
			// the RHIs' interface headers are used (virtual calls).
			PrivateDependencyModuleNames.AddRange(new string[] { "D3D11RHI", "D3D12RHI", "VulkanRHI" });
			AddEngineThirdPartyPrivateStaticDependencies(Target, "DX11", "DX12", "Vulkan");
			PrivateDefinitions.AddRange(new string[] { "JSUI_UE_D3D11=1", "JSUI_UE_D3D12=1", "JSUI_UE_VULKAN=1" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			string So = Path.Combine(LibraryDir, "lib", "libjsui.so");
			PublicAdditionalLibraries.Add(So);
			RuntimeDependencies.Add("$(BinaryOutputDir)/libjsui.so", So);

			// The GPU path: Skia Graphite on the Vulkan RHI's device and
			// graphics queue (IVulkanDynamicRHI::RHIRunOnQueue).
			PrivateDependencyModuleNames.Add("VulkanRHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "Vulkan");
			PrivateDefinitions.Add("JSUI_UE_VULKAN=1");
		}

		if (Target.Platform != UnrealTargetPlatform.Mac)
		{
			PrivateDefinitions.Add("JSUI_UE_METAL=0");
		}
		if (Target.Platform != UnrealTargetPlatform.Win64)
		{
			PrivateDefinitions.AddRange(new string[] { "JSUI_UE_D3D11=0", "JSUI_UE_D3D12=0" });
		}
		if (Target.Platform != UnrealTargetPlatform.Win64 && Target.Platform != UnrealTargetPlatform.Linux)
		{
			PrivateDefinitions.Add("JSUI_UE_VULKAN=0");
		}
	}
}
