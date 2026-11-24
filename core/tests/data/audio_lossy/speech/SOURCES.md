# Speech codec fixtures

Sources: see `../SOURCES.md`. AMR references are ffmpeg's own (native,
floating-point) AMR decoders; the GSM reference is SoX/libgsm
(`sox -t gsm sox_speech_8000.gsm -b 16 x.wav`).

The AMR encoders are the example programs of the official release
tarballs, built in the container from `opencore-amr-0.1.6.tar.gz`
(`./configure --enable-examples --enable-amrnb-encoder --disable-shared && make`)
and `vo-amrwbenc-0.1.3` (`./configure --enable-example --disable-shared && make`).

| File | Content | Command |
| --- | --- | --- |
| oc_amrnb_122_8000.amr | AMR-NB 12.2 kbit/s | `test/amrnb-enc -r 12200 src/speech_8000_mono.wav oc_amrnb_122_8000.amr` |
| oc_amrnb_475_8000.amr | AMR-NB 4.75 kbit/s | `test/amrnb-enc -r 4750 src/speech_8000_mono.wav oc_amrnb_475_8000.amr` |
| oc_amrnb_dtx_795_8000.amr | AMR-NB 7.95 kbit/s with DTX (SID frames) | `test/amrnb-enc -d -r 7950 src/speech_8000_mono.wav oc_amrnb_dtx_795_8000.amr` |
| vo_amrwb_2385_16000.awb | AMR-WB 23.85 kbit/s | `amrwb-enc -r 23850 src/speech_16000_mono.wav vo_amrwb_2385_16000.awb` |
| vo_amrwb_1265_16000.awb | AMR-WB 12.65 kbit/s | `amrwb-enc -r 12650 src/speech_16000_mono.wav vo_amrwb_1265_16000.awb` |
| sox_speech_8000.gsm | GSM 06.10 full rate, 33-byte frames | `sox src/speech_8000_mono.wav -t gsm sox_speech_8000.gsm` |
