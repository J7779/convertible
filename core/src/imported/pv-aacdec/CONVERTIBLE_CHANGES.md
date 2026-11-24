# Changes to the PacketVideo AAC decoder (Android 4.0.4)

- Only the decoder library sources listed in the original `Android.mk` and
  their headers are kept; the Android/OpenMAX wrapper (`SoftAAC.*`) and the
  build file are not.
- `LICENSE` is the Apache License 2.0 text named in every file header (the
  original directory relies on the repository-wide notice).
- Built with `AAC_PLUS HQ_SBR` but without `PARAMETRICSTEREO`: the parametric
  stereo tool crashes on 64-bit targets, so HE-AAC v2 streams play as their
  mono HE-AAC core.
- The OSCL platform macros (`OSCL_IMPORT_REF`, `OSCL_EXPORT_REF`,
  `OSCL_UNUSED_ARG`) and a `cvt_pv_` prefix for every global symbol come from
  a force-included header (`core/src/families/audio_lossy/aac/pv_aacdec_rename.h`).
  The source files themselves are unchanged, except:
- `get_tns.cpp`: the TNS filter's upper band (`top`) is clamped at zero after
  the start-band length is subtracted. A damaged stream made it negative, the
  unsigned `tempInt` then held a huge value and `SCALE_FACTOR_BAND_OFFSET`
  read far outside the band table (a crash with GCC builds on Linux).
