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
| `post_tweet.py` | One LinkedIn post per invocation, from the issue JSON — and only if the day's run record says the issue was sent. The post is the text and image; the source link goes in the **first comment**, a second API call (see below). |
| `generate_images.py` | One LinkedIn illustration per item, from each item's `image_prompt`, through a running ComfyUI (Qwen Image 2.1). Writes `images/<pub>/<date>/<n>.png`. |
| `run_images.sh` | The cron entry for the above: lends the GPU from the LLM for a window and always gives it back (EXIT trap). Does nothing, GPU untouched, if there is nothing to render. |
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

## The link goes in the first comment

LinkedIn demotes a post that carries an outbound link, so `post_tweet.py` posts
the text (and image) alone and then comments the source URL on it. A post is
therefore two API calls, and the second can fail after the first has made
something public. So the post id is written to
`posted/<publication>/<date>.json` the moment LinkedIn returns it, *before* the
comment is attempted:

```json
{"1": {"post_id": "urn:li:share:…", "comment": "done", "posted_at": "…"},
 "2": {"post_id": "urn:li:share:…", "comment": "pending", "comment_error": "HTTP 403 …"}}
```

- Re-running a slot only ever finishes its comment. It cannot repost.
- Every invocation also sweeps the day's earlier slots for comments still owed,
  so a transient failure heals at the next cron slot.
- The comment is retried (5 s, then 15 s) on 404/429/5xx and dropped connections
  — LinkedIn can 404 a post it created a second ago — and **not** on a 403,
  which is a missing permission and will not fix itself.
- Exit `4` = posted, but a comment is still owed. The post stays up without its
  link until a re-run or the next slot; look at `post_log.txt`.
- A corrupt `posted/` file is an error, not an empty record: treating it as
  empty would repost the day.

The comment is `POST /v2/socialActions/{post urn}/comments` as the member that
posted, which needs the `w_member_social` scope the post already requires.

## Illustrations (LinkedIn only)

The curator may give each item an `image_prompt`; it is stored in the issue JSON
and listed in `image_prompts_<date>.json`. `run_images.sh` renders them once the
day's run record says `sent`, and `post_tweet.py` attaches `images/.../<n>.png`
to slot *n*. Images never reach the newsletter, and a missing or failed image
means that post goes out text-only.

Measured on yoda (RX 9060 XT 16 GB): ~170 s for the first image, ~106 s for each
after, nearly all of it the CPU VAE decode — the GPU decode runs out of memory
because the diffusion model stays resident, which is why ComfyUI runs with
`--cpu-vae`. **Funes has no model for the whole window** (~17 min for nine), so
schedule it where that is cheapest, and before the first post slot.

`run_images.sh` exit codes: `2` the issue was never sent, `4` VRAM never freed,
`5` the model did not come back healthy (look at this first), `6` already
running. The log is `images_log.txt`; ComfyUI's own is `comfy_images.log`.

## Scheduling

Funes has no scheduler. `run_publication.sh` is the smallest thing that closes
the gap that mattered — the newsletter was started by hand while the LinkedIn
posts consuming it were on cron, so a failed newsletter still left cron posting
yesterday's items.

Two editions share this machinery (`ai-pulse`, `ai-pulse-es`), interleaved:
English at :00, Spanish at :30, so a feed sees one post every half hour.

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

The Spanish edition runs the same way, offset so the two never contend for the
model or the GPU. The newsletter is a Funes cron job (`schedule_job`, agent
`curator`, task "Publish today's ai-pulse-es issue"); the rest is crontab:

```cron
# Spanish edition: images after the Spanish issue, posts on the half hour.
30 10 * * * /home/julio/funes-julio/publishing/run_images.sh ai-pulse-es >> ~/Documents/X_posts/images_log.txt 2>&1
30 13 * * * /usr/bin/python3 /home/julio/funes-julio/publishing/post_tweet.py 1 --publication ai-pulse-es --dir ~/Documents/X_posts >> ~/Documents/X_posts/post_log.txt 2>&1
30 14 * * * … post_tweet.py 2 --publication ai-pulse-es …
...
30 21 * * * … post_tweet.py 9 --publication ai-pulse-es …
```

The Spanish issue is also limited to the nine slots its config asks for (the
English crontab has a tenth that the issue has never filled).

A scheduler inside Funes is a bigger question — it wants a job table, retries
and a UI — and is deliberately not this.

## Deployment

Same `git pull` as the binary (see the yoda deployment notes). Nothing to copy.

## Tests

`publishing/tests/`, run by `ctest` as the `publishing` test and by
`--self-test` on the host. One suite, both places.
