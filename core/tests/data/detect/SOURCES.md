# Sources: detect

Real files for the detection tests of the known-formats database
(`core/tests/engine/known_formats_test.cpp`). `manifest.tsv` lists every
file with the format id expected when it is detected with its name and from
its content alone (`-` = nothing). The files are tiny synthetic samples: test
patterns (`testsrc`, a 16x16 red-blue gradient), a 440 Hz sine tone, "hello
archive" / "Lorem ipsum" text, made-up records. No file contains personal or
machine data: every fixture was scanned for user names, host names, home and
build paths and author fields; metadata was stripped at generation time.

Commands are shown as run in a scratch directory, with relative paths. Shell
variables used below:

```sh
FF="ffmpeg -nostdin -hide_banner -loglevel error -y"
BX="-map_metadata -1 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact"
V="-f lavfi -i testsrc=size=64x48:rate=10:duration=0.3"   # V25: same at 25 fps, 0.2 s
A="-f lavfi -i sine=frequency=440:duration=0.25:sample_rate=8000"
A44="-f lavfi -i sine=frequency=440:duration=0.1:sample_rate=44100"
I="-f lavfi -i testsrc=size=16x16:rate=1:duration=1 -frames:v 1"
LO="soffice --headless --norestore --nologo"
TAR="tar --owner=0 --group=0 --numeric-owner --mtime=@0 --sort=name"
CF="-g0 -O1 -ffile-prefix-map=<scratch>=. -fno-ident"     # macOS: CF=(-g0 -O1 -ffile-prefix-map=<scratch>=. -mmacosx-version-min=13.3)
g.png = magick -size 16x16 gradient:red-blue -strip g.png
tone.wav = 8 kHz mono 16-bit 440 Hz sine, 0.2 s, written with Python's wave module
SUBJ='/CN=example.invalid'; key.pem = openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out key.pem
mk OUT MEMBERS... = create each member as a one-line placeholder file dated 1980-01-01, then `zip -X -q -r OUT MEMBERS...`
```

Tools. Debian 13 container: FFmpeg 7.1.5, SoX 14.4.2, ImageMagick 7.1.1-43,
LibreOffice 25.2.3.2, GDAL 3.10.3, GCC 14.2, MinGW-w64 GCC 14, clang 19.1.7,
OpenJDK 21.0.13, FontForge 20230101 with fontTools 4.57, Python 3.13 with
NumPy 2.2.4 / SciPy 1.15.3 / h5py 3.13 / Scapy 2.6.1, GNU tar 1.35, xz 5.8.1,
zstd 1.5.7, lz4 1.10.0, lzip 1.25, lzop (LZO 2.10), brotli 1.1.0, gcab 1.6,
7-Zip 25.01, bsdtar 3.7.4, ARJ, genisoimage 1.1.11, qemu-img 10.0.13,
squashfs-tools 4.6.1, mtd-utils, util-linux sfdisk, dpkg-deb 1.22.22,
Wireshark text2pcap 4.4.19, DCMTK 3.6.9, Lua 5.4.7. macOS 27.2: sips,
afconvert, textutil, plutil, iconutil, hdiutil, Apple clang 21.0.0,
ImageMagick 7.1.2-36, OpenSSL 4.1.0, SQLite 3.54.0, OpenSSH ssh-keygen 10.5,
Info-ZIP zip 3.0, Python 3.14 standard library.

Files under `package/`, `model/sample.*`, `office/sample.*`, `image/sample.sketch`
and `data/sample.{apkg,pt,keras}` are *structural samples*: ZIP archives
written by Info-ZIP whose member names are the ones that define the package
type (their member contents are placeholders). They exercise the ZIP
classifier; they are not complete packages. `security/private_*`, `security/id_test` and `security/bundle.pfx` are
cut to their first bytes so that no usable key material is committed.

