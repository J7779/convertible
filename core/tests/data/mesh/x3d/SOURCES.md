# Sources: mesh/x3d

Written by tools/blender_export.py (`export_scene.x3d`) and tools/usd_x3d_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender.x3d, quad_blender.x3d, two_objects_blender.x3d` | scenes (Blender turns them half a turn around Y) | Blender 4.1.1 | transforms, DEF materials, shape |
| `cube_meshlab.x3d` | cube with vertex colours | MeshLab (pymeshlab) | counts, colours |
| `sphere_assimp.x3d` | sphere | assimp `export -fx3d` | 224 triangles, winding |
