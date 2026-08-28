# tests/

Two-layer test suite: C++ unit tests (25 files) and a Bash integration test
that exercises the full stack end-to-end.

## Running

```sh
cmake --build build
cd build && ctest --output-on-failure
```

The publishing scripts also have tests, run by `ctest` as the `publishing`
test and by `python3 publishing/publish_newsletter.py --self-test` on the
deployment machine.

## Unit tests

Each `test_*.cpp` is a standalone binary with its own `main()`. No external
test framework -- a minimal `CHECK(cond)` macro keeps things zero-dependency.

| Test file | What it covers |
|---|---|
| `test_agent_config` | YAML parsing, defaults, answer_schema type preservation |
| `test_tools` | ToolRegistry dispatch, schema generation, error handling, memory scoping |
| `test_memory` | MemoryStore: keyword/semantic recall, source weighting, turns, consolidation, prune/merge, backfill draining (one capped call is not "the backfill"), cron sessions hidden from the conversation list, `cron-cleanup` scoping |
| `test_user_isolation` | **The one to keep green.** No account can read, delete or overwrite another's memories, turns, summaries, results, cron jobs or files. Also *fidelity* -- a user still gets their own recall hits when another pool is 200x larger -- plus regressions for the two 4.0 migration bugs (pre-4.0 vec table rebuilt before the first recall; merged memories keeping their vector) |
| `test_users` | Accounts, login, token expiry and revocation, delete-cascades-tokens, unmapped jid resolves to nobody |
| `test_password` | Malformed stored hashes fail closed, iteration downgrade rejected, salt randomness, constant-time compare |
| `test_delegation` | Validation: missing agent, self-delegation, unknown agent error messages |
| `test_completion_contract` | Contract bookkeeping, satisfaction tracking, nudge messages, failure format |
| `test_answer_schema` | JSON extraction generosity + validation strictness, type/required/enum/minItems |
| `test_tool_budget` | Over-budget boundary, refusal wording |
| `test_run_outcome` | Failure message format, `is_run_failure` detection |
| `test_context_compressor` | Token estimator, no-op guard, prune_turns + summary |
| `test_result_store` | Session isolation, preview shape, read_result windowing |
| `test_cron_schedule` | Cron expression parsing, next-run arithmetic |
| `test_cron_tool` | schedule_job validation, CRUD roundtrip, run_job_now |
| `test_file_tools` | fs_guard traversal prevention, read/write/append, PDF extraction, image detection |
| `test_shell_tool` | Disabled-by-default check, enabled path |
| `test_harvest` | URL stripping, dedup, stale filter, story clustering, shortlisting, pool format |
| `test_issue` | Grounding checks, evidence extraction, candidate substitution, build_issue validation |
| `test_publication` | Publication config loading and defaults |
| `test_web_fetch` | Non-UTF-8 handling, large inline script, HTML landmark dropping |
| `test_text_utils` | UTF-8 validation, safe truncation, dump_safe |
| `test_base64` | RFC 4648 vectors, roundtrip, binary with NUL |
| `test_meta_tools` | list_tools, create_tool, create_agent |
| `test_llm_client_images` | Multipart image wire format |

## Integration test

`integration.sh` (~1000 lines, 220 assertions) starts the real `funes` binary against
`mock_llm.py` on scratch ports with a scratch database and workspace. It
creates fixture agents on the fly and validates responses via `curl`.

### mock_llm.py

A mock OpenAI-compatible server that handles `/v1/chat/completions` (streaming
and non-streaming) and `/v1/embeddings`. Uses keyword matching in the user
message to trigger specific agent behaviors:

| Keyword | Behavior tested |
|---|---|
| `use-tool` | Tool-call round trip |
| `delegate-now` | Agent delegation |
| `contract-comply` / `contract-refuse` | Completion contracts |
| `budget-burn` / `budget-stubborn` | Tool budgets |
| `step-hog` | max_steps reservation |
| `xml-hog` / `xml-yielder` / `needs-telling` | Tool withholding |
| `delegate-boom` / `sub-gives-up` | Delegation failure propagation |
| `schema-comply` / `schema-refuse` | Answer schemas |
| `big-result` / `deref-full` | Result store |
| `schedule-now` | Cron scheduling |

### Coverage highlights

- Memory CRUD + semantic search
- Tool-call round trips through the agent loop
- Completion contracts (comply and refuse paths)
- Tool budgets (cooperative and stubborn models)
- Last-step synthesis reservation
- Tool withholding (XML prose calls rescued vs. discarded)
- Delegation failure propagation
- Answer schema enforcement
- Result store (store, dereference, no re-store)
- File upload (text, binary, image)
- Chat with image attachments
- Session continuity (history restoration)
- A caller's own resolved permissions on `/api/auth/status` (and none at all
  for an anonymous request, since the route is public)
- Scheduled runs: hidden from the conversation list, reachable via `?cron=1`,
  and writing no auto-memory
- `funes cron-cleanup` reporting without deleting unless `--apply`
- An admin's box-wide `/api/jobs?all=1` -- with a job owned by *each* account,
  because with only the admin's the widening would be unobservable
- An expired session cookie (backdated via `tests/expire_token.py`) failing to
  authenticate over HTTP, and login still working afterwards
- `funes perms` bad input: unknown options, missing values, a nonexistent
  user, exit codes, the JSON actually written, and a half-valid line leaving
  the stored blob untouched

## Testing patterns

- **FakeEmbedder**: `test_memory.cpp` uses 26-dimensional letter-frequency
  vectors for deterministic semantic similarity without a real embedding model.
- **Fixture agents**: `integration.sh` writes temporary YAML agent configs with
  specific tool sets and contract/budget settings.
- **Mutation-checking a new test**: before trusting an assertion, break the
  code it covers and confirm it fails, and fails for the stated reason. Two
  tests in this suite would otherwise have passed while proving nothing -- the
  concurrency test passes identically against a serialising server (hence the
  timing floor), and `/api/jobs?all=1` passed against a handler that ignored
  `all` entirely until the suite grew a second job owner.
- **`expire_token.py`**: backdates one `auth_tokens` row so expiry can be
  tested over HTTP without waiting out a 30-day TTL. It asserts the row is
  still present and now expired, because "no such token" and "expired token"
  are deliberately indistinguishable from outside and the test must not pass
  for the wrong one.
- **Self-test flag**: `publish_newsletter.py --self-test` runs the publishing
  test suite, used both in CI and on the deployment machine.
- **Two users, one fixture**: isolation tests use ids 1 and 2 and deliberately
  collide on session names and file paths, since a client-supplied session id
  is exactly what two accounts can pick the same value for.
