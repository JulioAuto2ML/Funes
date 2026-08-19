# Dev plan: VoC → debate → content → publish pipeline

> Adapted from `voc-to-mvp-pipeline-plan.md`, which designed the same
> pipeline around Hermes cron + The Council + content-pipeline — three
> separate Python projects on yoda, each with its own database and agent
> loop. This plan rebuilds it natively on Funes: its cron runner, its
> delegation model, its publication system, and its existing tools. Nothing
> shells out to Council or content-pipeline; nothing adds a second agent
> framework.
>
> Design principle carried from the publications plan:
> **deterministic over agentic** — only delegate steps that truly need
> model judgment, everything else is code.

---

## What the pipeline does

One sentence: **listen to what people complain about → debate which
problem is worth addressing → write about it → review → publish.**

Five stages, three automated and two human-driven:

```
  VoC research       →   Council debate   →   Content writer   →   Review (manual)   →   Publish (script)   →   MVP builder
  (Mon/Wed/Fri 9am)      (same run)           (daily 11am)        (you + Opus)           (cron script)           (Sat 10am)
  ─────────────────       ──────────────       ──────────────       ──────────────         ──────────────          ──────────
  web_search topics       3 perspectives       outline → draft     read draft              read final.md          read approved
  extract pain points     rank proposals       from debate          grade + edit            condense to post       scaffold
  distill topics          write decision        winners              write final.md         post to LinkedIn       prototype
  seed debate             file to workspace     write to workspace   approve/reject         write record           test it runs
  remember findings                                                                                               report
```

The review and publish steps are deliberately not agents. Review is
human judgment — you read the draft with Opus and decide whether it's
good, what to fix, and whether to publish. Publishing is deterministic —
a script reads the approved final, condenses it, and posts it. Neither
step needs model autonomy.

---

## Decisions (all resolved)

| # | Decision | Resolution |
|---|---|---|
| 1 | Human gate for MVP building | **Yes** — mvp-builder only acts on `status: "approved"` debates |
| 2 | Delegation depth limit | **Raised to 3** — gives one hop of headroom beyond the VoC → chair → panelist chain (implemented: `delegation.cpp`) |
| 3 | Env-var expansion in agent configs | **Implemented** — `${VAR}` expansion in `llm_url`, `llm_api_key`, `llm_provider` fields (implemented: `agent_config.cpp`, tested) |
| 4 | Review model / review approach | **Manual** — review is you + Opus, not an automated agent. Publication is a deterministic script, not an agent. |
| 5 | LinkedIn post format | **Condensed posts** — the full article stays in the workspace; LinkedIn gets a hook + insight + CTA under ~2900 chars. Track engagement to decide if linking out is worth adding later. |

---

## What already exists in Funes that this reuses

| Funes primitive | What it covers here |
|---|---|
| `web_search` / `web_fetch` | VoC research — search X/Reddit/HN/forums, read pages |
| `remember` / `recall` | Persist VoC findings, debate decisions, topic backlog across sessions |
| `delegate_to_agent` | Council debate (orchestrator delegates to 3 perspective agents) |
| `read_file` / `write_file` | All structured state: topic queues, debate records, article drafts |
| `schedule_job` (cron runner) | Recurring agent jobs (VoC, writer, MVP builder) |
| `publishing/post_tweet.py` | LinkedIn posting (UGC API, already tested) |
| Per-agent `llm_url` / `llm_api_key` / `llm_provider` | Now supports `${VAR}` expansion for secrets (implemented this session) |

---

## What needs building

### New agents (4)

| Agent | Tools | Purpose |
|---|---|---|
| `voc-researcher` | web_search, web_fetch, read_result, remember, recall, write_file, delegate_to_agent | Search, extract pain points, distill topics, kick off debate |
| `council-chair` | read_file, write_file, delegate_to_agent, recall | Run a structured debate: delegate to 3 panelists, collect rankings, write decision |
| `council-panelist` | read_file, recall | One perspective on a topic; returns a ranked proposal list |
| `content-writer` | read_file, write_file, web_search, web_fetch, read_result, recall | Write a long-form article from a debate-winning topic |

### New script (1)

`scripts/publish_voc_post.py` — reads an approved `final.md`, condenses
it to a LinkedIn post, calls `post_to_linkedin()` from
`publishing/post_tweet.py`. Pure Python, no LLM, no agent. Runs as a
`kind: shell` cron job.

