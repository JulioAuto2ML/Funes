# Roadmap

Three improvement plans exist. This file says which order they run in and why.
It does not restate them — each links to its own document.

Current release: **5.1** (security fixes + the core/extension split, `project(Funes VERSION 5.1.0)`).

| Release | Theme | Source plan | Status |
|---------|-------|-------------|--------|
| **4.1** | Generalization: config over prose, tools over model arithmetic | [generalization-plan.md](generalization-plan.md) phases 1, 4, 5 | shipped |
| **4.2** | Per-user publications; generic channel adapter; self-service pointed at MCP | [generalization-plan.md](generalization-plan.md) phases 2, 3 + below | moved to funes-julio |
| **5.0** | Connected memories + localization | v5 plan (8 phases) | done |
| **5.0.1** | Security: shell-job permission bypass, child env allowlist, resolving SSRF guard, hashed tokens, login throttle, body cap | review of 2026-09-18 | done |
| **5.1** | Split: the core keeps the harness, `funes-julio` takes one operator's tools, agents and deployment; `src/core/extension.h` is the seam | same review | done |
| **6.0** | Voice: STT + TTS sidecars | v6 voice research | next |

## Why this order

**4.1 before 5.0.** The generalization work is the only plan that pays down
debt already in the tree rather than adding surface. It touches no schema, no
external service and no new dependency: a config loader, two tools, one
arithmetic tool, and prompt edits. 5.0 adds two schema migrations plus a
271-memory LLM backfill; doing that on top of five agents that still encode
their file conventions in prose means migrating the prose too.

**Phases 1, 4, 5 in 4.1; phases 2 and 3 held for 4.2.** Phase 1 (the pipeline
primitive) is a direct repeat of an already-proven fix — the newsletter
rewrite — against a pipeline that was built after it without absorbing it.
Phase 4 is prompt edits only. Phase 5 is a documentation change plus defaults.
Phases 2 (channel adapter) and 3 (`mcp-builder`) are infrastructure for
requests that have not arrived: a second chat channel and a third tool author.
Building either now is speculative generality, which is the same mistake the
plan is about, pointed the other way.

**5.0 before 6.0.** Voice is additive and independent, but it needs CPU
headroom on yoda that the memory backfill also wants, and connected memories
change what a voice session recalls. Shipping recall changes first means the
voice work is tested against the recall behaviour it will live with.

## 5.0.1 — what was wrong and what changed

Found in a code review of the whole tree, all verified against the code, none
previously covered by a test. Fixed before any new surface (6.0 voice) on the
principle that a privilege escalation open in the current release outranks a
feature in the next one.

