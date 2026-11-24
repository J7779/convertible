# Sources: mesh/md2

Written by tools/game_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_hand.md2` | Quake II MD2 cube: one skin name, 8 st, 12 clockwise triangles, one compressed frame | Python struct, per the MD2 format; assimp reads 12 faces | volume +1, uvs, Z-up |
| `cube_hand_assimp_reference.obj` | assimp's decoding | assimp `export -fobj` | independent reference |
