# V16.2 — UE 5.8.3 HLSL Shader Compile Fix

## Issue
`FGaussianSplatPS` permutations 60 and 4 failed to compile in `GaussianSplatRendering.usf` (lines 2113 and 1737): `use of undeclared identifier PCVREarlyAlphaEnabled`.

## Minimal fix
Added `uint PCVREarlyAlphaEnabled;` in the `#if PIXELSHADER` parameter declarations in `Shaders/Private/PCVRSharedLighting.ush`. The corresponding shader parameter `SHADER_PARAMETER(uint32, PCVREarlyAlphaEnabled)` and its C++ assignments already existed in V16.1. Unreal's shader parameter metadata doesn't replace the HLSL source declaration used by this shader.

## Scope
Only the HLSL declaration was changed. Everything else (including V16.1 Slate padding build fix, lighting/shadow code, A/B/C/D test toggles, bloom controls, and GPU profiling) is carried over unchanged from V16.1.

## Check
Archive CRC/integrity, exact single-source-file diff, declaration/usage and existing C++ binding checked. Actual Unreal Engine 5.8.3/D3D SM6 shader compilation cannot be run here, so any further runtime/compile issues must be confirmed in Unreal Editor.
