#!/usr/bin/env python3
"""tv_weather_art — a museum piece for today's weather, with the forecast on it.

Run by Funes through `run_script` (see tv_weather_art.yaml). stdout is
exactly one JSON object; stderr is logs. Three events, three schedules:

    --mode=render    every 15 min: fetch the forecast, reuse today's work
                     (chosen and translated once a day), compose the picture.
                     Never touches the TV.
    --mode=publish   every minute: if there is a picture the TV has not been
                     given and the TV answers, send it. A set that is asleep
                     stays pending, so turning it on is what publishes —
                     within a minute, with data at most 15 min old.
    --mode=both      (default) render, then publish — what a person asking gets.

    [--refresh_art]   choose a different work today
    [--query=<text>]  choose today's work by this search instead ("Turner")
    [--wake]          wake the TV if it is asleep; also publishes even if the
                      TV already has this picture (a person asked)

Decisions this script owns:

- **Publishing is one rule:** the newest render is newer than the last one
  the TV was given, and the TV answers. A one-line "answers?" probe comes
  first so a minute-by-minute job costs nothing while the set is asleep.
- **A set in use is not taken over.** Scheduled publishes pass tv_art
  `--gentle`: if the set answers with Art Mode off someone is watching, and
  the picture is only selected for the next time Art Mode shows.

- **One work per day, chosen by weather and season.** Today's forecast and the
  southern-hemisphere season become Met searches ("spring rain", then "rain",
  then "spring landscape"); candidates are shuffled with the date as seed, so
  a rerun the same day picks the same work. Recently shown works are skipped.
- **Only works that will look right on the wall.** Public domain, an original
  image, landscape-shaped (by the catalogue's measurements before anything is
  downloaded, then by pixels) and at least MIN_PX wide — the Frame is 4K, and
  the approved mockup's 1995 px original was already at the limit.
- **A Spanish label, or the original — never a broken one.** The title, bio
  and medium are translated once per work by the installation's LLM. The
  reply must have exactly the expected fields, single-line and of plausible
  length; anything else keeps the Met's English, and the next run retries.
- **The LLM is shared, and has one slot.** llama-server on yoda runs
  --parallel 1, so a translation in the middle of the newsletter would evict
  the curator's prompt cache. The script asks /slots first: a scheduled run
  translates only when the LLM is idle (busy, or stopped for run_images'
  ComfyUI window, means "English for now, retry next run"); a person's
  request (--wake, --refresh_art, --query) waits up to LLM_WAIT_SECONDS.
- **A sleeping TV is not a failure.** The Frame drops off the network when
  Art Mode blanks the screen; a scheduled run then reports shown=false and
  exits 0, so an empty living room does not fill the job log with FAILED.

Exit codes: 0 done (shown or deliberately not), 1 a source or the TV failed,
2 bad arguments, 3 not configured.

Config (manifest env, from config/funes.local and the service):
    FUNES_WEATHER_LAT, FUNES_WEATHER_LON, FUNES_WEATHER_PLACE
    FUNES_TV_HOST, FUNES_TV_TOKEN_FILE (+ FUNES_TV_MAC)   — see tv_art.py
    FUNES_LLM_URL, FUNES_LLM_MODEL                        — for the translation
"""

import fcntl
import io
import json
import os
import random
import re
import socket
import sys
import time
import urllib.parse
import urllib.request
from datetime import date, datetime
from pathlib import Path

import tv_art
import tv_canvas
from PIL import Image

