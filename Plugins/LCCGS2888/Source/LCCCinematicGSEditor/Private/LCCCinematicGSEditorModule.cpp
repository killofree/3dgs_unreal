// Copyright Epic Games, Inc. All Rights Reserved.

#include "LCCCinematicGSEditorModule.h"
#include "GaussianSplatAssetTypeActions.h"
#include "GaussianSplatThumbnailRenderer.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatDetails.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "PropertyEditorModule.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "GaussianSplatComponent.h"
#include "ComponentVisualizer.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "SceneManagement.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Misc/CoreDelegates.h"
#include "GSAutomaticWorldScope.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Engine/GameViewportClient.h"
#include "Editor.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "RHI.h"
#include "EngineUtils.h"
#include "GaussianSplatActor.h"
#include "GSPreviewPassCounters.h"
#include "GSPreviewRecorderSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "UObject/UObjectIterator.h"
#include "GameFramework/Pawn.h"

// Selection-scoped editor overlay: never modifies the receiver, materials, or captures.
class FGaussianProxyAlignmentVisualizer final : public FComponentVisualizer
{
public:
	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override
	{
		const UGaussianSplatComponent* Splat = Cast<UGaussianSplatComponent>(Component);
		if (!Splat || !Splat->bShowProxyAlignment || !Splat->ProxyShadowMesh || !PDI) return;
		const UStaticMesh* Mesh = Splat->ProxyShadowMesh;
		if (Mesh->IsCompiling()) return;
		const FStaticMeshRenderData* Data = Mesh->GetRenderData();
		if (!Data || Data->LODResources.IsEmpty()) return;
		const FStaticMeshLODResources& LOD = Data->LODResources[0];
		const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
		const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
		// Use the same mesh, LOD and transform as Stable binding, never cached bindings.
		const FTransform Transform = Splat->GetProxyShadowMeshWorldTransform();
		const uint8 Depth = Splat->bProxyAlignmentForeground ? SDPG_Foreground : SDPG_World;
		for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
		{
			const uint32 A = Indices[I], B = Indices[I + 1], C = Indices[I + 2];
			if (A >= Positions.GetNumVertices() || B >= Positions.GetNumVertices() || C >= Positions.GetNumVertices()) continue;
			const FVector PA = Transform.TransformPosition(FVector(Positions.VertexPosition(A)));
			const FVector PB = Transform.TransformPosition(FVector(Positions.VertexPosition(B)));
			const FVector PC = Transform.TransformPosition(FVector(Positions.VertexPosition(C)));
			PDI->DrawLine(PA, PB, Splat->ProxyAlignmentColor, Depth);
			PDI->DrawLine(PB, PC, Splat->ProxyAlignmentColor, Depth);
			PDI->DrawLine(PC, PA, Splat->ProxyAlignmentColor, Depth);
		}
	}
};

void FLCCCinematicGSEditorModule::RegisterProxyAlignmentVisualizer()
{
	if (GUnrealEd)
	{
		TSharedRef<FComponentVisualizer> Visualizer = MakeShared<FGaussianProxyAlignmentVisualizer>();
		GUnrealEd->RegisterComponentVisualizer(UGaussianSplatComponent::StaticClass()->GetFName(), Visualizer);
		Visualizer->OnRegister();
	}
}

#define LOCTEXT_NAMESPACE "FLCCCinematicGSEditorModule"

static TAutoConsoleVariable<float> GSEditorMaxFPS(TEXT("gs.PCVR.EditorMaxFPS"),30.f,
    TEXT("Editing GS worlds only: viewport frame ceiling. 0 disables. Does not cap PIE/VR."));

static TAutoConsoleVariable<int32> GSPreviewStats(TEXT("gs.PCVR.PreviewStats"),0,
    TEXT("Opt-in editor play/VR preview HUD: FPS, frame time, Game/Draw/GPU timing. Independent of GS content."));

void FLCCCinematicGSEditorModule::EnablePreviewStats(bool)
{
    if (!GEngine || GSPreviewStats.GetValueOnGameThread()==0) return;
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        UWorld* World=Context.World();
        UGameViewportClient* Viewport=Context.GameViewport;
        if (Context.WorldType!=EWorldType::PIE || !World || !Viewport || !GSAutomaticWorldScope::HasGS(World)) continue;
        // Set explicitly rather than toggling: a second preview must stay on.
        GEngine->SetEngineStat(World,Viewport,TEXT("FPS"),true);
        GEngine->SetEngineStat(World,Viewport,TEXT("Unit"),true);
        UE_LOG(LogTemp,Display,TEXT("[GSPreviewStats] FPS=%d Unit=%d World=%s BuiltInHUD=1 GSRequired=0"),
            Viewport->IsStatEnabled(TEXT("FPS")),Viewport->IsStatEnabled(TEXT("Unit")),*World->GetName());
    }
}

bool FLCCCinematicGSEditorModule::UpdateGSFrameLimit(float)
{
    if (!GEditor || IsRunningCommandlet()) return true;
    const float Limit=GSEditorMaxFPS.GetValueOnGameThread();
    const bool Active=!GEditor->PlayWorld && Limit>0 &&
        GSAutomaticWorldScope::HasGS(GEditor->GetEditorWorldContext().World());
    if (Active && !SavedFrameRange.IsSet())
    {
        SavedFrameRange=GEditor->SmoothedFrameRateRange;
        AppliedFrameRange=FFloatRange(SavedFrameRange->GetLowerBound(),
            FFloatRangeBound::Inclusive(SavedFrameRange->HasUpperBound() ?
                FMath::Min(Limit,SavedFrameRange->GetUpperBoundValue()) : Limit));
        GEditor->SmoothedFrameRateRange=AppliedFrameRange;
    }
    else if (!Active) RestoreGSFrameLimit();
    return true;
}
void FLCCCinematicGSEditorModule::RestoreGSFrameLimit()
{
    if (SavedFrameRange.IsSet())
    {
        if (GEditor && GEditor->SmoothedFrameRateRange==AppliedFrameRange)
            GEditor->SmoothedFrameRateRange=SavedFrameRange.GetValue();
        SavedFrameRange.Reset();
    }
}


// PIE/VR Preview telemetry. Game-thread-only, opt-in; no GS shaders or scene state changed.
static TAutoConsoleVariable<int32> GSPreviewRecordEnabled(TEXT("gs.PCVR.RecordPreview"), 0,
    TEXT("Record PIE/VR Preview frame timings to CSV and TXT. 0=off(default), 1=on. Set before starting PIE."));
static TAutoConsoleVariable<int32> GSPreviewRecordInterval(TEXT("gs.PCVR.RecordEveryNFrames"), 30,
    TEXT("Record one sample every N game frames (minimum 1)."));

static TAutoConsoleVariable<int32> GSPreviewTraceGPU(TEXT("gs.PCVR.RecordGPUTrace"), 1,
    TEXT("When preview recording begins, start Unreal Insights GPU trace. 1=on (default), 0=off. GPU pass ms must be inspected in the trace."));

// Built-in Unreal Insights (UE 5.8) can export its actual GPU timer aggregates without UI.
// This is independent of the CPU-side counters; never substitute CPU cycles for GPU ms.
static TAutoConsoleVariable<int32> GSPreviewExportGPUTimers(TEXT("gs.PCVR.AutoExportGPUTimers"), 1,
    TEXT("After recorded PIE/VR Preview, run UnrealInsights.exe headlessly and export GPU timer statistics to a second CSV. 1=on(default), 0=off."));

// Integrated: one-click GS-only profiling / test-mode button, persisted per project. Does not change any
// render-quality setting, and does not run profiling outside an actual GS PIE.
namespace GSPreviewRecorderUI
{
    namespace
    {
        static const TCHAR* ConfigSection = TEXT("LCCGS.VRPreviewRecorder");
        static const TCHAR* ConfigKey = TEXT("bFrameRecordingEnabled");
        static const TCHAR* RasterVariantKey = TEXT("RasterDiagnosticVariant");
        static const TCHAR* GSOnlySceneKey = TEXT("bGSOnlyScene");
        static const TCHAR* DisableAllBloomKey = TEXT("bDisableAllBloomV13");
        static const TCHAR* EarlyAlphaV15Key = TEXT("bEarlyAlphaV15");
        static const TCHAR* TrimFogMRTV15Key = TEXT("bTrimFogMRTV15");
        static const TCHAR* PerSplatCacheV16Key = TEXT("bPerSplatLightCacheV16");
        static const TCHAR* IndirectDispatchV16Key = TEXT("bLightIndirectDispatchV16");
        static const TCHAR* RadialDepthV17Key = TEXT("bRadialDepthReuseV17");
        static bool bDisableAllBloom = true;
        static bool bGSOnlyScene = false;
        static int32 RasterDiagnosticVariant = 0;
        struct FPreviewCVar
        {
            const TCHAR* Name;
            int32 EnabledValue;
            int32 PreviousValue = 0;
            bool bHasPrevious = false;
        };
        static FPreviewCVar PreviewCVars[] =
        {
            { TEXT("gs.PCVR.RecordPreview"), 1 },
            { TEXT("gs.PCVR.RecordEveryNFrames"), 1 },
            { TEXT("gs.PCVR.RecordGPUTrace"), 1 },
            { TEXT("gs.PCVR.AutoExportGPUTimers"), 1 },
            { TEXT("gs.PCVR.RecordIndirectCounts"), 1 },
            { TEXT("gs.PCVR.PreviewStats"), 1 }
        };
        static bool bPresetEnabled = false;

