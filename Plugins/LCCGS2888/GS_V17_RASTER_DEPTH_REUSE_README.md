# V17: Gaussian centre radial depth reuse (stage E, Raster hot-path)

**Based directly on V16.2 shader-fix ZIP.** V15 A/B and V16 C/D, V13.1 shadow/bloom and all profiling features preserved. V14-A occlusion culling is not included.

## Why this instead of expanding covariance bake storage now
The prior trace shows stereo Raster ~38 ms while CalcViewData is only ~0.35-0.5 ms per eye. Storing six 3D covariance numbers per splat would add roughly 12-24 bytes per Gaussian and extra GPU bandwidth without tackling the measured bottleneck. The existing cluster, stable proxy, snow patterns and SH inputs already have bake/cache stages. No persistent asset layout is changed in V17.

## V17-E: cache splat centre depth in an existing interpolator
A single Gaussian quad has exactly one `ViewData.TranslatedWorldPos` centre, independent of the quad corner. The old pixel shader repeatedly performed `length(Input.TranslatedWorldPos)` for GS radial depth, snow-cut depth and fog depth. V17 optionally computes the same vector length in the Gaussian vertex shader (four vertices/quad), transports it in **the otherwise unused `w` component of the existing TEXCOORD3**, and reads the cached value wherever the old PS measured radial depth. The full `xyz` is untouched, so shadow capture, water-plane tests, removal boxes, and diagnostics continue using the same translated position.

CVar `gs.PCVR.ReuseRadialDepth` defaults to `0`. Details > 07 > `E. Gaussian 중심 깊이 계산 재사용 (V17)` enables it. The switch affects the usual PS/VS plus the lean/final PS path; snow overlay continues its existing separate shader and is unchanged.

`0` retains the original pixel `length(float3)` and avoids the extra VS sqrt; `1` does the sqrt in VS and uses the transported scalar in PS. It's a **mathematically equivalent centre-depth calculation**, apart from minor floating-point interpolation differences. Whether the real GPU benefits is uncertain: extra per-vertex work and an additional interpolated scalar may offset pixel shader savings.

## Next-time benchmark
Raster diagnostic mode 0 with same VR pose/weather, shadow bake and mesh state. Compare V17 E OFF vs ON with A/B/C/D settings unchanged. Compare GSPCVRRaster_LeftEye/RightEye, GPU frame duration, snow/fog depth, Stable Cast screenshots and validation of 3DGS vertices when rotating. FeatureSettings logs `gs.PCVR.ReuseRadialDepth` and `V17RadialDepthReuseSelected`.

## Notes
- **Not** a true left/right shared-final-colour cache: both eyes have separate depth and view-dependent colour; C/D retains existing per-eye dispatch.
- No new cluster bake format or static covariance buffers are introduced (intentionally: poor cost/benefit based on measured hot path).
- OFF by default pending VR tests. Does not force lighting/shadow/weather off.
- ZIP structure, source symbol/parameter wiring and CRC checks were performed. Unreal Engine 5.8.3 HLSL compilation / VR run cannot be performed in this environment, so this is an **unverified source-level experiment**.
