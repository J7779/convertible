# Sources: mesh/las

Written by tools/pointcloud_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `points_laspy_v12_f3.las` | LAS 1.2, point format 3 (GPS time + RGB), scale 0.001, offsets 1000/2000 | laspy 2.7 | coordinates, intensity, classification, GPS time, colours |
| `points_laspy_v14_f7.las` | LAS 1.4, point format 7 with an extra-bytes dimension (record longer than the format minimum) | laspy 2.7 | same, 1.4 header, longer records |
