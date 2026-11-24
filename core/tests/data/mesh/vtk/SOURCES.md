# Sources: mesh/vtk

Written by tools/pointcloud_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_meshio_42_ascii.vtk` | UNSTRUCTURED_GRID of 6 quads with a point scalar `height` | meshio 5.3 (`vtk42`, ASCII) | quads, scalars |
| `sphere_meshio_42_binary.vtk, sphere_meshio_51_binary.vtk` | sphere triangles, big-endian binary, classic cells and 5.1 OFFSETS/CONNECTIVITY | meshio 5.3 | both cell layouts |
