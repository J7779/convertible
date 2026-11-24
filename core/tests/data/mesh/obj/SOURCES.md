# Sources: mesh/obj

Written by tools/blender_export.py (Blender `wm.obj_export`, Y-up, materials, vertex colours) and tools/classic_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender.obj` | cube, 6 quads with flat normals (24 corner vertices) | Blender 4.1.1 | counts, area 6, volume 1, shape |
| `cube_blender.mtl, sphere_blender.mtl, star_blender.mtl, two_objects_blender.mtl` | material libraries written alongside (not used by the plain-OBJ tests) | Blender 4.1.1 | - |
| `sphere_blender.obj` | UV sphere 16 x 8 (quads and triangles) | Blender 4.1.1 | 224 triangles, shape, outward volume |
| `star_blender.obj` | concave 10-gon | Blender 4.1.1 | one 10-sided face, area |
| `two_objects_blender.obj` | two objects `o Box`/`o Ball`, `usemtl Red` | Blender 4.1.1 | objects, groups, bounds, missing-MTL warning |
| `quad_blender.obj.zip` | Blender's textured quad (quad.obj, quad.mtl with map_Kd, checker.png) zipped | Blender 4.1.1 + Python zipfile | material, texture bytes (81-byte PNG), uvs, colours |
| `sphere_trimesh.obj` | UV sphere, triangles, vertex normals | trimesh `export_obj(include_normals=True)` | counts, normals |
| `cube_meshlab_colors.obj` | cube, 12 triangles, `v x y z r g b` colours (corner j: red = bit 0 of j ...) | MeshLab (pymeshlab) | colour per corner |
