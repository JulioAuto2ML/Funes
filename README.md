# funes-julio

Julio's own agents, tools, pipelines and deployment for [Funes](../Funes) —
everything in the Funes tree that had a personal path, a personal identity or
a single consumer. Funes is the harness; this is one operator's use of it.
Split out in Funes 5.1 so the core stops being 19% one person's habits and so
this repository can hold what only makes sense on one machine: a newsletter
with subscribers, a WhatsApp number, a Gmail mailbox, a manuscript.

**This repository does not run on its own, and is not meant to.** It has no
`main()`, no UI and none of the seven general-purpose agents: it is a set of
tools, agents and scripts that compile into the Funes binary and load
alongside the core's own. Cloning it without Funes gets you source files with
nothing to link against. That asymmetry is the point of the split — the core
is complete without this, and this is deliberately incomplete without the
core.

Built into the `funes` binary as an *extension* (see `src/core/extension.h`
in Funes):

```bash
cd ../Funes
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUNES_EXTENSIONS=$PWD/../funes-julio
cmake --build build -j$(nproc)
cd build && ctest            # core tests + this repo's, prefixed julio_
```

and run with both agent directories and both script libraries:

```bash
FUNES_AGENTS_DIR=./agents:../funes-julio/agents \
FUNES_SCRIPTS_DIR=./scriptlib:../funes-julio/scriptlib ./bin/funes
```

Settings this extension reads are listed in `config/funes.conf.example`; they
go in Funes's `config/funes.local`. Nothing here is read by the core.

## Layout

```
funes-julio/
├── extension.cmake     # what the core includes: adds src/*.cpp to the funes target, tests/
├── src/                # native tools: harvest_candidates, publish_issue,
│   │                   # write_structured/read_structured, merge_rankings,
│   │                   # + publication/pipeline config loaders; register.cpp plugs them in
├── agents/             # curator, voc-researcher, council-chair/-panelist, content-writer,
│                       # mvp-builder, whatsapp-assistant/-autoresponder, gmail-assistant,
│                       # rss-reader, book-editor, home, noticiero (+ templates/)
├── scriptlib/          # scripts for run_script: tv_art, tv_weather_art (Samsung Frame),
│                       # speak, news_headlines + publish_briefing (Echo news feed)
├── publications/       # one YAML + one voice file per publication (ai-pulse, voc-insights)
├── pipelines/          # voc.yaml — stage dirs, filenames, schemas for the VoC chain
├── publishing/         # Python: render, link-check, send, post an issue (+ its tests)
├── scripts/            # WhatsApp autoresponder + whitelist, systemd units, benchmark
├── config/             # funes.conf.example — the variables this extension reads
├── tests/              # C++ tests for src/, run under the core's ctest
└── third-party/        # whatsapp-mcp (Go bridge), imap-email-mcp-patched (Node) — the MCP
                        # servers the WhatsApp and Gmail agents spawn (moved from Funes)
```

## How an extension plugs in

`src/register.cpp` has one static initializer that queues a function; the core
runs it after its built-in tools with `{tools, memory, workspace_dir}`. The
function resolves this repository's own directories (`FUNES_PUBLICATIONS_DIR`
etc., defaulting to this checkout — `extension.cmake` bakes the path in) and
calls the four `register_*` functions. `harvest_candidates` registers with
`inline_result = true`: its output is a numbered menu the model must see whole,
and the core's result store honours that flag instead of knowing the tool's
name.

The sources include core headers by bare name (`tools.h`, `memory.h`,
`fs_guard.h`); `extension.cmake` adds `src/core` and `src/core/tools` to the
include path. They are sources of the *executable*, not of `funes_core`, for
the reason the core's generated HTTP tools are: a static library drops an
object file nothing references, and nothing references a self-registering one.

## Deployment notes (from the split)

- Funes 5.0.1 stopped children inheriting the server's environment. The
  publishing scripts read credentials from `<publish dir>/.env` first and from
  the environment second; after the split only `.env` reaches them unless the
  variable is named in `FUNES_CHILD_ENV`. Put SMTP/LinkedIn credentials in
  `X_posts/.env`.
