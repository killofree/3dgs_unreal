# Source viewer rendering profile

The weather sequence preview uses `gs.SuperSplatCompare=0` (production rendering) in DefaultEngine.ini. Source viewer mode 2 remains available in the actor details dropdown.

- 2: source SH/DC color, original opacity and scale, full source splats, minimum projected diameter 2 pixels. Accumulate premultiplied display RGB, preserve the mesh depth test at each MSAA sample, resolve GS color, and composite after UE tone mapping. UE exposure, color grading, bloom and plugin relighting are bypassed for GS. Ordinary meshes keep their UE rendering.
- 1: production rendering with accumulated-alpha sealing disabled.
- 0: original production rendering and effects.

The profile deliberately preserves the trained appearance, including lighting already recorded in the PLY. Plugin weather, seasonal edits, removal-box pixel masks, proxy shadows and grade are not part of profile 2. Asset transforms are unchanged. The viewport FOV was matched separately to 85 degrees; no camera position is inferred from a screenshot.

This does not claim pixel identity with SuperSplat. Scale/rotation GPU quantization and unknown training antialiasing metadata remain possible differences. The same source, camera and viewport resolution are required for a strict comparison.

Native code changes require the new plugin binary to be loaded on editor startup. Shader recompilation alone cannot activate the post-tonemap routing. No screenshots or level saves are performed by this change.

## Editor control
Select a 3DGS actor, then open **01 기본 설정 → 렌더링 모드 (전체 3DGS)**. The dropdown offers production, alpha-seal bypass, and SuperSplat source rendering. It controls all GS actors together, applies immediately and saves the selected default to the existing project DefaultEngine.ini. Restart the editor once after installing the new native plugin binaries to see the menu.
