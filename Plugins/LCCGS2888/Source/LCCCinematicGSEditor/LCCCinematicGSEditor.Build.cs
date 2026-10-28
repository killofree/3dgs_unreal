// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LCCCinematicGSEditor : ModuleRules
{
	public LCCCinematicGSEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

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
				"LCCCinematicGS"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"InputCore",
				"UnrealEd",
				"PropertyEditor",
				"AssetTools",
				"EditorFramework",
				"Projects",
				"ToolMenus",
				"ImageCore",
				"ImageWrapper",
				"RenderCore",
				"RHI"
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
