# Sources: support

Fixtures for the tests of the fixture loader itself
(`core/tests/support/fixtures_test.cpp`). They are not format samples.

All three were written with the `printf` built-in of zsh 5.9; any POSIX
`printf` produces the same bytes. Commands are run in this directory.

| File | Contents | Tool | Command |
| --- | --- | --- | --- |
| `hello.txt` | 16 bytes: the ASCII text `Hello, fixture!` and one LF | `printf` (zsh 5.9 built-in) | `printf 'Hello, fixture!\n' > hello.txt` |
| `bytes.bin` | 8 bytes: `00 01 02 7F 80 FE FF 0A` (NUL, control, high and non-UTF-8 values) | `printf` (zsh 5.9 built-in) | `printf '\000\001\002\177\200\376\377\012' > bytes.bin` |
| `nested/crlf.txt` | 20 bytes: `line one` and `line two`, each ended by CR LF | `printf` (zsh 5.9 built-in) | `printf 'line one\r\nline two\r\n' > nested/crlf.txt` |

`.gitattributes` (`* -text`) keeps git from translating line endings here;
the loader tests compare these files byte for byte.
