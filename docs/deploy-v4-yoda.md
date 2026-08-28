# Deploying Funes 4.0 to yoda, alongside 3.x

Funes 4.0 runs as a **completely independent install**. The 3.x install is not
modified in any way. The two share exactly two things: the LLM on `:8080` and
the embedding model on `:8081`.

| | 3.x (existing) | 4.0 (new) |
|---|---|---|
| clone | `~/Funes` | `~/Funes-v4` |
| service | `funes.service` | `funes-v4.service` |
| port | 8484 | 8485 |
| database | `~/.funes/memory.db` | `~/.funes-v4/memory.db` |
| workspace | `~/.funes/workspace` | `~/.funes-v4/workspace` |
| WhatsApp | on | **off** |
| newsletter / cron | on | **off** |
| shell | on | **off** |
| vision | `:8083` | **none** — the `:8080` model handles images |

> **The failure that matters.** Funes migrates whatever database it opens, and
> 4.0's migration rebuilds `memories` and repartitions the vector index. There
> is no downgrade. If `FUNES_DB` is unset the binary opens
> `~/.funes/memory.db` — the 3.x one. Every path is set explicitly in
> `scripts/funes-v4.service` for this reason; do not remove any of them.

---

## 0. Pre-flight (read-only, safe to run any time)

```bash
# Confirm the 3.x install is where we think it is, and still running.
systemctl --user status funes --no-pager | head -3
ls -la ~/Funes ~/.funes/memory.db

# 8485 must be free, and nothing should already exist at the v4 paths.
ss -ltnp | grep -E ':(8484|8485)\b' || echo "8485 free"
ls -d ~/Funes-v4 ~/.funes-v4 2>/dev/null && echo "WARNING: v4 paths already exist"

# Shared config read by BOTH installs. If it exists, check it sets nothing
# that should differ between them (FUNES_DB, FUNES_PORT, FUNES_WORKSPACE_DIR).
# The unit sets those explicitly so it cannot win, but know what is in there.
cat ~/.funes/config 2>/dev/null || echo "no ~/.funes/config — good"
```

## 1. Clone and build (touches nothing existing)

```bash
git clone -b feat/v4-users-permissions \
    git@github.com:JulioAuto2ML/Funes.git ~/Funes-v4
cd ~/Funes-v4
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2          # -j2, not -j$(nproc): yoda is RAM-limited
                                 # and the model is resident
cd build && ctest --output-on-failure
```

All 28 tests must pass before going further. `migration` is the one that
matters most here — it is the CI proof that the upgrade in step 2 does not
lose rows.

## 2. Migrate the 3.x data

The copy is read-only on the source. Do **not** `cp` the database: 3.x is
running in WAL mode and a plain copy can catch a torn state. `.backup` is
SQLite's online-backup path and is safe against a live writer.

**There is no `sqlite3` CLI on yoda.** Use python's stdlib module, which is
the same library. Every read below opens the source with a `file:...?mode=ro`
URI so a typo cannot write to the live database.

```bash
mkdir -p ~/.funes-v4 ~/.funes-v4/workspace

# Database — online backup, read-only on the source by construction.
python3 - <<'EOF'
import sqlite3, os
src = sqlite3.connect(f"file:{os.path.expanduser('~/.funes/memory.db')}?mode=ro", uri=True)
dst = sqlite3.connect(os.path.expanduser("~/.funes-v4/memory.db"))
src.backup(dst)
dst.close(); src.close()
print("backup complete")
EOF

# Workspace — copied flat. Funes 4.0's first start moves it into
# ~/.funes-v4/workspace/1/ (user 1 = the admin), which is the tested path.
cp -a ~/.funes/workspace/. ~/.funes-v4/workspace/
```

Verify the copy before anything migrates it — the counts must match:

```bash
python3 - <<'EOF'
import sqlite3, os
def counts(path, ro=True):
    p = os.path.expanduser(path)
    uri = f"file:{p}?mode=ro" if ro else p
    c = sqlite3.connect(uri, uri=ro)
    out = {t: c.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
           for t in ("memories", "turns")}
    c.close(); return out
a, b = counts("~/.funes/memory.db"), counts("~/.funes-v4/memory.db")
print("3.x:", a); print("4.0:", b)
print("MATCH" if a == b else "MISMATCH — STOP")
EOF
```

## 3. Secrets — none, deliberately

This install gets no secrets at all.

