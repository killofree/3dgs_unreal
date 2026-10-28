# SDGS VR Test

Unreal Engine 5.8 VR project with 3D Gaussian Splatting and reusable scene orchestration.

## Open the project

1. Install Git LFS before cloning: `git lfs install`.
2. Clone the repository, then run `git lfs pull` if assets were not downloaded automatically.
3. Use Unreal Engine 5.8 and install the external engine plugins enabled in `SDGS_VR_TEST_00.uproject`, including the PICO controller plugin.
4. Open `SDGS_VR_TEST_00.uproject`. Rebuild the project plugins if prompted; editor binaries are generated locally.

## Project folders

- `Content`: levels, Blueprint graphs, meshes, textures, audio and 3DGS assets (Git LFS).
- `Plugins/LCCGS2888`: 3DGS rendering and related effects.
- `Plugins/SceneEnvironment`: environment effects and Blueprint parameter access.
- `Plugins/SceneTransitionTest`: scene director, timed actions, paths and transitions.
- `Plugins/VR360Skybox`: VR skybox support.
- `Config`, `Tools`, `Docs`: project settings, maintenance scripts and workflow notes.

The first scene graph is `/Game/SceneFlow/Blueprints/BP_SceneDirector` in `j_02`. Enable Auto Start to run the scenario; disable it for ordinary preview. See `Docs/BlueprintScenario.md` and `Docs/SceneFlow.md`.

Generated caches, editor binaries, logs, local backups and packaged EXEs are excluded from version control. The scripts under `Tools` include one-time migrations and scene-specific maintenance: read a script before running it.