- `FUNES_ALLOW_SHELL` now defaults to `0` in the core's `funes.conf`. `operator`
  and `mvp-builder` want it on: set it in `funes.local`.
- Session tokens are hashed at rest since 5.0.1; every browser signs in once
  more after the upgrade. The WhatsApp autoresponder is unaffected (service
  token).
- `min_count` in `src/publication.h` is back to 8, below the default `count`
  of 10. Funes `fb54ffa` had raised it to 10 to get more items into an issue,
  but `publications/ai-pulse.yaml` sets `min_count: 6` explicitly, so the
  default never applied to the one publication that runs; what it did change is
  a bare config, where `min_count == count` refuses to publish unless the
  harvest fills every slot.
- The MCP servers the `gmail-assistant` and `whatsapp-assistant` agents spawn
  live in this checkout, and their YAML names them as
  `${FUNES_JULIO_DIR}/third-party/...`. That variable is published at startup
  by `src/register.cpp` from the path `extension.cmake` baked into the binary,
  so nothing has to be configured per machine — but it also means **the binary
  must be rebuilt after this repository moves**, not just after it changes.
  `funes.local` can set `FUNES_JULIO_DIR` to override.
- Their dependencies are not in git and do not move themselves:
  `third-party/imap-email-mcp-patched/node_modules` (`npm install` there, which
  applies the SNI patch) and `third-party/whatsapp-mcp/whatsapp-mcp-server/.venv`
  (`uv sync`), and `scriptlib/.venv` (`python3 -m venv scriptlib/.venv &&
  scriptlib/.venv/bin/pip install -r scriptlib/requirements.txt`). On an upgrade from a pre-5.1 tree, move the existing ones over
  from the Funes checkout rather than re-fetching.
- Two systemd units name paths that the split moved; both are copies under
  `~/.config/systemd/user/`, so editing the files here changes nothing until
  they are copied over and `systemctl --user daemon-reload` is run:
  `funes-v4.service` needs `FUNES_AGENTS_DIR=<Funes>/agents:<here>/agents`
  (and, for `tv_art`, `FUNES_SCRIPTS_DIR=<Funes>/scriptlib:<here>/scriptlib`) —
  without the second entry all eleven agents in this repository are missing and
  both cron jobs fail to resolve their agent — and
  `whatsapp-autoresponder-v4.service` runs `scripts/whatsapp_autoresponder.py`,
  which is now in this repository (its `WorkingDirectory` stays the Funes
  checkout, which is what makes the script find `config/funes.local`).

## 4.2 — per-user publications

**Why now.** The deferred items in 4.2 were held back for want of real demand.
This one acquired it on 2026-09-14: a second account exists (`daniela`), so
"any user can publish their own newsletter" stopped being hypothetical. The
other two (channel adapter, `mcp-builder`) still have no second consumer and
stay deferred.

**The goal.** `curator` loses its absolute `workspace_dir`
(`/home/julio/Documents/X_posts`) and resolves per-user like every other
agent, so publishing is something an account does rather than something the
installation does.

**What must not happen on the way.** Moving that directory wholesale into the
user workspace is the obvious version of this change and it is wrong twice:

1. **It would publish the credentials.** The directory holds `.env` (Gmail +
   LinkedIn) and `subscribers.txt`. Today they sit outside every workspace, and
   `curator` has no `read_file` — nothing the model drives can reach them.
   Inside `<workspace>/<user_id>/`, they land in the confinement root of every
   agent that account runs, and `funes`, `operator`, `file-reviewer` and
   `book-editor` all have `read_file`. "Read x_posts/.env" would print live
   credentials into a chat transcript, and `/api/upload` and the files pane
   would reach them too.
2. **It would split the publication.** `publications/ai-pulse.yaml` is
   installation-global, and `dedup_against_last_issues` reads the *caller's*
   `issues/` directory. Two accounts could each publish `ai-pulse` on the same
   morning, with independent dedup histories, to one subscriber list. The
   absolute path is currently what prevents that; removing it without settling
   ownership first replaces isolation with a split brain that a subscriber
   discovers, not a test.

**Order of work.**

