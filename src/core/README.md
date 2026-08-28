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

### Users and authentication

| File | Purpose |
|---|---|
| `users.h/cpp` | `UserStore` -- accounts, session tokens, WhatsApp jid mapping. Its own class and connection on the same SQLite file; authentication has nothing to do with recall. |
| `password.h/cpp` | PBKDF2-HMAC-SHA256 via OpenSSL (already linked for HTTPS). Self-describing stored form, so the cost can be raised without invalidating existing hashes. Malformed hashes fail closed. |

### LLM communication

| File | Purpose |
|---|---|
| `llm_client.h/cpp` | HTTP client for OpenAI-compatible and Anthropic APIs, with streaming. Handles Qwen quirks, tool-call recovery from prose, and tool schema withholding. |

### Memory

| File | Purpose |
|---|---|
| `memory.h/cpp` | `MemoryStore` -- SQLite + sqlite-vec. Long-term memories with semantic search, conversation turns, rolling summaries, tool results, cron jobs. Thread-safe, graceful degradation to keyword search. Every method takes the `user_id` it acts as -- deliberately with no default, so a new call site that forgets one fails to compile rather than writing into the admin's data. |

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
  -> auth gate: valid session cookie or service token, else 401
  -> handler resolves the caller (require_auth) -> user_id
  -> FunesApi creates FunesAgent(config, tools, memory, defaults)
    -> run(message, session, user_id, ...)
      0. ToolContext carries user_id into every tool, including delegation
      1. Recall relevant memories (semantic search, this user's partition)
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

## Per-user isolation (4.0)

Isolation is a property of the SQL, not of the handlers: every query carries
`WHERE user_id = ?`, and ownership checks sit on the statement itself
(`DELETE ... WHERE id = ? AND user_id = ?`) rather than being a lookup
followed by a branch. That makes "not yours" and "not there" the same answer,
so ids cannot be probed, and removes the check-then-act race.

Three things in `memory.cpp` are easy to get wrong and are worth reading
before changing anything there:

- `vec_memories` declares `user_id` as a vec0 **PARTITION KEY**. vec0 refuses
  to combine `MATCH` with an arbitrary `WHERE`, but accepts a partition key
  and prunes partitions rather than post-filtering. Post-filtering is not a
  leak, but the shared candidate pool means a busy account crowds a quiet one
  out of its own top-k.
- The vec table is rebuilt inside `migrate()`, not lazily on the next write.
  `recall_semantic` names `v.user_id`, and a recall normally happens before
  any write -- a pre-4.0 table makes that a failed prepare, which throws and
  takes the agent run with it.
- `insert_vector` deletes then inserts. vec0 with a partition key rejects
  `INSERT OR REPLACE` on an existing row, and the only caller that hits one is
  consolidation re-vectorising a merged memory.

Background work that has no request to take an identity from carries its own:
cron jobs record their owner and the runner adopts it; consolidation iterates
one user's pool at a time.

## What a run leaves behind (`Persist`)

`FunesAgent::run` takes a three-valued `Persist` (agent.h) rather than a bool,
because the two things it used to gate together are wanted separately:

| Mode | Session turns | Auto-memory | Used by |
|---|---|---|---|
| `Full` | yes | yes | a person talking -- `/api/chat` |
| `TurnsOnly` | yes | **no** | scheduled runs -- `cron_runner.cpp` |
| `None` | no | no | delegated sub-agent calls -- `delegation.cpp` |

The middle one is why the split exists. A cron firing used to persist exactly
like a conversation, which meant it also wrote an auto-memory phrased
`User said: "<the job's task>" -- I replied: "..."`. The scheduler is not the
user: one of those on the deployment recorded the job-runner preamble verbatim,
as if the person had typed `[You are running as a scheduled job...]`.

They were not merely clutter -- they were *recalled*. Two of them carried
`recall_count` of 13 and 15, i.e. they had been injected into that many real
conversations, presenting the scheduler talking to itself as something the
person had said.

They also did not accumulate one per firing, which is why this took so long to
notice. `remember()` inserts under `UNIQUE(user_id, agent, text)`, so a job
whose reply is word-for-word last week's dedupes away silently; the count grows
only when the model words itself differently. Irregular and bounded, easy to
mistake for nothing happening.

The turns are kept, though, and deliberately: `cron_jobs.last_output` is a
4000-byte preview of the **last** run only, which is no help the morning after
a failure two runs ago. They are hidden from the conversation list instead --
`MemoryStore::list_sessions` filters `cron-%` unless asked. `funes cron-cleanup`
removes the auto-memories an older database already holds (and, only if asked,
the old transcripts); it goes through `forget()`/`delete_session()` rather than
SQL, because a raw `DELETE` from `memories` leaves the vec0 index holding a
vector whose row is gone and nothing ever notices.

`MemoryStore::CRON_SESSION_PREFIX` and `CRON_TASK_PREAMBLE` live next to each
other for the same reason: `cron_runner.cpp` writes both and `memory.cpp`
matches on both, and two spellings would make the filter and the cleanup
silently stop finding anything.
