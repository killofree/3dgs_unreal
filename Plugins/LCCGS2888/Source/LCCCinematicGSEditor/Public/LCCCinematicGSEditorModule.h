// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"

class FLCCCinematicGSEditorModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
    bool UpdateGSFrameLimit(float);
    void RestoreGSFrameLimit();
    FTSTicker::FDelegateHandle FrameLimitTicker;
    TOptional<FFloatRange> SavedFrameRange;
    FFloatRange AppliedFrameRange;
	void RegisterProxyAlignmentVisualizer();
    void EnablePreviewStats(bool bIsSimulating);
    FDelegateHandle PreviewStatsHandle;
    // PIE-only native mesh isolation for the optional GS-only measurement.
    struct FGSOnlySavedPrimitive
    {
        TWeakObjectPtr<class UPrimitiveComponent> Component;
        bool bOriginallyHiddenInGame = false;
        bool bOriginallyCastShadow = false;
    };
    TArray<FGSOnlySavedPrimitive> GSOnlySavedPrimitives;
    int32 GSOnlyHiddenMeshCount = 0;
    int32 GSOnlyPreservedProxyCount = 0;
    bool bGSOnlySceneSession = false;
    // PIE-only bloom-only comparison. Original values are restored on EndPIE.
    struct FGSPreviewSavedLightShaft
    {
        TWeakObjectPtr<class UDirectionalLightComponent> Component;
        bool bHadShaftBloom = false;
    };
    TArray<FGSPreviewSavedLightShaft> SavedPreviewLightShafts;
    bool bVRBloomSession=false;
    bool bSavedGSBloom=false, bSavedUEBloom=false;
    int32 OriginalGSBloom=0, OriginalUEBloom=0;
    void ApplyVRBloomAblation(class UWorld* World);
    void RestoreVRBloomAblation();
    void ApplyGSOnlyScene(class UWorld* World);
    void RestoreGSOnlyScene();
    void BeginGSPreviewRecord(bool bIsSimulating);
    void EndGSPreviewRecord(bool bIsSimulating);
    bool TickGSPreviewRecord(float DeltaTime);
    void FlushGSPreviewRecord();
    FDelegateHandle GSRecordBeginHandle;
    FDelegateHandle GSRecordEndHandle;
    FTSTicker::FDelegateHandle GSRecordTicker;
    uint64 GSPreviousRasterMetrics[3][5] = {};
    FString GSTraceRegionName;
    bool bGSStartedTraceRegion = false;
    uint64 GSPreviousDispatchCounts[17] = {};
    uint64 GSTotalDispatchCounts[17] = {};
    uint64 GSPreviousCPUCycles[17] = {};
    uint64 GSTotalCPUCycles[17] = {};
    uint64 GSPreviousWork[17] = {};
    uint64 GSTotalWork[17] = {};
    uint64 GSPreviousSkips[17] = {};
    uint64 GSTotalSkips[17] = {};
    // A completed preview may still be undergoing headless .utrace analysis when the next preview starts.
    // Keep independent jobs so a later session cannot overwrite the previous export state.
    struct FGSInsightsExportJob
    {
        FString TracePath;
        FString GPUCSVPath;
        FString ResponsePath;
        FString LogPath;
        FString StatusPath;
        double QueuedSeconds = 0.0;
        int64 PreviousTraceBytes = -1;
        int32 StablePolls = 0;
        bool bProcessLaunched = false;
        FProcHandle Process;
    };
    TArray<TUniquePtr<FGSInsightsExportJob>> GSInsightsExportJobs;
    double GSLastInsightsExportPollSeconds = 0.0;
    void QueueGSInsightsExport(const FString& InBasePath, const FString& InTracePath);
    void TickGSInsightsExports();
    void WriteGSInsightsExportStatus(const FGSInsightsExportJob& Job, const TCHAR* State, const FString& Details) const;
    bool bGSStartedGPUTrace = false;
    bool bGSRecording = false;
    uint64 GSRecordFrames = 0;
    uint64 GSRecordSamples = 0;
    uint64 GSRecordGPUSamples = 0;
    double GSRecordStartSeconds = 0.0;
    double GSRecordSumMs = 0.0;
    double GSRecordMaxMs = 0.0;
    double GSRecordGPUSumMs = 0.0;
    FString GSRecordBasePath;
    FString GSTraceFilePath;
    FString GSRecordBuffer;
    // Separately sampled raster work (not GPU shader invocations). Never change the production GS shader for telemetry.
    FString GSRasterWorkloadBuffer;
    uint64 GSRasterWorkloadPrevious[3][5] = {};
    bool bRasterVariantWasApplied = false;
    int32 GSRasterOriginalVariant = 0;
    int32 GSRasterRecordedVariant = 0;
    void RestoreGSRasterDiagnosticVariant();
	FDelegateHandle PostEngineInitHandle;
	/** Handle for registered asset type actions */
	TArray<TSharedPtr<class IAssetTypeActions>> RegisteredAssetTypeActions;
};
