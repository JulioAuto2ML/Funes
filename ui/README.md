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
- **Left panel**: conversations sidebar with previews and relative timestamps
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
