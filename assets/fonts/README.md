# Bundled fonts

Unmodified font files from the official upstream releases. Both families are licensed under the
SIL Open Font License, Version 1.1. The licence texts are included here and must ship with the fonts.

| File | Family | Weight | Upstream release |
| --- | --- | --- | --- |
| `Inter-Regular.ttf` | Inter | 400 | Inter 4.1, https://github.com/rsms/inter/releases/tag/v4.1 (`extras/ttf/`) |
| `Inter-Medium.ttf` | Inter | 500 | same |
| `Inter-SemiBold.ttf` | Inter | 600 | same |
| `JetBrainsMonoNL-Regular.ttf` | JetBrains Mono NL | 400 | JetBrains Mono 2.304, https://github.com/JetBrains/JetBrainsMono/releases/tag/v2.304 (`fonts/ttf/`) |
| `JetBrainsMonoNL-Medium.ttf` | JetBrains Mono NL | 500 | same |

Licences: `Inter-LICENSE.txt`, `JetBrainsMono-LICENSE.txt`.

SHA-256

```
40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82  Inter-Regular.ttf
97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872  Inter-Medium.ttf
78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3  Inter-SemiBold.ttf
fb3b2575d7b0657359707993288f12a7360344d39387bb26050e276d61f6bd2a  JetBrainsMonoNL-Regular.ttf
44099e1efefba55637e0abbbf8dd3f526e59523345888a257bb01d39df4af74c  JetBrainsMonoNL-Medium.ttf
```

Notes for app developers

- Inter is the UI face. The desktop app must load these files itself. iOS, Android and the web app may bundle them or use the system font.
- JetBrains Mono NL is the build without coding ligatures. Use it for format labels, file names and code previews.
- Each static file reports its own family name in the legacy name table ("Inter Medium", "Inter SemiBold"), so load by file and weight rather than by family lookup where the platform allows.
- For numbers that change in place (sizes, percentages, durations) enable Inter's tabular figures (`tnum`).
