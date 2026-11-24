# Sources: audio_lossless

Fixtures for `core/tests/families/audio_lossless/`. All content is synthetic
(sines, chirps and pseudo-random noise); there is no recorded sound and no
personal or machine data (`ffmpeg` was run with bit-exact flags so that no
encoder name is written; the reference `flac` encoder writes its own vendor
string into Vorbis comments, which is part of the format).

## Tools

- `python3` 3.14 standard library (`wave`): the `ref_*` reference PCM files, written by
  `gen_ref.py` below (`python3 gen_ref.py .`).
- `afconvert` (Audio File Convert 2.0, macOS): `af_*` files and the `*_afconvert_decoded.wav`
  reference decodes (Apple's decoders) of the lossy fixtures.
- Ubuntu 24.04 packages in a throwaway container: `flac` 1.4.3 (`flac_*`), `wavpack` 5.6.0
  (`wv_*`), `sox` 14.4.2 (`sox_*`), `ffmpeg` 6.1.1 (`ff_*`, `tta_*`).
- `pic_4x4.png` used for the FLAC picture block is a 4x4 RGB gradient written by Python's
  `zlib` (not committed).

Commands run in this directory; `ff_extensible_s16_6ch_16000.wav` is the 6-channel
input for the files that need a proper channel mask.

## Files

| File | Contents | Tool | Command |
| --- | --- | --- | --- |
| `af_alac16_51_16000.m4a` | Encoded from `ff_extensible_s16_6ch_16000.wav` | afconvert | `afconvert -f m4af -d alac ff_extensible_s16_6ch_16000.wav af_alac16_51_16000.m4a` |
| `af_alac16_mono_8000.m4a` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f m4af -d alac ref_s16_mono_8000.wav af_alac16_mono_8000.m4a` |
| `af_alac16_stereo_11025.caf` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f caff -d alac ref_s16_stereo_11025.wav af_alac16_stereo_11025.caf` |
| `af_alac16_stereo_11025.m4a` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f m4af -d alac ref_s16_stereo_11025.wav af_alac16_stereo_11025.m4a` |
| `af_alac24_stereo_22050.caf` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f caff -d alac/3 ref_s24_stereo_22050.wav af_alac24_stereo_22050.caf` |
| `af_alac24_stereo_22050.m4a` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f m4af -d alac/3 ref_s24_stereo_22050.wav af_alac24_stereo_22050.m4a` |
| `af_alaw_mono_8000.aifc` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f AIFC -d alaw ref_s16_mono_8000.wav af_alaw_mono_8000.aifc` |
| `af_alaw_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f WAVE -d alaw ref_s16_mono_8000.wav af_alaw_mono_8000.wav` |
| `af_bw64_stereo_22050.wav` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f BW64 -d LEI24 ref_s24_stereo_22050.wav af_bw64_stereo_22050.wav` |
| `af_f32_stereo_11025.wav` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f WAVE -d LEF32 ref_s16_stereo_11025.wav af_f32_stereo_11025.wav` |
| `af_f64_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f WAVE -d LEF64 ref_s16_mono_8000.wav af_f64_mono_8000.wav` |
| `af_fl32_stereo_11025.aifc` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f AIFC -d BEF32 ref_s16_stereo_11025.wav af_fl32_stereo_11025.aifc` |
| `af_ima4_mono_8000.aifc` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f AIFC -d ima4 ref_s16_mono_8000.wav af_ima4_mono_8000.aifc` |
| `af_ima4_stereo_11025.aifc` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f AIFC -d ima4 ref_s16_stereo_11025.wav af_ima4_stereo_11025.aifc` |
| `af_ima4_stereo_11025.caf` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f caff -d ima4 ref_s16_stereo_11025.wav af_ima4_stereo_11025.caf` |
| `af_ima4_stereo_11025_afconvert_decoded.wav` | Decoded by Apple `af_ima4_stereo_11025.aifc` | afconvert | `afconvert -f WAVE -d LEI16 af_ima4_stereo_11025.aifc af_ima4_stereo_11025_afconvert_decoded.wav` |
| `af_rf64_stereo_11025.wav` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f RF64 -d LEI16 ref_s16_stereo_11025.wav af_rf64_stereo_11025.wav` |
| `af_s16_51_16000.wav` | Encoded from `ref_s16_6ch_16000.wav` | afconvert | `afconvert -f WAVE -d LEI16 -l MPEG_5_1_A ref_s16_6ch_16000.wav af_s16_51_16000.wav` |
| `af_s16_stereo_11025.aiff` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f AIFF -d BEI16 ref_s16_stereo_11025.wav af_s16_stereo_11025.aiff` |
| `af_s16_stereo_11025.au` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f NeXT -d BEI16 ref_s16_stereo_11025.wav af_s16_stereo_11025.au` |
| `af_s16_stereo_11025.caf` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f caff -d LEI16 ref_s16_stereo_11025.wav af_s16_stereo_11025.caf` |
| `af_s16_stereo_11025.w64` | Encoded from `ref_s16_stereo_11025.wav` | afconvert | `afconvert -f W64f -d LEI16 ref_s16_stereo_11025.wav af_s16_stereo_11025.w64` |
| `af_s24_stereo_22050.aiff` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f AIFF -d BEI24 ref_s24_stereo_22050.wav af_s24_stereo_22050.aiff` |
| `af_s24_stereo_22050.au` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f NeXT -d BEI24 ref_s24_stereo_22050.wav af_s24_stereo_22050.au` |
| `af_s24_stereo_22050.wav` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f WAVE -d LEI24 ref_s24_stereo_22050.wav af_s24_stereo_22050.wav` |
| `af_s24be_stereo_22050.caf` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f caff -d BEI24 ref_s24_stereo_22050.wav af_s24be_stereo_22050.caf` |
| `af_s32_stereo_22050.aiff` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f AIFF -d BEI32 ref_s24_stereo_22050.wav af_s32_stereo_22050.aiff` |
| `af_s32_stereo_22050.wav` | Encoded from `ref_s24_stereo_22050.wav` | afconvert | `afconvert -f WAVE -d LEI32 ref_s24_stereo_22050.wav af_s32_stereo_22050.wav` |
| `af_s8_mono_8000.aifc` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f AIFC -d I8 ref_s16_mono_8000.wav af_s8_mono_8000.aifc` |
| `af_s8_mono_8000.aiff` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f AIFF -d BEI8 ref_s16_mono_8000.wav af_s8_mono_8000.aiff` |
| `af_u8_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f WAVE -d UI8 ref_s16_mono_8000.wav af_u8_mono_8000.wav` |
| `af_ulaw_mono_8000.aifc` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f AIFC -d ulaw ref_s16_mono_8000.wav af_ulaw_mono_8000.aifc` |
| `af_ulaw_mono_8000.au` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f NeXT -d ulaw ref_s16_mono_8000.wav af_ulaw_mono_8000.au` |
| `af_ulaw_mono_8000.caf` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f caff -d ulaw ref_s16_mono_8000.wav af_ulaw_mono_8000.caf` |
| `af_ulaw_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | afconvert | `afconvert -f WAVE -d ulaw ref_s16_mono_8000.wav af_ulaw_mono_8000.wav` |
| `ff_alac_s16_stereo_11025.caf` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a alac ff_alac_s16_stereo_11025.caf` |
| `ff_alac_s16_stereo_11025.m4a` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a alac ff_alac_s16_stereo_11025.m4a` |
| `ff_alac_tags_s16_mono_8000.m4a` | Encoded from `ref_s16_mono_8000.wav` | ffmpeg | `ffmpeg -i ref_s16_mono_8000.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -metadata title="Test tone" -metadata artist="Convertible test" -c:a alac ff_alac_tags_s16_mono_8000.m4a` |
| `ff_bext_s16_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | ffmpeg | `ffmpeg -i ref_s16_mono_8000.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -metadata title="Test tone" -metadata artist="Convertible test" -write_bext 1 -c:a pcm_s16le ff_bext_s16_mono_8000.wav` |
| `ff_extensible_s16_6ch_16000.wav` | Encoded from `ref_s16_6ch_16000.wav` | ffmpeg | `ffmpeg -i ref_s16_6ch_16000.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a pcm_s16le ff_extensible_s16_6ch_16000.wav` |
| `ff_extensible_s24_stereo_22050.wav` | Encoded from `ref_s24_stereo_22050.wav` | ffmpeg | `ffmpeg -i ref_s24_stereo_22050.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a pcm_s24le ff_extensible_s24_stereo_22050.wav` |
| `ff_f32_stereo_11025.au` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a pcm_f32be ff_f32_stereo_11025.au` |
| `ff_id3_s16_mono_8000.aiff` | Encoded from `ref_s16_mono_8000.wav` | ffmpeg | `ffmpeg -i ref_s16_mono_8000.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -metadata title="Test tone" -metadata artist="Convertible test" -write_id3v2 1 -c:a pcm_s16be ff_id3_s16_mono_8000.aiff` |
| `ff_imaadpcm_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | ffmpeg | `ffmpeg -i ref_s16_mono_8000.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a adpcm_ima_wav ff_imaadpcm_mono_8000.wav` |
| `ff_imaadpcm_stereo_11025.wav` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a adpcm_ima_wav ff_imaadpcm_stereo_11025.wav` |
| `ff_imaadpcm_stereo_11025_afconvert_decoded.wav` | Decoded by Apple `ff_imaadpcm_stereo_11025.wav` | afconvert | `afconvert -f WAVE -d LEI16 ff_imaadpcm_stereo_11025.wav ff_imaadpcm_stereo_11025_afconvert_decoded.wav` |
| `ff_info_s16_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | ffmpeg | `ffmpeg -i ref_s16_mono_8000.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -metadata title="Test tone" -metadata artist="Convertible test" -metadata comment="synthetic" -c:a pcm_s16le ff_info_s16_mono_8000.wav` |
| `ff_msadpcm_stereo_11025.wav` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a adpcm_ms ff_msadpcm_stereo_11025.wav` |
| `ff_msadpcm_stereo_11025_afconvert_decoded.wav` | Decoded by Apple `ff_msadpcm_stereo_11025.wav` | afconvert | `afconvert -f WAVE -d LEI16 ff_msadpcm_stereo_11025.wav ff_msadpcm_stereo_11025_afconvert_decoded.wav` |
| `ff_rf64_s16_stereo_11025.wav` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a pcm_s16le -rf64 always ff_rf64_s16_stereo_11025.wav` |
| `ff_s24_stereo_22050.w64` | Encoded from `ref_s24_stereo_22050.wav` | ffmpeg | `ffmpeg -i ref_s24_stereo_22050.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a pcm_s24le -f w64 ff_s24_stereo_22050.w64` |
| `ff_sowt_stereo_11025.aifc` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a pcm_s16le ff_sowt_stereo_11025.aifc` |
| `ff_stream_s16_stereo_11025.wav` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a pcm_s16le -f wav - > ff_stream_s16_stereo_11025.wav` |
| `flac_b1152_s16_mono_8000.flac` | Encoded from `ref_s16_mono_8000.wav` | flac | `flac -s -f --no-padding -5 -b 1152 -o flac_b1152_s16_mono_8000.flac ref_s16_mono_8000.wav` |
| `flac_b16384_s16_stereo_11025.flac` | Encoded from `ref_s16_stereo_11025.wav` | flac | `flac -s -f --no-padding --lax -b 16384 -o flac_b16384_s16_stereo_11025.flac ref_s16_stereo_11025.wav` |
| `flac_exhaustive_s16_stereo_11025.flac` | Encoded from `ref_s16_stereo_11025.wav` | flac | `flac -s -f --no-padding -8 -e -p -o flac_exhaustive_s16_stereo_11025.flac ref_s16_stereo_11025.wav` |
| `flac_l0_s16_stereo_11025.flac` | Encoded from `ref_s16_stereo_11025.wav` | flac | `flac -s -f --no-padding -0 -o flac_l0_s16_stereo_11025.flac ref_s16_stereo_11025.wav` |
| `flac_l5_s16_stereo_11025.flac` | Encoded from `ref_s16_stereo_11025.wav` | flac | `flac -s -f --no-padding -5 -o flac_l5_s16_stereo_11025.flac ref_s16_stereo_11025.wav` |
| `flac_l8_s16_stereo_11025.flac` | Encoded from `ref_s16_stereo_11025.wav` | flac | `flac -s -f --no-padding -8 -o flac_l8_s16_stereo_11025.flac ref_s16_stereo_11025.wav` |
| `flac_s16_6ch_16000.flac` | Encoded from `ff_extensible_s16_6ch_16000.wav` | flac | `flac -s -f --no-padding -8 -o flac_s16_6ch_16000.flac ff_extensible_s16_6ch_16000.wav` |
| `flac_s16_stereo_11025.oga` | Encoded from `ref_s16_stereo_11025.wav` | flac | `flac -s -f --no-padding -5 --ogg --serial-number=1 -o flac_s16_stereo_11025.oga ref_s16_stereo_11025.wav` |
| `flac_s24_stereo_22050.flac` | Encoded from `ref_s24_stereo_22050.wav` | flac | `flac -s -f --no-padding -5 -o flac_s24_stereo_22050.flac ref_s24_stereo_22050.wav` |
| `flac_s32_stereo_8000.flac` | Encoded from `ref_s32_stereo_8000.wav` | flac | `flac -s -f --no-padding -5 -o flac_s32_stereo_8000.flac ref_s32_stereo_8000.wav` |
| `flac_tags_s16_mono_8000.flac` | Encoded from `ref_s16_mono_8000.wav` | flac | `flac -s -f --no-padding -5 -T "TITLE=Test tone" -T "ARTIST=Convertible test" -T "ALBUM=Fixtures" -T "TRACKNUMBER=3" --picture=pic_4x4.png -o flac_tags_s16_mono_8000.flac ref_s16_mono_8000.wav` |
| `flac_u8_mono_8000.flac` | Encoded from `ref_u8_mono_8000.wav` | flac | `flac -s -f --no-padding -5 -o flac_u8_mono_8000.flac ref_u8_mono_8000.wav` |
| `ref_s16_6ch_16000.wav` | 800 frames, 16000 Hz, 6 channels, 16-bit, legacy (non-extensible) header; channel c: sine at 440(c+1)+37c Hz, odd channels chirps. | python3 `wave` | `python3 gen_ref.py .` |
| `ref_s16_mono_8000.wav` | 2000 frames, 8000 Hz, mono, 16-bit, 0.45 sin(440 Hz) + noise. | python3 `wave` | `python3 gen_ref.py .` |
| `ref_s16_stereo_11025.wav` | 2000 frames, 11025 Hz, stereo, 16-bit. Left: 0.45 sin(440 Hz) + 0.04 noise; right: 0.4 linear chirp 200 -> 2200 Hz + 0.03 noise. First left sample -479. | python3 `wave` | `python3 gen_ref.py .` |
| `ref_s24_stereo_22050.wav` | 1500 frames, 22050 Hz, stereo, 24-bit (legacy format tag 1), same signal recipe. | python3 `wave` | `python3 gen_ref.py .` |
| `ref_s32_stereo_8000.wav` | 1000 frames, 8000 Hz, stereo, 32-bit integer (full 32-bit noise). | python3 `wave` | `python3 gen_ref.py .` |
| `ref_u8_mono_8000.wav` | 2000 frames, 8000 Hz, mono, unsigned 8-bit, 0.45 sin(440 Hz) + noise. | python3 `wave` | `python3 gen_ref.py .` |
| `sox_alaw_stereo_11025.au` | Encoded from `ref_s16_stereo_11025.wav` | sox | `sox ref_s16_stereo_11025.wav -e a-law sox_alaw_stereo_11025.au` |
| `sox_gsm610_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | sox | `sox ref_s16_mono_8000.wav -e gsm-full-rate -t wav sox_gsm610_mono_8000.wav` |
| `sox_gsm610_mono_8000_afconvert_decoded.wav` | Decoded by Apple `sox_gsm610_mono_8000.wav` | afconvert | `afconvert -f WAVE -d LEI16 sox_gsm610_mono_8000.wav sox_gsm610_mono_8000_afconvert_decoded.wav` |
| `sox_imaadpcm_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | sox | `sox ref_s16_mono_8000.wav -e ima-adpcm sox_imaadpcm_mono_8000.wav` |
| `sox_imaadpcm_mono_8000_afconvert_decoded.wav` | Decoded by Apple `sox_imaadpcm_mono_8000.wav` | afconvert | `afconvert -f WAVE -d LEI16 sox_imaadpcm_mono_8000.wav sox_imaadpcm_mono_8000_afconvert_decoded.wav` |
| `sox_msadpcm_mono_8000.wav` | Encoded from `ref_s16_mono_8000.wav` | sox | `sox ref_s16_mono_8000.wav -e ms-adpcm sox_msadpcm_mono_8000.wav` |
| `sox_msadpcm_stereo_11025.wav` | Encoded from `ref_s16_stereo_11025.wav` | sox | `sox ref_s16_stereo_11025.wav -e ms-adpcm sox_msadpcm_stereo_11025.wav` |
| `sox_msadpcm_stereo_11025_afconvert_decoded.wav` | Decoded by Apple `sox_msadpcm_stereo_11025.wav` | afconvert | `afconvert -f WAVE -d LEI16 sox_msadpcm_stereo_11025.wav sox_msadpcm_stereo_11025_afconvert_decoded.wav` |
| `sox_s16_stereo_11025.au` | Encoded from `ref_s16_stereo_11025.wav` | sox | `sox ref_s16_stereo_11025.wav sox_s16_stereo_11025.au` |
| `sox_s16_stereo_11025.sph` | Encoded from `ref_s16_stereo_11025.wav` | sox | `sox ref_s16_stereo_11025.wav sox_s16_stereo_11025.sph` |
| `sox_s16_stereo_11025.voc` | Encoded from `ref_s16_stereo_11025.wav` | sox | `sox ref_s16_stereo_11025.wav sox_s16_stereo_11025.voc` |
| `sox_s8_mono_8000.8svx` | Encoded from `ref_s16_mono_8000.wav` | sox | `sox ref_s16_mono_8000.wav -b 8 sox_s8_mono_8000.8svx dither -s` |
| `sox_stream_s16_stereo_11025.wav` | Encoded from `ref_s16_stereo_11025.wav` | sox | `sox ref_s16_stereo_11025.wav -t wav - > sox_stream_s16_stereo_11025.wav` |
| `sox_u8_mono_8000.voc` | Encoded from `ref_u8_mono_8000.wav` | sox | `sox ref_u8_mono_8000.wav sox_u8_mono_8000.voc` |
| `sox_ulaw_mono_8000.sph` | Encoded from `ref_s16_mono_8000.wav` | sox | `sox ref_s16_mono_8000.wav -e u-law sox_ulaw_mono_8000.sph` |
| `tta_s16_stereo_11025.tta` | Encoded from `ref_s16_stereo_11025.wav` | ffmpeg | `ffmpeg -i ref_s16_stereo_11025.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a tta tta_s16_stereo_11025.tta` |
| `tta_s24_stereo_22050.tta` | Encoded from `ref_s24_stereo_22050.wav` | ffmpeg | `ffmpeg -i ref_s24_stereo_22050.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a tta tta_s24_stereo_22050.tta` |
| `tta_u8_mono_8000.tta` | Encoded from `ref_u8_mono_8000.wav` | ffmpeg | `ffmpeg -i ref_u8_mono_8000.wav -fflags +bitexact -flags:a +bitexact -map_metadata -1 -c:a tta tta_u8_mono_8000.tta` |
| `wv_f32_stereo_11025.wv` | Encoded from `af_f32_stereo_11025.wav` | wavpack | `wavpack -q -y -o wv_f32_stereo_11025.wv af_f32_stereo_11025.wav` |
| `wv_fast_s16_stereo_11025.wv` | Encoded from `ref_s16_stereo_11025.wav` | wavpack | `wavpack -q -y -f -o wv_fast_s16_stereo_11025.wv ref_s16_stereo_11025.wav` |
| `wv_hh_s16_stereo_11025.wv` | Encoded from `ref_s16_stereo_11025.wav` | wavpack | `wavpack -q -y -hh -x3 -o wv_hh_s16_stereo_11025.wv ref_s16_stereo_11025.wav` |
| `wv_hybrid_s16_stereo_11025.wv` | Encoded from `ref_s16_stereo_11025.wav` | wavpack | `wavpack -q -y -b3.0 -o wv_hybrid_s16_stereo_11025.wv ref_s16_stereo_11025.wav` |
| `wv_hybrid_s24_stereo_22050.wv` | Encoded from `ref_s24_stereo_22050.wav` | wavpack | `wavpack -q -y -b3.0 -o wv_hybrid_s24_stereo_22050.wv ref_s24_stereo_22050.wav` |
| `wv_hybridc_s16_mono_8000.wv` | Encoded from `ref_s16_mono_8000.wav` | wavpack | `wavpack -q -y -b2.5 -c -o wv_hybridc_s16_mono_8000.wv ref_s16_mono_8000.wav  (the .wvc correction file is not kept)` |
| `wv_s16_6ch_16000.wv` | Encoded from `ref_s16_6ch_16000.wav` | wavpack | `wavpack -q -y -o wv_s16_6ch_16000.wv ref_s16_6ch_16000.wav` |
| `wv_s16_stereo_11025.wv` | Encoded from `ref_s16_stereo_11025.wav` | wavpack | `wavpack -q -y -o wv_s16_stereo_11025.wv ref_s16_stereo_11025.wav` |
| `wv_s24_stereo_22050.wv` | Encoded from `ref_s24_stereo_22050.wav` | wavpack | `wavpack -q -y -o wv_s24_stereo_22050.wv ref_s24_stereo_22050.wav` |
| `wv_s32_stereo_8000.wv` | Encoded from `ref_s32_stereo_8000.wav` | wavpack | `wavpack -q -y -o wv_s32_stereo_8000.wv ref_s32_stereo_8000.wav` |
| `wv_u8_mono_8000.wv` | Encoded from `ref_u8_mono_8000.wav` | wavpack | `wavpack -q -y -o wv_u8_mono_8000.wv ref_u8_mono_8000.wav` |

## gen_ref.py

```python
import math, struct, sys, wave, os

out = sys.argv[1]

def samples(frames, channels, rate, bits, seed=1):
    state = seed * 2654435761 % (1 << 32) or 1
    full = (1 << (bits - 1)) - 1
    data = []
    for n in range(frames):
        for c in range(channels):
            state ^= (state << 13) & 0xFFFFFFFF
            state ^= state >> 17
            state ^= (state << 5) & 0xFFFFFFFF
            noise = (state / 4294967296.0) * 2 - 1
            f = 440.0 * (c + 1) + 37.0 * c
            v = 0.45 * math.sin(2 * math.pi * f * n / rate) + 0.04 * noise
            if c % 2 == 1:
                v = 0.4 * math.sin(2 * math.pi * (200 + 2000 * n / frames) * n / rate) + 0.03 * noise
            data.append(max(-full - 1, min(full, int(round(v * (full + 1))))))
    return data

def write(name, frames, channels, rate, bits):
    vals = samples(frames, channels, rate, bits)
    w = wave.open(os.path.join(out, name), 'wb')
    w.setnchannels(channels)
    w.setsampwidth(bits // 8)
    w.setframerate(rate)
    raw = bytearray()
    for v in vals:
        if bits == 8:
            raw += struct.pack('<B', v + 128)
        elif bits == 16:
            raw += struct.pack('<h', v)
        elif bits == 24:
            raw += struct.pack('<i', v)[:3]
        else:
            raw += struct.pack('<i', v)
    w.writeframes(bytes(raw))
    w.close()

write('ref_s16_stereo_11025.wav', 2000, 2, 11025, 16)
write('ref_s24_stereo_22050.wav', 1500, 2, 22050, 24)
write('ref_u8_mono_8000.wav', 2000, 1, 8000, 8)
write('ref_s16_mono_8000.wav', 2000, 1, 8000, 16)
write('ref_s32_stereo_8000.wav', 1000, 2, 8000, 32)
write('ref_s16_6ch_16000.wav', 800, 6, 16000, 16)
write('ref_s16_stereo_44100.wav', 4410, 2, 44100, 16)
```

## Interoperability of Convertible's own output (checked while developing)

Files written by the family's encoders were decoded by independent tools and compared
sample by sample with the input (not committed; see the family report):
- `flac -t` (reference decoder, CRC and MD5 check) passed for 16/24/8-bit, mono/stereo/6-channel
  and all levels 0-8; `ffmpeg` decodes them bit-exactly.
- ALAC (`.m4a`, `.caf`): `afconvert` and `ffmpeg` decode 16/24-bit, mono/stereo/5.1 bit-exactly.
- WavPack: `wvunpack -v` verifies every block; `ffmpeg` decodes bit-exactly.
- TTA, AIFF, AIFF-C (sowt, fl32), AU, CAF, W64, RF64, WAVE (PCM, float, extensible 6-channel),
  VOC, NIST SPHERE: `ffmpeg` decodes bit-exactly; `afconvert` decodes WAV/AIFF/AIFC/CAF/AU/ALAC
  bit-exactly; `sox` reads VOC, 8SVX, SPHERE and the ADPCM WAVs.
