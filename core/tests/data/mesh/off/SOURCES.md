# Sources: mesh/off

Written by `tools/off_fixtures.py` (see `../SOURCES.md` for the container).

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_meshlab.off` | unit cube, 8 vertices, 12 triangles, vertex colours (RGBA bytes, corner i has red = bit 0, green = bit 1, blue = bit 2) and MeshLab's own normals; header `NCOFF` with the colour columns BEFORE the normal columns | MeshLab (pymeshlab 2025.7) `save_current_mesh(..., save_vertex_color=True)` | counts, area 6, volume 1, shape against the generated cube, colours present |
| `sphere_trimesh.off` | UV sphere radius 1, 16 x 8, 114 vertices, 224 triangles | trimesh `Trimesh.export("...off")` | counts and bounds |
