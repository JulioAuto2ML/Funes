# scriptlib/ — this extension's scripts for `run_script`

Loaded beside the core's library: `FUNES_SCRIPTS_DIR=<Funes>/scriptlib:<here>/scriptlib`.
Same rules as the core's (see `Funes/scriptlib/README.md`): one manifest plus one
executable per script, granted per agent under `scripts:`.

| Script | Agent | What |
|---|---|---|
| `tv_art` | `home` | Samsung Frame Art Mode: `status`, `show --image=<workspace path>`, `artmode_on`/`off`. JSON out. |
| `tv_weather_art` | `home` | The "art and weather" picture: a Met painting chosen by weather + season, with a forecast placard (drawn by `tv_canvas.py`). JSON out. |

## Setup (once per machine)

```bash
python3 -m venv scriptlib/.venv
scriptlib/.venv/bin/pip install -r scriptlib/requirements.txt
```

`tv_art.sh` runs `tv_art.py` with that venv and exits 3 with the command above
if it is missing. Then set `FUNES_TV_HOST` and `FUNES_TV_TOKEN_FILE` in Funes's
`config/funes.local` (see `config/funes.conf.example`) and accept the "Allow?"
prompt on the TV the first time.

## tv_art — what it decides for itself

- **The image path.** A script runs outside `fs_guard`, so `--image` is checked
  here: relative, inside the working directory (the caller's workspace), symlinks
  resolved, `.jpg`/`.jpeg`/`.png`, at most 20 MB.
- **Whether a change happened.** The set routinely applies a command and then
  times out answering it. Every change is read back; the JSON reports the state
  read back, not the reply.
- **What it deletes.** `show` deletes the previous image *it* uploaded (recorded
  in `<token file>.state.json`) unless `--keep`. It never deletes an id it did
  not upload, so the Art Store pieces and the owner's own photos are safe.

Exit 4 means the TV is off or unreachable and was not woken — `--no-wake`
(used by `tv_weather_art` on scheduled runs) skips Wake-on-LAN on purpose.

## tv_weather_art — what it decides for itself

- **One work per day.** The day's forecast and the southern-hemisphere season
  become Met searches (`spring rain`, `rain`, `spring landscape`…); candidates
  are shuffled with the date as seed, the first usable one wins, and it is
  cached in `<token dir>/tv_weather_art/`. Works shown in the last 60 days are
  skipped. `--refresh_art` picks another; `--query=Turner` searches instead.
- **Only works that look right on a 4K wall:** public domain, landscape
  (catalogue measurements first, then pixels), original ≥ 2500 px wide.
- **Spanish label or the original, never a broken one.** Title, bio and
  medium are translated once per work by the installation's LLM
  (`FUNES_LLM_URL`); a reply with the wrong keys, several lines or an odd
  length keeps the English, and the next run tries again.
- **It shares the LLM politely.** llama-server runs `--parallel 1`; a
  translation mid-newsletter would evict the curator's prompt cache. The
  script checks `/slots` first: scheduled runs translate only when idle (busy,
  or stopped during `run_images.sh`, keeps English and retries next run); a
  person's request waits up to 30 s.
- **A sleeping TV is a quiet `shown: false`**, exit 0. Scheduled runs never
  wake it; an empty living room should not fill the job log with FAILED.

Uses the Met's `/v1.1/search` (the `/v1/search` endpoint was retired on
2026-10-01) and Open-Meteo (credited on the placard).

Three events, two schedules (`--mode`): the day's pick and translation happen
once (first render of the day, ~6:02, before anything else runs; an untranslated
day retries at later renders, only while the LLM is idle); **render** every
15 min at :02/:17/:32/:47 (Open-Meteo updates on the quarter hour; builds the
picture, never touches the TV); **publish** every minute (a 2 s probe of the
set's port 8002 when something is pending, nothing otherwise; sends the newest
render once the TV answers, so turning it on shows data at most 15 min old).
Publishing passes tv_art `--gentle`: a set that answers with Art Mode off is
being watched, so the picture is only selected, not switched to. A person
asking (`--wake`, default `--mode=both`) renders and sends at once.

**What it logs.** A job row keeps only its *last* output, which for a
per-minute job is always "up to date", so the script keeps its own history:
`<token dir>/tv_weather_art/events.log` (on yoda `~/.funes-v4/tv_weather_art/`),
one line per render, publish or failure — nothing for a minute where nothing
happened. It rotates itself at 256 KB, keeping three older files (about two
months). Scheduled script runs write nothing else: no turns, no journal lines.

Schedule it from a chat, through the `home` agent: "programá el arte y clima
en el tele" → two `schedule_job(kind="script", script="tv_weather_art")`, with
`{"mode": "render"}` at `2,17,32,47 6-23 * * *` and `{"mode": "publish"}` at
`* 6-23 * * *` (the agent's prompt carries the exact calls).

Tests: `scriptlib/.venv/bin/python -m unittest discover -s scriptlib/tests`
(the TV, the Met, Open-Meteo and the LLM are stubbed; the canvas tests need
Pillow, hence the venv).
