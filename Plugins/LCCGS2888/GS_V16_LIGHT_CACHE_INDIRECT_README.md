# LCCGS2888 V16 - C/D Raster sharing experiments (UE 5.8.3 source)

Base: V15 full plugin; **V14-A occlusion changes not included.** All V15 A/B and original shadow, weather, bloom, and recording options retained. No changes to Stable Cast bake data or the production `EvaluateFinalSplatLighting` math.

## C: visible per-Gaussian final lighting cache
- Details 07: `C. Gaussian별 최종 조명 계산 캐시 (원본 그림자 유지)`.
- CVar `gs.PCVR.ComputeFinalColor` 0 reference / 1 opt-in.
- Uses the existing `PCVRFinalColor.usf` (`GaussianSplatPCVRFinalColor` GPU timer) to compute full final light/Stable Cast colour once per sorted visible GS, then raster reads `PCVRFinalColors` rather than executing the same final-light function per pixel.
- Does **not** move lighting to VS as V12 did. The exact shared `EvaluateFinalSplatLighting` function is retained. It runs again for the second eye because training SH/ViewData are eye dependent.
- Existing safe gates remain: PCVR LeanOutputs, final shader, VR mode 0, not original unlit, no raster clip, pre-sort compact and sort params. Where they do not apply, raster uses original pixel-lighting path. No weather feature is forcibly disabled.
- Rendering benefit must be measured **including new GaussianSplatPCVRFinalColor compute time**. Performance may get worse for narrow Gaussians.

## D: use GPU visible count for C dispatch
- Details 07: `D. Visible Gaussian 수 기반 GPU 조명 Dispatch`.
- CVar `gs.PCVR.FinalColorIndirectDispatch` 0 reference linear dispatch over source capacity / 1 visible-count GPU indirect dispatch.
- Reuses the existing GPU-generated `GlobalSortIndirectArgsGlobalBuffer` from compact+sort (ceil(visible/1024) workgroups). The V16 indirect CS permutation has 256 lanes x 4 splats so no extra prefix pass or CPU readback.
- This **does not** pretend to share final colour across eyes. Both eyes reuse stereo cull/compact as before, but eye-specific SH/2D projection/depth/sort/colour are calculated per eye.
- This reduces idle thread launch overhead only; the effect is not guaranteed to be large.

## Benchmark
Set Raster mode 0 and identical camera/weather/shadow bake; keep GSOnly unchecked. Measure baseline A/B settings unchanged, C=0 D=0, then C=1 D=0, then C=1 D=1. Compare frame GPU, GaussianSplatPCVRFinalColor, GSPCVRRasterOnly per-eye, and _FeatureSettings.csv. If the FinalColor timer doesn't appear, C eligibility wasn't met; refer to Cvar gates. If artifacts appear, turn C off immediately.

## Build status
Static source checks and ZIP CRC verification only. Unreal Engine 5.8.3 compiler / shader compiler and VR runtime unavailable in this container. Real-world correctness and speed unverified. This is an opt-in experimental optimization, NOT the complete stereo-shared lighting architecture.
