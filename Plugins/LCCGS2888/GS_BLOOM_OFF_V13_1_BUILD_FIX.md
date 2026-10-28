# V13.1 — UE 5.8.3 editor build fix

Fixed `GaussianSplatDetails.cpp` compilation failures by adding the two declarations missing from `GSPreviewRecorderSettings.h`:

- `GSPreviewRecorderUI::IsDisableAllBloomEnabled()`
- `GSPreviewRecorderUI::SetDisableAllBloomEnabled(bool)`

Both functions were already defined in `LCCCinematicGSEditorModule.cpp`. The missing declarations caused `C2039`, `C3861`, followed by cascading Slate `C2664`, `C7732`, and `C2679` errors. No rendering implementation, shader, visual effects, or shadow behavior changed relative to V13.

Close UE before replacing `Plugins/LCCGS2888` (plugin root contents are at ZIP root). Remove this plugin's old `Binaries` and `Intermediate` directories if rebuild errors persist. Compile from Unreal Editor/Build.bat again.

ZIP and source consistency were checked. UE 5.8.3 compile was not available in this environment, so actual success is not guaranteed.
