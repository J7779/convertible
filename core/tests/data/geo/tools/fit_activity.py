# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Writes the synthetic "Hotel Run" track as a FIT activity with fit-tool:
#   /venv/bin/python fit_activity.py out.fit
# Two laps (one per track segment), records with position, altitude, heart
# rate, cadence, power and temperature, a session and an activity message.
# Manufacturer "development", no serial number.
import sys

from fit_tool.fit_file_builder import FitFileBuilder
from fit_tool.profile.messages.activity_message import ActivityMessage
from fit_tool.profile.messages.event_message import EventMessage
from fit_tool.profile.messages.file_id_message import FileIdMessage
from fit_tool.profile.messages.lap_message import LapMessage
from fit_tool.profile.messages.record_message import RecordMessage
from fit_tool.profile.messages.session_message import SessionMessage
from fit_tool.profile.profile_type import Event, EventType, FileType, Manufacturer, Sport

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import synthetic  # noqa: E402

START = 1710057600000  # 2024-03-10T08:00:00Z


def ms(t):
    # "2024-03-10T08:mm:ssZ"
    return START + (int(t[14:16]) * 60 + int(t[17:19])) * 1000


builder = FitFileBuilder(auto_define=True, min_string_size=16)
m = FileIdMessage()
m.type = FileType.ACTIVITY
m.manufacturer = Manufacturer.DEVELOPMENT.value
m.product = 0
m.time_created = START
builder.add(m)

e = EventMessage()
e.event = Event.TIMER
e.event_type = EventType.START
e.timestamp = START
builder.add(e)

laps = []
for seg in (synthetic.SEG1, synthetic.SEG2):
    for p in seg:
        r = RecordMessage()
        r.timestamp = ms(p["t"])
        r.position_lat = round(p["lat"], 7)
        r.position_long = round(p["lon"], 7)
        r.altitude = p["ele"]
        r.heart_rate = p["hr"]
        r.cadence = p["cad"]
        r.power = p["power"]
        r.temperature = p["temp"]
        builder.add(r)
    lap = LapMessage()
    lap.timestamp = ms(seg[-1]["t"])
    lap.start_time = ms(seg[0]["t"])
    lap.total_elapsed_time = (ms(seg[-1]["t"]) - ms(seg[0]["t"])) / 1000
    lap.total_timer_time = lap.total_elapsed_time
    lap.avg_heart_rate = round(sum(p["hr"] for p in seg) / len(seg))
    lap.max_heart_rate = max(p["hr"] for p in seg)
    laps.append(lap)

e = EventMessage()
e.event = Event.TIMER
e.event_type = EventType.STOP_ALL
e.timestamp = ms(synthetic.SEG2[-1]["t"])
builder.add(e)
builder.add_all(laps)

s = SessionMessage()
s.timestamp = ms(synthetic.SEG2[-1]["t"])
s.start_time = START
s.total_elapsed_time = (ms(synthetic.SEG2[-1]["t"]) - START) / 1000
s.total_timer_time = s.total_elapsed_time
s.sport = Sport.RUNNING
s.num_laps = 2
builder.add(s)

a = ActivityMessage()
a.timestamp = ms(synthetic.SEG2[-1]["t"])
a.num_sessions = 1
a.total_timer_time = s.total_elapsed_time
builder.add(a)

builder.build().to_file(sys.argv[1])
