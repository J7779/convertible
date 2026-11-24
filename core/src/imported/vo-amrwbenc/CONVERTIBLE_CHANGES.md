# Changes to vo-amrwbenc 0.1.3

- Only the encoder sources listed in `Makefile.am` and the headers/tables
  they include are kept (no examples, build system or assembly).
- `amrwbenc/src/c4t64fx.c` (`ACELP_4t64_fx`): the search for a free slot in
  `ind[]` is bounded by the array size (`i < NPMAXPT * NB_TRACK`); without
  the bound it could write past the stack array (AddressSanitizer finding on
  a frequency sweep). The same fix is in Android's copy of this encoder.
- Every global symbol gets a `cvt_voamr_` prefix through a force-included
  header (`core/src/families/audio_lossy/speech/vo_amrwbenc_rename.h`).
