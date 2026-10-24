using UnrealBuildTool;

public class JsuiExampleEditorTarget : TargetRules
{
	public JsuiExampleEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("JsuiExample");
	}
}
