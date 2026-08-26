# Funes 4.0 — what's left

Branch: `feat/v4-users-permissions`.
State as of 2026-08-26: phases 1–4 of `dev-plan-users-permissions.md` are
implemented. 28/28 `ctest` plus the full `tests/integration.sh` pass, and the
branch has been benchmarked against the real model.

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

**NOT verified**
- **`web_search` / `web_fetch`.** Every bench run so far used `--skip-web`.
  See "Deferred by decision" below — this is now a design task, not just an
  untested path.
- **The WhatsApp service-token path end to end.** Only curl-tested; never run
  against a real bridge with a real incoming message. Also now a design task.
- **The yoda deployment.** Runbook in `deploy-v4-yoda.md`.

---

## Deferred by decision (2026-08-26)

These two are not oversights. Both need a design choice that 4.0's multi-user
model forces, and both were deliberately left for a later pass.

1. **WhatsApp becomes per-user, keyed by phone number.** Today a service token
   plus `X-Funes-User-Jid` resolves to an account through `funes jid-map`,
   which already has the right shape — one jid, one user. What is missing is
   the story for a household: each person's number maps to their own account,
   so their memories, workspace and permissions are theirs. Until that is
   thought through, the v4 unit leaves WhatsApp off entirely.

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

## Known gaps (ordered by how much they'd annoy someone)

1. **No admin panel.** Phase 5's user list and permissions editor were
   deferred by the plan itself in favour of the CLI, and that decision still
   holds — but a member has no way to see their own permissions, and an admin
   has no way to see anyone's without SSH. `funes perms <user>` prints a
   resolved view; a read-only `/api/auth/status` extension would be the
   cheapest improvement.

2. **Sessions still cannot be deleted**, only rotated. Pre-existing
   (`MemoryStore::prune_turns` exists, nothing exposes it) but more visible
   now: a shared household install accumulates other people's conversation
   list entries that nobody can remove.

3. **`funes passwd` does not revoke existing sessions.** Deliberate — it is
   the admin resetting a forgotten password, not a response to a stolen
   cookie. Revisit when users can change their own password, where the
   opposite behaviour is correct.

4. **The admin cron view is unexposed.** `MemoryStore::list_cron_jobs(-1)`
   returns every user's jobs, and nothing calls it with `-1` — `/api/jobs`
   passes the caller's own id. An admin cannot see what is scheduled on the
   box.

5. **`curator.yaml` uses an absolute `workspace_dir`**
   (`/home/julio/Documents/X_posts`), which is the documented escape hatch out
   of per-user isolation into a folder every account shares. That is fine for
   a single-admin install and is how the newsletter keeps working, but the
   comment in `src/core/tools/fs_guard.h` still claims *no* shipped agent uses
   it. Either the comment or the agent should change; if a second account ever
   runs the curator they will collide in that directory.

6. **One startup does not finish the embedding backfill on a large database.**
   `backfill_embeddings(max_items = 256)` is capped per call and `main.cpp`
   calls it once at startup, so a database with more than 256 memories comes
   back from a 4.0 migration with the remainder unvectored — they fall back to
   keyword recall until the next restart. Pre-existing, but 4.0 is what makes
   it visible: the migration drops every vector, so the cap never used to be
   reached. Seen on yoda (281 memories: 256 on the first start, 25 on the
   second). Either loop until it returns 0, or say so in the runbook.

7. **Config is read relative to the working directory.** `funes::load_config()`
   reads `./config/funes.local` and `./config/funes.conf` before
   `~/.funes/config`, so a binary started from the repo root inherits the
   repo's config — which sets `FUNES_ALLOW_SHELL=1`. Not a bug, and the v4
   systemd unit sets every meaningful variable explicitly so it cannot bite in
   production. It bites when *testing*: a scratch server started from the repo
   is not running with the environment you think it is.

---

## Tests worth adding

- **`funes perms` CLI beyond the happy path.** `integration.sh` now covers
  `--allow`, `--deny`, `--agents` and `--reset` as a side effect of testing
  the permissions they set, but bad input and the JSON it writes are still
  unasserted.
- **Session-token lifetime.** Expiry is implemented and never tested; only
  revocation is.

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
5. Merge to `main` has not been discussed. The branch is self-contained; 3.x
   on yoda is unaffected either way.
