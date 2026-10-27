# scriptlib/ — this extension's scripts for `run_script`

Loaded beside the core's library: `FUNES_SCRIPTS_DIR=<Funes>/scriptlib:<here>/scriptlib`.
Same rules as the core's (see `Funes/scriptlib/README.md`): one manifest plus one
executable per script, granted per agent under `scripts:`.

| Script | Agent | What |
|---|---|---|
| `tv_art` | `home` | Samsung Frame Art Mode: `status`, `show --image=<workspace path>`, `artmode_on`/`off`. JSON out. |
| `tv_weather_art` | `home` | The "art and weather" picture: a Met painting chosen by weather + season, with a forecast placard (drawn by `tv_canvas.py`). JSON out. |
| `speak` | — (not granted yet) | Say a sentence on the living-room Echo over Bluetooth (`espeak-ng` → `paplay`). Quiet hours, reconnects the Echo itself. JSON out. |
| `news_headlines` | `noticiero` | Last 18 h from Infobae, La Nación and Clarín, same story across outlets merged, ranked by how many outlets cover it. No arguments. JSON out. |
| `publish_briefing` | `noticiero` | Writes the spoken briefing as the Echo's Alexa news feed (`feed.json`, served by `tailscale funnel`). Refuses URLs, markdown, cut-off text. JSON out. |

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

## speak — what it decides for itself

Needs `espeak-ng` on the host and the Echo paired over Bluetooth; stdlib Python,
no venv. Set `FUNES_ECHO_MAC` (the audio address) in `config/funes.local`.

- **What it says.** Control characters are dropped; empty text and text over 600
  characters are refused, not truncated. The text follows `--`, so it can never
  become an `espeak-ng` option.
- **When it is silent.** `FUNES_SPEAK_QUIET` (default 23:00-07:00) refuses with
  exit 5. There is deliberately no "urgent" flag a model could reach for.
- **Whether the Echo is there.** The Echo's PulseAudio sink exists only while the
  Bluetooth link is up. A missing sink gets one `connect`, then — once — an
  adapter power cycle and another `connect`. The power cycle is for a state hit on
  2026-10-22: the controller held a phantom ACL link to the Echo that BlueZ did
  not know about, so pair/connect answered "Already Connected" until the adapter
  was reset.
- **The first half-second.** A2DP takes a moment to open and the Echo swallows the
  start of the stream, so the audio is prefixed with 0.8 s of silence.

Exit 4 means the Echo is off, out of range, or no longer paired with this host.

## news_headlines + publish_briefing — the Echo's news briefing

The `noticiero` agent runs them once a morning (a cron job): `news_headlines`
gathers, the model writes about two minutes of spoken Spanish, and
`publish_briefing` publishes it as a one-item Alexa **news feed** (the
"Flash Briefing" JSON format). The Echo reads it when someone asks for the
news, through a news skill in development stage on the household's Amazon
account whose feed URL is `FUNES_NEWS_PUBLIC_URL`. Nothing is pushed: the Echo
speaks only when asked, so no third party sees the text and no quota applies.

What the code decides rather than the model:

- **news_headlines — the editorial ranking.** Same story from different outlets
  is one item (content-word overlap of the titles; never merged within one
  outlet), ranked by how many outlets cover it, then by recency. Items without a
  date, older than 18 h, or in a skipped category (horoscopes, recipes, games…)
  are dropped. A feed that fails is reported in `errors`; the others are used.
  The feed list is `FEEDS` in the script — an admin edit, not a model argument.
  Under-merging is the safe failure: the model sees one story twice; an
  over-merge would fuse two stories into one.
- **publish_briefing — what reaches the speaker.** Alexa reads `mainText`
  verbatim, so URLs (spelled out), markdown, tags and text that stops
  mid-sentence are refused with the reason; 300–4000 characters. The feed holds
  exactly one item, replaced on every publish, written to a temp file and
  renamed into place. `FUNES_NEWS_FEED_DIR` must already exist and lie outside
  every workspace, so no agent file tool can edit what the Echo reads.

Setup on the host: create the directory, then
`tailscale funnel --bg --set-path /noticias <FUNES_NEWS_FEED_DIR>` (needs
Funnel enabled for the tailnet and `tailscale set --operator=$USER` once).

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

Ideas not built yet (mates, slideshows, ComfyUI picture of the day, special dates…): `docs/tv-ideas.md`.

Tests: `scriptlib/.venv/bin/python -m unittest discover -s scriptlib/tests`
(the TV, the Met, Open-Meteo and the LLM are stubbed; the canvas tests need
Pillow, hence the venv).
