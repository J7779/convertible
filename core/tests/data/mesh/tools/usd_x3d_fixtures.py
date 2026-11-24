# USD (usd-core), X3D and VRML (MeshLab, assimp) fixtures (run from
# tests/data/mesh after tools/blender_export.py):
#   /venv/bin/python tools/usd_x3d_fixtures.py
import os
import shutil
import subprocess
import sys

import numpy as np
import pymeshlab
from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade, UsdUtils, Vt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_np as S  # noqa: E402

for d in ("usd", "x3d", "wrl"):
    os.makedirs(d, exist_ok=True)

# --- usd-core: Z-up centimetre stage with transforms, primvars and a material ---
def build_stage(path, textured=False):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, 0.01)
    root = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(root.GetPrim())
    material = UsdShade.Material.Define(stage, "/World/Looks/Green")
    shader = UsdShade.Shader.Define(stage, "/World/Looks/Green/Preview")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.25)
    shader.CreateInput("metallic", Sdf.ValueTypeNames.Float).Set(0.5)
    if textured:
        tex = UsdShade.Shader.Define(stage, "/World/Looks/Green/Texture")
        tex.CreateIdAttr("UsdUVTexture")
        tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set("textures/checker.png")
        tex.CreateOutput("rgb", Sdf.ValueTypeNames.Float3)
        shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(tex.ConnectableAPI(), "rgb")
    else:
        shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.1, 0.7, 0.2))
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
    for name, scene, scale, translate in S.two_objects():
        xf = UsdGeom.Xform.Define(stage, f"/World/{name}")
        # Z-up stage: the Y-up scene rotated like Blender does, in centimetres.
        xf.AddTranslateOp().Set(Gf.Vec3d(*[100 * c for c in S.to_z_up(translate)]))
        xf.AddRotateXYZOp().Set(Gf.Vec3f(90, 0, 0))
        xf.AddScaleOp().Set(Gf.Vec3f(100 * scale, 100 * scale, 100 * scale))
        mesh = UsdGeom.Mesh.Define(stage, f"/World/{name}/Geom")
        mesh.CreatePointsAttr([Gf.Vec3f(*p) for p in scene["positions"]])
        mesh.CreateFaceVertexCountsAttr([len(f) for f in scene["faces"]])
        mesh.CreateFaceVertexIndicesAttr([i for f in scene["faces"] for i in f])
        mesh.CreateDisplayColorPrimvar(UsdGeom.Tokens.constant).Set([Gf.Vec3f(0.5, 0.5, 0.5)])
        if name == "Box":
            UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
            # face-varying uvs through an index array
            st = UsdGeom.PrimvarsAPI(mesh).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
            st.Set([Gf.Vec2f(0, 0), Gf.Vec2f(1, 0), Gf.Vec2f(1, 1), Gf.Vec2f(0, 1)])
            st.SetIndices(Vt.IntArray([0, 1, 2, 3] * 6))
    stage.GetRootLayer().documentation = ""
    stage.Save()


build_stage("usd/two_objects_usdcore.usda")
with open("usd/two_objects_usdcore.usda") as f:
    text = f.read()
with open("usd/two_objects_usdcore.usda", "w") as f:
    f.write(text.replace('    doc = ""\n', ""))

# usdz with a text root layer and a texture, packed by USD's own packager (64-byte aligned).
os.makedirs("/tmp/pkg/textures", exist_ok=True)
with open("/tmp/pkg/textures/checker.png", "wb") as f:
    f.write(S.checker_png())
build_stage("/tmp/pkg/scene.usda", textured=True)
assert UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath("/tmp/pkg/scene.usda"), "usd/two_objects_usdcore.usdz")
# usdz holding a binary crate layer (what most tools produce): must be rejected clearly.
Usd.Stage.Open("usd/cube_blender.usda").Export("/tmp/pkg/cube.usdc")
assert UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath("/tmp/pkg/cube.usdc"), "usd/cube_crate.usdz")

# --- X3D / VRML from MeshLab and assimp ---------------------------------------
cube, sphere = S.unit_cube(), S.uv_sphere()
ms = pymeshlab.MeshSet()
cols = np.array([[(i & 1) * 1.0, ((i >> 1) & 1) * 1.0, ((i >> 2) & 1) * 1.0, 1.0] for i in range(8)])
ms.add_mesh(pymeshlab.Mesh(vertex_matrix=np.array(cube["positions"], dtype=float),
                           face_matrix=np.array(S.triangulate_fan(cube["faces"])), v_color_matrix=cols))
ms.save_current_mesh("wrl/cube_meshlab.wrl", save_vertex_color=True)
ms.save_current_mesh("x3d/cube_meshlab.x3d", save_vertex_color=True)
subprocess.run(["assimp", "export", "obj/sphere_blender.obj", "/tmp/sphere.x3d", "-fx3d"], check=True, stdout=subprocess.DEVNULL)
shutil.copy("/tmp/sphere.x3d", "x3d/sphere_assimp.x3d")
print("done")
