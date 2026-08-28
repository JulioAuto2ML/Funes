# Funes 4.0 — what's left

Branch: `feat/v4-users-permissions`.
State as of 2026-08-28: phases 1–4 of `dev-plan-users-permissions.md` are
implemented, benchmarked against the real model, and **deployed on yoda** as
an independent install on `:8485`. 28/28 `ctest` and 220 `integration.sh`
assertions pass.

**2026-08-28:** Julio tested the deployed 4.0 install and it works. Six of the
seven pending items below are now done (1, 2, 3, 4, 6, 7) and **deployed to
yoda** (commit `c6f189d`, 28/28 `ctest` on the host, service restarted clean).
Only #5 remains, and it is blocked on a feature that does not exist yet. The
two items under "Deferred by decision" are untouched and are still the largest
open questions.

**`funes cron-cleanup` has not been run.** The dry run on yoda reports 10
auto-memories and 36 scheduled-run sessions (94 turns). See "The cleanup still
to run" at the end — the numbers there are worse than this document previously
recorded.

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
- **Real use of the yoda install**, 2026-08-28: Julio used it and reported it
  working. That closes the "deployed but nobody has talked to it" gap; it is
  not a substitute for the bench, which last ran on 2026-08-26.
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
- **Anything from 2026-08-28.** All of it is covered by `ctest` and
  `integration.sh` against the mock LLM, and each new assertion was checked
  against a deliberate mutation of the code it covers. None of it has run on
  yoda, and none of it has been near the real model.

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

One item, and it is not actionable yet. Everything else that was here on
2026-08-26 was done on 2026-08-28 — see "Done on 2026-08-28" below for what
each turned into, including the two decisions that were left open.

### 5. `funes passwd` does not revoke existing sessions
Deliberate today: it is the admin resetting a forgotten password, not a
response to a stolen cookie, and revoking would kick the user out of a session
they are mid-conversation in.
**Done when:** users can change their own password — at which point the
opposite behaviour is correct and this needs a `--revoke-sessions` flag, or an
unconditional revoke on the self-service path only. Not actionable before that
exists; listed so the reasoning is not rediscovered.

---

## Done on 2026-08-28

Kept rather than deleted, because two of them were decisions rather than
straightforward fixes and the reasoning is worth not rediscovering.

### 1. Members can see their own permissions — done
`/api/auth/status` (and `/api/login`, so the UI need not re-fetch on a page it
just authenticated) now returns `permissions`: `role`, `is_admin`,
`agents_restricted`, the allowed `agents`, and `denied_tools`. **Resolved**,
not the raw blob — `FunesApi::resolved_permissions` runs the same
`Permissions::parse` the runtime uses, intersects the agent allowlist with the
agents actually loaded, and answers `allows_tool` for every registered tool. A
UI that re-derived "absent means different things for agents and tools" in
JavaScript would eventually disagree with `permissions.cpp`.

Still the caller's own permissions only: the route is public, so returning
anyone else's would describe the box's roster to an anonymous request. That is
asserted. In the UI it is the user chip's hover tooltip — no new element, no
CSS.

### 2. The embedding backfill drains — done
The startup thread in `main.cpp` loops until a pass reports nothing done.
`MemoryStore::count_missing_vectors()` was added because a pass returning 0 is
ambiguous — nothing left, or the endpoint is down — and the two need different
responses. An unreachable embedding endpoint is now retried every 5 minutes for
about an hour (`kMaxRetries`) before giving up with a line saying how many are
still on keyword recall, so the ordinary case of `:8081` coming up *after*
Funes does no longer waits for a restart. The runbook stopgap in
`deploy-v4-yoda.md` is replaced.

### 3. Cron runs — done, as a three-way split rather than the flip
The "Done when" here said `persist=false`. That was **not** what was done, and
deliberately: it would have dropped the per-run transcripts, which are the only
record of what a run older than the last one did (`cron_jobs.last_output` is a
preview of the last run only, and a newsletter run had failed the morning this
was written).

Instead `FunesAgent::run`'s `bool persist` became a three-valued `Persist`
(`Full` / `TurnsOnly` / `None`, see `src/core/agent.h`). Cron uses
`TurnsOnly`: keep the transcript, write no auto-memory. The auto-memory was
always the real harm — it phrased the exchange as `User said: "<the job's
task>"`, and those were being recalled into real conversations.

The clutter half is solved by hiding rather than deleting:
`MemoryStore::list_sessions` excludes `cron-%` unless asked, and
`GET /api/sessions?cron=1` is how the job history is reached — the per-run
epoch makes the session name unguessable, so hiding it without a listing would
be losing it. The filter sits inside the grouped subquery, not after the
`LIMIT`, or a busy scheduler returns short pages.

`MemoryStore::CRON_SESSION_PREFIX` and `CRON_TASK_PREAMBLE` are now constants
shared by `cron_runner.cpp` (which writes them) and `memory.cpp` (which matches
on them). Two spellings would make the filter and the cleanup silently find
nothing.

