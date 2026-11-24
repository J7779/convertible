# Integration tests

Conversions of real files across families, run the way an application runs
them: through the public C ABI (`capi_convert` in `integration_support.hpp`)
or through `test::convert`, which plans and executes exactly like
`cvt_convert`. Unit tests of one format live with their family under
`tests/families/<family>/`; this directory checks that the pieces fit.

| File | What it checks |
| --- | --- |
| `offered_targets_test.cpp` | Every target the planner offers for a representative set of real files (txt, md, html, docx, pdf, png, jpg, gif, svg, wav, mp3, flac, mp4, mkv, csv, xlsx, json, yaml, zip, tar.gz, gz, obj, stl, srt, ics, vcf, ttf, py, dxf, pptx, epub) converts with the option schema's defaults. The promise of the product: a target that is offered must work. |
| `target_lists_test.cpp` | The leading suggestions for those sources (what a user most likely wants first), the absence of nonsense routes, `role` and `compress` flags, and real conversions with content checks for every pinned (source, target) pair. |
| `pack_test.cpp` | `cvt_pack`: several named files into zip, 7z, tar and tar.gz, with sanitised and de-duplicated names, cancellation and errors. |
| `image_audio_integration_test.cpp` | Image and audio formats through each other with PSNR/SNR thresholds. |
| `video_integration_test.cpp`, `legacy_integration_test.cpp` | Container and codec combinations, remuxing and re-encoding with PSNR against reference frames. |

Fixtures come from `tests/data/<family>/` and `tests/data/detect/` (written by
independent software; see the `SOURCES.md` next to them). Tests that need a
codec missing from a build check the registry and `SKIP` with the reason.

When a new format makes `offered_targets_test` fail, fix the route rather than
the test: give writers that need a schema an IR dialect (`Codec::ir_tag`),
restrict writers that need content of their own kind to direct reads
(`Codec::direct_only`) or to their own sources (`Codec::only_from`), mark
bridges that extract existing structure `Bridge::after_reader_only`, and mark
formats that are only ever produced `FormatInfo::output_only` (see
`core/README.md`, "Planner rules"). A conversion that depends on content
(a document without tables cannot become a spreadsheet) may be listed in the
test's documented exceptions, with the reason.