        void Apply(bool bEnabled)
        {
            if (bPresetEnabled == bEnabled) return;
            // PIE/VR Preview is not a supported moment to change capture settings.
            if (GEditor && GEditor->PlayWorld) return;
            for (FPreviewCVar& Setting : PreviewCVars)
            {
                IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Setting.Name);
                if (!Var)
                {
                    UE_LOG(LogTemp, Warning, TEXT("[GSPreviewRecorderUI] Missing CVar: %s"), Setting.Name);
                    continue;
                }
                if (bEnabled)
                {
                    Setting.PreviousValue = Var->GetInt();
                    Setting.bHasPrevious = true;
                    Var->Set(Setting.EnabledValue, ECVF_SetByConsole);
                }
                else if (Setting.bHasPrevious)
                {
                    Var->Set(Setting.PreviousValue, ECVF_SetByConsole);
                    Setting.bHasPrevious = false;
                }
            }
            bPresetEnabled = bEnabled;
            UE_LOG(LogTemp, Display, TEXT("[GSPreviewRecorderUI] VR performance test mode %s. Next GS VR Preview will %srecord."),
                bEnabled ? TEXT("ENABLED") : TEXT("DISABLED"), bEnabled ? TEXT("") : TEXT("not "));
        }
    }

    bool IsEnabled()
    {
        return bPresetEnabled;
    }

    // V15-A/B: persistent pre-PIE CVar controls. No effect is disabled here.
    bool IsEarlyAlphaEnabled()
    {
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.EarlyAlphaBeforeLighting")))
            return V->GetInt()!=0;
        return false;
    }
    void SetEarlyAlphaEnabled(bool bEnabled)
    {
        if (GEditor && GEditor->PlayWorld) return;
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.EarlyAlphaBeforeLighting")))
            V->Set(bEnabled?1:0, ECVF_SetByConsole);
        if (GConfig)
        {
            GConfig->SetBool(ConfigSection, EarlyAlphaV15Key, bEnabled, GEditorPerProjectIni);
            GConfig->Flush(false, GEditorPerProjectIni);
        }
    }
    bool IsTrimFogMRTEnabled()
    {
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.TrimInactiveFogMRT")))
            return V->GetInt()!=0;
        return false;
    }
    void SetTrimFogMRTEnabled(bool bEnabled)
    {
        if (GEditor && GEditor->PlayWorld) return;
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.TrimInactiveFogMRT")))
            V->Set(bEnabled?1:0, ECVF_SetByConsole);
        if (GConfig)
        {
            GConfig->SetBool(ConfigSection, TrimFogMRTV15Key, bEnabled, GEditorPerProjectIni);
            GConfig->Flush(false, GEditorPerProjectIni);
        }
    }
    bool IsPerSplatLightCacheEnabled()
    {
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.ComputeFinalColor")))
            return V->GetInt()!=0;
        return false;
    }
    void SetPerSplatLightCacheEnabled(bool bEnabled)
    {
        if (GEditor && GEditor->PlayWorld) return;
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.ComputeFinalColor")))
            V->Set(bEnabled?1:0,ECVF_SetByConsole);
        if (GConfig)
        {
            GConfig->SetBool(ConfigSection,PerSplatCacheV16Key,bEnabled,GEditorPerProjectIni);
            GConfig->Flush(false,GEditorPerProjectIni);
        }
    }
    bool IsVisibleLightDispatchEnabled()
    {
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.FinalColorIndirectDispatch")))
            return V->GetInt()!=0;
        return false;
    }
    void SetVisibleLightDispatchEnabled(bool bEnabled)
    {
        if (GEditor && GEditor->PlayWorld) return;
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.FinalColorIndirectDispatch")))
            V->Set(bEnabled?1:0,ECVF_SetByConsole);
        if (GConfig)
        {
            GConfig->SetBool(ConfigSection,IndirectDispatchV16Key,bEnabled,GEditorPerProjectIni);
            GConfig->Flush(false,GEditorPerProjectIni);
        }
    }
    bool IsRadialDepthReuseEnabled()
    {
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.ReuseRadialDepth")))
            return V->GetInt()!=0;
        return false;
    }
    void SetRadialDepthReuseEnabled(bool bEnabled)
    {
        if (GEditor && GEditor->PlayWorld) return;
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.ReuseRadialDepth")))
            V->Set(bEnabled?1:0,ECVF_SetByConsole);
        if (GConfig)
        {
            GConfig->SetBool(ConfigSection,RadialDepthV17Key,bEnabled,GEditorPerProjectIni);
            GConfig->Flush(false,GEditorPerProjectIni);
        }
    }
    bool IsDisableAllBloomEnabled() { return bDisableAllBloom; }
    void SetDisableAllBloomEnabled(bool bEnabled)
    {
        if (GEditor && GEditor->PlayWorld) return;
        bDisableAllBloom = bEnabled;
        if (GConfig)
        {
            GConfig->SetBool(ConfigSection,DisableAllBloomKey,bEnabled,GEditorPerProjectIni);
            GConfig->Flush(false,GEditorPerProjectIni);
        }
    }

    bool IsGSOnlySceneEnabled()
    {
        return bGSOnlyScene;
    }

    void SetGSOnlySceneEnabled(bool bEnabled)
    {
        if (GEditor && GEditor->PlayWorld) return;
        bGSOnlyScene = bEnabled;
        if (GConfig)
        {
            GConfig->SetBool(ConfigSection, GSOnlySceneKey, bGSOnlyScene, GEditorPerProjectIni);
            GConfig->Flush(false, GEditorPerProjectIni);
        }
    }

    int32 GetRasterDiagnosticVariant()
    {
        return RasterDiagnosticVariant;
    }

    void SetRasterDiagnosticVariant(int32 Mode)
    {
        if (GEditor && GEditor->PlayWorld) return;
        RasterDiagnosticVariant = FMath::Clamp(Mode, 0, 3);
        if (GConfig)
        {
            GConfig->SetInt(ConfigSection, RasterVariantKey, RasterDiagnosticVariant, GEditorPerProjectIni);
            GConfig->Flush(false, GEditorPerProjectIni);
        }
    }

    void SetEnabled(bool bEnabled)
    {
        Apply(bEnabled);
        if (bPresetEnabled == bEnabled && GConfig)
        {
            GConfig->SetBool(ConfigSection, ConfigKey, bEnabled, GEditorPerProjectIni);
            GConfig->Flush(false, GEditorPerProjectIni);
        }
    }

    void LoadFromEditorConfig()
    {
        bool bEnabled = false;
        if (GConfig)
        {
            GConfig->GetBool(ConfigSection, ConfigKey, bEnabled, GEditorPerProjectIni);
            GConfig->GetInt(ConfigSection, RasterVariantKey, RasterDiagnosticVariant, GEditorPerProjectIni);
            GConfig->GetBool(ConfigSection, GSOnlySceneKey, bGSOnlyScene, GEditorPerProjectIni);
            GConfig->GetBool(ConfigSection, DisableAllBloomKey, bDisableAllBloom, GEditorPerProjectIni);
            bool bRestoreAlphaV15=false, bRestoreTrimFogV15=false;
            GConfig->GetBool(ConfigSection, EarlyAlphaV15Key, bRestoreAlphaV15, GEditorPerProjectIni);
            GConfig->GetBool(ConfigSection, TrimFogMRTV15Key, bRestoreTrimFogV15, GEditorPerProjectIni);
            if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.EarlyAlphaBeforeLighting")))
                V->Set(bRestoreAlphaV15?1:0, ECVF_SetByConsole);
            if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.TrimInactiveFogMRT")))
                V->Set(bRestoreTrimFogV15?1:0, ECVF_SetByConsole);
            bool bCacheV16=false,bIndirectV16=false;
            const bool bHasCacheSetting=GConfig->GetBool(ConfigSection,PerSplatCacheV16Key,bCacheV16,GEditorPerProjectIni);
            const bool bHasIndirectSetting=GConfig->GetBool(ConfigSection,IndirectDispatchV16Key,bIndirectV16,GEditorPerProjectIni);
            bool bDepthV17=false;
            if (GConfig->GetBool(ConfigSection,RadialDepthV17Key,bDepthV17,GEditorPerProjectIni))
                if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.ReuseRadialDepth")))
                    V->Set(bDepthV17?1:0,ECVF_SetByConsole);
            // Do not silently override ComputeFinalColor set in an existing project.
            if (bHasCacheSetting)
                if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.ComputeFinalColor")))
                    V->Set(bCacheV16?1:0, ECVF_SetByConsole);
            if (bHasIndirectSetting)
                if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.FinalColorIndirectDispatch")))
                    V->Set(bIndirectV16?1:0, ECVF_SetByConsole);
        }
        RasterDiagnosticVariant = FMath::Clamp(RasterDiagnosticVariant, 0, 3);
        Apply(bEnabled);
    }

    void RestoreCVarsOnShutdown()
    {
        // Do not touch the persisted selection; reapply it next editor launch.
        Apply(false);
    }
}

