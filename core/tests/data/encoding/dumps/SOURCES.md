# Sources: encoding/dumps

Dumps for `core/tests/families/encoding/byte_views_test.cpp`, written in a throwaway Ubuntu 24.04
container by util-linux 2.39.3 `hexdump` and GNU coreutils 9.4 `od`.

| File | Contents | Command |
| --- | --- | --- |
| `repeats.bin` | 92 bytes: `Header` LF, 80 zero bytes, `tail!` (exercises the `*` repeat lines) | `printf 'Header\n' > repeats.bin; head -c 80 /dev/zero >> repeats.bin; printf 'tail!' >> repeats.bin` |
| `sample.hexdump-c` | `hexdump -C` of `../bases/sample.bin` | `hexdump -C sample.bin > sample.hexdump-c` |
| `repeats.hexdump-c` | `hexdump -C` of `repeats.bin` | `hexdump -C repeats.bin > repeats.hexdump-c` |
| `sample.od` | `od` (octal words) of `sample.bin` | `od sample.bin > sample.od` |
| `repeats.od` | `od` of `repeats.bin` | `od repeats.bin > repeats.od` |
| `sample.od-x1` | `od -t x1` of `sample.bin` | `od -t x1 sample.bin > sample.od-x1` |
| `repeats.od-x2` | `od -t x2` of `repeats.bin` | `od -t x2 repeats.bin > repeats.od-x2` |

Our `hexdump-c` and `od` writers reproduce these files byte for byte, and read them back.

Array-literal and listing writers were checked by compiling and running their output for all 256 byte
values (each program prints the length and a weighted sum; all agree): on macOS with Apple clang
(C header, C++ array, the `.inc` list inside a C array), Python 3.14, Node 26, Ruby 4.0, Swift 6; in an
Ubuntu 24.04 container with Go 1.22, rustc 1.75, PHP 8.3, TypeScript 4.8 (`tsc --strict`), javac 21, Mono
mcs 6.8 (C#); GNU as 2.42 and NASM 2.16 assemble the `.byte`/`db` listings back into the same 256 bytes.
Kotlin and Zig toolchains were not available offline; those two writers are covered by exact-output
tests only.
