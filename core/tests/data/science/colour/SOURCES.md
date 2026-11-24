# Sources: science/colour

Synthetic palettes and one colour profile (`gen_palettes.py`, Python 3
virtual environment). Commands run in this directory.

| File | Contents | Tool / method |
| --- | --- | --- |
| `swatches.ase` | "Warm Red" RGB (1, 0.2, 0); group "Grouped" with "Cyan Ink" CMYK (1, 0, 0, 0) and spot "Lab Mid" LAB (0.5, 20, -30); global "Half Grey" Gray 0.5 | `swatch` 0.4 (`swatch.write([...], "swatches.ase")`) |
| `swatches.aco` | Photoshop ACO version 1 + version 2: "pink" RGB (65535, 0, 32896), "green hsb" HSB (21845, 65535, 65535), "red cmyk" CMYK (inverted storage, C0 M100 Y100 K0), "lab" Lab (5000, 2000, -3000), "grey 25" Gray 2500 | written field by field with Python `struct.pack` from the Adobe Photoshop file format specification (no independent ACO writer was available); the same entries in a different container decode identically through `swatches.ase` for the Lab colour |
| `four.gpl` | GIMP palette "Four colours": 255 0 0 Red, 0 128 0 Dark green, 0 0 255 Blue, 250 250 210 Light goldenrod | written as text by the script; read back with Pillow 13.0.0 `GimpPaletteFile` |
| `four.pal` | the same four colours as JASC-PAL 0100 (CRLF) | written as text by the script |
| `four_riff.pal` | the same four colours as a Microsoft RIFF PAL (`data` chunk, version 0x0300) | `struct.pack` |
| `four.act` | the same four colours as a 772-byte Adobe Color Table (count 4, no transparent index) | `struct.pack` |
| `srgb_lcms.icc` | Little CMS built-in sRGB profile ("sRGB built-in", v4, parametric curves) | Pillow 13.0.0 `ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()`; `scrub_icc.py` then set the creation date to 2000-01-01 00:00:00 (the profile ID was already zero) |

Reference colour values in `palette_test.cpp` (sRGB -> Lab under D50 with
Bradford adaptation, and D65) were computed with colour-science 0.4.7
(`colour.sRGB_to_XYZ`, `chromatic_adaptation_VonKries(..., transform="Bradford")`,
`colour.XYZ_to_Lab`); the script prints them.

Interop of Convertible's output (checked while developing): an ASE written from
`swatches.aco` is read by `swatch.parse` with the original names, RGB, CMYK,
LAB and Gray models; a GPL written from `swatches.ase` is read by Pillow's
`GimpPaletteFile`.
