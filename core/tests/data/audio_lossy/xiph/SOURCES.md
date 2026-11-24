# Opus, Vorbis and Speex fixtures

Sources: see `../SOURCES.md`. References come from the projects' own
decoders, converted to FLAC by ffmpeg and cut to 0.2 s:
`opusdec --quiet --rate 48000 --no-dither <file> x.wav`,
`oggdec --quiet -b 16 -o x.wav <file>`, `speexdec <file> x.wav`.

| File | Content | Command |
| --- | --- | --- |
| opus_celt_96_st_48000.opus | CELT, 96 kbit/s, stereo | `opusenc --quiet --bitrate 96 src/music_48000_st.wav opus_celt_96_st_48000.opus` |
| opus_silk_10_mono_16000.opus | SILK, 10 kbit/s, 20 ms | `opusenc --quiet --bitrate 10 --framesize 20 src/speech_16000_mono.wav opus_silk_10_mono_16000.opus` |
| opus_hybrid_24_mono_48000.opus | hybrid SILK+CELT, 24 kbit/s | `opusenc --quiet --bitrate 24 --framesize 20 src/speech_48000_mono.wav opus_hybrid_24_mono_48000.opus` |
| opus_celt_2p5ms_st_48000.opus | 2.5 ms frames | `opusenc --quiet --bitrate 64 --framesize 2.5 src/music_48000_st.wav opus_celt_2p5ms_st_48000.opus` |
| opus_60ms_st_44100.opus | 60 ms frames, 44.1 kHz input | `opusenc --quiet --bitrate 32 --framesize 60 src/music_44100_st.wav opus_60ms_st_44100.opus` |
| oggenc_q5_st_44100.ogg | Vorbis q5 stereo (floor 1) | `oggenc --quiet -q 5 -o oggenc_q5_st_44100.ogg src/music_44100_st.wav` |
| oggenc_q0_mono_22050.ogg | Vorbis q0 mono, a single audio page | `oggenc --quiet -q 0 -o oggenc_q0_mono_22050.ogg src/music_22050_mono.wav` |
| oggenc_tags_mono_8000.ogg | title "Vorbis Tone", artist "Convertible Fixtures" | `oggenc --quiet -q 3 -t "Vorbis Tone" -a "Convertible Fixtures" -o oggenc_tags_mono_8000.ogg src/music_8000_mono.wav` |
| ff_vorbis_q4_st_44100.oga | ffmpeg + libvorbis | `ffmpeg -i src/music_44100_st.wav -c:a libvorbis -q:a 4 -fflags +bitexact -map_metadata -1 ff_vorbis_q4_st_44100.oga` |
| speexenc_nb_q8_8000.spx | Speex narrowband q8 | `speexenc --quiet --quality 8 src/speech_8000_mono.wav speexenc_nb_q8_8000.spx` |
| speexenc_wb_q6_16000.spx | Speex wideband q6 | `speexenc --quiet --wideband --quality 6 src/speech_16000_mono.wav speexenc_wb_q6_16000.spx` |

No current encoder writes Vorbis floor 0, so floor 0 is not covered by a fixture.
