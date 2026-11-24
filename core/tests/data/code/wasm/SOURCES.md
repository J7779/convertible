# Sources: code/wasm

Hand-written WebAssembly text modules (made up for these tests) assembled and disassembled with wabt
1.0.34 in a throwaway Ubuntu 24.04 container.

| File | Contents | Command |
| --- | --- | --- |
| `basics.wat` | MVP module: imports (function, memory), globals, a table with an element segment, a data segment, blocks/loops/if, `br_table`, `call_indirect`, loads/stores with offsets, floats, `select`, exports, a start function | hand-written |
| `basics.wasm` | the binary | `wat2wasm basics.wat -o basics.wasm` |
| `basics.wasm2wat.wat` | wabt's disassembly, the expected output of our `wasm -> wat` | `wasm2wat basics.wasm -o basics.wasm2wat.wat` |
| `features.wat` | post-MVP features: multi-value results and block parameters, sign extension, saturating truncation, bulk memory (`memory.init/copy/fill`, `data.drop`, `table.init`, `elem.drop`), reference types (`externref` tables, `ref.null/is_null/func`, `table.set/size`), typed `select` | hand-written |
| `features.wasm` | the binary | `wat2wasm features.wat -o features.wasm` |
| `features.wasm2wat.wat` | wabt's disassembly | `wasm2wat features.wasm -o features.wasm2wat.wat` |

Interop of our output: our disassembly of both binaries is byte-identical to wasm2wat's, and
`wat2wasm` turns our text back into binaries identical to the originals.
