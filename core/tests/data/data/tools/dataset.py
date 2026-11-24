"""Synthetic dataset shared by the interop scripts (no personal data)."""
import base64
import datetime
import math

UTC = datetime.timezone.utc
BINARY = bytes(range(0, 256, 17)) + b"\x00\xff"
DATE = datetime.datetime(2024, 5, 1, 12, 30, 45, 123456, tzinfo=UTC)


def core(big=True, uint64=True, specials=True):
    """Values every JSON-like format can hold."""
    d = {
        "string": "hello",
        "unicode": "héllo wörld ✓ 日本語 \U0001F600",
        "escapes": "quote \" backslash \\ tab \t newline \n end",
        "int_small": 42,
        "int_neg": -17,
        "int_zero": 0,
        "int_max64": 9223372036854775807,
        "int_min64": -9223372036854775808,
        "float": 3.141592653589793,
        "float_neg": -0.5,
        "float_exp": 6.02214076e23,
        "float_tiny": 5e-324,
        "bool_true": True,
        "bool_false": False,
        "null": None,
        "empty_list": [],
        "empty_object": {},
        "list": [1, "two", 3.25, None, True],
        "nested": {"level1": {"level2": {"level3": [1, [2, [3, [4, {"deep": "yes"}]]]]}}},
        "records": [{"id": 1, "name": "alpha"}, {"id": 2, "name": "beta"}],
    }
    if not big:
        del d["int_max64"], d["int_min64"]
    if uint64:
        d["uint64_max"] = 18446744073709551615
    if specials:
        d["nan"] = math.nan
        d["inf"] = math.inf
        d["neg_inf"] = -math.inf
    return d


def tagged(value):
    """The dataset in the tagged-JSON convention of the data family."""
    if isinstance(value, dict):
        return {k: tagged(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [tagged(v) for v in value]
    if isinstance(value, (bytes, bytearray)):
        return {"$binary": base64.b64encode(bytes(value)).decode()}
    if isinstance(value, datetime.datetime):
        text = value.isoformat()
        if text.endswith("+00:00"):
            text = text[:-6] + "Z"
        return {"$date": text}
    if isinstance(value, datetime.date):
        return {"$date": value.isoformat()}
    if isinstance(value, float) and math.isnan(value):
        return {"$float": "NaN"}
    if isinstance(value, float) and math.isinf(value):
        return {"$float": "Infinity" if value > 0 else "-Infinity"}
    return value
