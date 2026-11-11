# Funes in the OS — research and plan

Status: **phases 0–2 done** (7.0, 7.4, 7.5; 2026-11-10/11); phase 3 is next. Facts about
yoda were checked on 2026-11-10 (see "Open questions", 2); anything still
marked *(unverified)* was not.

## The question, restated

Funes began as two ideas: a personal assistant, and AI embedded as deep as
possible in a Linux system. Five releases built the first one. Of the second,
what exists is `execute_shell`, `run_script` and a cron table — a web
application that happens to be able to start processes. Nothing in the tree
needs C++ yet: HTTP, SQLite and JSON are the same work in any language. This
plan is the first that would cash that cheque, because everything in it is a
direct syscall or a kernel interface with no FFI in between.

"As deep as possible" is not a goal, though, and taken literally it leads
somewhere wrong. So the first job is to decide what depth is *for*.

## What "deep" should not mean

Three directions were researched and rejected.

| Direction | State of the art | Why not |
|---|---|---|
| **The model in the kernel's path** — inference as a kernel primitive, LLM-tuned schedulers | ProbeLogits (2026, single-author preprint); assorted "AI-native OS" projects | A kernel decision takes microseconds, a model call takes seconds and is sometimes wrong. Anything a model decides per-syscall is either too slow to enforce or too unreliable to trust. This is the "don't ask the model for what a tool can supply" incident, one layer down. |
| **A new agent OS** — AIOS, WASM-isolate "agent kernels" | AIOS, Rivet agentOS, nanos | They reimplement scheduling, isolation and IPC in userspace for agents. Linux already has all three, kernel-enforced. A single-host household install gains nothing from a second, weaker kernel. |
| **A tool per subsystem** — `systemctl`, `journalctl`, `nmcli` behind MCP | A dozen systemd-MCP servers already exist | Useful, commodity, and not depth: it is `execute_shell` with nicer names. It also gives a 13B local model root-adjacent verbs with nothing underneath to catch a mistake. Buildable in a weekend *after* the layers below exist; dangerous before. |

## The thesis

> **The model never sits in the kernel's path. It compiles to it.**

Funes already works this way one level up: agents are config, grants are
written down, isolation lives in the SQL statement rather than in the handler.
Every one of those is a declaration that a deterministic layer enforces while
the model is absent. The OS plan is the same move with the kernel as the
enforcing layer:

- what an agent **may do** becomes kernel policy (Landlock, seccomp, cgroups),
  not a path check in Funes's own code;
- what the machine **did** becomes memory, attributed to the tool call that
  caused it by the kernel, not by a log-correlation heuristic;
- what an agent **wants to change** becomes a proposal the filesystem can
  discard, not an action it has to be trusted with;
- who is **asking** is answered by the kernel (`SO_PEERCRED`), not by a token.

The model's role in each is the slow, reviewable one: reading evidence,
proposing a policy, explaining a diff. Enforcement never waits for it.

## What Funes already has that makes this cheap

| Existing piece | What it becomes |
|---|---|
| `proc::run_argv` — one fork/exec path for scripts, shell, MCP stdio | the one place a sandbox has to be applied |
| script manifests (`params`, `env`, `output`) — "the model supplies arguments, never a command" | the place a script declares what it touches |
| `fs_guard::workspace_for` — one resolver for every path | the source of a Landlock ruleset |
| `child_environment()` allowlist | same idea, extended from env to filesystem and network |
| `tool_log`, run/step ids | the join key for kernel events (see Pillar 1) |
| `MemoryStore`: `source` weights, links with recorded judgments, consolidation, per-`user_id` partitions | the store for a machine's history |
| result store — big outputs out of transcript, by id | where a pending diff lives |
| cron's fire-time permission re-resolution | the model for "revoking stops the timer" on any standing grant |
| `jid_users` — service token says *trusted*, jid says *who for* | `uid_users` — the socket says *who*, nothing else needed |
| `systemd --user` service | a delegated cgroup subtree, for free, without root |

