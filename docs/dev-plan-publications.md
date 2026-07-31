# Dev plan: generalizing newsletter generation into publications

> Written 2026-07-31, the day the current pipeline produced its first clean
> end-to-end run and, in the same run, shipped an item pointing at a Stripe
> checkout page.
>
> **Status (2026-07-31): all six phases implemented.**
>
> | Phase | Landed as |
> |---|---|
> | 1 — scripts into the repo | `publishing/`, tests under `ctest` |
> | 2 — `harvest_candidates` | `src/core/tools/harvest.{h,cpp}`, `tavily.*`, `page_text.*` |
> | 3 — the selection turn | `agents/curator.yaml`, evidence check in `src/core/tools/issue.cpp` |
> | 4 — `publish_issue` + run records | `src/core/tools/issue.cpp`, `publishing/publish_issue.py` |
> | 5 — publications as config | `publications/`, `src/core/publication.{h,cpp}` |
> | 6 — scheduling + LinkedIn | `publishing/run_publication.sh`, `publishing/post_tweet.py` |
>
> Two decisions were taken against the letter of §2.2 and §8 and are marked
> **[decided]** in place below.

## The premise

> Selecting the ~10 articles from the web search and checking them for
> relevance and correct links is the only real task of the LLM, apart from
> keeping the user informed. Everything else is automation that could be built
> into tools.

That is the whole design constraint. Everything below follows from taking it
literally: find every part of the pipeline where a model is being shepherded
through a deterministic step, and delete the shepherding by making the step a
tool.

---

## 1. Where the LLM's effort actually goes today

Three agents run per newsletter (`agents/ai-newsletter.yaml` →
`agents/researcher.yaml` + `agents/newsletter-publisher.yaml`), with a combined
`max_steps` of 46. Classify what those steps do:

| Step | Judgement or automation? | Who does it now |
|---|---|---|
| Get today's date | automation | `ai-newsletter`, via `execute_shell date` |
| Run the search queries | automation | `researcher`, via `web_search` |
| Drop social/junk domains | automation (already a tool) | `web_search.cpp` exclude list |
| Deduplicate, drop stale items | automation | nobody — done implicitly, badly |
| Fetch each candidate | automation | `researcher`, via `web_fetch` |
| **Pick 10 of N** | **judgement** | the model |
| **Write the post text** | **judgement** | the model |
| **Confirm the link matches the story** | **judgement** | nobody |
| Format as `N. emoji text` / `URL — headline` | automation | the model, from a prompt spec |
| Write the posts `.txt` | automation | `newsletter-publisher`, `write_file` |
| Render HTML from the template | automation (already a tool) | `publish_newsletter.py` |
| Check every link resolves | automation (already a tool) | `publish_newsletter.py` |
| Send | automation (already a tool) | `send_newsletter.py` |
| Report what happened | **judgement (thin)** | the model, ad hoc |

Four rows are judgement. Ten are automation, and six of those ten are being
performed *by a language model reading instructions about how to perform them*.
The framework work of the last two days — `require_tools`, `tool_limits`, the
reserved last step, withholding tools on the final turn — was all built to stop
a model from wandering off in the middle of those ten rows. It works, and it
should stay, because it fixes a general class. But the cheapest way to stop a
model from getting a deterministic step wrong is not to ask it.

### The bug this is really about

On 2026-07-31 item #8 was a post about an OpenAI breach post-mortem, linked to
`https://hansenit.solutions/stripe-checkout-result`. It returned 200, so
`publish_newsletter.py`'s checker passed it, and it would have gone out.

That is not a model being dumb. It is an architecture that requires a model to
**retype a URL it read three steps earlier**, from a transcript that has since
been compressed, previewed, and stored-by-reference. Every URL in the posts
file is a fresh act of transcription. Ask for ten of those a day and you will
get a wrong one every few weeks, forever, and no amount of prompt tightening
changes the arithmetic.

The fix is not a better checker. It is to stop asking the model for URLs.

---

## 2. Target architecture

Three deterministic stages with one model call in the middle.

