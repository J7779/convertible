# Sources: pim/feeds

Written by `gen_pim_b.py` (feedgen 1.0.0, Python 3.14): `python -I gen_pim_b.py out`.
One made-up feed, "Example News" (https://news.example.org/, language en,
author Ada Example / ada@example.org), with two items:

1. "First story — ünïcödé", https://news.example.org/1, published 2024-05-01T12:30:00+02:00, summary `Summary with <b>HTML</b>`, category fixtures;
2. "שלום from the newsroom 📰", https://news.example.org/2, published 2024-05-02T08:00:00Z, enclosure 2.mp3 (audio/mpeg, 1234 bytes).

| File | Tool | Notes |
| --- | --- | --- |
| `feedgen.rss` | feedgen `FeedGenerator.rss_str(pretty=True)` | RSS 2.0 with RFC 822 dates |
| `feedgen.atom` | feedgen `FeedGenerator.atom_str(pretty=True)` | Atom 1.0 |
| `rss1.rdf` | hand-written | RSS 1.0 (RDF) with Dublin Core date/creator/language |
| `feed.json` | `json.dumps` | JSON Feed 1.1 with tags and an attachment |

Interoperability (development): RSS and Atom written by Convertible
(`convertible convert feedgen.rss -t rss|atom`) parse in feedparser 6.0.14
without errors (`bozo` false) with identical titles, links and dates.
feedparser 6 does not read JSON Feed from a file; the JSON Feed output was
checked against the JSON Feed 1.1 required fields with Python's `json`.
