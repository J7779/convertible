# Sources: encoding/firmware

Firmware images of `../bases/sample.bin` (300 bytes, byte i = (i * 7 + 3) % 256) for
`core/tests/families/encoding/firmware_hex_test.cpp`, written in a throwaway Ubuntu 24.04 container
with SRecord 1.64 (`srec_cat`) and GNU objcopy 2.42, run in a directory holding `sample.bin`:

| File | Contents | Command |
| --- | --- | --- |
| `sample_1000.ihex` | Intel HEX at 0x1000, 32-byte records | `srec_cat sample.bin -binary -offset 0x1000 -o sample_1000.ihex -intel -disable=exec_start_address` |
| `sample_ffc0.ihex` | Intel HEX at 0xFFC0, crossing 64 KiB (two type 04 records) | `srec_cat sample.bin -binary -offset 0xFFC0 -o sample_ffc0.ihex -intel -disable=exec_start_address` |
| `sample_objcopy.ihex` | Intel HEX at 0, 16-byte records, CRLF | `objcopy -I binary -O ihex sample.bin sample_objcopy.ihex` |
| `sample_1000.srec` | S19 at 0x1000 (S0 header holds srec_cat's project URL, S5 count, no S9) | `srec_cat sample.bin -binary -offset 0x1000 -o sample_1000.srec -motorola` |
| `sample_123400.s28` | S28 at 0x123400 (24-bit addresses) | `srec_cat sample.bin -binary -offset 0x123400 -o sample_123400.s28 -motorola -address-length=3` |
| `sample_objcopy.srec` | S-records at 0 | `objcopy -I binary -O srec sample.bin sample_objcopy.srec` |
| `sample_1000.ti-txt` | TI-TXT at 0x1000 | `srec_cat sample.bin -binary -offset 0x1000 -o sample_1000.ti-txt -ti-txt` |
| `sample.vmem` | Verilog VMEM, 8-bit words, with a comment and @address on every line | `srec_cat sample.bin -binary -o sample.vmem -vmem 8` |

The tests check that every file decodes to `sample.bin`, and that our writers produce the same data
records as these tools for the same address and record size (Intel HEX byte for byte against objcopy
with `line_ending=crlf`).