The constraint that shapes everything: **Funes runs unprivileged**, as a user
unit. eBPF (AgentSight's approach: intercept TLS to recover intent, trace
syscalls for effect) needs root. Everything below is chosen to work without it.

## Pillar 1 — Grants become kernel policy

Today's honest sentence in `scriptlib/README.md` is "this is an allowlist, not
a sandbox", and `operator`'s prompt has to tell the model that `execute_shell`
is not confined. A prompt is carrying a security boundary. The fix is that the
grant *is* the sandbox.

**Mechanism.** A hidden `funes _exec` subcommand: `run_argv` execs the same
binary with a policy on an inherited fd; the trampoline applies it and execs
the real program. This keeps the existing rule that the child does nothing
between `fork` and `exec` but exec, keeps the single binary, and puts
`landlock_restrict_self` in a process that is about to be replaced anyway.

The trampoline applies, in order:

1. **cgroup** — the parent creates `…/funes.service/run-<run>-<step>/` in its
   delegated subtree and moves the child in. `memory.max`, `pids.max`,
   `cpu.weight` from the manifest; `cgroup.kill` replaces `kill(-pgid)`, which
   a double-forking child escapes today.
2. **Landlock** — filesystem rules derived from what Funes already knows: the
   caller's workspace read-write, the interpreter and library paths read-only,
   plus an optional manifest `access:` block. TCP rules by port (ABI 4+);
   signal and abstract-socket scoping (ABI 6+). Rights are masked against the
   running ABI so an older kernel degrades instead of failing.
3. **seccomp** — a small static denylist (`ptrace`, `mount`, `bpf`, `keyctl`,
   module loading, `io_uring`), not a per-script allowlist nobody will maintain.

**The novel part: intent-stamped effects.** AgentSight exists because an
outside observer cannot tell which LLM decision caused which syscall — the
"semantic gap" — and it closes that gap with timing heuristics. Funes is not
outside. It names the cgroup after the run and step, so every kernel-side fact
about that cgroup (peak memory, OOM kill, CPU time, PSI, a Landlock denial in
the audit log) is attributed to one tool call *by construction*. The cgroup
path is the join key between intent and effect. No eBPF, no root, no guessing.

**Second novel part: learned least privilege.** Nobody writes minimal sandbox
profiles by hand, which is why most sandboxes ship permissive. So:

1. A new script runs in **learn mode**: sandboxed widely, with every file and
   socket it touches recorded. Unprivileged route: a seccomp user-notification
   supervisor (the Sandlock design) observing `openat`/`connect`; simpler
   fallback: Landlock audit denials (ABI 7, Linux 6.15) — **not available on
   yoda** (6.12, ABI 6), so seccomp user-notification is the route there.
2. The observations are plain data. A deterministic tool folds them into a
   candidate `access:` block; the model's only job is to explain it and flag
   what looks wrong ("why does a TV script read `~/.ssh`?").
3. An admin commits the manifest — the same "installing a script is an admin
   editing the repo" rule as today.
4. From then on the kernel enforces it, and a denial is a recorded event that
   says *this script changed behaviour*, which is a supply-chain signal
   `pip`-installed dependencies otherwise never give you.

The model is in the review seat, not the enforcement seat.

**What this does not fix, stated so nobody assumes it:** Landlock's network
rules are per-port, not per-address, so it does not close the DNS-rebinding
gap `ROADMAP.md` lists as open. That one has a cheaper fix — see Phase 0.

## Pillar 2 — The machine as a memory owner

Borges's Funes remembered every leaf on every tree and could not think,
because thinking is forgetting differences. `journald` is already that Funes.
What no Linux box has is the other half: a memory of the machine that
*abstracts* — baselines, changes, and which change preceded which failure.

**Design.** A reserved account, `machine`, owns a memory pool like any user's
(so isolation and the partitioned vector index apply unchanged, and
`test_user_isolation` gains a case: only admins recall it). A collector thread
ingests structured events — no free text where a field will do:

