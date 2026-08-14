# agents/

One YAML file per agent. The shared C++ runtime in `src/core/agent.cpp` executes
all of them identically -- an "agent" is a name, a system prompt, a tool
allowlist, and a handful of knobs, not an independent binary or container.

## The roster

| Agent | Role | Key tools | Steps |
|---|---|---|---|
| `funes` | Orchestrator. The only agent the user talks to. | delegate_to_agent, web_search, remember/recall, read/write_file | 8 |
| `operator` | Workspace and scheduler. Files, shell, cron jobs. | read/write_file, execute_shell, schedule/list/cancel_job | 12 |
| `researcher` | Deep web research with source attribution. | web_search, web_fetch, remember/recall | 20 |
| `curator` | Newsletter publisher. Picks stories, writes posts, publishes. | harvest_candidates, publish_issue, read_result | 24 |
| `agent-builder` | Creates new agents via interview. The only holder of `create_agent`. | create_agent, list_tools | 8 |
| `agent-doctor` | Diagnoses and fixes broken agents. | read_file, delegate_to_agent, create_agent | 16 |
| `tool-builder` | Scaffolds new HTTP-template tools via interview. | create_tool | 8 |
| `whatsapp-assistant` | Reads/sends WhatsApp via MCP bridge. | search_contacts, send_message, list_messages | 8 |
| `whatsapp-autoresponder` | Generates replies for incoming WhatsApp. Never invoked by funes. | recall, remember, read_file | 6 |
| `gmail-assistant` | Searches, reads, and drafts Gmail via IMAP MCP. Cannot send. | search/list/get_email, create/update_draft | 8 |
| `rss-reader` | Reads RSS/Atom feeds via MCP. | fetch_feed_entries, fetch_article_content | 6 |
| `astro-ph-summarizer` | Summarizes arXiv astrophysics papers. | fetch_feed_entries, fetch_article_content | 15 |

## Architecture: hub and spoke

```
User <-> funes (orchestrator)
              |
              +-> researcher         (deep web research)
              +-> operator           (workspace, shell, cron)
              +-> curator            (newsletter pipeline)
              +-> whatsapp-assistant (WhatsApp via MCP)
              +-> gmail-assistant    (Gmail via MCP)
              +-> rss-reader         (RSS via MCP)
              +-> astro-ph-summarizer(arXiv via MCP)
              +-> agent-builder      (create new agents)
              +-> agent-doctor       (diagnose/fix agents)
              +-> tool-builder       (scaffold new tools)
```

The user talks only to `funes`. When a request needs a specialist, funes
delegates via `delegate_to_agent(agent, task)`. The task string must be
entirely self-contained -- the specialist sees only it, not the conversation.

**Special cases:**

- `whatsapp-autoresponder` is never invoked by funes. It is called exclusively
  by `scripts/whatsapp_autoresponder.py`, which polls for incoming messages and
  sends the agent's reply text back via the bridge API.
- `agent-builder` is the only agent with `create_agent`. Never delegate
  agent-creation to `operator` -- it can write files but cannot register them.
- `curator` runs 30+ tool calls per newsletter issue. Funes must never attempt
  to do the same work in parallel.

## YAML format

```yaml
name: my-agent
description: What it does (shown in the UI agent picker)
model: default                    # or a specific model name
tools: [remember, recall, web_search]
max_steps: 8                      # tool-call budget per run
context_limit: 8192               # token ceiling
tool_choice: auto                 # auto | required | none
system_prompt: |
  You are ...

# Optional safety mechanisms
require_tools: [some_tool]        # completion contract
tool_limits:                      # per-tool call ceilings
  web_search: 3
answer_schema:                    # JSON shape enforcement
  type: object
  required: [answer]

# Optional infrastructure
workspace_dir: /some/path         # override default workspace
memory_scope: funes               # share another agent's memory pool
mcp_servers:
  - name: my-server
    command: npx -y my-mcp-server
```

## Adding an agent

Create `agents/my-agent.yaml`, then either restart funes or hit
`POST /api/agents/reload`. No code changes, no rebuild. The new agent
appears in the UI and in `funes`'s delegation roster immediately.

For the full YAML reference, see the root [README.md](../README.md).
