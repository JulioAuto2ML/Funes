# Funes

**A personal assistant that learns and remembers.**

> *“I have more memories than all mankind since the world began.”*
> — Jorge Luis Borges, *Funes the Memorious*

Funes is a self-hosted AI assistant with one defining trait: **persistent memory**.
Tell it something today and it will know it tomorrow, next week, in a different
conversation. Memory is not a bolt-on — it is the product. Every answer starts by
recalling what Funes knows about you, and the web UI shows you exactly what it
remembers (and lets you delete any of it).

- **Fast C++17 backend, one binary.** The `funes` binary serves the web UI, the
  chat API, the agent runtime, and the memory engine. No Python, no Node, no
  containers, no external database.
- **Memory in a single file.** SQLite + [sqlite-vec](https://github.com/asg017/sqlite-vec)
  (vendored). Semantic search when an embedding model is available, keyword
  search when it isn't. Your entire memory is `~/.funes/memory.db` — back it up
  with `cp`.
- **Works with any LLM.** OpenAI-compatible endpoints (llama.cpp, Groq, OpenAI…)
  and Anthropic's native API, with token streaming. Fully local or cloud — your
  choice per config.
- **Tools without the burden.** Built-in tools (`web_search`, `web_fetch`,
  `remember`, `recall`) run in-process — no protocol overhead. External
  [MCP](https://modelcontextprotocol.io) servers can be plugged in when you
  want more.
- **Agents as YAML.** An agent is a name, a prompt, and a tool allowlist in
  `agents/*.yaml`. Ship your own in five lines.

![Funes UI](docs/screenshot.png)

---

## Quick start

**Requirements:** Linux, CMake ≥ 3.14, a C++17 compiler, `libssl-dev`, `libyaml-cpp-dev`.
Everything else (SQLite, sqlite-vec, HTTP, JSON, MCP) is vendored.

```bash
git clone https://github.com/Auto2ML/Funes.git
cd Funes
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**Point it at an LLM** — pick one:

```bash
# Option A: cloud (Anthropic) — create config/funes.local (gitignored):
cat > config/funes.local <<'EOF'
FUNES_LLM_URL=https://api.anthropic.com
FUNES_LLM_PROVIDER=anthropic
FUNES_LLM_MODEL=claude-haiku-4-5-20251001
FUNES_LLM_KEY=sk-ant-...
EOF

# Option B: fully local (llama.cpp) — the default config already points here:
llama-server -m qwen3-8b.gguf --port 8080 -c 8192
```

**Optional but recommended — semantic memory.** Any OpenAI-compatible
`/v1/embeddings` endpoint:

```bash
# local:
llama-server -m nomic-embed-text-v1.5.Q4_K_M.gguf --port 8081 --embedding
# or cloud (add to config/funes.local):
#   FUNES_EMBED_URL=https://api.openai.com
#   FUNES_EMBED_KEY=sk-...
#   FUNES_EMBED_MODEL=text-embedding-3-small
```

Without embeddings Funes still works — memory falls back to keyword search,
and missing vectors are backfilled automatically once an embedding endpoint
appears.

**Run it:**

```bash
./bin/funes
# → open http://localhost:8484
```

---

## How memory works

Funes has three layers of memory, all in one SQLite file:

1. **Automatic recall.** Before every answer, the most relevant memories are
   retrieved by vector similarity and injected into the system prompt. The UI
   shows a *“Funes remembered N things”* chip you can expand.
2. **Deliberate memory.** The model has `remember` and `recall` tools and is
   prompted to quietly store facts worth keeping (preferences, projects,
   decisions) and to search its past before saying “I don't know”.
3. **Conversation history.** Each chat session's turns are stored and reloaded,
   so a page refresh doesn't lose the thread.

Every memory is visible in the **memory panel**: source-tagged (`user` — you
taught it, `tool` — the model chose to keep it, `auto` — conversation log),
searchable, and deletable. You can also teach Funes facts directly from the
panel. Forgetting is a feature: the Borges story is a warning, after all.

---

## Configuration

Config is layered: shell env > `config/funes.local` (gitignored, secrets) >
`config/funes.conf` (committed defaults) > `~/.funes/config`.

| Variable | Default | Description |
|---|---|---|
| `FUNES_LLM_URL` | `http://localhost:8080` | LLM endpoint (OpenAI-compatible or Anthropic) |
| `FUNES_LLM_PROVIDER` | `openai` | `openai` \| `anthropic` |
| `FUNES_LLM_MODEL` | `default` | Model name |
| `FUNES_LLM_KEY` | *(empty)* | API key |
| `FUNES_EMBED_URL` | `http://localhost:8081` | Embeddings endpoint (unset → keyword memory) |
| `FUNES_EMBED_MODEL` | `default` | Embedding model name |
| `FUNES_EMBED_KEY` | *(empty)* | Embeddings API key |
| `FUNES_HOST` / `FUNES_PORT` | `127.0.0.1` / `8484` | Where the UI/API listens |
| `FUNES_DB` | `~/.funes/memory.db` | The memory file |
| `FUNES_DEFAULT_AGENT` | `funes` | Agent used when none is selected |
| `FUNES_MEMORY_RECALL_K` | `4` | Memories injected per answer |
| `FUNES_MEMORY_TURNS` | `10` | Past turns loaded per answer |
| `FUNES_AUTO_MEMORY` | `1` | Store each exchange as an `auto` memory |
| `FUNES_MCP_SERVERS` | *(empty)* | Extra MCP servers, `;`-separated URLs |
| `FUNES_ALLOW_LOCAL_FETCH` | `0` | Let `web_fetch` reach private/loopback hosts |

---

## Agents

```yaml
# agents/researcher.yaml
name: researcher
description: Digs into a topic on the web, then remembers what it learned.
tools: [web_search, web_fetch, remember, recall]
max_steps: 12
system_prompt: |
  You are a research assistant with persistent memory. ...
```

Drop a file in `agents/`, hit *reload* (`POST /api/agents/reload`), and it
appears in the UI's agent picker. Each agent has its own memory namespace.

To give an agent tools from an external MCP server:

```yaml
mcp_servers:
  - url: http://localhost:9000
    name: my-tools
```

---

## API

Everything the UI does is plain HTTP — script it if you like:

```
GET    /api/status                    health + model + memory stats
GET    /api/agents                    available agents
POST   /api/agents/reload             re-read agents/*.yaml
POST   /api/chat                      {agent?, session, message} → SSE stream
GET    /api/memories?agent=&q=        list / semantic search
POST   /api/memories                  {text, agent?} — teach a fact
DELETE /api/memories/<id>             forget
GET    /api/history?session=          a session's turns
```

The chat stream emits `memories`, `delta`, `tool_call`, `tool_result`,
`done`, and `error` SSE events.

---

## Tests

```bash
cd build && ctest --output-on-failure   # unit tests (memory, tools, config)
bash tests/integration.sh               # end-to-end against a mock LLM, no network
```

---

## Project structure

```
Funes/
├── agents/            # agent definitions (funes, researcher, yours…)
├── config/            # funes.conf (defaults) + funes.local (secrets, gitignored)
├── src/
│   ├── core/          # llm_client, memory, tools, agent runtime
│   │   └── tools/     # web_search, web_fetch, remember/recall
│   └── server/        # HTTP API + SSE + entry point
├── ui/                # web UI (vanilla JS — no build step)
├── tests/             # unit tests + mock-LLM integration test
└── third-party/       # vendored: sqlite, sqlite-vec, cpp-mcp (httplib, json)
```

## Lineage

Funes is the convergence of a series of experiments in giving LLMs memory and
tools: the original Python Funes (pgvector + Gradio, 2024), AgentOS/NeuralOS
(C++ agent runtimes), and [AresOS](https://github.com/Auto2ML) (an agent-native
OS layer for Linux, where most of this backend was born). This repo keeps the
useful 20% of all of that — an assistant that remembers — without the burden.

## License

MIT — see [LICENSE](LICENSE).
