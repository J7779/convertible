# Fixtures for 3DS, DirectX, LWO2, MD2, MD3, MS3D and MagicaVoxel (run from
# tests/data/mesh after tools/blender_export.py and tools/legacy_fixtures.py):
#   /venv/bin/python tools/game_fixtures.py
#
# No exporter for LWO2, MD2, MD3 or MS3D exists in the container. Those files
# are written here with Python's struct module, field by field from the
# published format descriptions, and each one is then decoded by assimp
# (an independent implementation): `assimp info` must report the expected
# counts and `assimp export ... .obj` gives the reference geometry the tests
# compare against (assimp's own interpretation of the file).
import os
import shutil
import struct
import subprocess
import sys
import zipfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_np as S  # noqa: E402

for d in ("3ds", "x", "lwo", "md2", "md3", "ms3d", "vox"):
    os.makedirs(d, exist_ok=True)

cube = S.unit_cube()
tris = S.triangulate_fan(cube["faces"])           # counter-clockwise, Y-up
zup = [S.to_z_up(p) for p in cube["positions"]]     # the same cube in Z-up axes


def assimp(src, dst, fmt, *extra):
    tmp = "/tmp/" + os.path.basename(dst)
    subprocess.run(["assimp", "export", src, tmp, "-f" + fmt, *extra], check=True, stdout=subprocess.DEVNULL)
    shutil.copy(tmp, dst)


def check(path, expect_faces):
    out = subprocess.run(["assimp", "info", path], capture_output=True, text=True).stdout
    faces = [l for l in out.splitlines() if l.strip().startswith("Faces:")]
    print(path, faces)
    assert faces and int(faces[0].split()[-1]) == expect_faces, out


# --- 3DS and DirectX from assimp -------------------------------------------------
assimp("obj/sphere_blender.obj", "3ds/sphere_assimp.3ds", "3ds", "-tri")
with zipfile.ZipFile("obj/quad_blender.obj.zip") as z:
    z.extractall("/tmp/quad")
assimp("/tmp/quad/quad.obj", "x/quad_assimp.x", "x")

# --- LightWave LWO2 (left-handed, polygons clockwise from the front) --------------
def lwo_chunk(cid, data):
    return cid + struct.pack(">I", len(data)) + data + (b"\0" if len(data) % 2 else b"")


def lwo_string(s):
    b = s.encode() + b"\0"
    return b + (b"\0" if len(b) % 2 else b"")


def lwo_vx(i):
    return struct.pack(">H", i) if i < 0xFF00 else struct.pack(">I", 0xFF000000 | i)


body = b"LWO2"
body += lwo_chunk(b"TAGS", lwo_string("Default"))
body += lwo_chunk(b"LAYR", struct.pack(">HH3f", 0, 0, 0, 0, 0) + lwo_string("Cube"))
body += lwo_chunk(b"PNTS", b"".join(struct.pack(">3f", x, y, -z) for x, y, z in cube["positions"]))
body += lwo_chunk(b"POLS", b"FACE" + b"".join(struct.pack(">H", len(f)) + b"".join(lwo_vx(i) for i in f) for f in cube["faces"]))
body += lwo_chunk(b"PTAG", b"SURF" + b"".join(lwo_vx(i) + struct.pack(">H", 0) for i in range(6)))
surf = lwo_string("Default") + lwo_string("")
surf += b"COLR" + struct.pack(">H", 14) + struct.pack(">3f", 0.2, 0.4, 0.6) + struct.pack(">H", 0)
surf += b"DIFF" + struct.pack(">H", 6) + struct.pack(">f", 1.0) + struct.pack(">H", 0)
body += lwo_chunk(b"SURF", surf)
with open("lwo/cube_hand.lwo", "wb") as f:
    f.write(b"FORM" + struct.pack(">I", len(body)) + body)
check("lwo/cube_hand.lwo", 12)  # assimp triangulates the six quads
assimp("lwo/cube_hand.lwo", "lwo/cube_hand_assimp_reference.obj", "obj")

# --- Quake II MD2 (Z-up, triangles clockwise from the front) ---------------------
skin_w, skin_h = 64, 64
scale, translate = (1 / 255, 1 / 255, 1 / 255), (-0.5, -0.5, -0.5)
frame = struct.pack("<3f3f", *scale, *translate) + b"frame1".ljust(16, b"\0")
for x, y, z in zup:
    frame += struct.pack("<4B", round((x + 0.5) * 255), round((y + 0.5) * 255), round((z + 0.5) * 255), 0)
st = b"".join(struct.pack("<2h", (i & 1) * 63, ((i >> 1) & 1) * 63) for i in range(8))
md2_tris = b"".join(struct.pack("<6H", a, c, b, a, c, b) for a, b, c in tris)  # reversed: clockwise
skins = b"skin.pcx".ljust(64, b"\0")
header_size = 68
ofs_skins = header_size
ofs_st = ofs_skins + len(skins)
ofs_tris = ofs_st + len(st)
ofs_frames = ofs_tris + len(md2_tris)
ofs_gl = ofs_frames + len(frame)
glcmds = struct.pack("<i", 0)  # an empty OpenGL command list (just the terminator)
ofs_end = ofs_gl + len(glcmds)
header = b"IDP2" + struct.pack("<16i", 8, skin_w, skin_h, len(frame), 1, 8, 8, len(tris), 1, 1, ofs_skins, ofs_st, ofs_tris,
                                ofs_frames, ofs_gl, ofs_end)
