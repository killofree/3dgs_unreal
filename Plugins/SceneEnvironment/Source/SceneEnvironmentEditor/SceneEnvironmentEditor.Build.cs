using UnrealBuildTool;
public class SceneEnvironmentEditor : ModuleRules
{
    public SceneEnvironmentEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UnrealEd" });
    }
}
