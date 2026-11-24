# Sources: mesh

Fixtures for `core/tests/families/mesh/`. Every model is one of the
canonical synthetic scenes of `core/tests/families/mesh/scenes.hpp`, built by
`tools/scenes_np.py` (the same numbers in Python) and exported by
independent, widely used software:

| Scene | Content |
| --- | --- |
| cube | unit cube [-0.5, 0.5]^3, 8 vertices, 6 quads, area 6, volume 1 |
| sphere | UV sphere radius 1, 16 segments x 8 rings: 114 vertices, 224 triangles |
| star | one concave 10-gon (5-pointed star, radii 1 and 0.4), area 1.17557 |
| quad | 2 x 2 quad facing +Z, uv 0..1, vertex colours red/green/blue/white, material "Checker" with a 4x4 PNG |
| two objects | "Box" = cube at (2, 0, 0) with material "Red" (0.8, 0.1, 0.1, metallic 0.25, roughness 0.6); "Ball" = sphere scaled 0.5 at (-2, 0, 0) |
| point grid | 10 x 10 points, z = 0.1 (x + y), sRGB colours (round(255 x/0.9), round(255 y/0.9), 128) |

No fixture was written by Convertible. Files that no available program can
write (LWO2, MD2, MD3, MS3D, AMF, the hand-made 3MF and the coloured STL)
were written with Python's `struct`/`zipfile` field by field from the
published specifications and are each cross-checked by an independent
decoder (assimp), as noted in their directories.

## How to regenerate

The tools run in a throwaway container built from `tools/Dockerfile`
(Ubuntu 24.04 with Blender 4.0.2 and Blender 4.1.1 from blender.org,
assimp 5.3.1, gmsh 4.12.1, and in a Python venv: numpy, trimesh, plyfile,
pygltflib 1.16, laspy 2.7, meshio 5.3, numpy-stl 4.1, usd-core 26.11,
py-vox-io 0.1, pymeshlab 2025.7, open3d 0.20). From this directory:

```sh
docker build --platform linux/amd64 -t cvt-meshfix tools
run() { docker run --rm --platform linux/amd64 --memory 2g -v "$PWD":/w -w /w cvt-meshfix "$@"; }
run blender41 -b --factory-startup --python tools/blender_export.py
run /venv/bin/python tools/off_fixtures.py
run /venv/bin/python tools/classic_fixtures.py      # needs Blender's quad OBJ in /tmp: run in one container with the line above
run /venv/bin/python tools/scene_fixtures.py
run /venv/bin/python tools/legacy_fixtures.py
run /venv/bin/python tools/pointcloud_fixtures.py
run /venv/bin/python tools/game_fixtures.py
run /venv/bin/python tools/usd_x3d_fixtures.py
python3 tools/scrub_fixtures.py   # removes container paths, author placeholders and export dates
```

`tools/scrub_fixtures.py` rewrites the temporary directory and the container
mount point that Blender records in COLLADA, X3D and binary FBX files with
same-length relative paths (so binary length fields stay valid), removes
`<author>` placeholders and fixes export dates. Every file was then checked
with `LC_ALL=C grep -raiE "<login>|/Users|/home/|\.local"` (no matches).

## Directories

Each directory's `SOURCES.md` lists its files, the tool, and what the tests check.

| Directory | Formats | Tools |
| --- | --- | --- |
| `obj/` | OBJ, OBJ+MTL in a ZIP | Blender 4.1, trimesh, MeshLab, Python zipfile |
| `stl/` | binary and ASCII STL, "solid" header, VisCAM colours | Blender 4.1, numpy-stl, Python struct |
| `ply/` | ASCII, binary LE/BE, lists, colours, face colours | Blender 4.1, plyfile, MeshLab |
| `off/` | OFF, NCOFF | MeshLab, trimesh |
| `gltf/` | GLB, glTF with data URIs and with a missing .bin, edge cases | Blender 4.1, pygltflib, trimesh, assimp |
| `3mf/` | 3MF | assimp, trimesh, Python zipfile (components, materials) |
| `amf/` | AMF plain and zipped | Python (spec), checked with assimp |
| `dae/` | COLLADA 1.4.1 | Blender 4.1, assimp |
| `fbx/` | FBX 7.4 binary (deflated arrays, embedded texture) and ASCII | Blender 4.1, assimp |
| `3ds/` | 3D Studio | Blender 4.1, assimp |
| `x3d/`, `wrl/` | X3D XML, VRML 2.0 | Blender 4.1, MeshLab, assimp |
| `usd/` | usda, usdz (text layer), usdz (binary crate) | Blender 4.1, usd-core |
| `xyz/`, `pcd/`, `las/` | point lists, PCD ascii/binary/compressed, LAS 1.2/1.4 | Open3D, laspy |
| `vtk/`, `msh/` | VTK legacy 4.2/5.1, Gmsh 2.2/4.1 | meshio, gmsh |
| `x/` | DirectX text | assimp |
| `lwo/`, `md2/`, `md3/`, `ms3d/` | LightWave, Quake II/III, MilkShape | Python (spec), checked with assimp |
| `vox/` | MagicaVoxel | py-vox-io |

## Interop of our output

Files written by Convertible (CLI, from `gltf/two_objects_blender.glb`,
`gltf/quad_blender.glb` and `ply/points_plyfile_ascii.ply`, into a scratch
directory) were loaded by the independent tools in the same container.
Expected: two objects, 236 triangles, x from -2.5 to 2.5; quad 2 triangles;
100 coloured points.

| Our output | Blender 4.1 import | assimp 5.3 | other |
| --- | --- | --- | --- |
| OBJ | 2 objects, 236 tris, x -2.5..2.5, material Red | 236 faces | trimesh, MeshLab: 236 faces, same bounds |
| STL (binary, ASCII) | 236 tris | 236 faces | trimesh, MeshLab |
| PLY | 236 tris | 236 faces | trimesh, MeshLab; points: Open3D 100 points, colours exact |
| OFF | - | 236 faces | trimesh, MeshLab |
| GLB / glTF | 2 objects, Red; quad with Checker texture | 236 faces | trimesh, pygltflib |
| GLB with KHR_mesh_quantization | 2 objects, 236 tris, same bounds | not supported by assimp | trimesh: same bounds |
| 3MF | - | 236 faces | trimesh: 236 faces, same bounds (Z-up) |
| AMF | - | reads the first volume only (assimp limitation); zipped AMF not read by assimp | - |
| COLLADA | 2 objects, Red | 236 faces | MeshLab |
| FBX (ASCII 7.4) | refused: Blender imports binary FBX only | 236 faces, quad 2 | - |
| 3DS | 2 objects, Red | 236 faces | MeshLab reads the first object only |
| X3D, VRML | 2 objects, 236 tris | X3D: 236 faces | MeshLab: 236 faces |
| USDA, USDZ | 2 objects, Red; quad.usdz with its texture | - | usd-core opens both, 2 meshes, 236 faces |
| VTK (ASCII, binary) | - | - | VTK 9 vtkPolyDataReader: 504 points, 236 polys, same bounds; points: 100 verts |
| XYZ, PCD (3 encodings) | - | - | Open3D: 100 points, colours equal |
| LAS 1.2 (PDRF 3), 1.4 (PDRF 7) | - | - | laspy: versions and formats as written, 100 points, colours |

Not available offline: the Khronos glTF validator (needs Node.js) and
usd-core's compliance checker (not in this usd-core build); our GLB output
is checked by the structural validation in `gltf_test.cpp` instead.
