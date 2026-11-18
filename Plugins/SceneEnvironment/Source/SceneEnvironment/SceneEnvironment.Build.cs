using UnrealBuildTool;
public class SceneEnvironment : ModuleRules
{
    public SceneEnvironment(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] { "Niagara", "LCCCinematicGS" });
        if (Target.bBuildEditor) PrivateDependencyModuleNames.Add("NiagaraEditor");
    }
}
