"""Natural Earth 1:10m land GeoJSON から各空港周辺の陸域を生成する。"""
import json
import math
import sys
from pathlib import Path

size = 960
scale = size / 120
# 保存済みの地域番号に合わせ、この順序を保持する。
regions = [
    ("HND", "TOKYO  /  HANEDA", 35.5494, 139.7798),
    ("KIX", "OSAKA  /  KANSAI", 34.4347, 135.2440),
    ("OKA", "OKINAWA  /  NAHA", 26.1958, 127.6459),
]


def rasterize(data, lat, lon):
    longitude_scale = 60 * math.cos(math.radians(lat))
    rows = [[] for _ in range(size)]
    for feature in data["features"]:
        geometry = feature["geometry"]
        polys = geometry["coordinates"] if geometry["type"] == "MultiPolygon" else [geometry["coordinates"]]
        for polygon in polys:
            xs, ys = zip(*polygon[0])
            half_width = 60 / longitude_scale
            if max(xs) < lon-half_width or min(xs) > lon+half_width or max(ys) < lat-1 or min(ys) > lat+1:
                continue
            cuts = [[] for _ in range(size)]
            for ring in polygon:
                coords = [((x - lon) * longitude_scale * scale + size/2,
                           (lat - y) * 60 * scale + size/2) for x, y in ring]
                for (x1, y1), (x2, y2) in zip(coords, coords[1:]):
                    start = max(0, math.ceil(min(y1, y2) - .5))
                    stop = min(size, math.ceil(max(y1, y2) - .5))
                    for y in range(start, stop):
                        cuts[y].append(x1 + (y + .5 - y1) * (x2 - x1) / (y2 - y1))
            for row, edges in zip(rows, cuts):
                edges.sort()
                for a, b in zip(edges[::2], edges[1::2]):
                    left, right = max(0, math.ceil(a - .5)), min(size, math.ceil(b - .5))
                    if right > left:
                        row.extend((left, right))
    offsets, spans = [0], []
    for row in rows:
        spans.extend(row)
        offsets.append(len(spans))
    assert 0 < len(spans) <= 65535
    return offsets, spans


def array(name, numbers):
    lines = [", ".join(map(str, numbers[i:i+16])) for i in range(0, len(numbers), 16)]
    return f"constexpr uint16_t {name}[] = {{\n    " + ",\n    ".join(lines) + "\n};\n"


data = json.loads(Path(sys.argv[1]).read_bytes())
output = ["// Natural Earth 1:10m land (public domain), tools/make_coast.py\n",
          f"constexpr int CoastSize = {size};\n"]
entries = []
for code, title, lat, lon in regions:
    offsets, spans = rasterize(data, lat, lon)
    output.extend((array(code + "Rows", offsets), array(code + "Spans", spans)))
    entries.append(f'    {{"{code}", "{title}", {lat:.4f}, {lon:.4f}, {code}Rows, {code}Spans}}')
output.append("const Region regions[] = {\n" + ",\n".join(entries) + "\n};\n")

out = Path(__file__).resolve().parents[1] / "src" / "coast.h"
out.write_text("".join(output), encoding="utf-8")
