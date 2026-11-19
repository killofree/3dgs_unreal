#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "PropertyEditorModule.h"
#include "IDetailCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "CherrySeasonTreeActor.h"
#include "SceneSeasonController.h"

class FSeasonControlsDetails : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance() { return MakeShared<FSeasonControlsDetails>(); }
    virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override
    {
        auto& Category=Builder.EditCategory(TEXT("Unified Controls"),FText::FromString(TEXT("나무 · 태양 통합 조절 / Unified Controls")),ECategoryPriority::Important);
        Category.SetSortOrder(-1000);
        Category.AddProperty(GET_MEMBER_NAME_CHECKED(ASceneSeasonController,FlowerSize));
        Category.AddProperty(GET_MEMBER_NAME_CHECKED(ASceneSeasonController,LeafSize));
        Category.AddProperty(GET_MEMBER_NAME_CHECKED(ASceneSeasonController,SunSharpness));
    }
};

class FCherryTreeDetails : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance() { return MakeShared<FCherryTreeDetails>(); }
    virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override
    {
        auto& Category=Builder.EditCategory(TEXT("Tree Growth"), FText::FromString(TEXT("벚꽃나무 조절 / Cherry Tree Controls")), ECategoryPriority::Important);
        Category.SetSortOrder(-1000);
        Category.AddProperty(GET_MEMBER_NAME_CHECKED(ACherrySeasonTreeActor,FlowerSize));
        Category.AddProperty(GET_MEMBER_NAME_CHECKED(ACherrySeasonTreeActor,LeafSize));
    }
};

static TAutoConsoleVariable<int32> ScenePreviewStats(TEXT("scene.PreviewStats"), 0,
    TEXT("Enable FPS and Unit HUD in PIE/VR preview, independent of scene content."));

class FSceneEnvironmentEditorModule : public IModuleInterface
{
    FDelegateHandle PreviewHandle;
    void EnablePreviewStats(bool)
    {
        if (!GEngine || ScenePreviewStats.GetValueOnGameThread() == 0) return;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            UGameViewportClient* Viewport = Context.GameViewport;
            if (Context.WorldType != EWorldType::PIE || !World || !Viewport) continue;
            GEngine->SetEngineStat(World, Viewport, TEXT("FPS"), true);
            GEngine->SetEngineStat(World, Viewport, TEXT("Unit"), true);
        }
    }
public:
    virtual void StartupModule() override
    {
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor").RegisterCustomClassLayout(
            TEXT("SceneSeasonController"), FOnGetDetailCustomizationInstance::CreateStatic(&FSeasonControlsDetails::MakeInstance));
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor").RegisterCustomClassLayout(
            TEXT("CherrySeasonTreeActor"), FOnGetDetailCustomizationInstance::CreateStatic(&FCherryTreeDetails::MakeInstance));
        PreviewHandle = FEditorDelegates::PostPIEStarted.AddRaw(this, &FSceneEnvironmentEditorModule::EnablePreviewStats);
    }
    virtual void ShutdownModule() override
    {
        if (auto* Properties=FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
        {
            Properties->UnregisterCustomClassLayout(TEXT("CherrySeasonTreeActor"));
            Properties->UnregisterCustomClassLayout(TEXT("SceneSeasonController"));
        }
        FEditorDelegates::PostPIEStarted.Remove(PreviewHandle);
    }
};
IMPLEMENT_MODULE(FSceneEnvironmentEditorModule, SceneEnvironmentEditor)