MET = "https://collectionapi.metmuseum.org/public/collection"
UA = {"User-Agent": "Funes tv_weather_art (home use)"}
MIN_PX = 2500                 # original's width; the panel is 3840
RATIO = (1.2, 2.4)            # landscape works only; hanging scrolls look lost
CHECK_PER_QUERY = 25          # objects examined per search before moving on
HISTORY = 60                  # days before a work may come back
RETRY_SECONDS = 5
LLM_WAIT_SECONDS = 30         # a person's request waits this long for a busy LLM
LLM_POLL_SECONDS = 5
OUTPUT = "tv/arte_y_clima.jpg"   # inside the caller's workspace (the cwd)
MODES = ("render", "publish", "both")
TV_PORT = 8002                # the set's secure websocket; closed while it sleeps
PROBE_SECONDS = 2
LOG_BYTES = 256 * 1024        # events.log rotates at this size...
LOG_KEEP = 3                  # ...keeping this many older files (~2 months)

SEASON_EN = {"primavera": "spring", "verano": "summer", "otoño": "autumn", "invierno": "winter"}
WEATHER_EN = {
    "sun": ["sunlight", "sunny"], "partly": ["clouds", "sky"],
    "cloud": ["clouds", "overcast"], "fog": ["fog", "mist"],
    "drizzle": ["rain", "mist"], "rain": ["rain", "wet"],
    "storm": ["storm", "tempest"], "snow": ["snow", "winter"],
}


class Refusal(Exception):
    pass


class NotConfigured(Exception):
    pass


class NoArtwork(Exception):
    pass


def log(msg):
    print(f"tv_weather_art: {msg}", file=sys.stderr, flush=True)


def today():
    return date.today()


# ── choosing the work ────────────────────────────────────────────────────────

def season(d):
    """Southern hemisphere, by month."""
    return {12: "verano", 1: "verano", 2: "verano", 3: "otoño", 4: "otoño", 5: "otoño",
            6: "invierno", 7: "invierno", 8: "invierno"}.get(d.month, "primavera")


def queries(kind, season_name):
    s = SEASON_EN[season_name]
    terms = WEATHER_EN.get(kind, ["landscape"])
    out = [f"{s} {t}" for t in terms] + terms + [f"{s} landscape"]
    return list(dict.fromkeys(out))


class Met:
    def _get(self, url, raw=False):
        req = urllib.request.Request(url, headers=UA)
        with urllib.request.urlopen(req, timeout=30) as r:
            data = r.read()
        return data if raw else json.loads(data)

    def search(self, query):
        q = urllib.parse.quote(query)
        r = self._get(f"{MET}/v1.1/search?hasImages=true&medium=Paintings&q={q}&limit=80")
        return r.get("objectIDs") or []

    def obj(self, oid):
        return self._get(f"{MET}/v1/objects/{oid}")

    def image(self, url):
        return self._get(url, raw=True)


def catalogue_ratio(o):
    """Width/height from the catalogue's measurements, or None if it has none."""
    for m in o.get("measurements") or []:
        em = m.get("elementMeasurements") or {}
        w, h = em.get("Width"), em.get("Height")
        if w and h:
            return w / h
    return None


def pick_artwork(met, query_list, day, history):
    seen = set(history)
    for q in query_list:
        try:
            ids = [i for i in met.search(q) if i not in seen]
        except Exception as e:
            log(f"search {q!r} failed: {e}")
            continue
        random.Random(f"{day.isoformat()}:{q}").shuffle(ids)
        for oid in ids[:CHECK_PER_QUERY]:
            try:
                o = met.obj(oid)
                if not (o.get("isPublicDomain") and o.get("primaryImage")):
                    continue
                r = catalogue_ratio(o)
                if r is not None and not RATIO[0] <= r <= RATIO[1]:
                    continue
                data = met.image(o["primaryImage"])
                w, h = Image.open(io.BytesIO(data)).size
            except Exception as e:
                log(f"object {oid}: {type(e).__name__}: {e}")
                continue
            if w < MIN_PX or not RATIO[0] <= w / h <= RATIO[1]:
                continue
            log(f"chose {oid} for {q!r}: {o.get('title')} ({w}x{h})")
            return {**o, "_image": data}
    raise NoArtwork(f"no usable public-domain landscape for {query_list}")


