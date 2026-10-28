#include "PCVRTileRenderer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "GaussianGlobalAccumulator.h"
#include "GSRemovalBoxRenderState.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "SceneView.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "RenderUtils.h"
#include "CommonRenderResources.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

static TAutoConsoleVariable<int32> CVarTile(TEXT("gs.PCVR.TileComposite"),0,
 TEXT("Experimental source-order PCVR tile accumulation. 0 original raster, 1 tiled. Current lean R16F fog/no-removal Final only; unsupported states retain raster."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileRemoval(TEXT("gs.PCVR.TileRemoval"),0,TEXT("Opt-in existing exact splat-plane box clipping for the tiled source-GS path. 0 retains legacy raster fallback for boxes."),ECVF_RenderThreadSafe);
bool IsPCVRTileRemovalEnabled_RenderThread(){return CVarTileRemoval.GetValueOnRenderThread()!=0;}
static TAutoConsoleVariable<int32> CVarTileAlphaCull(TEXT("gs.PCVR.TileAlphaCull"),0,TEXT("Conservative alpha-support intersection in Resolve 8x8 masks. Tile lists/bounds and final pixel equations unchanged; no additional lists/buffers."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileCandidateCull(TEXT("gs.PCVR.TileCandidateCull"),1,TEXT("Conservative ellipse/tile intersection before candidate registration."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileFrontToBack(TEXT("gs.PCVR.TileFrontToBack"),1,TEXT("Near-to-far tile composition. 0 preserves legacy back-to-front rounding."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarTileEarlyThreshold(TEXT("gs.PCVR.TileEarlyThreshold"),0.0001f,TEXT("Bound on omitted alpha/HDR radiance before quantization. 0 disables termination. Nearest GS depth is still evaluated."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileSubtileCull(TEXT("gs.PCVR.TileSubtileCull"),1,TEXT("Filter each ordered batch into four conservative 8x8 candidate masks."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileReuseMask(TEXT("gs.PCVR.TileReuseMask"),1,TEXT("Reuse Build ellipse/tile membership for GS covering up to 64 bounding tiles."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileProfile(TEXT("gs.PCVR.TileProfile"),0,TEXT("0 stable production; 1 split GPU attribution; 2 split + sampled work counters. Diagnostics change timing, never use their FPS as production FPS."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileProbeEvery(TEXT("gs.PCVR.TileProbeEvery"),0,TEXT("Diagnostic scratch Resolve replay interval per eye; 0 off. Requires TileProfile >= 1. Adds reference/no-RGB-fog/no-8x8 GPU work, never changes displayed output."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileParallelMerge(TEXT("gs.PCVR.TileParallelMerge"),0,TEXT("Parallel compact merge of dense sorted runs, preserving all candidates. 0 reference."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileDenseCapacity(TEXT("gs.PCVR.TileDenseCapacity"),262144,TEXT("Compact dense merge entry budget per eye; excess tiles use exact reference merge."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileWideMask(TEXT("gs.PCVR.TileWideMask"),0,TEXT("Reuse up to 256 bounding tile intersections instead of 64. Additional 24 bytes/source/eye."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileAdaptiveSort(TEXT("gs.PCVR.TileAdaptiveSort"),0,TEXT("Sort with 32/256/1024/8192 shared slots selected by tile density."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileBatchSkip(TEXT("gs.PCVR.TileBatchSkip"),0,TEXT("Skip a candidate batch after color termination only when no member can improve exact nearest depth."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileAblation(TEXT("gs.PCVR.Ablation"),0,TEXT("DIAGNOSTIC ONLY: 0 normal,1 empty pixels,2 no tile pipeline,3 minimal GS post,4 no tile+minimal post,5 unsorted linear composition. Never production FPS."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTilePoolCache(TEXT("gs.PCVR.TilePoolCache"),0,TEXT("Sparse membership cache for GS exceeding inline mask, up to 4096 bounding tiles. Pool overflow recomputes exactly."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTilePoolWords(TEXT("gs.PCVR.TilePoolWords"),1048576,TEXT("Sparse membership word budget per eye, default 4 MiB."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileWaveSort(TEXT("gs.PCVR.TileWaveSort"),0,TEXT("Sort first 32 slots in wave registers, preserving exact ordering. Runtime fallback for non-32 waves."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileCooperative(TEXT("gs.PCVR.TileCooperative"),0,TEXT("32-thread cooperative registration/emission for GS bounding more than 64 tiles; exact membership and full GS retained."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarResolveAuditEvery(TEXT("gs.PCVR.ResolveAuditEvery"),0,TEXT("Diagnostic scratch pairs: mask preparation/reuse, no depth tail, packed16 output/read. 0 disabled; minimum interval120. Visible output unchanged. Timings non-additive."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileWaveMask(TEXT("gs.PCVR.TileWaveMask"),0,TEXT("Exact parallel 8x8 mask/depth reduction inside Resolve. Actual wave job allocation, no group-index mapping assumption; validated eight 32-lane waves, otherwise original masks. No extra global buffer."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarMaskABEvery(TEXT("gs.PCVR.MaskABEvery"),0,TEXT("Same-input scratch reference/wave-mask Resolve comparison. 0 off; interval min120. Includes symmetric per-group branch proof stores; diagnostic only."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTileSortQueue(TEXT("gs.PCVR.TileSortQueue"),0,TEXT("Exact per-density active-tile sort queues built in existing prefix pass; indirect dispatch only matching nonempty tiles. No candidate/order reduction."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarResolveOrder(TEXT("gs.PCVR.ResolveOrder"),0,TEXT("Resolve launch order:0 row major,1 8-wide stripes,2 16-wide. Exact tile/output permutation, no candidate reduction."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarResolveOrderEvery(TEXT("gs.PCVR.ResolveOrderEvery"),0,TEXT("Same-input row-major/striped scratch Resolve comparisons. 0 off, minimum interval120. Diagnostic only."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarTilePackedOutput(TEXT("gs.PCVR.TilePackedOutput"),0,TEXT("16-byte Resolve/TilePS storage. Full GS/effects, exact existing half-rounded color/fog and FP32 depth. Normal non-diagnostic path only."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarResolveBatch(TEXT("gs.PCVR.ResolveBatch"),0,TEXT("0 reference,1 transposed shared fields with deferred RGB,2 fused four-wave load/masks. 3 staged AoS payload/one prepared validity flag. Exact candidate order/depth preserved; no resolution/effect change."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarResolveBatchEvery(TEXT("gs.PCVR.ResolveBatchEvery"),0,TEXT("Same-input reference vs modes1/2. 0 off, minimum120. Diagnostic scratch only."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarRegionList(TEXT("gs.PCVR.RegionList"),0,TEXT("Exact stable 8x8 sublists fused into tile sort/bounds. Bounded memory; tiny/dense/unsupported tiles use original Resolve."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarRegionListEvery(TEXT("gs.PCVR.RegionListEvery"),0,TEXT("Same-input sort+Resolve reference/sublist comparisons, includes list generation. 0 off, minimum120."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarRegionListWords(TEXT("gs.PCVR.RegionListWords"),16777216,TEXT("Region list uint word budget per eye. Capacity fallback preserves full output."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarResolveCostsEvery(TEXT("gs.PCVR.ResolveCostsEvery"),0,TEXT("Diagnostic same-input producer/noRGB and sampled recorded geometry comparisons. 0 off, minimum120. Does not modify visible output. Non-additive timings."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarResolveBatchTestMode(TEXT("gs.PCVR.ResolveBatchTestMode"),0,TEXT("0 compare historical modes1/2;3 compare staged original-layout mode only. Requires ResolveBatchEvery."),ECVF_RenderThreadSafe);
class FGSPCVRTileCS:public FGlobalShader {
 DECLARE_GLOBAL_SHADER(FGSPCVRTileCS);SHADER_USE_PARAMETER_STRUCT(FGSPCVRTileCS,FGlobalShader);
 class FStage:SHADER_PERMUTATION_INT("GS_TILE_STAGE",19);
 class FCounters:SHADER_PERMUTATION_BOOL("GS_TILE_COUNTERS");
 class FProbe:SHADER_PERMUTATION_INT("GS_TILE_PROBE",10);
 class FWideMask:SHADER_PERMUTATION_BOOL("GS_TILE_WIDE_MASK");
 class FSortBucket:SHADER_PERMUTATION_INT("GS_TILE_SORT_BUCKET",5);
 class FDenseMerge:SHADER_PERMUTATION_BOOL("GS_TILE_DENSE_MERGE");
 class FBatchSkip:SHADER_PERMUTATION_BOOL("GS_TILE_BATCH_SKIP");
 class FCooperative:SHADER_PERMUTATION_BOOL("GS_TILE_COOPERATIVE");
 class FPoolCache:SHADER_PERMUTATION_BOOL("GS_TILE_POOL_CACHE");
 class FResolvePart:SHADER_PERMUTATION_INT("GS_RESOLVE_PART",6);
 class FRegionList:SHADER_PERMUTATION_BOOL("GS_TILE_REGION_LIST");
 class FResolveBatch:SHADER_PERMUTATION_INT("GS_TILE_RESOLVE_BATCH",4);
 class FPackedOutput:SHADER_PERMUTATION_BOOL("GS_TILE_PACKED_OUTPUT");
 class FResolveOrder:SHADER_PERMUTATION_INT("GS_TILE_RESOLVE_ORDER",3);
 class FSortQueue:SHADER_PERMUTATION_BOOL("GS_TILE_SORT_QUEUE");
 class FWaveMask:SHADER_PERMUTATION_BOOL("GS_TILE_WAVE_MASK");
 class FWaveSort:SHADER_PERMUTATION_BOOL("GS_TILE_WAVE_SORT");
 using FPermutationDomain=TShaderPermutationDomain<FStage,FCounters,FProbe,FWideMask,FSortBucket,FDenseMerge,FBatchSkip,FPoolCache,FWaveSort,FCooperative,FWaveMask,FSortQueue,FResolveOrder,FPackedOutput,FResolveBatch,FRegionList,FResolvePart>;
 BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
  SHADER_PARAMETER(uint32,TileSourceCapacity)
  SHADER_PARAMETER(uint32,TileCountOverride)
  SHADER_PARAMETER(uint32,TileWidth)
  SHADER_PARAMETER(uint32,TileHeight)
  SHADER_PARAMETER(uint32,TileOriginX)
  SHADER_PARAMETER(uint32,TileOriginY)
  SHADER_PARAMETER(uint32,TileCountX)
  SHADER_PARAMETER(uint32,TileCountY)
  SHADER_PARAMETER(uint32,TileEntryCapacity)
  SHADER_PARAMETER(uint32,TileQuality)
  SHADER_PARAMETER(uint32,TileTrim)
  SHADER_PARAMETER(uint32,TileHalfRound)
  SHADER_PARAMETER(uint32,TileCandidateCull)
  SHADER_PARAMETER(uint32,TileAlphaCull)
  SHADER_PARAMETER(uint32,TileFrontToBack)
  SHADER_PARAMETER(float,TileEarlyThreshold)
  SHADER_PARAMETER(uint32,TileSubtileCull)
  SHADER_PARAMETER(uint32,TileReuseMask)
  SHADER_PARAMETER(uint32,TileProfileSplit)
  SHADER_PARAMETER(uint32,TileDensityClass)
  SHADER_PARAMETER(uint32,TileEmitClass)
  SHADER_PARAMETER(uint32,TileProfileSeed)
  SHADER_PARAMETER(uint32,TileProfileMetaOffset)
  SHADER_PARAMETER(uint32,TileProfileWords)
  SHADER_PARAMETER(uint32,TileDenseCapacity)
  SHADER_PARAMETER(uint32,TileAblation)
  SHADER_PARAMETER(uint32,TilePoolCapacity)
  SHADER_PARAMETER(uint32,TileRegionCapacity)
  SHADER_PARAMETER(uint32,TileRemovalCount)
  SHADER_PARAMETER(FVector3f,TileRemovalPreViewTranslation)
  SHADER_PARAMETER(FMatrix44f,TileRemovalClipToWorld)
  SHADER_PARAMETER_ARRAY(FVector4f,TileRemovalCenters,[8])
  SHADER_PARAMETER_ARRAY(FVector4f,TileRemovalAxisX,[8])
  SHADER_PARAMETER_ARRAY(FVector4f,TileRemovalAxisY,[8])
  SHADER_PARAMETER_ARRAY(FVector4f,TileRemovalAxisZ,[8])
  SHADER_PARAMETER_SRV(StructuredBuffer<TileViewData>,TileViewDataBuffer)
  SHADER_PARAMETER_SRV(StructuredBuffer<uint>,TileSortKeys)
  SHADER_PARAMETER_SRV(StructuredBuffer<uint>,TileDrawArgs)
  SHADER_PARAMETER_SRV(StructuredBuffer<float4>,TileFinalColors)
  SHADER_PARAMETER_TEXTURE(Texture2D,TileSceneDepth)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<TileRecord>,TileRecords)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileCounts)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileOffsets)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileCursors)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileEntries)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<TilePixel>,TilePixels)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileStats)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileRasterArgs)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint2>,TileBoundsLimits)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileMembershipMasks)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileFrozenDrawArgs)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileProfileData)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileDenseEntries)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint2>,TileDenseMeta)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileCooperativeQueue)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileCooperativeArgs)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>,TileCostTrace)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>,TileCostMeta)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint2>,TileCostDigest)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileRegionEntries)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>,TileRegionMeta)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileSortQueue)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileSortQueueArgs)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>,TileAuditMasks)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>,TileAuditPacked)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileAuditDigest)
  SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,TileExtendedMembership)
 END_SHADER_PARAMETER_STRUCT()
 static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){
  FPermutationDomain D(P.PermutationId);
  const int32 Stage=D.Get<FStage>();
  const bool Cooperative=D.Get<FCooperative>();
  const int32 Part=D.Get<FResolvePart>();
  if(Stage==18)return (Part==4||Part==5) && IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6) && !D.Get<FCounters>() && !D.Get<FProbe>() && !D.Get<FWideMask>() && !D.Get<FSortBucket>() && !D.Get<FDenseMerge>() && !D.Get<FBatchSkip>() && !Cooperative && !D.Get<FPoolCache>() && !D.Get<FPackedOutput>() && !D.Get<FResolveOrder>() && !D.Get<FSortQueue>() && !D.Get<FWaveMask>() && !D.Get<FWaveSort>() && !D.Get<FResolveBatch>() && !D.Get<FRegionList>();
  if(Part!=0 && (Stage!=6 || Part>3 || D.Get<FCounters>() || D.Get<FProbe>()!=1 || D.Get<FResolveBatch>() || D.Get<FRegionList>() || D.Get<FResolveOrder>()))return false;
  if(D.Get<FRegionList>() && ((Stage!=0 && Stage!=5 && Stage!=6) || D.Get<FCounters>() || D.Get<FProbe>()>1 || D.Get<FResolveBatch>()!=0 || D.Get<FResolveOrder>()!=0 || !IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6)))return false;
  if(D.Get<FResolveBatch>()!=0 && (Stage!=6 || D.Get<FCounters>() || D.Get<FProbe>()>1 || !D.Get<FWaveMask>() || !D.Get<FBatchSkip>()))return false;
  if(D.Get<FPackedOutput>() && ((Stage!=6 && Stage!=13) || D.Get<FCounters>() || D.Get<FProbe>()>1))return false;
  if(D.Get<FResolveOrder>()!=0 && Stage!=6)return false;
  if(D.Get<FSortQueue>() && Stage!=0 && Stage!=2 && Stage!=5)return false;
  if(D.Get<FWaveMask>() && (Stage!=6 || !IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6)))return false;
  if(Cooperative && (!IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6) || (Stage!=0 && Stage!=1 && Stage!=3 && Stage!=7 && Stage!=8 && Stage!=9 && (Stage<14 || Stage>16))))return false;
  if(Stage>=14 && Stage<=16 && !Cooperative)return false;
  if(D.Get<FWaveSort>() && (Stage!=5 || !IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6)))return false;
  if(D.Get<FPoolCache>() && Stage!=1 && Stage!=3 && Stage!=7 && Stage!=8 && Stage!=9 && Stage!=15 && Stage!=16)return false;
  if(!IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5))return false;
  const int32 Probe=D.Get<FProbe>();
  if(Stage==17){if(Probe!=7 && Probe!=8)return false;}
  else if(Probe>=7 && Probe!=9)return false;
  if(Probe!=0 && ((Stage!=6 && Stage!=17) || D.Get<FCounters>()))return false;
  if(D.Get<FSortBucket>()!=0 && Stage!=5)return false;
  if(D.Get<FBatchSkip>() && Stage!=6)return false;
  if(Stage==11||Stage==12)return D.Get<FDenseMerge>();
  return !D.Get<FDenseMerge>() || Stage==6;
 }
};
class FGSPCVRTileVS:public FGlobalShader {
 DECLARE_GLOBAL_SHADER(FGSPCVRTileVS);SHADER_USE_PARAMETER_STRUCT(FGSPCVRTileVS,FGlobalShader);
 BEGIN_SHADER_PARAMETER_STRUCT(FParameters,) END_SHADER_PARAMETER_STRUCT()
 static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
};
class FGSPCVRTilePS:public FGlobalShader {
 DECLARE_GLOBAL_SHADER(FGSPCVRTilePS);SHADER_USE_PARAMETER_STRUCT(FGSPCVRTilePS,FGlobalShader);
 class FPackedOutput:SHADER_PERMUTATION_BOOL("GS_TILE_PACKED_OUTPUT");
 using FPermutationDomain=TShaderPermutationDomain<FPackedOutput>;
 BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
  SHADER_PARAMETER(uint32,TileWidth)
  SHADER_PARAMETER(uint32,TileOriginX)
  SHADER_PARAMETER(uint32,TileOriginY)
  SHADER_PARAMETER_SRV(StructuredBuffer<TilePixel>,TileResolvedPixels)
  SHADER_PARAMETER_SRV(StructuredBuffer<uint>,TileOutputStats)
  SHADER_PARAMETER(uint32,TileEntryCapacity)
 END_SHADER_PARAMETER_STRUCT()
 static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
};
IMPLEMENT_GLOBAL_SHADER(FGSPCVRTileCS,"/Plugin/LCCCinematicGS/Private/PCVRTileComposite.usf","MainCS",SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGSPCVRTileVS,"/Plugin/LCCCinematicGS/Private/PCVRTileComposite.usf","TileVS",SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGSPCVRTilePS,"/Plugin/LCCCinematicGS/Private/PCVRTileComposite.usf","TilePS",SF_Pixel);

class FGSPCVRTileResources {
public:
 FBufferRHIRef Buffers[10];FUnorderedAccessViewRHIRef UAVs[10];FShaderResourceViewRHIRef PixelSRV,StatsSRV;
 uint32 Capacity=0,Width=0,Height=0,Entries=0;
 bool WideMask=false,DenseEnabled=false,Cooperative=false,PackedOutput=false;
 uint32 PixelStride=32;
 FBufferRHIRef CooperativeQueue,CooperativeArgs;FUnorderedAccessViewRHIRef CooperativeQueueUAV,CooperativeArgsUAV;
 uint32 DenseCapacity=0,PoolCapacity=0;
 FBufferRHIRef MembershipPool;FUnorderedAccessViewRHIRef MembershipPoolUAV;
 FBufferRHIRef CostScratch,CostTrace,CostMeta,CostDigest;FUnorderedAccessViewRHIRef CostScratchUAV,CostTraceUAV,CostMetaUAV,CostDigestUAV;
 TUniquePtr<FRHIGPUBufferReadback> CostMetaReadback,CostDigestReadback;bool CostNeedsCopy=false,CostPending=false;uint64 CostPair=0,CostSerial=0;int32 CostEye=0;
 FBufferRHIRef RegionEntries,RegionMeta,RegionScratch;FUnorderedAccessViewRHIRef RegionEntriesUAV,RegionMetaUAV,RegionScratchUAV;
 uint32 RegionWords=0;bool RegionUsed=false,RegionPending=false;uint64 RegionSerial=0,RegionPair=0;int32 RegionEye=0;
 TUniquePtr<FRHIGPUBufferReadback> RegionReadback;
 uint64 ResolveBatchPair=0;FBufferRHIRef BatchScratch;FUnorderedAccessViewRHIRef BatchScratchUAV;
 uint64 ResolveOrderPair=0;
 FBufferRHIRef SortQueueBuffer,SortQueueArgs;FUnorderedAccessViewRHIRef SortQueueUAV,SortQueueArgsUAV;
 TUniquePtr<FRHIGPUBufferReadback> SortQueueReadback;bool SortQueuePending=false,SortQueueUsed=false;uint64 SortQueueSerial=0;int32 SortQueueEye=0;
 FBufferRHIRef MaskABProof;FUnorderedAccessViewRHIRef MaskABProofUAV;
 TUniquePtr<FRHIGPUBufferReadback> MaskABReadback;
 bool MaskABNeedsCopy=false,MaskABPending=false;
 uint64 MaskABSerial=0,MaskABPair=0;int32 MaskABEye=0;
 FBufferRHIRef AuditMasks,AuditPacked,AuditDigest;
 FUnorderedAccessViewRHIRef AuditMasksUAV,AuditPackedUAV,AuditDigestUAV;
 uint64 AuditBytes=0;
 FBufferRHIRef DenseEntries,DenseMeta;FUnorderedAccessViewRHIRef DenseEntriesUAV,DenseMetaUAV;
 int32 ProfileMode=0;uint32 ProfileMetaOffset=0,ProfileWords=0;
 FBufferRHIRef ProfileBuffer,ScratchBuffer;FUnorderedAccessViewRHIRef ProfileUAV,ScratchUAV;
 TUniquePtr<FRHIGPUBufferReadback> ProfileReadback;bool ProfilePending=false;double LastProfileReadback=0;
 uint64 ProfilePendingSerial=0;int32 ProfilePendingEye=0;
 FBufferRHIRef FrozenDrawArgs;FShaderResourceViewRHIRef FrozenDrawSRV;FUnorderedAccessViewRHIRef FrozenDrawUAV;
 uint64 DispatchSerial=0,PendingSerial=0;int32 PendingEye=0,CurrentEye=0;bool Rejected=false;
 TUniquePtr<FRHIGPUBufferReadback> Readback,DrawReadback;bool Pending=false;double LastReadback=0;
 FGSPCVRTileResources(FRHICommandListImmediate& Cmd,uint32 N,uint32 W,uint32 H,int32 Mode,bool Wide,uint32 DenseBudget,uint32 PoolBudget,bool bCooperative,bool bPackedOutput):Capacity(N),Width(W),Height(H),WideMask(Wide),DenseEnabled(DenseBudget!=0),DenseCapacity(DenseBudget),PoolCapacity(PoolBudget),Cooperative(bCooperative),ProfileMode(Mode) {
  PackedOutput=bPackedOutput;PixelStride=PackedOutput?16u:32u;
  const uint32 Tiles=FMath::DivideAndRoundUp(W,16u)*FMath::DivideAndRoundUp(H,16u);
  Entries=FMath::Max(4000000u,N*8u);
  const uint32 Strides[10]={64,4,4,4,4,PixelStride,4,4,8,4},Counts[10]={N,Tiles,Tiles,Tiles,Entries,W*H,16,5,Tiles,N*(Wide?8u:2u)};
  uint64 Bytes=0;
  for(int32 i=0;i<10;++i){
   const FString Name=FString::Printf(TEXT("GSPCVRTile%d"),i);
   auto Desc=FRHIBufferCreateDesc::Create(*Name,Strides[i]*Counts[i],Strides[i],BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer|(i==7?BUF_DrawIndirect:BUF_None)).SetInitialState(ERHIAccess::UAVCompute);
   Buffers[i]=Cmd.CreateBuffer(Desc);UAVs[i]=Cmd.CreateUnorderedAccessView(Buffers[i],FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(Strides[i]));Bytes+=uint64(Strides[i])*Counts[i];
  }
  PixelSRV=Cmd.CreateShaderResourceView(Buffers[5],FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(PixelStride));
  StatsSRV=Cmd.CreateShaderResourceView(Buffers[6],FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));
  auto InputDesc=FRHIBufferCreateDesc::Create(TEXT("GSPCVRTileFrozenDraw"),20,4,BUF_ShaderResource|BUF_UnorderedAccess|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
  FrozenDrawArgs=Cmd.CreateBuffer(InputDesc);
  FrozenDrawUAV=Cmd.CreateUnorderedAccessView(FrozenDrawArgs,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));
  FrozenDrawSRV=Cmd.CreateShaderResourceView(FrozenDrawArgs,FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));
  if(Mode==2){
   ProfileMetaOffset=16+FMath::DivideAndRoundUp(N,256u)*4;ProfileWords=ProfileMetaOffset+Tiles*32;
   auto PD=FRHIBufferCreateDesc::Create(TEXT("GSTileDetailedCounters"),ProfileWords*4,4,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   ProfileBuffer=Cmd.CreateBuffer(PD);ProfileUAV=Cmd.CreateUnorderedAccessView(ProfileBuffer,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));
   ProfileReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("GSTileDetailedCounters"));Bytes+=uint64(ProfileWords)*4;
  }
  if(DenseEnabled){
   auto ED=FRHIBufferCreateDesc::Create(TEXT("GSTileDenseEntries"),DenseCapacity*4,4,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   DenseEntries=Cmd.CreateBuffer(ED);DenseEntriesUAV=Cmd.CreateUnorderedAccessView(DenseEntries,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));
   auto MD=FRHIBufferCreateDesc::Create(TEXT("GSTileDenseMeta"),Tiles*8,8,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   DenseMeta=Cmd.CreateBuffer(MD);DenseMetaUAV=Cmd.CreateUnorderedAccessView(DenseMeta,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(8));Bytes+=uint64(DenseCapacity)*4+uint64(Tiles)*8;
  }
  if(PoolCapacity){
   auto PD=FRHIBufferCreateDesc::Create(TEXT("GSTileSparseMembershipPool"),PoolCapacity*4,4,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   MembershipPool=Cmd.CreateBuffer(PD);MembershipPoolUAV=Cmd.CreateUnorderedAccessView(MembershipPool,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));Bytes+=uint64(PoolCapacity)*4;
  }
  if(Cooperative){
   auto QD=FRHIBufferCreateDesc::Create(TEXT("GSTileCooperativeQueue"),(N+1)*4,4,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   CooperativeQueue=Cmd.CreateBuffer(QD);CooperativeQueueUAV=Cmd.CreateUnorderedAccessView(CooperativeQueue,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));
   auto AD=FRHIBufferCreateDesc::Create(TEXT("GSTileCooperativeArgs"),12,4,BUF_UnorderedAccess|BUF_DrawIndirect|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   CooperativeArgs=Cmd.CreateBuffer(AD);CooperativeArgsUAV=Cmd.CreateUnorderedAccessView(CooperativeArgs,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));Bytes+=uint64(N+1)*4+12;
  }
  Readback=MakeUnique<FRHIGPUBufferReadback>(TEXT("GSPCVRTileStats"));
  DrawReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("GSPCVRTileExpectedDraw"));
  UE_LOG(LogTemp,Display,TEXT("[GSPCVR][TileAllocate] SourceCapacity=%u EyePixels=%ux%u EntryCapacity=%u Bytes=%llu"),N,W,H,Entries,Bytes);
  UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileConfig] Mode=%d CounterBytes=%u GSStride=256 PixelSamplesPerTile=1 ProductionFPS=%d"),Mode,ProfileWords*4,Mode==0?1:0);
  if(Mode>0)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileMemory] SourceRecordBytes=%llu EntryCapacityBytes=%llu PixelBufferBytes=%llu MembershipBytes=%llu TileIndexBytes=%llu BoundBytes=%llu FrozenArgsBytes=20 CounterBytes=%u"),uint64(N)*64,uint64(Entries)*4,uint64(W)*H*PixelStride,uint64(N)*(Wide?32:8),uint64(Tiles)*12,uint64(Tiles)*8,ProfileWords*4);
 }
};

bool DrawPCVRTileComposite(FRHICommandListImmediate& Cmd,const FSceneView& View,FGaussianGlobalAccumulator* A,FRHITexture* SceneDepth,int32 Quality,bool bTrim,FBufferRHIRef& RasterArgs) {
 if(CVarTile.GetValueOnRenderThread()==0 || !SceneDepth || !A || !A->PCVRFinalColorSRV.IsValid() || !A->GlobalDrawIndirectArgsBufferSRV.IsValid())return false;
 const auto* Fog=IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("gs.PCVR.PackedFog"));
 if(!Fog || Fog->GetValueOnRenderThread()!=2 || (Quality!=0 && Quality!=3))return false;
 const FIntRect Rect=View.UnscaledViewRect;const uint32 W=Rect.Width(),H=Rect.Height();
 if(!W || !H || uint64(W)*H>5000000 || !A->AllocatedCount || A->AllocatedCount>(FParse::Param(FCommandLine::Get(),TEXT("GSPCVRLODTest"))?4000000u:3000000u))return false;
 // Diagnostic scratch buffers retain the original 32-byte layout.
 const bool PackedOutput=CVarTilePackedOutput.GetValueOnRenderThread()!=0 && CVarTileProfile.GetValueOnRenderThread()==0 && CVarTileAblation.GetValueOnRenderThread()==0 && CVarTileProbeEvery.GetValueOnRenderThread()==0 && CVarResolveAuditEvery.GetValueOnRenderThread()==0 && CVarMaskABEvery.GetValueOnRenderThread()==0 && CVarResolveOrderEvery.GetValueOnRenderThread()==0 && CVarTileFrontToBack.GetValueOnRenderThread()!=0;
 FGSPCVRTilePS::FPermutationDomain PSPerm;PSPerm.Set<FGSPCVRTilePS::FPackedOutput>(PackedOutput);
 TShaderMapRef<FGSPCVRTileVS> VS(GetGlobalShaderMap(GMaxRHIFeatureLevel));TShaderMapRef<FGSPCVRTilePS> PS(GetGlobalShaderMap(GMaxRHIFeatureLevel),PSPerm);
 if(!VS.IsValid()||!PS.IsValid())return false;
 const int32 ProfileMode=FMath::Clamp(CVarTileProfile.GetValueOnRenderThread(),0,2);
 const uint32 PoolBudget=CVarTilePoolCache.GetValueOnRenderThread()!=0?uint32(FMath::Clamp(CVarTilePoolWords.GetValueOnRenderThread(),1,4194304)):0;
 const bool Cooperative=CVarTileCooperative.GetValueOnRenderThread()!=0 && GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6;
 const bool WaveSort=CVarTileWaveSort.GetValueOnRenderThread()!=0 && GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6;
 const bool Wide=CVarTileWideMask.GetValueOnRenderThread()!=0;
 const int32 Ablation=FMath::Clamp(CVarTileAblation.GetValueOnRenderThread(),0,5);
 const bool Adaptive=CVarTileAdaptiveSort.GetValueOnRenderThread()!=0;
 const uint32 DenseBudget=CVarTileParallelMerge.GetValueOnRenderThread()!=0?uint32(FMath::Clamp(CVarTileDenseCapacity.GetValueOnRenderThread(),8192,1048576)):0;
 if(!A->TileResources.IsValid() || A->TileResources->Capacity!=A->AllocatedCount || A->TileResources->Width!=W || A->TileResources->Height!=H || A->TileResources->ProfileMode!=ProfileMode || A->TileResources->WideMask!=Wide || A->TileResources->DenseCapacity!=DenseBudget || A->TileResources->PoolCapacity!=PoolBudget || A->TileResources->Cooperative!=Cooperative || A->TileResources->PackedOutput!=PackedOutput)
  A->TileResources=MakeShared<FGSPCVRTileResources>(Cmd,A->AllocatedCount,W,H,ProfileMode,Wide,DenseBudget,PoolBudget,Cooperative,PackedOutput);
 auto& R=*A->TileResources;
 TRACE_CPUPROFILER_EVENT_SCOPE(GSTileSubmit);
 if(R.Rejected)return false;
 SCOPED_DRAW_EVENTF(Cmd,GSPCVRTileComposite,TEXT("GSPCVR_TileComposite Eye=%d"),int32(View.StereoPass));
 R.CurrentEye=int32(View.StereoPass);
 const bool BatchCompatible=ProfileMode==0 && Ablation==0 && CVarTileProbeEvery.GetValueOnRenderThread()==0 && CVarResolveAuditEvery.GetValueOnRenderThread()==0 && CVarMaskABEvery.GetValueOnRenderThread()==0 && CVarResolveOrderEvery.GetValueOnRenderThread()==0 && CVarTileWaveMask.GetValueOnRenderThread()!=0 && CVarTileBatchSkip.GetValueOnRenderThread()!=0 && GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6;
 const int32 BatchMode=BatchCompatible?FMath::Clamp(CVarResolveBatch.GetValueOnRenderThread(),0,3):0;
 ++R.DispatchSerial;
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ResolveBatchConfig] Eye=%d Mode=%d ReferenceFallback=1 ExactCandidateOrder=1 EffectsUnchanged=1"),R.CurrentEye,BatchMode);
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][PackedOutputConfig] Eye=%d Enabled=%d PixelStride=%u PixelBytes=%llu ColorFogExactHalf=1 DepthFP32=1 FullGS=1 EffectsUnchanged=1"),R.CurrentEye,R.PackedOutput?1:0,R.PixelStride,uint64(W)*H*R.PixelStride);
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ResolveOrderConfig] Eye=%d Mode=%d ExactTilePermutation=1 CandidateOrderUnchanged=1 EffectsUnchanged=1"),R.CurrentEye,FMath::Clamp(CVarResolveOrder.GetValueOnRenderThread(),0,2));
 const bool RegionEnabled=(CVarRegionList.GetValueOnRenderThread()!=0 || CVarRegionListEvery.GetValueOnRenderThread()>0) && ProfileMode==0 && Ablation==0 && CVarTileSubtileCull.GetValueOnRenderThread()!=0 && CVarTileWaveMask.GetValueOnRenderThread()!=0 && CVarTileBatchSkip.GetValueOnRenderThread()!=0 && CVarResolveBatch.GetValueOnRenderThread()==0 && CVarResolveBatchEvery.GetValueOnRenderThread()==0 && CVarResolveOrder.GetValueOnRenderThread()==0 && CVarResolveOrderEvery.GetValueOnRenderThread()==0 && CVarTileProbeEvery.GetValueOnRenderThread()==0 && CVarResolveAuditEvery.GetValueOnRenderThread()==0 && CVarMaskABEvery.GetValueOnRenderThread()==0 && Adaptive && GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6;
 const uint32 RegionWords=uint32(FMath::Clamp(CVarRegionListWords.GetValueOnRenderThread(),4,67108864));
 if(RegionEnabled && (!R.RegionEntries.IsValid() || R.RegionWords!=RegionWords)){
  auto Make=[&](const TCHAR* Name,uint32 Count,uint32 Stride,FBufferRHIRef& B,FUnorderedAccessViewRHIRef& U){auto D=FRHIBufferCreateDesc::Create(Name,Count*Stride,Stride,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);B=Cmd.CreateBuffer(D);U=Cmd.CreateUnorderedAccessView(B,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(Stride));};
  R.RegionWords=RegionWords;Make(TEXT("GSRegionEntries"),RegionWords,4,R.RegionEntries,R.RegionEntriesUAV);Make(TEXT("GSRegionMeta"),FMath::DivideAndRoundUp(W,16u)*FMath::DivideAndRoundUp(H,16u),16,R.RegionMeta,R.RegionMetaUAV);
  R.RegionReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("GSRegionMeta"));R.RegionPending=false;R.RegionSerial=0;
 }
 R.RegionUsed=RegionEnabled;
 if(RegionEnabled){Cmd.Transition(FRHITransitionInfo(R.RegionEntries,ERHIAccess::Unknown,ERHIAccess::UAVCompute));Cmd.Transition(FRHITransitionInfo(R.RegionMeta,ERHIAccess::Unknown,ERHIAccess::UAVCompute));}
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][RegionListConfig] Eye=%d Enabled=%d Words=%u Bytes=%llu FullGS=1 ExactOrder=1 DenseFallback=1 EffectsUnchanged=1"),R.CurrentEye,RegionEnabled?1:0,RegionWords,uint64(RegionWords)*4);
 const uint32 SortTiles=FMath::DivideAndRoundUp(W,16u)*FMath::DivideAndRoundUp(H,16u);
 const bool SortQueue=CVarTileSortQueue.GetValueOnRenderThread()!=0 && Adaptive && ProfileMode==0 && Ablation==0 && SortTiles<=65535;
 R.SortQueueUsed=SortQueue;
 if(SortQueue && !R.SortQueueBuffer.IsValid()){
  auto MakeQueue=[&](const TCHAR* Name,uint32 Words,EBufferUsageFlags Extra,FBufferRHIRef& B,FUnorderedAccessViewRHIRef& U){
   auto D=FRHIBufferCreateDesc::Create(Name,Words*4,4,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer|Extra).SetInitialState(ERHIAccess::UAVCompute);
   B=Cmd.CreateBuffer(D);U=Cmd.CreateUnorderedAccessView(B,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));};
  MakeQueue(TEXT("GSTileSortQueue"),SortTiles*5,BUF_None,R.SortQueueBuffer,R.SortQueueUAV);
  MakeQueue(TEXT("GSTileSortQueueArgs"),15,BUF_DrawIndirect,R.SortQueueArgs,R.SortQueueArgsUAV);
  R.SortQueueReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("GSTileSortQueueArgsReadback"));
 }
 if(SortQueue){Cmd.Transition(FRHITransitionInfo(R.SortQueueBuffer,ERHIAccess::Unknown,ERHIAccess::UAVCompute));Cmd.Transition(FRHITransitionInfo(R.SortQueueArgs,ERHIAccess::Unknown,ERHIAccess::UAVCompute));}
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][SortQueueConfig] Eye=%d Enabled=%d Tiles=%u QueueBytes=%u PrefixReservation=1 CandidateOrderUnchanged=1 EffectsUnchanged=1"),R.CurrentEye,SortQueue?1:0,SortTiles,SortQueue?SortTiles*20+60:0);
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][WaveMaskConfig] Eye=%d Enabled=%d GroupMappingValidated=1 RuntimeFallback=1 WaveJobAllocation=1 ExtraGlobalBytes=0 FullGS=1 EffectsUnchanged=1"),R.CurrentEye,CVarTileWaveMask.GetValueOnRenderThread()!=0 && GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6);
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][OptimizeConfig] Eye=%d ParallelMerge=%d DenseCapacity=%u WideMask=%d AdaptiveSort=%d BatchSkip=%d MaskTiles=%u DefaultsReference=1"),R.CurrentEye,R.DenseEnabled?1:0,R.DenseCapacity,Wide?1:0,Adaptive?1:0,CVarTileBatchSkip.GetValueOnRenderThread()!=0,Wide?256u:64u);
 if(R.DenseEnabled){Cmd.Transition(FRHITransitionInfo(R.DenseEntries,ERHIAccess::Unknown,ERHIAccess::UAVCompute));Cmd.Transition(FRHITransitionInfo(R.DenseMeta,ERHIAccess::Unknown,ERHIAccess::UAVCompute));}
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ReuseConfig] Eye=%d PoolWords=%u PoolBytes=%llu WaveSort=%d EffectsUnchanged=1"),R.CurrentEye,R.PoolCapacity,uint64(R.PoolCapacity)*4,WaveSort?1:0);
 if(R.PoolCapacity)Cmd.Transition(FRHITransitionInfo(R.MembershipPool,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
 if(Cooperative){
  Cmd.Transition(FRHITransitionInfo(R.CooperativeQueue,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
  Cmd.Transition(FRHITransitionInfo(R.CooperativeArgs,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
 }
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][CooperativeConfig] Eye=%d Enabled=%d Threads=32 MinAreaExclusive=%u QueueCapacity=%u QueueBytes=%llu FullGS=1 ExactMembership=1"),R.CurrentEye,Cooperative?1:0,Wide?256u:64u,R.Capacity,uint64(R.Capacity+1)*4+12);
 // All GPU readback copies and CPU consumption live in the following RDG pass.
 Cmd.Transition(FRHITransitionInfo(A->GlobalDrawIndirectArgsBuffer,ERHIAccess::Unknown,ERHIAccess::SRVCompute));
 Cmd.Transition(FRHITransitionInfo(R.FrozenDrawArgs,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
 Cmd.Transition(FRHITransitionInfo(A->GlobalViewDataBuffer,ERHIAccess::Unknown,ERHIAccess::SRVCompute));
 Cmd.Transition(FRHITransitionInfo(A->GlobalSortKeysBuffer,ERHIAccess::Unknown,ERHIAccess::SRVCompute));
 Cmd.Transition(FRHITransitionInfo(A->PCVRFinalColorBuffer,ERHIAccess::Unknown,ERHIAccess::SRVCompute));
 Cmd.Transition(FRHITransitionInfo(SceneDepth,ERHIAccess::Unknown,ERHIAccess::DSVRead|ERHIAccess::SRVMask));
 for(auto& B:R.Buffers)Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
 FGSPCVRTileCS::FParameters P;FMemory::Memzero(P);P.TileCountOverride=0;P.TileSourceCapacity=A->AllocatedCount;P.TileWidth=W;P.TileHeight=H;P.TileOriginX=Rect.Min.X;P.TileOriginY=Rect.Min.Y;
 FGSRemovalBoxGPUData RemovalBoxes[GSGSRemovalBoxMax];
 P.TileRemovalCount=GetGSRemovalBoxes_RenderThread(View.Family?View.Family->Scene:nullptr,RemovalBoxes);
 P.TileRemovalPreViewTranslation=FVector3f(View.ViewMatrices.GetPreViewTranslation());
 P.TileRemovalClipToWorld=FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
 for(uint32 i=0;i<P.TileRemovalCount;++i){P.TileRemovalCenters[i]=RemovalBoxes[i].Center;P.TileRemovalAxisX[i]=RemovalBoxes[i].X;P.TileRemovalAxisY[i]=RemovalBoxes[i].Y;P.TileRemovalAxisZ[i]=RemovalBoxes[i].Z;}
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][AlphaCullConfig] Eye=%d Enabled=%d ConservativeChords=8 RadiusSquaredGuard=0.002 ExtraGlobalBytes=0 PixelFormulaUnchanged=1"),R.CurrentEye,CVarTileAlphaCull.GetValueOnRenderThread()!=0);
 if(R.DispatchSerial==1)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][RemovalConfig] Eye=%d Boxes=%u ExactSplatPlane=1 PixelCandidateClip=1 EffectsUnchanged=1"),R.CurrentEye,P.TileRemovalCount);
 P.TileCountX=FMath::DivideAndRoundUp(W,16u);P.TileCountY=FMath::DivideAndRoundUp(H,16u);P.TileEntryCapacity=R.Entries;P.TileQuality=Quality;P.TileTrim=bTrim?1:0;P.TileHalfRound=1;P.TileCandidateCull=CVarTileCandidateCull.GetValueOnRenderThread()!=0;P.TileAlphaCull=CVarTileAlphaCull.GetValueOnRenderThread()!=0;P.TileFrontToBack=CVarTileFrontToBack.GetValueOnRenderThread()!=0;P.TileEarlyThreshold=FMath::Clamp(CVarTileEarlyThreshold.GetValueOnRenderThread(),0.0f,0.001f);
 P.TileViewDataBuffer=A->GlobalViewDataBufferSRV;P.TileSortKeys=A->GlobalSortKeysBufferSRV;P.TileDrawArgs=A->GlobalDrawIndirectArgsBufferSRV;P.TileFrozenDrawArgs=R.FrozenDrawUAV;P.TileSubtileCull=CVarTileSubtileCull.GetValueOnRenderThread()!=0;P.TileFinalColors=A->PCVRFinalColorSRV;P.TileSceneDepth=SceneDepth;
 P.TileRecords=R.UAVs[0];P.TileCounts=R.UAVs[1];P.TileOffsets=R.UAVs[2];P.TileCursors=R.UAVs[3];P.TileEntries=R.UAVs[4];P.TilePixels=R.UAVs[5];P.TileStats=R.UAVs[6];P.TileRasterArgs=R.UAVs[7];P.TileBoundsLimits=R.UAVs[8];P.TileMembershipMasks=R.UAVs[9];P.TileReuseMask=CVarTileReuseMask.GetValueOnRenderThread()!=0;
 P.TileRegionCapacity=R.RegionWords;P.TileRegionEntries=R.RegionEntriesUAV;P.TileRegionMeta=R.RegionMetaUAV;
 P.TileSortQueue=R.SortQueueUAV;P.TileSortQueueArgs=R.SortQueueArgsUAV;
 P.TileCooperativeQueue=R.CooperativeQueueUAV;P.TileCooperativeArgs=R.CooperativeArgsUAV;
 P.TileProfileSplit=ProfileMode!=0;P.TileDensityClass=0;P.TileEmitClass=0;P.TileProfileSeed=uint32(R.DispatchSerial);P.TileProfileMetaOffset=R.ProfileMetaOffset;P.TileProfileWords=R.ProfileWords;P.TileProfileData=R.ProfileUAV;P.TilePoolCapacity=R.PoolCapacity;P.TileExtendedMembership=R.MembershipPoolUAV;P.TileAblation=uint32(Ablation);P.TileDenseCapacity=R.DenseCapacity;P.TileDenseEntries=R.DenseEntriesUAV;P.TileDenseMeta=R.DenseMetaUAV;
 if(ProfileMode==2)Cmd.Transition(FRHITransitionInfo(R.ProfileBuffer,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
 const uint32 Written[7][5]={{1,2,3,6,8},{0,1,9,10,10},{2,6,7,10,10},{3,4,10,10,10},{6,7,10,10,10},{4,6,7,8,10},{5,6,10,10,10}};

 auto DispatchRaw=[&](int32 Stage,bool Counters,int32 Probe,int32 Bucket=0,int32 MaskOverride=-1,int32 OrderOverride=-1,int32 BatchOverride=-1,int32 RegionOverride=-1,int32 CostPart=0){
  FGSPCVRTileCS::FPermutationDomain Perm;Perm.Set<FGSPCVRTileCS::FSortQueue>(SortQueue && (Stage==0||Stage==2||Stage==5));Perm.Set<FGSPCVRTileCS::FStage>(Stage);Perm.Set<FGSPCVRTileCS::FCounters>(Counters);Perm.Set<FGSPCVRTileCS::FProbe>(Probe);
  Perm.Set<FGSPCVRTileCS::FWideMask>(Wide);Perm.Set<FGSPCVRTileCS::FSortBucket>(Bucket);
  Perm.Set<FGSPCVRTileCS::FDenseMerge>((Stage==11||Stage==12) || (Stage==6 && R.DenseEnabled && Ablation!=5));Perm.Set<FGSPCVRTileCS::FBatchSkip>(Stage==6 && CVarTileBatchSkip.GetValueOnRenderThread()!=0);
  Perm.Set<FGSPCVRTileCS::FPoolCache>(R.PoolCapacity!=0 && (Stage==1||Stage==3||Stage==7||Stage==8||Stage==9||Stage==15||Stage==16));
  Perm.Set<FGSPCVRTileCS::FCooperative>(Cooperative && (Stage==0||Stage==1||Stage==3||Stage==7||Stage==8||Stage==9||(Stage>=14&&Stage<=16)));
  Perm.Set<FGSPCVRTileCS::FWaveSort>(WaveSort && Stage==5);
  Perm.Set<FGSPCVRTileCS::FWaveMask>(Stage==6 && (MaskOverride>=0?MaskOverride!=0:CVarTileWaveMask.GetValueOnRenderThread()!=0) && GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6);
  Perm.Set<FGSPCVRTileCS::FResolveOrder>(Stage==6?FMath::Clamp(OrderOverride>=0?OrderOverride:CVarResolveOrder.GetValueOnRenderThread(),0,2):0);
  Perm.Set<FGSPCVRTileCS::FPackedOutput>(R.PackedOutput && (Stage==6||Stage==13));
  const bool CanBatch=BatchCompatible && Stage==6 && !Counters && Probe<=1 && Perm.Get<FGSPCVRTileCS::FWaveMask>() && Perm.Get<FGSPCVRTileCS::FBatchSkip>();
  Perm.Set<FGSPCVRTileCS::FResolveBatch>(CanBatch?FMath::Clamp(BatchOverride>=0?BatchOverride:BatchMode,0,3):0);
  Perm.Set<FGSPCVRTileCS::FRegionList>(RegionEnabled && (Stage==0||Stage==5||Stage==6) && !Counters && Probe<=1 && (Stage==0 || (RegionOverride>=0?RegionOverride!=0:CVarRegionList.GetValueOnRenderThread()!=0)));
  Perm.Set<FGSPCVRTileCS::FResolvePart>(CostPart);
  if(Stage==18){Perm=FGSPCVRTileCS::FPermutationDomain();Perm.Set<FGSPCVRTileCS::FStage>(18);Perm.Set<FGSPCVRTileCS::FResolvePart>(CostPart);}
  TShaderMapRef<FGSPCVRTileCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel),Perm);
  SetComputePipelineState(Cmd,CS.GetComputeShader());SetShaderParameters(Cmd,CS,CS.GetComputeShader(),P);
  if(Stage==18)Cmd.DispatchComputeShader(FMath::DivideAndRoundUp(FMath::DivideAndRoundUp(P.TileCountX*P.TileCountY,16u),64u),1,1);
  else if(Stage==5 && SortQueue)Cmd.DispatchIndirectComputeShader(R.SortQueueArgs,(P.TileDensityClass-1)*12);
  else if(Stage==15||Stage==16)Cmd.DispatchIndirectComputeShader(R.CooperativeArgs,0);
  else if(Stage==14)Cmd.DispatchComputeShader(1,1,1);
  else if(Stage==0||Stage==4){uint32 Count=P.TileCountX*P.TileCountY;if(Stage==0 && Counters)Count=FMath::Max(Count,P.TileProfileMetaOffset);Cmd.DispatchComputeShader(FMath::DivideAndRoundUp(Count,256u),1,1);}
  else if(Stage==1||Stage==3||Stage==7||Stage==8||Stage==9)Cmd.DispatchComputeShader(FMath::DivideAndRoundUp(A->AllocatedCount,256u),1,1);
  else if(Stage==2||Stage==11)Cmd.DispatchComputeShader(1,1,1);
  else Cmd.DispatchComputeShader(P.TileCountX,P.TileCountY,1);
  UnsetShaderUAVs(Cmd,CS,CS.GetComputeShader());
  if(RegionEnabled && (Stage==0||Stage==5)){Cmd.Transition(FRHITransitionInfo(R.RegionEntries,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));Cmd.Transition(FRHITransitionInfo(R.RegionMeta,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));}
  if(SortQueue && Stage==0)Cmd.Transition(FRHITransitionInfo(R.SortQueueArgs,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
  if(SortQueue && Stage==2){Cmd.Transition(FRHITransitionInfo(R.SortQueueBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));Cmd.Transition(FRHITransitionInfo(R.SortQueueArgs,ERHIAccess::UAVCompute,ERHIAccess::IndirectArgs));}
  if(Stage==0){Cmd.Transition(FRHITransitionInfo(R.FrozenDrawArgs,ERHIAccess::UAVCompute,ERHIAccess::SRVCompute));Cmd.Transition(FRHITransitionInfo(A->GlobalDrawIndirectArgsBuffer,ERHIAccess::SRVCompute,ERHIAccess::IndirectArgs));P.TileDrawArgs=R.FrozenDrawSRV;}
  if(ProfileMode==0 && Stage<7){for(uint32 Index:Written[Stage])if(Index<10)Cmd.Transition(FRHITransitionInfo(R.Buffers[Index],ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));}
  else {for(auto& B:R.Buffers)Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));}
  if(Stage==11||Stage==12){Cmd.Transition(FRHITransitionInfo(R.DenseMeta,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));Cmd.Transition(FRHITransitionInfo(R.DenseEntries,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));}
  if(R.PoolCapacity && (Stage==1||Stage==8||Stage==15)){
   Cmd.Transition(FRHITransitionInfo(R.MembershipPool,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
   Cmd.Transition(FRHITransitionInfo(R.Buffers[6],ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
  }
  if(Cooperative && (Stage==0||Stage==1||Stage==8))Cmd.Transition(FRHITransitionInfo(R.CooperativeQueue,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
  if(Stage==14)Cmd.Transition(FRHITransitionInfo(R.CooperativeArgs,ERHIAccess::UAVCompute,ERHIAccess::IndirectArgs));
  if(Counters)Cmd.Transition(FRHITransitionInfo(R.ProfileBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
 };
 auto Dispatch=[&](int32 Stage,bool Counters,int32 Probe,int32 Bucket=0){
  DispatchRaw(Stage,Counters,Probe,Bucket);
  if(Cooperative && (Stage==1||Stage==8)){
   {SCOPED_DRAW_EVENT(Cmd,GSTile_CooperativeArgs);DispatchRaw(14,Counters,0);}
   {SCOPED_DRAW_EVENT(Cmd,GSTile_CooperativeRegister);DispatchRaw(15,Counters,0);}
  }
  if(Cooperative && (Stage==3||Stage==9)){
   SCOPED_DRAW_EVENT(Cmd,GSTile_CooperativeEmit);DispatchRaw(16,Counters,0);
  }
 };
 auto MergeDense=[&](bool Counts){if(R.DenseEnabled){
  {SCOPED_DRAW_EVENT(Cmd,GSTile_DensePrefix);Dispatch(11,Counts,0);}
  {SCOPED_DRAW_EVENT(Cmd,GSTile_DenseParallelMerge);Dispatch(12,Counts,0);}
 }};
 auto SortAdaptive=[&](bool Counts){
  P.TileDensityClass=1;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_1_32);Dispatch(5,Counts,0,1);}
  P.TileDensityClass=2;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_33_256);Dispatch(5,Counts,0,2);}
  P.TileDensityClass=3;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_257_1024);Dispatch(5,Counts,0,3);}
  P.TileDensityClass=4;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_1025_8192);Dispatch(5,Counts,0,4);}
  P.TileDensityClass=5;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_Dense);Dispatch(5,Counts,0,4);}P.TileDensityClass=0;
 };
 if(Ablation==2 || Ablation==4){
  {SCOPED_DRAW_EVENT(Cmd,GSTile_AblationClear);Dispatch(0,false,0);}
  {SCOPED_DRAW_EVENT(Cmd,GSTile_AblationEmpty);Dispatch(13,false,0);}
 }else if(ProfileMode==0){
  for(int32 Stage=0;Stage<7;++Stage){
   SCOPED_DRAW_EVENTF(Cmd,GSPCVRTileStage,TEXT("GSPCVR_TileStage=%d Eye=%d"),Stage,int32(View.StereoPass));
   if(Stage==5 && Ablation==5){SCOPED_DRAW_EVENT(Cmd,GSTile_AblationBoundsOnly);Dispatch(10,false,0);}
   else if(Stage==6 && Ablation==1){SCOPED_DRAW_EVENT(Cmd,GSTile_AblationEmpty);Dispatch(13,false,0);}
   else if(Stage==5 && Adaptive)SortAdaptive(false);else Dispatch(Stage,false,0);
   if(Stage==5 && Ablation!=5)MergeDense(false);
  }
 }else{
  const bool Counts=ProfileMode==2;
  {SCOPED_DRAW_EVENT(Cmd,GSTile_ClearFreeze);Dispatch(0,Counts,0);}
  {SCOPED_DRAW_EVENT(Cmd,GSTile_ProjectCull);Dispatch(7,Counts,0);}
  {SCOPED_DRAW_EVENT(Cmd,GSTile_RegisterTiles);Dispatch(8,Counts,0);}
  {SCOPED_DRAW_EVENT(Cmd,GSTile_Prefix);Dispatch(2,Counts,0);}
  P.TileEmitClass=1;{SCOPED_DRAW_EVENT(Cmd,GSTile_EmitCached);Dispatch(9,Counts,0);}
  P.TileEmitClass=2;{SCOPED_DRAW_EVENT(Cmd,GSTile_EmitRecheck);Dispatch(9,Counts,0);}P.TileEmitClass=0;
  {SCOPED_DRAW_EVENT(Cmd,GSTile_Validate);Dispatch(4,Counts,0);}
  P.TileDensityClass=1;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_1_32);Dispatch(5,Counts,0,Adaptive?1:0);}
  P.TileDensityClass=2;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_33_256);Dispatch(5,Counts,0,Adaptive?2:0);}
  P.TileDensityClass=3;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_257_1024);Dispatch(5,Counts,0,Adaptive?3:0);}
  P.TileDensityClass=4;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_1025_8192);Dispatch(5,Counts,0,Adaptive?4:0);}
  P.TileDensityClass=5;{SCOPED_DRAW_EVENT(Cmd,GSTile_Sort_Dense);Dispatch(5,Counts,0,Adaptive?4:0);}P.TileDensityClass=0;
  {SCOPED_DRAW_EVENT(Cmd,GSTile_HDRFogBounds);Dispatch(10,Counts,0);}
  MergeDense(Counts);
  P.TileDensityClass=6;{SCOPED_DRAW_EVENT(Cmd,GSTile_Resolve_Empty);Dispatch(6,Counts,0);}
  P.TileDensityClass=1;{SCOPED_DRAW_EVENT(Cmd,GSTile_Resolve_1_32);Dispatch(6,Counts,0);}
  P.TileDensityClass=2;{SCOPED_DRAW_EVENT(Cmd,GSTile_Resolve_33_256);Dispatch(6,Counts,0);}
  P.TileDensityClass=3;{SCOPED_DRAW_EVENT(Cmd,GSTile_Resolve_257_1024);Dispatch(6,Counts,0);}
  P.TileDensityClass=4;{SCOPED_DRAW_EVENT(Cmd,GSTile_Resolve_1025_8192);Dispatch(6,Counts,0);}
  P.TileDensityClass=5;{SCOPED_DRAW_EVENT(Cmd,GSTile_Resolve_Dense);Dispatch(6,Counts,0);}P.TileDensityClass=0;
  const int32 Every=CVarTileProbeEvery.GetValueOnRenderThread();
  if(Every>0 && R.DispatchSerial%uint64(FMath::Max(Every,30))==0){
   if(!R.ScratchBuffer.IsValid()){
    auto D=FRHIBufferCreateDesc::Create(TEXT("GSTileProbeScratch"),W*H*32,32,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
    R.ScratchBuffer=Cmd.CreateBuffer(D);R.ScratchUAV=Cmd.CreateUnorderedAccessView(R.ScratchBuffer,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(32));
   }
   Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::Unknown,ERHIAccess::UAVCompute));P.TilePixels=R.ScratchUAV;
   {SCOPED_DRAW_EVENT(Cmd,GSTile_ProbeReference);Dispatch(6,false,1);}
   Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
   {SCOPED_DRAW_EVENT(Cmd,GSTile_ProbeNoRGBFog);Dispatch(6,false,2);}
   Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
   const uint32 SavedSubtile=P.TileSubtileCull;P.TileSubtileCull=0;
   {SCOPED_DRAW_EVENT(Cmd,GSTile_ProbeNo8x8);Dispatch(6,false,1);}
   P.TileSubtileCull=SavedSubtile;P.TilePixels=R.UAVs[5];
   UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileProbe] Eye=%d Dispatch=%llu ScratchBytes=%llu ProductionFPS=0"),R.CurrentEye,R.DispatchSerial,uint64(W)*H*32);
  }
 }

 const int32 AuditInterval=CVarResolveAuditEvery.GetValueOnRenderThread();
 if(AuditInterval>0 && ProfileMode==0 && Ablation==0 && P.TileFrontToBack!=0 && P.TileSubtileCull!=0 &&
    CVarTileBatchSkip.GetValueOnRenderThread()!=0 && R.DispatchSerial%uint64(FMath::Max(AuditInterval,120))==0){
  const uint64 Pair=R.DispatchSerial/uint64(FMath::Max(AuditInterval,120));
  const uint32 Case=uint32((Pair-1)%3),Reverse=uint32(((Pair-1)/3)&1u);
  if(!R.ScratchBuffer.IsValid()){
   auto Make=[&](const TCHAR* Name,uint32 Count,uint32 Stride,FBufferRHIRef& B,FUnorderedAccessViewRHIRef& U){
    auto D=FRHIBufferCreateDesc::Create(Name,Count*Stride,Stride,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
    B=Cmd.CreateBuffer(D);U=Cmd.CreateUnorderedAccessView(B,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(Stride));R.AuditBytes+=uint64(Count)*Stride;
   };
   Make(TEXT("GSResolveAuditScratch32"),W*H,32,R.ScratchBuffer,R.ScratchUAV);
   // floor(offset/32)+tile gives nonoverlapping ceil(n/32) batch intervals.
   Make(TEXT("GSResolveAuditMasks"),(R.Entries/32+P.TileCountX*P.TileCountY+1)*2,16,R.AuditMasks,R.AuditMasksUAV);
   Make(TEXT("GSResolveAuditPacked16"),W*H,16,R.AuditPacked,R.AuditPackedUAV);
   Make(TEXT("GSResolveAuditDigest"),W*H,4,R.AuditDigest,R.AuditDigestUAV);
  }
  for(const auto& B:{R.ScratchBuffer,R.AuditMasks,R.AuditPacked,R.AuditDigest})Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
  P.TilePixels=R.ScratchUAV;P.TileAuditMasks=R.AuditMasksUAV;P.TileAuditPacked=R.AuditPackedUAV;P.TileAuditDigest=R.AuditDigestUAV;
  auto Ref=[&](){DispatchRaw(6,false,1);Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
  if(Case==0){
   {SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Region_PrepareMasks);DispatchRaw(6,false,3);}
   Cmd.Transition(FRHITransitionInfo(R.AuditMasks,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
   auto Reference=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Region_Reference);Ref();};
   auto Cached=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Region_CachedMasks);DispatchRaw(6,false,4);Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
   if(Reverse){Cached();Reference();}else{Reference();Cached();}
  }else if(Case==1){
   auto Reference=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Depth_Reference);Ref();};
   auto WithoutTail=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Depth_NoTail);DispatchRaw(6,false,5);Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
   if(Reverse){WithoutTail();Reference();}else{Reference();WithoutTail();}
  }else{
   auto Reference=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Storage_Write32);Ref();};
   auto Packed=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Storage_Write16);DispatchRaw(6,false,6);Cmd.Transition(FRHITransitionInfo(R.AuditPacked,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
   if(Reverse){Packed();Reference();}else{Reference();Packed();}
   auto Read32=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Storage_Read32);DispatchRaw(17,false,7);Cmd.Transition(FRHITransitionInfo(R.AuditDigest,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
   auto Read16=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveAudit_Storage_Read16);DispatchRaw(17,false,8);Cmd.Transition(FRHITransitionInfo(R.AuditDigest,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
   if(Reverse){Read16();Read32();}else{Read32();Read16();}
  }
  P.TilePixels=R.UAVs[5];
  UE_LOG(LogTemp,Display,TEXT("[GSResolveAudit] Eye=%d Dispatch=%llu Pair=%llu Case=%u Reverse=%u Interval=%d ScratchBytes=%llu SameInput=1 VisibleOutputPreserved=1 EffectsUnchanged=1 DiagnosticOnly=1 NonAdditive=1"),R.CurrentEye,R.DispatchSerial,Pair,Case,Reverse,FMath::Max(AuditInterval,120),R.AuditBytes);
 }

 const int32 MaskEvery=CVarMaskABEvery.GetValueOnRenderThread();
 if(MaskEvery>0 && ProfileMode==0 && Ablation==0 && AuditInterval==0 &&
    GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6 && !R.MaskABPending && !R.MaskABNeedsCopy &&
    R.DispatchSerial%uint64(FMath::Max(MaskEvery,120))==0){
  const uint32 Tiles=P.TileCountX*P.TileCountY;
  if(!R.ScratchBuffer.IsValid()){
   auto D=FRHIBufferCreateDesc::Create(TEXT("GSMaskABScratch"),W*H*32,32,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   R.ScratchBuffer=Cmd.CreateBuffer(D);R.ScratchUAV=Cmd.CreateUnorderedAccessView(R.ScratchBuffer,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(32));
  }
  if(!R.MaskABProof.IsValid()){
   auto D=FRHIBufferCreateDesc::Create(TEXT("GSMaskABProof"),Tiles*8+1024,4,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   R.MaskABProof=Cmd.CreateBuffer(D);R.MaskABProofUAV=Cmd.CreateUnorderedAccessView(R.MaskABProof,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(4));
   R.MaskABReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("GSMaskABProofReadback"));
  }
  const uint64 Pair=++R.MaskABPair;const uint32 Reverse=uint32((Pair-1)&1u);
  Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
  Cmd.Transition(FRHITransitionInfo(R.MaskABProof,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
  P.TilePixels=R.ScratchUAV;P.TileAuditDigest=R.MaskABProofUAV;
  auto One=[&](int32 Wave){DispatchRaw(6,false,9,0,Wave);
   Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));
   Cmd.Transition(FRHITransitionInfo(R.MaskABProof,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
  auto Reference=[&](){SCOPED_DRAW_EVENT(Cmd,GSMaskAB_Reference);One(0);};
  auto Wave=[&](){SCOPED_DRAW_EVENT(Cmd,GSMaskAB_Wave);One(1);};
  if(Reverse){Wave();Reference();}else{Reference();Wave();}
  P.TilePixels=R.UAVs[5];P.TileAuditDigest=nullptr;
  R.MaskABSerial=R.DispatchSerial;R.MaskABEye=R.CurrentEye;R.MaskABNeedsCopy=true;
  UE_LOG(LogTemp,Display,TEXT("[GSMaskAB][Pair] Eye=%d Dispatch=%llu Pair=%llu Reverse=%u Interval=%d SameInput=1 VisibleOutputPreserved=1 TimedProofStores=1 EffectsUnchanged=1 DiagnosticOnly=1"),R.CurrentEye,R.DispatchSerial,Pair,Reverse,FMath::Max(MaskEvery,120));
 }

 const int32 OrderEvery=CVarResolveOrderEvery.GetValueOnRenderThread();
 if(OrderEvery>0 && ProfileMode==0 && Ablation==0 && AuditInterval==0 && MaskEvery==0 &&
    R.DispatchSerial%uint64(FMath::Max(OrderEvery,120))==0){
  if(!R.ScratchBuffer.IsValid()){
   auto D=FRHIBufferCreateDesc::Create(TEXT("GSResolveOrderScratch"),W*H*32,32,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   R.ScratchBuffer=Cmd.CreateBuffer(D);R.ScratchUAV=Cmd.CreateUnorderedAccessView(R.ScratchBuffer,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(32));
  }
  uint64 Pair=++R.ResolveOrderPair;uint32 Mode=uint32((Pair-1)%2)+1,Reverse=uint32(((Pair-1)/2)&1u);
  Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::Unknown,ERHIAccess::UAVCompute));P.TilePixels=R.ScratchUAV;
  auto One=[&](int32 Order){DispatchRaw(6,false,1,0,-1,Order);Cmd.Transition(FRHITransitionInfo(R.ScratchBuffer,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
  auto Reference=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveOrder_Reference);One(0);};
  auto Tiled=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveOrder_Tiled);One(Mode);};
  if(Reverse){Tiled();Reference();}else{Reference();Tiled();}
  P.TilePixels=R.UAVs[5];
  UE_LOG(LogTemp,Display,TEXT("[GSResolveOrder][Pair] Eye=%d Dispatch=%llu Pair=%llu Mode=%u Reverse=%u SameInput=1 VisibleOutputPreserved=1 CandidateOrderUnchanged=1 DiagnosticOnly=1"),R.CurrentEye,R.DispatchSerial,Pair,Mode,Reverse);
 }

 const int32 BatchEvery=CVarResolveBatchEvery.GetValueOnRenderThread();
 if(BatchEvery>0 && ProfileMode==0 && Ablation==0 && AuditInterval==0 && MaskEvery==0 && OrderEvery==0 &&
    CVarTileWaveMask.GetValueOnRenderThread()!=0 && CVarTileBatchSkip.GetValueOnRenderThread()!=0 &&
    R.DispatchSerial%uint64(FMath::Max(BatchEvery,120))==0){
  if(!R.BatchScratch.IsValid()){
   auto D=FRHIBufferCreateDesc::Create(TEXT("GSResolveBatchScratch"),W*H*R.PixelStride,R.PixelStride,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   R.BatchScratch=Cmd.CreateBuffer(D);R.BatchScratchUAV=Cmd.CreateUnorderedAccessView(R.BatchScratch,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(R.PixelStride));
  }
  uint64 Pair=++R.ResolveBatchPair;uint32 TestMode=CVarResolveBatchTestMode.GetValueOnRenderThread()==3?3u:0u;uint32 Mode=TestMode?TestMode:uint32((Pair-1)%2)+1,Reverse=uint32((TestMode?(Pair-1):((Pair-1)/2))&1u);
  Cmd.Transition(FRHITransitionInfo(R.BatchScratch,ERHIAccess::Unknown,ERHIAccess::UAVCompute));P.TilePixels=R.BatchScratchUAV;
  auto One=[&](int32 ModeValue){DispatchRaw(6,false,1,0,-1,0,ModeValue);Cmd.Transition(FRHITransitionInfo(R.BatchScratch,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
  auto Reference=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveBatch_Reference);One(0);};
  auto Changed=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveBatch_Changed);One(Mode);};
  if(Reverse){Changed();Reference();}else{Reference();Changed();}
  P.TilePixels=R.UAVs[5];
  UE_LOG(LogTemp,Display,TEXT("[GSResolveBatch][Pair] Eye=%d Dispatch=%llu Pair=%llu Mode=%u Reverse=%u SameInput=1 VisibleOutputPreserved=1 CandidateOrderUnchanged=1 PixelStride=%u DiagnosticOnly=1"),R.CurrentEye,R.DispatchSerial,Pair,Mode,Reverse,R.PixelStride);
 }

 const int32 RegionEvery=CVarRegionListEvery.GetValueOnRenderThread();
 if(RegionEnabled && RegionEvery>0 && R.DispatchSerial%uint64(FMath::Max(RegionEvery,120))==0){
  if(!R.RegionScratch.IsValid()){
   auto D=FRHIBufferCreateDesc::Create(TEXT("GSRegionScratch"),W*H*R.PixelStride,R.PixelStride,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
   R.RegionScratch=Cmd.CreateBuffer(D);R.RegionScratchUAV=Cmd.CreateUnorderedAccessView(R.RegionScratch,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(R.PixelStride));
  }
  uint64 Pair=++R.RegionPair;uint32 Reverse=uint32((Pair-1)&1u);
  Cmd.Transition(FRHITransitionInfo(R.RegionScratch,ERHIAccess::Unknown,ERHIAccess::UAVCompute));P.TilePixels=R.RegionScratchUAV;
  auto SortOne=[&](int32 UseList){for(uint32 cls=1;cls<=5;++cls){P.TileDensityClass=cls;DispatchRaw(5,false,0,FMath::Min(cls,4u),-1,0,0,UseList);}P.TileDensityClass=0;};
  auto Reference=[&](){SCOPED_DRAW_EVENT(Cmd,GSRegion_Reference);
   {SCOPED_DRAW_EVENT(Cmd,GSRegion_ReferenceSort);SortOne(0);}
   {SCOPED_DRAW_EVENT(Cmd,GSRegion_ReferenceResolve);DispatchRaw(6,false,1,0,-1,0,0,0);}
   Cmd.Transition(FRHITransitionInfo(R.RegionScratch,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
  auto Changed=[&](){SCOPED_DRAW_EVENT(Cmd,GSRegion_Changed);
   {SCOPED_DRAW_EVENT(Cmd,GSRegion_ListSort);SortOne(1);}
   {SCOPED_DRAW_EVENT(Cmd,GSRegion_ListResolve);DispatchRaw(6,false,1,0,-1,0,0,1);}
   Cmd.Transition(FRHITransitionInfo(R.RegionScratch,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
  if(Reverse){Changed();Reference();}else{Reference();Changed();}
  // If reference ran last it did not alter list bytes; sorted slots/bounds are identical.
  P.TilePixels=R.UAVs[5];
  UE_LOG(LogTemp,Display,TEXT("[GSRegion][Pair] Eye=%d Dispatch=%llu Pair=%llu Mode=1 Reverse=%u SameInput=1 IncludesListBuild=1 SortedInputReplay=1 VisibleOutputPreserved=1 PixelStride=%u DiagnosticOnly=1"),R.CurrentEye,R.DispatchSerial,Pair,Reverse,R.PixelStride);
 }

 const int32 CostsEvery=CVarResolveCostsEvery.GetValueOnRenderThread();
 if(CostsEvery>0 && ProfileMode==0 && Ablation==0 && AuditInterval==0 && MaskEvery==0 && OrderEvery==0 && BatchEvery==0 && !RegionEnabled && CVarResolveBatch.GetValueOnRenderThread()==0 && CVarResolveOrder.GetValueOnRenderThread()==0 && CVarTileProbeEvery.GetValueOnRenderThread()==0 && P.TileFrontToBack!=0 && !R.CostPending && !R.CostNeedsCopy && R.DispatchSerial%uint64(FMath::Max(CostsEvery,120))==0){
  const uint32 Tiles=P.TileCountX*P.TileCountY,Selected=FMath::DivideAndRoundUp(Tiles,16u);
  if(!R.CostScratch.IsValid()){
   auto Make=[&](const TCHAR* Name,uint32 Count,uint32 Stride,FBufferRHIRef& B,FUnorderedAccessViewRHIRef& U){auto D=FRHIBufferCreateDesc::Create(Name,Count*Stride,Stride,BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);B=Cmd.CreateBuffer(D);U=Cmd.CreateUnorderedAccessView(B,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(Stride));};
   Make(TEXT("GSResolveCostsScratch"),W*H,R.PixelStride,R.CostScratch,R.CostScratchUAV);
   Make(TEXT("GSResolveCostsTrace"),Selected*1024,16,R.CostTrace,R.CostTraceUAV);
   Make(TEXT("GSResolveCostsMeta"),Selected,16,R.CostMeta,R.CostMetaUAV);
   Make(TEXT("GSResolveCostsDigest"),Tiles,8,R.CostDigest,R.CostDigestUAV);
   R.CostMetaReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("GSResolveCostsMeta"));R.CostDigestReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("GSResolveCostsDigest"));
  }
  const uint64 Pair=++R.CostPair;const uint32 Case=uint32((Pair-1)%3),Reverse=uint32(((Pair-1)/3)&1u),SavedSeed=P.TileProfileSeed,TraceSeed=uint32((Pair-1)/3);
  for(const auto& B:{R.CostScratch,R.CostTrace,R.CostMeta,R.CostDigest})Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
  P.TilePixels=R.CostScratchUAV;P.TileCostTrace=R.CostTraceUAV;P.TileCostMeta=R.CostMetaUAV;P.TileCostDigest=R.CostDigestUAV;
  auto One=[&](int32 Part){DispatchRaw(Part>=4?18:6,false,Part>=4?0:1,0,-1,0,0,0,Part);for(const auto& B:{R.CostScratch,R.CostTrace,R.CostMeta,R.CostDigest})Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::UAVCompute,ERHIAccess::UAVCompute));};
  if(Case<2){
   auto Ref=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveCosts_Reference);One(0);};
   if(Case==0){auto Variant=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveCosts_RecordsOnly);One(1);};if(Reverse){Variant();Ref();}else{Ref();Variant();}}
   else{auto Variant=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveCosts_NoRGBFog);One(2);};if(Reverse){Variant();Ref();}else{Ref();Variant();}}
  }else{
   // Step spatial selection independently of the 120-dispatch cadence.
   P.TileProfileSeed=TraceSeed;
   {SCOPED_DRAW_EVENT(Cmd,GSResolveCosts_TracePrepare);One(3);}
   auto Ref=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveCosts_GeometryReference);One(4);};
   auto Cached=[&](){SCOPED_DRAW_EVENT(Cmd,GSResolveCosts_GeometryCached);One(5);};
   if(Reverse){Cached();Ref();}else{Ref();Cached();}
   R.CostNeedsCopy=true;R.CostSerial=R.DispatchSerial;R.CostEye=R.CurrentEye;
  }
  P.TileProfileSeed=SavedSeed;P.TilePixels=R.UAVs[5];P.TileCostTrace=nullptr;P.TileCostMeta=nullptr;P.TileCostDigest=nullptr;
  UE_LOG(LogTemp,Display,TEXT("[GSResolveCosts][Pair] Eye=%d Dispatch=%llu Pair=%llu Case=%u Reverse=%u SameInput=1 VisibleOutputPreserved=1 SelectedTiles=%u PixelStride=%u TraceStride=16 MaxVisits=1024 SampledOnly=%d NonAdditive=1 DiagnosticOnly=1"),R.CurrentEye,R.DispatchSerial,Pair,Case,Reverse,Selected,R.PixelStride,Case==2?1:0);
 }

 Cmd.Transition(FRHITransitionInfo(R.Buffers[5],ERHIAccess::UAVCompute,ERHIAccess::SRVGraphics));
 Cmd.Transition(FRHITransitionInfo(R.Buffers[6],ERHIAccess::Unknown,ERHIAccess::SRVGraphics));
 Cmd.Transition(FRHITransitionInfo(R.Buffers[7],ERHIAccess::UAVCompute,ERHIAccess::IndirectArgs));
 RasterArgs=R.Buffers[7];
 FGraphicsPipelineStateInitializer G;Cmd.ApplyCachedRenderTargets(G);G.RasterizerState=TStaticRasterizerState<FM_Solid,CM_None>::GetRHI();G.DepthStencilState=TStaticDepthStencilState<false,CF_Always>::GetRHI();
 G.BlendState=TStaticBlendState<CW_RGBA,BO_Add,BF_One,BF_Zero,BO_Add,BF_One,BF_Zero,CW_RED,BO_Add,BF_One,BF_Zero,BO_Add,BF_One,BF_Zero,CW_RED,BO_Add,BF_One,BF_Zero,BO_Add,BF_One,BF_Zero>::GetRHI();
 G.PrimitiveType=PT_TriangleList;G.BoundShaderState.VertexDeclarationRHI=GEmptyVertexDeclaration.VertexDeclarationRHI;G.BoundShaderState.VertexShaderRHI=VS.GetVertexShader();G.BoundShaderState.PixelShaderRHI=PS.GetPixelShader();SetGraphicsPipelineState(Cmd,G,0);
 Cmd.SetViewport(Rect.Min.X,Rect.Min.Y,0,Rect.Max.X,Rect.Max.Y,1);
 FGSPCVRTilePS::FParameters Out;FMemory::Memzero(Out);Out.TileWidth=W;Out.TileOriginX=Rect.Min.X;Out.TileOriginY=Rect.Min.Y;Out.TileResolvedPixels=R.PixelSRV;Out.TileOutputStats=R.StatsSRV;Out.TileEntryCapacity=R.Entries;SetShaderParameters(Cmd,PS,PS.GetPixelShader(),Out);
 {SCOPED_DRAW_EVENT(Cmd,GSPCVRTileOutput);Cmd.SetStreamSource(0,nullptr,0);Cmd.DrawPrimitive(0,1,1);}
 return true;
}


// Existing GPU counters, eight bytes per periodic asynchronous sample.
// Buffer capture occurs in the raster work; actual copies occur outside it.
struct FPCVRLODProofState {
 FBufferRHIRef LODCount,VisibleCount;
 TUniquePtr<FRHIGPUBufferReadback> LODRead,VisibleRead;
 uint32 Original=0,Total=0,PendingOriginal=0,PendingTotal=0;
 uint64 Frame=0,PendingFrame=0;double Last=0;bool Pending=false;
};
static FPCVRLODProofState PCVRLODProof;
void CapturePCVRLODProof(FBufferRHIRef LOD,FBufferRHIRef Visible,uint32 Original,uint32 Total){
 auto& S=PCVRLODProof;S.LODCount=LOD;S.VisibleCount=Visible;S.Original=Original;S.Total=Total;S.Frame=GFrameNumberRenderThread;
}
static void DrainPCVRLODProof(FRHICommandListImmediate& Cmd){
 auto& S=PCVRLODProof;
 if(S.Pending && S.LODRead->IsReady() && S.VisibleRead->IsReady()){
  uint32 L=*static_cast<const uint32*>(S.LODRead->Lock(4)),V=*static_cast<const uint32*>(S.VisibleRead->Lock(4));
  S.LODRead->Unlock();S.VisibleRead->Unlock();S.Pending=false;
  UE_LOG(LogTemp,Display,TEXT("[GSPCVRLOD][Counts] SharedBothEyes=1 SelectedLODGS=%u CompactedGS=%u OriginalCapacity=%u TotalCapacity=%u Frame=%llu OutsideRasterPass=1 ExtraFullScan=0"),L,V,S.PendingOriginal,S.PendingTotal,S.PendingFrame);
 }
 double Now=FPlatformTime::Seconds();
 if(S.Pending || !S.LODCount.IsValid() || !S.VisibleCount.IsValid() || S.Frame!=GFrameNumberRenderThread || Now-S.Last<1)return;
 if(!S.LODRead){S.LODRead=MakeUnique<FRHIGPUBufferReadback>(TEXT("PCVRLODCount"));S.VisibleRead=MakeUnique<FRHIGPUBufferReadback>(TEXT("PCVRLODVisible"));}
 Cmd.Transition(FRHITransitionInfo(S.LODCount,ERHIAccess::Unknown,ERHIAccess::CopySrc));
 Cmd.Transition(FRHITransitionInfo(S.VisibleCount,ERHIAccess::Unknown,ERHIAccess::CopySrc));
 S.LODRead->EnqueueCopy(Cmd,S.LODCount,4);S.VisibleRead->EnqueueCopy(Cmd,S.VisibleCount,4);
 Cmd.Transition(FRHITransitionInfo(S.LODCount,ERHIAccess::CopySrc,ERHIAccess::SRVCompute));
 Cmd.Transition(FRHITransitionInfo(S.VisibleCount,ERHIAccess::CopySrc,ERHIAccess::SRVCompute));
 S.Last=Now;S.Pending=true;S.PendingOriginal=S.Original;S.PendingTotal=S.Total;S.PendingFrame=S.Frame;
}

BEGIN_SHADER_PARAMETER_STRUCT(FGSPCVRTileReadbackParameters,)
 RDG_TEXTURE_ACCESS(RenderedColor,ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

void EnqueuePCVRTileDiagnostics(FRDGBuilder& GraphBuilder,FGaussianGlobalAccumulator* A,FRDGTextureRef RenderedColor) {
 if(CVarTile.GetValueOnRenderThread()==0 || !A || !RenderedColor)return;
 auto* Params=GraphBuilder.AllocParameters<FGSPCVRTileReadbackParameters>();Params->RenderedColor=RenderedColor;
 // Texture dependency orders this copy-only pass after the owning eye's raster pass.
 GraphBuilder.AddPass(RDG_EVENT_NAME("GSPCVRTileReadback"),Params,ERDGPassFlags::Readback,
 [A](FRHICommandListImmediate& Cmd){
  check(Cmd.IsOutsideRenderPass());
  DrainPCVRLODProof(Cmd);
  if(!A->TileResources.IsValid())return;
  auto& R=*A->TileResources;const double Now=FPlatformTime::Seconds();
 if(R.CostPending && R.CostMetaReadback->IsReady() && R.CostDigestReadback->IsReady()){
  const uint32 Tiles=FMath::DivideAndRoundUp(R.Width,16u)*FMath::DivideAndRoundUp(R.Height,16u),Selected=FMath::DivideAndRoundUp(Tiles,16u);
  const uint32* M=static_cast<const uint32*>(R.CostMetaReadback->Lock(Selected*16));const uint32* D=static_cast<const uint32*>(R.CostDigestReadback->Lock(Selected*8));
  uint32 Nonempty=0,Truncated=0,Mismatch=0,Invalid=0;uint64 Stored=0,Visited=0,Geometry=0,Exp=0;
  for(uint32 i=0;i<Selected;++i){Stored+=M[i*4];Visited+=M[i*4+1];Geometry+=M[i*4+2];Exp+=M[i*4+3];Nonempty+=M[i*4]!=0;Truncated+=M[i*4+1]>1024;Invalid+=M[i*4]>1024 || M[i*4]>M[i*4+1] || M[i*4+2]>M[i*4+1] || M[i*4+3]>M[i*4+2];Mismatch+=D[i*2]!=D[i*2+1];}
  UE_LOG(LogTemp,Display,TEXT("[GSResolveCosts][SampleProof] Eye=%d Dispatch=%llu Nonempty=%u Stored=%llu Visited=%llu Geometry=%llu Exp=%llu TruncatedTiles=%u DigestMismatch=%u Invalid=%u OutsideRasterPass=1 SampledOnly=1"),R.CostEye,R.CostSerial,Nonempty,Stored,Visited,Geometry,Exp,Truncated,Mismatch,Invalid);
  if(Mismatch || Invalid)UE_LOG(LogTemp,Error,TEXT("[GSResolveCosts][InvalidProof] Reject sampled comparison."));
  R.CostMetaReadback->Unlock();R.CostDigestReadback->Unlock();R.CostPending=false;
 }
 if(R.CostNeedsCopy){
  const uint32 Tiles=FMath::DivideAndRoundUp(R.Width,16u)*FMath::DivideAndRoundUp(R.Height,16u),Selected=FMath::DivideAndRoundUp(Tiles,16u);
  for(const auto& B:{R.CostMeta,R.CostDigest})Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::UAVCompute,ERHIAccess::CopySrc));
  R.CostMetaReadback->EnqueueCopy(Cmd,R.CostMeta,Selected*16);R.CostDigestReadback->EnqueueCopy(Cmd,R.CostDigest,Selected*8);
  for(const auto& B:{R.CostMeta,R.CostDigest})Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::CopySrc,ERHIAccess::UAVCompute));
  R.CostNeedsCopy=false;R.CostPending=true;
 }

 if(R.RegionPending && R.RegionReadback->IsReady()){
  const uint32 Tiles=FMath::DivideAndRoundUp(R.Width,16u)*FMath::DivideAndRoundUp(R.Height,16u);
  const uint32* V=static_cast<const uint32*>(R.RegionReadback->Lock(Tiles*16));uint32 Enabled=0,Invalid=0;uint64 Members=0;
  for(uint32 t=0;t<Tiles;++t){if(V[t*4]&0x80000000u){++Enabled;for(uint32 q=0;q<4;++q){uint32 n=V[t*4+q]&(q==0?0x7fffffffu:0xffffffffu);Members+=n;Invalid+=n>8192;}}}
  UE_LOG(LogTemp,Display,TEXT("[GSPCVR][RegionListStats] Eye=%d Dispatch=%llu EnabledTiles=%u RegionMembers=%llu Invalid=%u Words=%u OutsideRasterPass=1"),R.RegionEye,R.RegionSerial,Enabled,Members,Invalid,R.RegionWords);
  if(Invalid)UE_LOG(LogTemp,Error,TEXT("[GSPCVR][RegionListInvalid] Reject run."));
  R.RegionReadback->Unlock();R.RegionPending=false;
 }
 if(R.RegionUsed && !R.RegionPending && (!R.RegionSerial || R.DispatchSerial-R.RegionSerial>=120)){
  const uint32 Tiles=FMath::DivideAndRoundUp(R.Width,16u)*FMath::DivideAndRoundUp(R.Height,16u);
  Cmd.Transition(FRHITransitionInfo(R.RegionMeta,ERHIAccess::UAVCompute,ERHIAccess::CopySrc));R.RegionReadback->EnqueueCopy(Cmd,R.RegionMeta,Tiles*16);
  Cmd.Transition(FRHITransitionInfo(R.RegionMeta,ERHIAccess::CopySrc,ERHIAccess::UAVCompute));R.RegionPending=true;R.RegionSerial=R.DispatchSerial;R.RegionEye=R.CurrentEye;
 }

 if(R.SortQueuePending && R.SortQueueReadback->IsReady()){
  const uint32* Q=static_cast<const uint32*>(R.SortQueueReadback->Lock(60));const uint32 Tiles=FMath::DivideAndRoundUp(R.Width,16u)*FMath::DivideAndRoundUp(R.Height,16u);
  const uint32 Total=Q[0]+Q[3]+Q[6]+Q[9]+Q[12];bool Valid=Total<=Tiles;
  for(uint32 c=0;c<5;++c)Valid=Valid && Q[c*3]<=Tiles && Q[c*3+1]==1 && Q[c*3+2]==1;
  UE_LOG(LogTemp,Display,TEXT("[GSPCVR][SortQueueStats] Eye=%d Dispatch=%llu Classes=%u,%u,%u,%u,%u TotalGroups=%u FullGridGroups=%u Valid=%d OutsideRasterPass=1"),R.SortQueueEye,R.SortQueueSerial,Q[0],Q[3],Q[6],Q[9],Q[12],Total,Tiles*5,Valid?1:0);
  if(!Valid)UE_LOG(LogTemp,Error,TEXT("[GSPCVR][SortQueueInvalid] Reject result."));
  R.SortQueueReadback->Unlock();R.SortQueuePending=false;
 }
 if(R.SortQueueUsed && !R.SortQueuePending && (!R.SortQueueSerial || R.DispatchSerial-R.SortQueueSerial>=120)){
  Cmd.Transition(FRHITransitionInfo(R.SortQueueArgs,ERHIAccess::IndirectArgs,ERHIAccess::CopySrc));R.SortQueueReadback->EnqueueCopy(Cmd,R.SortQueueArgs,60);
  Cmd.Transition(FRHITransitionInfo(R.SortQueueArgs,ERHIAccess::CopySrc,ERHIAccess::IndirectArgs));R.SortQueuePending=true;R.SortQueueSerial=R.DispatchSerial;R.SortQueueEye=R.CurrentEye;
 }
 if(R.MaskABPending && R.MaskABReadback->IsReady()){
  const uint32 Tiles=FMath::DivideAndRoundUp(R.Width,16u)*FMath::DivideAndRoundUp(R.Height,16u);
  const uint32* V=static_cast<const uint32*>(R.MaskABReadback->Lock(Tiles*8+1024));
  uint32 Fast=0,Fallback=0,Empty=0,Reference=0,Mismatch=0;
  uint32 SizeFail=0,LayoutFail=0,Forced=0,Wave32=0,OtherWave=0;uint64 MismatchLanes=0;
  for(uint32 t=0;t<Tiles;++t){const uint32 Raw=V[t],W=Raw&3u,B=V[t+Tiles];
   if(W!=0){uint32 Reasons=(Raw>>2)&7u,Size=(Raw>>8)&255u;
    SizeFail+=(Reasons&1)!=0;LayoutFail+=(Reasons&2)!=0;Forced+=(Reasons&4)!=0;Wave32+=Size==32;OtherWave+=Size!=32;MismatchLanes+=(Raw>>16)&511u;
   }
   Fast+=W==1;Fallback+=W==2;Empty+=W==0;Reference+=B==3;
   Mismatch+=(W>2 || (B!=0 && B!=3) || ((W!=0)!=(B==3)))?1:0;
  }
  UE_LOG(LogTemp,Display,TEXT("[GSMaskAB][GPUChoice] Eye=%d Dispatch=%llu Pair=%llu FastTiles=%u FallbackTiles=%u EmptyTiles=%u ReferenceTiles=%u MismatchTiles=%u TotalTiles=%u OutsideRasterPass=1 TimedProofStores=1"),R.MaskABEye,R.MaskABSerial,R.MaskABPair,Fast,Fallback,Empty,Reference,Mismatch,Tiles);
  UE_LOG(LogTemp,Display,TEXT("[GSMaskAB][Cause] Eye=%d Dispatch=%llu Pair=%llu Wave32Tiles=%u OtherWaveTiles=%u SizeFailTiles=%u AssignmentFailTiles=%u ForcedTiles=%u MismatchLanes=%llu WaveJobAllocation=1 TimedProofStores=1"),R.MaskABEye,R.MaskABSerial,R.MaskABPair,Wave32,OtherWave,SizeFail,LayoutFail,Forced,MismatchLanes);
  FString Layout;for(uint32 i=0;i<32;++i){uint32 Raw=V[Tiles*2+i];Layout+=FString::Printf(TEXT("%s%u:%u:%u"),i?TEXT(","):TEXT(""),Raw&255u,(Raw>>8)&255u,Raw>>16);}
  UE_LOG(LogTemp,Display,TEXT("[GSMaskAB][LaneLayout] Eye=%d Dispatch=%llu Pair=%llu GroupLane:WaveLane:WaveSize=%s"),R.MaskABEye,R.MaskABSerial,R.MaskABPair,*Layout);
  if(Mismatch)UE_LOG(LogTemp,Error,TEXT("[GSMaskAB][InvalidProof] Reject paired result."));
  R.MaskABReadback->Unlock();R.MaskABPending=false;
 }
 if(R.MaskABNeedsCopy){
  const uint32 Tiles=FMath::DivideAndRoundUp(R.Width,16u)*FMath::DivideAndRoundUp(R.Height,16u);
  Cmd.Transition(FRHITransitionInfo(R.MaskABProof,ERHIAccess::Unknown,ERHIAccess::CopySrc));
  R.MaskABReadback->EnqueueCopy(Cmd,R.MaskABProof,Tiles*8+1024);
  Cmd.Transition(FRHITransitionInfo(R.MaskABProof,ERHIAccess::CopySrc,ERHIAccess::UAVCompute));
  R.MaskABNeedsCopy=false;R.MaskABPending=true;
 }

 if(R.Pending && R.Readback->IsReady() && R.DrawReadback->IsReady()){
  const uint32* S=static_cast<const uint32*>(R.Readback->Lock(64));
  const uint32* D=static_cast<const uint32*>(R.DrawReadback->Lock(20));
  UE_LOG(LogTemp,Display,TEXT("[GSPCVR][TileStats] Eye=%d Entries=%u OverflowTiles=%u MaxPerTile=%u ActiveGS=%u ExpectedDrawGS=%u CountMatch=%d SourceCapacity=%u HalfRound=1 GSReduction=0 Rect=%ux%u DenseTiles=%u FullScan=0 CandidateCull=%d FrontToBack=%d EarlyThreshold=%.6f TerminatedPixels=%u SubtileCull=%d GuardFlags=%u SortFaultFlags=%u Dispatch=%llu"),R.PendingEye,S[0],S[1],S[2],S[3],D[1],S[3]==D[1]?1:0,R.Capacity,R.Width,R.Height,S[4],CVarTileCandidateCull.GetValueOnRenderThread(),CVarTileFrontToBack.GetValueOnRenderThread(),CVarTileEarlyThreshold.GetValueOnRenderThread(),S[5],CVarTileSubtileCull.GetValueOnRenderThread(),S[6],S[7],R.PendingSerial);
  UE_LOG(LogTemp,Display,TEXT("[GSPCVR][Counts] Eye=%d SourceLimit=%u Sorted=%u Drawn=%u SubmittedFrame=%llu DelayedReadback=1 OutsideRasterPass=1"),R.PendingEye,R.Capacity,S[3],D[1],R.PendingSerial);
  UE_LOG(LogTemp,Display,TEXT("[GSPCVR][TileOptimize] Eye=%d Dispatch=%llu DenseMergedTiles=%u DenseMergedEntries=%u DenseFallbackTiles=%u"),R.PendingEye,R.PendingSerial,S[8],S[9],S[10]);
  UE_LOG(LogTemp,Display,TEXT("[GSPCVR][MembershipPool] Eye=%d Dispatch=%llu WordsRequested=%u CachedGS=%u OverflowGS=%u CachedEntries=%u CapacityWords=%u"),R.PendingEye,R.PendingSerial,S[11],S[12],S[13],S[14],R.PoolCapacity);
  if(R.Cooperative)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][CooperativeStats] Eye=%d Dispatch=%llu QueuedGS=%u SourceCapacity=%u ExactMembership=1"),R.PendingEye,R.PendingSerial,S[15],R.Capacity);
  if(S[3]!=D[1])UE_LOG(LogTemp,Error,TEXT("[GSPCVR][TileCountMismatch] Invalid optimization result; shader count differs from original draw args."));
  if(S[6] || S[7])UE_LOG(LogTemp,Error,TEXT("[GSPCVR][TileGuardFallback] GuardFlags=%u SortFaultFlags=%u: original full raster used; result not eligible for optimization adoption."),S[6],S[7]);
  if(S[1])UE_LOG(LogTemp,Warning,TEXT("[GSPCVR][TileOverflow] Entry capacity exceeded: original raster route for this eye. No per-pixel full scan."));
  if(S[3]!=D[1] || S[6]!=0 || S[7]!=0)R.Rejected=true;
  R.Readback->Unlock();R.DrawReadback->Unlock();R.Pending=false;
 }


  if(R.ProfilePending && R.ProfileReadback->IsReady()){
   TRACE_CPUPROFILER_EVENT_SCOPE(GSTileProfileReadback);
   const uint32* V=static_cast<const uint32*>(R.ProfileReadback->Lock(R.ProfileWords*4));
   if(V[0]==1 && V[8]==R.ProfileMetaOffset && V[9]==R.ProfileWords){
    uint64 Status[8]={},GSArea=0,GSHits=0,CacheGS=0,ClassTiles[6]={},ClassEntries[6]={},Meta[6]={},Pix[17]={};uint64 Comparisons=0,Padding=0,Barriers=0,CachedEntries=0,RecheckEntries=0;uint32 Maximum=0;double CoverageSum=0;uint64 BatchSkippedCandidates=0,BatchSkippedGroups=0;
    for(uint32 slot=V[1]&255;slot<V[5];slot+=256){const uint32* G=V+16+(slot>>8)*4;Status[FMath::Min(G[0],7u)]++;GSArea+=G[1];GSHits+=G[2];CacheGS+=G[3];}
    for(uint32 tile=0;tile<V[7];++tile){const uint32* T=V+V[8]+tile*32;uint32 N=T[0];int32 C=N==0?5:N<=32?0:N<=256?1:N<=1024?2:N<=8192?3:4;ClassTiles[C]++;ClassEntries[C]+=N;Maximum=FMath::Max(Maximum,N);
     float SampleAlpha=0;FMemory::Memcpy(&SampleAlpha,T+24,4);CoverageSum+=double(SampleAlpha)*T[7];
     BatchSkippedCandidates+=uint64(T[27])*T[7];BatchSkippedGroups+=uint64(T[28])*T[7];CachedEntries+=T[25];RecheckEntries+=T[26];for(int32 j=0;j<6;++j)Meta[j]+=T[j];for(int32 j=0;j<16;++j)Pix[j]+=uint64(T[8+j])*T[7];
     for(uint32 start=0;start<N;start+=8192){uint32 Count=FMath::Min(8192u,N-start),Size=1,L=0;while(Size<Count){Size<<=1;++L;}uint64 Steps=uint64(L)*(L+1)/2;Comparisons+=uint64(Size/2)*Steps;Padding+=Size-Count;Barriers+=Steps+2;}
    }
    UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileGS] Eye=%d Dispatch=%llu Seed=%u Input=%u SampleStride=256 Status=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu SampleBoundsTests=%llu SampleAccepted=%llu SampleCacheGS=%llu"),R.ProfilePendingEye,R.ProfilePendingSerial,V[1],V[5],Status[0],Status[1],Status[2],Status[3],Status[4],Status[5],Status[6],Status[7],GSArea,GSHits,CacheGS);
    for(int32 C=0;C<6;++C)UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileDensity] Eye=%d Dispatch=%llu Class=%d Tiles=%llu Entries=%llu"),R.ProfilePendingEye,R.ProfilePendingSerial,C+1,ClassTiles[C],ClassEntries[C]);
    UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileCoverageEstimate] Eye=%d Dispatch=%llu MeanAlpha=%.8f"),R.ProfilePendingEye,R.ProfilePendingSerial,CoverageSum/(double(V[2])*V[3]));
    UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileTile] Eye=%d Dispatch=%llu Entries=%llu Runs=%llu Batches=%llu MergeHeadReads=%llu Rect8Tests=%llu MaskHits=%llu Max=%u BitonicComparisons=%llu BitonicPadding=%llu BitonicGroupBarriers=%llu EstimatedRecordLoadBytes=%llu EstimatedMergeIndexBytes=%llu"),R.ProfilePendingEye,R.ProfilePendingSerial,Meta[0],Meta[1],Meta[2],Meta[3],Meta[4],Meta[5],Maximum,Comparisons,Padding,Barriers,Meta[0]*64,Meta[3]*4);
    UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfilePixelEstimate] Eye=%d Dispatch=%llu Samples=%u PixelCount=%llu Raw=%llu After8=%llu Batches=%llu Visited=%llu Invalid=%llu TermDepthSkip=%llu SceneTests=%llu SceneReject=%llu EllipseTests=%llu EllipseReject=%llu Exp=%llu AlphaReject=%llu Accepted=%llu DepthOnly=%llu Blends=%llu Terminated=%llu"),R.ProfilePendingEye,R.ProfilePendingSerial,V[7],uint64(V[2])*V[3],Pix[0],Pix[1],Pix[2],Pix[3],Pix[4],Pix[5],Pix[6],Pix[7],Pix[8],Pix[9],Pix[10],Pix[11],Pix[12],Pix[13],Pix[14],Pix[15]);
    UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileEmission] Eye=%d Dispatch=%llu CachedEntries=%llu RecheckEntries=%llu"),R.ProfilePendingEye,R.ProfilePendingSerial,CachedEntries,RecheckEntries);
    UE_LOG(LogTemp,Display,TEXT("[GSPCVR][ProfileBatchSkipEstimate] Eye=%d Dispatch=%llu Candidates=%llu Batches=%llu"),R.ProfilePendingEye,R.ProfilePendingSerial,BatchSkippedCandidates,BatchSkippedGroups);
   }else UE_LOG(LogTemp,Error,TEXT("[GSPCVR][ProfileInvalid] Header/layout mismatch; reject detailed counters."));
   R.ProfileReadback->Unlock();R.ProfilePending=false;
  }
  if(R.ProfileMode==2 && !R.Rejected && !R.ProfilePending && Now-R.LastProfileReadback>=5){
   Cmd.Transition(FRHITransitionInfo(R.ProfileBuffer,ERHIAccess::Unknown,ERHIAccess::CopySrc));R.ProfileReadback->EnqueueCopy(Cmd,R.ProfileBuffer,R.ProfileWords*4);
   Cmd.Transition(FRHITransitionInfo(R.ProfileBuffer,ERHIAccess::CopySrc,ERHIAccess::UAVCompute));
   R.ProfilePending=true;R.LastProfileReadback=Now;R.ProfilePendingSerial=R.DispatchSerial;R.ProfilePendingEye=R.CurrentEye;
  }
  if(R.Rejected || R.Pending || Now-R.LastReadback<1)return;
  Cmd.Transition(FRHITransitionInfo(R.Buffers[6],ERHIAccess::Unknown,ERHIAccess::CopySrc));
  Cmd.Transition(FRHITransitionInfo(A->GlobalDrawIndirectArgsBuffer,ERHIAccess::Unknown,ERHIAccess::CopySrc));
  R.Readback->EnqueueCopy(Cmd,R.Buffers[6],64);
  R.DrawReadback->EnqueueCopy(Cmd,A->GlobalDrawIndirectArgsBuffer,20);
  Cmd.Transition(FRHITransitionInfo(R.Buffers[6],ERHIAccess::CopySrc,ERHIAccess::SRVGraphics));
  Cmd.Transition(FRHITransitionInfo(A->GlobalDrawIndirectArgsBuffer,ERHIAccess::CopySrc,ERHIAccess::IndirectArgs));
  R.Pending=true;R.LastReadback=Now;R.PendingSerial=R.DispatchSerial;R.PendingEye=R.CurrentEye;
 });
}