### New voice file (1)

`publications/voice/voc-insights.md` — who reads the VoC content, what
angle, what tone. Read by the content-writer agent.

### New workspace directory structure

All pipeline state lives under `data/voc-pipeline/` in the Funes
workspace, as flat JSON and markdown files. No second database.

```
data/voc-pipeline/
├── topics/                      # topic queue
│   ├── 2026-08-20-local-llm-compliance.json
│   └── 2026-08-22-gpu-inference-cost.json
├── debates/                     # council debate records
│   ├── 2026-08-20-local-llm-compliance.json
│   └── ...
├── articles/                    # content drafts and finals
│   ├── 2026-08-21-local-llm-compliance/
│   │   ├── draft.md
│   │   └── final.md             # written by you during review
│   └── ...
├── published/                   # post-publish records
│   └── 2026-08-21-local-llm-compliance.json
└── mvp/                         # MVP prototypes
    └── local-compliance-checker/
        ├── main.py
        └── build-log.json
```

---

## Agent designs

### 1. `voc-researcher`

The entry point. Three cron jobs fire it on Mon/Wed/Fri, each with a
different topic focus baked into the task string.

**What it does in one run:**

1. `recall` — check what it found last time on this topic (memories
   persist across runs via `memory_scope`).
2. `web_search` 3–4 queries targeting the topic's communities
   (r/LocalLLaMA, r/selfhosted, HN, X, industry forums). Time range:
   `week` to catch what's new since last run.
3. `web_fetch` the most promising 4–6 results. Extract pain language:
   what people complain about, what they wish existed, what workarounds
   they describe.
4. `remember` the top findings (pain points + sources) so the next run
   starts from a higher baseline.
5. Distill 2–3 short topic phrases suitable as debate/article topics.
6. `write_file` each topic as a JSON file under `data/voc-pipeline/topics/`:
   ```json
   {
     "topic": "local AI compliance for legal teams",
     "source_job": "regulated-industry",
     "date": "2026-08-20",
     "pain_points": ["can't send data to OpenAI", "HIPAA audit trail"],
     "evidence_urls": ["https://reddit.com/r/privacy/..."],
     "status": "pending"
   }
   ```
7. `delegate_to_agent(council-chair, ...)` with the strongest topic,
   passing the full topic JSON as the task. The other topics stay queued
   for future debates.
8. Final answer: one paragraph — what it found, which topic went to
   debate, how many topics were queued.

```yaml
name: voc-researcher
description: >
  Searches the web for voice-of-customer pain points on a given topic,
  distills debate-ready topics, and kicks off a council debate on the
  strongest one.
model: default
tools: [web_search, web_fetch, read_result, remember, recall,
        write_file, delegate_to_agent]
max_steps: 16
context_limit: 8192
tool_limits:
  web_search: 4
  web_fetch: 6
  delegate_to_agent: 1
workspace_dir: data/voc-pipeline
memory_scope: voc-researcher
delegation_notes: >
  Runs web research and a council debate internally. Give it a
  self-contained task naming the topic focus and target communities.
  Don't search for the same topic yourself after delegating.
```

**Why `delegate_to_agent` instead of separate cron jobs:** The debate
needs the VoC findings as input. Making the researcher delegate to the
council-chair in the same run means the topic + evidence travels in the
task string, not through a file that a second cron job has to find and
parse. The council-chair's answer comes back as a tool result, so the
researcher can report the outcome.

Delegation depth: voc-researcher → council-chair → council-panelist.
That's depth 3 — within `kMaxDelegationDepth` (raised from 2 to 3 this
session).

### 2. `council-chair`

Runs a structured debate. Not a free-form conversation — a defined
protocol with deterministic bookkeeping.

**What it does:**

1. `read_file` any prior debates on this topic (checks
   `data/voc-pipeline/debates/`) to avoid re-debating.
2. Delegate to `council-panelist` three times, each with a different
   perspective instruction baked into the task:
   - **Builder perspective:** "You are a technical founder building
     products for this market. What solutions would you build? Rank by
     feasibility and market fit."
   - **Buyer perspective:** "You are a decision-maker in this space
     evaluating solutions. What would you pay for? Rank by urgency and
     willingness to pay."
   - **Critic perspective:** "You are a skeptic looking for flaws. What
     could go wrong? Which proposals are hype vs. substance? Rank by
     risk."
