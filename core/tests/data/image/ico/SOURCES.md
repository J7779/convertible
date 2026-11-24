# ICO, CUR and ICNS fixtures

Produced by `make_fixtures.py` (Pillow 13.0.0, ImageMagick 7.1.2-36, macOS
27.2 `iconutil`). The picture is an n x n alpha gradient: R = 255x/(n-1),
G = 255y/(n-1), B = 255 - 255(x+y)/(2n-2), A = 255(x+y)/(2n-2).

| File | Content | Command |
| --- | --- | --- |
| `multi.ico` | 48, 32 and 16 px 32-bit bitmap entries of the 48 px gradient | `magick a48.png -define icon:auto-resize=48,32,16` |
| `png_entry.ico` | one 64 px PNG-compressed entry | Pillow PNG + directory written by the script |
| `cursor.cur` | 48 px cursor | `magick a48.png CUR:` |
| `pal8_mask.ico` | 16x16 8-bit palette entry (entry i = (i, 255-i, 128), pixel `(x + 16y) % 256`) whose AND mask hides the left 4 columns | written by the script from the ICO/DIB specification |
| `iconset.icns` | 16, 32, 64, 128 px PNG entries (ic04/ic05/ic11/ic12/ic07) | `iconutil -c icns t.iconset` (iconset written by Pillow) |
| `rle.icns` | 32x32 `il32` run-length RGB (R = 8x, G = 8y, B = 77) with `l8mk` mask (top row transparent) | written by the script (Apple icon RLE) |
