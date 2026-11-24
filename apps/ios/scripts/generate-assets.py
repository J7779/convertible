#!/usr/bin/env python3
"""Generates the colour sets of the iOS asset catalogue from assets/brand/tokens.json.

Run after changing the design tokens:  python3 -I apps/ios/scripts/generate-assets.py
Every colour token becomes a colour set with a light and a dark appearance:
  Colors/<token>             UI colours (color.dark / color.light)
  Colors/category-<name>     file-category accents (category.<name>.dark/light)
  Colors/flux-0..2           brand gradient stops (gradient.stops / stopsOnLight)
  AccentColor                accentText (the text-safe accent)
"""
import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
IOS = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(IOS))
TOKENS = os.path.join(REPO, "assets", "brand", "tokens.json")
CATALOG = os.path.join(IOS, "Convertible", "Resources", "Assets.xcassets")
INFO = {"author": "xcode", "version": 1}


def components(hex_color):
    h = hex_color.lstrip("#")
    r, g, b = h[0:2], h[2:4], h[4:6]
    a = int(h[6:8], 16) / 255 if len(h) == 8 else 1.0
    return {"alpha": f"{a:.3f}", "blue": f"0x{b.upper()}", "green": f"0x{g.upper()}", "red": f"0x{r.upper()}"}


def write_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")


def colorset(folder, name, light, dark):
    entry = lambda hex_color, appearance: dict(
        {"color": {"color-space": "srgb", "components": components(hex_color)}, "idiom": "universal"},
        **({"appearances": [{"appearance": "luminosity", "value": appearance}]} if appearance else {}),
    )
    write_json(
        os.path.join(folder, f"{name}.colorset", "Contents.json"),
        {"colors": [entry(light, None), entry(light, "light"), entry(dark, "dark")], "info": INFO},
    )


def main():
    with open(TOKENS, encoding="utf-8") as f:
        tokens = json.load(f)
    colors_dir = os.path.join(CATALOG, "Colors")
    shutil.rmtree(colors_dir, ignore_errors=True)
    write_json(os.path.join(colors_dir, "Contents.json"), {"info": INFO, "properties": {"provides-namespace": False}})
    dark, light = tokens["color"]["dark"], tokens["color"]["light"]
    for key in dark:
        colorset(colors_dir, key, light[key], dark[key])
    for key, value in tokens["category"].items():
        colorset(colors_dir, f"category-{key}", value["light"], value["dark"])
    gradient = tokens["brand"]["gradient"]
    for i, (on_dark, on_light) in enumerate(zip(gradient["stops"], gradient["stopsOnLight"])):
        colorset(colors_dir, f"flux-{i}", on_light["color"], on_dark["color"])
    colorset(CATALOG, "AccentColor", light["accentText"], dark["accentText"])
    write_json(os.path.join(CATALOG, "Contents.json"), {"info": INFO})


if __name__ == "__main__":
    main()