3. Collect the three ranked lists. Merge them: a proposal that ranks #1
   for all three is the clear winner; conflicting rankings get a simple
   Borda count (sum of inverse ranks).
4. `write_file` the debate record to `data/voc-pipeline/debates/`:
   ```json
   {
     "topic": "local AI compliance for legal teams",
     "date": "2026-08-20",
     "panelists": {
       "builder": {"proposals": [...]},
       "buyer":   {"proposals": [...]},
       "critic":  {"proposals": [...]}
     },
     "merged_ranking": [
       {"proposal": "On-prem audit trail generator", "score": 8},
       {"proposal": "Compliance pre-check API", "score": 6}
     ],
     "winner": "On-prem audit trail generator",
     "status": "decided"
   }
   ```
5. Return the winner and the merged ranking to the caller.

```yaml
name: council-chair
description: >
  Runs a structured 3-perspective debate on a topic and writes the
  decision. Always called via delegation, never directly by the user.
model: default
tools: [read_file, write_file, delegate_to_agent, recall]
max_steps: 12
context_limit: 8192
tool_limits:
  delegate_to_agent: 3
workspace_dir: data/voc-pipeline
```

**Why three agents instead of one agent with three prompts:** Isolation.
Each panelist sees only the topic and its own perspective instructions.
It can't read the other panelists' answers, so it can't anchor on them.
The chair merges independently generated rankings — which is the whole
point of a council pattern.

### 3. `council-panelist`

A stateless evaluator. One YAML, three instantiations (the perspective
comes from the task string, not the config).

```yaml
name: council-panelist
description: >
  Evaluates a topic from a given perspective and returns ranked proposals.
  Stateless — each run sees only the topic and the perspective it was given.
model: default
tools: [read_file, recall]
max_steps: 4
context_limit: 8192
workspace_dir: data/voc-pipeline
system_prompt: |
  You receive a topic and a perspective (builder, buyer, or critic).
  Generate 3–5 concrete proposals that address the topic, then rank them
  from strongest to weakest according to your perspective's criteria.

  Return ONLY a JSON object:
  {
    "perspective": "<builder|buyer|critic>",
    "proposals": [
      {"rank": 1, "title": "...", "rationale": "one sentence"}
    ]
  }

  No preamble, no commentary outside the JSON.
answer_schema:
  type: object
  required: [perspective, proposals]
  properties:
    perspective:
      type: string
    proposals:
      type: array
      items:
        type: object
        required: [rank, title, rationale]
        properties:
          rank: {type: integer}
          title: {type: string}
          rationale: {type: string}
```

The `answer_schema` enforces structure so the chair can parse the result
deterministically — same validation engine as `publish_issue`'s argument
schema (`answer_schema.h`).

### 4. `content-writer`

Takes a debate winner and turns it into a long-form article.

**What it does:**

1. `read_file` the debate record to understand the full context: the
   topic, the pain points, the winning proposal, the critiques.
2. `recall` any prior VoC findings on this topic.
3. `web_search` + `web_fetch` for supporting evidence, examples, data
   points — to ground the article in real sources, not generate from
   nothing.
4. Write an outline, then a full draft. `write_file` the draft to
   `data/voc-pipeline/articles/<slug>/draft.md`.
5. Final answer: confirms where the draft was written.

```yaml
name: content-writer
description: >
  Writes a long-form article from a debate-winning topic, grounded in
  VoC evidence and web research. Writes to the pipeline workspace.
model: default
tools: [read_file, write_file, web_search, web_fetch, read_result, recall]
max_steps: 16
context_limit: 16384
tool_limits:
  web_search: 4
  web_fetch: 6
workspace_dir: data/voc-pipeline
delegation_notes: >
  Give it a self-contained task: the debate record path and any
  specific angle or length requirements. It writes the draft to the
  articles/ directory.
```

**Cron trigger:** A daily job (11am) whose task string is:

```
Read data/voc-pipeline/debates/ for the most recent debate with
status "decided" that has no matching draft in data/voc-pipeline/articles/.
Write the article draft. If no undrafted debate exists, say so and stop.
```

### 5. `mvp-builder` (stretch)

