# MSAA sample composite

`gs.MSAASampleComposite` defaults to `1`. Set it to `0` for the previous
resolved-depth path, or back to `1` to enable sample-preserving compositing.
Keep the project's `r.MSAACount=2` setting.

For clear, ordinary Final rendering with matching multisampled scene color and
depth targets, the GS MRTs retain the scene sample count. Grade and bloom pixel
shaders read `SV_SampleIndex`; alpha sealing and blending run separately for
each sample, before Unreal resolves scene color. Shared scene depth is read-only.
The existing accumulated-alpha-only 16x seal is unchanged.

Rain/wet, snow, diagnostic, RAW, single-sample and experimental post-tonemap
isolation routes retain the previous path. The tiled compute rasterizer falls
back to raster when the destination has multisampled depth.

Validation: UE 5.8 Win64 C++ build and SM6 shaders passed. Mai_02 screenshots
at the same camera with MSAA 2x show removal of the bright cyan mesh fringes.
The sampled ground region remained at 148.29 versus 148.30 mean grayscale.
See `Saved/Screenshots/WindowsEditor/MSAAFix_01_Legacy.png` and
`MSAAFix_02_PerSample.png`. Physical stereo/HMD validation was unavailable:
OpenXR initialization returned `XR_ERROR_INITIALIZATION_FAILED` on this run.