| Step | Deliverable |
|------|-------------|
| 1 | Decide ownership: a publication belongs to a user (an `owner` in the YAML, or per-user publication configs) rather than to the install. Everything else follows from this. |
| 2 | Split the publish directory by what each thing *is*: working files (harvest pools, issue JSON, run records, rendered artifacts) are per-user and model-visible; `.env` and `subscribers.txt` are publication-owned and resolved by the **tool**, never by a path the model can name — the same rule `publications/*.yaml` and `funes.local` already follow. |
| 3 | Per-user sending identity. A second publisher either brings their own SMTP/LinkedIn credentials or sends from the owner's — a decision, not a default. |
| 4 | Drop `workspace_dir` from `curator.yaml`. Smallest step, and last. |
| 5 | Update `run_publication.sh` and the LinkedIn cron, which read `runs/<pub>/<date>.json` at a fixed path and will need to know whose. A live cron that publishes real things. |

**A category this exposed, worth writing down.** Funes isolates its own data —
memories, turns, stored results, workspace files — in SQL and in `fs_guard`.
It cannot isolate a *third-party account* reached through an MCP server with
installation-wide credentials. `gmail-assistant` (`IMAP_USER` in
`funes.local`, one mailbox) and `whatsapp-assistant` (one bridge store, one
phone number) are both in that class: granting either to a second account
hands over the first account's inbox or chats, however well-isolated Funes's
own storage is. Per-user credentials for those is its own piece of work, not
part of this one. Until then they stay off a member's agent allowlist —
`daniela`'s is `funes, researcher, operator, rss-reader, file-reviewer,
book-editor`.

Note `--agents` is an allowlist with no deny form, so that list freezes at
today's roster: a newly added agent will not reach a restricted account until
somebody grants it. That is the safer default for a member account, but it is
a maintenance cost, not an accident.

## Agents: the full hub-and-spoke (with this extension loaded)

```
User <-> funes (orchestrator)
              |
              +-> researcher         (deep web research)
              +-> operator           (workspace, shell, cron)
              +-> curator            (newsletter pipeline)
              +-> whatsapp-assistant (WhatsApp via MCP)
              +-> gmail-assistant    (Gmail via MCP)
              +-> rss-reader         (RSS via MCP)
              +-> book-editor        (manuscript editing)
              +-> file-reviewer      (uploaded documents)
              +-> home               (living-room TV, Art Mode, Echo)
              +-> noticiero          (Echo's morning news feed)
              +-> agent-builder      (create new agents)
              +-> agent-doctor       (diagnose/fix agents)
              +-> tool-builder       (scaffold new tools)
              +-> voc-researcher     (VoC pipeline entry point)
                    |
                    +-> council-chair
                    |     +-> council-panelist  x3 (builder/buyer/critic)
                    +-> content-writer
                    +-> mvp-builder
```

The VoC chain is a *pipeline*, not a conversation: each stage hands the next
one a file, and where those files go is `pipelines/voc.yaml`, not a paragraph
in five prompts. See [pipelines/README.md](pipelines/README.md).

The user talks only to `funes`. When a request needs a specialist, funes
delegates via `delegate_to_agent(agent, task)`. The task string must be
entirely self-contained -- the specialist sees only it, not the conversation.

Delegation carries identity: the specialist runs as the delegating user and
shares its session, so anything it recalls, remembers or stores belongs to the
same account the caller is acting for.

**Special cases:**

- `whatsapp-autoresponder` is never invoked by funes. It is called exclusively
  by `scripts/whatsapp_autoresponder.py`, which polls for incoming messages and
  sends the agent's reply text back via the bridge API. The sending number has
  to be mapped to an account (`funes jid-map <jid> <username>`) or Funes
  refuses the call -- identity comes from that mapping, never from the script.
- `agent-builder` is the only agent with `create_agent`. Never delegate
  agent-creation to `operator` -- it can write files but cannot register them.
- `curator` runs 30+ tool calls per newsletter issue. Funes must never attempt
  to do the same work in parallel.
- `gmail-assistant`, `whatsapp-assistant` and `curator` declare
  `shared_identity`. Their tools authenticate as the installation — one IMAP
  account, one phone number, one subscriber list — so granting one to a second
  account hands over the first account's data, however well Funes isolates its
  own. The field grants and restricts nothing by itself (the agent allowlist
  still decides); it changes what a caller is *told* when the agent is
  unavailable, because "ask an administrator" is good advice for an allowlist
  and bad advice for this. See `src/core/agent_roster.h`.
- `council-panelist` has no tools at all. That is the point: three independent
  opinions are only independent if none of them can look at what the others
  saw. Its `answer_schema` is what makes its reply mergeable by a tool.



## Why the newsletter looks the way it does

This is the incident write-up that used to live in the Funes README under
"Not asking the model for what a tool can supply". The principle stayed in the
core; the example moved here with the code it is about.

### Not asking the model for what a tool can supply

The three features above make a model finish, stop and answer correctly. The
cheapest way to stop it getting a step wrong is not to ask it, and the
`curator` agent is what that looks like carried all the way.

The daily newsletter used to be three agents and 46 steps, most of them a
language model reading instructions about how to perform a deterministic task.
One of those steps was retyping ten URLs it had read earlier, out of a
transcript that had since been compressed and stored by reference. On
2026-07-31 an item about an OpenAI breach shipped pointing at a Stripe checkout
page — which returned 200, so the link checker passed it. That is arithmetic,
not carelessness: ask for ten transcriptions a day and one comes out wrong every
few weeks, forever.

So `harvest_candidates` runs the searches, drops duplicates, stale items and
anything that already ran this week, fetches every survivor (a failed fetch
drops the candidate *there*, which is the link check moved before selection
instead of after writing), and returns a numbered pool. The model picks by
number. `publish_issue` resolves those numbers back to URLs it never showed the
model a reason to retype, and renders, re-checks and sends in one call whose
exit code is what makes "it was sent" a fact.

The one thing a model cannot be relieved of is whether an item is worth running
and whether the post is true. The second half of that is checkable, so it is
checked: each item carries an `evidence` quote copied off the candidate's page,
verified against the text the harvester actually fetched. A post about a breach
cannot produce a supporting quote from a checkout page. A failed check names one
item and sends nothing.

Three agents and 46 steps became one agent and about four.

The same split decides what is configuration. A publication is one YAML in
`publications/` — queries, recency window, caps, artifacts, channels — plus a
prose voice file, because voice is the one part a model has to read rather than
obey. The agent never sees the config; it is handed a pool and a voice note by
the tools, which is why a second publication needs no second agent. Every run
leaves a record in `runs/<publication>/<date>.json`, and both the scheduler and
the LinkedIn cron read it rather than believing anything the model said.

#### The same fix, generalized (4.1)

Knowing the rule and applying it are different things. The voice-of-customer
chain — research a problem, debate it from three perspectives, write the
article, scaffold a prototype — was built *after* the newsletter rewrite and
repeated two of the mistakes it had just fixed.

Its stages hand work to each other as files, and each of five system prompts
carried the convention as prose: "write the topic as JSON to
`topics/<date>-<slug>.json` with these six fields." Nothing enforced it. A slug
that drifted by a character, a field the model dropped, an object where an
array belonged — each produces a file that exists, so the stage reports success
and the next stage finds nothing where it looked. You learn at the end of a
five-step pipeline, from an empty article.

And `council-chair`'s prompt asked the model to merge three panelists' rankings
by *computing a Borda count in its head* — "for N proposals, rank 1 gets N
points, rank 2 gets N-1, sum across all three" — while holding three JSON
replies in an 8K context.

So: a pipeline is now a YAML in `pipelines/` naming each stage's directory,
filename template and JSON schema. `write_structured` takes a pipeline, a stage,
a slug and content; it derives the path, fills the date, and validates against
the stage schema — and a violation is refused with the offending field named and
**nothing written**, because a malformed file that exists is worse than no file:
the next stage parses it and believes it. `read_structured` answers "has this
been done already?" and restates the shape on request. `merge_rankings` does the
Borda count in C++, matching proposals three independent panelists inevitably
named three different ways.

The slug is sanitized rather than validated, which is the part that matters
more than the schema: `"Local AI Compliance!"` and `local-ai-compliance` are the
same entry, so drift stops being possible instead of being caught.

Five agents lost their file-convention paragraphs and their arithmetic. None of
them lost a judgement — which topic is worth debating, which proposal wins, what
the article says, whether the prototype runs. That is the line.


`agents/gmail-assistant.yaml` is a second example, wired to
[imap-email-mcp](https://www.npmjs.com/package/imap-email-mcp) over IMAP/SMTP
rather than the Gmail API — no OAuth app to register, it authenticates with a
plain Gmail App Password. Its `tools:` list only allows the read/search/draft
tools (`search_emails`, `list_emails`, `get_email`, `list_folders`,
`list_drafts`, `get_draft`, `create_draft`, `update_draft`); the server also
exposes `send_email` and `delete_email`, but leaving them out of `tools:` is
enough to keep the agent from ever calling them — Funes filters the tool
schema handed to the model down to that list (`agent.cpp`'s
`tools_.openai_schema(cfg_.tools)`), so unlisted tools are invisible to it,
not just discouraged. Note what's *not* in the yaml's `env:` block: only
`IMAP_HOST` is set there, because it isn't secret. `IMAP_USER` and
`IMAP_PASSWORD` are deliberately absent from the file — a stdio server's
subprocess inherits Funes' own process environment (that's how
`FUNES_MCP_SERVERS` reaches agents too), so the credentials just need to live
in `config/funes.local` (same Gmail address and App Password
`publishing/send_newsletter.py` uses — see the Email section in
`config/funes.conf`). Never put a real secret in an agent yaml's `env:` map —
anything written there is committed to the repo in plain text.

The `command:` doesn't run `npx -y imap-email-mcp` directly, though — it
points at `third-party/imap-email-mcp-patched/node_modules/imap-email-mcp/index.js`,
a pinned local install. The published package's `node-imap` dependency never
sets the TLS SNI `servername` when connecting, and Gmail's IMAP frontend
responds to a connection with no SNI by handing back a fallback certificate
that's genuinely self-signed — every request would fail with `Error:
self-signed certificate` otherwise. `third-party/imap-email-mcp-patched/`
pins the exact version and carries a one-line patch (via
[patch-package](https://www.npmjs.com/package/patch-package), see
`patches/imap+0.8.19.patch`) that sets `servername` explicitly. Run `npm
install` in that directory once per host (local dev and yoda both) before
this agent can connect — `patch-package` reapplies the patch automatically
on every install, so it survives a clean `node_modules` wipe.

`agents/whatsapp-assistant.yaml` is a third example, wired to
[whatsapp-mcp](https://github.com/lharries/whatsapp-mcp) — a personal
WhatsApp account connected the same way WhatsApp Web/Desktop links a device
(scan a QR code with the phone once), via the unofficial `whatsmeow`
protocol library. Unlike the other two, it's a two-process design vendored
in full under `third-party/whatsapp-mcp/`:

- **The Go bridge** (`whatsapp-bridge/`) holds the actual WhatsApp
  connection and writes incoming messages to a local SQLite store. It is
  **not** spawned by Funes — Funes only starts short-lived stdio
  subprocesses per request, but this bridge has to stay connected
  continuously to receive messages at all. Run it as its own systemd
  `--user` service; see `scripts/whatsapp-bridge.service` for the unit file
  and setup steps (including the one-time QR scan, and re-pairing roughly
  every 20 days when the session expires).
- **The Python MCP server** (`whatsapp-mcp-server/`) is the thin tool layer
  Funes actually spawns per request (`uv --directory
  third-party/whatsapp-mcp/whatsapp-mcp-server run main.py`) — it just reads
  the bridge's SQLite DB and calls its local REST API.

Locally patched, three times. The bridge's REST API was hardcoded to port
8080, which collides with yoda's `llama-server` (`FUNES_LLM_URL`); both
`whatsapp-bridge/main.go` and `whatsapp-mcp-server/whatsapp.py` were edited
to use 8090 instead. Separately, the vendored `whatsmeow` dependency was
~17 months stale as of first deploy and got rejected outright ("Client
outdated (405)") — `go get -u go.mau.fi/whatsmeow@latest` fixed that but
changed several method signatures to take a leading `context.Context`
(`client.Download`, `sqlstore.New`, `container.GetFirstDevice`,
`client.GetGroupInfo`, `client.Store.Contacts.GetContact`), all updated in
`main.go` to pass `context.Background()`. Expect to need this again
periodically — it's an unofficial protocol implementation racing WhatsApp's
actual client version. The agent's `tools:` list deliberately excludes
`send_file`, `send_audio_message`, and `download_media` — it can search,
read, and send plain-text replies, nothing else. Because WhatsApp message
content is untrusted input the model reads directly, the system prompt
tells it explicitly not to treat message text as instructions.

**Two numbers, two bridge instances.** `whatsapp-assistant` acts as *you* —
it should stay on your own number. `whatsapp-autoresponder` talks back
autonomously to whoever's on the whitelist, which is a materially different
thing to hand your personal WhatsApp identity to, so it gets a second,
dedicated number instead. The third patch — `WHATSAPP_STORE_DIR` /
`WHATSAPP_BRIDGE_PORT` env vars read once at package init in `main.go` —
turns every hardcoded `"store"` path and the port literal into per-instance
config, so the same compiled binary runs twice with independent sessions and
SQLite stores: `whatsapp-bridge.service` (personal, port 8090, `store/`) and
`whatsapp-bridge-funes.service` (dedicated, port 8091, `store-funes/`, env
set in the unit file). Pairing the second instance needs an actual second
phone number you control — that's on you to provide, this only handles the
bridge process once you have one. `whatsapp-mcp-server` (whatsapp-assistant's
MCP layer) still only ever points at the personal instance;
`whatsapp_autoresponder.py` points at the dedicated one via
`WHATSAPP_DB_PATH`/`WHATSAPP_BRIDGE_URL` in `config/funes.conf`.

**Auto-replying to incoming messages** is a separate, opt-in layer on top of
the above — `agents/whatsapp-assistant.yaml` only answers when *you* ask
Funes (through its own UI) to check or send WhatsApp; it does nothing when a
message just arrives. `scripts/whatsapp_autoresponder.py` is a small
standalone poller (stdlib-only, no new dependency) that watches the
dedicated instance's SQLite store directly and, for messages from chats on
`WHATSAPP_WHITELIST` only, asks a dedicated `whatsapp-autoresponder` agent
for a reply and sends it back. Every other chat is silently ignored.
Manage the whitelist with `scripts/whatsapp_whitelist.py list/add/remove`
(matches by contact name, so you don't have to hand-look-up a `jid`) rather
than editing `config/funes.local` directly — see the WhatsApp section of
`config/funes.conf` for the commands. It's a plain local CLI, not something
exposed to any agent: changing who Funes will auto-reply to should stay a
step only you can take.

The autoresponder agent is deliberately more restricted than
`whatsapp-assistant`: its only tools are `recall`/`remember` (memory), no
`delegate_to_agent`, no MCP servers, and specifically **no send capability**
— it can only return text. `whatsapp_autoresponder.py` is the one thing that
actually calls the bridge's `/api/send`, and it always sends into the exact
chat the incoming message came from, for whitelisted chats only. That split
exists so the model can never pick who to send to; it only ever picks what
to say, and even that only reaches a chat that already passed the whitelist
check in plain Python before the model saw anything. Run it as its own
systemd `--user` service — see `scripts/whatsapp-autoresponder.service`.

That restriction is right for a contact and wrong for you. Funes has its own
number, so a message *you* send it reaches the poller looking exactly like a
stranger's, and got the same declawed agent: asked to go find an email it
answered — truthfully, for that agent — that it had no Gmail access, while
`agents/gmail-assistant.yaml` sat loaded one delegation away. So the agent is
now chosen by the sender's role, which 4.0 already resolves from their jid:
an **admin** gets `WHATSAPP_ADMIN_AGENT` (`funes` by default), the
orchestrator with `delegate_to_agent`; every other role keeps
`whatsapp-autoresponder` unchanged. Map your own number with
`funes jid-map <jid> <username>` to be recognized.

Routing is not permission, and this deliberately does not become one. The
jid is resolved server-side and every call still runs as that user, under
that user's own agent and tool allowlists — `Permissions::allows_agent`,
enforced in `/api/chat`, the delegation roster, `delegate_to_agent` and cron.
Pointing a member at `funes` would simply be refused, and narrowing what an
admin can reach over WhatsApp is a `funes user permissions` change, not a
routing one. What the poller decides is only *which agent is asked first*.

Attachments follow the same split, because a path is only meaningful relative
to the workspace its reader is confined to. `whatsapp-autoresponder` declares
`workspace_dir: whatsapp-uploads`, so the upload folder is its root and it
gets a bare `<chat>/<file>`; an admin agent declaring no `workspace_dir` is
rooted one level up at the user's workspace, so it gets
`whatsapp-uploads/<chat>/<file>`. Same file on disk, in the same per-user
workspace, spelled for whoever is allowed to read it.

It does, though, share `funes`'s own memory rather than starting a separate,
empty pool — via `memory_scope: funes` in its yaml. Every agent normally has
fully isolated memory (`recall`/`remember` are scoped by agent name — see
`AgentConfig::memory_scope` in `src/core/agent_config.h`); this is one field
away from opting out of that isolation for a specific agent, so a message
sent over WhatsApp and one sent through the web UI draw on and add to the
same facts, rather than the WhatsApp side starting from a blank slate.

**Starting a fresh conversation.** A contact can send `/new` to reset their
thread. This is handled entirely in `whatsapp_autoresponder.py` — the model
never sees it — and, like the web UI's own "New Chat" button (`ui/app.js`),
it rotates to a new session rather than deleting the old one's history
(`sanitize_session`'s `generation` suffix, tracked per `chat_jid` in the
poller's own state file). Note this only resets *conversation* history —
`recall`/`remember` long-term memory is shared with `funes` (see above) and
is unaffected either way. There's currently no way to actually delete a
session's history — through WhatsApp or the web UI — only start a new one;
the storage layer has the pieces (`MemoryStore::prune_turns`) but nothing
exposes them yet.

**Documents and photos over WhatsApp.** A whitelisted contact can send a
"document" attachment (PDF or plain text file) or a photo ("image"
attachment) and Funes will read it — audio/video are still ignored, that
would need transcription, a separate feature. `whatsapp_autoresponder.py`
downloads it via the bridge's `/api/download` (pre-checking the message's
known `file_length` against `WHATSAPP_MAX_MEDIA_BYTES` before downloading,
and re-checking the actual size after, so an oversized file is never handed
to the model), copies it into the *sender's own* workspace under a per-chat
subfolder, and tells the agent about it with a `[Document received: <path>]`
or `[Photo received: <path>]` marker in the message text. Which workspace
that is comes from Funes, not from the script: the poller asks which account
the number maps to and writes there, so identity is resolved in one place and
an unmapped number's attachments are ignored rather than written somewhere
nothing can read them. `whatsapp-autoresponder`'s only new tool for this is
`read_file`, scoped via `workspace_dir: whatsapp-uploads` in its yaml, which
resolves to `<workspace>/<user_id>/whatsapp-uploads/` — the same confinement
`read_file` gives every other agent (see `src/core/tools/fs_guard.h`), so it
can never reach anything outside that one folder, including another
contact's. `read_file` already knows
how to pull text out of a PDF (`src/core/tools/pdf_extract.cpp`, the same
code path the web UI's drag-and-drop upload uses) and to hand a PNG/JPEG/
GIF/WebP image back as multimodal content (`funes::detect_image_mime`,
`src/core/tools/file_tools.cpp`) for a vision-capable backend to read —
Funes' own deployment uses a Qwen model with an `--mmproj` file loaded for
this (see `FUNES_LLM_URL`/the llama-server setup); without a vision-capable
backend, images are silently ignored by the model the same way they'd be by
a text-only one. Anything read_file still can't handle (spreadsheets, Word
docs) comes back as a plain error the model is told to relay honestly
rather than bluff through. Uploads are deleted automatically once they're
older than `WHATSAPP_UPLOAD_MAX_AGE_DAYS` (default 30) — see the WhatsApp section
of `config/funes.conf`.