| # | Defect | Fix | Test |
|---|--------|-----|------|
| 1 | `schedule_job(kind="shell")` checked only `FUNES_ALLOW_SHELL`, never the owner's `execute_shell` permission — a member (denied the shell by default) could schedule any command and the runner executed it as the process, from the workspace *root*. `funes.conf` shipped the switch **on**. | Permission checked at schedule time (`cron_tool.cpp`) and re-resolved at fire time (`cron_runner.cpp`); cwd is the owner's workspace; `FUNES_ALLOW_SHELL` defaults to `0` in `funes.conf` (enable in `funes.local`). | `test_cron_tool: test_shell_jobs_need_execute_shell_permission` |
| 2 | Every child process inherited the server's whole environment, into which `funes_config.h` had loaded `funes.local`: service token, LLM key, IMAP password, Tavily key — reachable by `env` in `execute_shell`, by any library script, by any MCP stdio server. | `proc::child_environment()` allowlist; `run_argv`, `run_shell_command` and the vendored `mcp::stdio_client` (new `set_environment_filter` hook) all use it. `FUNES_CHILD_ENV` names extra pass-throughs. | `test_shell_tool: test_child_environment_is_an_allowlist` |
| 3 | `net_guard::is_private_host` was a regex over the host literal: `127.0.0.1.nip.io`, `0x7f000001`, `2130706433`, `[::ffff:127.0.0.1]`, IPv6 ULA/link-local and any attacker-controlled domain pointed at the LAN all passed. httplib followed redirects to any host without re-checking. | Resolve with `getaddrinfo`, refuse if any address is private (v4 and v6, incl. 169.254/16 and 100.64/10); unresolvable = refused. Redirects followed manually, guard per hop, 5 max; `set_follow_location` off in generated HTTP tools. | `test_web_fetch: test_private_host_guard_resolves`, `test_redirect_to_private_host_is_refused` |
| 4 | Session tokens stored in clear in `auth_tokens`. | Store `sha256(token)`; existing sessions invalidate once (one re-login). | `test_users` (unchanged API), `integration.sh` expiry section |
| 5 | No throttle on `/api/login`; each guess cost the server ~200 ms of PBKDF2. | Per-address and per-username lock after 5 failures, doubling to 300 s, 429 + `Retry-After`, checked before hashing. | `integration.sh` "login throttle" |
| 6 | No request body cap; httplib buffers the body before the auth gate. | `set_payload_max_length(64 MB)`. | — |
| 7 | `/api/status` said `4.0.0`. | `FUNES_VERSION` from `project()`. | — |
| 8 | Recalled memories injected into the system prompt with no framing; `remember` stores model-chosen text from web pages verbatim. | Prompt frames them as records, not instructions. **Mitigation only** — the design fix (don't store tool-sourced text verbatim) is open. | — |

Still open from the same review, deliberately not done here: one httplib
worker thread per chat for the whole run (concurrency ceiling ≈ pool size);
MCP stdio servers spawned per request; DNS rebinding between the guard's
lookup and httplib's connect (needs address pinning httplib does not offer);
memory design (auto-memory is a conversation log, not fact extraction).

## 5.1 — the split

The review that produced 5.0.1 also measured the tree: 13.3k lines, of which
~2.5k (`harvest`, `issue`, `publication`, `rankings`, `structured`,
`pipeline`) and eleven of eighteen agents existed for one operator's
newsletter, debate pipeline, phone number, mailbox and manuscript. The core
now carries the harness and seven agents; everything with a personal path or
a personal identity lives in `funes-julio`, a sibling repository compiled in
with `-DFUNES_EXTENSIONS=<dir>` and loaded with a second `agents/` directory
(`FUNES_AGENTS_DIR` is colon-separated).

What the core gained to make that possible, and nothing more:

| Change | Where |
|---|---|
| Registration queue for out-of-tree tools: static initializer queues, `main()` applies with `{tools, memory, workspace_dir}` | `src/core/extension.{h,cpp}`, `CMakeLists.txt` (`FUNES_EXTENSIONS`, includes `<dir>/extension.cmake`) |
| `NativeTool::inline_result` — a tool declares its output exempt from the result store; replaces the core knowing `harvest_candidates` by name | `tools.h`, `result_store.h`, `agent.cpp` |
| Several agent directories | `FunesApi::agent_dirs`, `main.cpp` |
| `config/funes.conf` loses the Gmail/WhatsApp/publishing sections | → `funes-julio/config/funes.conf.example` |

What moved, verbatim apart from include paths: the six modules above and
their six tests; `publishing/`, `publications/`, `pipelines/`, `scripts/`;
`third-party/whatsapp-mcp` and `imap-email-mcp-patched`; the agents `curator`,
`voc-researcher`, `council-chair`, `council-panelist`, `content-writer`,
`mvp-builder`, `whatsapp-assistant`, `whatsapp-autoresponder`,
`gmail-assistant`, `rss-reader`, `book-editor` and `agents/templates/`.

The test that the split is clean: a core build with no extension passes all
28 tests and `integration.sh`; a build with the extension passes those plus
the extension's seven (`julio_*`).

The other question the review raised — whether a memory that is a
conversation log with vector search is the product's claim or its weakest part
— is still open, and is now the only substantive open question in this tree.

## 4.1 breakdown

| Step | Deliverable | State |
|------|-------------|-------|
| 1.1 | `pipelines/*.yaml` config + loader (`src/core/pipeline.{h,cpp}`) | done |
| 1.2 | `write_structured` / `read_structured` tools, schema-enforced | done |
| 1.3 | `merge_rankings` tool (Borda count in code, not in-context) | done |
| 1.4 | Retrofit the five VoC-pipeline agents onto 1.1–1.3 | done |
| 4.1 | `book-editor`: style ruleset out of the prompt into a style file | done |
| 4.2 | `astro-ph-summarizer`: fold into `rss-reader` as a configured feed | done |
| 5.1 | Archetype defaults documented in `agents/README.md` | done |

## 4.2 — per-user publications

Moved with the newsletter to the funes-julio repository (its README carries
the plan). Whether a publication belongs to a user or to the install is that
repository's question now; what the core owes it is already there — per-user
workspaces, `shared_identity`, the agent allowlist.

## 5.0 breakdown

| Step | Deliverable | State |
|------|-------------|-------|
| 1 | `memory_links` + `memory_link_judgments` + `memories_fts` in `migrate()` | done |
| 2 | `link_memories` / `unlink_memories` / `links_of` / `count_links` | done |
| 3 | Recall widening: one-hop expansion + term-match fill, append-only | done |
| 4 | `backfill_links()` + the background pass that feeds it | done |
| 5 | UI string tables (`ui/i18n/*.json`), `t(key)`, `data-i18n` | done |
| 6 | `users.locale` + `PUT /api/me/locale`, detected from `navigator.language` | done |
| 7 | Agent reply locale — a runtime instruction in `agent.cpp`, not per-YAML | done |
| 8 | `memories.lang`, detected at `remember()` time (`lang_detect.h`) | done |

Cross-lingual recall needs no code: a multilingual embedding model already
matches a Spanish query to an English memory, and `memories.lang` is
descriptive — nothing filters on it, so a misdetection costs a wrong label
rather than a lost memory.

Phases 1–4 are additive and safe by construction: with no links and no term
matches, recall returns exactly what 4.x returned. The link backfill is the
only slow operation in 5.0 — one model call per candidate pair, capped and
resumable, off by default.

Grounding checks for `content-writer` (the `evidence`-quote pattern from
`publish_issue`, applied to article claims) are listed in the plan under
phase 1 but land in 4.2: they need a fetch-cache the writer agent does not
have yet, and the pipeline primitive is useful without them.
