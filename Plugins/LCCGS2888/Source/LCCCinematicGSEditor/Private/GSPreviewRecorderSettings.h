// Editor-only, per-project UI preset for VR Preview profiling.
// Kept private to the LCCCinematicGSEditor module; never serialized on GS assets.
#pragma once
#include "CoreMinimal.h"

namespace GSPreviewRecorderUI
{
    bool IsEnabled();
    // Opt-in: hide ordinary meshes ONLY in the transient GS PIE world; default is full scene.
    // V13.1: exported to the Details panel. Definitions live in LCCCinematicGSEditorModule.cpp.
    bool IsEarlyAlphaEnabled();
    void SetEarlyAlphaEnabled(bool bEnabled);
    bool IsTrimFogMRTEnabled();
    void SetTrimFogMRTEnabled(bool bEnabled);
    // V16-C per-visible-Gaussian lighting (existing exact shadow equation).
    bool IsPerSplatLightCacheEnabled();
    void SetPerSplatLightCacheEnabled(bool bEnabled);
    // V16-D GPU indirect visible-count dispatch (no unsafe cross-eye colour reuse).
    bool IsVisibleLightDispatchEnabled();
    void SetVisibleLightDispatchEnabled(bool bEnabled);
    bool IsRadialDepthReuseEnabled();
    void SetRadialDepthReuseEnabled(bool bEnabled);
    bool IsDisableAllBloomEnabled();
    void SetDisableAllBloomEnabled(bool bEnabled);
    bool IsGSOnlySceneEnabled();
    void SetGSOnlySceneEnabled(bool bEnabled);
    void SetEnabled(bool bEnabled);
    // 0 = original rendering; 1..3 = temporary comparison shaders during recorded PIE only.
    int32 GetRasterDiagnosticVariant();
    void SetRasterDiagnosticVariant(int32 Mode);
    void LoadFromEditorConfig();
    void RestoreCVarsOnShutdown();
}
