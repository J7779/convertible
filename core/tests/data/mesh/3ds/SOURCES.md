# Sources: mesh/3ds

Written by tools/blender_export.py (`export_scene.max3ds`) and tools/game_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender.3ds` | cube with smoothing groups | Blender 4.1.1 | counts, volume, normals from smoothing |
| `quad_blender.3ds` | quad with material Checker and texture map | Blender 4.1.1 | material, texture name, uvs |
| `two_objects_blender.3ds` | two objects, material Red | Blender 4.1.1 | objects, shape |
| `sphere_assimp.3ds` | sphere | assimp `export -f3ds` | 224 triangles |
