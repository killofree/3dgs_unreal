# LCCGS2888 Raster Cost Compare (V9) - UE 5.8.3

Based on the LAST KNOWN RUNNING integrated StereoViewAPI fixed version. Does not modify any .usf/.ush shader, GS actor/asset, environment or production renderer draw path. The existing optional shader variants `gs.VRDiagMode=1..3` already existed in the baseline. Test-only UI applies these only during a recorded Gaussian PIE Preview and restores `gs.VRDiagMode` when PIE ends. **Modes 1..3 DO change visual output DURING the experiment**; mode 0 is unmodified production quality.

## Editor UI
Actor > Details > 07 진단 및 고급 > VR 성능 테스트 모드 켜기. Then choose one of 4 raster cost variants (0 default, 1 simplified PS same MRT, 2 simplified PS / 2 MRT, 3 trained SH no weather / 1 MRT). Exit VR Preview between tests. Auto capture only runs in Gaussian worlds, when UI test mode is ON. UI settings stored in EditorPerProjectUserSettings.ini.

## Generated output
Saved/LCCGS/Profiling/GS_Preview_TIMESTAMP_*:
- Main frame CSV, `Summary.txt`, `.utrace`, `GPU_Timers.csv`, `GPU_GS_Timers.csv`, `RasterEyes.csv`, `FeatureSettings.csv` (unchanged).
- `RasterWorkload.csv`: per recorded game-ticker sample per-eye DrawCalls, mean source upper bound, viewport pixels, sparse delayed DrawInstances readbacks, mean instances / viewport megapixel. **NOT fragment count, overdraw or GPU duration**.
- `RasterStages.csv`: per-pipeline-pass actual **Insights GPU timer** statistics; parents and nested child rows overlap and are NOT additive.
- FeatureSettings + Summary label the chosen raster test variant to make run-to-run comparisons easier.

## Recommended test
Run 0, then 1, then 2, then 3 separately; keep camera placement, weather state and VR runtime resolution constant. Capture >=30sec each. Mode 0 is the only real-quality baseline; experimental variants deliberately simplify some shader outputs. Compare _RasterStages.csv mean RasterLeft/Right with and without those features; relative changes are NOT exact VS/PS/lighting subpass times. Renderer code remains unchanged and GPU events continue to measure the whole draw (VS+PS+blend). UE 5.8.3 build and VR run must be verified locally; this environment cannot compile UE.
