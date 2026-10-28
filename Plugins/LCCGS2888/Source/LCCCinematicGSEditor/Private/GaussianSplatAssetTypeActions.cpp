// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetTypeActions.h"
#include "GaussianSplatAsset.h"
#include "EditorReimportHandler.h"
#include "ToolMenuSection.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/Paths.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "FileHelpers.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions_GaussianSplatAsset"

FText FAssetTypeActions_GaussianSplatAsset::GetName() const
{
	return LOCTEXT("AssetName", "Gaussian Splat Asset");
}

FColor FAssetTypeActions_GaussianSplatAsset::GetTypeColor() const
{
	// Teal color to distinguish from other assets
	return FColor(64, 200, 180);
}

UClass* FAssetTypeActions_GaussianSplatAsset::GetSupportedClass() const
{
	return UGaussianSplatAsset::StaticClass();
}

uint32 FAssetTypeActions_GaussianSplatAsset::GetCategories()
{
	return EAssetTypeCategories::Misc;
}

void FAssetTypeActions_GaussianSplatAsset::GetActions(const TArray<UObject*>& InObjects, FToolMenuSection& Section)
{
	TArray<TWeakObjectPtr<UGaussianSplatAsset>> GaussianSplatAssets;
	for (UObject* Object : InObjects)
	{
		if (UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Object))
		{
			GaussianSplatAssets.Add(Asset);
		}
	}

	// GAUSSIAN SPLAT ACTIONS section
	Section.AddMenuEntry(
		"GaussianSplatAsset_Reimport",
		LOCTEXT("ReimportLabel", "Reimport"),
		LOCTEXT("ReimportTooltip", "Reimport the Gaussian Splat asset from the source PLY file"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteReimport, GaussianSplatAssets),
			FCanExecuteAction()
		)
	);

	Section.AddMenuEntry(
		"GaussianSplatAsset_ShowInfo",
		LOCTEXT("ShowInfoLabel", "Show Info"),
		LOCTEXT("ShowInfoTooltip", "Display information about the Gaussian Splat asset"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteShowInfo, GaussianSplatAssets),
			FCanExecuteAction()
		)
	);

    Section.AddMenuEntry(
        "GaussianSplatAsset_BakeVRClusterCopy",
        LOCTEXT("BakeVRClustersLabel", "Bake VR Clusters (Full Detail Copy)"),
        LOCTEXT("BakeVRClustersTip", "Create a separate clustered asset from imported data. Preserves original packed splats, adds no simplified LOD splats, and does not require the source PLY. Existing level actors keep their current asset."),
        FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this,
            &FAssetTypeActions_GaussianSplatAsset::ExecuteBakeVRClusterCopy, GaussianSplatAssets)));

	// Nanite submenu (similar to UE's native Nanite menu for Static Meshes)
	Section.AddSubMenu(
		"GaussianSplatAsset_Nanite",
		LOCTEXT("NaniteSubMenuLabel", "Nanite"),
		LOCTEXT("NaniteSubMenuTooltip", "Nanite LOD and culling options"),
		FNewMenuDelegate::CreateLambda([this, GaussianSplatAssets](FMenuBuilder& SubMenuBuilder)
		{
			// Determine current state
			bool bAllEnabled = AreAllNaniteEnabled(GaussianSplatAssets);
			bool bAllDisabled = AreAllNaniteDisabled(GaussianSplatAssets);

			// Nanite checkbox - checked if enabled
			SubMenuBuilder.AddMenuEntry(
				LOCTEXT("NaniteEnabledLabel", "Nanite"),
				LOCTEXT("NaniteEnabledTooltip", "Toggle Nanite support on the selected Gaussian Splat assets"),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([this, GaussianSplatAssets, bAllEnabled]()
					{
						if (bAllEnabled)
						{
							// All enabled -> disable all
							ExecuteDisableNanite(GaussianSplatAssets);
						}
						else
						{
							// Mixed or all disabled -> enable all
							ExecuteEnableNanite(GaussianSplatAssets);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([bAllEnabled]() { return bAllEnabled; })
				),
				NAME_None,
				EUserInterfaceActionType::ToggleButton
			);

			SubMenuBuilder.AddSeparator();

			// Enable Nanite action
			SubMenuBuilder.AddMenuEntry(
				FText::Format(LOCTEXT("EnableNaniteLabel", "Enable Nanite ({0} Assets)"), FText::AsNumber(GaussianSplatAssets.Num())),
				LOCTEXT("EnableNaniteTooltip", "Build Nanite cluster hierarchy for LOD and culling optimization"),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteEnableNanite, GaussianSplatAssets),
					FCanExecuteAction::CreateLambda([bAllEnabled]() { return !bAllEnabled; })
				)
			);

			// Disable Nanite action
			SubMenuBuilder.AddMenuEntry(
				FText::Format(LOCTEXT("DisableNaniteLabel", "Disable Nanite ({0} Assets)"), FText::AsNumber(GaussianSplatAssets.Num())),
				LOCTEXT("DisableNaniteTooltip", "Remove Nanite cluster hierarchy to reduce asset size"),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteDisableNanite, GaussianSplatAssets),
					FCanExecuteAction::CreateLambda([bAllDisabled]() { return !bAllDisabled; })
				)
			);
		}),
		false,
		FSlateIcon()
	);
}

