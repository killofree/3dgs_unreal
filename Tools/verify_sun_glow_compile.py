import unreal,pathlib,json
root=pathlib.Path(unreal.Paths.project_dir())
rows=[]
for name in ['M_SunMeshHalo','M_SunCoreMesh']:
    m=unreal.load_asset('/Game/SceneFlow/Materials/Sun/Mai02/'+name)
    assert m
    errors=list(unreal.MaterialEditingLibrary.recompile_material(m))
    rows.append({'material':m.get_path_name(),'depth_test_disabled':m.get_editor_property('disable_depth_test'),'errors':errors,'stats':str(unreal.MaterialEditingLibrary.get_statistics(m))})
    assert not errors, errors
(root/'Saved/SunMeshGlowCompile.json').write_text(json.dumps(rows,indent=2),encoding='utf-8')
unreal.log('SUN_MESH_GLOW_COMPILE_REQUESTED')
