# Sources: mesh/ms3d

Written by tools/game_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_hand.ms3d` | MilkShape 3D v4 cube: per-corner normals and uvs, a group, material Blue | Python struct, per the MS3D 1.8 format; assimp reads 12 faces | volume +1, normals, uvs, material |
| `cube_hand_assimp_reference.obj` | assimp's decoding | assimp `export -fobj` | independent reference |
