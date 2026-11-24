# MPEG audio fixtures

Sources and reference decoding: see `../SOURCES.md`. `src/` holds the
generated WAV signals. Every `*_ref.flac` is ffmpeg's decode, except
`lame_free200_js_44100_ref.flac`, decoded by mpg123 (`mpg123 -w full.wav`)
because ffmpeg 6.1 refuses that free-format stream.

| File | Content | Command |
| --- | --- | --- |
| lame_cbr128_js_44100.mp3 | MPEG-1 L3, 128 kbit/s, joint stereo, Info + LAME tag, 26460 frames after gapless trim | `lame --quiet --noreplaygain -b 128 -m j src/music_44100_st.wav lame_cbr128_js_44100.mp3` |
| lame_v2_st_44100.mp3 | VBR (-V 2), plain stereo, Xing + LAME tag | `lame --quiet --noreplaygain -V 2 -m s src/music_44100_st.wav lame_v2_st_44100.mp3` |
| lame_cbr64_mono_22050.mp3 | MPEG-2 LSF, mono | `lame --quiet --noreplaygain -b 64 -m m src/music_22050_mono.wav lame_cbr64_mono_22050.mp3` |
| lame_cbr24_mono_8000.mp3 | MPEG-2.5, 8 kHz | `lame --quiet --noreplaygain -b 24 -m m src/music_8000_mono.wav lame_cbr24_mono_8000.mp3` |
| lame_free200_js_44100.mp3 | free format 200 kbit/s, no tag | `lame --quiet --noreplaygain --freeformat -t -b 200 -m j src/music_44100_st.wav lame_free200_js_44100.mp3` |
| lame_cbr160_nores_32000.mp3 | 32 kHz, no bit reservoir | `lame --quiet --noreplaygain -b 160 -m j --nores src/music_32000_st.wav lame_cbr160_nores_32000.mp3` |
| lame_notag_st_48000.mp3 | 48 kHz, no Xing/LAME tag (delays not trimmed) | `lame --quiet --noreplaygain -t -b 96 -m s src/music_48000_st.wav lame_notag_st_48000.mp3` |
| lame_id3v23_mono_22050.mp3 | ID3v2.3: title "Test Tone", artist "Convertible Fixtures", album "Synthetic", year 2024, track 3, genre Ambient | `lame --quiet --noreplaygain -b 96 -m m --id3v2-only --tt "Test Tone" --ta "Convertible Fixtures" --tl "Synthetic" --ty 2024 --tn 3 --tg Ambient src/music_22050_mono.wav lame_id3v23_mono_22050.mp3` |
| lame_id3v1_mono_22050.mp3 | the same tags as ID3v1.1 | as above with `--id3v1-only` and without `--tg` |
| ff_id3v24_cover_mono_22050.mp3 | ID3v2.4 with an 8x8 PNG cover (title "Cover Test") around the 64 kbit/s stream | `ffmpeg -f lavfi -i "color=c=0x336699:s=8x8:d=1" -frames:v 1 -fflags +bitexact cover.png` then `ffmpeg -i lame_cbr64_mono_22050.mp3 -i cover.png -map 0:a -map 1:v -c copy -map_metadata -1 -fflags +bitexact -id3v2_version 4 -metadata title="Cover Test" -metadata artist="Convertible Fixtures" -metadata:s:v comment="Cover (front)" -write_xing 0 ff_id3v24_cover_mono_22050.mp3` |
| ff_mp2_192_st_48000.mp2 | Layer II 192 kbit/s stereo | `ffmpeg -i src/music_48000_st.wav -c:a mp2 -b:a 192k -fflags +bitexact -map_metadata -1 ff_mp2_192_st_48000.mp2` |
| ff_mp2_48_mono_24000.mp2 | Layer II LSF (MPEG-2) mono | `ffmpeg -i src/music_24000_mono.wav -c:a mp2 -b:a 48k -fflags +bitexact -map_metadata -1 ff_mp2_48_mono_24000.mp2` |
| twolame_160_js_44100.mp2 | Layer II joint stereo | `twolame --quiet -b 160 -m j src/music_44100_st.wav twolame_160_js_44100.mp2` |
| twolame_64_mono_32000.mp2 | Layer II, low-rate allocation table (32 kHz) | `twolame --quiet -b 64 -m m src/music_32000_st.wav twolame_64_mono_32000.mp2` |
