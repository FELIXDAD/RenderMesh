"""Builds large Blender projects for import timing.

Run inside Blender:
  blender --background --factory-startup --python make_large_fixtures.py -- <folder> <mesh MB> <textures>

Creates:
  heavy/heavy.blend        self-contained scene with a mesh of about <mesh MB>
  textured/textured.blend  scene that uses <textures> external 4K OpenEXR files
"""
import os
import sys

import bpy
import numpy

args = sys.argv[sys.argv.index('--') + 1:]
root = os.path.abspath(args[0])
mesh_megabytes = int(args[1])
texture_count = int(args[2])


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.frame_start, scene.frame_end = 1, 10
    camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
    scene.collection.objects.link(camera)
    scene.camera = camera


def save(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=path, compress=False)


reset()
mesh = bpy.data.meshes.new('Heavy')
count = mesh_megabytes * 1024 * 1024 // 12
mesh.vertices.add(count)
mesh.vertices.foreach_set('co', numpy.random.random(count * 3).astype(numpy.float32))
bpy.context.scene.collection.objects.link(bpy.data.objects.new('Heavy', mesh))
save(os.path.join(root, 'heavy', 'heavy.blend'))

reset()
save(os.path.join(root, 'textured', 'textured.blend'))
mesh = bpy.data.meshes.new('Card')
mesh.from_pydata([(0, 0, 0), (1, 0, 0), (1, 0, 1)], [], [(0, 1, 2)])
bpy.context.scene.collection.objects.link(bpy.data.objects.new('Card', mesh))
pixels = numpy.random.random(4096 * 4096 * 4).astype(numpy.float32)
for index in range(texture_count):
    path = os.path.join(root, 'textured', 'textures', f'plate_{index:02d}.exr')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image = bpy.data.images.new(f'plate_{index:02d}', 4096, 4096, float_buffer=True)
    image.pixels.foreach_set(pixels)
    image.filepath_raw = path
    image.file_format = 'OPEN_EXR'
    image.save()
    material = bpy.data.materials.new(f'plate_{index:02d}')
    material.use_nodes = True
    material.node_tree.nodes.new('ShaderNodeTexImage').image = image
    image.filepath = bpy.path.relpath(path)
    mesh.materials.append(material)
save(os.path.join(root, 'textured', 'textured.blend'))
print('LARGE_FIXTURES_OK')
