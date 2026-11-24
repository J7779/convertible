# GIF fixtures

Produced by `make_fixtures.py` (Pillow 13.0.0, ImageMagick 7.1.2-36);
ImageMagick runs with `-strip`.

| File | Content | Command |
| --- | --- | --- |
| `palette.gif` | 32x24, 200 colours, entry i = (7i, 13i, 29i) mod 256, pixel `(5x + 3y) % 200` | Pillow mode P |
| `interlaced.gif` | the same, interlaced | `magick palette.gif -interlace GIF` |
| `palette87a.gif` | the same as GIF87a | `magick palette.gif GIF87:...` |
| `transparent.gif` | 4 columns of 8 px: black, red, green, transparent (index 3) | Pillow `transparency=3` |
| `gradient.gif` | `cvt::test::gradient_frame(32, 24)` reduced to 256 colours with dithering | `magick gradient.ppm gradient.gif` |
| `anim_disposal.gif` | 4 frames: red 32x24 (none, 10 cs); blue 8x8 at (8,4) (background, 20 cs); lime 8x8 at (16,8) (previous, 30 cs); white 4x4 at (0,0) (none, 40 cs); NETSCAPE loop value 2 = 3 plays | `magick -dispose ... -delay ... ( -size 8x8 xc:blue -repage 32x24+8+4 ) ... -loop 3` |
| `anim_pillow.gif` | 4 frames, background (20,40,60), yellow 8x8 square moving right 4 px per frame, 50 ms, loop forever | Pillow `save_all=True` (Pillow stores frame differences) |
