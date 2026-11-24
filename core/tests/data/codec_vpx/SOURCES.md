# Sources: codec_vpx

VP8 and VP9 streams in IVF files, written by libvpx 1.14.0 (through
FFmpeg 6.1.1 `-c:v libvpx` / `libvpx-vp9`, and through libvpx's own `vpxenc`
for the two-pass alternate-reference clips), with ffmpeg's `framemd5` of the
decoded pictures next to each one. FFmpeg decodes VP8/VP9 with its own
decoders, independent of libvpx; both are bit-exact, so the tests require an
exact MD5 match for every frame. The content is FFmpeg's synthetic
`testsrc2` pattern (moving colour bars, gradients and a counter).

Tools: Ubuntu 24.04 packages `ffmpeg` (6.1.1-3ubuntu5) and `vpx-tools`
(libvpx 1.14.0), run in a throwaway container. Commands are run in this
directory, in this order. Shorthands:

```sh
F="ffmpeg -hide_banner -loglevel error -y"
COMMON="-map_metadata -1 -fflags +bitexact -flags:v +bitexact"
```

| File | Contents | Command |
| --- | --- | --- |
| `vp8_64x64.ivf` | VP8, 64x64, 12 frames, 25 fps, one keyframe | `$F -f lavfi -i testsrc2=size=64x64:rate=25 -frames:v 12 -pix_fmt yuv420p -c:v libvpx -crf 10 -b:v 300k -auto-alt-ref 0 -g 30 $COMMON vp8_64x64.ivf` |
| `vp8_altref_96x64.ivf` | VP8, 96x64, 20 frames in 21 packets: packet 1 is an invisible alternate reference frame with the same timestamp as packet 2 | `$F -f lavfi -i testsrc2=size=96x64:rate=25 -frames:v 20 -pix_fmt yuv420p -f yuv4mpegpipe src_96x64.y4m` then `vpxenc --codec=vp8 --passes=2 --good --cpu-used=1 --target-bitrate=300 --auto-alt-ref=1 --lag-in-frames=16 --kf-max-dist=60 --ivf -q -o vp8_altref_96x64.ivf src_96x64.y4m` |
| `vp8_odd_33x19.ivf` | VP8, 33x19 (odd), 10 frames | `$F -f lavfi -i testsrc2=size=64x48:rate=25 -vf scale=33:19 -frames:v 10 -pix_fmt yuv420p -c:v libvpx -crf 10 -b:v 100k -auto-alt-ref 0 $COMMON vp8_odd_33x19.ivf` |
| `vp9_p0_altref_176x144.ivf` | VP9 profile 0, 176x144, 20 frames, 30 fps, BT.709 signalled; packets 1 and 13 are superframes holding a hidden alternate reference frame | `$F -f lavfi -i testsrc2=size=176x144:rate=30 -frames:v 20 -pix_fmt yuv420p -f yuv4mpegpipe src_176x144.y4m` then `vpxenc --codec=vp9 --profile=0 --passes=2 --good --cpu-used=1 --end-usage=q --cq-level=34 --auto-alt-ref=1 --lag-in-frames=16 --kf-max-dist=60 --tile-columns=0 --color-space=bt709 --ivf -q -o vp9_p0_altref_176x144.ivf src_176x144.y4m` |
| `vp9_p2_10bit_64x64.ivf` | VP9 profile 2 (10 bit 4:2:0), 64x64, 10 frames | `$F -f lavfi -i testsrc2=size=64x64:rate=25 -frames:v 10 -pix_fmt yuv420p10le -c:v libvpx-vp9 -crf 30 -b:v 0 -row-mt 0 $COMMON vp9_p2_10bit_64x64.ivf` |
| `vp9_p1_444_64x48.ivf` | VP9 profile 1 (8 bit 4:4:4), full range, 64x48, 8 frames | `$F -f lavfi -i testsrc2=size=64x48:rate=25 -frames:v 8 -pix_fmt yuv444p -color_range pc -c:v libvpx-vp9 -crf 30 -b:v 0 -row-mt 0 $COMMON vp9_p1_444_64x48.ivf` |
| `vp9_odd_65x47.ivf` | VP9 profile 0, 65x47 (odd), 10 frames | `$F -f lavfi -i testsrc2=size=96x64:rate=25 -vf scale=65:47 -frames:v 10 -pix_fmt yuv420p -c:v libvpx-vp9 -crf 30 -b:v 0 -row-mt 0 $COMMON vp9_odd_65x47.ivf` |
| `vp9_lossless_64x64.ivf` | VP9 lossless, 64x64, 6 frames | `$F -f lavfi -i testsrc2=size=64x64:rate=25 -frames:v 6 -pix_fmt yuv420p -c:v libvpx-vp9 -lossless 1 -row-mt 0 $COMMON vp9_lossless_64x64.ivf` |
| `vp9_resize.ivf` | VP9, 6 frames at 64x64 then 6 frames at 96x48 (second keyframe at the new size) | two encodes, `$F -f lavfi -i testsrc2=size=64x64:rate=25 -frames:v 6 -pix_fmt yuv420p -c:v libvpx-vp9 -crf 30 -b:v 0 -auto-alt-ref 0 -row-mt 0 $COMMON part_a.ivf` and the same with `size=96x48` into `part_b.ivf`, joined frame by frame by the Python script below: `python3 ivfcat.py part_a.ivf part_b.ivf vp9_resize.ivf` |
| `vp9_lossless_source.framemd5` | MD5s of the 6 uncompressed source pictures of `vp9_lossless_64x64.ivf` | `$F -f lavfi -i testsrc2=size=64x64:rate=25 -frames:v 6 -pix_fmt yuv420p -f framemd5 vp9_lossless_source.framemd5` |
| `<name>.framemd5` (one per `.ivf`) | per decoded frame: timestamp, duration, size and MD5 of the planes (Y, Cb, Cr; 16-bit little-endian words for 10 bit) | `$F -i <name>.ivf -autoscale 0 -f framemd5 <name>.framemd5` |

