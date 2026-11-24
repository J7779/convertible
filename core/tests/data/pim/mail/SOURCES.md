# Sources: pim/mail

Written by `gen_mail.py` with the Python 3.14 standard library (`email`,
`mailbox`, `plistlib`): `python3 -I gen_mail.py mail`. All addresses use
example.org; all names and texts are made up. MIME boundaries are random
per run, so a regenerated file differs in those lines only.

| File | API | Contents |
| --- | --- | --- |
| `multipart.eml` | `EmailMessage(policy=email.policy.SMTP)` | multipart/mixed: multipart/alternative (quoted-printable UTF-8 text with emoji, a 137-character line and a signature; HTML), plus an attachment `données été.bin` (RFC 2231 name, base64, 1024 bytes = byte values 0..255 four times); subject `Meeting notes 📝 — שלום` as encoded words; `To` with an encoded display name |
| `html_latin1.eml` | `MIMEText(..., "html", "iso-8859-1")`, `Header(..., "iso-8859-1")` | HTML-only body in ISO-8859-1 with named entities; Q-encoded subject `Prix spécial` |
| `forward.eml` | `MIMEMultipart`, `MIMEMessage`, `MIMEApplication` | text part, a forwarded `message/rfc822`, and `report.pdf` (`%PDF-1.4 synthetic\n`) |
| `mailbox.mbox` | `mailbox.mbox`, `mboxMessage.set_from` | three messages; the second has a non-ASCII subject and a body line `From the start of a line` stored as `>From ...` |
| `message.emlx` | byte count + `multipart.eml` bytes + `plistlib.dumps` | Apple Mail layout |

The expected contents in the tests are the values passed to these APIs.
