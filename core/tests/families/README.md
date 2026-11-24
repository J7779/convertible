# Family tests

One directory per format family, mirroring `core/src/families/`:

```
tests/families/<family>/<format>_test.cpp     tests of one format (or a few closely related ones)
tests/data/<family>/<name>                    fixtures used by those tests
tests/data/<family>/SOURCES.md                how every fixture was produced
```

Rules:

- Stay inside your family's two directories. Shared helpers live in
  `tests/support/`; if one is missing, ask for it rather than copying code
  between families.
- Go through the registered codecs (`test::read_as`, `test::write_from`,
  `test::convert`) so that tests exercise what users get. Pure functions of
  your family may additionally be unit-tested through the family's private
  headers (`#include "families/<family>/<header>.hpp"`).
- Name test cases `"<family>/<format>: ..."`.
- Every format needs the five kinds of tests listed in `tests/README.md`:
  real-file decoding, round trips with quality thresholds, hostile input,
  options, integration.
- Fixtures follow `tests/data/README.md`: tiny, synthetic, made by
  independent software, free of personal or machine data, documented.

The `encoding` family is the worked example: see
`tests/families/encoding/` and `src/families/encoding/`.
