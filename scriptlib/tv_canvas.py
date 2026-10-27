"""tv_canvas — draw an artwork plus a weather placard as one 3840x2160 image.

Pure drawing: no network, no TV. tv_weather_art decides what to show; this
decides how it looks. The layout is the one approved on the real Frame
(2026-10-21): the work on the left over a blurred, darkened copy of itself,
a museum-style placard on the right with today's weather, four days ahead,
and the work's label. Placard colours come from the work so it never clashes.
"""

import datetime as dt
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageStat

W, H = 3840, 2160
PANEL_W, PANEL_RIGHT, PANEL_Y = 980, 150, 210

# Preferred faces first; DejaVu ships with almost every Linux and is the floor.
_FACES = {
    "serif":    ["/usr/share/fonts/opentype/urw-base35/C059-Roman.otf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf"],
    "serif_it": ["/usr/share/fonts/opentype/urw-base35/C059-Italic.otf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Italic.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf"],
    "serif_bd": ["/usr/share/fonts/opentype/urw-base35/C059-Bold.otf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"],
    "sans":     ["/usr/share/fonts/truetype/lato/Lato-Regular.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"],
    "sans_lt":  ["/usr/share/fonts/truetype/lato/Lato-Light.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-ExtraLight.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"],
    "sans_bd":  ["/usr/share/fonts/truetype/lato/Lato-Bold.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"],
}
_cache = {}


def font(face, size):
    key = (face, size)
    if key not in _cache:
        path = next((p for p in _FACES[face] if Path(p).exists()), None)
        _cache[key] = ImageFont.truetype(path, size) if path else ImageFont.load_default()
    return _cache[key]


DIAS = ["lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"]
MESES = ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
         "septiembre", "octubre", "noviembre", "diciembre"]
LABEL = {"sun": "despejado", "partly": "parcialmente nublado", "cloud": "nublado",
         "fog": "niebla", "drizzle": "llovizna", "rain": "lluvia", "snow": "nieve",
         "storm": "tormenta"}


def kind(code):
    """WMO weather code -> one of LABEL's keys."""
    if code == 0: return "sun"
    if code in (1, 2): return "partly"
    if code == 3: return "cloud"
    if code in (45, 48): return "fog"
    if 51 <= code <= 57: return "drizzle"
    if 61 <= code <= 67 or 80 <= code <= 82: return "rain"
    if 71 <= code <= 77 or code in (85, 86): return "snow"
    if code >= 95: return "storm"
    return "cloud"


def icon(d, cx, cy, r, k, ink):
    """Line icons, drawn rather than glyphs, so they match the ink of the work."""
    w = max(2, int(r * 0.09))

    def sun(x, y, rr):
        d.ellipse([x - rr * .45, y - rr * .45, x + rr * .45, y + rr * .45], outline=ink, width=w)
        for i in range(8):
            a = i * math.pi / 4
            d.line([x + math.cos(a) * rr * .62, y + math.sin(a) * rr * .62,
                    x + math.cos(a) * rr * .85, y + math.sin(a) * rr * .85], fill=ink, width=w)

    def cloud(x, y, rr):
        d.arc([x - rr * .9, y - rr * .25, x - rr * .1, y + rr * .55], 90, 270, fill=ink, width=w)
        d.arc([x - rr * .55, y - rr * .75, x + rr * .35, y + rr * .15], 180, 340, fill=ink, width=w)
        d.arc([x + rr * .05, y - rr * .45, x + rr * .9, y + rr * .55], 270, 90, fill=ink, width=w)
        d.line([x - rr * .5, y + rr * .55, x + rr * .48, y + rr * .55], fill=ink, width=w)

    if k == "sun":
        sun(cx, cy, r)
        return
    if k == "partly":
        sun(cx + r * .35, cy - r * .35, r * .7)
        cloud(cx - r * .1, cy + r * .1, r * .8)
        return
    cloud(cx, cy - (r * .2 if k in ("rain", "drizzle", "snow", "storm") else 0), r)
    base = cy + r * .45
    if k in ("rain", "drizzle"):
        n, ln = (3, .35) if k == "rain" else (4, .15)
        for i in range(n):
            x = cx - r * .45 + i * r * .9 / (n - 1)
            d.line([x, base + r * .1, x - r * .12, base + r * (.1 + ln)], fill=ink, width=w)
    elif k == "snow":
        for i in range(3):
            d.text((cx - r * .4 + i * r * .4, base + r * .2), "*",
                   font=font("sans_bd", int(r * .5)), fill=ink, anchor="mm")
    elif k == "storm":
        d.line([cx, base, cx - r * .15, base + r * .3, cx + r * .1, base + r * .3,
                cx - r * .05, base + r * .6], fill=ink, width=w)
    elif k == "fog":
        for i in range(3):
            y = cy + r * (.45 + i * .18)
            d.line([cx - r * .7, y, cx + r * .7, y], fill=ink, width=w)


def wrap(d, text, fnt, width, max_lines=3):
    words, lines, cur = (text or "").split(), [], ""
    for word in words:
        trial = (cur + " " + word).strip()
        if d.textlength(trial, font=fnt) <= width:
            cur = trial
        else:
            if cur:
                lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] = lines[-1].rstrip(" ,.;") + "…"
    return lines


def _background(art):
    scale = max(W / art.width, H / art.height)
    bg = art.resize((math.ceil(art.width * scale), math.ceil(art.height * scale)), Image.LANCZOS)
    left, top = (bg.width - W) // 2, (bg.height - H) // 2
    bg = bg.crop((left, top, left + W, top + H)).filter(ImageFilter.GaussianBlur(60))
    return Image.blend(bg, Image.new("RGB", (W, H), (20, 18, 16)), 0.45)


