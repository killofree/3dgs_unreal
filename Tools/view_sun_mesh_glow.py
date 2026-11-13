import unreal
s=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
sun=next(a for a in s.get_all_level_actors() if a.get_actor_label()=='J02_sun')
s.set_selected_level_actors([])
camera=unreal.Vector(10000,-800,8000)
unreal.EditorLevelLibrary.set_level_viewport_camera_info(camera,unreal.MathLibrary.find_look_at_rotation(camera,sun.get_actor_location()))
