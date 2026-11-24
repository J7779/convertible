# Sources: science/dicom

Synthetic DICOM files written with pydicom 3.0.2 (and Pillow 13.0.0 for the
JPEG frame) in a Python 3 virtual environment (`gen_dicom.py`). Patient and
institution fields hold obviously fake values (`Test^Patient`, `TEST-0001`,
birth date `19000101`, `Test Institution`) so that the privacy filter can be
tested; UIDs use a fixed example root. No real images.

| File | Contents |
| --- | --- |
| `ct_explicit_le.dcm` | CT Image Storage, 8x6 int16 (pixel k = 20k - 100), rescale intercept -1024, window centre -674 / width 400, a ReferencedImageSequence item, private element (0009,1010) "private value"; explicit VR little endian |
| `ct_implicit_le.dcm` | the same data set, implicit VR little endian |
| `ct_explicit_be.dcm` | the same, explicit VR big endian (retired syntax) |
| `ct_deflated_undefined_seq.dcm` | the same with the sequence and its item of undefined length, deflated explicit VR little endian |
| `mf_uncompressed.dcm` | Secondary Capture, 3 frames 5x4 uint8, frame k pixel i = i (k + 1) |
| `mf_rle.dcm` | the same compressed with pydicom's RLE Lossless encoder (`ds.compress(RLELossless, encoding_plugin="pydicom")`) |
| `rgb_planar.dcm` | 3x2 RGB, planar configuration 1: red, green, blue / (10,40,70), (20,50,80), (30,60,90) |
| `mono1.dcm` | 3x1 MONOCHROME1 values 0, 128, 255 |
| `palette.dcm` | 4x1 PALETTE COLOR, 16-bit LUTs: index 0 black, 1 red, 2 green, 3 blue |
| `jpeg_baseline.dcm` | 8x8 grey ramp (value 4 i) as one JPEG baseline frame (Pillow, quality 95), encapsulated with `pydicom.encaps.encapsulate` |

Interop of Convertible's output (checked while developing): Secondary Capture
files written from a FITS image, from `rgb_planar.dcm` and from `mf_rle.dcm`
open with `pydicom.dcmread(...).pixel_array` (MONOCHROME2 uint8 2x3, RGB uint8
2x3x3, Multi-frame Grayscale Byte SC 3x4x5) with empty patient name.
