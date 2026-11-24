# Sources: pim/calendar

Calendar fixtures for `core/tests/families/pim/calendar_test.cpp`,
`calendar_csv_test.cpp` and `pim_bridges_test.cpp`. All events, names and
addresses are invented (example.org); there is no personal or machine data.

Every file was produced by one Python script, `make_calendar.py`, run in this
directory with Python 3.14 in a scratch virtual environment holding
`icalendar` 7.3.2 and `vobject` 0.9.9 (`python -m pip install icalendar==7.3.2 vobject==0.9.9`):

```sh
python3 make_calendar.py
```

The script is not committed; what each part does is described below so the
files can be regenerated with the same libraries.

| File | Contents | Tool |
| --- | --- | --- |
| `events_icalendar.ics` | `X-WR-CALNAME:Team calendar`; a VTIMEZONE `Europe/Berlin` (STANDARD + DAYLIGHT); VEVENT `planning-1` 2024-05-06 10:00-11:30 Europe/Berlin with folded escaped DESCRIPTION, LOCATION `Room 4, Example Tower; floor 2`, ORGANIZER/ATTENDEE with CN, CATEGORIES `Work,Planning`, URL, VALARM `-PT15M`; all-day `birthday-2` 2024-06-10 with `RRULE:FREQ=YEARLY`; UTC `rtl-3` with an Arabic summary and Hebrew location; `standup-4` weekly MO,WE COUNT=6 with EXDATE 2024-05-08 09:00; `review-5` last Friday monthly until 2024-09-30 with `DURATION:PT1H`; VTODO `todo-6` (DUE, PRIORITY); VJOURNAL `journal-7` | `icalendar.Calendar.to_ical()` |
| `freebusy.ifb` | One VFREEBUSY `fb-1` with FREEBUSY `20240506T080000Z/20240506T100000Z` | `icalendar` |
| `events_vobject.ics` | `vobject-1` UTC 2024-07-01 13:00-17:00 with Japanese/Russian summary and a DESCRIPTION folded over three lines with `\,`, `\;`, `\\`; all-day `vobject-2` 2024-08-01..03 | `vobject.iCalendar().serialize()` |
| `legacy.vcs` | vCalendar 1.0: VEVENT with QUOTED-PRINTABLE UTF-8 SUMMARY `Café meeting`, QP two-line DESCRIPTION, `CATEGORIES:BUSINESS;MEETING`, `RRULE:W1 MO TH #4`, `DALARM` 15 minutes before; VTODO with DUE | lines assembled by the script, values encoded with Python's stdlib `quopri` |
| `google_calendar.csv` | Google Calendar import layout (`Subject,Start Date,Start Time,End Date,End Time,All Day Event,Description,Location,Private`), US dates and 12-hour times; a timed event, an all-day private event, an event crossing midnight with a two-line description | Python stdlib `csv.writer` |
| `outlook_calendar.csv` | Outlook calendar export layout (22 columns incl. `Reminder on/off`, `Meeting Organizer`, `Categories`, `Show time as`), every field quoted; a timed event with a reminder 15 minutes before and an all-day private event with exclusive end date | Python stdlib `csv.writer(quoting=csv.QUOTE_ALL)` |

## Interoperability of our output

Checked during development by converting with the `convertible` CLI and
re-reading with the independent libraries (comparison script in a scratch
directory, not committed):

- `convertible convert events_icalendar.ics -t ics`, then both files parsed
  with `icalendar` and compared component by component (UID, SUMMARY,
  DESCRIPTION, LOCATION, DTSTART, DTEND, DUE, RRULE, EXDATE, ORGANIZER,
  ATTENDEE, CATEGORIES, STATUS, URL, VALARM triggers): identical except that
  `review-5`'s `DURATION:PT1H` is written as the equivalent
  `DTEND:20240531T170000Z`. `vobject.readComponents` parses our file (5 VEVENTs).
- `events_vobject.ics -t ics`: identical in `icalendar`; parsed by `vobject`.
- `freebusy.ifb -t ifb`: identical in `icalendar` (FREEBUSY kept verbatim).
