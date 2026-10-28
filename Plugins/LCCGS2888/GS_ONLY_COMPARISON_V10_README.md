# LCCGS2888 V10 — opt-in 3DGS-only VR Preview test

Built on V9 RasterCompare (same runtime renderer and 29 shader files). The Details panel (Gaussian Splat Actor > 07 진단 및 고급) now has **3DGS만 표시 (일반 메쉬 임시 숨김)**. Normal unselected play mode remains the existing mixed scene; there is no separate Mixed/Mesh-only UI mode.

## How to use
1. Turn on `VR 성능 테스트 모드 켜기`. Select `0 원본 화질 / 전체 셰이더` for this comparison: modes 1–3 are diagnostic raster shaders, not production quality.
2. Capture 30–60 s with `3DGS만 표시` unchecked (mixed scene), then stop the VR Preview.
3. Check `3DGS만 표시`, repeat VR Preview from same camera/lighting/resolution, then stop.
4. Compare `GPUFrameMs` distributions and actual Insights GPU timers. Look for `GSOnlySceneEnabled`, `GSOnlyHiddenNativeMeshCount`, `GSOnlyPreservedProxyMeshCount` in `_FeatureSettings.csv` and `_Summary.txt`.

## Scope, safety and measurement limitations
- During PostPIEStarted in a GS-containing PIE world **only**, eligible native Static Mesh (including ISM/HISM), Skeletal/Skinned Mesh, Landscape, Procedural Mesh and Geometry Collection components have `HiddenInGame=true` and `CastShadow=false` temporarily.
- GS actors and their attached/owned proxy geometry, XR pawns, capture-only proxy meshes, named `/collider/` proxy meshes and `SkySphere` actors are kept. Niagara and other environment components are not touched.
- Existing HiddenInGame and CastShadow flags are saved per PIE component and restored at EndPIE (not saved to actor/level/asset). The toggle persists per project in EditorPerProjectUserSettings.ini.
- **Only meshes already present at PostPIEStarted are isolated.** Dynamically spawned geometry may remain visible. The preserved sky sphere and GS proxy geometry can render mesh draw calls. This test therefore isolates *most native scene mesh rendering*, not a mathematically perfect GS-only GPU budget.
- Hiding native meshes also changes their shadows and possible occlusion; GPU costs are shared/nonadditive. Do not subtract GPU frame times and label the difference as an exact mesh-only GPU time.
- Native UE 5.8.3 build and VR test are not available in this environment; compile/runtime validation must be done locally.
