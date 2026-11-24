# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Location-history exports in the three layouts Google Takeout has used,
# filled with the synthetic "Hotel Run" track (made-up values), written with
# the Python standard library:
#   python3 tools/takeout.py <output-directory>
#   records.json   "locations": latitudeE7 / longitudeE7 / timestamp (ISO) or timestampMs, altitude, accuracy
#   semantic.json  "timelineObjects": placeVisit + activitySegment (with simplifiedRawPath)
#   timeline.json  on-device export: "semanticSegments" with "timelinePath" and visits, "geo:lat,lon" strings
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import synthetic  # noqa: E402

out = sys.argv[1]
points = synthetic.SEG1 + synthetic.SEG2
START_MS = 1710057600000


def ms(t):
    return START_MS + (int(t[14:16]) * 60 + int(t[17:19])) * 1000


locations = []
for i, p in enumerate(points):
    loc = {"latitudeE7": round(p["lat"] * 1e7), "longitudeE7": round(p["lon"] * 1e7), "accuracy": 5,
           "altitude": int(p["ele"])}
    if i % 2:
        loc["timestampMs"] = str(ms(p["t"]))
    else:
        loc["timestamp"] = p["t"][:19] + ".000Z"
    locations.append(loc)
json.dump({"locations": locations}, open(f"{out}/records.json", "w"), indent=1)

wp = synthetic.WAYPOINT
semantic = {"timelineObjects": [
    {"placeVisit": {"location": {"latitudeE7": round(wp["lat"] * 1e7), "longitudeE7": round(wp["lon"] * 1e7),
                                 "name": wp["name"], "address": "Nowhere 1"},
                    "duration": {"startTimestamp": "2024-03-10T07:50:00Z", "endTimestamp": "2024-03-10T07:59:00Z"}}},
    {"activitySegment": {
        "startLocation": {"latitudeE7": round(points[0]["lat"] * 1e7), "longitudeE7": round(points[0]["lon"] * 1e7)},
        "endLocation": {"latitudeE7": round(points[5]["lat"] * 1e7), "longitudeE7": round(points[5]["lon"] * 1e7)},
        "duration": {"startTimestamp": points[0]["t"], "endTimestamp": points[5]["t"]},
        "activityType": "RUNNING",
        "simplifiedRawPath": {"points": [
            {"latE7": round(p["lat"] * 1e7), "lngE7": round(p["lon"] * 1e7), "timestamp": p["t"]}
            for p in synthetic.SEG1]}}},
]}
json.dump(semantic, open(f"{out}/semantic.json", "w"), indent=1)

timeline = {"semanticSegments": [
    {"startTime": "2024-03-10T07:50:00.000+00:00", "endTime": "2024-03-10T07:59:00.000+00:00",
     "visit": {"topCandidate": {"placeLocation": {"latLng": f"{wp['lat']}°, {wp['lon']}°"},
                                "semanticType": "UNKNOWN"}}},
    {"startTime": "2024-03-10T08:00:00.000+00:00", "endTime": "2024-03-10T08:05:40.000+00:00",
     "timelinePath": [{"point": f"{round(p['lat'], 7)}°, {round(p['lon'], 7)}°",
                       "time": p["t"][:19] + ".000+00:00"} for p in points]},
]}
json.dump(timeline, open(f"{out}/timeline.json", "w"), ensure_ascii=False, indent=1)