Takes approved, high-ranking debate winners and scaffolds a prototype.
Only runs on debates with `status: "approved"` — the human gate.

```yaml
name: mvp-builder
description: >
  Scaffolds runnable prototypes from approved council debate winners.
  Writes Python scripts and tests them.
model: default
tools: [read_file, write_file, execute_shell, web_search, web_fetch,
        read_result, recall]
max_steps: 20
context_limit: 16384
workspace_dir: data/voc-pipeline
tool_limits:
  web_search: 3
  web_fetch: 4
  execute_shell: 6
delegation_notes: >
  Give it a debate record path or a topic. It scaffolds and tests a
  prototype. The output lives in data/voc-pipeline/mvp/<slug>/.
```

---

## Review and publish — the non-agent stages

### Review (manual, you + Opus)

No agent, no cron job. You read drafts in `data/voc-pipeline/articles/`
when you want to, using Opus (this tool) or by reading the files
directly. The review workflow:

1. Read `articles/<slug>/draft.md`.
2. Edit, rewrite, or reject. If approved, write `articles/<slug>/final.md`
   with whatever edits you made.
3. The existence of `final.md` is the approval signal. No `review.json`,
   no status field — if the file exists, it's approved.

This keeps the quality bar exactly where you want it. An automated
reviewer rubber-stamps at the capability level of whatever model runs
it; you don't.

### Publish (deterministic script)

`scripts/publish_voc_post.py` — a thin Python script, no LLM:

1. Scans `data/voc-pipeline/articles/` for directories containing
   `final.md` but no matching record in `data/voc-pipeline/published/`.
2. For each: reads `final.md`, extracts the title and key takeaway
   (first heading + first paragraph, or a structured frontmatter block
   if we add one), formats a LinkedIn post under ~2900 chars.
3. Calls `post_to_linkedin()` from `publishing/post_tweet.py`.
4. Writes `published/<slug>.json` with timestamp, platform, post text.
5. Exits 0 on success, 3 on "nothing to publish" (same contract as
   `post_tweet.py`).

**Open question on condensing:** If the script extracts title + first
paragraph mechanically, the condensing is deterministic but might produce
poor posts. Alternative: require `final.md` to include a
`<!-- linkedin: ... -->` block that you write during review — the post
text is then authored by you, not extracted. This is more work per
article but guarantees quality. **Recommendation: start with the
`<!-- linkedin: ... -->` block.** You're already reviewing; adding 2–3
sentences of post text is marginal effort for much better output.

**Cron:** `kind: shell`, daily at 3pm:
```
python3 /path/to/scripts/publish_voc_post.py --dir data/voc-pipeline
```

---

## Cron schedule

| Job name | Schedule | Kind | Agent/Command | Task summary |
|---|---|---|---|---|
| VoC: Small Business Local AI | `0 9 * * 1` (Mon 9am) | agent | voc-researcher | Search for small-business local AI pain points on X, Reddit, forums |
| VoC: Local LLM Builders | `0 9 * * 3` (Wed 9am) | agent | voc-researcher | Search for local LLM builder pain points on r/LocalLLaMA, r/selfhosted, HN, X |
| VoC: Regulated Industry AI | `0 9 * * 5` (Fri 9am) | agent | voc-researcher | Search for regulated-industry local AI adoption on r/privacy, r/msp, LinkedIn |
| Content Writer | `0 11 * * *` (daily 11am) | agent | content-writer | Read debates/ for undrafted winners, write the article |
| VoC Publish | `0 15 * * *` (daily 3pm) | shell | `publish_voc_post.py` | Post approved articles to LinkedIn |
| MVP Builder | `0 10 * * 6` (Sat 10am) | agent | mvp-builder | Read approved debates with no MVP, build one |

No review job — review is on your schedule, not cron's.

---

## Data flow

```
                     [topics/*.json]
                          │
    voc-researcher ───────┤ writes topic files
         │                │
         │ delegates       │
         ▼                │
    council-chair ────────┤ reads topic, writes debate
         │                │
         │ delegates ×3   [debates/*.json]
         ▼                │         status: "decided"
    council-panelist      │
    (builder/buyer/       │
     critic)              │
                          │
    content-writer ───────┤ reads debate, writes draft
                          │
                     [articles/slug/draft.md]
                          │
    YOU + Opus ───────────┤ read draft, edit, write final
                          │
                     [articles/slug/final.md]    ← approval signal
                          │
    publish_voc_post.py ──┤ read final, post to LinkedIn
                          │
                     [published/slug.json]
                          │
    YOU ──────────────────┤ change debate status to "approved"
                          │
    mvp-builder ──────────┘ reads approved debates, builds prototype
                          │
                     [mvp/slug/main.py]
```

