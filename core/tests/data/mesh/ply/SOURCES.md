# Sources: mesh/ply

Written by tools/blender_export.py (`wm.ply_export`) and tools/classic_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender_ascii.ply` | cube, ASCII, normals | Blender 4.1.1 | counts, normals, shape |
| `sphere_blender_binary.ply` | sphere, binary little endian, Z-up | Blender 4.1.1 | shape |
| `star_blender_ascii.ply` | concave 10-gon | Blender 4.1.1 | one 10-sided face |
| `quad_blender_colors.ply` | quad, uchar RGBA colours (Blender writes 254 for full channels), float s t | Blender 4.1.1 | colours, uvs |
| `cube_plyfile_be.ply` | cube, binary big endian, double x/y/z, ushort colours, int8 `label`, float `quality`, uint lists with ushort counts, a comment | plyfile 1.1 | types, attributes, comment |
| `points_plyfile_ascii.ply` | point grid with uchar colours and float `intensity` | plyfile 1.1 (text) | point cloud, exact colours, attribute |
| `cube_meshlab_face_colors.ply` | cube, binary, per-face colours (red/green/blue pairs) | MeshLab (pymeshlab) | face colours split into vertex colours (20 vertices) |
