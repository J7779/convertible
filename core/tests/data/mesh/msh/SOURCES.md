# Sources: mesh/msh

Written by tools/pointcloud_fixtures.py (a .geo plate meshed with `gmsh -2`); see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `plate_gmsh22_ascii.msh, plate_gmsh22_binary.msh, plate_gmsh41_ascii.msh` | 2 x 1 plate, triangles, physical surface "Plate" | gmsh 4.12.1 (`-format msh22`, `-bin`, `msh41`) | area 2, bounds, physical name |
| `plate_quads_gmsh41.msh` | the plate recombined into quadrilaterals | gmsh 4.12.1 | quads |
