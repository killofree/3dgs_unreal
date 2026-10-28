// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class FGaussianSplatSceneProxy;
class FRDGTexture;
class FSceneInterface;
struct FPostProcessMaterialInputs;

/**
 * Scene View Extension for Gaussian Splatting
 * Manages registration of Gaussian Splat scene proxies.
 * Actual rendering is handled by PostOpaqueRenderDelegate in FLCCCinematicGSModule.
 */
class LCCCINEMATICGS_API FGaussianSplatViewExtension : public FSceneViewExtensionBase
{
public:
	FGaussianSplatViewExtension(const FAutoRegister& AutoRegister);
	virtual ~FGaussianSplatViewExtension();

	//~ Begin ISceneViewExtension Interface
	//Different render pipeline timing for hooking
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;

	virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override;
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

	virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs) override;
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
	//~ End ISceneViewExtension Interface

	/** Register a scene proxy for rendering */
	void RegisterProxy(FGaussianSplatSceneProxy* Proxy);

	/** Unregister a scene proxy */
	void UnregisterProxy(FGaussianSplatSceneProxy* Proxy);

	/** Get the singleton instance */
	static FGaussianSplatViewExtension* Get();

	/** Get registered proxies for external rendering */
	void GetRegisteredProxies(TArray<FGaussianSplatSceneProxy*>& OutProxies) const;

	/** Latest actual non-SceneCapture view rendered for a scene. Used only by weather Niagara. */
	bool GetLatestRenderedViewLocation(const FSceneInterface* Scene, FVector& OutLocation) const;

	/** Rain-only composite ordering. Non-Rain keeps the original MotionBlur hook. */
	static void SetRainNiagaraCompositeVisible(bool bEnabled);

	/** v2896: publish whether the actor-owned baked snow Gaussian layer is actually visible.
	 *  This is independent of the legacy direct-snow overlay texture path, which is
	 *  intentionally disabled for baked snow relief. The render thread uses this only
	 *  to apply panorama GPU back-pressure before a heavy snow ViewFamily begins. */
	static void SetBakedSnowGaussianVisible(bool bEnabled);
	void SetPendingComposite_RenderThread(
		const FRDGBuilder* OwningGraphBuilder,
		const FSceneView* View,
		FRDGTexture* IntermediateTexture,
		FRDGTexture* IntermediateDepthTexture,
		FRDGTexture* IntermediateNormalTexture,
		FRDGTexture* IntermediateFogDepthTexture,
		FRDGTexture* SnowIntermediateTexture,
		FRDGTexture* SnowIntermediateDepthTexture,
		FRDGTexture* SnowOcclusionDepthTexture,
		const FIntRect& ViewRect,
		const FVector4f& CompositeLookParams0,
		const FVector4f& CompositeLookParams1,
		const FVector4f& CompositeLookParams2,
		const FVector4f& CompositeLookParams3,
		const FVector4f& CompositeLookParams4,
		const FVector4f& CompositeBloomParams,
		const FVector4f& CompositeColorBloomParams,
		const FVector4f& CompositeBloomTint,
		float CompositeBloomSoftKnee, int32 VRDiagnosticMode = 0);
    FScreenPassTexture PostProcessPassBeforeTonemap_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);
	FScreenPassTexture PostProcessPassAfterTonemap_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);
	FScreenPassTexture CompositePending_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs, bool bAfterTonemap);

	/** Get proxy lock for thread-safe access */
	FCriticalSection& GetProxyLock() const { return ProxyLock; }

private:
	/** Registered scene proxies */
	TArray<FGaussianSplatSceneProxy*> RegisteredProxies;

	/** Critical section for thread-safe proxy access */
	mutable FCriticalSection ProxyLock;

	/** Weather-Niagara-only cache of the actual rendered camera position. */
	mutable FCriticalSection ActiveViewLock;
	TMap<const FSceneInterface*, FVector> LatestRenderedViewLocations;

	struct FPendingComposite
	{
		const FRDGBuilder* OwningGraphBuilder = nullptr;
		uint32 RenderFrameNumber = 0;
		FRDGTexture* IntermediateTexture = nullptr;
		FRDGTexture* IntermediateDepthTexture = nullptr;
		FRDGTexture* IntermediateNormalTexture = nullptr;
		FRDGTexture* IntermediateFogDepthTexture = nullptr;
		FRDGTexture* SnowIntermediateTexture = nullptr;
		FRDGTexture* SnowIntermediateDepthTexture = nullptr;
		FRDGTexture* SnowOcclusionDepthTexture = nullptr;
		FIntRect ViewRect;
		FVector4f CompositeLookParams0 = FVector4f::Zero();
		FVector4f CompositeLookParams1 = FVector4f::Zero();
		FVector4f CompositeLookParams2 = FVector4f::Zero();
		FVector4f CompositeLookParams3 = FVector4f::Zero();
		FVector4f CompositeLookParams4 = FVector4f::Zero();
		FVector4f CompositeBloomParams = FVector4f::Zero();
		FVector4f CompositeColorBloomParams = FVector4f(1.0f, 0.25f, 0.20f, 0.15f);
		FVector4f CompositeBloomTint = FVector4f(1, 1, 1, 0);
		float CompositeBloomSoftKnee = 0.25f;
		uint32 PanoramaGSCompositeMode = 0u;
		bool bAfterTonemap = false;
		int32 VRDiagnosticMode = 0;
	};
	TMap<const FSceneView*, FPendingComposite> PendingComposites;

	/** Singleton instance */
	static FGaussianSplatViewExtension* Instance;
};
