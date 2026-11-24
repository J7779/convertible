# Exports the canonical scenes (tools/scenes_np.py) with Blender's own exporters.
# Run from tests/data/mesh:
#   blender41 -b --factory-startup --python tools/blender_export.py
# (Blender 4.1.1 from blender.org, which includes the COLLADA, USD, 3DS
# and X3D exporters.)
# Blender is Z-up: the Y-up scene data is rotated into Blender's frame
# (scenes_np.to_z_up); exporters that convert to Y-up (OBJ, glTF, FBX,
# X3D, USD) give back the original coordinates, the others (STL, PLY,
# 3DS, COLLADA) write Z-up data.
import os
import sys
import traceback

import addon_utils
import bpy

sys.path.insert(0, os.path.join(os.getcwd(), "tools"))
import scenes_np as S  # noqa: E402

TMP = "/tmp/cvt-blender"
os.makedirs(TMP, exist_ok=True)
CHECKER = os.path.join(TMP, "checker.png")
with open(CHECKER, "wb") as f:
    f.write(S.checker_png())


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for addon in ("io_scene_3ds", "io_scene_x3d", "io_scene_fbx", "io_scene_gltf2", "io_mesh_stl"):
        try:
            addon_utils.enable(addon, default_set=True)
        except Exception:
            pass


def checker_material():
    mat = bpy.data.materials.new("Checker")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Roughness"].default_value = 0.5
    bsdf.inputs["Metallic"].default_value = 0.0
    img = bpy.data.images.load(CHECKER)
    tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = img
    mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    return mat


def red_material():
    mat = bpy.data.materials.new("Red")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.8, 0.1, 0.1, 1.0)
    bsdf.inputs["Metallic"].default_value = 0.25
    bsdf.inputs["Roughness"].default_value = 0.6
    return mat


def add_object(scene, name=None, location=(0, 0, 0), scale=1.0, material=None):
    name = name or scene["name"]
    me = bpy.data.meshes.new(name)
    me.from_pydata([S.to_z_up(p) for p in scene["positions"]], [], scene["faces"])
    if "uvs" in scene:
        uv = me.uv_layers.new(name="UVMap")
        for loop in me.loops:
            uv.data[loop.index].uv = scene["uvs"][loop.vertex_index]
    if "colors" in scene:
        col = me.color_attributes.new(name="Col", type="BYTE_COLOR", domain="POINT")
        for i, c in enumerate(scene["colors"]):
            col.data[i].color = c
    me.validate()
    me.update()
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = S.to_z_up(location)
    obj.scale = (scale, scale, scale)
    if material is not None:
        me.materials.append(material)
    return obj


def build(kind):
    reset()
    if kind == "cube":
        add_object(S.unit_cube())
    elif kind == "sphere":
        add_object(S.uv_sphere())
    elif kind == "star":
        add_object(S.concave_star())
    elif kind == "quad":
        add_object(S.textured_quad(), material=checker_material())
    elif kind == "two":
        for name, scene, scale, translate in S.two_objects():
            add_object(scene, name=name, location=translate, scale=scale, material=red_material() if name == "Box" else None)
    bpy.context.view_layer.update()