| File | Made on | Command | Note | Expected (named / unnamed) |
| --- | --- | --- | --- | --- |
| `video/clip.mp4` | Debian 13 container | `$FF $V $A $BX -c:v libx264 -preset ultrafast -c:a aac -b:a 16k -shortest clip.mp4` |  | mp4 / mp4 |
| `video/clip.mov` | Debian 13 container | `$FF $V $A $BX -c:v libx264 -preset ultrafast -c:a aac -b:a 16k -shortest clip.mov` |  | mov / mov |
| `video/clip.m4v` | Debian 13 container | `$FF $V $A $BX -c:v libx264 -preset ultrafast -c:a aac -b:a 16k -shortest -f ipod clip.m4v` |  | m4v / m4v |
| `video/clip.3gp` | Debian 13 container | `$FF $V $A $BX -c:v mpeg4 -s 176x144 -c:a aac -b:a 16k -shortest clip.3gp` |  | 3gp / 3gp |
| `video/clip.3g2` | Debian 13 container | `$FF $V $A $BX -c:v mpeg4 -s 176x144 -c:a aac -b:a 16k -shortest clip.3g2` |  | 3g2 / 3g2 |
| `video/clip.f4v` | Debian 13 container | `$FF $V $A $BX -c:v libx264 -preset ultrafast -c:a aac -b:a 16k -shortest clip.f4v` |  | f4v / f4v |
| `video/clip.ismv` | Debian 13 container | `$FF $V $A $BX -c:v mpeg4 -c:a aac -shortest -f ismv clip.ismv` |  | ismv / ismv |
| `video/clip.mkv` | Debian 13 container | `$FF $V $A $BX -c:v libx264 -preset ultrafast -c:a libvorbis -shortest clip.mkv` |  | mkv / mkv |
| `video/clip.mk3d` | Debian 13 container | `$FF $V $BX -c:v libx264 -preset ultrafast -an -f matroska -metadata:s:v stereo_mode=left_right clip.mk3d` |  | mk3d / mkv |
| `video/clip.webm` | Debian 13 container | `$FF $V $A $BX -c:v libvpx -b:v 50k -c:a libopus -ar 48000 -shortest clip.webm` |  | webm / webm |
| `video/clip.avi` | Debian 13 container | `$FF $V $A $BX -c:v mpeg4 -c:a pcm_u8 -shortest clip.avi` |  | avi / avi |
| `video/clip.wmv` | Debian 13 container | `$FF $V $A $BX -c:v wmv2 -c:a wmav2 -b:a 32k -ar 22050 -shortest clip.wmv` |  | wmv / wmv |
| `video/clip.asf` | Debian 13 container | `$FF $V $A $BX -c:v wmv2 -c:a wmav2 -ar 22050 -shortest -f asf clip.asf` |  | asf / wmv |
| `video/clip.flv` | Debian 13 container | `$FF $V $A $BX -c:v flv1 -c:a mp3 -ar 22050 -shortest clip.flv` |  | flv / flv |
| `video/clip.swf` | Debian 13 container | `$FF $V $BX -c:v flv1 -an clip.swf` |  | swf / swf |
| `video/clip.mpg` | Debian 13 container | `$FF $V25 $A $BX -c:v mpeg1video -c:a mp2 -shortest -f mpeg clip.mpg` |  | mpg / mpg |
| `video/clip.vob` | Debian 13 container | `$FF $V $A $BX -c:v mpeg2video -c:a ac3 -ar 48000 -shortest -f vob clip.vob` |  | vob / mpg |
| `video/clip.ts` | Debian 13 container | `$FF $V $A $BX -c:v mpeg2video -c:a mp2 -ar 48000 -shortest -f mpegts clip.ts` |  | mpegts / mpegts |
| `video/clip.m2ts` | Debian 13 container | `$FF $V $A $BX -c:v mpeg2video -c:a mp2 -ar 48000 -shortest -f mpegts -mpegts_m2ts_mode 1 clip.m2ts` |  | m2ts / m2ts |
| `video/clip.mxf` | Debian 13 container | `$FF -f lavfi -i testsrc=size=720x576:rate=25:duration=0.04 $A $BX -c:v mpeg2video -pix_fmt yuv422p -b:v 10M -c:a pcm_s16le -shortest -f mxf clip.mxf` | first 4096 bytes kept (all that detection reads) | mxf / mxf |
| `video/clip.gxf` | Debian 13 container | `$FF -f lavfi -i testsrc=size=720x576:rate=25:duration=0.04 $BX -c:v mpeg2video -an -f gxf clip.gxf` | first 4096 bytes kept (all that detection reads) | gxf / gxf |
| `video/clip.dv` | Debian 13 container | `$FF -f lavfi -i testsrc=size=720x576:rate=25:duration=0.04 $BX -pix_fmt yuv420p -c:v dvvideo -an -f dv clip.dv` | first 4096 bytes kept (all that detection reads) | dv / dv |
| `video/clip.h264` | Debian 13 container | `$FF $V $BX -c:v libx264 -preset ultrafast -an -f h264 clip.h264` |  | h264 / h264 |
| `video/clip.hevc` | Debian 13 container | `$FF $V $BX -c:v libx265 -preset ultrafast -x265-params log-level=none -an -f hevc clip.hevc` |  | hevc / hevc |
| `video/clip.m1v` | Debian 13 container | `$FF $V25 $BX -c:v mpeg1video -an -f mpeg1video clip.m1v` |  | m1v / m1v |
| `video/clip.m2v` | Debian 13 container | `$FF $V $BX -c:v mpeg2video -an -f mpeg2video clip.m2v` |  | m2v / m2v |
| `video/clip.y4m` | Debian 13 container | `$FF -f lavfi -i testsrc=size=16x16:rate=5:duration=0.2 $BX -pix_fmt yuv420p -f yuv4mpegpipe clip.y4m` |  | y4m / y4m |
| `video/clip.ivf` | Debian 13 container | `$FF $V $BX -c:v libvpx -b:v 50k -an -f ivf clip.ivf` |  | ivf / ivf |
| `video/clip.nut` | Debian 13 container | `$FF $V $A $BX -c:v mpeg4 -c:a pcm_s16le -shortest -f nut clip.nut` |  | nut / nut |
| `video/clip.ogv` | Debian 13 container | `$FF $V $A $BX -c:v libtheora -c:a libvorbis -shortest clip.ogv` |  | ogv / ogv |
| `video/clip.rm` | Debian 13 container | `$FF $V $A $BX -c:v rv10 -s 64x48 -c:a ac3 -ar 48000 -shortest -f rm clip.rm` |  | rm / rm |
| `video/stream.mpd` | Debian 13 container | `mkdir -p dash && $FF -f lavfi -i testsrc=size=64x48:rate=10:duration=0.3 $BX -c:v libx264 -preset ultrafast -an -f dash dash/stream.mpd && ls dash >&2 && cp dash/chunk-stream0-00001.m4s segment.m4s` |  | mpd / mpd |
| `video/segment.m4s` | Debian 13 container | `mkdir -p dash && $FF -f lavfi -i testsrc=size=64x48:rate=10:duration=0.3 $BX -c:v libx264 -preset ultrafast -an -f dash dash/stream.mpd && ls dash >&2 && cp dash/chunk-stream0-00001.m4s segment.m4s` |  | m4s / m4s |
| `video/index.m3u8` | Debian 13 container | `mkdir -p hls && $FF $V $BX -c:v libx264 -preset ultrafast -an -f hls hls/index.m3u8 && cp hls/index.m3u8 index.m3u8` |  | m3u8 / m3u8 |
| `audio/tone.mp3` | Debian 13 container | `$FF $A $BX -c:a libmp3lame -b:a 32k tone.mp3` |  | mp3 / mp3 |
| `audio/tone_id3.mp3` | Debian 13 container | `$FF $A $BX -c:a libmp3lame -b:a 32k -write_xing 0 -id3v2_version 3 -metadata title=tone tone_id3.mp3` |  | mp3 / mp3 |
| `audio/tone.mp2` | Debian 13 container | `$FF $A $BX -c:a mp2 -ar 32000 tone.mp2` |  | mp2 / mp2 |
| `audio/tone.aac` | Debian 13 container | `$FF $A $BX -c:a aac -b:a 16k -f adts tone.aac` |  | aac / aac |
| `audio/tone.m4a` | Debian 13 container | `$FF $A $BX -c:a aac -b:a 16k -f ipod tone.m4a` |  | m4a / m4a |
| `audio/tone.flac` | Debian 13 container | `$FF $A $BX -c:a flac tone.flac` |  | flac / flac |
| `audio/tone.ogg` | Debian 13 container | `$FF $A $BX -c:a libvorbis tone.ogg` |  | ogg / ogg |
| `audio/tone.opus` | Debian 13 container | `$FF $A $BX -c:a libopus -ar 48000 tone.opus` |  | opus / opus |
| `audio/tone.spx` | Debian 13 container | `$FF $A $BX -c:a libspeex tone.spx` |  | spx / spx |
| `audio/tone_flac.oga` | Debian 13 container | `$FF $A $BX -c:a flac -f ogg tone.oga` |  | oga / oga |
| `audio/tone.wav` | Debian 13 container | `$FF $A $BX -c:a pcm_s16le tone.wav` |  | wav / wav |
| `audio/tone.w64` | Debian 13 container | `$FF $A $BX -c:a pcm_s16le -f w64 tone.w64` |  | w64 / w64 |
| `audio/tone_rf64.wav` | Debian 13 container | `$FF $A $BX -c:a pcm_s16le -rf64 always tone_rf64.wav` |  | rf64 / rf64 |
| `audio/tone.aiff` | Debian 13 container | `$FF $A $BX -c:a pcm_s16be tone.aiff` |  | aiff / aiff |
| `audio/tone.au` | Debian 13 container | `$FF $A $BX -c:a pcm_s16be -f au tone.au` |  | au / au |
| `audio/tone.caf` | Debian 13 container | `$FF $A $BX -c:a pcm_s16le -f caf tone.caf` |  | caf / caf |
| `audio/tone.wma` | Debian 13 container | `$FF $A $BX -c:a wmav2 -ar 22050 tone.wma` |  | wma / wma |
| `audio/tone.ac3` | Debian 13 container | `$FF $A44 $BX -c:a ac3 -ar 48000 tone.ac3` |  | ac3 / ac3 |
| `audio/tone.eac3` | Debian 13 container | `$FF $A44 $BX -c:a eac3 -ar 48000 tone.eac3` |  | eac3 / eac3 |
| `audio/tone.dts` | Debian 13 container | `$FF $A44 $BX -strict -2 -c:a dca -ar 48000 tone.dts` | first 4096 bytes kept (all that detection reads) | dts / dts |
| `audio/tone.thd` | Debian 13 container | `$FF $A44 $BX -strict -2 -c:a truehd -ar 48000 -f truehd tone.thd` | first 4096 bytes kept (all that detection reads) | thd / thd |
| `audio/tone.mlp` | Debian 13 container | `$FF $A44 $BX -strict -2 -c:a mlp -ar 48000 -f mlp tone.mlp` | first 4096 bytes kept (all that detection reads) | mlp / mlp |
| `audio/tone.wv` | Debian 13 container | `$FF $A $BX -c:a wavpack tone.wv` |  | wv / wv |
| `audio/tone.tta` | Debian 13 container | `$FF $A $BX -c:a tta tone.tta` |  | tta / tta |
| `audio/tone.mka` | Debian 13 container | `$FF $A $BX -c:a libvorbis tone.mka` |  | mka / mka |
| `audio/tone.voc` | Debian 13 container | `$FF $A $BX -c:a pcm_u8 -f voc tone.voc` |  | voc / voc |
| `audio/tone.gsm` | Debian 13 container | `$FF $A $BX -c:a libgsm -ar 8000 -ac 1 -f gsm tone.gsm` |  | gsm / - |
| `audio/tone.8svx` | Debian 13 container | `sox -n -r 8000 -c 1 -b 8 tone.8svx synth 0.05 sine 440` |  | 8svx / 8svx |
| `audio/tone.sph` | Debian 13 container | `sox -n -r 8000 -c 1 -b 16 -e signed tone.sph synth 0.05 sine 440` |  | sph / sph |
| `audio/python_tone.wav` | macOS 27.2 | `cp tone.wav python_tone.wav` |  | wav / wav |
| `audio/afconvert.m4a` | macOS 27.2 | `afconvert -f m4af -d aac tone.wav afconvert.m4a` |  | m4a / m4a |
| `audio/afconvert.m4b` | macOS 27.2 | `afconvert -f m4bf -d aac tone.wav afconvert.m4b` |  | m4a / m4a |
| `audio/afconvert.caf` | macOS 27.2 | `afconvert -f caff -d LEI16 tone.wav afconvert.caf >/dev/null 2>&1` |  | caf / caf |
| `audio/afconvert.aiff` | macOS 27.2 | `afconvert -f AIFF -d BEI16 tone.wav afconvert.aiff >/dev/null 2>&1` |  | aiff / aiff |
| `audio/afconvert.aifc` | macOS 27.2 | `afconvert -f AIFC -d ulaw tone.wav afconvert.aifc >/dev/null 2>&1` |  | aiff / aiff |
| `audio/afconvert.3gp` | macOS 27.2 | `afconvert -f 3gpp -d aac tone.wav afconvert.3gp` |  | 3gp / 3gp |
| `audio/afconvert.aac` | macOS 27.2 | `afconvert -f adts -d aac tone.wav afconvert.aac` |  | aac / aac |
| `subtitle/subs.ass` | Debian 13 container | `$FF -i in.srt subs.ass` |  | ass / ass |
| `subtitle/subs.ssa` | Debian 13 container | `$FF -i in.srt -c:s ssa subs.ssa` |  | ass / ass |
| `subtitle/subs.vtt` | Debian 13 container | `$FF -i in.srt subs.vtt` |  | vtt / vtt |
| `subtitle/subs.ttml` | Debian 13 container | `$FF -i in.srt -c:s ttml -f ttml subs.ttml` |  | ttml / ttml |
| `subtitle/subs.lrc` | Debian 13 container | `$FF -i in.srt -f lrc subs.lrc` |  | lrc / lrc |
| `subtitle/subs.srt` | Debian 13 container | `$FF -i in.srt subs.srt` |  | srt / srt |
| `subtitle/subs.mks` | Debian 13 container | `$FF -i in.srt -c:s srt -f matroska subs.mks` |  | mks / mks |
| `image/anim.apng` | Debian 13 container | `$FF $V $BX -c:v apng -f apng anim.apng` |  | apng / apng |
| `image/anim.gif` | Debian 13 container | `$FF $V $BX -c:v gif -f gif anim.gif` |  | gif / gif |
| `image/pattern.png` | Debian 13 container | `$FF $I $BX pattern.png` |  | png / png |
| `image/pattern.jpg` | Debian 13 container | `$FF $I $BX pattern.jpg` |  | jpg / jpg |
| `image/pattern.bmp` | Debian 13 container | `$FF $I $BX pattern.bmp` |  | bmp / bmp |
| `image/pattern.tiff` | Debian 13 container | `$FF $I $BX pattern.tiff` |  | tiff / tiff |
| `image/pattern.tga` | Debian 13 container | `$FF $I $BX pattern.tga` |  | tga / tga |
| `image/pattern.pcx` | Debian 13 container | `$FF $I $BX pattern.pcx` |  | pcx / pcx |
| `image/pattern.pbm` | Debian 13 container | `$FF $I $BX -pix_fmt monob pattern.pbm` |  | pbm / pbm |
| `image/pattern.pgm` | Debian 13 container | `$FF $I $BX -pix_fmt gray pattern.pgm` |  | pgm / pgm |
| `image/pattern.ppm` | Debian 13 container | `$FF $I $BX pattern.ppm` |  | ppm / ppm |
| `image/pattern.pam` | Debian 13 container | `$FF $I $BX pattern.pam` |  | pam / pam |
| `image/pattern.pfm` | Debian 13 container | `$FF $I $BX -pix_fmt gbrpf32le pattern.pfm` |  | pfm / pfm |
| `image/pattern.sgi` | Debian 13 container | `$FF $I $BX pattern.sgi` |  | sgi / sgi |
| `image/pattern.ras` | Debian 13 container | `$FF $I $BX -c:v sunrast pattern.ras` |  | ras / ras |
| `image/pattern.xbm` | Debian 13 container | `$FF $I $BX -pix_fmt monow pattern.xbm` |  | xbm / xbm |
| `image/pattern.xwd` | Debian 13 container | `$FF $I $BX pattern.xwd` |  | xwd / xwd |
| `image/pattern.dpx` | Debian 13 container | `$FF $I $BX pattern.dpx` |  | dpx / dpx |
| `image/pattern.exr` | Debian 13 container | `$FF $I $BX -pix_fmt gbrapf32le pattern.exr` |  | exr / exr |
| `image/pattern.hdr` | Debian 13 container | `$FF $I $BX -c:v hdr pattern.hdr` |  | hdr / hdr |
| `image/pattern.jp2` | Debian 13 container | `$FF $I $BX -c:v jpeg2000 pattern.jp2` |  | jp2 / jp2 |
| `image/pattern.j2k` | Debian 13 container | `$FF -f lavfi -i testsrc=size=16x16:rate=1:duration=1 -frames:v 1 $BX -c:v jpeg2000 -format j2k -f image2 pattern.j2k` |  | j2k / j2k |
| `image/pattern.jls` | Debian 13 container | `$FF $I $BX -c:v jpegls pattern.jls` |  | jls / jls |
| `image/pattern.qoi` | Debian 13 container | `$FF $I $BX pattern.qoi` |  | qoi / qoi |
| `image/pattern.fits` | Debian 13 container | `$FF $I $BX -pix_fmt gbrp pattern.fits` |  | fits / fits |
| `image/pattern.webp` | Debian 13 container | `$FF $I $BX -c:v libwebp pattern.webp` |  | webp / webp |
| `image/pattern.ico` | Debian 13 container | `$FF $I $BX -s 16x16 pattern.ico` |  | ico / ico |
| `image/magick.miff` | Debian 13 container | `convert g.png -strip magick.miff` |  | miff / miff |
| `image/magick.xpm` | Debian 13 container | `convert g.png -strip magick.xpm` |  | xpm / xpm |
| `image/magick.psd` | Debian 13 container | `convert g.png -strip magick.psd` |  | psd / psd |
| `image/magick.psb` | Debian 13 container | `convert g.png -strip PSB:magick.psb` |  | psb / psb |
| `image/magick.wbmp` | Debian 13 container | `convert g.png -strip magick.wbmp` |  | wbmp / - |
| `image/magick.mng` | Debian 13 container | `convert g.png -strip magick.mng` |  | mng / mng |
| `image/magick.jng` | Debian 13 container | `convert g.png -strip magick.jng` |  | jng / jng |
| `image/magick.dds` | Debian 13 container | `convert g.png -strip magick.dds` |  | dds / dds |
| `image/magick.ff` | Debian 13 container | `convert g.png -strip magick.ff` |  | ff / ff |
| `image/magick.cin` | Debian 13 container | `convert g.png -strip magick.cin` |  | cin / cin |
| `image/magick.pict` | Debian 13 container | `convert g.png -strip magick.pict` |  | pict / pict |
| `image/magick.eps` | Debian 13 container | `convert g.png -strip magick.eps` |  | eps / eps |
| `image/magick.ps` | Debian 13 container | `convert g.png -strip magick.ps` |  | ps / ps |
| `image/magick.pdf` | Debian 13 container | `convert g.png -strip magick.pdf` |  | pdf / pdf |
| `image/magick_mm.tiff` | Debian 13 container | `convert g.png -strip -define tiff:endian=msb magick_mm.tiff` |  | tiff / tiff |
| `image/magick_pyramid.tif` | Debian 13 container | `convert g.png -strip PTIF:magick_pyramid.tif` |  | tiff / tiff |
| `image/mac_gradient.png` | macOS 27.2 | `cp g.png mac_gradient.png` |  | png / png |
| `image/mac.heic` | macOS 27.2 | `sips -s format heic g.png --out mac.heic >/dev/null 2>&1` |  | heic / heic |
| `image/mac.jp2` | macOS 27.2 | `sips -s format jp2 g.png --out mac.jp2 >/dev/null 2>&1` |  | jp2 / jp2 |
| `image/mac.psd` | macOS 27.2 | `sips -s format psd g.png --out mac.psd >/dev/null 2>&1` |  | psd / psd |
| `image/mac.tga` | macOS 27.2 | `sips -s format tga g.png --out mac.tga >/dev/null 2>&1` |  | tga / - |
| `image/mac.jpg` | macOS 27.2 | `sips -s format jpeg g.png --out mac.jpg >/dev/null 2>&1` |  | jpg / jpg |
| `image/mac.tiff` | macOS 27.2 | `sips -s format tiff g.png --out mac.tiff >/dev/null 2>&1` |  | tiff / tiff |
| `image/mac.bmp` | macOS 27.2 | `sips -s format bmp g.png --out mac.bmp >/dev/null 2>&1` |  | bmp / bmp |
| `image/mac.gif` | macOS 27.2 | `sips -s format gif g.png --out mac.gif >/dev/null 2>&1` |  | gif / gif |
| `image/mac.icns` | macOS 27.2 | `mkdir -p icon.iconset && cp g.png icon.iconset/icon_16x16.png && iconutil -c icns icon.iconset -o mac.icns` |  | icns / icns |
| `image/magick.avif` | macOS 27.2 | `magick g.png -strip magick.avif 2>/dev/null` |  | avif / avif |
| `image/magick.heic` | macOS 27.2 | `magick g.png -strip magick.heic 2>/dev/null` |  | heic / heic |
| `image/magick_mac.webp` | macOS 27.2 | `magick g.png -strip magick.webp` |  | webp / webp |
| `image/magick_v3.bmp` | macOS 27.2 | `magick g.png -strip -define bmp:format=bmp3 magick_v3.bmp` |  | bmp / bmp |
| `image/magick_mac.qoi` | macOS 27.2 | `magick g.png -strip magick.qoi` |  | qoi / qoi |
| `image/magick_mac.sgi` | macOS 27.2 | `magick g.png -strip magick.sgi` |  | sgi / sgi |
| `image/magick_mac.ico` | macOS 27.2 | `magick g.png -strip magick.ico` |  | ico / ico |
| `image/magick_mac.tga` | macOS 27.2 | `magick g.png -strip magick.tga` |  | tga / - |
| `image/drawing.emf` | Debian 13 container | ` (then copied to the fixture name)` |  | emf / emf |
| `image/drawing.wmf` | Debian 13 container | ` (then copied to the fixture name)` |  | wmf / wmf |
| `image/drawing.eps` | Debian 13 container | ` (then copied to the fixture name)` |  | eps / eps |
| `image/drawing.odg` | Debian 13 container | ` (then copied to the fixture name)` |  | odg / odg |
| `image/drawing.otg` | Debian 13 container | ` (then copied to the fixture name)` |  | otg / otg |
| `image/drawing.fodg` | Debian 13 container | ` (then copied to the fixture name)` |  | fodg / fodg |
| `office/document.doc` | Debian 13 container | ` (then copied to the fixture name)` |  | doc / doc |
| `office/document.dot` | Debian 13 container | ` (then copied to the fixture name)` |  | dot / doc |
| `office/document.docx` | Debian 13 container | ` (then copied to the fixture name)` |  | docx / docx |
| `office/document.dotx` | Debian 13 container | ` (then copied to the fixture name)` |  | dotx / dotx |
| `office/document.docm` | Debian 13 container | ` (then copied to the fixture name)` |  | docm / docm |
| `office/document.rtf` | Debian 13 container | ` (then copied to the fixture name)` |  | rtf / rtf |
| `office/document.odt` | Debian 13 container | ` (then copied to the fixture name)` |  | odt / odt |
| `office/document.ott` | Debian 13 container | ` (then copied to the fixture name)` |  | ott / ott |
| `office/document.fodt` | Debian 13 container | ` (then copied to the fixture name)` |  | fodt / fodt |
| `office/document.epub` | Debian 13 container | ` (then copied to the fixture name)` |  | epub / epub |
| `office/document.xhtml` | Debian 13 container | ` (then copied to the fixture name)` |  | xhtml / xhtml |
| `office/document.html` | Debian 13 container | ` (then copied to the fixture name)` |  | html / html |
| `office/document.pdf` | Debian 13 container | ` (then copied to the fixture name)` |  | pdf / pdf |
| `office/wordml_2003.xml` | Debian 13 container | `cp doc.xml wordml_2003.xml 2>/dev/null` |  | wordml / wordml |
| `office/spreadsheet.xls` | Debian 13 container | ` (then copied to the fixture name)` |  | xls / xls |
| `office/spreadsheet.xlt` | Debian 13 container | ` (then copied to the fixture name)` |  | xlt / xls |
| `office/spreadsheet.xlsx` | Debian 13 container | ` (then copied to the fixture name)` |  | xlsx / xlsx |
| `office/spreadsheet.xltx` | Debian 13 container | ` (then copied to the fixture name)` |  | xltx / xltx |
| `office/spreadsheet.xlsm` | Debian 13 container | ` (then copied to the fixture name)` |  | xlsm / xlsm |
| `office/spreadsheet.ods` | Debian 13 container | ` (then copied to the fixture name)` |  | ods / ods |
| `office/spreadsheet.ots` | Debian 13 container | ` (then copied to the fixture name)` |  | ots / ots |
| `office/spreadsheet.fods` | Debian 13 container | ` (then copied to the fixture name)` |  | fods / fods |
| `office/spreadsheet.dif` | Debian 13 container | ` (then copied to the fixture name)` |  | dif / txt |
| `office/spreadsheet.slk` | Debian 13 container | ` (then copied to the fixture name)` |  | slk / slk |
| `office/spreadsheetml_2003.xml` | Debian 13 container | `cp sheet.xml spreadsheetml_2003.xml 2>/dev/null` |  | xmlss / xmlss |
| `office/presentation.odp` | Debian 13 container | `cp presentation.odp p.odp` |  | odp / odp |
| `office/presentation.otp` | Debian 13 container | `$LO --convert-to "$spec" p.odp >/dev/null 2>&1` |  | otp / otp |
| `office/presentation.fodp` | Debian 13 container | `$LO --convert-to "$spec" p.odp >/dev/null 2>&1` |  | fodp / fodp |
| `office/presentation.pptx` | Debian 13 container | `cp presentation.pptx p.pptx` |  | pptx / pptx |
| `office/presentation.potx` | Debian 13 container | `$LO --convert-to "$spec" p.odp >/dev/null 2>&1` |  | potx / potx |
| `office/presentation.ppsx` | Debian 13 container | `$LO --convert-to "$spec" p.odp >/dev/null 2>&1` |  | ppsx / pptx |
| `office/presentation.pptm` | Debian 13 container | `$LO --convert-to "$spec" p.odp >/dev/null 2>&1` |  | pptm / pptm |
| `office/presentation.ppt` | Debian 13 container | `$LO --convert-to "$spec" p.odp >/dev/null 2>&1` |  | ppt / ppt |
| `office/textutil.doc` | macOS 27.2 | `textutil -convert doc doc.txt -output textutil.doc  (doc.txt = 'Lorem ipsum dolor sit amet.')` |  | doc / doc |
| `office/textutil.docx` | macOS 27.2 | `textutil -convert docx doc.txt -output textutil.docx  (doc.txt = 'Lorem ipsum dolor sit amet.')` |  | docx / docx |
| `office/textutil.rtf` | macOS 27.2 | `textutil -convert rtf doc.txt -output textutil.rtf  (doc.txt = 'Lorem ipsum dolor sit amet.')` |  | rtf / rtf |
| `office/textutil.odt` | macOS 27.2 | `textutil -convert odt doc.txt -output textutil.odt  (doc.txt = 'Lorem ipsum dolor sit amet.')` |  | odt / odt |
| `office/textutil.html` | macOS 27.2 | `textutil -convert html doc.txt -output textutil.html  (doc.txt = 'Lorem ipsum dolor sit amet.')` |  | html / html |
| `office/textutil.webarchive` | macOS 27.2 | `textutil -convert webarchive doc.txt -output textutil.webarchive  (doc.txt = 'Lorem ipsum dolor sit amet.')` |  | webarchive / plist |
| `office/textutil_wordml.xml` | macOS 27.2 | `[ -f textutil.wordml ] && mv textutil.wordml textutil_wordml.xml` |  | wordml / wordml |
| `archive/archive.tar` | Debian 13 container | `tar --owner=0 --group=0 --numeric-owner --mtime=@0 --sort=name -cf - payload \| xz --format=lzma -c > archive.tar.lzma` |  | tar / tar |
| `archive/archive_v7.tar` | Debian 13 container | `$TAR --format=v7 -cf archive_v7.tar payload` |  | tar / tar |
| `archive/archive.tar.gz` | Debian 13 container | `$TAR -czf archive.tar.gz payload` |  | tar.gz / tar.gz |
| `archive/archive.tar.bz2` | Debian 13 container | `$TAR -cjf archive.tar.bz2 payload` |  | tar.bz2 / bz2 |
| `archive/archive.tar.xz` | Debian 13 container | `$TAR -cJf archive.tar.xz payload` |  | tar.xz / xz |
| `archive/archive.tar.zst` | Debian 13 container | `$TAR --zstd -cf archive.tar.zst payload` |  | tar.zst / zst |
| `archive/archive.tar.lz` | Debian 13 container | `$TAR --lzip -cf archive.tar.lz payload` |  | tar.lz / lz |
| `archive/archive.tar.lzma` | Debian 13 container | `tar --owner=0 --group=0 --numeric-owner --mtime=@0 --sort=name -cf - payload \| xz --format=lzma -c > archive.tar.lzma` |  | tar.lzma / lzma |
| `archive/archive.tar.z` | Debian 13 container | `$TAR -Z -cf archive.tar.z payload` |  | tar.z / z |
| `archive/hello.txt.gz` | Debian 13 container | `gzip -9n -c payload/hello.txt > hello.txt.gz` |  | gz / gz |
| `archive/hello.txt.bz2` | Debian 13 container | `bzip2 -c payload/hello.txt > hello.txt.bz2` |  | bz2 / bz2 |
| `archive/hello.txt.xz` | Debian 13 container | `xz -c payload/hello.txt > hello.txt.xz` |  | xz / xz |
| `archive/hello.txt.lzma` | Debian 13 container | `xz --format=lzma -c payload/hello.txt > hello.txt.lzma` |  | lzma / lzma |
| `archive/hello.txt.zst` | Debian 13 container | `zstd -q -c payload/hello.txt > hello.txt.zst` |  | zst / zst |
| `archive/hello.txt.lz` | Debian 13 container | `lzip -c payload/hello.txt > hello.txt.lz` |  | lz / lz |
| `archive/hello.txt.lz4` | Debian 13 container | `lz4 -q -c payload/hello.txt > hello.txt.lz4` |  | lz4 / lz4 |
| `archive/hello_legacy.txt.lz4` | Debian 13 container | `lz4 -q -l -c payload/hello.txt > hello_legacy.txt.lz4` |  | lz4 / lz4 |
| `archive/hello.txt.lzo` | Debian 13 container | `lzop -c payload/hello.txt > hello.txt.lzo` |  | lzo / lzo |
| `archive/hello.txt.z` | Debian 13 container | `compress -c payload/hello.txt > hello.txt.z` |  | z / z |
| `archive/hello.txt.br` | Debian 13 container | `brotli -c payload/hello.txt > hello.txt.br` |  | br / - |
| `archive/archive.7z` | Debian 13 container | `(cd payload && 7z a -bd -mtm=off -mtc=off -mta=off archive.7z hello.txt >/dev/null)` |  | 7z / 7z |
| `archive/archive.arj` | Debian 13 container | `(cd payload && arj a -i archive.arj hello.txt >/dev/null)` |  | arj / arj |
| `archive/archive.cab` | Debian 13 container | `(cd payload && gcab -c -n archive.cab hello.txt)` |  | cab / cab |
| `archive/archive.cpio` | Debian 13 container | `(cd payload && echo hello.txt \| cpio -o -H newc --owner=0:0 > archive.cpio 2>/dev/null)` |  | cpio / cpio |
| `archive/archive_odc.cpio` | Debian 13 container | `(cd payload && echo hello.txt \| cpio -o -H odc --owner=0:0 > archive_odc.cpio 2>/dev/null)` |  | cpio / cpio |
| `archive/archive.wim` | Debian 13 container | `(cd payload && 7z a -bd -twim archive.wim hello.txt >/dev/null)` |  | wim / wim |
| `archive/archive.xar` | Debian 13 container | `(cd payload && bsdtar --format xar --uid 0 --gid 0 --uname '' --gname '' -cf archive.xar hello.txt)` |  | xar / pkg |
| `archive/libanswer.a` | Debian 13 container | `printf 'int answer(void) { return 42; }\n' > lib.c && gcc -g0 -O1 -fno-ident -c lib.c -o lib.o && rm -f libanswer.a && ar rcsD libanswer.a lib.o` |  | a / a |
| `archive/image.squashfs` | Debian 13 container | `mksquashfs payload image.squashfs -all-root -noappend -quiet >/dev/null` |  | squashfs / squashfs |
| `archive/image.jffs2` | Debian 13 container | `mkfs.jffs2 -r payload -o image.jffs2 -U 2>/dev/null` |  | jffs2 / - |
| `archive/sample.deb` | Debian 13 container | `SOURCE_DATE_EPOCH=0 dpkg-deb --root-owner-group -Zgzip --build deb sample.deb >/dev/null 2>&1` |  | deb / deb |
| `disk/disc.iso` | Debian 13 container | `genisoimage -quiet -V TEST -o disc.iso payload` | first 36864 bytes kept (all that detection reads) | iso / iso |
| `disk/disc_udf.iso` | Debian 13 container | `genisoimage -quiet -udf -V TEST -o disc_udf.iso payload` | first 36864 bytes kept (all that detection reads) | iso / iso |
| `disk/floppy.img` | Debian 13 container | `dd if=/dev/zero of=floppy.img bs=1024 count=1440 status=none && mkfs.fat -C -i 12345678 -n TEST floppy.img 1440 >/dev/null 2>&1 \|\| mkfs.fat -i 12345678 -n TEST floppy.img >/dev/null` | first 1024 bytes kept (all that detection reads) | img / img |
| `disk/ext4.img` | Debian 13 container | `dd if=/dev/zero of=ext4.img bs=1024 count=1024 status=none && mkfs.ext4 -q -F -U 00000000-0000-0000-0000-000000000001 -E root_owner=0:0 ext4.img` | first 4096 bytes kept (all that detection reads) | img / img |
| `disk/mbr.img` | Debian 13 container | `printf 'label: dos\nlabel-id: 0x12345678\nstart=2048, size=8, type=83\n' \| sfdisk -q --no-reread --no-tell-kernel mbr.img` | first 1024 bytes kept (all that detection reads) | img / img |
| `disk/gpt.img` | Debian 13 container | `printf 'label: gpt\nlabel-id: 00000000-0000-0000-0000-000000000002\nstart=2048, size=8, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=00000000-0000-0000-0000-000000000003\n' \| sfdisk -q --no-reread --no-tell-kernel gpt.img` | first 1024 bytes kept (all that detection reads) | img / img |
| `disk/disk.qcow2` | Debian 13 container | `cp disk.qcow2 disk.qcow2; cp disk.vdi disk.vdi; cp disk.vmdk disk.vmdk; cp disk.vpc disk.vhd` | first 1024 bytes kept (all that detection reads) | qcow2 / qcow2 |
| `disk/disk.vdi` | Debian 13 container | `cp disk.qcow2 disk.qcow2; cp disk.vdi disk.vdi; cp disk.vmdk disk.vmdk; cp disk.vpc disk.vhd` | first 1024 bytes kept (all that detection reads) | vdi / vdi |
| `disk/disk.vmdk` | Debian 13 container | `cp disk.qcow2 disk.qcow2; cp disk.vdi disk.vdi; cp disk.vmdk disk.vmdk; cp disk.vpc disk.vhd` | first 1024 bytes kept (all that detection reads) | vmdk / vmdk |
| `disk/disk.vhd` | Debian 13 container | `cp disk.qcow2 disk.qcow2; cp disk.vdi disk.vdi; cp disk.vmdk disk.vmdk; cp disk.vpc disk.vhd` | first 1024 bytes kept (all that detection reads) | vhd / vhd |
| `disk/disk.vhdx` | Debian 13 container | `cp disk.vhdx disk.vhdx; cp disk.qed disk.qed; cp disk.parallels disk.hdd` | first 1024 bytes kept (all that detection reads) | vhdx / vhdx |
| `disk/disk.qed` | Debian 13 container | `cp disk.vhdx disk.vhdx; cp disk.qed disk.qed; cp disk.parallels disk.hdd` | first 1024 bytes kept (all that detection reads) | qed / qed |
| `disk/disk.hdd` | Debian 13 container | `cp disk.vhdx disk.vhdx; cp disk.qed disk.qed; cp disk.parallels disk.hdd` | first 1024 bytes kept (all that detection reads) | hdd / hdd |
| `disk/udzo.dmg` | macOS 27.2 | `hdiutil create -quiet -srcfolder dmgsrc -volname T -fs HFS+ -format UDZO udzo.dmg` |  | dmg / dmg |
| `disk/hfs_raw_head.dmg` | macOS 27.2 | `head -c 8192 hfs.dmg > hfs_raw_head.dmg` |  | dmg / img |
| `disk/apfs_raw_head.dmg` | macOS 27.2 | `head -c 8192 apfs.dmg > apfs_raw_head.dmg` |  | dmg / img |
| `disk/head.sparseimage` | macOS 27.2 | `head -c 4096 s.sparseimage > head.sparseimage` |  | sparseimage / sparseimage |
| `archive/archive.zip` | Info-ZIP zip 3.0 (macOS) | `(cd plain && zip -X -q archive.zip hello.txt)` |  | zip / zip |
| `archive/comic.cbz` | Info-ZIP zip 3.0 (macOS) | `cp comic.cbz comic_images.zip` |  | cbz / zip |
| `archive/comic_images.zip` | Info-ZIP zip 3.0 (macOS) | `cp comic.cbz comic_images.zip` |  | zip / zip |
| `geo/points.kmz` | Info-ZIP zip 3.0 (macOS) | `mkdir -p kmz && cp $D/points.kml kmz/doc.kml && (cd kmz && zip -X -q points.kmz doc.kml)` |  | kmz / kmz |
| `package/sample.apk` | Info-ZIP zip 3.0 (macOS) | `mk sample.apk AndroidManifest.xml classes.dex resources.arsc` |  | apk / apk |
| `package/sample.aab` | Info-ZIP zip 3.0 (macOS) | `mk sample.aab BundleConfig.pb base/manifest/AndroidManifest.xml` |  | aab / aab |
| `package/sample.ipa` | Info-ZIP zip 3.0 (macOS) | `mk sample.ipa Payload/Sample.app/Info.plist` |  | ipa / ipa |
| `package/sample.xpi` | Info-ZIP zip 3.0 (macOS) | `mk sample.xpi manifest.json META-INF/mozilla.rsa` |  | xpi / xpi |
| `package/sample.war` | Info-ZIP zip 3.0 (macOS) | `mk sample.war WEB-INF/web.xml index.html` |  | war / war |
| `package/sample.msix` | Info-ZIP zip 3.0 (macOS) | `mk sample.msix AppxManifest.xml '[Content_Types].xml'` |  | msix / msix |
| `package/sample.nupkg` | Info-ZIP zip 3.0 (macOS) | `mk sample.nupkg sample.nuspec '[Content_Types].xml'` |  | nupkg / nupkg |
| `package/sample.vsix` | Info-ZIP zip 3.0 (macOS) | `mk sample.vsix extension.vsixmanifest '[Content_Types].xml'` |  | vsix / vsix |
| `package/sample-1.0-py3-none-any.whl` | Info-ZIP zip 3.0 (macOS) | `mk sample-1.0-py3-none-any.whl sample/__init__.py sample-1.0.dist-info/WHEEL sample-1.0.dist-info/METADATA` |  | whl / whl |
| `model/sample.3mf` | Info-ZIP zip 3.0 (macOS) | `mk sample.3mf '[Content_Types].xml' 3D/3dmodel.model` |  | 3mf / 3mf |
| `model/sample.usdz` | Info-ZIP zip 3.0 (macOS) | `mkdir -p usdz && cp $D/scene.usda usdz/scene.usda && (cd usdz && zip -X -q -0 sample.usdz scene.usda)` |  | usdz / usdz |
| `office/sample.xps` | Info-ZIP zip 3.0 (macOS) | `mk sample.xps '[Content_Types].xml' FixedDocumentSequence.fdseq` |  | xps / xps |
| `image/sample.sketch` | Info-ZIP zip 3.0 (macOS) | `mk sample.sketch document.json meta.json pages/page.json` |  | sketch / sketch |
| `office/sample.numbers` | Info-ZIP zip 3.0 (macOS) | `mk sample.numbers Index/Document.iwa Index/CalculationEngine.iwa` |  | numbers / numbers |
| `office/sample.key` | Info-ZIP zip 3.0 (macOS) | `mk sample.key Index/Document.iwa Index/Slide-1.iwa` |  | key / key |
| `office/sample.pages` | Info-ZIP zip 3.0 (macOS) | `mk sample.pages Index/Document.iwa Index/Metadata.iwa` |  | pages / pages |
| `data/sample.apkg` | Info-ZIP zip 3.0 (macOS) | `mk sample.apkg collection.anki2 media` |  | apkg / apkg |
| `data/sample.pt` | Info-ZIP zip 3.0 (macOS) | `mk sample.pt archive/data.pkl archive/version` |  | pt / pt |
| `data/sample.keras` | Info-ZIP zip 3.0 (macOS) | `mk sample.keras config.json metadata.json model.weights.h5` |  | keras / keras |
| `exec/program.elf` | Debian 13 container | `gcc $CF -o program.elf main.c && strip program.elf` | first 4096 bytes kept (all that detection reads) | elf / elf |
| `exec/program_static.elf` | Debian 13 container | `gcc $CF -no-pie -static -o program_static.elf main.c && strip program_static.elf` | first 4096 bytes kept (all that detection reads) | elf / elf |
| `exec/libanswer.so` | Debian 13 container | `gcc $CF -shared -fPIC -o libanswer.so lib.c && strip libanswer.so` | first 4096 bytes kept (all that detection reads) | so / so |
| `exec/object.o` | Debian 13 container | `gcc $CF -c -o object.o lib.c` |  | o / o |
| `exec/program.exe` | Debian 13 container | `x86_64-w64-mingw32-gcc $CF -o program.exe main.c && x86_64-w64-mingw32-strip program.exe` |  | exe / exe |
| `exec/library.dll` | Debian 13 container | `x86_64-w64-mingw32-gcc $CF -shared -o library.dll lib.c && x86_64-w64-mingw32-strip library.dll` |  | dll / dll |
| `exec/object.obj` | Debian 13 container | `x86_64-w64-mingw32-gcc $CF -c -o object.obj lib.c` |  | coff / coff |
| `exec/app.efi` | Debian 13 container | `x86_64-w64-mingw32-gcc $CF -nostdlib -Wl,--subsystem,10 -e answer -o app.efi lib.c && x86_64-w64-mingw32-strip app.efi` |  | efi / efi |
| `exec/driver.sys` | Debian 13 container | `x86_64-w64-mingw32-gcc $CF -nostdlib -Wl,--subsystem,native -e answer -o driver.sys lib.c && x86_64-w64-mingw32-strip driver.sys` |  | sys / sys |
| `exec/module.wasm` | Debian 13 container | `clang --target=wasm32 -nostdlib -O1 -g0 -Wl,--no-entry -Wl,--export-all -o module.wasm lib.c` |  | wasm / wasm |
| `exec/module.bc` | Debian 13 container | `clang -c -emit-llvm -O1 -g0 -ffile-prefix-map=<scratch>=. -o module.bc lib.c` |  | bc / bc |
| `exec/Hello.class` | Debian 13 container | `(cd java && javac -g:none Hello.java && cp Hello.class Hello.class && jar --create --file hello.jar --date=1980-01-01T00:00:02Z Hello.class)` |  | class / class |
| `exec/hello.jar` | Debian 13 container | `(cd java && javac -g:none Hello.java && cp Hello.class Hello.class && jar --create --file hello.jar --date=1980-01-01T00:00:02Z Hello.class)` |  | jar / jar |
| `exec/hello.luac` | Debian 13 container | `printf 'print("hi")\n' > hello.lua && luac5.4 -s -o hello.luac hello.lua` |  | luac / luac |
| `exec/program_arm64.macho` | macOS 27.2 | `clang $CF -arch arm64 -o program_arm64.macho main.c && strip program_arm64.macho` |  | macho / macho |
| `exec/program_universal.macho` | macOS 27.2 | `clang $CF -arch arm64 -arch x86_64 -o program_universal.macho main.c && strip program_universal.macho` | first 4096 bytes kept (all that detection reads) | macho / macho |
| `exec/libanswer.dylib` | macOS 27.2 | `clang $CF -arch arm64 -dynamiclib -install_name @rpath/libanswer.dylib -o libanswer.dylib lib.c && strip -x libanswer.dylib` |  | dylib / dylib |
| `exec/libfat.dylib` | macOS 27.2 | `clang $CF -arch arm64 -arch x86_64 -dynamiclib -install_name @rpath/libfat.dylib -o libfat.dylib lib.c && strip -x libfat.dylib` | first 20480 bytes kept (all that detection reads) | dylib / dylib |
| `exec/plugin.bundle` | macOS 27.2 | `clang $CF -arch arm64 -bundle -o plugin.bundle lib.c && strip -x plugin.bundle` |  | bundle / bundle |
| `exec/macho_object.o` | macOS 27.2 | `clang $CF -arch arm64 -c -o macho_object.o lib.c` |  | o / o |
| `exec/module.pyc` | macOS 27.2 | `py_compile.compile('<scratch>/__hello_pyc.py', cfile=out + '/module.pyc', dfile='module.py')` |  | pyc / pyc |
| `geo/points.shp` | Debian 13 container | `ogr2ogr -q -f "ESRI Shapefile" pts.shp pts.geojson && cp pts.shp pts.shx pts.dbf pts.prj ` |  | shp / shp |
| `geo/points.shx` | Debian 13 container | `ogr2ogr -q -f "ESRI Shapefile" pts.shp pts.geojson && cp pts.shp pts.shx pts.dbf pts.prj ` |  | shx / shp |
| `geo/points.dbf` | Debian 13 container | `ogr2ogr -q -f "ESRI Shapefile" pts.shp pts.geojson && cp pts.shp pts.shx pts.dbf pts.prj ` |  | dbf / dbf |
| `geo/points.prj` | Debian 13 container | `ogr2ogr -q -f "ESRI Shapefile" pts.shp pts.geojson && cp pts.shp pts.shx pts.dbf pts.prj ` |  | prj / prj |
| `geo/points.gpkg` | Debian 13 container | `ogr2ogr -q -f GPKG points.gpkg pts.geojson` | first 4096 bytes kept (all that detection reads) | gpkg / gpkg |
| `geo/points.kml` | Debian 13 container | `ogr2ogr -q -f KML points.kml pts.geojson` |  | kml / kml |
| `geo/points.gml` | Debian 13 container | `ogr2ogr -q -f GML points.gml pts.geojson` |  | gml / gml |
| `geo/points.gpx` | Debian 13 container | `ogr2ogr -q -f GPX points.gpx pts.geojson -dsco GPX_USE_EXTENSIONS=YES` |  | gpx / gpx |
| `geo/points.fgb` | Debian 13 container | `ogr2ogr -q -f FlatGeobuf points.fgb pts.geojson` |  | fgb / fgb |
| `geo/points.pmtiles` | Debian 13 container | `ogr2ogr -q -f PMTiles points.pmtiles pts.geojson -dsco MAXZOOM=1 2>/dev/null` |  | pmtiles / pmtiles |
| `geo/geo.tif` | Debian 13 container | `gdal_translate -q -of GTiff -outsize 8 8 -a_ullr 0 8 8 0 -a_srs EPSG:4326 g.png geo.tif` |  | geotiff / geotiff |
| `geo/grid.asc` | Debian 13 container | `gdal_translate -q -of AAIGrid -b 1 -outsize 4 4 geo.tif grid.asc` |  | asc-grid / asc-grid |
| `data/grid.nc` | Debian 13 container | `gdal_translate -q -of netCDF -b 1 -co WRITE_GDAL_VERSION=NO -co WRITE_GDAL_HISTORY=NO geo.tif grid.nc` |  | nc / nc |
| `geo/grid.grb2` | Debian 13 container | `gdal_translate -q -of GRIB -b 1 -ot Byte geo.tif grid.grb2 2>/dev/null` |  | grib / grib |
| `geo/tiles.mbtiles` | Debian 13 container | `gdal_translate -q -of MBTiles geo.tif tiles.mbtiles 2>/dev/null` | first 4096 bytes kept (all that detection reads) | mbtiles / sqlite |
| `font/sample.ttf` | Debian 13 container | `c = TTCollection(); c.fonts = [TTFont('sample.ttf'), TTFont('sample.ttf')]; c.save('sample.ttc')` |  | ttf / ttf |
| `font/sample.otf` | Debian 13 container | `fontforge -quiet -lang=py -script font.py  (one-glyph font 'Sample', glyph A = triangle; f.generate('sample.otf'); run as a uid without a passwd entry)` |  | otf / otf |
| `font/sample.woff` | Debian 13 container | `fontforge -quiet -lang=py -script font.py  (one-glyph font 'Sample', glyph A = triangle; f.generate('sample.woff'); run as a uid without a passwd entry)` |  | woff / woff |
| `font/sample.woff2` | Debian 13 container | `fontforge -quiet -lang=py -script font.py  (one-glyph font 'Sample', glyph A = triangle; f.generate('sample.woff2'); run as a uid without a passwd entry)` |  | woff2 / woff2 |
| `font/sample.pfb` | Debian 13 container | `cp sample.pfa sample.pfb ` |  | pfb / pfb |
| `font/sample.pfa` | Debian 13 container | `grep -a "Creator" sample.pfa` |  | pfa / pfa |
| `font/sample.afm` | Debian 13 container | `fontforge -quiet -lang=py -script font.py  (one-glyph font 'Sample', glyph A = triangle; f.generate('sample.afm'); run as a uid without a passwd entry)` |  | afm / afm |
| `font/sample.sfd` | Debian 13 container | `f.save("sample.sfd")` |  | sfd / sfd |
| `font/sample.ttc` | Debian 13 container | `c = TTCollection(); c.fonts = [TTFont('sample.ttf'), TTFont('sample.ttf')]; c.save('sample.ttc')` |  | ttc / ttc |
| `data/capture.pcap` | Debian 13 container | `text2pcap -q -t '%s' pkt.txt capture.pcap 2>/dev/null \|\| text2pcap -q pkt.txt capture.pcap` |  | pcap / pcap |
| `data/capture.pcapng` | Debian 13 container | `text2pcap -q -n pkt.txt capture.pcapng` |  | pcapng / pcapng |
| `data/image.dcm` | Debian 13 container | `img2dcm --no-checks pattern.jpg image.dcm 2>/dev/null \|\| (convert g.png g.jpg && img2dcm g.jpg image.dcm)` | first 1024 bytes kept (all that detection reads) | dcm / dcm |
| `data/array.npy` | Debian 13 container | `np.save('array.npy', np.arange(6, dtype=np.int16))` |  | npy / npy |
| `data/arrays.npz` | Debian 13 container | `np.savez('arrays.npz', a=np.arange(3, dtype=np.int8))` |  | npz / npz |
| `data/vars.mat` | Debian 13 container | `import scipy.io; scipy.io.savemat('vars.mat', {'a': np.arange(3)})` |  | mat / mat |
| `data/magick.mat` | Debian 13 container | `convert g.png -strip MAT:magick.mat` |  | mat / mat |
| `data/data.h5` | Debian 13 container | `with h5py.File('data.h5', 'w') as f: f['a'] = np.arange(3)` |  | h5 / h5 |
| `data/database.sqlite` | macOS 27.2 | `sqlite3 database.sqlite 'PRAGMA page_size=512; CREATE TABLE t(a INTEGER, b TEXT); INSERT INTO t VALUES (1, "x");'` |  | sqlite / sqlite |
| `data/objects.pkl` | macOS 27.2 | `with open(out + '/objects.pkl', 'wb') as f:` |  | pkl / pkl |
| `data/link.webloc` | macOS 27.2 | `plutil -convert xml1 link.json -o link.webloc` |  | webloc / plist |
| `data/binary.plist` | macOS 27.2 | `plutil -convert binary1 link.json -o binary.plist` |  | plist / plist |
| `data/settings.plist` | macOS 27.2 | `plutil -convert xml1 link.json -o settings.plist` |  | plist / plist |
| `mail/message.eml` | macOS 27.2 | `open(out + '/message.eml', 'wb').write(bytes(m))` |  | eml / eml |
| `mail/mail.mbox` | macOS 27.2 | `box = mailbox.mbox(out + '/mail.mbox'); box.add(m); box.flush(); box.close()` |  | mbox / mbox |
| `security/private_pkcs8.pk8` | macOS 27.2 | `cp key.pem private.key` | first 64 bytes kept (all that detection reads) | private-key / private-key |
| `security/private_der.p8` | macOS 27.2 | `openssl pkcs8 -topk8 -nocrypt -in key.pem -outform DER -out private_der.key` | first 32 bytes kept (all that detection reads) | private-key / private-key |
| `security/id_test` | macOS 27.2 | `ssh-keygen -q -t ed25519 -N '' -C '' -f id_test && cp id_test.pub id_test.pub && cp id_test openssh.key` | first 72 bytes kept (all that detection reads) | private-key / private-key |
| `security/cert.crt` | macOS 27.2 | `openssl req -new -x509 -key key.pem -subj $SUBJ -days 1 -out cert.crt 2>/dev/null` |  | crt / crt |
| `security/cert.cer` | macOS 27.2 | `openssl x509 -in cert.crt -outform DER -out cert.cer` |  | crt / crt |
| `security/request.csr` | macOS 27.2 | `openssl req -new -key key.pem -subj $SUBJ -out request.csr 2>/dev/null` |  | csr / csr |
| `security/request_der.csr` | macOS 27.2 | `openssl req -new -key key.pem -subj $SUBJ -outform DER -out request_der.csr 2>/dev/null` |  | csr / csr |
| `security/bundle.pfx` | macOS 27.2 | `openssl pkcs12 -export -in cert.crt -inkey key.pem -passout pass:test -out bundle.p12 2>/dev/null` | first 32 bytes kept (all that detection reads) | p12 / p12 |
| `security/chain.p7b` | macOS 27.2 | `openssl crl2pkcs7 -nocrl -certfile cert.crt -out chain.p7b 2>/dev/null` |  | p7b / p7b |
| `security/chain_der.p7b` | macOS 27.2 | `openssl crl2pkcs7 -nocrl -certfile cert.crt -outform DER -out chain_der.p7b 2>/dev/null` |  | p7b / p7b |
| `security/id_test.pub` | macOS 27.2 | `ssh-keygen -q -t ed25519 -N '' -C '' -f id_test && cp id_test.pub id_test.pub && cp id_test openssh.key` |  | ssh-pubkey / ssh-pubkey |

