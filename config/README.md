# config/

Layered configuration loaded at startup. The C++ binary and the Python scripts
both read these files the same way: shell-sourceable `KEY=value` pairs, later
files override earlier ones.

## Precedence (highest to lowest)

1. Shell environment variables (e.g. `FUNES_LLM_URL=... ./funes`)
2. `config/funes.local` -- gitignored, machine-specific secrets and overrides
3. `config/funes.conf` -- version-controlled defaults
4. `~/.funes/config` -- user-level fallback

## Files

| File | Tracked | Purpose |
|---|---|---|
| `funes.conf` | Yes | Documented defaults for every setting. Read this to understand what is configurable. |
| `funes.local` | No (.gitignored) | API keys, passwords, machine-specific overrides. Never committed. |

## Key settings

### LLM backend
- `FUNES_LLM_URL` -- OpenAI-compatible endpoint (default: `http://yoda:8080`)
- `FUNES_LLM_PROVIDER` -- `openai` (llama.cpp, Groq, OpenAI) or `anthropic`
- `FUNES_LLM_MODEL` -- model name, or `default` (auto-detected from `/v1/models`)
- `FUNES_VISION_URL` -- separate endpoint for image-bearing turns

### Embeddings (semantic memory)
- `FUNES_EMBED_URL` -- embedding server (e.g. nomic-embed-text on llama.cpp)
- Falls back to keyword search when unavailable

### Memory
- `FUNES_DB_PATH` -- SQLite database path (default: `~/.funes/memory.db`)
- `FUNES_MEMORY_RECALL_K` -- memories injected per answer (default: 6)

### Server
- `FUNES_HOST` / `FUNES_PORT` -- bind address (default: `0.0.0.0:8484`)
- `FUNES_DEFAULT_AGENT` -- which agent handles requests without an explicit name
- `FUNES_ALLOW_SHELL` -- enables `execute_shell` tool and shell cron jobs (default: off)

### Email (Gmail SMTP)
- `GMAIL_ADDRESS` / `GMAIL_APP_PASSWORD` -- shared by newsletter sending and IMAP MCP

### WhatsApp
- Two bridge instances: personal (port 8090) and dedicated Funes number (port 8091)
- `WHATSAPP_WHITELIST` -- comma-separated JIDs for autoresponder

### Search
- `FUNES_TAVILY_API_KEY` -- Tavily Search API key

For the complete reference, read the comments in [funes.conf](funes.conf).