| Source | Interface | Privilege |
|---|---|---|
| unit state changes, failures, restarts | systemd Varlink `io.systemd.Unit` (v258+; yoda has 257) or D-Bus signals; `journalctl -o json --follow` as the dependency-free fallback — on yoda, the route | user units: none; system units: `systemd-journal` group (julio is not in it) |
| package installs/upgrades | `/var/log/dpkg.log` via inotify | read |
| boots, kernel version, OOM kills | journal fields | as above |
| pressure | `/proc/pressure/*`, per-cgroup `*.pressure` | none |
| disk, SMART, temperatures | `statvfs`, sysfs | none |
| Funes's own runs | Pillar 1 cgroup stats | none |

Three rules, each from an incident class this repo has already met:

- **Events are not memories.** Raw events go to a plain ring table with a
  retention window. Only two things are promoted to `memories`: a *change*
  ("`llama-server` unit file edited", "kernel 6.17 → 6.18") and an *anomaly
  against a baseline* ("backup took 41 min; median 4"). Baselines are
  arithmetic in C++, never the model's.
- **Links are the product.** "What changed before it broke" is a link between
  a change and a failure. Candidate pairs come from time adjacency and shared
  unit/package names, and go through the existing `backfill_links` judge with
  its recorded negatives. A deterministic `host_changes(since, unit?)` tool
  answers the common question with no model call at all.
- **Log text is hostile input.** Any process can write to the journal; an SSH
  login attempt puts an attacker-chosen username in it. Roadmap item 5.0.1 #8
  (tool-sourced text stored verbatim) was closed in 7.1 by provenance and
  taint (memory-plan.md, phase 3); machine memories should carry
  `tainted=1` with their unit as origin, which is that mechanism reused. Machine memories are built from *fields* (unit, exit code,
  version), and message text is stored quoted, length-capped and never
  promoted to `source=user` weight.

What this buys that a log viewer does not: `funes` answering "why is the
newsletter late?" with "the embedding server was OOM-killed at 05:58, eleven
minutes after `llama.cpp` was rebuilt; it has been killed after 2 of the last
3 rebuilds" — each clause a recalled record with an id, not a guess.

## Pillar 3 — Effects as proposals

The gap between "read-only tools" and "`execute_shell`" is where every useful
sysadmin task lives, and today the only thing in it is trust. Recent work
(BranchFS, DeltaBox — both 2026) shows filesystem branching with commit and
rollback at millisecond cost. Funes needs less than they build.

Each manifest declares an `effects:` class:

| Class | Meaning | How it runs |
|---|---|---|
| `none` | reads only | Pillar 1 sandbox, read-only rules |
| `workspace` | writes inside the caller's workspace | sandbox; no ceremony (today's behaviour, now enforced) |
| `host` | writes files outside it | **staged**: runs against an overlay in a user+mount namespace; the upper layer is the diff |
| `external` | talks to something that cannot be un-told (send mail, restart a unit, call an API) | needs approval *before* running, or a declared `undo:` script |

For `host`, the run's result is a diff stored in the result store. The model
sees a summary and a `result_id`; the person sees the diff in the UI (or a
one-line summary on WhatsApp) and commits or discards. Commit replays the
upper layer; discard deletes a directory. The completion contract then gets a
new, honest state: *proposed, not applied* — a run cannot report a change as
done while it is still a diff.

`external` gets no magic. Network effects cannot be rolled back and both
papers above list that as future work. The plan's position is that pretending
otherwise is worse than an approval prompt.

**Risk checked:** unprivileged user namespaces are restricted by AppArmor on
recent Ubuntu, but yoda is Debian 13 and `unshare -Ur true` succeeds as julio.
On a host that does block them, the fallback is a copy-on-write
staging directory for scripts that take their target root as a parameter —
less general, zero privilege.

## Pillar 4 — Funes as a system service

Depth in the other direction: not Funes reaching into the OS, but the OS
reaching Funes.

