# Sources: mesh/vox

Written by tools/game_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `shapes_pyvox.vox` | 4x4x4 model: a 3x2x1 bar (colour 1) and a 3-voxel column (colour 2) | py-vox-io 0.1 (`Vox.from_dense`, `VoxWriter`) | volume 9, area 36, 12 greedy quads, two colours |