**Backfill: written, not run.** `funes cron-cleanup` (new
`src/server/maint_cli.{h,cpp}`) removes the auto-memories an older database
already holds, going through `MemoryStore::forget`/`delete_session` — a raw
`DELETE` from `memories` leaves the vec0 index holding an orphan vector.
It **reports without deleting** unless `--apply`, and it **keeps** the old
transcripts unless `--drop-sessions`, since they are already hidden and are
the only per-run record. Section 7 of `deploy-v4-yoda.md` has the runbook.
Nobody has run it on yoda.

### 4. An admin can see what is scheduled on the box — done
`GET /api/jobs?all=1`, gated on `require_admin()`, returns every account's jobs
with `owner`/`owner_id`; the response carries `"scope": "mine" | "all"`. Owners
are resolved once into a map (few owners, many jobs), and a job whose owner was
deleted shows as `(deleted user)` rather than vanishing — that is the one most
in need of cancelling. The caller's own listing deliberately carries no `owner`
field: there it is always the caller, and the field's presence is what tells
the client which view it got.

Worth knowing: the first version of this test passed against a handler that
ignored `all` entirely, because the admin owned the only job on the box. The
suite now schedules a job as the member too.

### 6. Session-token expiry is tested — done
The unit-level test already existed (`tests/test_users.cpp`); what was missing
was the HTTP path, which is the one that matters. `integration.sh` now
backdates the member's `auth_tokens` row via `tests/expire_token.py`, asserts
the cookie stops authenticating (`/api/auth/status` and a 401 on a protected
route), and that logging in again works — expiry is not a lockout. The helper
asserts the row is still present and now expired, because "no such token" and
"expired token" are deliberately indistinguishable from outside.

### 7. `funes perms` bad input is tested — done
Unknown options, missing values, a nonexistent username, exit codes, and the
JSON actually written. Also that a half-valid line writes nothing at all — a
command that errors and still takes effect is the worst outcome here.

Testing it turned up a small wart, now fixed: `cmd_perms` asked for an
option's value before checking the option was known, so a typo in the last
argument (`funes perms bob --agentz`) reported "Missing value for --agentz",
which reads as though the spelling was fine.

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
   trusting it. This keeps catching real gaps: the concurrency test passes
   identically against a serialising server (hence the timing floor); the
   migration test was checked against three deliberate mutations of
   `migrate()`; and on 2026-08-28 the `/api/jobs?all=1` test passed against a
   handler that ignored `all` entirely, because the admin owned the only job
   on the box.
5. To ship a change to yoda: push the branch, then `git pull --ff-only` in
   `~/Funes-v4`, rebuild with `-j2` (the box is RAM-limited and the model is
   resident), run `ctest`, and `systemctl --user restart funes-v4`. Never
   `scp` or edit files there. Take a database backup first if the change
   touches `migrate()` — `deploy-v4-yoda.md` has the online-backup snippet,
   and note there is no `sqlite3` CLI on that host.
6. Merge to `main` has not been discussed. The branch is self-contained and
   3.x on yoda is unaffected either way, but 4.0 is now the install being
   iterated on, so the two will keep diverging until this is decided.

---

## Deployed, 2026-08-28

`c6f189d` is live on yoda. Pulled into `~/Funes-v4`, rebuilt with `-j2`, 28/28
`ctest` on the host, `funes-v4` restarted and serving on `:8485`. A database
backup was taken first even though `migrate()` was untouched:
`~/.funes-v4/memory.db.bak-20260828-121310` (270 memories, 696 turns, 2 jobs,
1 user — matching the live database at the time).

Two things behave differently since the restart:

- The conversation list no longer shows `cron-*` entries. They are hidden, not
  deleted; `GET /api/sessions?cron=1` lists them.
- The startup backfill drains instead of stopping at 256. On this database
  every memory already had a vector, so the thread found nothing missing and
  exited — which is the "all done, stop for good" path, and why the log has no
  backfill line at all.

Verified on the host after the restart: an unauthenticated `/api/auth/status`
carries no `permissions` block, `/api/jobs?all=1` is 401 without credentials,
and `funes cron-cleanup` reports the same counts as a direct read of the
database. The authenticated views (a member's own permissions, the box-wide
job list, `?cron=1`) are covered by `integration.sh` but have not been
exercised against the live install — that needs a login.

## The cleanup still to run

`funes cron-cleanup --apply` has **not** been run. The dry run on yoda:

```
  10 auto-memories written by a scheduled run
  (keeping 36 scheduled-run session(s), 94 turns)
```

The recall counts are worse than this document previously recorded. It said
two memories at 13 and 15; the live table has ten, and seven of them have been
recalled at least once:

| memory id | recalls |
|---|---|
| 281 | 18 |
| 271 | 13 |
| 311 | 11 |
| 316 | 11 |
| 321 | 5 |
| 346 | 2 |
| 348 | 1 |
| 323, 324, 350 | 0 |

That is 61 occasions on which the scheduler's own preamble was injected into a
real conversation as something the person had said. The code change stops new
ones; only the cleanup removes these.

Runbook is section 7 of `deploy-v4-yoda.md`. It is destructive and has no undo,
so read the dry run first. The transcripts are kept unless `--drop-sessions`,
and there is no reason to drop them — they are already hidden and are the only
record of what a run older than the last one did.
