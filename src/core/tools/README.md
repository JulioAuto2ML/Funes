# src/core/tools/

Every built-in tool implementation. Each file registers one or more tools into
the shared `ToolRegistry` at startup. Tools are plain C++ functions -- no HTTP
hop, no protocol overhead.

## Tool inventory

### User-facing tools

| Tool | File | What it does |
|---|---|---|
| `web_search` | `web_search.cpp` | Tavily API search with topic/time filters. Social platforms excluded. |
| `web_fetch` | `web_fetch.cpp` | Fetches a URL, converts HTML to readable text (8 KB cap). SSRF-protected. |
| `remember` | `memory_tools.cpp` | Stores a fact in persistent long-term memory. |
| `recall` | `memory_tools.cpp` | Semantic (or keyword) search over stored memories. |
| `read_file` | `file_tools.cpp` | Reads text, PDFs (via pdftotext, with image fallback for scans), and images (base64 for vision). Workspace-confined. |
| `write_file` | `file_tools.cpp` | Writes or appends text. Creates parent directories. Workspace-confined. Refuses to replace an existing file unless `overwrite: true`, so "write it again" becomes a question for the user (replace, or the free name the refusal suggests) instead of a silent loss of the previous version. |
| `execute_shell` | `shell_tool.cpp` | Runs a shell command. Opt-in only (`FUNES_ALLOW_SHELL=1`). Timeout, output cap. |
| `list_scripts` | `script_tools.cpp` | Lists the scripts the calling agent is granted, with the arguments each takes. |
| `run_script` | `script_tools.cpp` | Runs one script from the central library by name, with declared parameters passed as argv, and checks its output against the shape the manifest declares. Per-agent allowlist (`scripts:` in agent YAML); no shell, no global switch. |
| `compress_context` | `context_tools.cpp` | Folds old conversation turns into a summary to free context space. |
| `classify_decision` | `classify_decision.cpp` | Reflex/Jev-style typed decision: probability per option from the model already loaded for chat, via `LLMClient::label_probs` — no free-text generation, two option orderings averaged to cancel position bias. |
| `read_result` | `result_tools.cpp` | Reads a windowed portion of a large stored tool result. |
| `list_tools` | `introspection.cpp` | Lists every registered tool with its description. |
| `delegate_to_agent` | `delegation.cpp` | Hands a task to a specialist agent. Self-delegation refused, depth-2 cap. |

### Extension tools

Tools that belong to one operator's workflow are not in this directory. They
are compiled in from another repository through `src/core/extension.h`
(`-DFUNES_EXTENSIONS=<dir>`) and register themselves into the same
`ToolRegistry`. The first such extension, `funes-julio`, carries the newsletter
tools (`harvest_candidates`, `publish_issue`) and the pipeline tools
(`write_structured`, `read_structured`, `merge_rankings`) that used to be here;
its own README documents them.

### Scheduling tools

| Tool | File | What it does |
|---|---|---|
| `schedule_job` | `cron_tool.cpp` | Creates a recurring cron job: an agent task, a library script (`kind="script"` — no shell access needed, grant re-checked at fire time), or a shell command. |
| `list_jobs` | `cron_tool.cpp` | Lists all scheduled jobs with status and last run. |
| `cancel_job` | `cron_tool.cpp` | Deletes a scheduled job. |
| `run_job_now` | `cron_tool.cpp` | Fires a job immediately for testing. |

### Meta-tools

| Tool | File | What it does |
|---|---|---|
| `create_agent` | `agent_builder.cpp` | Writes a YAML agent definition and hot-reloads. Only `agent-builder` has this. |
| `create_tool` | `tool_builder.cpp` | Scaffolds a C++ HTTP-template tool source file. Requires rebuild. |

## Shared infrastructure (not tools themselves)