void FAssetTypeActions_GaussianSplatAsset::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	// For now, just open the default property editor
	// In the future, we could create a custom editor with 3D preview
	FAssetTypeActions_Base::OpenAssetEditor(InObjects, EditWithinLevelEditor);
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteReimport(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			FReimportManager::Instance()->Reimport(Asset, /*bAskForNewFileIfMissing=*/true);
		}
	}
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteShowInfo(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			FString NaniteStatus = Asset->IsNaniteEnabled() ?
				FString::Printf(TEXT("Enabled (%d clusters, %d LOD levels)"), Asset->GetClusterCount(), Asset->GetNumLODLevels()) :
				TEXT("Disabled");

			FString InfoMessage = FString::Printf(
				TEXT("Gaussian Splat Asset Info:\n\n")
				TEXT("Name: %s\n")
				TEXT("Splat Count: %d\n")
				TEXT("Original Splat Count: %d\n")
				TEXT("Memory Usage: %.2f MB\n")
				TEXT("Bounds: %s\n")
				TEXT("Source File: %s\n")
				TEXT("Quality: %s\n")
				TEXT("Nanite: %s"),
				*Asset->GetName(),
				Asset->GetSplatCount(),
				Asset->GetOriginalSplatCount(),
				Asset->GetMemoryUsage() / (1024.0 * 1024.0),
				*Asset->GetBounds().ToString(),
				*Asset->SourceFilePath,
				*UEnum::GetValueAsString(Asset->ImportQuality),
				*NaniteStatus
			);

			UE_LOG(LogTemp, Log, TEXT("%s"), *InfoMessage);

			// Show message box
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(InfoMessage));
		}
	}
}


UGaussianSplatAsset* FAssetTypeActions_GaussianSplatAsset::CreateVRClusterCopy(UGaussianSplatAsset* Source)
{
    if (!Source) return nullptr;
    UGaussianSplatAsset* Work=DuplicateObject<UGaussianSplatAsset>(Source,GetTransientPackage());
    if (!Work || !Work->BakeVRClusters())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            LOCTEXT("BakeVRClustersFailed", "VR cluster bake failed or was cancelled for {0}. The original asset is unchanged. See Output Log."),
            FText::FromString(Source->GetName())));
        return nullptr;
    }
    IAssetTools& Tools=FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
    FString PackageName,AssetName;
    Tools.CreateUniqueAssetName(Source->GetOutermost()->GetName(),TEXT("_VRClusters"),PackageName,AssetName);
    auto* Baked=Cast<UGaussianSplatAsset>(Tools.DuplicateAsset(AssetName,FPackageName::GetLongPackagePath(PackageName),Work));
    if (!Baked) return nullptr;
    TArray<UPackage*> Packages{Baked->GetOutermost()};
    if (!UEditorLoadingAndSavingUtils::SavePackages(Packages,true))
    {
        FMessageDialog::Open(EAppMsgType::Ok,LOCTEXT("BakeVRSaveFailed", "The baked copy could not be saved. The actor keeps its current asset. Check the Output Log."));
        return nullptr;
    }
    return Baked;
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteBakeVRClusterCopy(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
    TArray<UObject*> Created;
    for (const auto& Weak : Objects)
        if (auto* Baked=CreateVRClusterCopy(Weak.Get())) Created.Add(Baked);
    if (Created.Num() && GEditor) GEditor->SyncBrowserToObjects(Created);
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteEnableNanite(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (!Asset->IsNaniteEnabled())
			{
				// Check if source file exists
				if (Asset->SourceFilePath.IsEmpty() || !FPaths::FileExists(Asset->SourceFilePath))
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
						LOCTEXT("SourceFileNotFound", "Cannot enable Nanite for {0}:\nSource PLY file not found: {1}\n\nPlease reimport the asset first."),
						FText::FromString(Asset->GetName()),
						FText::FromString(Asset->SourceFilePath)
					));
					continue;
				}

				UE_LOG(LogTemp, Log, TEXT("Enabling Nanite for asset: %s"), *Asset->GetName());

				if (Asset->BuildNaniteClusterHierarchy())
				{
					UE_LOG(LogTemp, Log, TEXT("Successfully enabled Nanite for asset: %s (%d clusters)"),
						*Asset->GetName(), Asset->GetClusterCount());
				}
				else
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
						LOCTEXT("NaniteBuildFailed", "Failed to enable Nanite for {0}.\nSee Output Log for details."),
						FText::FromString(Asset->GetName())
					));
				}
			}
		}
	}
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteDisableNanite(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (Asset->IsNaniteEnabled())
			{
				UE_LOG(LogTemp, Log, TEXT("Disabling Nanite for asset: %s"), *Asset->GetName());
				Asset->ClearNaniteClusterHierarchy();
				UE_LOG(LogTemp, Log, TEXT("Successfully disabled Nanite for asset: %s"), *Asset->GetName());
			}
		}
	}
}

bool FAssetTypeActions_GaussianSplatAsset::AreAllNaniteEnabled(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects) const
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (!Asset->IsNaniteEnabled())
			{
				return false;
			}
		}
	}
	return Objects.Num() > 0;
}

bool FAssetTypeActions_GaussianSplatAsset::AreAllNaniteDisabled(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects) const
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (Asset->IsNaniteEnabled())
			{
				return false;
			}
		}
	}
	return Objects.Num() > 0;
}

#undef LOCTEXT_NAMESPACE