static FString GSRecorderCVar(const TCHAR* Name)
{
    if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
        return Var->GetString().Replace(TEXT(","), TEXT(";"));
    return TEXT("NA");
}

// This exporter starts only after Trace.Stop and after the trace file has stopped growing.
// Execution is separate from the editor game/render threads and never blocks PIE shutdown.
void FLCCCinematicGSEditorModule::WriteGSInsightsExportStatus(
    const FGSInsightsExportJob& Job, const TCHAR* State, const FString& Details) const
{
    const FString Status = FString::Printf(
        TEXT("LCCGS GPU TIMER CSV EXPORT\nState=%s\nDetails=%s\nTrace=%s\nGPUCSV=%s\nResponseFile=%s\nExporterLog=%s\n"),
        State, *Details, *Job.TracePath, *Job.GPUCSVPath, *Job.ResponsePath, *Job.LogPath);
    FFileHelper::SaveStringToFile(Status, *Job.StatusPath);
}

void FLCCCinematicGSEditorModule::QueueGSInsightsExport(
    const FString& InBasePath, const FString& InTracePath)
{
    TUniquePtr<FGSInsightsExportJob> Job = MakeUnique<FGSInsightsExportJob>();
    Job->TracePath = InTracePath;
    Job->GPUCSVPath = InBasePath + TEXT("_GPU_Timers.csv");
    Job->ResponsePath = InBasePath + TEXT("_GPU_Export.rsp");
    Job->LogPath = InBasePath + TEXT("_GPU_Exporter.log");
    Job->StatusPath = InBasePath + TEXT("_GPU_Export_Status.txt");
    Job->QueuedSeconds = FPlatformTime::Seconds();
    // Response-file syntax is documented by Epic for the -ExecOnAnalysisCompleteCmd option.
    // Intel's UE profiling guide documents -threads=GPU for ExportTimerStatistics.
    const FString ResponseText = FString::Printf(
        TEXT("TimingInsights.ExportTimerStatistics \"%s\" -threads=GPU -region=GSVRPreview\n"),
        *Job->GPUCSVPath.Replace(TEXT("\\"), TEXT("/")));
    if (!FFileHelper::SaveStringToFile(ResponseText, *Job->ResponsePath))
    {
        WriteGSInsightsExportStatus(*Job, TEXT("FAILED"), TEXT("Could not write export response file."));
        return;
    }
    WriteGSInsightsExportStatus(*Job, TEXT("WAITING_FOR_TRACE"), TEXT("Waiting for Trace.Stop to finish writing the .utrace file."));
    GSInsightsExportJobs.Add(MoveTemp(Job));
}