- **A Unix socket with kernel-attested identity.** `$XDG_RUNTIME_DIR/funes.sock`
  (and optionally a system-wide one), `SO_PEERCRED` → `uid_users` → account.
  Same shape as `jid_users`, minus the service token: the kernel already
  vouches for the peer. Every local program gets memory and a model with no
  credential to leak.
- **`funes ask`** — a subcommand that is a client of that socket.
  `journalctl -u foo -n 200 | funes ask "what went wrong"`. Stdin is framed as
  data, as recalled memories are. This is the Unix-shaped interface the web UI
  never was: composable with pipes.
- **Proper systemd citizenship.** `Type=notify` + watchdog (a hung LLM call
  today looks healthy to systemd), socket activation, and
  **`LoadCredential=`** for secrets. That last one removes the root cause of
  5.0.1 defect #2: secrets stop being environment variables at all, so there
  is nothing for a child to inherit and `child_environment()` becomes
  defence in depth rather than the only wall.
- **Pressure-aware background work.** `FUNES_LINK_BACKFILL` is off by default
  because it competes with chat for the GPU. Gate the background passes on
  Funes's own in-flight request count (it knows when the model is busy) and
  on PSI for CPU/IO. Then the backfill can default to on, and 5.0's connected
  memories stop depending on someone remembering to enable them.
- Publish the interface in systemd 260's Varlink registry
  (`/run/varlink/registry/`) — not on yoda's 257; deferred until it is.

## Order of work

Ordered so each phase is useful alone and none requires the next.

| Phase | Deliverable | Depends on | Size |
|---|---|---|---|
| **0** ✅ | Close the DNS-rebinding gap: the vendored httplib *does* have `set_hostname_addr_map` (ROADMAP says it has no pinning) — pin the address `net_guard` approved. Add `Type=notify` + watchdog. PSI/in-flight gating for background passes. | — | done in 7.0 |
| **1** ✅ | `funes _exec` trampoline: cgroup per tool call, `cgroup.kill`, resource limits; cgroup stats on the `tool_log` line | 0 | ~1 week |
| **2** ✅ | Landlock + seccomp in the trampoline; rules from `fs_guard` + manifest `access:`; ABI-masked. `execute_shell` confined to the workspace for real; `operator`'s prompt loses its warning | 1 | ~2 weeks |
| **3** | `funes.sock`, `uid_users`, `funes ask`; `LoadCredential=` | — (parallel) | ~1 week; **after 4** (2026-11-11) |
| **4** ✅ | Machine account, event ring, collector, baselines, `host_changes`; promotion rules; isolation test | 1 (for own-run stats) | done in 7.7 |
| **5** | Learn mode → proposed `access:` blocks | 2 | ~2 weeks |
| **6** | `effects:` classes, overlay staging, diff in result store, commit/discard in UI | 2 | ~3 weeks |
| **7** | *Only now:* a `sysadmin` agent with read tools over systemd/journal, and mutating scripts classed `external` | 2, 4, 6 | small |

Reordered 2026-11-11: phase 4 runs before phase 3. The machine is yoda, and
yoda's embedding server sits idle outside recall — the machine memory is what
it is for. Phase 3 is independent and loses nothing by waiting.

Phase 7 is the thing most projects build first. It is last here on purpose:
by then a wrong call is confined, attributed, remembered and, where the
filesystem allows, undoable.

Tests follow the house rule — every claim gets one that fails without the
code. Phase 2's are the important ones: a script that tries to read
`config/funes.local`, another user's workspace, and `memory.db` must get
`EACCES` from the kernel with `fs_guard` *deliberately bypassed* in the test,
or the sandbox is only being tested through the layer it is meant to back up.

## Phase 0 — what was built (7.0)

