# Camera RAW fixtures

All files are synthetic: a picture of four flat colour patches (no
photograph, no real camera metadata; the make/model strings are made up).
They are written by `make_fixtures.py` in this directory (its own small
TIFF/DNG writer, its own lossless-JPEG and ARW2 block encoders, Pillow for
the JPEG previews and lossy tiles). Regenerate with:

    python3 -m venv venv && venv/bin/pip install numpy pillow imagecodecs rawpy
    venv/bin/python make_fixtures.py

Versions used: Python 3.14, numpy 2.5.5, Pillow 13.0.0, imagecodecs
2026.11.11 (liblj92 cross-check of a lossless JPEG tile), rawpy 0.22.2 with
LibRaw (independent reference decoder).

## The scene

Linear sRGB of the patches (top-left, top-right, bottom-left, bottom-right):
(0.60, 0.25, 0.10), (0.10, 0.45, 0.20), (0.12, 0.18, 0.62), (0.40, 0.40, 0.40).
The made-up camera has ColorMatrix1 (XYZ -> camera, D65)
`[0.80 -0.20 -0.05; -0.40 1.20 0.20; -0.05 0.15 0.60]` and AsShotNeutral
(0.5, 1.0, 0.7). Camera values are derived from the scene with the DNG
model (rows of ColorMatrix x XYZ-from-sRGB normalised to 1, then divided by
the white-balance multipliers), so a correct decoder returns the scene
values at the patch centres. Maker files without a colour profile return the
white-balanced camera RGB instead (values listed in the test).

## Independent verification

For every file marked "LibRaw", the script decodes the file with LibRaw
(rawpy) and asserts that LibRaw's raw sample array equals the values the
script wrote. The `.libraw.ppm` files are LibRaw's developed output
(`postprocess(use_camera_wb=True, output_color=sRGB, gamma=(1,1),
no_auto_bright=True, output_bps=16, highlight_mode=Clip, user_flip=0)`),
i.e. linear sRGB, 16 bit, covering the active area; the tests require our
linear output to be within 33 dB PSNR of it (differences are confined to
the patch edges, where demosaicing algorithms differ). LibRaw's output at
the patch centres equals the scene values to 4 decimals.

## Files

| File | Content | Checked with |
| --- | --- | --- |
| `dng_cfa_u16_strips.dng` | DNG 1.4, 64x40 RGGB CFA, 16-bit uncompressed in three strips, black 256, white 60000, ActiveArea (2,4,40,64), DefaultCrop origin (2,2) size 56x34; IFD0 is a 30x18 JPEG preview (YCbCr, compression 7), the RAW is its SubIFD | LibRaw raw values + `dng_cfa_u16_strips.libraw.ppm` |
| `dng_cfa_lj92_tiles.dng` | 48x32 GRBG CFA, 14-bit lossless JPEG in 16x16 tiles (each tile one frame of 8 x 16 with 2 components); predictors 1-7 across the tiles, tile 4 uses predictor 1 with a restart marker every row; black 512 | liblj92 (tile 0), LibRaw raw values + `dng_cfa_lj92_tiles.libraw.ppm` |
| `dng_cfa_packed12_rot90.dng` | 40x28 BGGR, 12-bit MSB-packed single strip, LinearizationTable (code v -> 2v), 2x2 BlackLevelRepeatDim (60, 64, 66, 70), white 4000, Orientation 6 | LibRaw raw values |
| `dng_cfa_xtrans.dng` | 36x24 Fujifilm-style 6x6 X-Trans CFA, 16-bit, black 1024, white 16383; ColorMatrix1 (D65) and ColorMatrix2 (illuminant A) identical | LibRaw raw values |
| `dng_cfa_gainmap.dng` | 32x24 RGGB 16-bit, black 0, white 65535, OpcodeList2 with two GainMap opcodes (constant 2x2 maps): x2 on the red photosites (row/column pitch 2) and x1.25 on all pixels; the stored data is darkened by the inverse | LibRaw raw values |
| `dng_linear_float_deflate.dng` | 32x24 LinearRaw RGB, 32-bit float, Deflate with the floating-point predictor (3), 16x16 tiles, ForwardMatrix1, BaselineExposure +1 (data stored at half exposure) | values only (LibRaw float DNG support not used) |
| `dng_linear_lossy_jpeg.dng` | 32x24 LinearRaw RGB, 8-bit lossy JPEG tiles (Pillow, quality 95, 4:4:4), white 255 | values (tolerance 0.03) |
| `cr2_lj92_slices.cr2` | Canon CR2 layout: "CR" header, IFD0 48x32 JPEG preview (compression 6), IFD1 16x10 thumbnail, IFD2 8x6 RGB, IFD3 14-bit lossless JPEG (28 x 36, 2 components) cut into slices 16/16/24 (tag 0xC640 = 2, 16, 24); sensor 56x36 with borders left 8 / top 4; Canon maker note with SensorInfo (0x00E0) and ColorData version 1 (0x4001, 582 values, as-shot RGGB levels at index 25); black 1024 in the masked columns | LibRaw raw values |
| `nef_uncompressed.nef` | Nikon NEF layout: IFD0 16x10 thumbnail, SubIFD 48x32 RGGB 14-bit in 16-bit containers, Nikon maker note ("Nikon\0" 02 10, embedded TIFF) with WB_RBLevels (0x000C), BlackLevel (0x003D, 600) and PreviewIFD (0x0011) holding a 40x26 JPEG | LibRaw raw values |
| `arw2_blocks.arw` | Sony ARW layout: IFD0 40x10 JPEG preview, SubIFD 64x24 RGGB compressed as ARW2 blocks (compression 32767, one byte per pixel), tone curve tag 0x7010 = 8000, 10400, 12800, 14400; no recorded white balance | LibRaw raw values (equal to the script's own block decoding) |
| `orf_preview.orf` | "IIRO" header, IFD0 64x48 12-bit strip of 1024 bytes (stands in for Olympus-compressed data), Olympus maker note ("OLYMPUS\0" II 03 00) with CameraSettings PreviewImageStart/Length -> 40x24 JPEG, Orientation 8 | preview path only |
| `rw2_preview.rw2` | "IIU" Panasonic header, IFD0 with the 40x24 JPEG in tag 0x2E | preview path only |
| `raf_preview.raf` | Fujifilm RAF header ("FUJIFILMCCD-RAW 0201", big-endian JPEG offset/length at 84/88) + 40x24 JPEG | preview path only |
| `cr3_preview.cr3` | ISO-BMFF with `ftyp crx `, `moov/trak/mdia/minf/stbl` (stsz, stco) pointing at a 40x24 JPEG in `mdat` | preview path only |
| `crw_preview.crw` | Canon CIFF header ("HEAPCCDR"), one heap with a JpgFromRaw (0x2007) record holding a 40x24 JPEG | preview path only |

The JPEG previews are 8-bit Pillow JPEGs (quality 85) whose left half is
(40, 90, 200) and right half the colour noted in the script.

## Real-camera coverage

No real camera files are included (privacy policy). The CR2, NEF and ARW
fixtures reproduce the documented container layouts and were accepted and
decoded identically by LibRaw, but per-model details of real files (Canon
sRAW/mRAW, CFA phase on unusual sensors, Nikon compressed NEF, Sony ARW1 /
encrypted SR2 data, Fujifilm compressed RAF, Canon CRX, Panasonic and
Olympus compression, Pentax Huffman PEF, Samsung SRW compression) are not
covered; those files fall back to their embedded previews.