Gmail credentials are absent so it cannot send a second copy of the
newsletter. WhatsApp settings are absent so no second poller can start. And as
of 2026-08-26 the **Tavily key is withheld too**: web search in a multi-user
install needs one operator-owned key that no member can see or exfiltrate, and
that design is not built yet (see `v4-remaining-work.md`, "Deferred by
decision"). Shipping the key before deciding where it lives would put it on a
box where members exist.

The consequence is that `web_search` and `web_fetch` will fail on this install
until the key is added. That is the intended state.

```bash
cd ~/Funes-v4
ls config/funes.local 2>/dev/null && echo "UNEXPECTED — this install should have no secrets" \
                                  || echo "no local secrets, as intended"
```

## 4. Install the service

```bash
cd ~/Funes-v4
cp scripts/funes-v4.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now funes-v4
journalctl --user -u funes-v4 -f
```

Expect in the log, on first start only:

```
[memory] migrated to per-user memory scoping
[memory] rebuilding vector index with per-user partitioning
[funes] workspace is now per-user: moved N entries into /home/julio/.funes-v4/workspace/1
[funes] no accounts yet — open the web UI to create the first admin
```

The vector rebuild drops the embeddings and refills them in the background
from `:8081`. Recall falls back to keyword search until that finishes, which
for a few dozen memories is seconds.

The backfill is capped at 256 per call, but the startup thread now drains it
in a loop, so one start finishes the whole database. (It used to run exactly
once, which meant restarting once per 256 memories — if you remember doing
that, that is why.) One line reports the total when it is done:

```bash
journalctl --user -u funes-v4 --no-pager | grep -E "backfilled|embedding"
```

If the embedding endpoint on `:8081` is not up yet, the thread says how many
are still missing and retries every 5 minutes for about an hour rather than
giving up until the next restart. A `giving up on N memory embedding(s)` line
means `:8081` was down that whole time — start it and restart `funes-v4`.

## 5. Confirm 3.x is untouched

Run this immediately after the first start. It is the whole point of the
exercise.

```bash
systemctl --user status funes --no-pager | head -3     # still active
curl -sf http://127.0.0.1:8484/api/status >/dev/null && echo "3.x still serving"
ls ~/.funes/workspace | head                           # still flat, not 1/

python3 - <<'EOF'
import sqlite3, os
c = sqlite3.connect(f"file:{os.path.expanduser('~/.funes/memory.db')}?mode=ro", uri=True)
print("memories:", c.execute("SELECT COUNT(*) FROM memories").fetchone()[0])
leaked = c.execute(
    "SELECT name FROM sqlite_master WHERE name IN ('users','sessions')").fetchall()
print("4.0 tables in the 3.x db:", leaked or "none — good")
cols = [r[1] for r in c.execute("PRAGMA table_info(memories)")]
print("user_id column present:", "user_id" in cols, "(must be False)")
c.close()
EOF
```

The last two checks are the sharp ones: a `users` table, or a `user_id` column
on `memories`, means the 4.0 binary opened the 3.x database. There is no
downgrade from that. Stop and investigate before doing anything else.

## 6. First account, then the bench

```bash
# Either from the browser's first-run screen at http://yoda:8485, or:
cd ~/Funes-v4 && FUNES_DB=~/.funes-v4/memory.db ./bin/funes useradd julio --admin
```

Note the explicit `FUNES_DB` on the CLI: the subcommands read the same
environment as the server, and outside systemd the unit's variables are not
set. Getting this wrong creates the account in the 3.x database.

Then, before relying on it:

```bash
cd ~/Funes-v4
python3 scripts/funes_bench.py --url http://127.0.0.1:8485 --user julio --skip-web
```

The bench needs an account — 4.0 refuses `/api/chat` without one, and without
`--user` it stops with an explicit message rather than reporting a model that
cannot call tools. It prompts for the password, or reads
`$FUNES_BENCH_PASSWORD`. `--skip-web` because no Tavily key is configured on
this install yet.

Phase 4 narrows the tool schema each agent sees, so this is the run that says
whether the model still calls the right tools.

## 7. One-off: clear out what the old scheduler wrote

**Already done on this install** (2026-08-28: 10 auto-memories removed, all 36
transcripts kept). Kept here for a fresh migration from a 3.x database.

Only needed on a database that predates the `Persist::TurnsOnly` change. Until
then every cron firing also wrote an auto-memory phrased `User said: "<the
job's task>" — I replied: "..."` — the scheduler talking to itself, stored as
though the person had said it. They are not inert: two on this box carried
`recall_count` of 13 and 15, meaning they had been injected into that many real
conversations.

The command reports without deleting unless you pass `--apply`. Read the
numbers first — there is no undo and no downgrade.

```bash
cd ~/Funes-v4
FUNES_DB=~/.funes-v4/memory.db ./bin/funes cron-cleanup            # dry run
FUNES_DB=~/.funes-v4/memory.db ./bin/funes cron-cleanup --apply
```

The per-run transcripts are reported but **kept** unless you add
`--drop-sessions`. They no longer clutter anything — `/api/sessions` hides
`cron-*` and `?cron=1` is how you reach them — and they are the only record of
what a run older than the last one did, since `cron_jobs.last_output` keeps a
preview of the last run only. Take a database backup first either way; the
snippet is in section 2.

## Rolling back

Stop and disable `funes-v4`, then delete `~/Funes-v4` and `~/.funes-v4`.
Nothing else is involved — that is what "independent install" buys.

```bash
systemctl --user disable --now funes-v4
rm -rf ~/Funes-v4 ~/.funes-v4
```
