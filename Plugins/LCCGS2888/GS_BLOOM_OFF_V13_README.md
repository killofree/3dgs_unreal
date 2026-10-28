# V13 Bloom-off isolation (Unreal Engine 5.8.3 / PCVR)

## Baseline and rollback

This build STARTS FROM **V10** (GSOnly_V10) source/shaders, NOT V11/V12. All V11 SkipInactive MRT changes and V12 ReuseFinalLighting automatic per-vertex lighting have been rolled back to V10 reference. Existing Stable proxy shadows and final per-pixel lighting are intact.

## What is disabled (only while playing a PIE VR Preview with a Gaussian actor)

- GS local atmospheric bloom seed (CalcViewData.usf): negative sentinel in existing AtmosphereBloomParams.z, so the bloom amplitude is zero; original shadow, fog and indirect lighting still execute.
- GS post-grade spatial bloom: scatter/spread/core become zero, 40-tap sampling returns early. The composite pass remains because it is also responsible for scene color blending, fog and HDR pre-exposure.
- GS bloom extraction cache dispatch is not submitted.
- Unreal PostProcess bloom via r.BloomQuality=0, restored afterward.
- PIE directional light components' `bEnableLightShaftBloom` disabled and restored afterward. Cast shadows and shaft occlusion are NOT disabled.

Only the new checkbox `V13 블룸 전체 OFF` selects this. It is ON by default for the first comparison. UI is under GS Actor Details / 07.

If checkbox OFF, render path is identical to V10 (not V12).

## Stereo sharing

SharedCulling and PreSortCompact preserved from V10. Previous 1052 cluster/compact dispatch scheduling calls included 526 reuse skips. Do NOT assume stereo pixel rasterization, per-eye view projection, view-data or radix sorting are shared. A future stage needs robust per-eye output equivalence checks before further sharing.

## Test

Set VRDiagMode=0, GS-only checkbox OFF. Capture 30-60s with `V13 Bloom OFF` enabled. Then compare with disabled at same spot, lighting and time-of-day. Observe shadows visually. Compare _GPU_GS_Timers.csv and _FeatureSettings.csv, especially GaussianSplatPostGradeBloomComposite (must still appear: essential composite, but smaller GPU cost) and UE Bloom. 

NOTE: Unreal 5.8.3 compilation cannot be tested in this environment. Check compile and runtime screenshots and logs. If any shadows or color differ unexpectedly beyond the expected missing glow, turn checkbox OFF, or revert to V10.