def label(o):
    when = o.get("objectDate") or o.get("period") or o.get("dynasty") or ""
    return {
        "title": (o.get("title") or "Sin título").strip(),
        "bio": (o.get("artistDisplayBio") or "").strip(),
        "detail": " · ".join(x for x in (when.strip(), (o.get("medium") or "").strip()) if x),
    }


# ── the Spanish label ────────────────────────────────────────────────────────

PROMPT = """Traducí al castellano rioplatense neutro, como en la ficha de un museo, \
los valores de este JSON. No traduzcas nombres propios de personas. Respondé solo \
con el JSON, mismas claves, cada valor en una sola línea.

{payload}"""


def llm_complete(prompt):
    base = os.environ.get("FUNES_LLM_URL", "").rstrip("/")
    if not base or "anthropic" in base:
        raise OSError("no OpenAI-compatible LLM configured")
    body = {
        "model": os.environ.get("FUNES_LLM_MODEL") or "default",
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0, "max_tokens": 400,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    req = urllib.request.Request(base + "/v1/chat/completions", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)["choices"][0]["message"]["content"]


def llm_idle():
    """True only if the LLM answers and no slot is processing. Down = busy."""
    base = os.environ.get("FUNES_LLM_URL", "").rstrip("/")
    if not base or "anthropic" in base:
        return False
    try:
        with urllib.request.urlopen(base + "/slots", timeout=5) as r:
            slots = json.loads(r.read())
        return bool(slots) and not any(s.get("is_processing") for s in slots)
    except Exception as e:
        log(f"LLM status unknown ({type(e).__name__}: {e}); treating it as busy")
        return False


def may_translate(interactive):
    if llm_idle():
        return True
    if not interactive:
        log("LLM busy or stopped; keeping the original label until a later run")
        return False
    deadline = time.monotonic() + LLM_WAIT_SECONDS
    while time.monotonic() < deadline:
        time.sleep(LLM_POLL_SECONDS)
        if llm_idle():
            return True
    log(f"LLM still busy after {LLM_WAIT_SECONDS} s; keeping the original label for now")
    return False


def translate_if_allowed(meta, interactive):
    if not may_translate(interactive):
        return dict(meta), False
    return translate(meta, llm_complete)


def translate(meta, complete):
    """(translated, True) or (meta unchanged, False). Never half of each."""
    try:
        reply = complete(PROMPT.format(payload=json.dumps(meta, ensure_ascii=False)))
        reply = re.sub(r"<think>.*?</think>", "", reply, flags=re.S).strip()
        reply = re.sub(r"^```(?:json)?\s*|\s*```$", "", reply).strip()
        out = json.loads(reply)
        if not isinstance(out, dict) or set(out) != set(meta):
            raise ValueError(f"keys {sorted(out) if isinstance(out, dict) else type(out)}")
        for k, v in out.items():
            if not isinstance(v, str) or "\n" in v:
                raise ValueError(f"{k} is not one line of text")
            if len(v) > 2 * len(meta[k]) + 40:
                raise ValueError(f"{k} is implausibly long")
        return {k: v.strip() for k, v in out.items()}, True
    except Exception as e:
        log(f"translation kept the original: {type(e).__name__}: {e}")
        return dict(meta), False


# ── weather, cache, run ──────────────────────────────────────────────────────

def fetch_weather(lat, lon):
    q = (f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}"
         "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code"
         "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
         "&timezone=auto&forecast_days=5")
    req = urllib.request.Request(q, headers=UA)
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)


def parse(argv):
    args = {"refresh_art": False, "wake": False, "query": "", "mode": "both"}
    for token in argv:
        key, sep, value = token[2:].partition("=") if token.startswith("--") else ("", "", "")
        if key in ("refresh_art", "wake") and not sep:
            args[key] = True
        elif key == "query" and sep:
            args["query"] = value.strip()[:100]
        elif key == "mode" and value in MODES:
            args["mode"] = value
        else:
            raise Refusal(f"unknown argument {token!r}")
    return args