All state is files. No second database. You can inspect, edit, or
override any stage by reading/writing files in `data/voc-pipeline/`.

---

## What the original plan had that this drops, and why

| Original component | Disposition | Reason |
|---|---|---|
| Hermes cron (`jobs.json`) | **Replaced** by Funes cron | Funes has its own scheduler |
| The Council (`council.py`, `council.db`) | **Rebuilt** as council-chair + council-panelist | Council's debate pattern maps directly to Funes delegation |
| content-pipeline (`ContentDatabase`, coordinator) | **Rebuilt** as content-writer agent | The 5-stage pipeline is what the writer does in one run |
| `content.db` schema changes | **Dropped** | No `content.db` — state is in files |
| `content-reviewer` agent (Claude Sonnet) | **Dropped** | Review is manual (you + Opus) |
| `content-publisher` agent | **Dropped** | Publishing is a deterministic script |
| `mvp_ledger.json` | **Dropped** | The ledger is the `mvp/` directory listing |
| Telegram delivery | **Dropped** | `list_jobs` shows what happened; `recall` returns findings |
| `review_and_publish.sh` orchestrator | **Dropped** | Review is manual; publish script runs on its own |

---

## Already implemented (this session)

| Change | File | What it does |
|---|---|---|
| Env-var expansion | `src/core/agent_config.cpp` | `${VAR}` substitution in `llm_url`, `llm_api_key`, `llm_provider` from process env; unset vars expand to empty string |
| Delegation depth bump | `src/core/tools/delegation.cpp` | `kMaxDelegationDepth` raised from 2 to 3 |
| Tests | `tests/test_agent_config.cpp` | Env-var expansion: set/unset vars, mixed text, unclosed `${` |

---

## Build order

Ordered so each phase is testable independently.

### Phase 0 — Prerequisites (partially done)

- [x] Env-var expansion in `agent_config.cpp`
- [x] Delegation depth raised to 3
- [ ] Create `data/voc-pipeline/` directory structure on yoda
- [ ] Voice file `publications/voice/voc-insights.md`

### Phase 1 — VoC researcher (standalone)

Ship `agents/voc-researcher.yaml`. Test manually via delegation.
Verify it writes topic files and remembers findings. No cron yet.

### Phase 2 — Council debate

Ship `agents/council-chair.yaml` and `agents/council-panelist.yaml`.
Test with a hardcoded topic. Verify the full 3-level delegation chain
works (depth 3). Then wire voc-researcher's delegation to council-chair
and test end-to-end.

### Phase 3 — Schedule the VoC jobs

Add three cron jobs via the operator. Run each once with `run_job_now`.

### Phase 4 — Content writer

Ship `agents/content-writer.yaml`. Test manually against an existing
debate record. Add the daily 11am cron job.

### Phase 5 — Publish script

Write `scripts/publish_voc_post.py`. Test with `--dry-run` against a
hand-written `final.md`. Add the daily 3pm shell cron job.

### Phase 6 — MVP builder (stretch)

Ship `agents/mvp-builder.yaml`. Test manually on an approved debate.
Add the Saturday 10am cron job.

---

## Risks

| Risk | Mitigation |
|---|---|
| 9B model can't produce structured JSON reliably for council-panelist | `answer_schema` enforces shape; if content quality is too low, run the council on Claude Haiku (cheap, much better at structured output) |
| Content writer produces drafts that are always rejected | Expected to some degree — that's why review is manual. If rejection rate is too high, run the writer on a larger model or add a self-critique loop |
| Delegation depth 3 still not enough | The chain is exactly 3 deep. If any agent needs a sub-delegation, flatten: have voc-researcher call panelists directly instead of through the chair |
| LinkedIn API tokens expire | Same risk as the newsletter pipeline; `post_tweet.py` handles this |
| Condensed posts don't track engagement | Start measuring: post time, impressions, clicks. If engagement is low after 4 weeks, revisit the format |
