# Funes 4.0 — what's left

Branch: `feat/v4-users-permissions`.
State as of 2026-08-26: phases 1–4 of `dev-plan-users-permissions.md` are
implemented, benchmarked against the real model, and **deployed on yoda** as
an independent install on `:8485`. 28/28 `ctest` and 154 `integration.sh`
assertions pass.

**2026-08-26, later:** 3.x was stopped deliberately and 4.0 now owns the
schedule for testing. A host drop-in
(`~/.config/systemd/user/funes-v4.service.d/cron.conf`) sets
`FUNES_CRON_ENABLED=1`, overriding the shipped unit's `0`. **Revert that
drop-in before starting `funes.service` again**, or both installs fire every
job — the newsletter publishes twice and the daily reminder reaches a real
person twice. 4.0 also now holds the Tavily and Gmail keys, which the earlier
"no secrets on this install" note deliberately withheld; that note was written
when 4.0 was a parallel test install, not the primary one.

Start at "Pending work" below; everything above it is context for why those
items are what remain.

---

## Verified vs. not

Being explicit, because "tested" has meant two different things here.

**Verified against real data, a live server, or a real model**
- The schema migration, two ways: once by hand against a copy of a real 3.x
  database (38 memories, 108 turns preserved, ids intact), and now in CI by
  `tests/test_migration.cpp`, which builds the pre-4.0 schema from scratch.
- Cross-user isolation over HTTP with two real accounts: memory list/search,
  the `DELETE /api/memories/<id>` IDOR, a deliberate session-id collision.
- The workspace migration and per-user uploads on a live server.
- Permissions live, and now also in `integration.sh` over real HTTP: a member
  restricted to two agents sees only those in `/api/agents`, gets 403 on the
  others, and has a denied tool withheld from the schema it is shown.
- Auth surface: 401 on every protected route, forged/revoked cookies,
  service-token paths (no jid, unmapped jid, wrong token).
