# Funes User Guide

> *"I have more memories than all mankind since the world began."*
> -- Jorge Luis Borges, *Funes the Memorious*

Funes is a self-hosted AI assistant with persistent memory. Tell it something
today and it knows it tomorrow, in a different conversation. This guide covers
everything you need to get it running and make the most of it.

---

## Table of contents

1. [Quick start](#quick-start)
2. [Configuration](#configuration)
3. [The web interface](#the-web-interface)
4. [Memory](#memory)
5. [Conversations](#conversations)
6. [Files, PDFs, and images](#files-pdfs-and-images)
7. [Agents and delegation](#agents-and-delegation)
8. [Creating your own agents](#creating-your-own-agents)
9. [Creating your own tools](#creating-your-own-tools)
10. [Scheduled jobs](#scheduled-jobs)
11. [The newsletter pipeline](#the-newsletter-pipeline)
12. [WhatsApp integration](#whatsapp-integration)
13. [Gmail integration](#gmail-integration)
14. [Shell access](#shell-access)
15. [Deployment](#deployment)

---

## Quick start

### Prerequisites

- C++17 compiler (GCC 9+ or Clang 10+)
- CMake 3.14+
- OpenSSL development headers
- yaml-cpp (`apt install libyaml-cpp-dev` or equivalent)
- An LLM backend: llama.cpp, Groq, OpenAI, or Anthropic

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run

```bash
# Point to your LLM backend
export FUNES_LLM_URL=http://localhost:8080

# Start Funes
./build/funes
```

Open `http://localhost:8484` in your browser.

### With a local model (llama.cpp)

```bash
# Terminal 1: start the LLM
llama-server -m model.gguf --port 8080

# Terminal 2 (optional): start the embedding model
llama-server -m nomic-embed-text.gguf --port 8081 --embedding

# Terminal 3: start Funes
export FUNES_LLM_URL=http://localhost:8080
export FUNES_EMBED_URL=http://localhost:8081  # optional, enables semantic memory
./build/funes
```

### With a cloud provider

```bash
# OpenAI
export FUNES_LLM_URL=https://api.openai.com
export FUNES_LLM_API_KEY=sk-...
export FUNES_LLM_MODEL=gpt-4o

# Or Anthropic
export FUNES_LLM_URL=https://api.anthropic.com
export FUNES_LLM_PROVIDER=anthropic
export FUNES_LLM_API_KEY=sk-ant-...
export FUNES_LLM_MODEL=claude-sonnet-4-20250514
```

---

## Configuration

Funes reads configuration from a layered chain of `KEY=value` files:

| Priority | File | Tracked |
|---|---|---|
| 1 (highest) | Shell environment | N/A |
| 2 | `config/funes.local` | No (gitignored) |
| 3 | `config/funes.conf` | Yes |
| 4 | `~/.funes/config` | No |

Put secrets (API keys, passwords) in `config/funes.local`. Put documented
defaults in `config/funes.conf`. The full reference with every setting is in
the comments of [config/funes.conf](../config/funes.conf).

### Key settings at a glance

| Variable | Default | What it does |
|---|---|---|
| `FUNES_LLM_URL` | `http://localhost:8080` | LLM endpoint |
| `FUNES_LLM_PROVIDER` | `openai` | `openai` or `anthropic` |
| `FUNES_LLM_MODEL` | `default` | Model name (auto-detected when `default`) |
| `FUNES_EMBED_URL` | (none) | Embedding endpoint for semantic memory |
| `FUNES_VISION_URL` | (none) | Vision model endpoint for image understanding |
| `FUNES_PORT` | `8484` | HTTP server port |
| `FUNES_DB_PATH` | `~/.funes/memory.db` | SQLite database |
| `FUNES_ALLOW_SHELL` | `0` | Enable shell command execution |
| `FUNES_TAVILY_API_KEY` | (none) | Web search API key |
| `FUNES_MEMORY_RECALL_K` | `6` | Memories injected per answer |

---

## The web interface

The UI is a single-page app with no build step or framework. It has three
panels:

- **Chat** (center): the main conversation. Messages stream in real-time via
  SSE. During a response, you can see tool calls and results as expandable
  "activity chips" below the assistant's message.

- **Memory** (right panel, toggle via "Memory" button): browse, search, add,
  and delete memories. Each memory shows its source (user, tool, auto,
  consolidated) with a colored badge.

- **Conversations** (left panel, toggle via "Chats" button): switch between
  conversations. Every conversation with at least one message appears here
  with a preview and timestamp.

The **context gauge** in the header shows how full the context window is:
green (ok), yellow (warn at 60%), red (danger at 85%). When it gets high,
Funes automatically compresses older turns into a summary.

The **status dot** blinks when the server is unreachable.

---

## Memory

Memory is Funes's defining feature. It works at three levels:

### Automatic memory

Every exchange is automatically memorized (a truncated version of what you
said and what Funes replied). These "auto" memories are the lowest priority --
they fill in gaps but are outranked by deliberate memories.

### Deliberate memory (tools)

When you tell Funes something worth keeping ("my birthday is March 15th",
"I prefer TypeScript over JavaScript"), it calls the `remember` tool to store
it. These "tool" memories get a 1.3x relevance boost over auto memories.

You can also teach facts directly via the Memory panel in the UI. These "user"
memories get the same 1.3x boost.

### Memory recall

Before every answer, Funes retrieves the most relevant memories via semantic
search (cosine similarity over embeddings) or keyword search (when no embedding
model is available). The number of memories injected is controlled by
`FUNES_MEMORY_RECALL_K` (default 6).

The recalled memories appear in the UI as a collapsible section above the
response.

### Memory consolidation

A background thread runs every 6 hours (configurable via
`FUNES_CONSOLIDATE_HOURS`) to:

1. **Merge near-duplicates**: memories with cosine similarity >= 0.92 are
   clustered and merged into a single sentence via an LLM call.
2. **Prune stale auto-memories**: `auto` memories that have never been recalled
   and are older than 30 days are deleted. Deliberately taught memories (`user`,
   `tool`) are never pruned.

### Your data

All memory lives in a single SQLite file (`~/.funes/memory.db` by default).
Back it up with `cp`. Delete a memory from the UI or the API. Delete everything
by removing the file.

---

## Conversations

Every conversation is a session identified by a random ID. Sessions persist
across browser reloads -- the UI stores the current session in `localStorage`
and the URL.

Switching sessions in the Conversations panel restores the full chat history.
Each session has its own rolling summary (for context compression) and turn
history.

---

## Files, PDFs, and images

Click the paperclip icon in the composer to upload a file:

- **Text files**: content is inlined into the message as a fenced code block.
- **PDFs**: text is extracted automatically via `pdftotext`. If the PDF is
  image-only (scanned), pages are rendered as images for a vision model.
- **Images** (PNG, JPEG, GIF, WebP): sent as multimodal content alongside your
  message text. Requires a vision-capable LLM backend or a configured
  `FUNES_VISION_URL`.

The `read_file` and `write_file` tools let Funes read and write files within
its workspace directory. These tools are confined -- they cannot access
anything outside the workspace. The workspace is `~/.funes/workspace/` by
default (set via `FUNES_WORKSPACE_DIR`).

---

## Agents and delegation

You only ever talk to **funes**, the orchestrator. When your request needs a
specialist, funes delegates via `delegate_to_agent` and relays the result in
its own voice. You don't pick an agent -- funes does.

### Available specialists

| Agent | What it does |
|---|---|
| `researcher` | Deep web research with source attribution |
| `operator` | Reads/writes files, runs shell commands, schedules jobs |
| `curator` | Publishes newsletter issues |
| `whatsapp-assistant` | Reads and sends WhatsApp messages |
| `gmail-assistant` | Searches, reads, and drafts Gmail |
| `rss-reader` | Reads RSS/Atom feeds |
| `astro-ph-summarizer` | Summarizes arXiv astrophysics papers |
| `agent-builder` | Creates new agents (the only one that can) |
| `agent-doctor` | Diagnoses and fixes broken agents |
| `tool-builder` | Scaffolds new HTTP tools |

### How delegation works

1. You ask funes something like "check my WhatsApp messages"
2. Funes calls `delegate_to_agent(agent: "whatsapp-assistant", task: "List recent messages")`
3. The specialist runs its own tool loop (invisible to you)
4. The result comes back to funes, which relays it in its own voice

The specialist has **no access** to the conversation -- it only sees the task
string funes writes. That's why funes includes all concrete details (phone
numbers, dates, names) in the delegation.

---

## Creating your own agents

Ask funes to create an agent. It will delegate to `agent-builder`, which
interviews you for details and then creates the YAML file. The new agent is
available immediately -- no restart needed.

You can also create agents manually:

```yaml
# agents/my-agent.yaml
name: my-agent
description: What it does
model: default
tools: [remember, recall, web_search]
max_steps: 8
context_limit: 8192
system_prompt: |
  You are a specialist that ...
```

Then reload: `POST /api/agents/reload` (or restart funes).

### Safety mechanisms you can configure

**Completion contract** -- tools that must succeed before an answer is accepted:
```yaml
require_tools: [publish_issue]
```

**Tool budget** -- per-tool call ceilings:
```yaml
tool_limits:
  web_search: 5
  web_fetch: 8
```

**Answer schema** -- enforce JSON shape on the final answer:
```yaml
answer_schema:
  type: object
  required: [summary, items]
  properties:
    summary: {type: string}
    items: {type: array, minItems: 1}
```

---

## Creating your own tools

Ask funes to create a tool. It will delegate to `tool-builder`, which
interviews you for the API endpoint details and scaffolds a C++ source file.

Tools are HTTP-call wrappers only -- one endpoint with argument placeholders.
After creation, you need to rebuild and restart:

```bash
cmake --build build
# restart funes
```

Secrets in tool headers use `${ENV_VAR}` syntax and are resolved from the
environment at call time -- never hardcoded in source.

---

## Scheduled jobs

The `operator` agent can schedule recurring tasks via cron expressions:

> "Schedule a daily reminder at 8pm to take my medication"

This creates a cron job that fires an agent task on the schedule. Two kinds:

- **Agent jobs**: delegate to a named agent with a task string
- **Shell jobs**: run a command directly (requires `FUNES_ALLOW_SHELL=1`)

View scheduled jobs in the Jobs panel of the UI, or ask funes to list them.

---

## The newsletter pipeline

Funes can produce and send a daily AI newsletter ("AI Pulse"). The pipeline:

1. **Harvest**: `harvest_candidates` searches for news, deduplicates, filters
   by recency, fetches every page, and builds a numbered candidate pool.
2. **Curate**: the `curator` agent picks the best stories and writes post text
   for each (under 280 characters).
3. **Publish**: `publish_issue` resolves candidate IDs to URLs, runs
   deterministic grounding checks (verifying each post's text against the
   actual page content), renders artifacts (HTML newsletter, text posts),
   checks all links, and sends via Gmail SMTP.
4. **Post**: `post_tweet.py` posts individual items to LinkedIn throughout
   the day, but only if the newsletter was actually sent.

### Adding a publication

Create two files:

```
publications/my-publication.yaml   # settings (queries, recipients, etc.)
publications/voice/my-publication.md   # editorial voice (prose)
```

No code changes needed. See [publications/README.md](../publications/README.md)
for the full field reference.

---

## WhatsApp integration

Two modes, two phone numbers:

### Interactive (whatsapp-assistant)

Ask funes: "send a WhatsApp to Mom saying I'll be late". Funes delegates to
`whatsapp-assistant`, which uses MCP tools backed by a WhatsApp Web bridge.

### Automated (whatsapp-autoresponder)

A separate service (`scripts/whatsapp_autoresponder.py`) polls for incoming
messages from whitelisted contacts on a dedicated phone number and replies
automatically using the `whatsapp-autoresponder` agent.

Manage the whitelist with `scripts/whatsapp_whitelist.py`:

```bash
python3 scripts/whatsapp_whitelist.py list
python3 scripts/whatsapp_whitelist.py add "Mom"
python3 scripts/whatsapp_whitelist.py remove "Mom"
```

The autoresponder shares funes's memory pool, so it knows everything you've
told funes.

---

## Gmail integration

Ask funes to search, read, or draft emails. It delegates to `gmail-assistant`,
which uses IMAP/SMTP via an MCP server.

**Gmail-assistant can search, read, and create drafts. It cannot send or
delete.** Every draft sits in your Drafts folder for you to review and send.

Setup: create a Gmail App Password and set `GMAIL_ADDRESS` and
`GMAIL_APP_PASSWORD` in `config/funes.local`.

---

## Shell access

The `execute_shell` tool is **disabled by default**. To enable:

```bash
export FUNES_ALLOW_SHELL=1
```

When enabled, the `operator` agent can run arbitrary shell commands within the
workspace directory. Commands have a hard timeout (default 20s, max 120s) and
output is capped at 16 KB.

Shell access also enables shell-type cron jobs.

---

## Deployment

### As a systemd service

```ini
# ~/.config/systemd/user/funes.service
[Unit]
Description=Funes AI Assistant
After=network.target

[Service]
ExecStart=/path/to/build/funes
WorkingDirectory=/path/to/Funes
EnvironmentFile=/path/to/Funes/config/funes.local
Restart=on-failure

[Install]
WantedBy=default.target
```

```bash
systemctl --user enable --now funes.service
```

### Updating

```bash
cd /path/to/Funes
git pull
cmake --build build -j$(nproc)
systemctl --user restart funes.service
```

Never `scp` or hand-edit files on the server -- always deploy via `git pull`
to avoid drift between the repo and what's running.

### Remote access

Funes binds to `0.0.0.0` by default. For remote access, put it behind a
reverse proxy (nginx, Caddy) with TLS. The chat endpoint uses SSE (chunked
HTTP), so make sure your proxy doesn't buffer responses.
