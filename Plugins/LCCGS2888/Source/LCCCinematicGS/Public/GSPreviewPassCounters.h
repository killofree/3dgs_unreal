// Opt-in preview dispatch counters. Counts render-thread *function entries*, NOT GPU pass milliseconds.
#pragma once
#include "CoreMinimal.h"

namespace GSPreviewPassCounters
{
    enum EPass : int32
    {
        ClusterCull,
        CompactLegacy,
        CompactPCVR,
        CompactStablePCVR,
        ViewDataFull,
        ViewDataCompacted,
        DistancesFull,
        DistancesIndirect,
        RadixSortFull,
        RadixSortIndirect,
        DrawFull,
        DrawIndirect,
        SnowDrawFull,
        SnowDrawIndirect,
        PostGradeHDR,
        BloomComposite,
        WetSurface,
        NumPasses
    };
    LCCCINEMATICGS_API void SetEnabled(bool bEnabled);
    LCCCINEMATICGS_API void Snapshot(uint64* OutCounts, int32 Count);
    LCCCINEMATICGS_API void Hit(EPass Pass);
    LCCCINEMATICGS_API bool IsEnabled();
    LCCCINEMATICGS_API void AddWork(EPass Pass, uint64 WorkItems);
    LCCCINEMATICGS_API void SnapshotWork(uint64* OutWork, int32 Count);
    LCCCINEMATICGS_API void MarkSkip(EPass Pass);
    LCCCINEMATICGS_API void SnapshotSkips(uint64* OutSkips, int32 Count);
    LCCCINEMATICGS_API void AddCPUTime(EPass Pass, uint64 Cycles);
    LCCCINEMATICGS_API void SnapshotCPUCycles(uint64* OutCycles, int32 Count);
    // Per-eye counts. EyeIndex: 0=left, 1=right, 2=mono/other.
    // Layout per eye: DrawCalls, DrawSourceCapacitySum, ViewportPixelSum,
    //                  ReadbackSampleCount, DrawnInstanceSampleSum.
    // SourceCapacity is an input upper bound, NOT actually rasterized GS or pixel coverage.
    // GPU readbacks are delayed asynchronous sparse samples; NOT every frame.
    static constexpr int32 RasterEyeCount = 3;
    static constexpr int32 RasterMetricCount = 5;
    LCCCINEMATICGS_API void RecordRasterDraw(int32 EyeIndex, uint32 SourceCapacity, uint64 ViewportPixels);
    LCCCINEMATICGS_API void RecordRasterReadback(int32 EyeIndex, uint32 DrawnInstances);
    LCCCINEMATICGS_API void SnapshotRaster(uint64 OutMetrics[RasterEyeCount][RasterMetricCount]);
    // Timing covers CPU-side command preparation/submission, NOT GPU shader execution.
    struct FScopedCPUTime
    {
        EPass Pass;
        bool bActive;
        uint64 StartCycles;
        explicit FScopedCPUTime(EPass InPass)
            : Pass(InPass), bActive(IsEnabled()), StartCycles(bActive ? FPlatformTime::Cycles64() : 0)
        {
            if (bActive) Hit(Pass);
        }
        ~FScopedCPUTime()
        {
            if (bActive)
            {
                const uint64 EndCycles = FPlatformTime::Cycles64();
                // Guard unsigned underflow and implausibly long sections; invalid data is never reported as ms.
                if (EndCycles >= StartCycles)
                {
                    const uint64 ElapsedCycles = EndCycles - StartCycles;
                    if (FPlatformTime::ToMilliseconds64(ElapsedCycles) <= 5000.0)
                        AddCPUTime(Pass, ElapsedCycles);
                }
            }
        }
    };
}