- **Against the real model** (Qwen3.5-9B-Q8_0 on yoda's `:8080`), via
  `scripts/funes_bench.py`: 6/6 as an admin and as a member. Phase 4's
  narrowed schema did not break tool-calling. The two runs also gave the
  cleanest available proof that permissions work end to end — same server,
  same model, minutes apart, only the account differing: the admin delegated
  to `operator`, ran `df` and answered "1.7 GB"; the member delegated
  identically, had no `execute_shell`, and said so. The figure matched the
  real `df`, so the admin path genuinely executed and the member's refusal
  was the permission, not a broken server.
- **Concurrency.** Six simultaneous chats, two accounts, deliberately sharing
  one session name. Neither account sees the other's turns, and the test
  fails if the requests merely queue rather than overlap.
- **Deleting a conversation.** `DELETE /api/sessions/<name>` and a control in
  the conversation list, covered by unit tests (cross-user, and that memories
  survive), integration tests over HTTP, and checked in a real browser. Its
  first test uncovered a pre-existing bug: `session_summaries` was keyed on
  `session` alone, so the second account to use a session name silently got no
  rolling summary at all. Key is now `(user_id, session)`; migrated on yoda
  2026-08-26.
- **The yoda deployment**, 2026-08-26. 4.0 runs as an independent install on
  `:8485` from `~/Funes-v4`, against `~/.funes-v4/`. The 3.x data migrated
  intact (281 memories, 706 turns, ids preserved, all attributed to user 1)
  and 3.x was verified untouched afterwards: still serving on `:8484`, its
  workspace still flat, and its database with no `users` table, no `user_id`
  column and no migration marker. A second restart produced no migration
  lines at all, which is the idempotency `test_migration.cpp` asserts, seen
  in production.

**NOT verified**
- **`web_search` / `web_fetch`.** Every bench run so far used `--skip-web`.
  See "Deferred by decision" below — this is now a design task, not just an
  untested path.
- **The WhatsApp service-token path end to end.** Only curl-tested; never run
  against a real bridge with a real incoming message. Also now a design task.
- **Any real use of the yoda install.** It is deployed and serving (see
  below), but it has no accounts yet and nobody has held a conversation on
  it.

---

## Deferred by decision (2026-08-26)

These two are not oversights. Both need a design choice that 4.0's multi-user
model forces, and both were deliberately left for a later pass.

1. **WhatsApp becomes per-user, keyed by phone number.** Today a service token
   plus `X-Funes-User-Jid` resolves to an account through `funes jid-map`,
   which already has the right shape — one jid, one user. What is missing is
   the story for a household: each person's number maps to their own account,
   so their memories, workspace and permissions are theirs.

   **Operational state as of 2026-08-26**, learned while handing the schedule
   to 4.0. `whatsapp-autoresponder.service` is **stopped** — it posts to
   `:8484`, and 3.x is down. It is still `enabled`, so a reboot restarts it
   against a dead port; disable it, or repoint it, before that matters.

   To bring it up on 4.0, three things are needed, in this order:
   - a `FUNES_SERVICE_TOKEN` on the v4 unit (absent by design today, which is
     why repointing the poller alone would still 401), and the same value in
     the autoresponder's config;
   - `funes jid-map <jid> <username>` per person — this is the per-user
     mechanism, already built and curl-tested but never run against a real
     incoming message;
   - `WHATSAPP_WHITELIST`, deliberately not copied to v4 with the other
     secrets.

   Note the `whatsapp-assistant` agent (used by the cron reminder) is a
   *different* path and already works on 4.0: it reaches WhatsApp through the
   MCP server, not the service token. That MCP resolves its message store
   from its own file location, so v4's copy needed
   `third-party/whatsapp-mcp/whatsapp-bridge/store` symlinked to the live
   store in the 3.x clone. **If the 3.x clone is ever deleted, that symlink
   breaks and every WhatsApp read fails** — move the bridge's store somewhere
   install-independent first. Both `whatsapp-bridge` units stay running
   regardless; stopping them loses the authenticated session and costs a QR
   re-scan.

2. **Web search gets one Tavily key shared by every account, which no user can
   see or steal.** The key is the operator's, not a per-user secret: it is
   billed centrally and must never reach a member. That rules out putting it
   anywhere a user-facing surface could echo it back — not in `/api/status`,
   not in a tool argument, not in an error message. `web_search` stays a
   permissioned *capability* (an admin can deny it per account) while the
   credential behind it stays entirely server-side. Note the precedent already
   set: `/api/status` now redacts the LLM url and provider for members for
   exactly this reason.

---

## Pending work

Ordered so the list can be worked top to bottom. Each entry says what is
wrong, where it lives, and what "done" looks like — the point is that none of
them needs this conversation to be actionable.

### 1. Members cannot see their own permissions
`/api/auth/status` returns identity only. A member who finds an agent missing
or a tool refused has no way to learn why, and an admin cannot inspect anyone
without SSH to run `funes perms <user>`.
**Done when:** `/api/auth/status` also returns the caller's resolved
permissions (allowed agents, denied tools), and the UI shows them somewhere
unobtrusive. Read-only — editing stays in the CLI, per phase 5's deferral.
Reuse `Permissions::parse` and the same resolved view `show_permissions()`
already prints in `src/server/user_cli.cpp`.

### 2. The embedding backfill does not finish in one start
`backfill_embeddings(max_items = 256)` is capped per call and `main.cpp` calls
it exactly once, so a database with more than 256 memories comes back from a
migration with the remainder on keyword-only recall until the next restart.
Pre-existing, but 4.0 exposes it: the migration drops every vector, so the cap
was never reached before. Seen on yoda — 256 on the first start, 25 on the
second.
**Done when:** the startup thread loops until a pass returns 0 (with a small
sleep between, so a down embedding endpoint does not spin), or the runbook
stops pretending one restart is enough. The loop is the better fix; the
runbook note is already in `deploy-v4-yoda.md` as a stopgap.

### 3. Cron runs are stored as conversations, and as memories
Every firing gets its own session (`cron-<id>-<epoch>`, one per run by design,
so tool budgets do not carry over), and `run_agent_job` passes `persist=true`.
On yoda that is **33 of 180 sessions (18%)**, two turns each, growing by one
per job per day forever — you have already deleted some by hand.

The turns are only clutter. The memories are worse. `persist` gates *both* the
turns and the auto-memory write (`src/core/agent.cpp`, step 5), so each firing
also stores a memory of the form `User said: "<the job's task>" — I replied:
"..."`. The scheduler is not the user, and one of them on yoda records the
job-runner preamble verbatim: `User said: "[You are running as a scheduled job
— there is no interactive user...]"`. **13 of 273 memories** are these. They
are semantically recalled into real conversations, so asking about the
newsletter surfaces the scheduler talking to itself.

The record that survives without them: `cron_jobs.last_status` and
`last_output` (a 4000-byte preview, **last run only**), the journal, and — for
these two jobs — the actual artifacts, which are the real record anyway: the
published issue files and the sent email, the delivered WhatsApp message. The
transcript was never the evidence that the newsletter went out.

**Done when:** `run_agent_job` passes `persist=false` (`src/core/cron_runner.cpp`;
its comment currently argues the opposite — "the job's own session is the
record" — and needs rewriting, not just flipping). Then decide separately
whether to backfill: existing `cron-%` sessions and their auto-memories can be
removed with the same care the bench cleanup used (delete through
`MemoryStore::forget`/`delete_session`, not raw SQL, or the vec0 index is
left with orphans).

**Consider first:** this drops per-run history, keeping only the last run's
bounded preview. If post-mortem on a *previous* failed run matters — and this
morning's newsletter did fail — either widen what `record_cron_job_run` keeps,
or take the smaller option instead: keep persisting but exclude `cron-%` from
`list_sessions`, which fixes the clutter and not the memory pollution.

### 4. An admin cannot see what is scheduled on the box
`MemoryStore::list_cron_jobs(-1)` already returns every user's jobs and
nothing calls it with `-1` — `/api/jobs` passes the caller's own id.
**Done when:** `/api/jobs` accepts something like `?all=1`, gated on
`require_admin()` (added 2026-08-26, `src/server/api.cpp`), and returns the
owning username per job. Small, and it makes a shared box operable.

### 5. `funes passwd` does not revoke existing sessions
Deliberate today: it is the admin resetting a forgotten password, not a
response to a stolen cookie, and revoking would kick the user out of a session
they are mid-conversation in.
**Done when:** users can change their own password — at which point the
opposite behaviour is correct and this needs a `--revoke-sessions` flag, or an
unconditional revoke on the self-service path only. Not actionable before that
exists; listed so the reasoning is not rediscovered.

### 6. Session-token expiry is untested
Expiry is implemented; only revocation has a test.
**Done when:** a test creates a token with a short TTL, waits it out (or
backdates the row), and asserts the session no longer authenticates.

### 7. `funes perms` bad input is untested
`integration.sh` covers `--allow`, `--deny`, `--agents` and `--reset` as a
side effect of testing the permissions they set. Unknown flags, missing
values, and a nonexistent username are unasserted, as is the JSON written.

---

## Known and deliberate

Not bugs, not scheduled — written down so they are not rediscovered as
surprises.

- **`curator` writes to a shared absolute workspace**
  (`/home/julio/Documents/X_posts`). That is the documented escape hatch in
  `src/core/tools/fs_guard.h`, and it is what keeps the newsletter running
  against one candidate pool rather than a copy per account: the publication
  belongs to the installation, not to a user. Every account running the
  curator touches the same files, so two people publishing the same day would
  overwrite each other. Fine while the admin is the only one who runs it.
  Revisit before granting `curator` to a member.

- **Config is read relative to the working directory.** `funes::load_config()`
  reads `./config/funes.local` and `./config/funes.conf` before
  `~/.funes/config`, so a binary started from the repo root inherits the
  repo's config — which sets `FUNES_ALLOW_SHELL=1`. The v4 systemd unit sets
  every meaningful variable explicitly, so it cannot bite in production. It
  bites when *testing*: a scratch server started from the repo root is not
  running with the environment you think it is. This is why the yoda bench run
  could measure the disk and the local member run could not.

---

## If picking this up cold

1. Read `dev-plan-users-permissions.md` — its status header lists the five
   corrections made while building, including the one factual error (the
   `vec_memories` "no change needed" claim).
2. `CLAUDE.md` has the isolation invariants to know before touching
   `memory.cpp` or `api.cpp`, including the three vec0 partition-key traps.
3. `tests/test_user_isolation.cpp` and `tests/test_migration.cpp` are the
   suites to keep green above all others — the first is a privacy breach if it
   regresses, the second is permanent data loss, since Funes migrates whatever
   database it opens and there is no downgrade.
4. When adding a test here, check it fails for the right reason before
   trusting it. Two of today's would have passed while proving nothing: the
   concurrency test passes identically against a serialising server (hence the
   timing floor), and the migration test was checked against three deliberate
   mutations of `migrate()`.
5. To ship a change to yoda: push the branch, then `git pull --ff-only` in
   `~/Funes-v4`, rebuild with `-j2` (the box is RAM-limited and the model is
   resident), run `ctest`, and `systemctl --user restart funes-v4`. Never
   `scp` or edit files there. Take a database backup first if the change
   touches `migrate()` — `deploy-v4-yoda.md` has the online-backup snippet,
   and note there is no `sqlite3` CLI on that host.
6. Merge to `main` has not been discussed. The branch is self-contained and
   3.x on yoda is unaffected either way, but 4.0 is now the install being
   iterated on, so the two will keep diverging until this is decided.
