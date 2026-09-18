# publishing/

The scripts that turn a day's selected items into a rendered issue, put it in
people's inboxes, and post it to LinkedIn through the day. They used to live
only in `~/Documents/X_posts` on two machines and were deployed by `scp` — no
history, no review, and two copies that could silently diverge, one of which
decides whether mail goes out.

| Script | What it does |
|---|---|
| `publish_issue.py` | The current path. Renders a publication's artifacts from its issue JSON, re-checks every link, sends, writes the run record. Invoked by the `publish_issue` tool. |
| `send_newsletter.py` | Gmail SMTP. Takes `--subject` and `--recipients` from the publication config. |
| `post_tweet.py` | One LinkedIn post per invocation, from the issue JSON — and only if the day's run record says the issue was sent. |
| `run_publication.sh` | The cron entry: runs the curator, then checks the run record rather than believing it. |
| `publish_newsletter.py` | The older path, reading the hand-written `X_posts_<date>.txt`. Still works, still tested; kept until the new path has run a clean week. Also the entry point to the test suite (`--self-test`). |

## Code here, data there

These scripts are code and live in the repo. The things they read and write are
not, and stay on the sending host:

| In the repo | On the host (`$FUNES_PUBLISH_DIR`) |
|---|---|
| the scripts above | `issues/<pub>/<date>.json` — the source of truth |
| `newsletter_template.html` (default) | `runs/<pub>/<date>.json` — what happened |
| `../publications/*.yaml` — the settings | `X_posts_<date>.txt`, `newsletter_<date>.html` — generated |
| — | `.env` (Gmail + LinkedIn credentials), `subscribers.txt` |

The issue directory is `--dir PATH`, else `$FUNES_PUBLISH_DIR`, else the current
working directory — in that order. It is deliberately *not* the script's own
directory, which is what tied the old copies to the machine they sat on.

Secrets are never in the repo, and `--dir` never needs to be a path inside it.

Nothing here parses a publication YAML. `publish_issue` copies the subject,
artifacts, channels and CTA into the issue JSON, and these read them from there
— one config dialect, in one language, in one place.

## Running

```sh
# Normally the publish_issue tool does this; by hand for a re-run or a rehearsal.
python3 publishing/publish_issue.py 2026-07-31 --publication ai-pulse \
        --dir ~/Documents/X_posts --send

python3 publishing/post_tweet.py 3 --dir ~/Documents/X_posts --dry-run
python3 publishing/publish_newsletter.py --self-test
```

Exit codes are the interface a caller branches on. `publish_issue.py`: `0` sent
(with `--send`), `1` bad input, `2` broken links and nothing sent.
`post_tweet.py`: `3` means it refused because the day's issue did not go out.
A cron slot past the end of the issue exits `0` with "Nothing to post" — an
issue can carry fewer items than there are slots, because `publish_issue` drops
an item no page in the pool supports rather than sinking the whole issue, so a
short day is expected rather than a failure.

## Scheduling

Funes has no scheduler. `run_publication.sh` is the smallest thing that closes
the gap that mattered — the newsletter was started by hand while the LinkedIn
posts consuming it were on cron, so a failed newsletter still left cron posting
yesterday's items.

```cron
# Produce and send the issue, then post from it through the day.
0  9 * * * /path/to/Funes/publishing/run_publication.sh ai-pulse >> ~/Documents/X_posts/publication_log.txt 2>&1
30 12 * * * /usr/bin/python3 /path/to/Funes/publishing/post_tweet.py 1  >> ~/Documents/X_posts/post_log.txt 2>&1
30 13 * * * /usr/bin/python3 /path/to/Funes/publishing/post_tweet.py 2  >> ~/Documents/X_posts/post_log.txt 2>&1
...
30 21 * * * /usr/bin/python3 /path/to/Funes/publishing/post_tweet.py 10 >> ~/Documents/X_posts/post_log.txt 2>&1
```

Both ends read the same run record, so they agree on what "today went out"
means. Set `FUNES_PUBLISH_DIR` in the crontab or pass `--dir`.

A scheduler inside Funes is a bigger question — it wants a job table, retries
and a UI — and is deliberately not this.

## Deployment

Same `git pull` as the binary (see the yoda deployment notes). Nothing to copy.

## Tests

`publishing/tests/`, run by `ctest` as the `publishing` test and by
`--self-test` on the host. One suite, both places.
