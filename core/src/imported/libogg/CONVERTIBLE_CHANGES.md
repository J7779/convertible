# Changes to libogg 1.3.5

Only the bit packer (`src/bitwise.c`) and the public headers are imported:
libtheora's encoder writes its header packets with `oggpack_*`. Ogg paging
(`framing.c`) is not used - containers are handled elsewhere in the core.

- `include/ogg/config_types.h` is added. Upstream generates it from
  `config_types.h.in` at configure time; this version uses `<stdint.h>` types,
  which is what the generated file contains on every supported platform.

No upstream file is modified.
