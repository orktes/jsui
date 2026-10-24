using UnrealBuildTool;

public class JsuiExample : ModuleRules
{
	public JsuiExample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore", "UMG", "RHI", "RenderCore", "Jsui" });
	}
}
