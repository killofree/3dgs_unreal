// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LCCCinematicGS : ModuleRules
{
	public LCCCinematicGS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"));
		PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"));

		PublicIncludePaths.AddRange(
			new string[] {
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"RHI",
				"Renderer",
				"Projects",
				"AssetRegistry",
				"Niagara"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
                "HeadMountedDisplay",
                "InputCore"
			}
		);

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