| Piece | Where | Test |
|---|---|---|
| `check_host` returns the address it judged (IPv4 preferred — pinning gives up httplib's walk down the address list, and a v6 pin on a host with no v6 route would fail where the unpinned connect fell back); `pin_address` makes httplib connect there. `web_fetch` (every redirect hop) and generated HTTP tools use it. | `src/core/tools/net_guard.{h,cpp}`, `page_text.cpp`, `http_tool_runtime.cpp` | `test_web_fetch: test_connection_is_pinned_to_checked_address` — a name only the guard's (test) resolver knows; an unpinned client resolves it again, finds nothing and fails |
| `READY=1` once the bound server accepts; `WATCHDOG=1` every `WATCHDOG_USEC/2` *only while a request to itself is answered*; `STOPPING=1` on exit. No libsystemd — one datagram. Unit: `Type=notify`, `NotifyAccess=main`, `WatchdogSec=120`. | `src/core/sd_notify.{h,cpp}`, `src/server/main.cpp`, `scripts/funes.service` | `test_host_signals` (path and abstract sockets, `WATCHDOG_PID`, ping withheld while the probe fails); `integration.sh` "systemd notify" against the real binary |
| `RunInFlight` held by every `FunesAgent::run`; `wait_until_quiet()` (no run in flight, PSI cpu/io `some avg10` ≤ 20%, give up after 30 min) before each consolidation merge and each link judgment. Unreadable PSI counts as quiet. Link backfill now defaults **on**. | `src/core/busy_gate.{h,cpp}`, `agent.cpp`, `main.cpp`, `config/funes.conf` | `test_host_signals: test_quiet_means_no_runs_and_low_pressure`, `test_agent_run_counts_as_in_flight` (counted during the model call, released when the run throws) |

What it does not cover: MCP HTTP/SSE servers and the LLM/embedding clients are
not pinned — their URLs come from an admin, not a model, so the guard does not
apply to them at all. `publishing/` scripts that call the model directly are
invisible to the in-flight count (PSI may or may not see them; it does not see
GPU load).

## Phase 1 — what was built (7.4)

| Piece | Where | Test |
|---|---|---|
| `funes _exec --cgroup=DIR -- PROG ARGS`: joins DIR (fails closed, 126) then execvp (127 on failure, as before). Dispatched first in `main()`, before config or database. `run_forked` builds the wrapped argv and envp before the fork, so the fork child still only dup2s, chdirs and execs. | `src/core/tools/process_runner.{h,cpp}`, `src/server/main.cpp` | `test_cgroup` (the test binary is its own trampoline) |
| `cgroup::setup()`: only with `FUNES_CGROUPS=1`, only when the process is *alone* in a writable cgroup; moves itself into `server/`, enables `memory pids cpu` for children. One cgroup per child, `exec-<n>-<program>`: `memory.max` + `memory.swap.max=0` + `memory.oom.group=1`, `pids.max`, `cpu.weight`. After the child is reaped: anything left is `cgroup.kill`ed, counters read, directory removed. Timeout = `cgroup.kill` + the old group kill. | `src/core/cgroup.{h,cpp}` | `test_cgroup`: child placed in its own cgroup and the cgroup removed; a `setsid` grandchild does not outlive the call; timeout kills the tree; `pids.max` and `memory.max` (OOM) enforced; CPU/memory counted; concurrent children get distinct cgroups; a shared cgroup is refused untouched |
| Limits: `FUNES_CHILD_MEMORY_MB` (4096), `FUNES_CHILD_PIDS_MAX` (512), `FUNES_CHILD_CPU_WEIGHT` (50); a manifest's `limits: {memory_mb, pids, cpu_weight}` overrides each. | `cgroup::default_limits`, `script_library.cpp` | `test_script_library: test_manifest_rules` |
| `ToolResult::usage` → `… → ok {exec-7-python3 cpu=1.20s mem=88.0MB pids=2}` on the `[tool]` line. | `tools.h`, `tool_log.{h,cpp}`, `agent.cpp`, `shell_tool.cpp`, `script_tools.cpp` | `test_cgroup: test_tool_line_carries_usage` |
| Unit: `Delegate=yes`, `FUNES_CGROUPS=1`. Startup prints `Children: a cgroup each in …` or why not. | `scripts/funes.service` | on yoda |

**"Writable" is not "delegated".** The first version moved every pid in its
own cgroup into `server/`. Run from a desktop terminal, the test found the
terminal app's scope writable (the user owns it) and moved the whole app,
which its session manager then tore down. systemd leaves no marker an
unprivileged process can read for `Delegate=yes`, so the check is the one that
matters: the process must be alone in its cgroup, and the switch is explicit.

Not covered: MCP stdio servers (spawned by the vendored client, not
`run_argv`) stay in `server/`; the cgroup is named after `argv[0]`, so a
script with an interpreter shows as `exec-<n>-python3` — the `[tool]` line's
key (`run_script:tv_art`) names it. yoda's user manager delegates
`cpu memory pids`, no `io`, so there is no `io.max`.

## Phase 2 — what was built (7.5)

| Piece | Where | Test |
|---|---|---|
| `sandbox::apply` in the trampoline, after the cgroup join: `no_new_privs`, a Landlock ruleset masked to the running ABI (fs rights; TCP connect when a manifest lists ports, ABI 4; signal + abstract-socket scoping, ABI 6), then a seccomp denylist (ptrace, mount and the new mount API, bpf, keyctl, module loading, kexec, perf_event_open, userfaultfd, process_vm_*, io_uring; foreign-arch calls killed). Fails closed (126). The Landlock uapi is declared locally — the build host's headers may be older than the kernel. | `src/core/sandbox.{h,cpp}`, `process_runner.cpp` | `test_sandbox` (with fs_guard nowhere in the path, and a no-policy control for each refusal; 7 checks fail with `apply` stubbed out) |
| Base policy: `/usr /bin /sbin /lib* /opt` read+exec, `/etc /run /proc /sys` read, a few `/dev` nodes, the workspace read-write, and a private `funes-exec-XXXXXX` temp dir (`TMPDIR`, removed after). `/tmp` itself is not granted. | `sandbox::for_workspace`, `run_forked` | `test_private_tmpdir` |
| Who gets what: `run_script` = base + the script's library dir (rx) + manifest `access: {read, write, exec, connect}`; `execute_shell` and cron shell jobs = base; `pdftotext`/`pdftoppm` = base + the PDF (r) + render dir (rw). `publish_issue`'s script stays unconfined: trusted pipeline code, no model-chosen argv. | `script_tools.cpp`, `shell_tool.cpp`, `cron_runner.cpp`, `pdf_extract.cpp`, `script_library.cpp` | `test_execute_shell_is_confined`, `test_script_gets_its_declared_access` |
| Shipped manifests: `tv_art`/`tv_weather_art` write `${FUNES_ROOT}/data/tv`, `publish_briefing` writes `${FUNES_NEWS_FEED_DIR}`, `speak` reads `${HOME}/.config/pulse`. | `scriptlib/*.yaml` | `funes script-run` on yoda |
| `funes script-run <name> --user <u> [--param=value]` — a script exactly as `run_script` runs it, no model. | `src/server/maint_cli.cpp` | integration: the real binary's `_exec` reads its grant and is refused next door |
| `execute_shell`'s description says whether it is confined (it is generated at startup); `operator`'s prompt no longer carries the "NOT confined" warning. | `shell_tool.cpp`, `agents/operator.yaml` | — |

What the kernel checks that fs_guard never could: `/proc/<server pid>/mem`.
`funes.local` is `setenv`'d into the server's heap, and with yama's
`ptrace_scope` at 0 (yoda) any process of the same user could open that file
and read it. Landlock's ptrace rule refuses it to a sandboxed child
(`test_parent_environment_is_unreadable`).

Not covered: the network is unrestricted unless a manifest lists `connect:`
ports (no shipped script manifest does yet — phase 5's learn mode is where
those come from); MCP stdio servers were neither in a cgroup nor sandboxed
(closed in 7.6, below); a pathname unix socket is reachable if its file is (Landlock's ABI ≤ 6 does not
gate connect on them).

## 7.6 — MCP stdio servers

The last children that ran as the server: the vendored `mcp::stdio_client`
forked and exec'd them itself, so the WhatsApp, IMAP and RSS servers had the
server's cgroup and its whole filesystem — `memory.db`, every workspace, and
`/proc/<server pid>/mem` with `funes.local` in it.

| Piece | Where | Test |
|---|---|---|
| Vendored client: a per-instance launch wrapper, called in the parent before fork with the command split into argv (the split no longer happens in the forked child); `on_exit` runs once the server is reaped. | `third-party/cpp-mcp` (`set_launch_wrapper`) | — |
| `proc::wrap()` — the trampoline argv, cgroup scope and private temp dir, shared by `run_forked` and the MCP client so they cannot drift. | `process_runner.{h,cpp}` | existing sandbox/cgroup tests |
| A stdio server gets `sandbox::base()` (the system, `/dev` nodes, private `TMPDIR`) plus its `access:` block in the agent YAML — same parser as a script manifest's (`parse_access`). No workspace: these servers act for the installation. Its cgroup kills what it leaves behind (npx's node child) and its cost is logged when it stops. | `mcp_launch.{h,cpp}`, `agent_config.cpp`, `script_library.cpp` | `test_mcp_server_is_confined` (a Python MCP server reads its grant, is refused another account's file and `funes.local`; unconfined control reads both); `test_agent_config` |
| Grants: `whatsapp-mcp` — uv + its Python and cache, its project dir, the bridge store read-only, workspaces read-only (send_file), TCP 8090 only. `imap-email-mcp` — its package, TCP 993/465 only. `rss-reader-mcp` — `~/.npm`, TCP 80/443. | `agents/{whatsapp-assistant,gmail-assistant,rss-reader}.yaml` | `funes mcp-probe <agent> [--call=…]` on yoda |

## Phase 4 — what was built (7.7)

| Piece | Where | Test |
|---|---|---|
| A `system` role: the `machine` account owns a memory pool and nothing else — `!` for a password, skipped by `find_by_username` / `find_by_id` / `list_users` / `count` / `verify_login` and by every update and delete, so no login, token, CLI command or jid mapping reaches it. Never created before a person exists: #1 is who the pre-4.0 migrations give legacy data to. | `users.{h,cpp}` | `test_host: test_system_account_cannot_be_used`, `test_user_isolation: test_machine_pool_is_its_own` |
| `host_events` (folded rows: first/last seen, count) and `host_state` (cursors, offsets, last-seen values) in memory.db, on their own connection. A failure that keeps happening is one row while it does, and a new row once a day. Samples kept `FUNES_MACHINE_EVENT_DAYS`, the rest four times as long. | `host_events.{h,cpp}` | `test_failure_folds_and_links_to_the_change_before_it` |
| Sources, all unprivileged: the user journal through `journalctl --user -o json`, filtered *at the source* to systemd's lifecycle catalog ids + priority ≤ 3 (yoda's journal is ~99% access logs), cursor kept; `dpkg.log` from a saved offset (rotation-aware, history before first sight skipped); boot id, kernel, Funes's version; `/proc/vmstat` oom_kill; `/tmp/llama_pids` + `/proc/<pid>/cmdline` (started / restarted / model swapped / died / stopped); watched files by mtime+size (funes.local is never read); PSI, hwmon, VRAM, disks every 5 min. Log text: quoted, digits folded, capped, tainted. `run-*.scope` is one subject; `funes-test-*` scopes (the cgroup test's deliberate OOM, every deploy) are dropped and taken off the OOM counter. | `host_sources.{h,cpp}` | `test_parse_journal`, `test_parse_dpkg_and_proc`, `funes machine-probe` on yoda |
| Collector: first tick records the box without announcing it; *changes* always promoted, *failures/anomalies* once per row; every child's cost (`proc::set_usage_observer`, MCP servers included) is a series per program, and a run ≥ 4× its median (≥ 5 points, ≥ 10 s CPU) is an anomaly; PSI against its floor and median; temperatures against the sensor's own critical point; disks at 90%. Memories are `[YYYY-MM-DD HH:MM] …` text in the machine pool, source `machine`, written behind the busy gate; a failure is linked (`preceded_by`, 0.6) to every remembered change in the 2 h before it and to changes to the same subject in the day before. `FUNES_MACHINE_PROMOTIONS_PER_DAY` (200) pauses promotion, not recording. | `host_collector.{h,cpp}`, `main.cpp` | `test_changes_are_remembered`, `test_oom_and_dead_llama_are_failures`, `test_run_baseline`, `test_daily_ceiling` (each rule checked by mutation: un-folding or un-linking fails a test) |
| `host_changes(since_hours, subject?)` — the ring, no model; `machine_recall(query)` — the pool. Both privileged; `funes` has them, so an admin gets them and a member does not. | `tools/host_tools.cpp`, `permissions.cpp`, `agents/funes.yaml` | `test_system_account_cannot_be_used` |

