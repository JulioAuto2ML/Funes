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
git clone -b feat/v4-users-permissions <repo-url> ~/Funes-v4
cd ~/Funes-v4
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2          # -j2, not -j$(nproc): yoda is RAM-limited
                                 # and the model is resident
cd build && ctest --output-on-failure
```

All 27 tests must pass before going further.

## 2. Migrate the 3.x data

The copy is read-only on the source. Do **not** `cp` the database: 3.x is
running in WAL mode and a plain copy can catch a torn state. `.backup` is
SQLite's online-backup path and is safe against a live writer.

```bash
mkdir -p ~/.funes-v4

# Database — read-only on the source, by construction.
sqlite3 -readonly ~/.funes/memory.db ".backup '$HOME/.funes-v4/memory.db'"

# Workspace — copied flat. Funes 4.0's first start moves it into
# ~/.funes-v4/workspace/1/ (user 1 = the admin), which is the tested path.
mkdir -p ~/.funes-v4/workspace
cp -a ~/.funes/workspace/. ~/.funes-v4/workspace/

# Verify the copy before anything migrates it.
sqlite3 ~/.funes-v4/memory.db \
  "SELECT 'memories', COUNT(*) FROM memories UNION ALL
   SELECT 'turns', COUNT(*) FROM turns;"
```

Compare those counts against the source (same query, `-readonly`, on
`~/.funes/memory.db`). They must match before you continue.

## 3. Secrets

Only the Tavily key. Gmail credentials are deliberately absent so this install
cannot send a second copy of the newsletter; WhatsApp settings are absent so
no second poller can start.

```bash
cd ~/Funes-v4
grep '^FUNES_TAVILY_API_KEY=' ~/Funes/config/funes.local > config/funes.local
chmod 600 config/funes.local
cat config/funes.local        # confirm it is exactly one line
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

## 5. Confirm 3.x is untouched

Run this immediately after the first start. It is the whole point of the
exercise.

```bash
systemctl --user status funes --no-pager | head -3     # still active
curl -sf http://127.0.0.1:8484/api/status >/dev/null && echo "3.x still serving"
sqlite3 -readonly ~/.funes/memory.db \
  "SELECT COUNT(*) FROM memories;"                      # unchanged
sqlite3 -readonly ~/.funes/memory.db \
  "SELECT name FROM sqlite_master WHERE name='users';"  # MUST be empty
ls ~/.funes/workspace | head                            # still flat, not 1/
```

The `users` table check is the sharp one: if it comes back non-empty, the 4.0
binary opened the 3.x database and the isolation failed. Stop and investigate
before doing anything else.

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
python3 scripts/funes_bench.py          # against the model on :8080
```

Phase 4 narrows the tool schema each agent sees, so this is the run that says
whether the model still calls the right tools.

## Rolling back

Stop and disable `funes-v4`, then delete `~/Funes-v4` and `~/.funes-v4`.
Nothing else is involved — that is what "independent install" buys.

```bash
systemctl --user disable --now funes-v4
rm -rf ~/Funes-v4 ~/.funes-v4
```
