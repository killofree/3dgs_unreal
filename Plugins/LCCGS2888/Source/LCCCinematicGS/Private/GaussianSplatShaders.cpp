// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatShaders.h"
#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"

// Implement global shaders
IMPLEMENT_GLOBAL_SHADER(FGaussianPCVRFinalColorCS, "/Plugin/LCCCinematicGS/Private/PCVRFinalColor.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCalcViewDataCS, "/Plugin/LCCCinematicGS/Private/CalcViewData.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianShadowValueSampleCS, "/Plugin/LCCCinematicGS/Private/ShadowValueSample.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildStableProxyVertexVisibilityCS, "/Plugin/LCCCinematicGS/Private/BuildStableProxyVertexVisibility.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildStableProxyReceiverTriangleIdCS, "/Plugin/LCCCinematicGS/Private/BuildStableProxyReceiverTriangleId.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildStableProxyCastGridCS, "/Plugin/LCCCinematicGS/Private/BuildStableProxyCastGrid.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCalcDistancesCS, "/Plugin/LCCCinematicGS/Private/CalcDistances.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatVS, "/Plugin/LCCCinematicGS/Private/GaussianSplatRendering.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSnowOverlayVS, "/Plugin/LCCCinematicGS/Private/GaussianSplatRendering.usf", "SnowOverlayVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSnowOverlayPS, "/Plugin/LCCCinematicGS/Private/GaussianSplatRendering.usf", "SnowOverlayPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPS, "/Plugin/LCCCinematicGS/Private/GaussianSplatRendering.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositeVS, "/Plugin/LCCCinematicGS/Private/GaussianSplatComposite.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositePS, "/Plugin/LCCCinematicGS/Private/GaussianSplatComposite.usf", "MainGradePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatBloomCompositePS, "/Plugin/LCCCinematicGS/Private/GaussianSplatComposite.usf", "MainBloomCompositePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatWetSurfacePS, "/Plugin/LCCCinematicGS/Private/GaussianSplatWetSurface.usf", "WetSurfacePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FRadixSortCountCS, "/Plugin/LCCCinematicGS/Private/RadixSort.usf", "CountCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRadixSortPrefixSumCS, "/Plugin/LCCCinematicGS/Private/RadixSort.usf", "PrefixSumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRadixSortDigitPrefixSumCS, "/Plugin/LCCCinematicGS/Private/RadixSort.usf", "DigitPrefixSumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRadixSortScatterCS, "/Plugin/LCCCinematicGS/Private/RadixSort.usf", "ScatterCS", SF_Compute);

// Cluster culling shaders
IMPLEMENT_GLOBAL_SHADER(FClusterCullingResetCS, "/Plugin/LCCCinematicGS/Private/ClusterCulling.usf", "ResetCounterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClusterCullingCS, "/Plugin/LCCCinematicGS/Private/ClusterCulling.usf", "MainCS", SF_Compute);

// Splat compaction shaders (GPU-driven work reduction)
IMPLEMENT_GLOBAL_SHADER(FCompactSplatsCS, "/Plugin/LCCCinematicGS/Private/CompactSplats.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FPrepareIndirectArgsCS, "/Plugin/LCCCinematicGS/Private/PrepareIndirectArgs.usf", "MainCS", SF_Compute);

// NOTE: GPU-driven LOD shaders removed in unified approach
// LOD splats are now processed by CalcViewData.usf using the same buffers as original splats

// Global accumulator + compaction prefix-sum shaders
IMPLEMENT_GLOBAL_SHADER(FGatherVisibleCountsCS,     "/Plugin/LCCCinematicGS/Private/GlobalAccumulatorPrefixSum.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FPrefixSumVisibleCountsCS,  "/Plugin/LCCCinematicGS/Private/GlobalAccumulatorPrefixSum.usf", "MainCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatVRDiagnosticCompositePS, "/Plugin/LCCCinematicGS/Private/VRDiagnosticComposite.usf", "MainPS", SF_Pixel);

IMPLEMENT_GLOBAL_SHADER(FGSVRBaselineDrawArgsCS, "/Plugin/LCCCinematicGS/Private/VRBaselineDrawArgs.usf", "MainCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FGSPCVRCompactCountCS,"/Plugin/LCCCinematicGS/Private/PCVRStableCompact.usf","CountCS",SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGSPCVRCompactPrefixCS,"/Plugin/LCCCinematicGS/Private/PCVRStableCompact.usf","PrefixCS",SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGSPCVRCompactScatterCS,"/Plugin/LCCCinematicGS/Private/PCVRStableCompact.usf","ScatterCS",SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FGSPCVRSourceCountCS,"/Plugin/LCCCinematicGS/Private/PCVRStableCompact.usf","CountCS",SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FGSPCVRSourcePrefixCS,"/Plugin/LCCCinematicGS/Private/PCVRStableCompact.usf","PrefixCS",SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FGSPCVRSourceScatterCS,"/Plugin/LCCCinematicGS/Private/PCVRStableCompact.usf","ScatterCS",SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FGaussianPCVRBloomExtractCS,"/Plugin/LCCCinematicGS/Private/GaussianSplatComposite.usf","MainBloomExtractCS",SF_Compute);
