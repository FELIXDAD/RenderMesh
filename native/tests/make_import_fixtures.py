"""Builds Blender projects that exercise the RenderMesh import paths.

Run inside Blender:
  blender --background --factory-startup --python make_import_fixtures.py -- <folder> [<mesh MB>]

Creates:
  plain/plain.blend            self-contained scene (large mesh, no external files)
  external/external.blend      image in a folder next to the project (must be packed)
  texproject/shot.blend        //tex/albedo.png (relative), an absolute path into tex,
                               and an image outside tex (must be packed)
  missing/missing.blend        image whose file does not exist
"""
import os
import sys

import bpy

args = sys.argv[sys.argv.index('--') + 1:]
root = os.path.abspath(args[0])
mesh_megabytes = int(args[1]) if len(args) > 1 else 40


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.render.resolution_x = 64
    scene.render.resolution_y = 64
    scene.render.image_settings.file_format = 'PNG'
    scene.frame_start, scene.frame_end = 1, 3
    camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
    scene.collection.objects.link(camera)
    scene.camera = camera
    camera.location = (0, -6, 0)
    camera.rotation_euler = (1.5708, 0, 0)
    return scene


def write_png(path, color):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image = bpy.data.images.new(os.path.basename(path), width=8, height=8)
    image.pixels = list(color) * 64
    image.filepath_raw = path
    image.file_format = 'PNG'
    image.save()
    bpy.data.images.remove(image)


def use_image(path, relative):
    image = bpy.data.images.load(path)
    if relative:
        image.filepath = bpy.path.relpath(path)
    material = bpy.data.materials.new('Mat-' + os.path.basename(path))
    material.use_nodes = True
    node = material.node_tree.nodes.new('ShaderNodeTexImage')
    node.image = image
    # Assign the material to an object: Blender does not save unused data.
    mesh = bpy.data.meshes.new('Card-' + os.path.basename(path))
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (1, 0, 1)], [], [(0, 1, 2)])
    mesh.materials.append(material)
    bpy.context.scene.collection.objects.link(bpy.data.objects.new(mesh.name, mesh))
    return image


def save(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=path, compress=False)


# 1. Large self-contained project.
reset_scene()
mesh = bpy.data.meshes.new('Heavy')
count = max(1, mesh_megabytes * 1024 * 1024 // 40)
mesh.vertices.add(count)
mesh.vertices.foreach_set('co', [float(i % 997) * 0.001 for i in range(count * 3)])
bpy.context.scene.collection.objects.link(bpy.data.objects.new('Heavy', mesh))
save(os.path.join(root, 'plain', 'plain.blend'))

# 2. External image outside any tex folder.
reset_scene()
external_png = os.path.join(root, 'external', 'textures', 'wood.png')
write_png(external_png, (0.6, 0.4, 0.2, 1.0))
save(os.path.join(root, 'external', 'external.blend'))
use_image(external_png, relative=True)
save(os.path.join(root, 'external', 'external.blend'))

# 3. tex folder: relative and absolute references into tex, one outside image.
reset_scene()
tex_root = os.path.join(root, 'texproject', 'tex')
write_png(os.path.join(tex_root, 'albedo.png'), (1, 0, 0, 1))
write_png(os.path.join(tex_root, 'maps', 'rough.png'), (0, 1, 0, 1))
outside_png = os.path.join(root, 'texproject', 'other', 'decal.png')
write_png(outside_png, (0, 0, 1, 1))
save(os.path.join(root, 'texproject', 'shot.blend'))
use_image(os.path.join(tex_root, 'albedo.png'), relative=True)
use_image(os.path.join(tex_root, 'maps', 'rough.png'), relative=False)
use_image(outside_png, relative=True)
save(os.path.join(root, 'texproject', 'shot.blend'))

# 4. Missing image (unused datablock kept with a fake user).
reset_scene()
missing_png = os.path.join(root, 'missing', 'gone.png')
write_png(missing_png, (1, 1, 1, 1))
save(os.path.join(root, 'missing', 'missing.blend'))
image = use_image(missing_png, relative=True)
image.use_fake_user = True
save(os.path.join(root, 'missing', 'missing.blend'))
os.remove(missing_png)
print('FIXTURES_OK')
