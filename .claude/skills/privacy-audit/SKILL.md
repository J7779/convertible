---
name: privacy-audit
description: Check the Convertible working tree and full git history for personal data, machine details, secrets and authorship problems (home paths, .local host names, private keys, tokens, signing files, e-mail addresses, non-noreply commit identities, co-author or "generated with" trailers) with tools/privacy-audit. Use before every push, before publishing anything, and after adding fixtures or imported code.
---

# Privacy audit

The repository must contain no personal or machine data, in files or in history.
`tools/privacy-audit` checks it; CI runs it with `--history` on every push.

## Steps

1. Run it on the tree and the history:
   ```sh
   tools/privacy-audit --history
   ```
2. Add words that must never appear but must not be written into the repository either (your
   real name, user name, host name, a private project name) at run time only:
   ```sh
   tools/privacy-audit --history --local-identity            # this machine's user and host name
   tools/privacy-audit --history --pattern 'word-one' --pattern 'word two'
   CVT_PRIVACY_PATTERNS="$(cat ~/.config/privacy-words)" tools/privacy-audit --history
   ```
   Never commit such a pattern list. (`LICENSE` is exempt from extra patterns: it names the
   copyright holder.)
3. Fix every finding:
   - **working-tree findings**: remove the data. A line that documents an example on purpose may
     carry the marker `privacy-audit: allow`; use it sparingly and never for real data.
   - **signing**: never commit keystores, `.p12`, provisioning profiles, `.pem`/`.key` or
     `local.properties`; they are also in `.gitignore`.
   - **e-mail**: addresses are allowed only in `core/src/imported/**`, licence files and the
     generated `NOTICES.md`; use `example.com` in examples.
   - **identity**: commits must use the GitHub noreply address
     (`git config user.email <id>+<login>@users.noreply.github.com`).
   - **trailer**: commit messages carry no co-author, "generated with/by" or tool-attribution
     lines.
4. Findings in **history** (unpushed commits) are fixed by amending or rebasing the local
   commits. Never rewrite pushed history without the owner's explicit agreement.
5. Re-run until it prints `privacy-audit: clean`.
