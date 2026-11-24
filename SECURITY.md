# Security policy

Convertible parses untrusted files on people's own devices, so memory-safety bugs and privacy
leaks are treated as security issues.

## What to report

- crashes, hangs, out-of-bounds reads or writes, or unbounded memory use caused by a crafted
  input file (in the core or in any app);
- any network access, data collection or file access beyond the files a user chose;
- identifying metadata that survives conversion while `strip_metadata` is on;
- personal data, credentials or signing material committed to this repository.

## How to report

Use GitHub's private vulnerability reporting: on the repository page open **Security ->
Report a vulnerability**. Please include the input file (or how to produce it), the command or
app steps, the platform and the version (`convertible --version` or the app's About screen).
Do not open a public issue for an unfixed vulnerability.

You should get a first response within a week. Fixes are released as soon as they are
ready, and reporters are credited in the release notes unless they prefer otherwise.

## Supported versions

Only the latest release and the `main` branch receive security fixes.
