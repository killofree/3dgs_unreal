using UnrealBuildTool;
public class SceneTransitionTest : ModuleRules
{
    public SceneTransitionTest(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });
        PrivateDependencyModuleNames.AddRange(new[] { "InputCore", "XRBase", "MoviePlayer", "Slate", "SlateCore", "SceneEnvironment" });
        if (Target.bBuildEditor) PrivateDependencyModuleNames.Add("UnrealEd");
    }
}
