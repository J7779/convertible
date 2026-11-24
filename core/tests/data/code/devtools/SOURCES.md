# Sources: code/devtools

| File | Contents | Tool | Command |
| --- | --- | --- | --- |
| `sample.diff` | unified diff of two 8-line files of Greek letter names (one line changed, one appended) | Apple diff (FreeBSD diff) on macOS | `diff -u --label a/greek.txt --label b/greek.txt old.txt new.txt > sample.diff` |
| `sample.patch` | `git diff` of a commit adding `added.txt`, changing `docs/numbers.txt` and `greek.txt` | git 2.43.0 in a throwaway Ubuntu 24.04 container (author set to `test`, no timestamps in the output) | `git diff --cached > sample.patch` after staging the changes |

The input files: `old.txt` = alpha … theta (one per line), `new.txt` = the same with `BETA` and an extra
`iota`; `docs/numbers.txt` went from `one two three` to `one 2 three four`; `added.txt` holds `fresh`.