with open("md2/cube_hand.md2", "wb") as f:
    f.write(header + skins + st + md2_tris + frame + glcmds)
check("md2/cube_hand.md2", 12)
assimp("md2/cube_hand.md2", "md2/cube_hand_assimp_reference.obj", "obj")

# --- Quake III MD3 (Z-up, clockwise) ---------------------------------------------
def md3_surface():
    tri_data = b"".join(struct.pack("<3i", a, c, b) for a, b, c in tris)
    shader = b"textures/cube".ljust(64, b"\0") + struct.pack("<i", 0)
    st_data = b"".join(struct.pack("<2f", (i & 1) * 1.0, ((i >> 1) & 1) * 1.0) for i in range(8))
    xyz = b"".join(struct.pack("<3hH", round(x * 64), round(y * 64), round(z * 64), 0) for x, y, z in zup)
    head = 4 + 64 + 4 * 10
    ofs_tris = head
    ofs_shaders = ofs_tris + len(tri_data)
    ofs_st = ofs_shaders + len(shader)
    ofs_xyz = ofs_st + len(st_data)
    ofs_end = ofs_xyz + len(xyz)
    return (b"IDP3" + b"Cube".ljust(64, b"\0") + struct.pack("<10i", 0, 1, 1, 8, len(tris), ofs_tris, ofs_shaders, ofs_st, ofs_xyz, ofs_end)
            + tri_data + shader + st_data + xyz)


surface = md3_surface()
frame3 = struct.pack("<3f3f3ff", -0.5, -0.5, -0.5, 0.5, 0.5, 0.5, 0, 0, 0, 0.87) + b"frame".ljust(16, b"\0")
head = 4 + 4 + 64 + 4 * 9
ofs_frames = head
ofs_tags = ofs_frames + len(frame3)
ofs_surfaces = ofs_tags
ofs_eof = ofs_surfaces + len(surface)
md3 = b"IDP3" + struct.pack("<i", 15) + b"cube.md3".ljust(64, b"\0") + struct.pack("<9i", 0, 1, 0, 1, 0, ofs_frames, ofs_tags,
                                                                                     ofs_surfaces, ofs_eof)
with open("md3/cube_hand.md3", "wb") as f:
    f.write(md3 + frame3 + surface)
check("md3/cube_hand.md3", 12)
assimp("md3/cube_hand.md3", "md3/cube_hand_assimp_reference.obj", "obj")

# --- MilkShape 3D (Y-up, counter-clockwise, per-corner normals and uvs) ----------
ms = b"MS3D000000" + struct.pack("<i", 4)
ms += struct.pack("<H", 8) + b"".join(struct.pack("<B3fbB", 0, x, y, z, -1, 0) for x, y, z in cube["positions"])
ms += struct.pack("<H", len(tris))
P = np.array(cube["positions"])
for a, b, c in tris:
    n = np.cross(P[b] - P[a], P[c] - P[a])
    n = n / np.linalg.norm(n)
    ms += struct.pack("<H3H", 0, a, b, c) + struct.pack("<9f", *n, *n, *n)
    ms += struct.pack("<3f", *[(i & 1) * 1.0 for i in (a, b, c)]) + struct.pack("<3f", *[((i >> 1) & 1) * 1.0 for i in (a, b, c)])
    ms += struct.pack("<BB", 1, 0)
ms += struct.pack("<H", 1) + struct.pack("<B", 0) + b"Cube".ljust(32, b"\0") + struct.pack("<H", len(tris))
ms += b"".join(struct.pack("<H", i) for i in range(len(tris))) + struct.pack("<b", 0)
ms += struct.pack("<H", 1) + b"Blue".ljust(32, b"\0") + struct.pack("<4f4f4f4fff", 0.2, 0.2, 0.2, 1, 0.1, 0.2, 0.9, 1, 0, 0, 0, 1,
                                                                    0, 0, 0, 1, 32.0, 1.0)
ms += struct.pack("<b", 0) + b"".ljust(128, b"\0") + b"".ljust(128, b"\0")
ms += struct.pack("<ffi", 24.0, 0.0, 1) + struct.pack("<H", 0)  # animation fps, current time, frames; no joints
with open("ms3d/cube_hand.ms3d", "wb") as f:
    f.write(ms)
check("ms3d/cube_hand.ms3d", 12)
assimp("ms3d/cube_hand.ms3d", "ms3d/cube_hand_assimp_reference.obj", "obj")

# --- MagicaVoxel via py-vox-io: a 3x2x1 bar and an L of three voxels --------------
from pyvox.models import Vox  # noqa: E402
from pyvox.writer import VoxWriter  # noqa: E402

dense = np.zeros((4, 4, 4), dtype=np.uint8)  # indexed [z][y][x] by py-vox-io's from_dense
dense[0, 0:2, 0:3] = 1                          # 6 voxels, colour index 1
dense[1:3, 3, 3] = 2                            # 2 voxels standing up
dense[0, 3, 3] = 2                              # + 1 at the foot: an L of 3
VoxWriter("vox/shapes_pyvox.vox", Vox.from_dense(dense)).write()
for leftover in ("lwo", "md2", "md3", "ms3d"):
    for name in os.listdir(leftover):
        if name.endswith(".mtl"):
            os.remove(os.path.join(leftover, name))
print("done")
