# Sources: mesh/fbx

Written by tools/blender_export.py (`export_scene.fbx`, embedded media) and tools/legacy_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender.fbx, sphere_blender.fbx, star_blender.fbx` | binary FBX 7.4 with zlib-deflated arrays, centimetres | Blender 4.1.1 | units, axes, counts, shape |
| `quad_blender.fbx` | quad with embedded checker.png (Video Content), UVs, colours (texture paths scrubbed) | Blender 4.1.1 | material, embedded texture bytes |
| `two_objects_blender.fbx` | two models with Lcl transforms, material Red | Blender 4.1.1 | model transform chain, materials |
| `two_objects_assimp_ascii.fbx` | ASCII FBX 7.4 of the two-object OBJ | assimp `export -ffbxa` | ASCII parser, 236 triangles |
| `cube_assimp_binary.fbx` | binary FBX of the cube OBJ | assimp `export -ffbx` | 12 triangles, volume |
