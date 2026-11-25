"""Natural Earth 1:10m land GeoJSON から羽田周辺の陸域を生成する。"""
import json
import math
import sys
from pathlib import Path

size = 960
scale = size / 120
longitude_scale = 60 * math.cos(math.radians(35.5494))
rows = [[] for _ in range(size)]
data = json.loads(Path(sys.argv[1]).read_bytes())
for feature in data["features"]:
    geometry = feature["geometry"]
    polys = geometry["coordinates"] if geometry["type"] == "MultiPolygon" else [geometry["coordinates"]]
    for polygon in polys:
        shell = polygon[0]
        if not any(137 < x < 142 and 34 < y < 37 for x, y in shell):
            continue
        cuts = [[] for _ in range(size)]
        for ring in polygon:
            coords = [((x - 139.7798) * longitude_scale * scale + size/2,
                       (35.5494 - y) * 60 * scale + size/2) for x, y in ring]
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

def array(name, numbers):
    lines = [", ".join(map(str, numbers[i:i+16])) for i in range(0, len(numbers), 16)]
    return f"constexpr uint16_t {name}[] = {{\n    " + ",\n    ".join(lines) + "\n};\n"

out = Path(__file__).resolve().parents[1] / "src" / "coast.h"
out.write_text("// Natural Earth 1:10m land (public domain), tools/make_coast.py\n"
               + f"constexpr int CoastSize = {size};\n"
               + array("CoastRows", offsets) + array("CoastSpans", spans), encoding="utf-8")
