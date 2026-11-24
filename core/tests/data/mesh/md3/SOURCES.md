# Sources: mesh/md3

Written by tools/game_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_hand.md3` | Quake III MD3 cube: one frame, one surface with a shader, 1/64 fixed-point vertices | Python struct, per the MD3 format; assimp reads 12 faces | volume +1, normals |
| `cube_hand_assimp_reference.obj` | assimp's decoding | assimp `export -fobj` | independent reference |
