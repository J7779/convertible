# Sources: mesh/stl

Written by tools/blender_export.py (`wm.stl_export`) and tools/classic_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender_binary.stl, cube_blender_ascii.stl` | cube, 12 facets | Blender 4.1.1 | welded to 8 vertices, volume, binary = ASCII |
| `sphere_blender_binary.stl` | sphere, Z-up (Blender frame) | Blender 4.1.1 | shape after Z-up to Y-up |
| `sphere_numpystl_ascii.stl` | sphere, Y-up, ASCII | numpy-stl `save(mode=ASCII)` | shape |
| `cube_solid_header_binary.stl` | binary cube whose 80-byte header starts with `solid` | Python struct (hand-made) | binary detected despite the header |
| `cube_viscam_colors.stl` | binary cube, VisCAM/SolidView facet colours (bit 15 valid; red, green, blue face pairs) | Python struct (hand-made per the VisCAM convention) | vertex colours, splitting |
