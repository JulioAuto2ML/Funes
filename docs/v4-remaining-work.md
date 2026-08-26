# Funes 4.0 — what's left

Branch: `feat/v4-users-permissions` (8 commits, based on `1e379a5`).
State as of 2026-08-26: phases 1–4 of `dev-plan-users-permissions.md` are
implemented and committed. 27/27 `ctest` plus the full `tests/integration.sh`
pass. Nothing has been deployed.

---

## Verified vs. not

Being explicit, because "tested" has meant two different things here.

**Verified against real data or a live server**
- The schema migration, against a copy of a real 3.x database (38 memories,
  108 turns preserved, ids intact).
- Cross-user isolation over HTTP with two real accounts: memory list/search,
  the `DELETE /api/memories/<id>` IDOR, a deliberate session-id collision.
- The workspace migration and per-user uploads on a live server.
- Permissions live: a member restricted to two agents sees only those in
  `/api/agents`, gets 403 on the others, has a denied tool withheld.
- Auth surface: 401 on every protected route, forged/revoked cookies,
  service-token paths (no jid, unmapped jid, wrong token).

**NOT verified**
- **Anything against a real LLM.** Every run used the mock or an unreachable
  endpoint. Phase 4 narrows the tool schema agents see, which is exactly what
  `scripts/funes_bench.py` measures — run it before trusting the branch.
- **The WhatsApp service-token path end to end.** Only curl-tested. Never run
  against a real bridge with a real incoming message.
- **The yoda deployment.** Not started. Runbook in `deploy-v4-yoda.md`.
- **Concurrency.** The plan's `-np` / slot analysis was never exercised; no
  two users have chatted simultaneously.

---

## Known gaps (ordered by how much they'd annoy someone)

1. **`POST /api/agents/reload` is available to any authenticated user**, not
   just admins. A member can force a reload of every agent definition. It
   needs an `is_admin()` check, and that raises the general question of
   whether other routes deserve one (`/api/status` exposes the model URL and
   provider, for instance). *Small, security-relevant, do this first.*

2. **No admin panel.** Phase 5's user list and permissions editor were
   deferred by the plan itself in favour of the CLI, and that decision still
   holds — but a member has no way to see their own permissions, and an admin
   has no way to see anyone's without SSH. `funes perms <user>` prints a
   resolved view; a read-only `/api/auth/status` extension would be the
   cheapest improvement.

3. **Sessions still cannot be deleted**, only rotated. Pre-existing
   (`MemoryStore::prune_turns` exists, nothing exposes it) but more visible
   now: a shared household install accumulates other people's conversation
   list entries that nobody can remove.

4. **`funes passwd` does not revoke existing sessions.** Deliberate — it is
   the admin resetting a forgotten password, not a response to a stolen
   cookie. Revisit when users can change their own password, where the
   opposite behaviour is correct.

5. **The admin cron view is unexposed.** `MemoryStore::list_cron_jobs(-1)`
   returns every user's jobs, and nothing calls it with `-1`. An admin cannot
   see what is scheduled on the box.

6. **`~/.funes/config` is read by every install on the machine.** Not a bug,
   but it is shared state between the 3.x and 4.0 installs. The v4 systemd
   unit sets every meaningful variable explicitly so this cannot bite; anyone
   adding a new `FUNES_*` setting should either set it in the unit or know it
   can leak across installs.

---

## Tests worth adding

- **Member-scoped integration cases.** `tests/integration.sh` bootstraps an
  admin and runs everything as that admin, so the permission paths are only
  covered by unit tests. It should create a second, restricted account and
  assert the 403s and the withheld tools over real HTTP.
- **`funes perms` CLI has no test.** `Permissions` itself is well covered
  (`test_permissions.cpp`, 8 cases); the argument parsing and the JSON it
  writes are not.
- **A migration test against a synthetic 3.x database.** The migration is
  currently only proven by a manual run against a copy of a real one. A
  fixture that builds the pre-4.0 schema, populates it, migrates and asserts
  would catch a regression in CI. The two bugs already found there — the
  eager vec rebuild and `INSERT OR REPLACE` under a partition key — both have
  regression tests, but the migration as a whole does not.
- **Concurrent-user smoke test.** Two sessions chatting at once against the
  mock, asserting neither sees the other's turns. Cheap, and the plan's whole
  multi-user premise rests on it.

---

## If picking this up cold

1. Read `dev-plan-users-permissions.md` — its status header lists the five
   corrections made while building, including the one factual error (the
   `vec_memories` "no change needed" claim).
2. `CLAUDE.md` has the isolation invariants to know before touching
   `memory.cpp` or `api.cpp`, including the three vec0 partition-key traps.
3. `tests/test_user_isolation.cpp` is the suite to keep green above all
   others.
4. Merge to `main` is not urgent and has not been discussed. The branch is
   self-contained; 3.x on yoda is unaffected either way.

## Note on `scripts/funes_bench.py`

It was an untracked work-in-progress in the working tree when this branch
started, and got committed as part of `cae9ccc` (phase 3) by a broad
`git add`. It therefore exists **only on this branch**. It is a general tool
rather than a 4.0 one, so it probably belongs on `main` — lift it across
before it gets forgotten here.
