# Sources: science/barcode

Reference module matrices (one text line per module row, `1` = dark, no
quiet zone) from independent encoders, written by `gen_barcodes.py` in a
Python 3 virtual environment. Commands run in this directory.

| File | Contents | Tool |
| --- | --- | --- |
| `qr_v1_q_m2.txt` | "HELLO WORLD", version 1, level Q, mask 2 | python-qrcode 8.2: `QRCode(version=1, error_correction=ERROR_CORRECT_Q, mask_pattern=2, border=0)`, `add_data(text, optimize=0)`, `make(fit=False)`, `get_matrix()` |
| `qr_v2_m_m5.txt` | "01234567890123456789" (numeric), version 2, level M, mask 5 | python-qrcode 8.2, as above |
| `qr_v7_h_m0.txt` | "Convertible test payload" (byte), version 7, level H, mask 0 (5 interleaved blocks, version information) | python-qrcode 8.2, as above |
| `qr_v10_l_m6.txt` | "byte mode: lower case", version 10, level L, mask 6 (4 blocks of two lengths) | python-qrcode 8.2, as above |
| `dm_digits.txt` | Data Matrix of "1234567890" (12x12) | zxing-cpp 3.1.1 `write_barcode(BarcodeFormat.DataMatrix, text, width=0, height=0, quiet_zone=0)` |
| `dm_digits40.txt` | Data Matrix of "1234567890" x 4 | zxing-cpp 3.1.1, as above |
| `code128.txt` | Code 128 of "Code128 test" (bars as `1`) | zxing-cpp 3.1.1, first pixel row |
| `ean13.txt` | EAN-13 of 590123412345 (+ check digit 7) | zxing-cpp 3.1.1, first pixel row |
| `upca.txt` | UPC-A of 03600029145 (+ check digit 2) | zxing-cpp 3.1.1, first pixel row |

segno 1.6.6 produces the same matrices for the single-block symbols but a
different (equally decodable) one for the multi-block versions 7 and 10;
python-qrcode and Convertible agree module for module there.

Interop of Convertible's output (checked while developing with
`check_barcodes.py`): PNG symbols written by the `convertible` CLI were decoded
by zxing-cpp 3.1.1 (`read_barcodes`) for 37/37 cases: QR in numeric,
alphanumeric and byte (UTF-8) mode, versions 1-40 (every third), all eight
masks, levels L/M/Q/H, a 2000-byte symbol; Data Matrix square, rectangular,
UTF-8 (ECI) and a 1000-character symbol; Code 128 with set switching; EAN-13;
UPC-A (reported by zxing as EAN-13 with a leading 0).
