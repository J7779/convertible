# Sources: mesh/dae

Written by tools/blender_export.py (`wm.collada_export`, Z_UP) and tools/legacy_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender.dae, sphere_blender.dae, star_blender.dae` | scenes as triangles | Blender 4.1.1 | Z-up conversion, counts, shape |
| `quad_blender.dae` | quad with a lambert effect sampling checker.png (path scrubbed) | Blender 4.1.1 | material, texture reference, uvs |
| `two_objects_blender.dae` | two nodes with matrices, bound material Red | Blender 4.1.1 | node transforms, materials, unit |
| `two_objects_assimp.dae` | the two-object OBJ | assimp `export -fcollada` | 236 triangles |
