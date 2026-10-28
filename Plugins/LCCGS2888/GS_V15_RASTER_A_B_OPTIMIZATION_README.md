# LCCGS2888 V15 - Original Raster A+B optimization (UE 5.8.3 source ZIP)

Base: V13.1 fixed plugin source. No V14-A occlusion culling is included.

## A - early alpha before expensive lighting
`gs.PCVR.EarlyAlphaBeforeLighting` default `0`; when `1`, ordinary source GS pixels which already fail the ORIGINAL falloff `1/255` cutoff are discarded before per-pixel `EvaluateFinalSplatLighting` / shadow sampling. The original falloff math and blend are reused, no shadow method changes. Debug/preview and puddle-specific alpha replacement are excluded from the early test. The optimized PCVR final-only shader does an equivalent early alpha test before its lighting calculation.

## B - inactive native GS fog target omission
`gs.PCVR.TrimInactiveFogMRT` default `0`; when `1` and native GS fog is actually OFF **and** PCVR LeanOutputs is active, the pass allocates two MRTs instead of three: HDR color and depth. A new `GS_PCVR_NO_FOG_OUTPUT` pixel shader permutation avoids writing SV_Target2 and matches a 2-RT blend state. If native GS fog turns ON the original 3-MRT route remains and the depth/coverage fog output is preserved. Full/non-lean paths retain the original output layout, not forced to 2 MRT.

## Editor Details
`07 diagnostics / advanced` has two independent toggles A and B (stored in editor per-project config; locked during PIE). FeatureSettings.csv exports both CVar values; existing GPU export stays.

## Important
This is *not* a share-all-eye-compute or cluster bake change. Stereo culling and compact reuse remain as before. Lighting/shadow bake are not modified. V13 Bloom OFF checkbox still exists and is independent of A/B; for production quality turn it OFF if not intended. Raster diagnosis set mode 0; GS Only OFF for the mixed scene.

Benchmark same camera and weather, A=0/B=0, then A=1/B=0, then A=1/B=1. B only helps when `LeanOutputs` really executes and native GS fog is inactive. Confirm effect outputs (rain / snow / fog / shadow / bloom), GPU timings, and warnings. Shader compiling may take longer due the new permutation. ZIP source lint/integrity only; no local UE 5.8.3 build or VR headset runtime verification.
