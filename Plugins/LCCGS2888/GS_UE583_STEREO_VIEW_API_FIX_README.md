# LCCGS2888 (7) VR Profiler / Test Mode — UE 5.8.3 stereo diagnostic fix

## Cause
The V7/V8 diagnostic-only GPU eye label function used `eSSP_LEFT_EYE` / `eSSP_RIGHT_EYE`, then `eSSP_PRIMARY` / `eSSP_SECONDARY`. The latter failed to compile even after `StereoRendering.h` was included, as confirmed in `SDGS_VR_TEST_00(6).log` (C2065 at line 129).

## Fix
Only `Source/LCCCinematicGS/Private/GaussianSplatRenderer.cpp` was modified.
- `GSRasterEyeBucket` now accepts a `const FSceneView&` and classifies views using the publicly documented UE 5.8 `IStereoRendering::IsStereoEyeView`, `IsAPrimaryView` and `IsASecondaryView` helpers, avoiding direct enum constants.
- Both Raster event/counter call sites pass the `FSceneView` itself.
- The asynchronous indirect count readback now records the already classified eye bucket; submissions store that same bucket rather than a raw `StereoPass` value.
- This is diagnostics only: the actual GS Raster draw, HLSL, shading, weather, shadows and scene isolation paths are unchanged.

## Verification
- ZIP integrity checked and changes diffed against prior StereoHeaderFix release.
- All references to `eSSP_PRIMARY`, `eSSP_SECONDARY`, `eSSP_LEFT_EYE` and `eSSP_RIGHT_EYE` removed from the renderer.
- Not compiled or runtime-tested against a local Unreal Engine 5.8.3 toolchain (unavailable here).

## Installation
Back up `Plugins/LCCGS2888`; replace contents from this archive; rebuild the project editor in UE 5.8.3. No additional console commands required; use the existing actor Test Mode button.
