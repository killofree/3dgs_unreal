using UnrealBuildTool;
public class SceneTransitionTestEditor : ModuleRules
{
    public SceneTransitionTestEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "SceneTransitionTest",
            "PropertyEditor", "Slate", "SlateCore", "BlueprintGraph", "Kismet", "KismetCompiler", "AssetRegistry"
        });
    }
}
