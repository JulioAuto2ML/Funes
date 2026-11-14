# Roadmap

Three improvement plans exist. This file says which order they run in and why.
It does not restate them — each links to its own document.

Current release: **7.20.0** (self-healing H2: `config/heal.yaml` says what to do when an incident opens — explain what a person should run, propose a runbook call («aprobar N»), or act at once (Funes's own WhatsApp bridge is restarted, since a dead bridge can carry no proposal); a call is a tool an agent already has, run as an approved proposal; act at most twice per incident a day, then it proposes). Before it, 7.19.0 (self-healing H1: `scripts/funes_guardian.py` — snapshot before a change, watch it, undo it by itself when Funes stops answering or an incident opens, "deshacer N" from the web chat or WhatsApp even with Funes down; `create_agent` goes through it, no snapshot no write; `funes_guardian.py deploy` is a watched deploy that undoes to the old commit and binary; `/clear` resets a WhatsApp chat like `/new`). Before it, 7.18.1 (`cancel_job` takes the job's name as well as its id and refuses a mismatch without naming the job, and is gated as external — the operator, asked to drop a reminder that was only ever proposal #3, had deleted job #3, the Spanish newsletter). Before it, 7.18.0 (self-healing H0: probes — library scripts with a `probe:` block plus the cron jobs' last runs — open one incident per failing check and send the admins a WhatsApp from Funes's number when it opens and closes, with quiet hours; an `incidents` tool; a cron job's `require_tools` makes a reminder that sent nothing a failed run). Before it, 7.17.0 (`notify_user`: a WhatsApp message to the run's own account from Funes's number, the recipient never an argument — reminders came from the person's own number through whatsapp-assistant, and one through `funes` would have reached nobody, a job's answer being only stored). Before it, 7.16.2 (newsletter: a post is grounded in its page only if the page carries its names and numbers — three or more missing, and at least 40% of them, fails it, after a post about models no page mentioned shipped linked to an unrelated article; an item can be `withdrawn` so neither the posts nor Sunday's carousel reuse it; the LinkedIn token is renewed from a WhatsApp link a week before it lapses, through `publishing/linkedin_token.py` and the `linkedin-callback` unit). Before it, 7.16.1 (the effects gate no longer proposes a call the tool would refuse — an ungranted or missing script, the shell when it is off — the model gets the error instead; a read-only `disk_usage` script for sysadmin). Before it, 7.16.0 (a chat run outlives its connection: a phone locking its screen no longer kills the run as "(stopped by user)" — it finishes and saves, the UI waits for it and shows the answer, the stream sends heartbeats, and the ■ button is an explicit `POST /api/chat/stop`). Before it, 7.15.0 (the two WhatsApp numbers kept apart by more than config: the autoresponder defaults to Funes's own bridge, its systemd unit is sandboxed away from the personal store, and agents on the installation's identity are admin-only, enforced). Before it, 7.14.0 (the effects gate counts what earlier turns of the conversation read — a send or a host change in a later turn is proposed or staged too; the journal script prints compact lines and the sysadmin agent gets two `read_result` calls). Before it, 7.13.0 (OS plan phase 7: a `sysadmin` agent — `system_health`, `unit_status`, `journal` read-only, `restart_unit` external, `prune_backups` host and so staged after outside content; scripts can be `admin_only`). Before it, 7.12.0 (OS plan phase 6, second half: overlay staging — a `host` script in a tainted run runs against an overlay in a user + mount namespace, its changes shown as a list and a diff, "aprobar N" applies exactly them and "rechazar N" throws them away). Before it, 7.11.0 (OS plan phase 6, first half: the approval gate — a host or external call in a run that has read outside content is stored as a proposal and runs only on "aprobar N"; scheduled jobs exempt; `effects:` on scripts and MCP servers). Before it, 7.10.0 (MCP stdio servers kept alive between runs: a run leases a started server from `McpPool` and returns it, idle ones stop after 10 minutes; the vendored client no longer reports a dead server as running or waits 60 s on it). Before it, 7.9.1 (the learned `connect:` lists: `news_headlines` [443], `tv_art` [8002], `tv_weather_art` [443, 8002, 8080]; `mcp-probe --learn` folds the whole log). Before it, 7.9.0 (OS plan phase 5: learn mode — a seccomp supervisor records what a script or MCP server touches, `funes script-learn` / `mcp-probe --learn` fold it into a proposed `access:` block, `learn: true` watches real calls). Before it, 7.8.1 (`funes ask` no longer hangs on an open, silent stdin) and 7.8.0 (OS plan phase 3: `funes.sock` with kernel-attested identity, `uid_users`, `funes ask`; config read into a private map instead of the environment; `LoadCredential=`).

## What is left, in priority order

Revised 2026-11-11. The memory plan is finished; the OS plan has phases 6–7
open (0–5 shipped, 3 after 4). Two decisions that reorder it: **the machine is yoda, always** (so
Pillar 2 is about a headless server, not a desktop), and **yoda's embedding
server is mostly idle** — recall uses it, nothing else does — so the machine
memory is the place to spend it.

| # | Item | Why here | Size |
|---|------|----------|------|
| 1 | **Watch the first sandboxed runs on yoda**, and try `funes ask` there (`funes uid-map julio julio` first): 06:30 `news_headlines` (its `[tool]` line should end in `{exec-… cpu=… mem=…}`), the first `tv_art` and `speak` calls, and one call each through the WhatsApp, Gmail and RSS agents (`MCP server '…' stopped {…}` lines). A `Permission denied` is a missing `access:` entry: check with `funes script-run` / `funes mcp-probe`. | Phases 1–2 and 7.6 are only proven where they ran. | checks, no code |
| 2 | **OS phase 4, second half**. Built in 7.7 (see the release table): the account, the ring, the collector, baselines, promotion, links, both tools. Left: watch a week of yoda's real events and tune what is promoted (thresholds, which units are noise); a daily *digest* memory ("yesterday: 3 deploys, dream-mdns failed all day") so a quiet day is remembered too; and the system journal (OOM kills, system units) if julio is added to `systemd-journal`. | The machine memory exists only as far as its first week on yoda says it works. | ~1 week |
| 3 | **Finish phase 5 on yoda**. Done 2026-11-11 for five scripts and the three MCP agents (`connect:` lists in 7.9.1; the RSS, Gmail and WhatsApp grants held up). Left: `speak` and `publish_briefing` (both act on the world when run — learn them with `learn: true` on a real call), and `tv_art show` / `tv_weather_art publish`, whose ports were taken from the code. | Unwatched modes are the ones a wrong `connect:` would break. | hours |
| 4 | **Concurrency ceiling**: one httplib worker thread per chat for the whole run. | Open since the 5.0.1 review. Matters when several people (or the autoresponder plus a person) chat at once; not seen to bite yet. | ~1 week |
| 5 | Watch the first real proposals on yoda (`[gate]` lines; whatsapp-assistant reading a chat before sending). Overlay staging for `host` shipped in 7.12. | The gate's policy was chosen on paper. | ongoing |
| 6 | Try the `sysadmin` agent on yoda for real (7.13): a staged `prune_backups`, a proposed restart after reading the journal. The OS plan is otherwise done. | First real `[gate]` traffic. | hours |
| — | 4.2 (per-user publications, generic channel adapter, MCP self-service) | Deferred: nothing has asked for a second channel or a second publication owner. | — |
| — | 8.0 voice | Deprioritised 2026-11-10. | — |

## Release history

| Release | Theme | Source plan | Status |
|---------|-------|-------------|--------|
| **4.1** | Generalization: config over prose, tools over model arithmetic | generalization plan, phases 1, 4, 5 | shipped |
| **4.2** | Per-user publications; generic channel adapter; self-service pointed at MCP | same plan, phases 2, 3 | deferred |
| **5.0** | Connected memories + localization | v5 plan (8 phases) | done |
| **5.0.1** | Security: shell-job permission bypass, child env allowlist, resolving SSRF guard, hashed tokens, login throttle, body cap | review of 2026-09-18 | done |
| **5.1** | Split (undone in 5.2): the core keeps the harness, `funes-julio` takes one operator's tools, agents and deployment; `src/core/extension.h` is the seam | same review | done |
| **5.2** | One checkout: funes-julio merged back with history, `src/julio` always built, all runtime state under `data/`, units renamed `funes` / `whatsapp-autoresponder` | — | done |
| **7.0** | OS plan phase 0: connections pinned to the address `net_guard` judged, `Type=notify` + self-probing watchdog, background passes yield to people (in-flight runs + PSI); link backfill on by default | [os-plan.md](os-plan.md) | done |
| **7.4** | OS plan phase 1: `funes _exec` trampoline, a cgroup per child process, limits from `FUNES_CHILD_*` and manifest `limits:`, usage on the `[tool]` line | [os-plan.md](os-plan.md) | done |
| **7.5** | OS plan phase 2: Landlock + seccomp in the trampoline, rules from the workspace + manifest `access:`, private `$TMPDIR`, `funes script-run` | [os-plan.md](os-plan.md) | done |
| **7.6** | MCP stdio servers through the trampoline: own cgroup, sandbox from the server's `access:` block, `funes mcp-probe` | [os-plan.md](os-plan.md) | done |
| **7.7** | OS plan phase 4: `machine` system account, `host_events` ring, collector (user journal, dpkg, boot/kernel/deploys, OOM counter, llama-server pid files, watched files, PSI, temperatures, VRAM, disks, every child's cost), baselines, promotion + links, `host_changes` / `machine_recall`, `funes machine-probe` | [os-plan.md](os-plan.md) | done |
| **7.8** | OS plan phase 3: `funes.sock` (SO_PEERCRED → `uid_users`, own children refused), `funes ask` (stdin framed + tainted), config in a private map instead of the environment, `LoadCredential=` | [os-plan.md](os-plan.md) | done |
| **7.9** | OS plan phase 5: learn mode — seccomp user-notification supervisor in the trampoline, the fold into a proposed `access:` block, `funes script-learn`, `mcp-probe --learn`, `learn: true` manifests | [os-plan.md](os-plan.md) | done |
| **7.10** | MCP stdio servers kept alive between agent runs (`mcp_pool.h`): exclusive leases, idle reaping, a failed call discards its server; vendored client patches (dead server not "running", pending calls fail at once, request registered before it is written, `poll()` instead of a 10 ms spin) | — | done |
| **7.11** | OS phase 6, first half: effect classes (`effects:` on manifests and MCP servers, built-ins listed), the approval gate in `dispatch_tool` for tainted runs, `proposals` table, "aprobar/rechazar N" answered by `/api/chat` and `funes ask`; taint inherited across delegation; scheduled jobs exempt | [os-plan.md](os-plan.md) | done |
| **7.12** | OS phase 6, second half: overlay staging for `host` scripts in tainted runs (`staging.h`: user + mount namespace in the trampoline, overlayfs over the manifest's write directories, collect/diff/commit/discard); approving a staged proposal commits it instead of re-running | [os-plan.md](os-plan.md) | done |
| **7.20** | Self-healing H2: `heal.h` + `config/heal.yaml` — explain / propose / act per incident key, runbooks as approved proposals, the act limits | self-healing plan, H2 | done |
| **7.19** | Self-healing H1: the guardian (`scripts/funes_guardian.py`, `funes-guardian.timer`, `src/core/guardian.h`), `create_agent` guarded, "deshacer N", watched deploys | self-healing plan, H1 | done |
| **7.18** | Self-healing H0 (`docs/self-healing-plan.md`): `probes.h`, `incidents.h`, `probe_services` / `probe_internet` / `probe_newsletter`, the `jobs` probe, `incidents` tool, `require_tools` on cron jobs | self-healing plan, H0 | done |
| **7.17** | `notify_user` (`tools/notify_tool.h`): reminders from Funes's own number to the caller's mapped jid; `UserStore::jid_for_user` | — | done |
| **7.16** | Chat runs outlive the connection: `ChatRuns` registry (`chat_runs.h`), `POST /api/chat/stop`, `GET /api/chat/active`, SSE heartbeat; the UI waits for a dropped run and reloads history | — | done |
| **7.15** | WhatsApp separation: autoresponder default bridge 8091, its unit sandboxed (`ProtectHome=tmpfs`, only its store/config/state/workspace mounted), `shared_identity` agents admin-only in `allows_agent`; `sysadmin` declares one | — | done |
| **7.14** | Taint across turns (`turns.origin`, `RunProvenance::earlier`), from the first real sysadmin run; compact journal output | [os-plan.md](os-plan.md) | done |
| **7.13** | OS phase 7: the `sysadmin` agent (five scripts, `admin_only:` manifests) | [os-plan.md](os-plan.md) | done; the OS plan is complete |
| **7.1** | Memory plan phases 1–3: recall eval (`tests/memory_eval`), bounded writes, provenance + taint (closes 5.0.1 #8) | [memory-plan.md](memory-plan.md) | done |
| **7.2** | Memory plan phases 4–5: exchanges queued and reduced to facts by a gated background pass; newer facts and `contradicts` verdicts supersede older ones, reversibly | [memory-plan.md](memory-plan.md) | done |
| **7.3** | Memory plan phases 6–7: `funes memory-migrate` (logs → facts, archived, undoable), uncited memories archived after a window | [memory-plan.md](memory-plan.md) | done; yoda migrated and on `extract` since 2026-11-10 |
| **8.0** | Voice: STT + TTS sidecars | v6 voice research | deprioritised |

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

**OS plan before voice.** Reordered 2026-11-10. Phases 0–2 of the OS plan
are security debt in the sense 5.0.1 was — a prompt currently carries a
security boundary (`operator`'s warning that `execute_shell` is not confined),
and an allowlist is described as "not a sandbox" — and by the principle that
put 5.0.1 ahead of everything, they outrank a feature. Voice keeps its research
and moves behind them; it was numbered 6.0 and is now 8.0, so 6.x is unused.

**5.0 before voice.** Voice is additive and independent, but on a single-host
install it needs CPU headroom the memory backfill also wants, and connected
memories
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
| 8 | Recalled memories injected into the system prompt with no framing; `remember` stores model-chosen text from web pages verbatim. | Prompt frames them as records, not instructions. **Closed in 7.1:** a `remember` after third-party content is tainted — weight capped at 1.0, never a link anchor, shown as "per <origin>: …" (memory plan, phase 3). | 7.1 |

Still open from the same review: one httplib worker thread per chat for the
whole run (item 4 in the list at the top). Since closed: MCP stdio servers
spawned per request (7.10, `mcp_pool.h`), DNS rebinding (7.0, `net_guard.h::pin_address`),
the memory design (memory plan, 7.1–7.3: facts, not a conversation log), and
MCP stdio servers running unconfined (7.6).

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
their six tests; the publishing, publication, pipeline and deployment-script
directories; the two vendored MCP servers; and eleven agents — a newsletter
curator, a five-agent debate pipeline, a WhatsApp pair, a mail assistant, a
feed reader and a manuscript editor — with their prompt templates.

The test that the split is clean: a core build with no extension passes all
28 tests and `integration.sh`; a build with the extension passes those plus
the extension's seven (`julio_*`).

The split was undone in 5.2 (nothing is going to be sold): funes-julio was
merged back with its history and builds as `src/julio`; `extension.h` stayed
as the registration seam. The other question the review raised — whether a
memory that is a conversation log with vector search is the product's claim
or its weakest part — was answered by the memory plan (7.1–7.3).

## 4.1 breakdown

The pipeline primitive, `write_structured` / `read_structured`,
`merge_rankings` and the agents retrofitted onto them were built for one
operator's pipelines and live in `src/julio`. The part that was never
pipeline-specific is the archetype defaults in
[agents/README.md](../agents/README.md).

## 4.2 — per-user publications

Deferred. Whether a publication belongs to a user or to the install is open;
what the core owes it is already there — per-user workspaces,
`shared_identity`, the agent allowlist.

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
