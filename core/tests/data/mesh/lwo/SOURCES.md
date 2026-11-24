# Sources: mesh/lwo

Written by tools/game_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_hand.lwo` | LWO2 cube: TAGS, LAYR, PNTS (left-handed), POLS FACE, PTAG SURF, SURF with COLR/DIFF | Python struct, per the LightWave LWO2 specification; assimp reads 12 triangles | volume +1 (orientation), surface colour |
| `cube_hand_assimp_reference.obj` | assimp's decoding of cube_hand.lwo | assimp `export -fobj` | independent reference |