`ivfcat.py` (Python 3 standard library; not kept in the repository): copies
the 32-byte IVF header of the first file, appends the frames of every input
with their timestamps continued after the previous file's last one, and
writes the total frame count at offset 24.

```python
import struct, sys
def frames(path):
    data = open(path, "rb").read()
    pos = struct.unpack_from("<H", data, 6)[0]
    while pos + 12 <= len(data):
        size, pts = struct.unpack_from("<IQ", data, pos)
        yield pts, data[pos + 12:pos + 12 + size]
        pos += 12 + size
*inputs, output = sys.argv[1:]
header, out, count, offset = bytearray(open(inputs[0], "rb").read()[:32]), bytearray(), 0, 0
for path in inputs:
    last = -1
    for pts, payload in frames(path):
        out += struct.pack("<IQ", len(payload), pts + offset) + payload
        last, count = pts, count + 1
    offset += last + 1
struct.pack_into("<I", header, 24, count)
open(output, "wb").write(bytes(header) + bytes(out))
```

Every file was checked with `strings -a` and the repository's personal-data
scan; the `framemd5` headers name only the FFmpeg library version.

## Interop of our encoders' output

Checked while developing (not part of the test run): 176x144, 30 frames of a
moving synthetic pattern (gradients, a block of noise, sweeping bars),
encoded by our VP8 and VP9 encoders at the Balanced preset, written to IVF and
decoded by FFmpeg 6.1.1's native decoders. FFmpeg's output was byte-identical
to our decoder's for every file (so our streams decode identically in an
independent implementation), `ffprobe` reported the expected profile and 30
frames, and FFmpeg's `psnr` filter against the source gave (average dB):

| quality | VP8 | VP9 8 bit (profile 0) | VP9 10 bit (profile 2) |
| --- | --- | --- | --- |
| 20 | 30.5 | 30.9 | 30.8 |
| 50 | 36.5 | 37.7 | 37.9 |
| 75 | 41.0 | 44.1 | 44.6 |
| 90 | 44.5 | 49.9 | 50.3 |
| 100 | 59.5 (finest quantiser) | lossless (inf) | lossless (inf) |
