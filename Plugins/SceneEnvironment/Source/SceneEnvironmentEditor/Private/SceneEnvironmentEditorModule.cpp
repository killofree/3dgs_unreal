#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"

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
        PreviewHandle = FEditorDelegates::PostPIEStarted.AddRaw(this, &FSceneEnvironmentEditorModule::EnablePreviewStats);
    }
    virtual void ShutdownModule() override
    {
        FEditorDelegates::PostPIEStarted.Remove(PreviewHandle);
    }
};
IMPLEMENT_MODULE(FSceneEnvironmentEditorModule, SceneEnvironmentEditor)