void FLCCCinematicGSEditorModule::TickGSInsightsExports()
{
    if (GSInsightsExportJobs.IsEmpty()) return;
    const double Now = FPlatformTime::Seconds();
    // File-size polls and external-process status are deliberately throttled.
    if (Now - GSLastInsightsExportPollSeconds < 1.0) return;
    GSLastInsightsExportPollSeconds = Now;

    for (int32 I = GSInsightsExportJobs.Num() - 1; I >= 0; --I)
    {
        FGSInsightsExportJob& Job = *GSInsightsExportJobs[I];
        if (!Job.bProcessLaunched)
        {
            const int64 Bytes = IFileManager::Get().FileSize(*Job.TracePath);
            if (Bytes > 0 && Bytes == Job.PreviousTraceBytes)
                ++Job.StablePolls;
            else
                Job.StablePolls = 0;
            Job.PreviousTraceBytes = Bytes;
            if (Now - Job.QueuedSeconds > 120.0)
            {
                WriteGSInsightsExportStatus(Job, TEXT("FAILED"), TEXT("Trace file not finalized within 120 seconds. Check Trace.File and Trace.Stop in Output Log."));
                GSInsightsExportJobs.RemoveAt(I);
                continue;
            }
            if (Bytes < 1 || Job.StablePolls < 2 || Now - Job.QueuedSeconds < 3.0)
                continue;

            const FString InsightsExe = FPaths::ConvertRelativePathToFull(
                FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealInsights.exe"));
            if (!IFileManager::Get().FileExists(*InsightsExe))
            {
                WriteGSInsightsExportStatus(Job, TEXT("FAILED"),
                    FString::Printf(TEXT("UnrealInsights.exe not found: %s"), *InsightsExe));
                UE_LOG(LogTemp, Warning, TEXT("[GSPreviewRecorder] GPU CSV export: UnrealInsights.exe not found at %s"), *InsightsExe);
                GSInsightsExportJobs.RemoveAt(I);
                continue;
            }
            // Use UE's documented headless analysis/export path. This is a separate process
            // so neither the game nor render thread waits while the .utrace is analyzed.
            const FString Args = FString::Printf(
                TEXT("-OpenTraceFile=\"%s\" -AutoQuit -NoUI -ABSLOG=\"%s\" -ExecOnAnalysisCompleteCmd=\"@=%s\""),
                *Job.TracePath, *Job.LogPath, *Job.ResponsePath);
            Job.Process = FPlatformProcess::CreateProc(*InsightsExe, *Args,
                true, true, true, nullptr, 0, *FPaths::ProjectDir(), nullptr, nullptr);
            if (!Job.Process.IsValid())
            {
                WriteGSInsightsExportStatus(Job, TEXT("FAILED"), TEXT("Failed to start the UnrealInsights exporter process."));
                GSInsightsExportJobs.RemoveAt(I);
                continue;
            }
            Job.bProcessLaunched = true;
            WriteGSInsightsExportStatus(Job, TEXT("EXPORTING"), TEXT("Unreal Insights is analyzing .utrace and exporting GPU timer statistics."));
            UE_LOG(LogTemp, Display, TEXT("[GSPreviewRecorder] GPU timer CSV export launched: %s"), *Job.GPUCSVPath);
            continue;
        }
        if (FPlatformProcess::IsProcRunning(Job.Process))
        {
            if (Now - Job.QueuedSeconds > 600.0)
            {
                // Do not terminate the external tool or block the editor; report the timeout.
                WriteGSInsightsExportStatus(Job, TEXT("TIMED_OUT"), TEXT("GPU exporter is still running after 10 minutes. Inspect exporter log; external process was not killed."));
                FPlatformProcess::CloseProc(Job.Process);
                GSInsightsExportJobs.RemoveAt(I);
            }
            continue;
        }
        int32 ExitCode = -1;
        const bool bGotCode = FPlatformProcess::GetProcReturnCode(Job.Process, &ExitCode);
        FPlatformProcess::CloseProc(Job.Process);
        const int64 CSVBytes = IFileManager::Get().FileSize(*Job.GPUCSVPath);
        FString ExportedContents;
        const bool bHasRows = CSVBytes > 100 && FFileHelper::LoadFileToString(ExportedContents, *Job.GPUCSVPath)
            && ExportedContents.Contains(TEXT("Name,Count,"))
            && ExportedContents.Contains(TEXT("\n"))
            && ExportedContents.Contains(TEXT("GSPCVR"));
        if (bGotCode && ExitCode == 0 && bHasRows)
        {
            // Produce a third, compact GPU-only CSV containing LCCGS GPU timers.
            // Values originate from Insights: Incl/I.Avg in seconds, converted to milliseconds.
            // A parent timer may include its children; do NOT sum hierarchy rows as total frame cost.
            TArray<FString> TimerLines;
            ExportedContents.ParseIntoArrayLines(TimerLines, true);
            FString GpuGSCSV = TEXT("Timer,Count,InclusiveTotalMs,AverageGpuMs,ExclusiveTotalMs\n");
            for (int32 RowIndex=1; RowIndex<TimerLines.Num(); ++RowIndex)
            {
                TArray<FString> Fields;
                TimerLines[RowIndex].ParseIntoArray(Fields, TEXT(","), false);
                if (Fields.Num()<9) continue;
                const FString& Name = Fields[0];
                if (!Name.Contains(TEXT("Gaussian")) && !Name.Contains(TEXT("GSPCVR")) && !Name.Contains(TEXT("GS_VR"))) continue;
                GpuGSCSV += FString::Printf(TEXT("%s,%s,%.6f,%.6f,%.6f\n"), *Name, *Fields[1],
                    FCString::Atod(*Fields[3])*1000.0,
                    FCString::Atod(*Fields[6])*1000.0,
                    FCString::Atod(*Fields[8])*1000.0);
            }
            FFileHelper::SaveStringToFile(GpuGSCSV, *Job.GPUCSVPath.Replace(TEXT("_GPU_Timers.csv"), TEXT("_GPU_GS_Timers.csv")));
            // A dedicated comparison-friendly list of nonadditive GPU event scopes.
            // ExclusiveTotalMs and InclusiveTotalMs originate solely from Insights, never CPU dispatch cost.
            FString RasterStages = TEXT("Stage,Timer,EventCount,MeanInclusiveGpuMs,TotalInclusiveGpuMs,TotalExclusiveGpuMs,Warning\n");
            for (int32 RowIndex=1; RowIndex<TimerLines.Num(); ++RowIndex)
            {
                TArray<FString> Fields;
                TimerLines[RowIndex].ParseIntoArray(Fields, TEXT(","), false);
                if (Fields.Num()<9) continue;
                const FString& Name = Fields[0];
                const TCHAR* Stage = nullptr;
                if (Name == TEXT("GSPCVRRaster_LeftEye")) Stage = TEXT("RasterLeft");
                else if (Name == TEXT("GSPCVRRaster_RightEye")) Stage = TEXT("RasterRight");
                else if (Name == TEXT("GSPCVRRaster_OtherEye")) Stage = TEXT("RasterOther");
                else if (Name == TEXT("GSPCVRRasterOnly")) Stage = TEXT("RasterParent");
                else if (Name == TEXT("GaussianSplatDrawGlobalIndirect")) Stage = TEXT("DrawParent");
                else if (Name == TEXT("GaussianSplatPCVRFinalColor")) Stage = TEXT("ColorCompute");
                else if (Name.Contains(TEXT("GaussianSplatRadixSort"))) Stage = TEXT("Sorting");
                else if (Name.Contains(TEXT("GaussianSplatCalcViewData"))) Stage = TEXT("ViewData");
                else if (Name.Contains(TEXT("GaussianSplatPostGradeHDR"))) Stage = TEXT("PostGrade");
                else if (Name.Contains(TEXT("GaussianSplatPostGradeBloomComposite"))) Stage = TEXT("GSBloom");
                if (!Stage) continue;
                RasterStages += FString::Printf(TEXT("%s,%s,%s,%.6f,%.6f,%.6f,ParentChildEventsOverlapDoNotSum\n"),
                    Stage, *Name, *Fields[1], FCString::Atod(*Fields[6])*1000.0,
                    FCString::Atod(*Fields[3])*1000.0, FCString::Atod(*Fields[8])*1000.0);
            }
            const FString StagesPath = Job.GPUCSVPath.Replace(TEXT("_GPU_Timers.csv"), TEXT("_RasterStages.csv"));
            FFileHelper::SaveStringToFile(RasterStages, *StagesPath);
            WriteGSInsightsExportStatus(Job, TEXT("COMPLETE"),
                FString::Printf(TEXT("GPU timer statistics filtered to GSVRPreview region (%lld bytes); GS GPU scopes found."), static_cast<long long>(CSVBytes)));
            UE_LOG(LogTemp, Display, TEXT("[GSPreviewRecorder] GPU timer CSV export completed: %s"), *Job.GPUCSVPath);
        }
        else
        {
            WriteGSInsightsExportStatus(Job, TEXT("FAILED"),
                FString::Printf(TEXT("ExitCode=%d ReturnCodeAvailable=%d CSVBytes=%lld GPUScopes=%d. Empty/missing GSVRPreview region or export failure; inspect _GPU_Exporter.log and trace region events."),
                    ExitCode, int32(bGotCode), static_cast<long long>(CSVBytes), int32(bHasRows)));
            UE_LOG(LogTemp, Warning, TEXT("[GSPreviewRecorder] GPU timer CSV export failed (code %d): %s"), ExitCode, *Job.GPUCSVPath);
        }
        GSInsightsExportJobs.RemoveAt(I);
    }
}

// GS-only GPU comparison: only transient PIE mesh render flags are changed.
// Never modify editor actors, static-mesh assets, transforms, collisions, GS-owned
// proxies, capture-only proxy geometry, sky domes or the XR player rig.
void FLCCCinematicGSEditorModule::ApplyGSOnlyScene(UWorld* World)
{
    RestoreGSOnlyScene();
    bGSOnlySceneSession = GSPreviewRecorderUI::IsGSOnlySceneEnabled();
    GSOnlyHiddenMeshCount = 0;
    GSOnlyPreservedProxyCount = 0;
    if (!bGSOnlySceneSession || !World || World->WorldType != EWorldType::PIE ||
        !GSAutomaticWorldScope::HasGS(World)) return;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || Actor->IsA<AGaussianSplatActor>()) continue;
        bool bGSOwned = false;
        bool bPlayerOwned = false;
        for (AActor* Parent = Actor; Parent; Parent = Parent->GetOwner())
        {
            bGSOwned |= Parent->IsA<AGaussianSplatActor>();
            bPlayerOwned |= Parent->IsA<APawn>();
        }
        for (AActor* Parent = Actor->GetAttachParentActor(); Parent;
            Parent = Parent->GetAttachParentActor())
        {
            bGSOwned |= Parent->IsA<AGaussianSplatActor>();
            bPlayerOwned |= Parent->IsA<APawn>();
        }
        // Spherical sky meshes are part of the requested GS scene environment.
        const bool bSky = Actor->GetName().Contains(TEXT("SkySphere")) ||
            Actor->GetClass()->GetName().Contains(TEXT("SkySphere")) ||
            Actor->GetName().Contains(TEXT("Sky_Sphere")) ||
            Actor->GetName().Contains(TEXT("SkyDome")) ||
            Actor->GetName().Contains(TEXT("Skybox"));
        if (bGSOwned || bPlayerOwned || bSky) continue;

        TArray<UPrimitiveComponent*> Primitives;
        Actor->GetComponents(Primitives);
        for (UPrimitiveComponent* Component : Primitives)
        {
            if (!Component || !Component->IsRegistered()) continue;
            const FString ClassName = Component->GetClass()->GetName();
            const bool bNativeGeometry = Component->IsA<UStaticMeshComponent>() ||
                Component->IsA<USkinnedMeshComponent>() ||
                ClassName.Contains(TEXT("LandscapeComponent")) ||
                ClassName.Contains(TEXT("ProceduralMeshComponent")) ||
                ClassName.Contains(TEXT("GeometryCollectionComponent"));
            if (!bNativeGeometry) continue;
            const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component);
            if (Component->bVisibleInSceneCaptureOnly ||
                (Mesh && GetPathNameSafe(Mesh->GetStaticMesh()).Contains(TEXT("/collider/"))))
            {
                ++GSOnlyPreservedProxyCount;
                continue;
            }
            // Record originals even if already hidden, ensuring exact flag restoration.
            FGSOnlySavedPrimitive& Saved = GSOnlySavedPrimitives.AddDefaulted_GetRef();
            Saved.Component = Component;
            Saved.bOriginallyHiddenInGame = Component->bHiddenInGame;
            Saved.bOriginallyCastShadow = Component->CastShadow;
            Component->SetHiddenInGame(true, false);
            Component->SetCastShadow(false);
            ++GSOnlyHiddenMeshCount;
        }
    }
    UE_LOG(LogTemp, Display, TEXT("[GSOnlyScene][Begin] Enabled=1 HiddenNativeMeshes=%d PreservedProxyMeshes=%d PIEOnly=1 GSRenderingUnchanged=1"),
        GSOnlyHiddenMeshCount, GSOnlyPreservedProxyCount);
    if (GSOnlyHiddenMeshCount == 0)
        UE_LOG(LogTemp, Warning, TEXT("[GSOnlyScene] No eligible native mesh components found; the GS-only and normal captures may be identical."));
}

void FLCCCinematicGSEditorModule::RestoreGSOnlyScene()
{
    for (FGSOnlySavedPrimitive& Saved : GSOnlySavedPrimitives)
    {
        if (UPrimitiveComponent* Component = Saved.Component.Get())
        {
            Component->SetHiddenInGame(Saved.bOriginallyHiddenInGame, false);
            Component->SetCastShadow(Saved.bOriginallyCastShadow);
        }
    }
    GSOnlySavedPrimitives.Empty();
    bGSOnlySceneSession = false;
}

void FLCCCinematicGSEditorModule::ApplyVRBloomAblation(UWorld* World)
{
    RestoreVRBloomAblation();
    if (!GSPreviewRecorderUI::IsDisableAllBloomEnabled() || !World || World->WorldType!=EWorldType::PIE ||
        !GSAutomaticWorldScope::HasGS(World)) return;

    // Do not mutate level components or the user's authored bloom settings.
    // Save and restore all console overrides used by this single PIE session.
    if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.DisableAllBloom")))
    {
        OriginalGSBloom=V->GetInt(); bSavedGSBloom=true;
        V->Set(1, ECVF_SetByConsole);
    }
    if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("r.BloomQuality")))
    {
        OriginalUEBloom=V->GetInt(); bSavedUEBloom=true;
        V->Set(0, ECVF_SetByConsole);
    }
    // Directional light-shaft bloom is independent of standard UE PP bloom.
    // Disable only its bloom flag on PIE copies; keep shaft occlusion and shadows.
    for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
    {
        UDirectionalLightComponent* Light=*It;
        if (!IsValid(Light) || Light->GetWorld()!=World) continue;
        if (Light->bEnableLightShaftBloom)
        {
            FGSPreviewSavedLightShaft Entry;
            Entry.Component=Light;
            Entry.bHadShaftBloom=true;
            SavedPreviewLightShafts.Add(Entry);
            Light->SetEnableLightShaftBloom(false);
        }
    }
    bVRBloomSession=true;
    UE_LOG(LogTemp, Display, TEXT("[GSOptV13][BloomOff] GS spatial=0 GS atmosphere seed=0 UE BloomQuality=0 LightShaftBloomDisabled=%d ShadowAndLighting=REFERENCE StereoSharedCulling=UNCHANGED"),
        SavedPreviewLightShafts.Num());
}