## Hand-written text fixtures (`text/`)

UTF-8 text written byte for byte with `printf` (zsh 5.9 built-in), shown
here as escaped string literals.

| File | Exact contents | Expected (named / unnamed) |
| --- | --- | --- |
| `text/notes.md` | `# Notes\n\nSome *emphasis* and a [link](https://example.com).\n\n- item one\n- item two\n` | md / txt |
| `text/table.csv` | `name,count,price\napple,3,1.20\npear,5,0.80\nplum,7,0.40\n` | csv / csv |
| `text/semicolon.csv` | `name;count;price\napple;3;1,20\npear;5;0,80\nplum;7;0,40\n` | csv / csv |
| `text/table.tsv` | `name\tcount\nalpha\t1\nbeta\t2\ngamma\t3\n` | tsv / tsv |
| `text/data.json` | `{\n  "name": "sample",\n  "values": [1, 2, 3],\n  "nested": {"ok": true}\n}\n` | json / json |
| `text/array.json` | `[1, 2, 3, {"a": null}]\n` | json / json |
| `text/events.jsonl` | `{"id": 1, "event": "start"}\n{"id": 2, "event": "stop"}\n` | jsonl / jsonl |
| `text/places.geojson` | `{"type": "FeatureCollection", "features": [{"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [1.0, 2.0]}}]}\n` | geojson / geojson |
| `text/shapes.topojson` | `{"type": "Topology", "objects": {"a": {"type": "GeometryCollection", "geometries": []}}, "arcs": []}\n` | topojson / topojson |
| `text/scene.gltf` | `{"asset": {"version": "2.0"}, "scenes": [{"nodes": []}], "scene": 0}\n` | gltf / gltf |
| `text/notebook.ipynb` | `{\n "cells": [\n  {"cell_type": "markdown", "metadata": {}, "source": ["# Title"]}\n ],\n "metadata": {},\n "nbformat": 4,\n "nbformat_minor": 5\n}\n` | ipynb / ipynb |
| `text/network.har` | `{"log": {"version": "1.2", "creator": {"name": "test", "version": "1"}, "entries": []}}\n` | har / har |
| `text/app.webmanifest` | `{"name": "Sample", "short_name": "S", "start_url": "/", "display": "standalone"}\n` | webmanifest / webmanifest |
| `text/animation.json` | `{"v": "5.7.4", "fr": 30, "ip": 0, "op": 60, "w": 100, "h": 100, "layers": []}\n` | lottie / lottie |
| `text/linked.jsonld` | `{"@context": "https://schema.org", "@type": "Thing", "name": "x"}\n` | jsonld / jsonld |
| `text/api.openapi` | `{"openapi": "3.0.0", "info": {"title": "t", "version": "1"}, "paths": {}}\n` | openapi / openapi |
| `text/drawing.excalidraw` | `{"type": "excalidraw", "version": 2, "elements": [], "appState": {}}\n` | excalidraw / excalidraw |
| `text/config.yaml` | `%YAML 1.2\n---\nname: sample\nitems:\n  - one\n  - two\n` | yaml / yaml |
| `text/plain.yml` | `name: sample\nitems:\n  - one\n` | yaml / txt |
| `text/settings.toml` | `[package]\nname = "sample"\nversion = "1.0"\n` | toml / txt |
| `text/settings.ini` | `[section]\nkey=value\nother=1\n` | ini / txt |
| `text/app.properties` | `greeting=hello\ncount=3\n` | properties / txt |
| `text/feed.rss` | `<?xml version="1.0"?>\n<rss version="2.0"><channel><title>t</title></channel></rss>\n` | rss / rss |
| `text/feed.atom` | `<?xml version="1.0" encoding="utf-8"?>\n<feed xmlns="http://www.w3.org/2005/Atom"><title>t</title></feed>\n` | atom / atom |
| `text/drawing.svg` | `<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect width="10" height="10"/></svg>\n` | svg / svg |
| `text/strings.xlf` | `<?xml version="1.0"?>\n<xliff version="1.2" xmlns="urn:oasis:names:tc:xliff:document:1.2"><file/></xliff>\n` | xlf / xlf |
| `text/book.fb2` | `<?xml version="1.0" encoding="utf-8"?>\n<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0"><body/></FictionBook>\n` | fb2 / fb2 |
| `text/article.dbk` | `<?xml version="1.0"?>\n<article xmlns="http://docbook.org/ns/docbook" version="5.0"><title>t</title></article>\n` | docbook / docbook |
| `text/formula.mml` | `<math xmlns="http://www.w3.org/1998/Math/MathML"><mi>x</mi></math>\n` | mml / mml |
| `text/score.musicxml` | `<?xml version="1.0"?>\n<score-partwise version="4.0"><part-list/></score-partwise>\n` | musicxml / musicxml |
| `text/list.xspf` | `<?xml version="1.0"?>\n<playlist version="1" xmlns="http://xspf.org/ns/0/"><trackList/></playlist>\n` | xspf / xspf |
| `text/style.xsl` | `<?xml version="1.0"?>\n<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"/>\n` | xslt / xslt |
| `text/schema.xsd` | `<?xml version="1.0"?>\n<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"/>\n` | xsd / xsd |
| `text/outline.opml` | `<?xml version="1.0"?>\n<opml version="2.0"><head/><body/></opml>\n` | opml / opml |
| `text/graph.graphml` | `<?xml version="1.0"?>\n<graphml xmlns="http://graphml.graphdrawing.org/xmlns"><graph edgedefault="directed"/></graphml>\n` | graphml / graphml |
| `text/scene.x3d` | `<?xml version="1.0"?>\n<X3D profile="Interchange" version="3.3"><Scene/></X3D>\n` | x3d / x3d |
| `text/model.dae` | `<?xml version="1.0"?>\n<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1"/>\n` | dae / dae |
| `text/workout.tcx` | `<?xml version="1.0"?>\n<TrainingCenterDatabase xmlns="http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2"/>\n` | tcx / tcx |
| `text/data.xml` | `<?xml version="1.0"?>\n<inventory><item id="1"/></inventory>\n` | xml / xml |
| `text/page.html` | `<!DOCTYPE html>\n<html><head><title>t</title></head><body><p>Hello</p></body></html>\n` | html / html |
| `text/map.osm` | `<?xml version='1.0' encoding='UTF-8'?>\n<osm version="0.6"><node id="1" lat="0" lon="0"/></osm>\n` | osm / osm |
| `text/project.csproj` | `<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup/></Project>\n` | csproj / txt |
| `text/pom.xml` | `<?xml version="1.0"?>\n<project xmlns="http://maven.apache.org/POM/4.0.0"><modelVersion>4.0.0</modelVersion></project>\n` | pom / pom |
| `text/subtitles.srt` | `1\n00:00:01,000 --> 00:00:02,500\nHello there\n\n2\n00:00:03,000 --> 00:00:04,000\nBye\n` | srt / srt |
| `text/captions.vtt` | `WEBVTT\n\n00:00:01.000 --> 00:00:02.500\nHello there\n` | vtt / vtt |
| `text/lyrics.lrc` | `[ti:Sample]\n[ar:Nobody]\n[00:01.00]First line\n[00:02.50]Second line\n` | lrc / lrc |
| `text/captions.sbv` | `0:00:01.000,0:00:02.500\nHello there\n\n0:00:03.000,0:00:04.000\nBye\n` | sbv / sbv |
| `text/movie.sub` | `{0}{25}Hello there\n{50}{75}Bye\n` | microdvd / microdvd |
| `text/styled.ass` | `[Script Info]\nScriptType: v4.00+\n\n[Events]\nFormat: Layer, Start, End, Style, Text\n` | ass / ass |
| `text/playlist.m3u` | `#EXTM3U\n#EXTINF:3,Tone\ntone.mp3\n` | m3u / m3u |
| `text/plain_list.m3u` | `tone.mp3\nother.mp3\n` | m3u / txt |
| `text/hls.m3u8` | `#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXTINF:2.0,\nseg0.ts\n#EXT-X-ENDLIST\n` | m3u8 / m3u8 |
| `text/radio.pls` | `[playlist]\nFile1=http://example.com/stream\nNumberOfEntries=1\nVersion=2\n` | pls / pls |
| `text/disc.cue` | `FILE "disc.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n` | cue / cue |
| `text/event.ics` | `BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Sample//EN\r\nBEGIN:VEVENT\r\nUID:1@example.invalid\r\nDTSTART:20000101T000000Z\r\nSUMMARY:Test\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n` | ics / ics |
| `text/contact.vcf` | `BEGIN:VCARD\r\nVERSION:4.0\r\nFN:Sample Contact\r\nEND:VCARD\r\n` | vcf / vcf |
| `text/directory.ldif` | `dn: cn=sample,dc=example,dc=invalid\ncn: sample\nobjectClass: person\n` | ldif / ldif |
| `text/change.diff` | `diff --git a/x.txt b/x.txt\n--- a/x.txt\n+++ b/x.txt\n@@ -1 +1 @@\n-old\n+new\n` | diff / diff |
| `text/script.sh` | `#!/bin/sh\necho hello\n` | sh / sh |
| `text/tool.py` | `#!/usr/bin/env python3\nprint('hello')\n` | py / py |
| `text/unnamed_python.py` | `import os\nfrom sys import argv\n\n\ndef main():\n    print(os.getcwd(), argv)\n\n\nif __name__ == "__main__":\n    main()\n` | py / py |
| `text/tool.pl` | `#!/usr/bin/perl\nprint "hello\\n";\n` | pl / pl |
| `text/tool.rb` | `#!/usr/bin/env ruby\nputs 'hello'\n` | rb / rb |
| `text/page.php` | `<?php\necho 'hello';\n` | php / php |
| `text/hello.c` | `#include <stdio.h>\n#include <stdlib.h>\n\nint main(void) {\n  puts("hello");\n  return 0;\n}\n` | c / c |
| `text/hello.cc` | `#include <iostream>\n\nint main() {\n  std::cout << "hello" << std::endl;\n}\n` | cpp / cpp |
| `text/hello.go` | `package main\n\nimport "fmt"\n\nfunc main() {\n\tfmt.Println("hello")\n}\n` | go / go |
| `text/hello.rs` | `use std::io;\n\nfn main() {\n    let mut s = String::new();\n    println!("hello {}", s.len());\n}\n` | rs / rs |
| `text/Hello.java` | `package demo;\n\nimport java.util.List;\n\npublic class Hello {\n  public static void main(String[] args) {}\n}\n` | java / java |
| `text/query.sql` | `CREATE TABLE t (a INTEGER);\nINSERT INTO t VALUES (1);\nSELECT a FROM t;\n` | sql / sql |
| `text/paper.tex` | `\\documentclass{article}\n\\begin{document}\nHello.\n\\end{document}\n` | tex / tex |
| `text/refs.bib` | `@article{key1,\n  title = {Sample},\n  year = {2000}\n}\n` | bib / bib |
| `text/refs.ris` | `TY  - JOUR\nTI  - Sample\nER  - \n` | ris / ris |
| `text/family.ged` | `0 HEAD\n1 GEDC\n2 VERS 5.5.1\n0 TRLR\n` | ged / ged |
| `text/game.pgn` | `[Event "Sample"]\n[Result "*"]\n\n1. e4 e5 *\n` | pgn / pgn |
| `text/game.sgf` | `(;FF[4]GM[1]SZ[9];B[ee])\n` | sgf / sgf |
| `text/seq.fasta` | `>seq1 sample\nACGTACGTACGT\nACGT\n` | fasta / fasta |
| `text/reads.fastq` | `@read1\nACGTACGT\n+\nIIIIIIII\n` | fastq / fastq |
| `text/aln.sam` | `@HD\tVN:1.6\tSO:unsorted\n@SQ\tSN:chr1\tLN:100\n` | sam / sam |
| `text/calls.vcf` | `##fileformat=VCFv4.2\n#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n` | vcf-variant / vcf-variant |
| `text/genes.gff3` | `##gff-version 3\nchr1\t.\tgene\t1\t10\t.\t+\t.\tID=g1\n` | gff3 / gff3 |
| `text/protein.pdb` | `HEADER    SAMPLE\nTITLE     SAMPLE STRUCTURE\nATOM      1  N   ALA A   1       0.000   0.000   0.000  1.00  0.00           N\nEND\n` | pdb / pdb |
| `text/crystal.cif` | `data_sample\n_cell_length_a 1.0\nloop_\n_atom_site_label\nC1\n` | cif / cif |
| `text/molecule.mol` | `methane\n  sample\n\n  1  0  0  0  0  0  0  0  0  0999 V2000\n    0.0000    0.0000    0.0000 C   0  0\nM  END\n` | mol / mol |
| `text/molecules.sdf` | `methane\n  sample\n\n  1  0  0  0  0  0  0  0  0  0999 V2000\n    0.0000    0.0000    0.0000 C   0  0\nM  END\n$$$$\n` | sdf / sdf |
| `text/molecule.mol2` | `@<TRIPOS>MOLECULE\nsample\n 1 0 0 0 0\nSMALL\n` | mol2 / mol2 |
| `text/cube.obj` | `v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n` | obj / obj |
| `text/cube.mtl` | `newmtl red\nKd 1 0 0\n` | mtl / mtl |
| `text/mesh.off` | `OFF\n3 1 0\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n` | off / off |
| `text/mesh.ply` | `ply\nformat ascii 1.0\nelement vertex 0\nend_header\n` | ply / ply |
| `text/part.stl` | `solid sample\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0\n  endloop\n endfacet\nendsolid sample\n` | stl / stl |
| `text/drawing.dxf` | `  0\nSECTION\n  2\nHEADER\n  0\nENDSEC\n  0\nEOF\n` | dxf / dxf |
| `text/part.step` | `ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\nENDSEC;\nEND-ISO-10303-21;\n` | step / step |
| `text/building.ifc` | `ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nEND-ISO-10303-21;\n` | ifc / ifc |
| `text/world.wrl` | `#VRML V2.0 utf8\nShape {}\n` | wrl / wrl |
| `text/print.gcode` | `;FLAVOR:Marlin\nG28\nG1 X10 Y10\nG1 X20 Y20\nM104 S0\n` | gcode / gcode |
| `text/firmware.hex` | `:10010000214601360121470136007EFE09D2190140\n:00000001FF\n` | ihex / ihex |
| `text/firmware.srec` | `S00F000068656C6C6F202020202000003C\nS11F00007C0802A6900100049421FFF07C6C1B787C8C23783C6000003863000026\nS9030000FC\n` | srec / srec |
| `text/graph.gv` | `digraph G {\n  a -> b;\n}\n` | gv / gv |
| `text/diagram.puml` | `@startuml\nA -> B\n@enduml\n` | puml / puml |
| `text/flow.mmd` | `graph TD\n  A --> B\n` | mmd / mmd |
| `text/settings.reg` | `Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Sample]\r\n"Value"="1"\r\n` | reg / reg |
| `text/app.desktop` | `[Desktop Entry]\nType=Application\nName=Sample\nExec=sample\n` | desktop / desktop |
| `text/link.url` | `[InternetShortcut]\r\nURL=https://example.com/\r\n` | url / url |
| `text/driver.inf` | `[Version]\r\nSignature="$Windows NT$"\r\nClass=Sample\r\n` | inf / inf |
| `text/Makefile` | `all:\n\techo hello\n` | makefile / txt |
| `text/Dockerfile` | `FROM debian:stable\nRUN echo hello\nCMD ["true"]\n` | dockerfile / dockerfile |
| `text/CMakeLists.txt` | `cmake_minimum_required(VERSION 3.22)\nproject(sample)\n` | cmake / cmake |
| `text/.gitignore` | `build/\n*.o\n` | gitignore / txt |
| `text/script.bat` | `@echo off\r\necho hello\r\n` | bat / bat |
| `text/notes.org` | `#+TITLE: Notes\n* Heading\n` | org / org |
| `text/scene.usda` | `#usda 1.0\n\ndef Xform "root" {}\n` | usda / usda |
| `text/motion.bvh` | `HIERARCHY\nROOT Hips\n{\n}\n` | bvh / bvh |
| `text/cloud.pcd` | `# .PCD v0.7 - Point Cloud Data file format\nVERSION 0.7\nFIELDS x y z\n` | pcd / pcd |
| `text/grid.asc` | `ncols 2\nnrows 2\nxllcorner 0\nyllcorner 0\ncellsize 1\n1 2\n3 4\n` | asc-grid / asc-grid |
| `text/projection.prj` | `GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]]` | prj / prj |
| `text/schema.proto` | `syntax = "proto3";\nmessage Sample { int32 id = 1; }\n` | proto / proto |
| `text/page.mhtml` | `From: <Saved>\r\nSubject: page\r\nDate: Thu, 01 Jan 1970 00:00:00 +0000\r\nMIME-Version: 1.0\r\nContent-Type: multipart/related; boundary="b"\r\n\r\n--b\r\nContent-Type: text/html\r\nContent-Location: http://example.invalid/\r\n\r\n<html></html>\r\n--b--\r\n` | mhtml / mhtml |
| `text/manual.1` | `.TH SAMPLE 1\n.SH NAME\nsample\n` | roff / roff |
| `text/notebook.nb` | `(* Content-type: application/vnd.wolfram.mathematica *)\nNotebook[{}]\n` | nb / nb |
| `text/feature.feature` | `Feature: Sample\n  Scenario: works\n` | feature / feature |
| `text/id_test.asc` | `-----BEGIN PGP PUBLIC KEY BLOCK-----\n\nmQENBAAAAAA=\n=AAAA\n-----END PGP PUBLIC KEY BLOCK-----\n` | asc / asc |
| `text/plain.txt` | `Just some notes.\nNothing more.\n` | txt / txt |
