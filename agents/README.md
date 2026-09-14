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
| `rss-reader` | Reads RSS/Atom feeds via MCP. Which feeds the user follows is remembered, not configured. | fetch_feed_entries, fetch_article_content, read_result | 10 |
| `file-reviewer` | Reviews an uploaded document and reports on it. | read_file, remember | 10 |
| `book-editor` | Edits manuscript chapters against the manuscript's own `style.md`. | read/write_file, recall/remember, compress_context | 16 |
| `voc-researcher` | Finds voice-of-customer pain points and opens a council debate. | web_search, write_structured, delegate_to_agent | 20 |
| `council-chair` | Runs the 3-perspective debate and files the decision. | delegate_to_agent, merge_rankings, write_structured | 12 |
| `council-panelist` | Ranks proposals from one perspective. Stateless. | (none — judgement only) | 4 |
| `content-writer` | Writes the article from a debate winner. | read_structured, web_search, write_structured | 16 |
| `mvp-builder` | Scaffolds and runs a prototype from an approved debate. | read_structured, execute_shell, write_structured | 20 |

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
              +-> book-editor        (manuscript editing)
              +-> file-reviewer      (uploaded documents)
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
in five prompts. See [../pipelines/README.md](../pipelines/README.md).

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
- `council-panelist` has no tools at all. That is the point: three independent
  opinions are only independent if none of them can look at what the others
  saw. Its `answer_schema` is what makes its reply mergeable by a tool.

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
workspace_dir: subfolder          # nested inside the caller's own workspace
                                  # (<root>/<user_id>/subfolder). An absolute
                                  # path is honoured verbatim and is then
                                  # shared by every account -- deliberate, but
                                  # nothing shipped uses it.
memory_scope: funes               # share another agent's memory pool
mcp_servers:
  - name: my-server
    command: npx -y my-mcp-server
```

## Archetypes: where the numbers come from

`max_steps` and `tool_limits` are the two fields people guess at, and a guess
is expensive in both directions -- too low kills a legitimate run mid-way, too
high lets a confused model burn twenty searches and synthesize nothing. The
values below are not theory: each is what a shipped agent settled on after a
real failure, generalized so the next agent's author inherits the answer
instead of rediscovering it.

Pick the row your agent resembles, start from its numbers, and change one only
when you can say what went wrong at the old value.

| Archetype | Shape | `max_steps` | `tool_limits` | `tool_choice` | Notes |
|---|---|---|---|---|---|
| **Orchestrator** | Talks to the user, delegates the work. `funes`, `agent-doctor`. | 8 | `delegate_to_agent: 3`, `web_search: 2` | auto | Low on purpose: an orchestrator that searches is an orchestrator doing the specialist's job badly. |
| **Researcher** | Gathers from the outside world, then synthesizes. `researcher`, `voc-researcher`, `content-writer`. | 16-20 | `web_search: 4-6`, `web_fetch: 6-8` | auto | The cap exists because a model that hasn't found the answer searches again rather than concluding. Leave room after the cap for the synthesis step. |
| **Pipeline worker** | Reads a stage, produces the next one. `curator`, `mvp-builder`, `council-chair`. | 12-24 | Per tool, sized to the stage | auto | Prefer `require_tools` over a high ceiling: say what must succeed, don't just allow more attempts. `write_structured` needs no cap -- a schema refusal is recoverable and retrying it is the correct behaviour. |
| **Stateless sub-agent** | One judgement, no side effects. `council-panelist`. | 4 | none needed | auto | Give it an `answer_schema` and few or no tools. Its output is consumed by a tool or another agent, so shape matters more than length. |

Two rules that apply to all four:

- **A limit that can be hit must be recoverable.** `tool_limits` refuses the
  call and lets the run continue; `max_steps` ends it. Put the pressure on the
  former.
- **Reserve the last step.** The runtime already withholds tools on the final
  step to force a written answer, so an agent whose real work needs N tool
  calls needs `max_steps` of at least N+1.

## Adding an agent

Create `agents/my-agent.yaml`, then either restart funes or hit
`POST /api/agents/reload`. No code changes, no rebuild. The new agent
appears in the UI and in `funes`'s delegation roster immediately.

Before writing a new agent, check whether the thing you want is config:

- A different **publication** is `publications/*.yaml` + a voice file.
- A different **pipeline**, or a new stage in one, is `pipelines/*.yaml`.
- A different **manuscript** is a copy of `book-editor.yaml` with another
  `workspace_dir` and its own `style.md`.
- A different **feed**, **contact list** or **habit** is usually a remembered
  fact, taught once.

`astro-ph-summarizer` was an agent until it was noticed that it was
`rss-reader` plus one URL; it is now something the user tells `rss-reader`
once. A near-duplicate agent is the cheapest thing to create and the most
expensive thing to keep.

For the full YAML reference, see the root [README.md](../README.md).