```
  harvest_candidates          →   [ one LLM turn ]   →   publish_issue
  (native tool)                   select + write         (native tool / script)
  ─────────────────────           ──────────────────     ─────────────────
  run queries                     pick ids               resolve ids → items
  dedup by URL + title            write post text        render .txt + HTML
  drop stale / seen before        pick emoji             re-check links
  fetch + extract each page       quote evidence         verify evidence
  check links up front            order them             send
  assign stable ids                                      write run record
  return a candidate pool                                → structured result
```

The model never sees a URL it has to reproduce. It sees a numbered pool and
returns numbers.

### 2.1 `harvest_candidates` — a new native tool

Roughly `src/core/tools/harvest.cpp`, reusing the Tavily client in
`web_search.cpp` and the extractor in `web_fetch.cpp`.

Input: a publication id (see §3) or an inline query set.

What it does, in order, none of it novel:

1. Run each configured query with `topic: news` and the configured
   `time_range`, taking `max_results` per query.
2. Normalize URLs (strip `utm_*`, resolve one level of redirect, canonical
   host) and dedup — across queries *and* against the last N issues of this
   publication, so the same story does not run twice in a week.
3. Drop anything older than the recency window, using the search result's
   published date where present.
4. `web_fetch` each survivor. A fetch failure or a non-2xx drops the candidate
   **here**, before it is ever offered. This is the pre-flight link check, and
   it happens before selection rather than after writing.
5. Keep the extracted page text in the result store, and give each candidate a
   stable `id` valid for the session.

Output: JSON, not the flat text `web_search` returns today.

```json
{
  "publication": "ai-pulse",
  "date": "2026-07-31",
  "candidates": [
    {"id": 1, "title": "...", "url": "https://...", "source": "reuters.com",
     "published": "2026-07-30", "excerpt": "first ~600 chars of the article",
     "result_id": 41}
  ]
}
```

Sized so the pool fits in context: 20–30 candidates at ~600 chars of excerpt is
~5K tokens, which fits `ai-newsletter`'s current 8192 with room to work. The
full text stays behind `result_id` for the rare case the model wants to read
more before deciding.

**Note on `web_search`'s current shape.** It returns `title\nsnippet\nurl`
blocks with no identifiers (`web_search.cpp:91`). Nothing addressable, so
nothing referenceable — which is why the URL has to be retyped. Leave
`web_search` alone; it is the right tool for open-ended research. `harvest_candidates`
is a different tool with a different contract.

### 2.2 The one LLM turn

An agent — call it `curator` — with `tools: [harvest_candidates, read_result]`,
`require_tools: [harvest_candidates]`, and an `answer_schema`:

```yaml
answer_schema:
  type: object
  required: [items]
  properties:
    items:
      type: array
      minItems: 8
      maxItems: 10
      items:
        type: object
        required: [id, emoji, text, evidence]
        properties:
          id:       {type: integer}
          emoji:    {type: string}
          text:     {type: string}
          evidence: {type: string}
```

Note what is absent: **no `url` field**. The model cannot supply a URL, so it
cannot supply a wrong one. `publish_issue` resolves `id` against the pool.

`evidence` is the relevance check, converted from a judgement into a string
search. The model must quote a verbatim phrase from the candidate's extracted
text that supports the claim its post makes. `publish_issue` then checks the
quote actually occurs in the stored page text (normalized whitespace, case-fold,
allow one elision). A post about an OpenAI breach cannot produce a supporting
quote from a Stripe checkout page — the failure mode that got through today
becomes a hard, deterministic reject.

This is a nudge-able contract, not a wall: a failed evidence check comes back
as one concrete violation, exactly like `validate_answer` does today
(`answer_schema.h`), and the model re-picks.

**[decided] It became `publish_issue`'s parameter schema, not an
`answer_schema`.** The two halves of this plan contradicted each other: §2.2
makes the selection the agent's final answer, while §2.3 says `require_tools`
plus an exit code is what makes the send checkable — and that only works if
publishing is a tool call the agent makes. It is now a tool call, and the schema
below is the tool's argument schema. Every property survives, `url` is still
absent, and the checks still come back as one concrete violation. What changes:
the curator learns whether the send worked, `require_tools: [harvest_candidates,
publish_issue]` covers it, and the final answer is the §4 paragraph for a human
— which wants no schema, for the reason `newsletter-publisher` records.