def _place_work(bg, art):
    panel_x = W - PANEL_W - PANEL_RIGHT
    margin = int(W * .05)
    box_w, box_h = panel_x - margin - 120, int(H * .80)
    s = min(box_w / art.width, box_h / art.height)
    fg = art.resize((max(1, int(art.width * s)), max(1, int(art.height * s))), Image.LANCZOS)
    x0, y0 = margin + (box_w - fg.width) // 2, (H - fg.height) // 2
    shadow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shadow).rectangle([x0 + 18, y0 + 24, x0 + fg.width + 18, y0 + fg.height + 24], fill=150)
    bg.paste((0, 0, 0), (0, 0), shadow.filter(ImageFilter.GaussianBlur(30)))
    bg.paste(fg, (x0, y0))


def _stamp(iso):
    """HH:MM of Open-Meteo's `current.time` (already local: timezone=auto), or ''."""
    try:
        return dt.datetime.fromisoformat(iso).strftime("%H:%M")
    except (TypeError, ValueError):
        return ""


def compose(art, meta, weather):
    """art: PIL image; meta: artist, bio, title, detail, place; weather: Open-Meteo JSON."""
    art = art.convert("RGB")
    bg = _background(art)
    _place_work(bg, art)

    dom = tuple(int(c) for c in ImageStat.Stat(art.resize((64, 64))).mean)
    panel = tuple(int(c * .25) for c in dom)
    ink, soft = (241, 234, 220), (196, 186, 168)
    px, py = W - PANEL_W - PANEL_RIGHT, PANEL_Y
    ph = H - 2 * PANEL_Y
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(over).rounded_rectangle([px, py, px + PANEL_W, py + ph], radius=36,
                                           fill=panel + (205,))
    bg = Image.alpha_composite(bg.convert("RGBA"), over).convert("RGB")
    d = ImageDraw.Draw(bg)
    left, right, width = px + 80, px + PANEL_W - 80, PANEL_W - 160
    y = py + 80

    cur, day = weather["current"], weather["daily"]
    today = dt.date.fromisoformat(day["time"][0])
    d.text((left, y), (meta.get("place") or "").upper(), font=font("sans_bd", 34), fill=soft)
    y += 56
    d.text((left, y), f"{DIAS[today.weekday()]} {today.day} de {MESES[today.month - 1]}",
           font=font("serif_it", 46), fill=ink)
    stamp = _stamp(cur.get("time"))
    if stamp:
        d.text((left, y + 62), f"clima actualizado a las {stamp}", font=font("sans", 28), fill=soft)
    y += 110
    k = kind(cur["weather_code"])
    icon(d, left + 95, y + 95, 95, k, ink)
    d.text((left + 240, y - 10), f"{round(cur['temperature_2m'])}°", font=font("sans_lt", 190), fill=ink)
    y += 220
    d.text((left, y), LABEL[k], font=font("serif", 48), fill=ink)
    y += 70
    d.text((left, y), f"sensación {round(cur['apparent_temperature'])}° · "
                      f"humedad {cur['relative_humidity_2m']}%", font=font("sans", 34), fill=soft)
    y += 52
    d.text((left, y), f"hoy  ↑{round(day['temperature_2m_max'][0])}°  "
                      f"↓{round(day['temperature_2m_min'][0])}°  ·  "
                      f"lluvia {day['precipitation_probability_max'][0]}%",
           font=font("sans", 34), fill=soft)
    y += 100
    d.line([left, y, right, y], fill=soft, width=2)
    y += 60

    colw = width / 4
    for i in range(1, min(5, len(day["time"]))):
        cx = left + colw * (i - 1) + colw / 2
        dd = dt.date.fromisoformat(day["time"][i])
        d.text((cx, y), DIAS[dd.weekday()][:3], font=font("sans_bd", 34), fill=soft, anchor="mt")
        icon(d, cx, y + 130, 52, kind(day["weather_code"][i]), ink)
        d.text((cx, y + 225), f"{round(day['temperature_2m_max'][i])}°", font=font("sans", 44),
               fill=ink, anchor="mt")
        d.text((cx, y + 280), f"{round(day['temperature_2m_min'][i])}°", font=font("sans_lt", 38),
               fill=soft, anchor="mt")
        d.text((cx, y + 334), f"{day['precipitation_probability_max'][i]}%", font=font("sans", 30),
               fill=soft, anchor="mt")
    y += 420
    d.line([left, y, right, y], fill=soft, width=2)
    y += 60

    if meta.get("artist"):
        d.text((left, y), meta["artist"], font=font("serif_bd", 46), fill=ink)
        y += 64
    if meta.get("bio"):
        # The Met's bios run long ("American, Boston, Massachusetts 1836–1910
        # Prouts Neck, Maine") and overflowed the placard on the first real run.
        for line in wrap(d, meta["bio"], font("sans", 30), width, max_lines=2):
            d.text((left, y), line, font=font("sans", 30), fill=soft)
            y += 42
        y += 18
    for line in wrap(d, meta.get("title"), font("serif_it", 50), width, max_lines=2):
        d.text((left, y), line, font=font("serif_it", 50), fill=ink)
        y += 64
    y += 6
    for line in wrap(d, meta.get("detail"), font("sans", 30), width, max_lines=2):
        d.text((left, y), line, font=font("sans", 30), fill=soft)
        y += 42
    d.text((left, py + ph - 70), "The Met · dominio público   ·   datos: Open-Meteo",
           font=font("sans", 26), fill=soft)
    return bg
