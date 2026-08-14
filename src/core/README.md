# src/core/

The agent harness -- the deterministic C++ runtime that controls what AI agents
can do, when they must stop, and what counts as success or failure. The model
produces text and tool calls; this code decides whether to execute them.

## Module map

### The agent loop

| File | Purpose |
|---|---|
| `agent.h/cpp` | `FunesAgent` -- the central class. One instance per request. Runs memory recall, context compression, the tool loop, and persistence. |
| `agent_config.h/cpp` | Parses YAML agent definitions into `AgentConfig` structs. |
| `funes_config.h` | Loads layered config files into the process environment at startup. |

### LLM communication

| File | Purpose |
|---|---|
| `llm_client.h/cpp` | HTTP client for OpenAI-compatible and Anthropic APIs, with streaming. Handles Qwen quirks, tool-call recovery from prose, and tool schema withholding. |

### Memory

| File | Purpose |
|---|---|
| `memory.h/cpp` | `MemoryStore` -- SQLite + sqlite-vec. Long-term memories with semantic search, conversation turns, rolling summaries, tool results, cron jobs. Thread-safe, graceful degradation to keyword search. |

### Safety mechanisms

| File | Purpose |
|---|---|
| `completion_contract.h/cpp` | Tools that must succeed before an answer is accepted. Prevents "described the work" from passing as "did the work." |
| `tool_budget.h/cpp` | Per-tool call ceilings. Refuses over-budget calls and withholds tool schemas to force synthesis. |
| `run_outcome.h/cpp` | Structured failure signals (`FAILED -- ...`) detectable by delegation. Prevents raw tool dumps from becoming answers. |
| `answer_schema.h/cpp` | JSON shape validation on final answers. Extraction handles fenced blocks, wrapped JSON, bare objects. |
| `context_compressor.h/cpp` | Folds old conversation turns into a rolling summary when approaching context limits. |
| `result_store.h/cpp` | Stores large tool outputs out-of-transcript with head/tail previews and `read_result` dereference. |

### Tools

| File | Purpose |
|---|---|
| `tools.h/cpp` | `ToolRegistry` -- in-process tool dispatch with OpenAI-format schema generation. |
| `tools/` | Individual tool implementations. See [tools/README.md](tools/README.md). |

### Scheduling

| File | Purpose |
|---|---|
| `cron_runner.h/cpp` | Background poll thread that fires due cron jobs (agent tasks or shell commands). |
| `cron_schedule.h/cpp` | Pure-function cron expression parser and next-run calculator. |

### Publishing

| File | Purpose |
|---|---|
| `publication.h/cpp` | Loads publication configs (queries, artifacts, channels) from `publications/*.yaml`. |

### Utilities

| File | Purpose |
|---|---|
| `text_utils.h/cpp` | UTF-8 validation, safe truncation, image MIME detection, crash-safe JSON dump. |
| `base64.h/cpp` | Base64 encode/decode for image attachments. |

## How the pieces fit together

A request flows through the system like this:

```
HTTP request
  -> FunesApi creates FunesAgent(config, tools, memory, defaults)
    -> run()
      1. Recall relevant memories (semantic search)
      2. Load conversation history + rolling summary
      3. Compress context if > 70% of limit
      4. Build prompt: system + memories + summary + history + user message
      5. Enter tool loop:
         LLM completion -> tool call? -> dispatch -> feed result -> repeat
         Safety checks at every step:
           - Tool in allowlist?
           - Over budget? (tool_limits)
           - Same call 3x? (loop detection)
           - Same tool too many times? (near-dup detection)
           - Trying to finish early? (completion contract)
           - Answer wrong shape? (answer_schema)
           - Last step? (reserve for synthesis)
      6. Persist turns + auto-memory
      7. Report context usage
```

## Safety mechanism layering

Each layer catches a specific failure mode, developed incrementally from
production incidents (all documented in comments with dates):

1. **Tool call recovery + withholding** (llm_client) -- local models sometimes
   write tool calls as JSON/XML in prose. Recovered on normal turns, discarded
   when tools are withheld, preventing loophole abuse.

2. **Tool budget** (tool_budget) -- per-tool ceiling. Refusal is recoverable;
   the model is told to conclude from what it has. After refusal, schemas are
   withheld on the next turn.

3. **Loop detection** (agent.cpp) -- exact-signature (3x same call = kill) and
   near-duplicate (same tool name too many times = kill).

4. **Last-step reservation** (agent.cpp) -- final step always withholds tools
   and sends a "synthesize now" notice.

5. **Completion contract** (completion_contract) -- required tools must succeed.
   Early answers get nudged; exhausted budget produces `FAILED --`.

6. **Answer schema** (answer_schema) -- JSON shape validation on final answer.
   Shares nudge budget with completion contract.

7. **Run outcome** (run_outcome) -- every failure exit carries `FAILED --`
   prefix. `delegation.cpp` turns this into a tool error instead of content.

8. **Result store** (result_store) -- large outputs stored by reference, not
   inline. Prevents a 50KB web scrape from becoming the answer.

9. **Context compression** (context_compressor) -- automatic and manual, keeps
   conversations alive indefinitely within fixed context windows.