The pool is an ordinary one, so the background passes already reach it:
consolidation merges near-duplicates there, and the link judge proposes links
beyond the deterministic ones.

What it saw on its first look at yoda (`funes machine-probe`, 2026-11-11):
`dream-mdns.service` had failed to start 13,424 times in the previous 24 hours
(`Changing to the requested working directory failed`), unnoticed — the case
the fold rules were written for; both llama-server models with their files;
every sensor including the GPU's own critical points (k10temp publishes none,
so 90 °C applies).

## Open questions (all answered)

1. ~~**Which machine?**~~ Answered 2026-11-11: **yoda, always.** So Pillar 2
   is about a headless server: the events that matter are the user units
   (`funes`, the two WhatsApp bridges, the autoresponder), the `llama-server`
   processes `~/launch_llama.sh` starts (not units — pids in
   `/tmp/llama_pids`), the crontab's publishing jobs, dpkg, PSI, disk and
   temperatures. System-unit and kernel (OOM) events need julio in the
   `systemd-journal` group — an admin decision, not code; without it the
   collector reads the user journal and `/proc` only.
2. ~~**yoda's kernel and systemd versions.**~~ Answered 2026-11-10: Debian 13,
   kernel **6.12** (Landlock in the LSM list → ABI 6: filesystem, TCP-port and
   scoping rules; no audit, which needs 6.15), systemd **257** (no Varlink unit
   API, which needs 258), unprivileged user namespaces **allowed**, PSI
   present, and the user manager is delegated **`cpu memory pids`** — no `io`,
   so phase 1 can set `memory.max`, `pids.max`, `cpu.weight` and use
   `cgroup.kill`, but not `io.max`.
