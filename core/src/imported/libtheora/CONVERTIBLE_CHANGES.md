# Changes to libtheora 1.1.1

Only the sources of the `th_*` decoder and encoder (`lib/`, without
`apiwrapper.c`, `decapiwrapper.c`, `encapiwrapper.c`, `encoder_disabled.c`,
`cpu.c` and the `x86`/`x86_vc` assembly) and the public headers in
`include/theora` are imported (`theora.h`, the legacy API, only for its declarations used by `lib/internal.h`). Four upstream problems found by the sanitizers are
fixed:

- `lib/decode.c`, `oc_dec_init()`: the post-processing sharpness sum indexed
  `dequant_tables[qti][pli][qi]` instead of `dequant_tables[qi][pli][qti]`,
  reading outside the inner arrays (fixed the same way in libtheora 1.2).
- `lib/state.c`, `oc_state_frag_copy_list_c()`: returns early for an empty
  fragment list, so that no pointer is formed from the reference index -1
  before the first frame exists.
- `lib/huffdec.c`: `_ogg_offsetof` is the standard `offsetof`; upstream
  computed it by member access through a null pointer.
- `lib/mathops.c`, `oc_ilog64()`: the de Bruijn multiplication is done in
  unsigned 64-bit arithmetic (it relied on signed overflow wrapping).

The family build turns off UBSan's `shift` check for this library only:
the code relies on arithmetic left shifts of negative values.