The paragraph below still describes why a schema was right *here* and wrong
there, and that reasoning is what the argument schema inherits:

**This is a real use of a schema where `newsletter-publisher`'s was not.**
The rejection recorded in `agents/newsletter-publisher.yaml` still stands: for
*that* agent the schema would have described a side effect it could not verify.
Here the schema *is* the interface between the model and the publisher, and
every field in it is checkable against something the tool already holds. See
also the memory note `project-no-schema-for-newsletter`.

### 2.3 `publish_issue` — deterministic, verifiable, one call

**[decided] Split in two along the line where the guarantees are.** Steps 1 and
2 are native (`src/core/tools/issue.cpp`): the pool and the page text are
already in the process, and a check that decides whether mail goes out belongs
next to the thing it checks. Steps 3–6 are `publishing/publish_issue.py`, which
reuses the rendering, link checking and SMTP that phase 1 put under test rather
than reimplementing them in C++. The tool returns the script's exit code, so the
contract in the last paragraph of this section is unchanged.

Extends what `publish_newsletter.py` already does well. Takes the curator's
items plus the publication config, and:

1. Resolves each `id` to the harvested candidate. Unknown id → error, no send.
2. Verifies each `evidence` quote against the stored page text. Miss → error,
   naming the item, no send.
3. Re-checks every link (cheap, and the harvest may be an hour old).
4. Renders every configured **artifact**: the posts `.txt`, the newsletter
   HTML, and anything else a channel needs.
5. Sends to the configured channels.
6. Writes a run record (§4).

Exit codes keep the current contract shape — 0 sent, 2 blocked, 1 bad input —
because `require_tools` plus an exit code is what makes "it really sent" a
checkable fact rather than a claim. That mechanism is doing its job and should
not be replaced by a schema.

### 2.4 What the agent chain becomes

Three agents and ~46 steps become one agent and about four: harvest, maybe one
`read_result`, answer, and — after `publish_issue` — a two-line summary to the
user. `researcher` stays as a general-purpose agent; it simply stops being on
this path. `newsletter-publisher` and `ai-newsletter` are retired once the new
path has run clean for a week.

---

## 3. Generalization: publications as config

Today "the AI newsletter" is spread across two YAML prompts, a hardcoded
template filename, a hardcoded posts-file naming convention, and a subscriber
list. Pull it into one file so a second publication is a config, not a fork.

`publications/ai-pulse.yaml`:

```yaml
id: ai-pulse
title: "AI Pulse"
subject: "AI Pulse · {date:%A, %B %-d, %Y}"
timezone: America/Argentina/Buenos_Aires

sources:
  queries:
    - "artificial intelligence news"
    - "AI model release"
    - "AI regulation policy"
  time_range: day
  per_query: 10
  exclude_domains: [tiktok.com, instagram.com, facebook.com]   # + the global list
  dedup_against_last_issues: 7

selection:
  count: 10
  min_count: 8          # what the curator may fall back to
  max_per_source: 2     # no three-Reuters issues

voice:
  prompt_file: publications/voice/ai-pulse.md   # tone, audience, what to skip

artifacts:
  - kind: posts_txt
    path: "{workspace}/X_posts_{date}.txt"
  - kind: newsletter_html
    template: newsletter_template.html
    path: "{workspace}/newsletter_{date}.html"

channels:
  - kind: email
    recipients_file: subscribers.txt
  # - kind: linkedin        # already cron-driven off posts_txt; see §5
```

A second publication (a security roundup, a local-news digest) is then a new
YAML plus a voice file. No new agent, no new prompt engineering, no new script.

**What stays out of the config:** anything a model must reason about. The voice
file is prose because voice is judgement; everything else is fields because
everything else is not.

---

## 4. Keeping the user informed

The second half of the user's premise, and the part with no design today. A run
currently ends in an SSE stream nobody is watching, or a `FAILED — ...` string.

`publish_issue` writes a run record per issue — landed at
`<workspace>/runs/ai-pulse/2026-07-31.json`, kept out of `issues/` so the
"already ran this week" check doesn't read its own exhaust:

```json
{"publication": "ai-pulse", "date": "2026-07-31", "status": "sent",
 "harvested": 27, "selected": 10, "rejected": [{"id": 8, "reason": "evidence not found in page"}],
 "links": {"ok": 10, "suspect": 5, "broken": 0},
 "recipients": 1, "duration_s": 214}
```

Then:

- The agent's final message to the user is one paragraph generated from that
  record — the one genuinely linguistic thing left in the reporting path.
- A `status: blocked` record is what a scheduled run leaves behind for a human
  to find, instead of a silence.
- Bot-blocked domains (the 401/403/429 SUSPECT set) are recorded but never
  block, as today. Worth a follow-up: a per-publication allowlist of hosts
  known to bounce HEAD requests, so "suspect" shrinks toward meaning something.

---

## 5. Two warts to fix while we are in here

**The publishing scripts are not in version control.** `publish_newsletter.py`
and `send_newsletter.py` live only in `/home/julio/Documents/X_posts` on two
machines and are deployed by `scp`. There is no history, no review, no CI, and
two copies that can silently diverge — and `publish_newsletter.py` is now the
component that decides whether mail goes out. Move them into the repo
(`publishing/`), keep `--self-test` and add real unit tests around
`parse_posts` / `check_link` / the new evidence check, and deploy them by the
same `git pull` the binary uses (`reference-yoda-deployment`). Secrets stay
where they are: `.env` on the host, never in the repo.

**The posts file has two independent parsers.** `publish_newsletter.py:parse_posts`
reads it with one regex set; `post_tweet.py:parse_posts` (line 41) reads it with
a different one, and cron invokes `post_tweet.py N` ten times a day, 13:00–22:00,
to post item N to LinkedIn. Two hand-rolled parsers of an undocumented format
that a language model writes by hand is three fragile things stacked.

Under this plan the `.txt` stops being the source of truth and becomes a
generated artifact — the issue JSON is authoritative. `post_tweet.py` should
read the issue JSON directly and index into `items[N-1]`. That also gets
LinkedIn posts the link-verified URL instead of a re-parsed one.

---

## 6. Scheduling

Funes has no scheduler; the newsletter run is started by hand, while the
LinkedIn posts that consume its output are on cron. That asymmetry is why a
failed newsletter can still leave cron posting yesterday's items.

Simplest thing that works: a cron entry on yoda that POSTs to
`/api/chat` with `agent: curator`, and a `post_tweet.py` that refuses to post
when the day's run record is missing or `status != sent`. A scheduler inside
Funes is a bigger question (it wants a job table, retries, and a UI) and is out
of scope here — note it and move on.

---

## 7. Phases

Ordered so each ships independently and is useful alone.

**Phase 1 — scripts into the repo.** ✅ Landed. Copied rather than moved: the
originals stay in `~/Documents/X_posts` until the repo copies have run a week,
so neither machine breaks meanwhile. The one behaviour change is that the issue
directory is now `--dir` / `$FUNES_PUBLISH_DIR` / cwd instead of the script's own
directory, which is what tied the old copies to the machine they sat on. Verified
byte-identical output against the original on the 2026-07-31 posts file.

Original text: move `publish_newsletter.py` and
`send_newsletter.py` under `publishing/`, add unit tests, switch deployment to
`git pull`. No behaviour change. Unblocks everything else and is the only phase
with no design risk.

**Phase 2 — `harvest_candidates`.** ✅ Landed. Queries, dedup, recency, fetch,
pre-flight link check, stable ids, JSON out. No Tavily fixture in the end: every
decision made between the search call and the fetch call is a pure function, and
those are tested directly, which is both cheaper and more honest than replaying a
recorded response. `harvest_candidates` is exempt from the result store — a
preview of a menu is not a menu — so the pool reaches the model whole.

**Phase 3 — the `curator` agent + evidence check.** ✅ Landed as
`agents/curator.yaml`. No `web_search`, no `web_fetch`, no `write_file`; the
evidence check is `check_evidence` in `src/core/tools/issue.cpp`. See the
`answer_schema` note in §2.2 for what changed.

