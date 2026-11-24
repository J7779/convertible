"""Arrow IPC / Feather fixtures for the table family (run inside the fixture container)."""
import datetime as dt
import decimal
import os
import sys

import pyarrow as pa
import pyarrow.feather as feather
import pyarrow.ipc as ipc

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
P = lambda name: os.path.join(out, name)

table = pa.table({
    "id": pa.array([1, 2, 3], pa.int64()),
    "small": pa.array([1, None, -128], pa.int8()),
    "u16": pa.array([0, 65535, None], pa.uint16()),
    "ratio": pa.array([0.5, None, -1e-7], pa.float64()),
    "f32": pa.array([1.5, -2.25, None], pa.float32()),
    "name": pa.array(["alpha", None, "世界"], pa.string()),
    "flag": pa.array([True, False, None], pa.bool_()),
    "day": pa.array([dt.date(2024, 5, 1), None, dt.date(1960, 2, 29)], pa.date32()),
    "ts": pa.array([dt.datetime(2024, 5, 1, 12, 30), dt.datetime(1999, 12, 31, 23, 59, 59), None], pa.timestamp("ms")),
    "tod": pa.array([dt.time(12, 30), None, dt.time(0, 0, 1)], pa.time64("us")),
    "price": pa.array([decimal.Decimal("12.34"), decimal.Decimal("-0.01"), None], pa.decimal128(9, 2)),
    "colour": pa.array(["red", "green", "red"]).dictionary_encode(),
    "point": pa.array([{"x": 1.0, "y": 2.0}, None, {"x": -1.5, "y": 0.0}], pa.struct([("x", pa.float64()), ("y", pa.float64())])),
    "tags": pa.array([["a", "b"], [], None], pa.list_(pa.string())),
})
feather.write_feather(table, P("types_lz4.feather"))  # default: LZ4-compressed buffers
feather.write_feather(table, P("types_plain.feather"), compression="uncompressed")
with ipc.new_stream(P("types.arrows"), table.schema) as writer:
    writer.write_table(table, max_chunksize=2)  # two record batches