def export(kind, fmt, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        if fmt == "obj":
            bpy.ops.wm.obj_export(filepath=path, export_materials=True, export_colors=True, path_mode="STRIP",
                                  forward_axis="NEGATIVE_Z", up_axis="Y", export_selected_objects=False)
        elif fmt == "objcopy":
            bpy.ops.wm.obj_export(filepath=path, export_materials=True, export_colors=True, path_mode="COPY",
                                  forward_axis="NEGATIVE_Z", up_axis="Y", export_selected_objects=False)
        elif fmt in ("stl", "stl_ascii"):
            try:
                bpy.ops.wm.stl_export(filepath=path, ascii_format=fmt == "stl_ascii", export_selected_objects=False)
            except Exception:
                bpy.ops.export_mesh.stl(filepath=path, ascii=fmt == "stl_ascii", use_selection=False)
        elif fmt == "ply":
            bpy.ops.wm.ply_export(filepath=path, ascii_format=False, export_selected_objects=False,
                                  export_uv=True, export_normals=False, export_colors="SRGB")
        elif fmt == "ply_ascii":
            bpy.ops.wm.ply_export(filepath=path, ascii_format=True, export_selected_objects=False,
                                  export_uv=True, export_normals=True, export_colors="SRGB")
        elif fmt == "glb":
            bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", export_colors=True)
        elif fmt == "gltf_embedded":
            bpy.ops.export_scene.gltf(filepath=path, export_format="GLTF_EMBEDDED", export_colors=True)
        elif fmt == "gltf_separate":
            bpy.ops.export_scene.gltf(filepath=path, export_format="GLTF_SEPARATE", export_colors=True)
        elif fmt == "fbx":
            bpy.ops.export_scene.fbx(filepath=path, path_mode="COPY", embed_textures=True, use_selection=False)
        elif fmt == "dae":
            bpy.ops.wm.collada_export(filepath=path, use_texture_copies=False)
        elif fmt == "x3d":
            bpy.ops.export_scene.x3d(filepath=path, path_mode="STRIP")
        elif fmt == "3ds":
            bpy.ops.export_scene.max3ds(filepath=path)
        elif fmt == "usda":
            bpy.ops.wm.usd_export(filepath=path, export_textures=False, relative_paths=True, export_materials=True)
        print("WROTE", path)
    except Exception:  # keep going: report and continue with the next export
        print("FAILED", kind, fmt)
        traceback.print_exc()


PLAN = {
    "cube": [("obj", "obj/cube_blender.obj"), ("stl", "stl/cube_blender_binary.stl"),
             ("stl_ascii", "stl/cube_blender_ascii.stl"), ("ply_ascii", "ply/cube_blender_ascii.ply"),
             ("glb", "gltf/cube_blender.glb"), ("fbx", "fbx/cube_blender.fbx"), ("dae", "dae/cube_blender.dae"),
             ("x3d", "x3d/cube_blender.x3d"), ("3ds", "3ds/cube_blender.3ds"), ("usda", "usd/cube_blender.usda")],
    "sphere": [("obj", "obj/sphere_blender.obj"), ("stl", "stl/sphere_blender_binary.stl"),
               ("ply", "ply/sphere_blender_binary.ply"), ("fbx", "fbx/sphere_blender.fbx"),
               ("dae", "dae/sphere_blender.dae")],
    "star": [("obj", "obj/star_blender.obj"), ("ply_ascii", "ply/star_blender_ascii.ply"),
             ("glb", "gltf/star_blender.glb"), ("fbx", "fbx/star_blender.fbx"), ("dae", "dae/star_blender.dae")],
    "quad": [("objcopy", "/tmp/cvt-blender/quad_obj/quad.obj"), ("ply", "ply/quad_blender_colors.ply"),
             ("glb", "gltf/quad_blender.glb"),
             ("gltf_separate", "/tmp/cvt-blender/quad_sep/quad.gltf"), ("fbx", "fbx/quad_blender.fbx"),
             ("dae", "dae/quad_blender.dae"), ("x3d", "x3d/quad_blender.x3d"), ("3ds", "3ds/quad_blender.3ds"),
             ("usda", "usd/quad_blender.usda")],
    "two": [("obj", "obj/two_objects_blender.obj"), ("glb", "gltf/two_objects_blender.glb"),
            ("fbx", "fbx/two_objects_blender.fbx"), ("dae", "dae/two_objects_blender.dae"),
            ("3ds", "3ds/two_objects_blender.3ds"), ("usda", "usd/two_objects_blender.usda"),
            ("x3d", "x3d/two_objects_blender.x3d")],
}

only = os.environ.get("CVT_ONLY")
for kind, exports in PLAN.items():
    build(kind)
    for fmt, path in exports:
        if only and fmt not in only.split(","):
            continue
        export(kind, fmt, path)