**Phase 4 — `publish_issue` + run records.** ✅ Landed. Issue JSON is the source
of truth; the `.txt` and the HTML are generated from it — which is what will
give `post_tweet.py` a link nobody retyped once phase 6 points it at the JSON.
`newsletter-publisher` and `ai-newsletter` are still in `agents/`; retire them
after a clean week.

**Phase 5 — publication configs.** ✅ Landed as `publications/ai-pulse.yaml` plus
`publications/voice/ai-pulse.md`, read by `src/core/publication.cpp`. The
generalization is proved in `tests/test_publication.cpp`, which loads a
publication that exists only inside the test — different queries, different
caps, one artifact instead of two, and no email channel — rather than shipping a
speculative second YAML that nobody reads. That is the same proof for less
weight: if a config-only publication round-trips in a test, it round-trips.

Two things came out differently. `subject` is not a strftime template: it takes
`{date}` and `{iso_date}`, because a second date-formatting dialect that only
this file speaks is not worth the generality. And the voice note rides back on
the `harvest_candidates` result rather than being spliced into the agent's
prompt — the tool already knows which publication this is, so the agent doesn't
have to, and `curator` stays genuinely generic instead of generic-looking.

**Phase 6 — scheduling + `post_tweet.py` reading the issue JSON.** ✅ Landed.
`post_tweet.py` indexes `items[N-1]` out of the issue JSON — so LinkedIn gets
the link-verified URL, and the second hand-rolled parser of the `.txt` is gone —
and refuses with exit 3 when the day's run record is missing or its status isn't
`sent`. `run_publication.sh` is the cron entry: it POSTs to `/api/chat` with
`agent: curator` and then decides whether it worked by reading the run record
rather than by believing the agent. Both ends read the same record, so they
agree on what "today went out" means.

The crontab itself is not changed by this repo. The lines to change are in
`publishing/README.md`.

---

## 8. Decisions, as taken

1. **Native C++ tool or Python script for `harvest_candidates`?** Native gets
   the result store, the net guard, and the existing Tavily/extractor code for
   free; a script would duplicate all four. Recommend native.
   → **Native.** The Tavily call and the HTML extractor were lifted out of
   `web_search.cpp` and `web_fetch.cpp` into `tavily.*` and `page_text.*` and
   are now shared rather than copied. `web_fetch` keeps its 8 KB cap because its
   output goes into the transcript; the harvester keeps 64 KB because its
   doesn't.
2. **How strict should the evidence check be?** Exact substring after whitespace
   and case normalization is the tight end; token-overlap scoring is the loose
   end. Recommend starting exact and loosening only against observed false
   rejects — a false reject costs a nudge, a false accept costs a wrong link in
   a sent newsletter.
   → **Exact, plus three concessions that cost nothing.** Case and whitespace
   fold, typographic quotes and dashes map to ASCII (a model renormalizes those
   without meaning to, and rejecting it for that is rejecting it for being a
   language model), and one `…` may stand in for a gap — two would let it
   assemble a sentence the page never made. A quote under 24 characters is
   refused outright: "the company" is on every page.
3. **Does the curator get `web_search` at all?** Giving it one is a hedge for a
   thin pool; withholding it removes the last path by which an unvetted URL can
   enter. Recommend withholding, and handling a thin pool by widening the
   harvest window instead.
   → **Withheld,** and recorded as such in `agents/curator.yaml` so the next
   person to add it has to delete the reason first. A thin pool is handled by a
   second `harvest_candidates` call with a wider `time_range`, which
   `tool_limits` allows exactly one of.
4. **`min_count: 8`** — today dropping to 8 items was a human decision made
   mid-run. Encoding it lets a run self-heal; it also lets a quiet news day pass
   silently as a short issue. Probably right, but it should show up loudly in
   the run record.
   → **Encoded as `publish_issue`'s `min_items`, default 8.** Below it the tool
   refuses and writes a `status: blocked` run record naming the count, so a
   short day is loud rather than silent. `selected` is in every record, so a run
   of 8 is visible even when it succeeds. Moves to the publication config in
   phase 5.

