# Sources: pim/contacts

Contact fixtures for `core/tests/families/pim/vcard_test.cpp`,
`contacts_formats_test.cpp` and `pim_bridges_test.cpp`. All people,
organisations, addresses, phone numbers (555 / reserved ranges) and e-mail
addresses (example.org) are invented.

Every file was produced by one Python script, `make_contacts.py`, run in this
directory with Python 3.14 in a scratch virtual environment holding
`vobject` 0.9.9 (`python -m pip install vobject==0.9.9`); everything else uses
the standard library (`base64`, `csv`, `json`, `quopri`, `struct`, `zlib`,
`xml.etree.ElementTree`):

```sh
python3 make_contacts.py
```

The script is not committed; what each part does is described below.

| File | Contents | Tool |
| --- | --- | --- |
| `dot_2x2.png` | 74-byte 2x2 RGB PNG (red, black / black, white), used as the contact photo everywhere | `struct` + `zlib` (PNG chunks written by hand) |
| `cards_v30_vobject.vcf` | vCard 3.0, three cards: Dr. Ada Marie Example PhD (N, NICKNAME, ORG `Example Org;Research`, TITLE, two typed EMAILs, CELL and WORK TEL, WORK ADR with extended part, URL, BDAY, CATEGORIES, folded NOTE with emoji, `PHOTO;ENCODING=b;TYPE=PNG`, UID, grouped `ITEM1.EMAIL` + `ITEM1.X-ABLABEL:_$!<Other>!$_`, `X-CUSTOM-FIELD`); an Arabic-named card with a Hebrew note; Bob Sample with a two-line HOME ADR in Zürich | `vobject.vCard().serialize()` |
| `cards_v21.vcf` | vCard 2.1, two cards: Jürgen Müller with `CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE` N/FN/ADR, bare types (`TEL;CELL;PREF`), a QP NOTE with a hard line break and a soft line break, `PHOTO;ENCODING=BASE64;TYPE=PNG` on indented lines ended by a blank line, `BDAY:19850315`; Jane Doe with `TEL;WORK;FAX` and `ORG:Example Org;Support` | lines assembled by the script, `quopri` and `base64` |
| `cards_v40.vcf` | vCard 4.0, one card: Chloé Exemple with KIND, GENDER, `EMAIL;PREF=1`, `TEL;VALUE=uri;TYPE="voice,cell":tel:...`, ADR with a LABEL parameter, `BDAY:--0612`, `ANNIVERSARY:20150620`, `PHOTO:data:image/png;base64,...`, LANG, IMPP, escaped NOTE with emoji, urn:uuid UID; folded at 75 octets on UTF-8 boundaries | lines assembled and folded by the script |
| `cards.jcard.json` | jCard (RFC 7095): structured `n`, `org`, `adr`, typed `email`, `tel` uri with a type array, `bday` date, multi-valued `categories`, unicode note, grouped `email` + `x-ablabel` (`"group": "item1"`) | `json.dump(..., ensure_ascii=False, indent=1)` |
| `cards.xcard.xml` | xCard (RFC 6351), two cards; parameters, `<uri>` tel, structured `<n>`/`<adr>`, `<date>19900427</date>`, a note with `&` and `<markup>`, a `<group name="item1">` with email and x-ablabel | `xml.etree.ElementTree` (`write(..., xml_declaration=True)`) |
| `contacts.ldif` | LDIF in Thunderbird's attribute vocabulary: Ada Example (givenName, sn, cn, mozillaNickname, mail, mozillaSecondEmail, telephoneNumber, homePhone, mobile, work and home addresses, o, ou, title, mozillaWorkUrl, birthyear/month/day, base64 multi-line description); Jürgen Müller with base64 `dn::`/`cn::` values and a base64 `jpegPhoto` (the PNG above), lines folded at 76 columns | lines assembled by the script, `base64` |
| `google_contacts.csv` | Google Contacts export layout (`First Name`, ..., `Labels`, `E-mail 1 - Label/Value`, `Phone n - ...`, `Address 1 - ...`, `Website 1 - ...`); multi-values joined with ` ::: `, `* myContacts` system label | `csv.writer` |
| `outlook_contacts.csv` | Outlook contacts export layout (62 columns), Ada Example with business/home addresses, phones, fax, three e-mail slots, `Birthday` `4/27/1990`, empty `Anniversary` `0/0/00`, quoted multi-line note, `Categories` `Friends;Work`; Jürgen Müller | `csv.writer` |

## Interoperability of our output

Checked during development with the `convertible` CLI and `vobject` 0.9.9
(comparison script in a scratch directory, not committed; it compares FN, N,
ORG, TITLE, NICKNAME, BDAY, NOTE, UID, CATEGORIES, EMAIL, TEL, URL, ADR and
the photo size card by card):

- `convertible convert cards_v30_vobject.vcf -t vcf` (vCard 3.0): identical.
- `--opt 'vcard_version="4.0"'`: identical except two representational
  differences vobject does not understand in 4.0: `BDAY:19900427` (RFC 6350
  basic form) and the `data:` photo URI.
- `--opt vcard_version=2.1`: identical except that QUOTED-PRINTABLE line
  breaks (`=0D=0A`) read back as CR LF instead of LF. Soft line breaks in
  quoted-printable values are not written, because vobject rejects them.
