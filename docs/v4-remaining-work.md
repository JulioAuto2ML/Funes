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

**`funes cron-cleanup --apply` was run on yoda the same day**, removing 10
auto-memories and keeping all 36 scheduled-run transcripts. See "The cleanup,
run" at the end.

**2026-08-28, decisions.** Julio settled both "Deferred by decision" items:
`web_search` keeps one shared key for every account (which is already what the
code does — see below), and per-user WhatsApp is **deferred again**, until a
few local accounts have been exercised through the web UI. That makes
"create a couple of member accounts and use them" the next piece of work.

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
- **The WhatsApp service-token path, end to end**, 2026-08-28. A real message
  ("Hola") from the whitelisted number, answered 2 seconds later. It had only
  ever been curl-tested before.

  The reply happening is the weaker half. What matters is where it landed, and
  it was checked in the database rather than inferred: the turn is owned by
  `user_id = 1` under session `whatsapp_162590433005727_lid` with agent
  `whatsapp-autoresponder` — an identity *resolved from the jid*, not a
  default. The auto-memory it wrote is filed under `agent = funes`, which is
  `memory_scope: funes` doing its job: a WhatsApp conversation feeds the same
  pool as the web UI rather than a separate empty one. v4's poller state
  advanced; 3.x's is still dated 2026-08-26, so the two watermarks are
  genuinely independent.

  Every negative case is covered too, by curl against the live server: no
  token, a token with no jid, a token naming an unmapped jid, and a wrong
  token are all 401.
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
- **`web_search` / `web_fetch`.** Every bench run so far has used
  `--skip-web`, so neither has ever run. The design question is settled (one
  shared key — see "Deferred by decision" below, where the answer turned out to
  be what the code already did), which leaves this a plain untested path rather
  than a decision. The key is present on yoda, so a bench run without
  `--skip-web` is all it takes.
- ~~The WhatsApp service-token path end to end.~~ **Proven 2026-08-28** — see
  "Verified against real data" above. `web_search` above is now the only
  untested path left here.
- **The bench, since 2026-08-28's changes.** `scripts/funes_bench.py` last ran
  on 08-26. The code is covered by `ctest` and `integration.sh` against the
  mock LLM, with every new assertion checked against a deliberate mutation of
  what it covers, and it is deployed and serving on yoda. The real model has
  exercised the agent loop through it since — the WhatsApp reply went through
  `/api/chat` on Qwen3.5-9B — but nobody has re-run the 6-case bench, so the
  tool-calling paths specifically are unmeasured on this build.

---

## Deferred by decision

Both items needed a design choice that 4.0's multi-user model forces. **Both
were decided on 2026-08-28**; what is left of each is written under it.

1. **WhatsApp becomes per-user, keyed by phone number.** A service token plus
   `X-Funes-User-Jid` resolves to an account through `funes jid-map`, which
   already has the right shape — one jid, one user. What is still missing is
   the story for a *household*: several numbers, several accounts, and
   whatever an unmapped-but-whitelisted sender should get.

   **Running on 4.0 as of 2026-08-28, single-user.** The whole path is wired
   and live, with the one whitelisted jid mapped to the admin (`julio`). That
   makes this item now about the second person, not the first — the mechanism
   is no longer theoretical.

   What was done, all on yoda unless noted:
   - `scripts/whatsapp-autoresponder-v4.service` (in git) points the existing
     poller at `:8485`. It `Conflicts=` the 3.x unit, which is now
     **disabled** — that also clears the reboot landmine noted on 08-26.
   - `FUNES_SERVICE_TOKEN` generated into the v4 clone's `config/funes.local`
     (gitignored). Deliberately not in the unit file: `WorkingDirectory` makes
     the server and the poller read that same file, so they cannot drift into
     a permanent 401.
   - `WHATSAPP_WHITELIST` copied from 3.x; `funes jid-map 162590433005727@lid
     julio`.
   - `WHATSAPP_STATE_PATH` (new config key, `scripts/whatsapp_autoresponder.py`)
     under `~/.funes-v4/`. It was hardcoded to `~/.funes/`, which two pollers
     on one bridge store would have shared — and the symptom of that is not an
     error, it is one poller silently skipping what the other consumed.
   - `WHATSAPP_DB_PATH` set absolute. The autoresponder reads the *dedicated*
     bridge (`store-funes`, :8091), not the personal one, and `funes.conf`
     gives that path relative to the repo root — which the v4 clone has no
     copy of. Left unset the poller crash-loops on "unable to open database
     file"; this is the step that is easy to miss.

   Verified: token+mapped jid resolves to `julio` (id 1) on `/api/auth/status`;
   no token, token without a jid, token with an unmapped jid, and a wrong
   token are all 401. The poller is `active`, `NRestarts=0`, watching the right
   store, with its own state file and 3.x's untouched.

   **Proven with a real message the same day** — see "Verified against real
   data" above for what was checked in the database, not just observed in the
   chat.

   **Deferred again on 2026-08-28**, deliberately: the household case waits
   until a few local accounts have been used through the web UI. Getting
   multi-user right for people who can see what is happening comes before
   getting it right for people reaching it through a phone.

   Still open for the household case:
   - Both bridges run out of the 3.x clone, and v4 reaches their stores by a
     `store` symlink plus that absolute `WHATSAPP_DB_PATH`. **Deleting
     `~/Funes` breaks every WhatsApp read.** Moving the stores somewhere
     install-independent is the real fix.
   - A whitelisted but unmapped sender currently gets a refusal. For a
     household that is probably right, but it has never been decided.

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

