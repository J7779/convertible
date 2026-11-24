# Changes to libogg 1.3.5

Only `src/bitwise.c`, `src/framing.c` (with `src/crctable.h`) and the public
headers are imported. Two families use this copy:

- `codec_legacy` builds the bit packer (`bitwise.c`) for libtheora, whose
  encoder writes its header packets with `oggpack_*`.
- `audio_lossy` builds the bit packer and the framing layer together with
  libvorbis (see `core/src/families/audio_lossy/cmake/xiph.cmake`). In that
  build every global symbol is renamed with a `cvt_` prefix at compile time
  (a generated list of `-D` definitions in the family's CMake fragment), so
  the two builds can never clash in one binary.

Other changes:

- `include/ogg/config_types.h` is added. Upstream generates it from
  `config_types.h.in` at configure time; this version uses the fixed-width
  types from `<stdint.h>`, which is what the generated file contains on every
  supported platform.

No upstream source file is modified.