def config():
    need = ["FUNES_WEATHER_LAT", "FUNES_WEATHER_LON", "FUNES_TV_HOST", "FUNES_TV_TOKEN_FILE"]
    missing = [n for n in need if not os.environ.get(n, "").strip()]
    if missing:
        raise NotConfigured(f"{', '.join(missing)} not set — add to config/funes.local")
    cache = Path(os.environ["FUNES_TV_TOKEN_FILE"]).parent / "tv_weather_art"
    return (float(os.environ["FUNES_WEATHER_LAT"]), float(os.environ["FUNES_WEATHER_LON"]),
            os.environ.get("FUNES_WEATHER_PLACE", "").strip(), cache)


def read_json(path, default):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return default


def write_json(path, value):
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(value, ensure_ascii=False))
    tmp.replace(path)


def todays_work(cache, args, weather):
    """Today's {objectID, meta, translated}, choosing or re-translating as needed."""
    cache.mkdir(parents=True, exist_ok=True)
    state_file, image_file = cache / "today.json", cache / "art.jpg"
    state = read_json(state_file, {})
    day = today()
    interactive = args["wake"] or args["refresh_art"] or bool(args["query"])
    fresh = (state.get("date") == day.isoformat() and image_file.exists()
             and not args["refresh_art"] and not args["query"])
    if fresh:
        if not state.get("translated"):
            state["meta"], state["translated"] = translate_if_allowed(state["original"], interactive)
            write_json(state_file, state)
        return state, image_file

    history = read_json(cache / "history.json", [])
    if args["refresh_art"] and state.get("objectID"):
        history = [state["objectID"]] + history
    kind = tv_canvas.kind(weather["daily"]["weather_code"][0])
    qs = [args["query"]] if args["query"] else queries(kind, season(day))
    o = pick_artwork(Met(), qs, day, history)
    image_file.write_bytes(o.pop("_image"))
    original = label(o)
    meta, translated = translate_if_allowed(original, interactive)
    state = {"date": day.isoformat(), "objectID": o["objectID"],
             "artist": (o.get("artistDisplayName") or "").strip(),
             "original": original, "meta": meta, "translated": translated, "query": qs[0]}
    write_json(state_file, state)
    write_json(cache / "history.json", ([o["objectID"]] + [h for h in history if h != o["objectID"]])[:HISTORY])
    return state, image_file


def event(cache, **fields):
    """One line in <cache>/events.log for something that happened (a render, a
    publish, a failure) — never for a minute where nothing did, and never a
    reason to fail the run. The job row's last_output only keeps the last
    run, which on a per-minute job is always "up to date"."""
    try:
        cache.mkdir(parents=True, exist_ok=True)
        path = cache / "events.log"
        if path.exists() and path.stat().st_size >= LOG_BYTES:
            for i in range(LOG_KEEP, 0, -1):
                older = path.with_name(f"{path.name}.{i}")
                source = path if i == 1 else path.with_name(f"{path.name}.{i - 1}")
                if source.exists():
                    source.replace(older)
        with open(path, "a") as f:
            f.write(f"{datetime.now():%Y-%m-%d %H:%M:%S} "
                    f"{json.dumps(fields, ensure_ascii=False)}\n")
    except OSError as e:
        log(f"could not write the event log: {e}")


def tv_awake(host):
    """Does the set answer at all? Cheap enough to ask every minute."""
    try:
        with socket.create_connection((host, TV_PORT), timeout=PROBE_SECONDS):
            return True
    except OSError:
        return False


def get_weather(lat, lon):
    for attempt in (1, 2):   # one retry: yoda reaches the internet through luke
        try:
            return fetch_weather(lat, lon)
        except Exception as e:
            log(f"weather unavailable (Open-Meteo, attempt {attempt}): {type(e).__name__}: {e}")
            if attempt == 1:
                time.sleep(RETRY_SECONDS)
    return None


