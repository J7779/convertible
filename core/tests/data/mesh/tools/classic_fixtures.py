# OBJ / STL / PLY fixtures written with Python tools (run from tests/data/mesh,
# after tools/blender_export.py):
#   /venv/bin/python tools/classic_fixtures.py
import os
import struct
import sys
import zipfile

import numpy as np
import plyfile
import pymeshlab
import trimesh
import stl as npstl_mod
from stl import mesh as npstl

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_np as S  # noqa: E402

for d in ("obj", "stl", "ply"):
    os.makedirs(d, exist_ok=True)

cube, sphere = S.unit_cube(), S.uv_sphere()
cube_tris = np.array(S.triangulate_fan(cube["faces"]))
sphere_tris = np.array(S.triangulate_fan(sphere["faces"]))

# --- OBJ -------------------------------------------------------------------
# obj.zip: Blender's textured quad (obj + mtl + copied PNG), zipped by Python's zipfile.
src = "/tmp/cvt-blender/quad_obj"
if os.path.isdir(src):
    with zipfile.ZipFile("obj/quad_blender.obj.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(src)):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            with open(os.path.join(src, name), "rb") as f:
                z.writestr(info, f.read(), zipfile.ZIP_DEFLATED)
# trimesh OBJ of the sphere with vertex normals (triangles).
tm = trimesh.Trimesh(vertices=np.array(sphere["positions"]), faces=sphere_tris, process=False)
with open("obj/sphere_trimesh.obj", "w") as f:
    f.write(trimesh.exchange.obj.export_obj(tm, include_normals=True, header=None))
# MeshLab OBJ with per-vertex colours ("v x y z r g b").
ms = pymeshlab.MeshSet()
cols = np.array([[(i & 1) * 1.0, ((i >> 1) & 1) * 1.0, ((i >> 2) & 1) * 1.0, 1.0] for i in range(8)])
ms.add_mesh(pymeshlab.Mesh(vertex_matrix=np.array(cube["positions"], dtype=float), face_matrix=cube_tris,
                           v_color_matrix=cols))
ms.save_current_mesh("obj/cube_meshlab_colors.obj", save_vertex_color=True, save_vertex_normal=False)
for leftover in ("obj/cube_meshlab_colors.obj.mtl",):
    if os.path.exists(leftover):
        os.remove(leftover)

# --- STL -------------------------------------------------------------------
m = npstl.Mesh(np.zeros(len(sphere_tris), dtype=npstl.Mesh.dtype))
for i, t in enumerate(sphere_tris):
    m.vectors[i] = np.array([sphere["positions"][k] for k in t])
m.save("stl/sphere_numpystl_ascii.stl", mode=npstl_mod.Mode.ASCII)
# Binary STL whose header starts with "solid" (common in the wild), written with struct.
with open("stl/cube_solid_header_binary.stl", "wb") as f:
    f.write(b"solid cube written as binary".ljust(80, b" "))
    f.write(struct.pack("<I", len(cube_tris)))
    for t in cube_tris:
        f.write(struct.pack("<3f", 0, 0, 0))
        for k in t:
            f.write(struct.pack("<3f", *cube["positions"][k]))
        f.write(struct.pack("<H", 0))
# VisCAM/SolidView colours: bit 15 valid, red bits 10-14, green 5-9, blue 0-4; faces 0-3 red, 4-7 green, 8-11 blue.
with open("stl/cube_viscam_colors.stl", "wb") as f:
    f.write(b"VisCAM colour cube".ljust(80, b"\0"))
    f.write(struct.pack("<I", len(cube_tris)))
    for i, t in enumerate(cube_tris):
        f.write(struct.pack("<3f", 0, 0, 0))
        for k in t:
            f.write(struct.pack("<3f", *cube["positions"][k]))
        rgb = [(31, 0, 0), (0, 31, 0), (0, 0, 31)][i // 4]
        f.write(struct.pack("<H", 0x8000 | (rgb[0] << 10) | (rgb[1] << 5) | rgb[2]))

# --- PLY -------------------------------------------------------------------
# plyfile: big-endian, double positions, ushort colours, int8 + float extra props, uint/ushort lists.
v = np.array([(p[0], p[1], p[2], 65535 * (i & 1), 65535 * ((i >> 1) & 1), 65535 * ((i >> 2) & 1), i - 4, 0.25 * i)
              for i, p in enumerate(cube["positions"])],
             dtype=[("x", ">f8"), ("y", ">f8"), ("z", ">f8"), ("red", ">u2"), ("green", ">u2"), ("blue", ">u2"),
                    ("label", "i1"), ("quality", ">f4")])
f = np.array([(face,) for face in cube["faces"]], dtype=[("vertex_indices", "O")])
el_v = plyfile.PlyElement.describe(v, "vertex")
el_f = plyfile.PlyElement.describe(f, "face", len_types={"vertex_indices": "u2"}, val_types={"vertex_indices": "u4"})
plyfile.PlyData([el_v, el_f], byte_order=">", comments=["synthetic unit cube"]).write("ply/cube_plyfile_be.ply")
# plyfile ASCII point cloud with colours and an intensity property.
pts, cols = S.point_grid()
pv = np.array([(p[0], p[1], p[2], c[0], c[1], c[2], float(i)) for i, (p, c) in enumerate(zip(pts, cols))],
              dtype=[("x", "f4"), ("y", "f4"), ("z", "f4"), ("red", "u1"), ("green", "u1"), ("blue", "u1"),
                     ("intensity", "f4")])
plyfile.PlyData([plyfile.PlyElement.describe(pv, "vertex")], text=True).write("ply/points_plyfile_ascii.ply")
# MeshLab binary PLY with per-face colours (faces 0-3 red, 4-7 green, 8-11 blue).
ms = pymeshlab.MeshSet()
fcols = np.array([[1.0, 0, 0, 1], [0, 1.0, 0, 1], [0, 0, 1.0, 1]])[np.arange(12) // 4]
ms.add_mesh(pymeshlab.Mesh(vertex_matrix=np.array(cube["positions"], dtype=float), face_matrix=cube_tris,
                           f_color_matrix=fcols))
ms.save_current_mesh("ply/cube_meshlab_face_colors.ply", binary=True, save_face_color=True,
                     save_vertex_color=False, save_vertex_normal=False)
print("done")
