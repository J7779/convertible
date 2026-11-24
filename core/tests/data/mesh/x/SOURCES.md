# Sources: mesh/x

Written by tools/legacy_fixtures.py and tools/game_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_assimp.x` | cube | assimp `export -fx` | counts, volume, left-handed conversion |
| `quad_assimp.x` | textured quad from Blender's OBJ (material with TextureFilename) | assimp `export -fx` | material, texture name, uvs |
