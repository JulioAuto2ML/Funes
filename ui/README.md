# ui/

The web frontend. Three files, no build step, no dependencies, no framework.
Served as static files by the C++ server at `/*`.

## Files

| File | Purpose |
|---|---|
| `index.html` | Single-page app shell |
| `app.js` | All client logic (~743 lines) |
| `style.css` | All styling |

## Design

Dark theme ("ink & parchment"):
- Near-black background (`#0e1013`), gold accent (`#d4a24e`), parchment text (`#e6e1d6`)
- Serif headings (Georgia), sans-serif body, monospace code blocks
- Chat bubbles with rise animation
- Elephant emoji favicon (Funes the Memorious)

## Layout

- **Header**: brand, controls (New chat, Chats, Memory, Jobs), context gauge, status dot
- **Left panel**: conversations sidebar with previews and relative timestamps.
  Scheduled runs are excluded server-side (`/api/sessions` hides `cron-*`);
  they are transcripts nobody held, and were 18% of the list on the deployment
- **User chip**: name, and on hover the account's resolved permissions --
  which agents it may reach and which tools are denied. The tooltip rather
  than a panel on purpose: the chip is already where "who am I" lives, so this
  needed no new element and no CSS, and the question ("why is that agent
  missing?") is one people ask rarely and answer once
- **Center**: chat pane with message history and composer (text + file attach)
- **Right panel**: memory sidebar (add/search/list) or jobs sidebar

Responsive: at < 860px, sidebars become fixed-position overlays.

## Chat features

- **SSE streaming**: handles all server events (delta, tool_call, tool_result,
  memories, context_compressed, usage, etc.)
- **Inline markdown**: code blocks (fenced + inline), bold, italic, links --
  custom renderer, no library
- **File upload**: text files inlined as fenced blocks, images sent as
  multimodal content with thumbnail previews
- **Context gauge**: three-state indicator (ok/warn/danger) showing how full
  the context window is
- **Activity chips**: expandable elements showing tool calls and results
  during streaming
- **Auto-growing textarea**: Enter to send, Shift+Enter for newline

## Memory pane

- CRUD operations for long-term memories
- Search with debounce
- Source badges colored by origin: user (gold), tool (green), auto (neutral),
  consolidated (neutral)
- Hover-to-reveal forget button

## Jobs pane

- Lists scheduled cron jobs with kind/status badges
- Relative timestamps for next/last run

## Server communication

All via `fetch()` to `/api/*` endpoints. Chat uses manual SSE line parsing
of the chunked response. No WebSockets, no polling (except periodic status
dot check). Session persistence via URL parameter or localStorage.

## Localization (5.0)

`i18n/<locale>.json` holds one flat key→string table per language; `en.json` is
the fallback and the only one that has to be complete. `t(key, vars)` looks up
the active table, then English, then returns the key itself — so a missing
translation shows English and a missing key shows the key, rather than an empty
button. `{name}` placeholders are the only substitution; a string needing more
than that is a string that should have been two.

Static markup carries `data-i18n`, `data-i18n-title` and
`data-i18n-placeholder`; `applyTranslations()` walks them in one idempotent
pass, so changing language re-renders without a reload. Strings built in JS
call `t()` directly.

Which language: the account's stored `locale` if it has one, otherwise
`navigator.language`, which is then persisted once so the agent's reply
language matches the UI. A browser default never overrides a deliberate
choice — otherwise opening the app on a different machine would silently
change it back.

Adding a language is one file plus one `<option>` in `index.html`. Keeping the
tables honest is a two-line check, since there is no extraction tool:

```bash
python3 -c "
import json,re,glob
en=set(json.load(open('ui/i18n/en.json')))
for f in glob.glob('ui/i18n/*.json'):
    print(f, sorted(en - set(json.load(open(f)))) or 'complete')"
```
