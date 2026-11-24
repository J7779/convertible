# FBX (ASCII), COLLADA and DirectX fixtures written by assimp from the Blender
# OBJ files (run from tests/data/mesh after tools/blender_export.py):
#   /venv/bin/python tools/legacy_fixtures.py
import os
import shutil
import subprocess

for d in ("fbx", "dae", "x"):
    os.makedirs(d, exist_ok=True)


def assimp(src, dst, fmt):
    tmp = "/tmp/" + os.path.basename(dst)
    subprocess.run(["assimp", "export", src, tmp, "-f" + fmt, "-tri"], check=True, stdout=subprocess.DEVNULL)
    shutil.move(tmp, dst)


assimp("obj/two_objects_blender.obj", "fbx/two_objects_assimp_ascii.fbx", "fbxa")
assimp("obj/cube_blender.obj", "fbx/cube_assimp_binary.fbx", "fbx")
assimp("obj/two_objects_blender.obj", "dae/two_objects_assimp.dae", "collada")
assimp("obj/cube_blender.obj", "x/cube_assimp.x", "x")
for f in ("fbx/two_objects_assimp_ascii.fbx", "dae/two_objects_assimp.dae", "x/cube_assimp.x"):
    out = subprocess.run(["assimp", "info", f], capture_output=True, text=True)
    print(f, [l.strip() for l in out.stdout.splitlines() if "Vertices" in l or "Faces" in l])
print("done")