2. **Web search: one shared key, decided 2026-08-28.** Every account uses the
   operator's Tavily key. It is billed centrally and must never reach a member,
   which rules out anywhere a user-facing surface could echo it back.

   **This is already the implementation**, which is why the decision cost no
   code: `funes::tavily::search` reads `FUNES_TAVILY_API_KEY` from the process
   environment at call time. It is never a tool argument, never per-user, and
   never in a response. `web_search` also already behaves as a permissioned
   *capability* — it is not in `PRIVILEGED`, so it is allowed by default and an
   admin can take it away per account with `funes perms <user> --deny
   web_search`. The precedent for keeping the credential server-side is
   `/api/status`, which already redacts the LLM url and provider for members.

   The one place a shared secret can reach a user is a **generated HTTP tool**
   (`src/core/tools/http_tool_runtime.cpp`), where `${ENV_VAR}` in a header
   value resolves via `getenv` at call time. Headers are safe because nothing
   echoes them; the **URL** is not, because a parse failure returns
   "invalid URL after substitution: <url>" and the model will repeat a tool
   result. That invariant is now commented at both the header and the call
   site, and asserted by `test_env_placeholders_never_reach_the_url`.

   Writing that test was instructive twice over. The first version passed
   against a deliberately broken build: it used a loopback URL, which parses
   fine and is refused by `net_guard` with a message naming only the host, so
   the echo path was never reached. The second surfaced why the current code is
   safe in a way nobody had written down — argument substitution runs first and
   its `{param}` regex eats the inner braces of `${VAR}`, so appending env
   resolution to the URL is harmless while applying it *before* substitution is
   a real leak. Only the second is a plausible mistake, and it is what the test
   now catches.

   **Still not verified: that `web_search` works at all.** Every bench run has
   used `--skip-web`. The decision settles the design, not the untested path.

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

## The cleanup, run

`funes cron-cleanup --apply` was run on yoda on 2026-08-28, against the backup
above:

```
Removed:
  10 auto-memories written by a scheduled run
  (keeping 36 scheduled-run session(s), 94 turns)
```

The recall counts were worse than this document had recorded. It said two
memories at 13 and 15; the live table held ten, seven of them recalled at least
once:

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

That is 61 occasions on which the scheduler's own preamble had been injected
into a real conversation as something the person had said. The earlier figure
came from spot-checking two rows rather than counting.

### Verified afterwards

- 270 → 260 memories; **696 turns and all 36 `cron-*` sessions untouched**,
  which is the point of not passing `--drop-sessions`.
- The single `source='user'` memory is still there. Explicit facts are never
  what this removes.
- **No orphaned vectors**, in either direction: `vec_memories_rowids` has 260
  entries and every one joins to a memory row, and every memory has a vector.
  This is the failure a raw `DELETE FROM memories` would have caused silently,
  and it is why the command goes through `MemoryStore::forget`.
- A restart logs `260 memories, semantic` — vec0 loads, the last embed
  succeeded, and the backfill thread finds nothing missing and exits.

Checking that from outside needs a trick worth writing down: **python's stdlib
`sqlite3` cannot open `vec_memories`** ("no such module: vec0"), and there is
no `sqlite3` CLI on yoda. Read the shadow table `vec_memories_rowids` instead.
Its `id` column is NULL and irrelevant — `vec_memories`' key is
`memory_id INTEGER PRIMARY KEY`, which maps onto the shadow table's **rowid**.
Joining on `id` silently matches nothing and reports every row as an orphan,
which is exactly as wrong as reporting none.

The backup taken beforehand is `~/.funes-v4/memory.db.bak-20260828-121310`
(with its `-wal`/`-shm` siblings). Delete it once you are satisfied.
