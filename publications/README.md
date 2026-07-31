# publications/

One YAML per publication. `harvest_candidates` reads the sources block,
`publish_issue` reads the rest, and the `curator` agent reads none of it — it is
handed a pool and a voice note and writes what it is given, which is why a
second publication needs no second agent.

## Adding one

```
publications/security-brief.yaml     # settings
publications/voice/security-brief.md # who reads it, what runs, how it reads
```

Then run the curator against it. There is nothing else: no prompt to write, no
script to fork, no code to change. `tests/test_publication.cpp` loads a
publication that exists only inside the test, which is what keeps that claim
honest.

## What goes where

**The YAML is settings.** Queries, recency window, per-source and per-story
caps, how many items an issue wants, where its artifacts land, who it is sent
to. Every one of them is a number, a path or a list.

**The voice file is prose**, because voice is the one part a model has to read
rather than obey. The moment "sceptical but not cynical" becomes an enum,
somebody has lost an argument with a config format.

The split is the same one the whole pipeline is built on: automate what has one
right answer, and leave the model exactly the part that doesn't.

## Fields

Everything except `id` is optional; the defaults reproduce what the AI
newsletter did before any of this was configurable. See
[ai-pulse.yaml](ai-pulse.yaml) for the annotated version.

| Block | Field | Default | Notes |
|---|---|---|---|
| | `id` | — | required; `[a-z0-9_-]` |
| | `title` | the id | |
| | `subject` | `<title> · {date}` | `{date}` long form, `{iso_date}` = 2026-07-30 |
| `voice` | `prompt_file` | none | relative to this directory |
| `sources` | `queries` | none | up to 6; the curator may add its own to widen a thin pool |
| | `time_range` | `day` | `day`/`week`/`month`/`year` |
| | `per_query` | 10 | |
| | `max_candidates` | 25 | the pool is sized to fit in context |
| | `max_age_days` | 2 | |
| | `dedup_against_last_issues` | 7 | don't run the same story twice a week |
| | `exclude_domains` | none | *added to* the global social-platform list |
| `selection` | `count` | 10 | |
| | `min_count` | 8 | below this the issue is refused, loudly, in the run record |
| | `max_per_source` | 0 (no cap) | |
| | `max_per_story` | 2 | one story, two outlets, so there is a choice |
| `artifacts` | `kind` | — | `posts_txt` or `newsletter_html` |
| | `path` | — | `{workspace}` and `{iso_date}` substituted |
| | `template` | repo default | `newsletter_html` only |
| `channels` | `kind` | — | `email` |
| | `recipients_file` | `subscribers.txt` | relative to the issue directory |

Omitting `channels` entirely gets the default email channel. An explicitly
empty `channels: []` is a publication that renders and does not send, which is
a real thing to want and is not treated as a failure.

## Where the values end up

The scripts in `publishing/` never read this directory. `publish_issue` copies
the subject, artifacts and channels into the issue JSON, and they read them from
there — one config dialect, in one language, in one place. The issue JSON was
already the source of truth for what goes out; this makes it the source of truth
for how.