3. ~~**6.0 voice is "next" in the roadmap.**~~ Settled 2026-11-10: voice is
   deprioritised (now 8.0); the OS plan runs first. Phases 4–6 are features,
   but they follow 1–2 without voice in between.
4. ~~**Does the machine pool share the embedding model's budget?**~~
   Answered 2026-11-11: yes, and generously — yoda's embedding server is idle
   outside recall, and putting it to use is the point. The ceiling stays, as a
   runaway guard rather than rationing: a promotions-per-day number in
   `funes.conf`, set high, with embedding calls behind the same busy gate as
   the link backfill so a person's recall never waits on the machine.

## Sources

- Sandlock — unprivileged Landlock + seccomp-notify sandbox for agent code: https://arxiv.org/abs/2605.26298
- AgentSight — eBPF boundary tracing, the "semantic gap": https://arxiv.org/abs/2508.02736
- BranchFS — branch contexts with commit/rollback: https://arxiv.org/abs/2602.08199
- DeltaBox — overlayfs-based checkpoint/rollback for agents: https://arxiv.org/abs/2605.22781
- ProbeLogits — kernel-level inference primitives (preprint): https://arxiv.org/abs/2604.11943
- Landlock userspace API (ABI history): https://docs.kernel.org/userspace-api/landlock.html
- systemd NEWS (Varlink unit API in 258/259, registry in 260): https://github.com/systemd/systemd/blob/main/NEWS
