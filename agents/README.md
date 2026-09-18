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
| `agent-builder` | Creates new agents via interview. The only holder of `create_agent`. | create_agent, list_tools | 8 |
| `agent-doctor` | Diagnoses and fixes broken agents. | read_file, delegate_to_agent, create_agent | 16 |
| `tool-builder` | Scaffolds new HTTP-template tools via interview. | create_tool | 8 |
| `file-reviewer` | Reviews an uploaded document and reports on it. | read_file, remember | 10 |

Eleven more — the newsletter `curator`, the VoC council (`voc-researcher`,
`council-chair`, `council-panelist`, `content-writer`, `mvp-builder`), and the
WhatsApp, Gmail, RSS and book-editor agents — live in the `funes-julio`
extension repository and load from its `agents/` when `FUNES_AGENTS_DIR` names
it (colon-separated). They run on this same runtime; they are simply one
person's, not the product's.

## Architecture: hub and spoke

```
User <-> funes (orchestrator)
              |
              +-> researcher         (deep web research)
              +-> operator           (workspace, shell, cron)
              +-> file-reviewer      (uploaded documents)
              +-> agent-builder      (create new agents)
              +-> agent-doctor       (diagnose/fix agents)
              +-> tool-builder       (scaffold new tools)
```

The user talks only to `funes`. When a request needs a specialist, funes
delegates via `delegate_to_agent(agent, task)`. The task string must be
entirely self-contained -- the specialist sees only it, not the conversation.
The roster funes sees is whatever is loaded, so an extension's agents appear
in it the moment their directory is on `FUNES_AGENTS_DIR`.

Delegation carries identity: the specialist runs as the delegating user and
shares its session, so anything it recalls, remembers or stores belongs to the
same account the caller is acting for.

**Special cases:**

- `agent-builder` is the only agent with `create_agent`. Never delegate
  agent-creation to `operator` -- it can write files but cannot register them.
- An agent may declare `shared_identity` when its tools authenticate as the
  installation rather than the caller — one mailbox, one phone number, one
  subscriber list — so granting it to a second account hands over the first
  account's data, however well Funes isolates its own. The field grants and
  restricts nothing by itself (the agent allowlist still decides); it changes
  what a caller is *told* when the agent is unavailable, because "ask an
  administrator" is good advice for an allowlist and bad advice for this. See
  `src/core/agent_roster.h`. None of the seven shipped agents needs it; the
  extension's Gmail, WhatsApp and newsletter agents all do.

## YAML format

```yaml
name: my-agent
description: What it does (shown in the UI agent picker)
model: default                    # or a specific model name
tools: [remember, recall, web_search]
scripts: [backup_workspace]       # vetted programs from scriptlib/ this agent
                                  # may run with run_script. EMPTY OR ABSENT
                                  # MEANS NONE -- the opposite of `tools:`
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
shared_identity: >                # this agent's tools authenticate as the
  the installation's Gmail mailbox # *installation*, not as the caller
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

## `scripts:` -- what an agent may run instead of a shell

`tools:` and `scripts:` are both allowlists, and they mean the opposite thing
when empty. An empty (or absent) `tools:` means *every registered tool*; an
empty `scripts:` means *no scripts at all*. The asymmetry is deliberate: a
tool is code compiled into the binary and reviewed as code, while a script is a
file that appears in a directory, and "every agent may run every file that
turns up in `scriptlib/`" is not a default anybody would choose on purpose.

The point of the field is to make `execute_shell` unnecessary in the common
case. An agent that needs to take a backup used to need the tool that also
runs `curl … | sh`; now it names the one script it needs, and the model
supplies *arguments*, never a command line -- they reach the process as argv
tokens with no shell in between. Granting a script takes both halves:

```yaml
tools:   [read_file, write_file, list_scripts, run_script]
scripts: [workspace_report, backup_workspace]
```

`run_script` in `tools:` gives the agent the tool; the names in `scripts:`
decide which scripts that tool can reach. `tool_limits` and `require_tools`
can name a single script rather than the tool, because one tool standing in
for n programs would otherwise share one budget and one contract slot:

```yaml
tool_limits:
  run_script: 6                            # all scripts together
  run_script:backup_workspace: 1           # this one, once per run
require_tools: [run_script:publish_issue]  # that script must have succeeded
``` Neither alone does anything, and a
grant naming a script the library doesn't have is reported as an installation
mistake rather than silently ignored. A script can also be put on a schedule —
`schedule_job(kind="script", ...)`, which needs no shell access — and its
`output:` block can declare a JSON shape the runtime checks, the same contract
`pipelines/*.yaml` puts on a pipeline stage. `agents/operator.yaml` is the
shipped example. See [../scriptlib/README.md](../scriptlib/README.md) for the manifest
format and [../src/core/script_library.h](../src/core/script_library.h) for
why the library sits outside every workspace.

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
| **Researcher** | Gathers from the outside world, then synthesizes. `researcher` (and the extension's `voc-researcher`, `content-writer`). | 16-20 | `web_search: 4-6`, `web_fetch: 6-8` | auto | The cap exists because a model that hasn't found the answer searches again rather than concluding. Leave room after the cap for the synthesis step. |
| **Pipeline worker** | Reads a stage, produces the next one. The extension's `curator`, `mvp-builder`, `council-chair`. | 12-24 | Per tool, sized to the stage | auto | Prefer `require_tools` over a high ceiling: say what must succeed, don't just allow more attempts. `write_structured` needs no cap -- a schema refusal is recoverable and retrying it is the correct behaviour. |
| **Stateless sub-agent** | One judgement, no side effects. The extension's `council-panelist`. | 4 | none needed | auto | Give it an `answer_schema` and few or no tools. Its output is consumed by a tool or another agent, so shape matters more than length. |

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

Before writing a new agent, check whether the thing you want is config: a
different **feed**, **contact list** or **habit** is usually a remembered fact,
taught once; a different instance of a pipeline is usually one more YAML for
the tools that pipeline already has. The extension repository has the worked
examples (a publication is a YAML plus a voice file; a manuscript is a copy of
`book-editor.yaml` with another `workspace_dir`). Its `astro-ph-summarizer` was
an agent until it was noticed that it was `rss-reader` plus one URL; it is now
something the user tells `rss-reader` once. A near-duplicate agent is the
cheapest thing to create and the most expensive thing to keep.

For the full YAML reference, see the root [README.md](../README.md).
