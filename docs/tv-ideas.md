# TV ideas backlog

Written 2026-10-21, after `tv_art` and `tv_weather_art` shipped (see `scriptlib/README.md`). Nothing here is scheduled; each item waits until it is asked for.

## Facts any of these must respect
- Formats: JPEG/PNG only, ideal JPEG 3840×2160 sRGB, ~q90 (1–3 MB); `tv_art` caps 20 MB. Still images only in Art Mode (no GIF/video); vertical/square images get a mat. Muted tones, no big pure whites — "museum" images look real on the matte panel.
- The TV is the whole household's: anything from Funes's memory (recuerdos, agenda, noticias, cumpleaños) is **opt-in**, never default for the `home` agent.
- One GPU: anything using ComfyUI must go through the `run_images.sh` window (09:30 and 11:00, LLM stopped ~17 min), never on demand mid-chat. PIL/HTML-to-image work is CPU and can be instant.
- The three-event design (translate once, render every 15 min, publish when the TV answers) is described in `scriptlib/README.md`.

## Status
| # | Idea | Status |
|---|---|---|
| 5 | Curated public-domain gallery picked by season/weather/request | **Done** for the Met (tv_weather_art). Art Institute of Chicago and Rijksmuseum APIs not added. |
| 2 | Weather "painted" | **Partly**: a real painting chosen by forecast + placard. Generated landscapes (ComfyUI) not done. |
| 7 | Requests from WhatsApp ("poné algo tranquilo") | Works via funes → home for `query`/`refresh_art`; no mood→art mapping beyond a search string. |
| 1 | Mates (frames) + slideshow in `tv_art` | Pending. Two more params (matte style/colour per image; auto-rotate a collection every N min). Low effort. |
| 3 | ComfyUI "cuadro del día" (e.g. top newsletter story as oil/engraving, small museum-label caption; ~1 MP + upscaler to 4K in the same workflow) | Pending; most impressive, most work; fits `run_images.sh`'s window. |
| 4 | Time-of-day picture: 4 variants of one scene (dawn/noon/dusk/night) generated overnight and swapped by the publish job; pair with Art Mode colour temperature | Pending. |
| 5b | Non-screen info posters (PIL/HTML, no GPU): almanac (date, moon phase, phrase of the day), morning-summary poster (agenda, weather, a Funes line), "recuerdo de hoy" from Funes memory (opt-in) | Pending. |
| 6 | Guest mode: welcome screen with the guest-WiFi QR, then back to the usual picture | Pending; pairs with the IoT-network separation idea. |
| 8 | The owner's own projects on the wall: book chapter illustrations / cover candidates for a week | Pending. |
| 9 | Motion outside Art Mode: ffmpeg loops (Ken Burns, drifting clouds, particles) pushed by DLNA (port 9197 open). ComfyUI video models impractical on the shared 16 GB. | Pending / speculative. |
| 10 | Special days: River match-day poster (design directed by the owner), party plates | Pending; **absorbed by item 11 below**. |
| API | Art Mode brightness and colour temperature (warmer at night); apps/keys outside Art Mode | Unused API surface of `samsungtvws`; untested on this model. |

## 11. Pictures that depend on special dates (new, requested 2026-10-21)
Navidad, Halloween and birthdays (plus whatever else fits: Argentine holidays such as 25 de Mayo / 9 de Julio, River match days).

Where it hooks in: `scriptlib/tv_weather_art.py` already picks one work per day at the day's first render from `queries(kind, season)` and `season()`; the date → occasion check would sit next to them and take priority over weather/season when it matches (the pick is cached in `today.json`, so a special date just changes that one decision). The placard (`tv_canvas.compose`) would need an optional greeting line ("Feliz Navidad", "Feliz cumpleaños, <nombre>").

Open design points to settle before building:
- **Where the dates live.** A small YAML in `scriptlib/` (fixed dates like 25/12, 31/10) vs. Funes memory for birthdays. Birthdays and names are personal and the TV is shared: opt-in, and decide whether the placard names the person.
- **Window.** A few days around the date (Christmas: ~20–25 dic; Halloween: ~28–31 oct) or the day only.
- **What counts as "fitting art".** Met searches with public-domain paintings: Christmas → "nativity", "winter snow", "epiphany"; Halloween → "witches", "skeleton", "night"/Goya, Fuseli; birthdays → "festival", "feast", "garden party", or a generated card via ComfyUI (item 3) in the GPU window. Check results exist at ≥2500 px landscape (MIN_PX, RATIO) — special themes may need a looser fallback to a hand-picked list of object ids.
- **Priority** between weather, season, and occasion, and what a manual `query` does on a special day (should still win, a person asked).
- Tests in the same style as `tests/test_tv_weather_art.py` (date injected through `twa.today`).

**Suggested order:** 1 (mates/slideshow), then 11 (special dates; no GPU needed for the paintings), then 3 (ComfyUI cuadro del día).
