using UnrealBuildTool;

public class VR360Skybox : ModuleRules
{
    public VR360Skybox(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "MediaAssets" });
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "MaterialEditor", "AssetRegistry" });
        }
    }
}
