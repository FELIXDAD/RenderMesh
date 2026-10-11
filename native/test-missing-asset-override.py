import bpy
import os
import sys


args = sys.argv[sys.argv.index("--") + 1 :]
destination = os.path.abspath(args[0])
fixture_kind = args[1] if len(args) > 1 else "unused-missing"

scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.render.image_settings.file_format = "PNG"
scene.render.resolution_x = 64
scene.render.resolution_y = 64
scene.render.resolution_percentage = 100
scene.frame_start = 1
scene.frame_end = 1

# Keep a deliberately missing image datablock in the file without connecting it
# to any material, compositor node, object, world, or sequencer strip. This is
# the exact safe-override case: dependency inventory warns, but the unused
# datablock should not prevent an explicit user override from preparing it.
missing_path = None
if fixture_kind == "unused-missing":
    image = bpy.data.images.new("RenderMesh Unused Missing Asset", width=1, height=1)
    image.use_fake_user = True
    missing_path = os.path.join(os.path.dirname(destination), "does-not-exist.png")
    image.filepath_raw = missing_path
    image.file_format = "PNG"
    image.save()
    image.source = "FILE"
elif fixture_kind == "no-camera":
    scene.camera = None

bpy.ops.wm.save_as_mainfile(filepath=destination)
if missing_path:
    os.remove(missing_path)
