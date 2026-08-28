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
| `write_file` | `file_tools.cpp` | Writes or appends text. Creates parent directories. Workspace-confined. |
| `execute_shell` | `shell_tool.cpp` | Runs a shell command. Opt-in only (`FUNES_ALLOW_SHELL=1`). Timeout, output cap. |
| `compress_context` | `context_tools.cpp` | Folds old conversation turns into a summary to free context space. |
| `read_result` | `result_tools.cpp` | Reads a windowed portion of a large stored tool result. |
| `list_tools` | `introspection.cpp` | Lists every registered tool with its description. |
| `delegate_to_agent` | `delegation.cpp` | Hands a task to a specialist agent. Self-delegation refused, depth-2 cap. |

### Newsletter pipeline tools

| Tool | File | What it does |
|---|---|---|
| `harvest_candidates` | `harvest.cpp` | Searches, deduplicates, fetches, and builds a numbered candidate pool for a publication. |
| `publish_issue` | `issue.cpp` | Publishes an issue: resolves IDs to URLs, runs deterministic grounding checks, renders artifacts, sends. |

### Scheduling tools

| Tool | File | What it does |
|---|---|---|
| `schedule_job` | `cron_tool.cpp` | Creates a recurring cron job (agent task or shell command). |
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
| `process_runner.h/cpp` | Fork/exec engine with timeout, process-group kill, output cap. Used by execute_shell, read_file (PDF), publish_issue. |
| `tavily.h/cpp` | Tavily Search API HTTP client. Used by web_search and harvest_candidates. |
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
- **Network**: `net_guard` blocks SSRF to localhost, 10.x, 192.168.x, 169.254.x.
- **Shell**: Disabled by default. When enabled: hard timeout (120s max), output
  cap (16 KB), process-group kill on timeout.
- **Delegation**: Self-delegation refused, depth capped at 2, failures detected
  and surfaced as errors (not content). The specialist runs as the delegating
  user, sharing its caller's session and memory pool.
- **Identity**: every handler receives `ToolContext::user_id` and must scope
  its storage calls to it. Tools that touch memories, results or cron jobs
  pass it straight through to `MemoryStore`.
- **Content**: Binary/non-UTF-8 rejected everywhere. Output capped at every
  boundary. Large results stored by reference.
- **Newsletter pipeline**: The model picks candidates by numeric ID. URL
  resolution is deterministic. Grounding checks verify post text against page
  content using word overlap, with no LLM involvement.
