"""Run on WSL, never on Kindle. Freeze reference fonts, icons and calendars."""
import io
import json
import shutil
import re
import base64
import ast
from pathlib import Path

import exchange_calendars as xcals
import resvg_py
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
source = ROOT.parent / "kindle-dash-gen"
tree = ast.parse((source / 'kindle_dash_gen/render.py').read_text(encoding='utf-8'))
cloud = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == '_draw_cloud_icon')
namespace = {'ImageDraw': ImageDraw, 'INK': 0}
exec(compile(ast.Module(body=[cloud], type_ignores=[]), '<reference cloud>', 'exec'), namespace)
fallback = Image.new('L', (74, 74), 255)
namespace['_draw_cloud_icon'](ImageDraw.Draw(fallback), (0, 0, 74, 74))
fallback.save(ROOT / 'assets/weather/fallback-74.png')
for name in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf"):
    shutil.copyfile(Path("/usr/share/fonts/truetype/dejavu") / name, ROOT / "assets/fonts" / name)
reference = ROOT / "reference.html"
if reference.exists():
    fonts = dict(re.findall(r"font-family:([^;]+);src:url\(data:font/ttf;base64,([^)]*)\)", reference.read_text(encoding="utf-8")))
    for family, filename in (("DashboardFont0", "DejaVuSans-Bold.ttf"), ("DashboardFont1", "DejaVuSans.ttf")):
        (ROOT / "assets/fonts" / filename).write_bytes(base64.b64decode(fonts[family]))
shutil.copyfile("/usr/share/doc/fonts-dejavu-core/copyright", ROOT / "assets/fonts/LICENSE")
for radius in (6, 9):
    mask = Image.new("L", (radius*2+1, radius*2+1))
    ImageDraw.Draw(mask).ellipse((0, 0, radius*2, radius*2), fill=255)
    mask.save(ROOT / f"assets/circle-{radius}.png")
for number in range(1, 26):
    # Freeze the reference renderer's actual prefix lookup, including its ordering.
    path = next((source / "img/weather").glob(f"weather_{number}*.svg"))
    for size in (74, 78):
        png = resvg_py.svg_to_bytes(path.read_text(encoding="utf-8"), width=size*2, height=size*2, background="#ffffff")
        Image.open(io.BytesIO(png)).convert("L").resize((size, size), Image.Resampling.LANCZOS).save(
            ROOT / f"assets/weather/{number}-{size}.png")

# Keep exchange holidays and early closes without installing pandas/numpy on Kindle.
result = {}
for name in ("XSHG", "XNYS"):
    calendar = xcals.get_calendar(name, start="2020-01-01", end="2026-12-31", side="both")
    sessions = {}
    for day, row in calendar.schedule.iterrows():
        spans = []
        start, end = row["open"], row["close"]
        if row["break_start"] is not None and str(row["break_start"]) != "NaT":
            spans = [[start.timestamp(), row["break_start"].timestamp()], [row["break_end"].timestamp(), end.timestamp()]]
        else:
            spans = [[start.timestamp(), end.timestamp()]]
        sessions[day.strftime("%Y-%m-%d")] = spans
    result[name] = {"start": "2020-01-01", "end": "2026-12-31", "sessions": sessions}
(ROOT / "assets/calendars.json").write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")
print("Prepared exact reference fonts/icons and 2020-2026 exchange sessions")