void FLCCCinematicGSEditorModule::RestoreVRBloomAblation()
{
    if (bSavedGSBloom)
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.DisableAllBloom")))
            V->Set(OriginalGSBloom, ECVF_SetByConsole);
    if (bSavedUEBloom)
        if (IConsoleVariable* V=IConsoleManager::Get().FindConsoleVariable(TEXT("r.BloomQuality")))
            V->Set(OriginalUEBloom, ECVF_SetByConsole);
    for (const FGSPreviewSavedLightShaft& Entry:SavedPreviewLightShafts)
        if (UDirectionalLightComponent* Light=Entry.Component.Get())
            Light->SetEnableLightShaftBloom(Entry.bHadShaftBloom);
    SavedPreviewLightShafts.Empty();
    bSavedGSBloom=false; bSavedUEBloom=false;
    bVRBloomSession=false;
}

void FLCCCinematicGSEditorModule::BeginGSPreviewRecord(bool bIsSimulating)
{
    if (GEditor && GEditor->PlayWorld) ApplyVRBloomAblation(GEditor->PlayWorld);
    if (GSPreviewRecordEnabled.GetValueOnGameThread() == 0 || !GEditor || !GEditor->PlayWorld || bGSRecording)
        return;
    UWorld* World = GEditor->PlayWorld;
    if (!World || !GSAutomaticWorldScope::HasGS(World)) return;
    // Isolate native geometry before opening the trace region or taking samples.
    ApplyGSOnlyScene(World);
    // The comparison path is opt-in and PIE-only. Production mode 0 never changes the shader.
    GSRasterRecordedVariant = GSPreviewRecorderUI::GetRasterDiagnosticVariant();
    bRasterVariantWasApplied = false;
    if (IConsoleVariable* Variant = IConsoleManager::Get().FindConsoleVariable(TEXT("gs.VRDiagMode")))
    {
        GSRasterOriginalVariant = Variant->GetInt();
        if (GSRasterOriginalVariant != GSRasterRecordedVariant)
        {
            Variant->Set(GSRasterRecordedVariant, ECVF_SetByConsole);
            bRasterVariantWasApplied = true;
        }
    }
    GSPreviewPassCounters::Snapshot(GSPreviousDispatchCounts, GSPreviewPassCounters::NumPasses);
    GSPreviewPassCounters::SnapshotCPUCycles(GSPreviousCPUCycles, GSPreviewPassCounters::NumPasses);
    GSPreviewPassCounters::SnapshotWork(GSPreviousWork, GSPreviewPassCounters::NumPasses);
    GSPreviewPassCounters::SnapshotSkips(GSPreviousSkips, GSPreviewPassCounters::NumPasses);
    GSPreviewPassCounters::SnapshotRaster(GSPreviousRasterMetrics);
    FMemory::Memcpy(GSRasterWorkloadPrevious, GSPreviousRasterMetrics, sizeof(GSRasterWorkloadPrevious));
    GSPreviewPassCounters::SetEnabled(true);
    bGSRecording = true;
    FMemory::Memzero(GSTotalDispatchCounts, sizeof(GSTotalDispatchCounts));
    FMemory::Memzero(GSTotalCPUCycles, sizeof(GSTotalCPUCycles));
    FMemory::Memzero(GSTotalWork, sizeof(GSTotalWork));
    FMemory::Memzero(GSTotalSkips, sizeof(GSTotalSkips));
    GSRecordFrames = 0;
    GSRecordSamples = 0;
    GSRecordSumMs = 0.0;
    GSRecordMaxMs = 0.0;
    GSRecordGPUSumMs = 0.0;
    GSRecordGPUSamples = 0;
    GSRecordStartSeconds = FPlatformTime::Seconds();
    GSRasterWorkloadBuffer = TEXT("ElapsedSeconds,SampleFrame,Eye,DrawCallsSinceLastSample,MeanSourceCapacityPerDraw,MeanViewportPixelsPerDraw,ReadbackSamplesSinceLastSample,MeanDrawInstancesPerReadback,MeanDrawInstancesPerMegapixel,DiagnosticVariant,Notes\n");
    GSRecordBuffer = TEXT("Frame,ElapsedSeconds,FrameDeltaMs,GPUFrameMs,GPUValid,GSActors,PCVR,SharedCulling,PreSortCompact,RuntimeMode,SortCacheDiagnostics,BloomExtractCache,TileComposite,ClusterCullCallsDelta,CompactLegacyCallsDelta,CompactPCVRCallsDelta,CompactStablePCVRCallsDelta,ViewDataFullCallsDelta,ViewDataCompactedCallsDelta,DistancesFullCallsDelta,DistancesIndirectCallsDelta,RadixSortFullCallsDelta,RadixSortIndirectCallsDelta,DrawFullCallsDelta,DrawIndirectCallsDelta,SnowDrawFullCallsDelta,SnowDrawIndirectCallsDelta,PostGradeHDRCallsDelta,BloomCompositeCallsDelta,WetSurfaceCallsDelta\\n");
    GSRecordBuffer.ReplaceInline(TEXT("\\n"), TEXT("\n"));
    GSRecordBuffer.RemoveFromEnd(TEXT("\n"));
    static const TCHAR* CPUColumns[] = { TEXT("ClusterCull"),TEXT("CompactLegacy"),TEXT("CompactPCVR"),TEXT("CompactStablePCVR"),TEXT("ViewDataFull"),TEXT("ViewDataCompacted"),TEXT("DistancesFull"),TEXT("DistancesIndirect"),TEXT("RadixSortFull"),TEXT("RadixSortIndirect"),TEXT("DrawFull"),TEXT("DrawIndirect"),TEXT("SnowDrawFull"),TEXT("SnowDrawIndirect"),TEXT("PostGradeHDR"),TEXT("BloomComposite"),TEXT("WetSurface") };
    for (int32 I = 0; I < GSPreviewPassCounters::NumPasses; ++I)
        GSRecordBuffer += FString::Printf(TEXT(",%s_CPUCommandMsDelta"), CPUColumns[I]);
    for (int32 I=0; I<GSPreviewPassCounters::NumPasses; ++I)
        GSRecordBuffer += FString::Printf(TEXT(",%s_InputCandidateDelta,%s_ReuseSkipDelta"), CPUColumns[I], CPUColumns[I]);
    GSRecordBuffer += TEXT("\n");
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("LCCGS/Profiling");
    IFileManager::Get().MakeDirectory(*Dir, true);
    GSRecordBasePath = Dir / FString::Printf(TEXT("GS_Preview_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
    // Capture effect settings without toggling/altering any quality or shader options.
    const TCHAR* FeatureCVars[] = {
        TEXT("gs.PCVR.Enable"), TEXT("gs.PCVR.SharedCulling"), TEXT("gs.PCVR.PreSortCompact"),
        TEXT("gs.PCVR.DisableAllBloom"), TEXT("r.BloomQuality"),
        TEXT("gs.PCVR.TileComposite"), TEXT("gs.PC.VertexLighting"), TEXT("gs.PC.SourceQuadTrim"),
        TEXT("gs.PCVR.EarlyAlphaBeforeLighting"), TEXT("gs.PCVR.TrimInactiveFogMRT"),
        TEXT("gs.PCVR.ComputeFinalColor"), TEXT("gs.PCVR.FinalColorIndirectDispatch"), TEXT("gs.PCVR.ReuseRadialDepth"), TEXT("gs.PCVR.PackedFog"), TEXT("gs.PCVR.ShadowBlueOverlay"),
        TEXT("gs.VRDiagMode"), TEXT("gs.PanoramaRainBloomHalfSamples"),
        TEXT("gs.SortCacheDiagnostics"), TEXT("gs.PCVR.RecordIndirectCounts"),
        TEXT("r.ScreenPercentage"), TEXT("vr.PixelDensity")
    };
    FString FeatureRows = TEXT("CVar,Value\n");
    for (const TCHAR* Name : FeatureCVars)
        FeatureRows += FString::Printf(TEXT("%s,%s\n"), Name, *GSRecorderCVar(Name));
    FeatureRows += FString::Printf(TEXT("V15EarlyAlphaSelected,%d\nV15TrimFogMRTSelected,%d\n"),
        GSPreviewRecorderUI::IsEarlyAlphaEnabled()?1:0, GSPreviewRecorderUI::IsTrimFogMRTEnabled()?1:0);
    FeatureRows += FString::Printf(TEXT("V17RadialDepthReuseSelected,%d\n"),
        GSPreviewRecorderUI::IsRadialDepthReuseEnabled()?1:0);
    FeatureRows += FString::Printf(TEXT("V16PerSplatLightCacheSelected,%d\nV16IndirectVisibleCountSelected,%d\n"),
        GSPreviewRecorderUI::IsPerSplatLightCacheEnabled()?1:0, GSPreviewRecorderUI::IsVisibleLightDispatchEnabled()?1:0);
    FeatureRows += FString::Printf(TEXT("V13BloomOffSelected,%d\nV13BloomOffApplied,%d\nV13LightShaftBloomComponentsDisabled,%d\nV13ShadowLightingReferencePath,1\n"),
        GSPreviewRecorderUI::IsDisableAllBloomEnabled()?1:0, bVRBloomSession?1:0, SavedPreviewLightShafts.Num());
    FeatureRows += FString::Printf(TEXT("GSOnlySceneEnabled,%d\nGSOnlyHiddenNativeMeshCount,%d\nGSOnlyPreservedProxyMeshCount,%d\n"),
        bGSOnlySceneSession ? 1 : 0, GSOnlyHiddenMeshCount, GSOnlyPreservedProxyCount);
    FeatureRows += FString::Printf(TEXT("RasterDiagnosticVariantSelected,%d\n"), GSRasterRecordedVariant);
    FeatureRows += TEXT("RasterDiagnosticVariant0,OriginalQuality\nRasterDiagnosticVariant1,BasicPS_SameMRT\nRasterDiagnosticVariant2,BasicPS_TwoMRT\nRasterDiagnosticVariant3,TrainedSH_OneMRT_NoWeather\n");
    FFileHelper::SaveStringToFile(FeatureRows, *(GSRecordBasePath + TEXT("_FeatureSettings.csv")));
    FFileHelper::SaveStringToFile(GSRasterWorkloadBuffer, *(GSRecordBasePath + TEXT("_RasterWorkload.csv")));
    GSRasterWorkloadBuffer.Empty();
    FFileHelper::SaveStringToFile(GSRecordBuffer, *(GSRecordBasePath + TEXT(".csv")));
    GSRecordBuffer.Empty();
    bGSStartedGPUTrace = false;
    bGSStartedTraceRegion = false;
    GSTraceRegionName = TEXT("GSVRPreview");
    GSTraceFilePath.Empty();
    if (GSPreviewTraceGPU.GetValueOnGameThread() != 0 && GEngine)
    {
        // UE 5.8 Trace.File accepts an explicit absolute path; use the same output
        // directory and session basename as CSV/TXT (not the Trace Store).
        GSTraceFilePath = (GSRecordBasePath + TEXT(".utrace")).Replace(TEXT("\\"), TEXT("/"));
        const FString TraceCommand = FString::Printf(TEXT("Trace.File \"%s\" gpu,frame,cpu,bookmark,region"), *GSTraceFilePath);
        bGSStartedGPUTrace = GEngine->Exec(World, *TraceCommand);
        if (bGSStartedGPUTrace)
        {
            // Headless Insights -region=GSVRPreview restricts statistics to this PIE window.
            const FString RegionStart = TEXT("Trace.RegionBegin GSVRPreview");
            bGSStartedTraceRegion = GEngine->Exec(World, *RegionStart);
        }
        UE_LOG(LogTemp, Display, TEXT("[GSPreviewRecorder] Trace.File requested: %s; Exec=%d RegionBegin=%d (inspect log if missing)"),
            *GSTraceFilePath, int32(bGSStartedGPUTrace), int32(bGSStartedTraceRegion));
    }
    UE_LOG(LogTemp, Display, TEXT("[GSPreviewRecorder] Started %s.csv"), *GSRecordBasePath);
}

bool FLCCCinematicGSEditorModule::TickGSPreviewRecord(float DeltaTime)
{
    TickGSInsightsExports();
    if (!bGSRecording) return true;
    if (!GEditor || !GEditor->PlayWorld) { EndGSPreviewRecord(false); return true; }
    ++GSRecordFrames;
    const uint32 Interval = FMath::Max(1, GSPreviewRecordInterval.GetValueOnGameThread());
    if ((GSRecordFrames % Interval) != 0) return true;
    // RHIGetGPUFrameCycles returns the RHI's most recent *whole frame* measurement.
    // It is NOT a measurement of any particular Gaussian pass and can be unavailable.
    const uint32 GPUCycles = RHIGetGPUFrameCycles();
    const bool bGPUValid = GPUCycles != 0;
    const double GPUms = bGPUValid ? FPlatformTime::ToMilliseconds(GPUCycles) : 0.0;
    const double FrameMs = DeltaTime * 1000.0;
    ++GSRecordSamples;
    GSRecordSumMs += FrameMs;
    GSRecordMaxMs = FMath::Max(GSRecordMaxMs, FrameMs);
    if (bGPUValid) { GSRecordGPUSumMs += GPUms; ++GSRecordGPUSamples; }
    int32 GSActors = 0;
    if (UWorld* World = GEditor->PlayWorld)
        for (TActorIterator<AGaussianSplatActor> It(World); It; ++It) ++GSActors;
    const FString GPUField = bGPUValid ? FString::Printf(TEXT("%.4f"), GPUms) : TEXT("");
    GSRecordBuffer += FString::Printf(TEXT("%llu,%.3f,%.4f,%s,%d,%d,%s,%s,%s,%s,%s,%s,%s\n"),
        static_cast<unsigned long long>(GSRecordFrames), FPlatformTime::Seconds() - GSRecordStartSeconds,
        FrameMs, *GPUField, bGPUValid ? 1 : 0, GSActors,
        *GSRecorderCVar(TEXT("gs.PCVR.Enable")), *GSRecorderCVar(TEXT("gs.PCVR.SharedCulling")),
        *GSRecorderCVar(TEXT("gs.PCVR.PreSortCompact")), *GSRecorderCVar(TEXT("gs.PCVR.RuntimeMode")),
        *GSRecorderCVar(TEXT("gs.SortCacheDiagnostics")), *GSRecorderCVar(TEXT("gs.PCVR.BloomExtractCache")),
        *GSRecorderCVar(TEXT("gs.PCVR.TileComposite")));
    GSRecordBuffer.RemoveFromEnd(TEXT("\n"));
    uint64 CurrentCounters[GSPreviewPassCounters::NumPasses] = {};
    GSPreviewPassCounters::Snapshot(CurrentCounters, GSPreviewPassCounters::NumPasses);
    for (int32 I = 0; I < GSPreviewPassCounters::NumPasses; ++I)
    {
        const uint64 Delta = CurrentCounters[I] - GSPreviousDispatchCounts[I];
        GSRecordBuffer += FString::Printf(TEXT(",%llu"), static_cast<unsigned long long>(Delta));
        GSPreviousDispatchCounts[I] = CurrentCounters[I];
        GSTotalDispatchCounts[I] += Delta;
    }
    uint64 CurrentCPU[GSPreviewPassCounters::NumPasses] = {};
    GSPreviewPassCounters::SnapshotCPUCycles(CurrentCPU, GSPreviewPassCounters::NumPasses);
    for (int32 I = 0; I < GSPreviewPassCounters::NumPasses; ++I)
    {
        const uint64 DeltaCycles = CurrentCPU[I] >= GSPreviousCPUCycles[I]
            ? CurrentCPU[I] - GSPreviousCPUCycles[I] : 0;
        GSPreviousCPUCycles[I] = CurrentCPU[I];
        GSTotalCPUCycles[I] += DeltaCycles;
        GSRecordBuffer += FString::Printf(TEXT(",%.5f"), FPlatformTime::ToMilliseconds64(DeltaCycles));
    }
    uint64 Work[GSPreviewPassCounters::NumPasses] = {};
    uint64 Skips[GSPreviewPassCounters::NumPasses] = {};
    GSPreviewPassCounters::SnapshotWork(Work, GSPreviewPassCounters::NumPasses);
    GSPreviewPassCounters::SnapshotSkips(Skips, GSPreviewPassCounters::NumPasses);
    for (int32 I=0; I<GSPreviewPassCounters::NumPasses; ++I)
    {
        const uint64 DeltaWork = Work[I] >= GSPreviousWork[I] ? Work[I]-GSPreviousWork[I] : 0;
        const uint64 DeltaSkips = Skips[I] >= GSPreviousSkips[I] ? Skips[I]-GSPreviousSkips[I] : 0;
        GSPreviousWork[I]=Work[I]; GSPreviousSkips[I]=Skips[I];
        GSTotalWork[I]+=DeltaWork; GSTotalSkips[I]+=DeltaSkips;
        GSRecordBuffer += FString::Printf(TEXT(",%llu,%llu"),
            static_cast<unsigned long long>(DeltaWork), static_cast<unsigned long long>(DeltaSkips));
    }
    // Candidate work counts are CPU-visible inputs; not GPU-executed element counts.
    // Counters and CPU submission timings are sampled from render thread asynchronously; NOT exact per-frame GPU timings.
    GSRecordBuffer += TEXT("\n");
    // Workload telemetry is read from atomic render-thread counters and has no shader side effects.
    // It is NOT a per-frame GPU timing or an actual pixel-overdraw count.
    uint64 RasterSample[GSPreviewPassCounters::RasterEyeCount][GSPreviewPassCounters::RasterMetricCount] = {};
    GSPreviewPassCounters::SnapshotRaster(RasterSample);
    const TCHAR* EyeNames[GSPreviewPassCounters::RasterEyeCount] = { TEXT("Left"), TEXT("Right"), TEXT("Other") };
    for (int32 Eye=0; Eye<GSPreviewPassCounters::RasterEyeCount; ++Eye)
    {
        uint64 Delta[GSPreviewPassCounters::RasterMetricCount] = {};
        for (int32 Metric=0; Metric<GSPreviewPassCounters::RasterMetricCount; ++Metric)
        {
            const uint64 Current = RasterSample[Eye][Metric];
            const uint64 Previous = GSRasterWorkloadPrevious[Eye][Metric];
            Delta[Metric] = Current >= Previous ? Current - Previous : 0;
            GSRasterWorkloadPrevious[Eye][Metric] = Current;
        }
        if (Delta[0] == 0 && Delta[3] == 0) continue;
        const double MeanCapacity = Delta[0] ? double(Delta[1])/double(Delta[0]) : 0.0;
        const double MeanViewport = Delta[0] ? double(Delta[2])/double(Delta[0]) : 0.0;
        const double MeanInstances = Delta[3] ? double(Delta[4])/double(Delta[3]) : 0.0;
        const double InstancesPerMPixel = Delta[3] && MeanViewport > 0.0 ? MeanInstances*1000000.0/MeanViewport : 0.0;
        GSRasterWorkloadBuffer += FString::Printf(
            TEXT("%.3f,%llu,%s,%llu,%.3f,%.3f,%llu,%.3f,%.3f,%d,ReadbackAsyncNotAlignedToSample\n"),
            FPlatformTime::Seconds()-GSRecordStartSeconds,
            static_cast<unsigned long long>(GSRecordFrames), EyeNames[Eye],
            static_cast<unsigned long long>(Delta[0]), MeanCapacity, MeanViewport,
            static_cast<unsigned long long>(Delta[3]), MeanInstances, InstancesPerMPixel,
            GSRasterRecordedVariant);
    }
    // Periodically persist samples so a crash loses at most a small segment.
    if (GSRecordSamples % 20 == 0) FlushGSPreviewRecord();
    return true;
}

void FLCCCinematicGSEditorModule::FlushGSPreviewRecord()
{
    if (!GSRecordBuffer.IsEmpty() && !GSRecordBasePath.IsEmpty())
    {
        FFileHelper::SaveStringToFile(GSRecordBuffer, *(GSRecordBasePath + TEXT(".csv")),
            FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
        GSRecordBuffer.Empty();
    }
    if (!GSRasterWorkloadBuffer.IsEmpty() && !GSRecordBasePath.IsEmpty())
    {
        FFileHelper::SaveStringToFile(GSRasterWorkloadBuffer, *(GSRecordBasePath + TEXT("_RasterWorkload.csv")),
            FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
        GSRasterWorkloadBuffer.Empty();
    }
}

void FLCCCinematicGSEditorModule::RestoreGSRasterDiagnosticVariant()
{
    if (!bRasterVariantWasApplied) return;
    if (IConsoleVariable* Variant = IConsoleManager::Get().FindConsoleVariable(TEXT("gs.VRDiagMode")))
        Variant->Set(GSRasterOriginalVariant, ECVF_SetByConsole);
    bRasterVariantWasApplied = false;
}

void FLCCCinematicGSEditorModule::EndGSPreviewRecord(bool bIsSimulating)
{
    if (!bGSRecording) { RestoreVRBloomAblation(); RestoreGSOnlyScene(); return; }
    GSPreviewPassCounters::SetEnabled(false);
    FlushGSPreviewRecord();
    const bool bThisSessionStartedTrace = bGSStartedGPUTrace;
    if (bGSStartedGPUTrace && GEngine)
    {
        if (bGSStartedTraceRegion)
            GEngine->Exec(GEditor ? GEditor->PlayWorld : nullptr, TEXT("Trace.RegionEnd GSVRPreview"));
        GEngine->Exec(GEditor ? GEditor->PlayWorld : nullptr, TEXT("Trace.Stop"));
        bGSStartedTraceRegion = false;
        bGSStartedGPUTrace = false;
    }
    const double Elapsed = FPlatformTime::Seconds() - GSRecordStartSeconds;
    const double AvgMs = GSRecordSamples ? GSRecordSumMs / GSRecordSamples : 0.0;
    const FString Summary = FString::Printf(TEXT(
        "LCCGS VR/PIE PREVIEW RECORDING\n"
        "DurationSeconds=%.3f\nFramesObserved=%llu\nSamples=%llu\n"
        "AverageSampledFrameDeltaMs=%.4f\nMaxSampledFrameDeltaMs=%.4f\n"
        "AverageAvailableWholeGPUFrameMs=%.4f\nValidGPUSamples=%llu\n"
        "NOTE: GPU time represents the whole GPU frame, NOT Gaussian-only time.\n"
        "NOTE: FrameDeltaMs is sampled ticker delta, NOT CPU Game/Render thread GPU pass cost.\n"
        "NOTE: Samples are every N frames; averages are sample averages, not exact session means.\n"
        "NOTE: GPU timings may lag the current frame and can be unavailable (blank CSV field).\n"
        "For per-pass cost capture, use Unreal Insights GPU trace or profilegpu with RDG event scopes.\n"
        "Console: gs.PCVR.RecordPreview 1 ; gs.PCVR.RecordEveryNFrames 30\n"
        ), Elapsed, static_cast<unsigned long long>(GSRecordFrames),
        static_cast<unsigned long long>(GSRecordSamples), AvgMs, GSRecordMaxMs,
        GSRecordGPUSamples ? GSRecordGPUSumMs / GSRecordGPUSamples : 0.0,
        static_cast<unsigned long long>(GSRecordGPUSamples));
    // A separate per-eye CSV avoids implying asynchronous readback values match a sampled frame.
    uint64 RasterNow[GSPreviewPassCounters::RasterEyeCount][GSPreviewPassCounters::RasterMetricCount] = {};
    GSPreviewPassCounters::SnapshotRaster(RasterNow);
    FString RasterRows = TEXT("Eye,DrawCalls,InputCapacitySum,ViewportPixelSum,ReadbackSamples,DrawnInstancesSampleSum,MeanDrawnInstancesPerReadback\n");
    const TCHAR* EyeLabels[3] = { TEXT("Left"), TEXT("Right"), TEXT("MonoOrOther") };
    for (int32 Eye=0; Eye<3; ++Eye)
    {
        uint64 D[5] = {};
        for (int32 M=0; M<5; ++M)
            D[M] = RasterNow[Eye][M] >= GSPreviousRasterMetrics[Eye][M]
                ? RasterNow[Eye][M]-GSPreviousRasterMetrics[Eye][M] : 0;
        RasterRows += FString::Printf(TEXT("%s,%llu,%llu,%llu,%llu,%llu,%.3f\n"), EyeLabels[Eye],
            static_cast<unsigned long long>(D[0]), static_cast<unsigned long long>(D[1]),
            static_cast<unsigned long long>(D[2]), static_cast<unsigned long long>(D[3]),
            static_cast<unsigned long long>(D[4]), D[3] ? double(D[4])/double(D[3]) : 0.0);
    }
    FFileHelper::SaveStringToFile(RasterRows, *(GSRecordBasePath + TEXT("_RasterEyes.csv")));
    FString DispatchSummary = TEXT("\nDISPATCH CALL COUNTS (sampled, render thread, not GPU timings)\n");
    static const TCHAR* Labels[] = {TEXT("ClusterCull"),TEXT("CompactLegacy"),TEXT("CompactPCVR"),TEXT("CompactStablePCVR"),TEXT("ViewDataFull"),TEXT("ViewDataCompacted"),TEXT("DistancesFull"),TEXT("DistancesIndirect"),TEXT("RadixSortFull"),TEXT("RadixSortIndirect"),TEXT("DrawFull"),TEXT("DrawIndirect"),TEXT("SnowDrawFull"),TEXT("SnowDrawIndirect"),TEXT("PostGradeHDR"),TEXT("BloomComposite"),TEXT("WetSurface")};
    for (int32 I = 0; I < GSPreviewPassCounters::NumPasses; ++I)
        DispatchSummary += FString::Printf(TEXT("%s=%llu\n"), Labels[I], static_cast<unsigned long long>(GSTotalDispatchCounts[I]));
    DispatchSummary += TEXT("\nCPU COMMAND PREPARATION TIMES (milliseconds; not GPU execution)\n");
    for (int32 I = 0; I < GSPreviewPassCounters::NumPasses; ++I)
        DispatchSummary += FString::Printf(TEXT("%s_CPUCommandMs=%.5f\n"), Labels[I], FPlatformTime::ToMilliseconds64(GSTotalCPUCycles[I]));
    DispatchSummary += TEXT("\nCPU-VISIBLE INPUT CANDIDATES / REUSE SKIPS (NOT EXACT GPU TEST COUNTS)\n");
    for (int32 I=0; I<GSPreviewPassCounters::NumPasses; ++I)
        DispatchSummary += FString::Printf(TEXT("%s_InputCandidate=%llu ReuseSkip=%llu\n"),Labels[I],
            static_cast<unsigned long long>(GSTotalWork[I]),static_cast<unsigned long long>(GSTotalSkips[I]));
    if (!GSTraceFilePath.IsEmpty())
    {
        const int64 TraceBytes = IFileManager::Get().FileSize(*GSTraceFilePath);
        DispatchSummary += FString::Printf(TEXT("GPU TRACE PATH: %s\nGPU TRACE FILE AT SUMMARY TIME: %s\n"),
            *GSTraceFilePath, TraceBytes >= 0 ? *FString::Printf(TEXT("present (%lld bytes)"), static_cast<long long>(TraceBytes)) : TEXT("not found yet; see output log / Trace.Status"));
    }
    else
    {
        DispatchSummary += TEXT("GPU TRACE: disabled for this session.\n");
    }
    DispatchSummary += FString::Printf(TEXT("GPU timer CSV auto export: %s\n"),
        bThisSessionStartedTrace && !GSTraceFilePath.IsEmpty() && GSPreviewExportGPUTimers.GetValueOnGameThread() != 0
            ? TEXT("queued after Trace.Stop; see _GPU_Export_Status.txt") : TEXT("disabled"));
    DispatchSummary += FString::Printf(TEXT("GSOnlyScene=%d HiddenNativeMeshComponents=%d PreservedProxyComponents=%d (PIE render visibility only; original flags restored).\n"),
        bGSOnlySceneSession ? 1 : 0, GSOnlyHiddenMeshCount, GSOnlyPreservedProxyCount);
    DispatchSummary += FString::Printf(TEXT("RasterDiagnosticVariant=%d (0=unchanged; 1..3=opt-in visual changes during this PIE; auto-restored).\n"), GSRasterRecordedVariant);
    DispatchSummary += TEXT("RasterWorkload.csv: sample-period view work; drawn instances are sparse asynchronous GPU readback. No pixel-shader invocation count or true overdraw is measured.\n");
    DispatchSummary += TEXT("Compare separate VR Preview runs at fixed camera pose/resolution/scene; raster GPU time must come from Unreal Insights, not source-capacity counts.\n");
    DispatchSummary += TEXT("V7 GPU timer filter: GSVRPreview region only; CSV GPU time aggregates come from Unreal Insights.\n");
    DispatchSummary += TEXT("V7 per-eye raster input upper bounds and sparse async indirect counts: see _RasterEyes.csv.\n");
    DispatchSummary += TEXT("NOTE: Viewport pixels are NOT pixel shader invocations or actual overdraw. Composite effects share GPU scopes unless separately instrumented.\n");
    DispatchSummary += TEXT("GPU TRACE: Open the named .utrace in Unreal Insights, GPU track; filter for GaussianSplat events.\n");
    DispatchSummary += TEXT("GPU pass timing is sourced from Unreal Insights; when auto export is enabled, look for _GPU_Timers.csv after export completes.\n");
    DispatchSummary += TEXT("NOTE: These are sampled function-call counts, NOT per-Gaussian checks, visible splat counts or per-pass GPU ms.\n");
    DispatchSummary += TEXT("For actual per-pass GPU milliseconds capture an Unreal Insights GPU trace / profilegpu; no timing values are fabricated.\n");
    FFileHelper::SaveStringToFile(Summary + DispatchSummary, *(GSRecordBasePath + TEXT("_Summary.txt")));
    UE_LOG(LogTemp, Display, TEXT("[GSPreviewRecorder] Stopped: %s.csv / %s_Summary.txt"), *GSRecordBasePath, *GSRecordBasePath);
    if (bThisSessionStartedTrace && !GSTraceFilePath.IsEmpty() && GSPreviewExportGPUTimers.GetValueOnGameThread() != 0)
        QueueGSInsightsExport(GSRecordBasePath, GSTraceFilePath);
    // Never leave any PIE-only mesh visibility or diagnostic shader overrides active.
    RestoreGSOnlyScene();
    RestoreGSRasterDiagnosticVariant();
    RestoreVRBloomAblation();
    bGSRecording = false;
    GSRecordBasePath.Empty();
    GSTraceFilePath.Empty();
}

void FLCCCinematicGSEditorModule::StartupModule()
{
    GSPreviewRecorderUI::LoadFromEditorConfig();
    PreviewStatsHandle=FEditorDelegates::PostPIEStarted.AddRaw(this,&FLCCCinematicGSEditorModule::EnablePreviewStats);
    GSRecordBeginHandle=FEditorDelegates::PostPIEStarted.AddRaw(this,&FLCCCinematicGSEditorModule::BeginGSPreviewRecord);
    GSRecordEndHandle=FEditorDelegates::EndPIE.AddRaw(this,&FLCCCinematicGSEditorModule::EndGSPreviewRecord);
    GSRecordTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this,&FLCCCinematicGSEditorModule::TickGSPreviewRecord),0.0f);
	FrameLimitTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this,&FLCCCinematicGSEditorModule::UpdateGSFrameLimit),0.5f);
	if (GUnrealEd) RegisterProxyAlignmentVisualizer();
	else PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FLCCCinematicGSEditorModule::RegisterProxyAlignmentVisualizer);
	// Register asset type actions
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Register Gaussian Splat asset type
	TSharedPtr<IAssetTypeActions> GaussianSplatAssetActions = MakeShareable(new FAssetTypeActions_GaussianSplatAsset());
	AssetTools.RegisterAssetTypeActions(GaussianSplatAssetActions.ToSharedRef());
	RegisteredAssetTypeActions.Add(GaussianSplatAssetActions);

	// Register custom thumbnail renderer for Gaussian Splat assets
	UThumbnailManager::Get().RegisterCustomRenderer(
		UGaussianSplatAsset::StaticClass(),
		UGaussianSplatThumbnailRenderer::StaticClass());

	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyEditor.RegisterCustomClassLayout(
		TEXT("GaussianSplatActor"),
		FOnGetDetailCustomizationInstance::CreateStatic(&FGaussianSplatDetails::MakeInstance));
	PropertyEditor.RegisterCustomClassLayout(
		TEXT("GaussianSplatComponent"),
		FOnGetDetailCustomizationInstance::CreateStatic(&FGaussianSplatDetails::MakeInstance));
	PropertyEditor.NotifyCustomizationModuleChanged();

	UE_LOG(LogTemp, Log, TEXT("LCCCinematicGSEditor module started."));
}

