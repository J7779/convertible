# Sources: science/maker

Synthetic engineering files (`gen_maker.py`, Python 3 virtual environment).

| File | Contents | Tool / method |
| --- | --- | --- |
| `layer.gbr` | Gerber X2 copper layer in mm (FSLAX36Y36): rectangle pads 1.0 x 0.5 flashed at (0, 0) and (5, 0), a 0.6 circle via at (2.5, 3), a 0.2 trace (0,0)-(2.5,3)-(5,0) | gerber-writer 0.4.3.3 (`DataLayer`, `add_pad`, `add_traces_path`, `dumps_gerber()`); the `TF.CreationDate` comment was then set to 2000-01-01T00:00:00 with `sed` |
| `drill.drl` | Excellon, METRIC,TZ: T1 0.3 mm at (2.5, 3) and (0, 0); T2 1.0 mm at (5, 0) | written by hand from the Excellon format description (no independent writer was available) |
| `square.gcode` | CNC program in mm: plunge to Z-1, 10 x 10 square sides, a clockwise G2 arc from (10, 10) to (0, 20) about (0, 10) (three quarters of a circle), back to (0, 0) | written by hand |

The expected values in `maker_test.cpp` (cutting distance 46 + 15 pi mm,
bounds, flash/draw counts) follow directly from the coordinates above.