def render(args, lat, lon, place, cache):
    """Compose the picture into the workspace and record it. Returns the
    render record, or None on failure."""
    weather = get_weather(lat, lon)
    if weather is None:
        event(cache, event="render", ok=False, error="weather unavailable")
        return None
    try:
        state, image_file = todays_work(cache, args, weather)
        meta = {**state["meta"], "artist": state["artist"], "place": place}
        img = tv_canvas.compose(Image.open(image_file), meta, weather)
        out = Path.cwd() / OUTPUT
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_name(out.name + ".tmp")        # a publish may be reading `out`
        img.save(tmp, "JPEG", quality=90)
        tmp.replace(out)
    except Exception as e:
        log(f"could not prepare today's image: {type(e).__name__}: {e}")
        event(cache, event="render", ok=False, error=f"{type(e).__name__}: {e}")
        return None
    record = {
        "rendered_at": time.time(),
        "weather_time": weather["current"].get("time"),
        "artwork": {"id": state["objectID"], "title": state["meta"]["title"],
                    "artist": state["artist"], "translated": state["translated"]},
        "weather": {"temperature": weather["current"]["temperature_2m"],
                    "kind": tv_canvas.kind(weather["current"]["weather_code"])},
    }
    write_json(cache / "render.json", record)
    event(cache, event="render", ok=True, weather_time=record["weather_time"],
          artwork=record["artwork"]["id"], translated=record["artwork"]["translated"],
          temperature=record["weather"]["temperature"])
    return record


def answer(record, **extra):
    return {"ok": True, "shown": False,
            "artwork": (record or {}).get("artwork", {}),
            "weather": (record or {}).get("weather", {}), **extra}


def publish(args, cache, record, force):
    """Send the render to the TV if it is new to it. Returns (code, result)."""
    cache.mkdir(parents=True, exist_ok=True)
    with open(cache / "publish.lock", "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            return 0, answer(record, reason="a publish is already running")
        if record is None or not (Path.cwd() / OUTPUT).is_file():
            return 0, answer(None, reason="nothing rendered yet")
        done = read_json(cache / "published.json", {})
        if not force and done.get("rendered_at") == record["rendered_at"]:
            return 0, answer(record, reason="the TV is up to date")
        if not args["wake"] and not force and not tv_awake(os.environ["FUNES_TV_HOST"]):
            return 0, answer(record, reason="TV asleep or off; it will be sent when it is on")

        argv = ["--action=show", f"--image={OUTPUT}"]
        argv += ["--gentle"] if not args["wake"] else []
        argv += [] if args["wake"] else ["--no-wake"]
        code, shown = tv_art.perform(argv)
        if code == tv_art.EXIT_UNREACHABLE and not args["wake"]:
            return 0, answer(record, reason="TV asleep or off; it will be sent when it is on")
        if code != 0:
            event(cache, event="publish", ok=False, code=code)
            return code, None
        event(cache, event="publish", ok=True, shown=True, content_id=shown.get("content_id"),
              weather_time=record["weather_time"], requested=bool(args["wake"]))
        write_json(cache / "published.json",
                   {"rendered_at": record["rendered_at"], "content_id": shown.get("content_id")})
        return 0, answer(record, shown=True, content_id=shown.get("content_id"))


def main(argv):
    try:
        args = parse(argv)
        lat, lon, place, cache = config()
    except Refusal as e:
        log(str(e))
        return 2
    except NotConfigured as e:
        log(f"not configured: {e}")
        return 3

    record = None
    if args["mode"] in ("render", "both"):
        record = render(args, lat, lon, place, cache)
        if record is None:
            return 1
        if args["mode"] == "render":
            print(json.dumps(answer(record, reason="rendered; publishing is a separate job"),
                             ensure_ascii=False))
            return 0
    else:
        record = read_json(cache / "render.json", None)

    # A person asking (both) always sends; the scheduled publish only what is new.
    code, result = publish(args, cache, record, force=args["mode"] == "both")
    if result is None:
        return code
    print(json.dumps(result, ensure_ascii=False))
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