void FLCCCinematicGSEditorModule::ShutdownModule()
{
    EndGSPreviewRecord(false);
    RestoreGSOnlyScene();
    RestoreVRBloomAblation();
    GSPreviewRecorderUI::RestoreCVarsOnShutdown();
    FEditorDelegates::PostPIEStarted.Remove(PreviewStatsHandle);
    FEditorDelegates::PostPIEStarted.Remove(GSRecordBeginHandle);
    FEditorDelegates::EndPIE.Remove(GSRecordEndHandle);
    // Exporter is launched detached; do not forcefully terminate ongoing trace analysis.
    for (TUniquePtr<FGSInsightsExportJob>& Job : GSInsightsExportJobs)
    {
        if (Job && Job->Process.IsValid())
            FPlatformProcess::CloseProc(Job->Process);
    }
    GSInsightsExportJobs.Empty();
    FTSTicker::GetCoreTicker().RemoveTicker(GSRecordTicker);
	FTSTicker::GetCoreTicker().RemoveTicker(FrameLimitTicker);
	RestoreGSFrameLimit();
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	if (GUnrealEd) GUnrealEd->UnregisterComponentVisualizer(UGaussianSplatComponent::StaticClass()->GetFName());
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyEditor =
			FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyEditor.UnregisterCustomClassLayout(TEXT("GaussianSplatActor"));
		PropertyEditor.UnregisterCustomClassLayout(TEXT("GaussianSplatComponent"));
	}

	// Unregister asset type actions
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		for (auto& Action : RegisteredAssetTypeActions)
		{
			AssetTools.UnregisterAssetTypeActions(Action.ToSharedRef());
		}
	}
	RegisteredAssetTypeActions.Empty();

	UE_LOG(LogTemp, Log, TEXT("LCCCinematicGSEditor module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLCCCinematicGSEditorModule, LCCCinematicGSEditor)