### Found in the first live runs, then fixed

- **The pool over-represented one story.** The first live harvest returned six
  outlets on the same Anthropic disclosure out of eight candidates: a
  25-candidate pool holding four distinct stories is a thin menu. `title_key`
  couldn't catch it (the headlines genuinely differ) and `max_per_source` is the
  wrong axis (one story across many sources, not many stories from one).

  Fixed with title-similarity clustering in `shortlist`: significant tokens —
  stop words and 1–2 character words dropped, which also removes "AI" from every
  headline in an AI newsletter where it separates nothing, number words mapped to
  digits, crude stemming so "hacked"/"hacks"/"hacking" agree — compared by
  overlap coefficient rather than Jaccard, so a six-word headline and a
  twelve-word one about the same event still match. Threshold 0.5 with at least
  three shared tokens, deliberately reluctant: merging two real stories costs an
  item, while missing a pair only leaves the menu as thin as it was. Each
  candidate carries a `story` number the curator can see, and `max_per_story`
  (default 2, so there is still a choice of outlet) caps the rest.

  Measured on the same queries: 8 candidates covering ~4 stories became 12
  candidates covering 12.

- **A transient timeout blocked the whole issue.** The link re-check uses a 12s
  HEAD-then-GET; washingtonpost.com timed out on it during a smoke run and the
  issue was correctly refused, though the harvester had fetched the same page a
  minute earlier. Fixed by retrying once, and only on a transport failure — a
  timeout, a refused connection, a DNS blip, all statements about the network at
  this instant. An HTTP status is never retried: the server answered, and asking
  again will not change its mind.

### Found live-testing the curator agent against the real 9B model

Six end-to-end attempts against the local `Qwen3.5-9B-Q8_0.gguf` model, each
against real Tavily/web data, sending only to julio's own test subscription.
Each surfaced one distinct, fixable problem:

1. **`max_steps: 8` was smaller than the tool budgets it had to fit around**
   (harvest 2 + read_result 8 + publish_issue 3 = 13, plus the reserved answer
   step). The model burned its whole budget on one harvest and six
   `read_result` calls paginating a single marginal candidate, never reaching
   `publish_issue`. Raised to 16, then 20.
2. **The `queries` field was required by the JSON schema while the tool's own
   description said to omit it** to use the publication's configured queries —
   a contradiction the model could never resolve. Fixed in `harvest.cpp`:
   `resolve_queries()` treats an absent key and an empty array the same way,
   and `queries` was dropped from the schema's `required` array.
3. **`resolve_dir()` returned a bare relative path**, harmless for
   same-process reads but wrong once embedded in a subprocess argv spawned
   with a different working directory — `publish_issue.py` couldn't be found.
   Fixed by always returning an absolute path.
4. **The model resubmitted an identical over-length item after a rejection.**
   The loop detector correctly killed the run, but the prompt hadn't said what
   "fix it" means per error type. Added explicit per-error repair instructions
   and a warning that an unchanged resubmission burns one of only three tries.
5. **The model under-selected and front-loaded `read_result`.** Its first
   `publish_issue` attempt had 3 items against a floor of 8, and all four
   `read_result` calls were already spent by then. Added prompt lines to draft
   the full list from excerpts before calling `read_result` at all, and to
   always submit the full configured count on the first attempt.
6. **The model confused a `result_id` (from `read_result`) with a candidate's
   pool-local `id`.** `build_issue` correctly rejected it, but the generic
   "no candidate with that id" message didn't say why the number was wrong,
   costing an attempt. Fixed in `issue.cpp`'s `build_issue`: an id that
   exactly matches some candidate's `result_id` now gets a message naming the
   mix-up and the right pool id directly, rather than the generic unknown-id
   message.

### Still open

- **Bot-blocked hosts.** The 401/403/429 SUSPECT set is recorded and never
  blocks, as it always has. A per-publication allowlist of hosts known to bounce
  HEAD requests would let "suspect" shrink toward meaning something.
- **A scheduler inside Funes.** `run_publication.sh` is cron plus a run-record
  check. A real one wants a job table, retries and a UI, and is a bigger question
  than this document.
