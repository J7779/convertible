# AAC fixtures

Sources and reference decoding: see `../SOURCES.md` (references: ffmpeg's
native AAC decoder, which applies MPEG-4 edit lists but not iTunSMPB).
The `af_*` files were written on macOS by Core Audio's `afconvert`.

| File | Content | Command |
| --- | --- | --- |
| af_aaclc_128_st_44100.m4a | AAC-LC 128 kbit/s, iTunSMPB gapless data | `afconvert -f m4af -d aac -b 128000 src/music_44100_st.wav af_aaclc_128_st_44100.m4a` |
| af_heaac_48_st_44100.m4a | HE-AAC (SBR), 48 kbit/s | `afconvert -f m4af -d aach -b 48000 src/music_44100_st.wav af_heaac_48_st_44100.m4a` |
| af_heaacv2_32_st_44100.m4a | HE-AAC v2 (SBR + PS), 32 kbit/s | `afconvert -f m4af -d aacp -b 32000 src/music_44100_st.wav af_heaacv2_32_st_44100.m4a` |
| af_heaac_24_mono_22050.m4a | HE-AAC mono | `afconvert -f m4af -d aach -b 24000 src/music_22050_mono.wav af_heaac_24_mono_22050.m4a` |
| af_aaclc_64_mono_22050.aac | AAC-LC in ADTS | `afconvert -f adts -d aac -b 64000 src/music_22050_mono.wav af_aaclc_64_mono_22050.aac` |
| af_aaclc_112_st_48000.caf | AAC-LC in CAF (read by the lossless family's CAF reader through the aac codec) | `afconvert -f caff -d aac -b 112000 src/music_48000_st.wav af_aaclc_112_st_48000.caf` |
| fdk_lc_vbr4_st_44100.m4a | Fraunhofer FDK AAC-LC, VBR mode 4 | `fdkaac --silent -p 2 -m 4 -o fdk_lc_vbr4_st_44100.m4a src/music_44100_st.wav` |
| fdk_lc_64_mono_16000.aac | FDK AAC-LC in ADTS | `fdkaac --silent -p 2 -b 64000 -f 2 -o fdk_lc_64_mono_16000.aac src/music_16000_mono.wav` |
| ff_aac_128_st_44100.m4a | ffmpeg's AAC encoder (uses PNS) | `ffmpeg -i src/music_44100_st.wav -c:a aac -b:a 128k -fflags +bitexact -map_metadata -1 -movflags +faststart ff_aac_128_st_44100.m4a` |
| ff_aac_48_mono_22050.aac | ffmpeg AAC in ADTS | `ffmpeg -i src/music_22050_mono.wav -c:a aac -b:a 48k -fflags +bitexact -map_metadata -1 -f adts ff_aac_48_mono_22050.aac` |

fdkaac in Ubuntu 24.04 is built without SBR ("unsupported profile" for -p
5/29), hence the HE-AAC files come from Apple's encoder only.