| File | Purpose |
|---|---|
| `fs_guard.h/cpp` | Filesystem path confinement plus `workspace_for`, the single resolver for which directory a call operates in (`<root>/<user_id>`, with an agent's own `workspace_dir` nested inside when relative). Catches `..` traversal, symlinks, absolute escapes. Used by read/write_file, shell, and /api/upload -- keep it the only resolver, or the confinement check ends up guarding a different root than the one being written to. |
| `net_guard.h/cpp` | SSRF protection. Blocks requests to private/loopback hosts. Used by web_fetch and HTTP template tools. |
| `process_runner.h/cpp` | Fork/exec engine with timeout, process-group kill, output cap. Used by execute_shell, read_file (PDF), publish_issue, run_script. Optionally passes extra environment (a script manifest's `env:`, so a credential never enters the model's context) and optionally keeps stderr separate — a script whose stdout is a declared JSON contract must not have it corrupted by a library's deprecation warning. |
| `tavily.h/cpp` | Tavily Search API HTTP client. Used by web_search (and by extensions that need the same call). |
| `../answer_schema.h/cpp` | The JSON-Schema subset used for both an agent's `answer_schema:` and a pipeline stage's `schema:` — one validator, so the two fail identically and read identically to a small local model. |
| `../script_library.h/cpp` | The script library: manifest parsing, the `[a-z0-9_-]` name alphabet, and argv construction from declared parameters. Split from `script_tools.cpp` so the allowlist and argument rules are testable without starting a process. |
| `page_text.h/cpp` | URL fetching + HTML-to-text extraction. Manual scan (no regex -- avoids stack overflow on large inline scripts). |
| `pdf_extract.h/cpp` | PDF text extraction via pdftotext, with image rendering fallback for scans. |
| `http_tool_runtime.h/cpp` | Execution engine for generated HTTP-template tools. Resolves `{param}` from arguments (URL and body) and `${ENV_VAR}` from the environment (**header values only** — the URL is echoed back to the model on a parse failure, so a secret resolved into it becomes something the model can print). |

## generated/

Tools scaffolded by `create_tool` land here as `.cpp` source files. They use
`http_tool_runtime.h` and self-register via file-scope static initializers.
These are **not** hot-reloaded -- they require `cmake --build build` and a
restart, keeping a human in the loop.

## Security model

The tool system enforces security at multiple layers:

- **Filesystem**: `fs_guard` confines read/write_file to the calling account's
  own workspace, `<root>/<user_id>/`. Path traversal, symlink escapes, and
  absolute paths are all caught -- including `../<other_user_id>/...`.
  Confinement is not the only way a file is lost, so `write_file` also refuses
  to replace one that already exists: the workspace has no history and no undo,
  and the ordinary sequence "write the report" -> the user changes something ->
  "write it again" destroyed the first version before anyone was asked. The
  refusal is recoverable (like a tool-budget refusal), names a free alternative
  path, and is lifted by `overwrite: true` -- the user's answer relayed back, or
  an unattended job that owns the file. Appending and byte-identical rewrites
  destroy nothing and are never refused.
- **Network**: `net_guard` blocks SSRF to localhost, 10.x, 192.168.x, 169.254.x.
- **Shell**: Disabled by default. When enabled: hard timeout (120s max), output
  cap (16 KB), process-group kill on timeout.
- **Scripts**: the narrow alternative to shell. `run_script` starts only what
  the calling agent's `scripts:` list names *and* the central library
  (`FUNES_SCRIPTS_DIR`) has installed -- an empty list means none, so the grant
  is always explicit. The model names a script and fills in declared
  parameters; those become `--name=value` argv tokens handed to `execvp`, so no
  shell parses them and a value containing `;` is punctuation rather than a
  second command. The library lives outside every workspace, which is what
  makes "installing a script" an admin action: `read_file`, `write_file` and
  `/api/upload` cannot reach it. Same timeout/output-cap/process-group-kill
  machinery as shell; **not** gated by `FUNES_ALLOW_SHELL`, because a reviewed
  program granted by name is a different act from an arbitrary command line —
  including on a schedule (`schedule_job(kind="script")`), where the agent's
  grant and the owner's permissions are re-resolved when the job fires rather
  than trusted from when it was written. Budgets, completion contracts and
  permission entries accept a per-script key (`run_script:<name>`), so the one
  tool does not collapse n programs into one ceiling, one contract slot and
  one grant.
  It is an allowlist, not a sandbox: a script still runs with the process's own
  permissions, so a script that takes a command or fetches code to run gives
  back everything the split was for.
- **Delegation**: Self-delegation refused, depth capped at 2, failures detected
  and surfaced as errors (not content). The specialist runs as the delegating
  user, sharing its caller's session and memory pool.
- **Identity**: every handler receives `ToolContext::user_id` and must scope
  its storage calls to it. Tools that touch memories, results or cron jobs
  pass it straight through to `MemoryStore`.
- **Content**: Binary/non-UTF-8 rejected everywhere. Output capped at every
  boundary. Large results stored by reference.
- **Child processes** (`process_runner.cpp`): an allowlisted environment, never
  the server's own — no `FUNES_*`, no key or token from `funes.local` reaches a
  script, a shell command or an MCP stdio server unless the operator names it
  in `FUNES_CHILD_ENV` or the manifest/agent YAML passes it explicitly.
- **Outbound URLs** (`net_guard.cpp`): the private-host check resolves the name
  and judges every address; redirects are followed by hand so each hop is
  checked; unresolvable names are refused.
