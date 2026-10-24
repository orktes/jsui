using UnrealBuildTool;

public class JsuiExampleTarget : TargetRules
{
	public JsuiExampleTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("JsuiExample");
	}
}
