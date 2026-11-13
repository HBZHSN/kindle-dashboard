"""Development only: compare C++ with the user's untouched Python/HTML renderer."""
import json
import math
import subprocess
import sys
import types
from datetime import datetime
from pathlib import Path

from PIL import Image, ImageChops
from PIL import ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / "kindle-dash-gen"))
# The reference imports yfinance but this check never fetches data.
sys.modules.setdefault("yfinance", types.ModuleType("yfinance"))
from kindle_dash_gen.data import MarketQuote, WeatherReport, AgentUsage, UsageWindow, SunsetForecast, SunsetEvent
from kindle_dash_gen.render import DashboardData, render_dashboard
from kindle_dash_gen.html_reference import render_dashboard_html
import kindle_dash_gen.render as reference_render
reference_render._font = lambda size, bold=False, cjk=False: ImageFont.truetype(
    str(ROOT / "assets/fonts" / ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")), size=size)

out = ROOT / "preview/qa"
out.mkdir(parents=True, exist_ok=True)
symbols = ["DEMO-A", "DEMO-B", "sh000001", "sz399303", "sh000688", "sz980017", ".NDX", "NQ", "BTC-USD", "GC"]
notes = ["", "", "Shanghai Composite", "CSI 2000", "STAR 50", "CNI Chip", "Nasdaq 100", "E-mini Nasdaq 100", "BTC", "COMEX Gold"]
prices = ["74%", "96%", "3,856.42", "2,895.70", "1,398.08", "8,218.33", "24,890.10", "25,140.75", "113,402.80", "3,845.70"]
market = [dict(symbol=s, note=notes[i], price=prices[i], change=f"{(i-3)*0.37:+.2f}%", status="OK",
               intraday=[round(math.sin(j*.19+i)*.12+j*.001*(i-4),4) if j%19 else None for j in range(120)],
               trading_progress=.63 if i<6 else 1, is_closed=i==6, is_24h=i>=7, delay_minutes=4 if i==7 else 0,
               converted="874.50 CNY/g" if i==9 else "") for i,s in enumerate(symbols)]
weather = dict(title="Hangzhou", temperature="28.6 C", temperature_value_c=28.6, temperature_min_c=23.4,
               temperature_max_c=31.2, temperature_min="23.4 C", temperature_max="31.2 C", weather_code=2,
               humidity="Humidity 65%", precipitation_probability="Rain 20%", precipitation_amount="Precip 0.1mm",
               uv_index="UV 3.2", aqi="AQI 42", status="OK")
usage = {key:dict(name=name,used_percent=pct,not_started=False,no_limit=False,status="OK",t_progress=t,e_progress=e,reset_after_human=reset)
         for key,name,pct,t,e,reset in [("codex_5h","CODEX 5H",35,48,55,"2h 36m"),("codex_7d","CODEX 7D",62,55,71,"3d 4h"),
                                       ("agy_5h","AGY 5H",17,36,27,"3h 12m"),("agy_7d","AGY 7D",48,64,59,"2d 7h")]}
sunset=dict(status="OK",fetched_at=1790062500,events=[
    dict(name="Sunset",time="2026-09-22T17:57:00+08:00",code="set_1",ec_aod="0.21",gfs_aod="0.24",ec_quality="0.42",gfs_quality="0.61",fetched_at=1790062500),
    dict(name="Sunrise",time="2026-09-23T05:48:00+08:00",code="rise_2",ec_aod="0.12",gfs_aod="0.15",ec_quality="0.22",gfs_quality="0.39",fetched_at=1790062500)])
raw=dict(generated_at="2026-09-22T15:35:00+08:00",market=market,weather=weather,agent_usage=usage,sunset=sunset)
fixture=ROOT/"tests/sample.json"
fixture.write_text(json.dumps(raw,indent=2),encoding="utf-8")
data=DashboardData(datetime.fromisoformat(raw["generated_at"]),[MarketQuote(**q) for q in market],WeatherReport(**weather),
                   agent_usage=AgentUsage(**{k:UsageWindow(**v) for k,v in usage.items()}),
                   sunset=SunsetForecast(status="OK",events=[SunsetEvent(**e) for e in sunset["events"]]))
render_dashboard(data,out/"reference-device.png","landscape")
Image.open(out/"reference-device.png").transpose(Image.Transpose.ROTATE_90).save(out/"reference.png")
(out/"reference.html").write_text(render_dashboard_html(data,"landscape"),encoding="utf-8")
subprocess.run([str(ROOT/".build/native/kindle-dashboard"),"--config",str(ROOT/"tests/demo.yaml"),"--simulate","--offline","--once","--freeze-time",
                "--snapshot",str(fixture),"--output",str(out/"native")],cwd=ROOT,check=True)
a=Image.open(out/"reference.png").convert("L")
b=Image.open(out/"native/frame.png").convert("L")
diff=ImageChops.difference(a,b)
different=sum(n for i,n in enumerate(diff.histogram()) if i)
report={"size":list(a.size),"different_pixels":different,"identical_percent":round(100*(1-different/(a.width*a.height)),4),
        "mean_absolute_error":round(sum(i*n for i,n in enumerate(diff.histogram()))/(a.width*a.height),4)}
diff.point(lambda x:255-x).save(out/"difference.png")
side=Image.new("L",(a.width*2,a.height),255);side.paste(a,(0,0));side.paste(b,(a.width,0));side.save(out/"comparison.png")
(out/"comparison.json").write_text(json.dumps(report,indent=2))
print(json.dumps(report))
assert different == 0, f'Native layout differs from reference: {report}'
