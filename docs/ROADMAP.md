# Roadmap

Three improvement plans exist. This file says which order they run in and why.
It does not restate them — each links to its own document.

Current release: **5.0** (connected memories + localization, `project(Funes VERSION 5.0.0)`).

| Release | Theme | Source plan | Status |
|---------|-------|-------------|--------|
| **4.1** | Generalization: config over prose, tools over model arithmetic | [generalization-plan.md](generalization-plan.md) phases 1, 4, 5 | shipped |
| **4.2** | Per-user publications; generic channel adapter; self-service pointed at MCP | [generalization-plan.md](generalization-plan.md) phases 2, 3 + below | planned |
| **5.0** | Connected memories + localization | v5 plan (8 phases) | done |
| **6.0** | Voice: STT + TTS sidecars | v6 voice research | next |

## Why this order

**4.1 before 5.0.** The generalization work is the only plan that pays down
debt already in the tree rather than adding surface. It touches no schema, no
external service and no new dependency: a config loader, two tools, one
arithmetic tool, and prompt edits. 5.0 adds two schema migrations plus a
271-memory LLM backfill; doing that on top of five agents that still encode
their file conventions in prose means migrating the prose too.

**Phases 1, 4, 5 in 4.1; phases 2 and 3 held for 4.2.** Phase 1 (the pipeline
primitive) is a direct repeat of an already-proven fix — the newsletter
rewrite — against a pipeline that was built after it without absorbing it.
Phase 4 is prompt edits only. Phase 5 is a documentation change plus defaults.
Phases 2 (channel adapter) and 3 (`mcp-builder`) are infrastructure for
requests that have not arrived: a second chat channel and a third tool author.
Building either now is speculative generality, which is the same mistake the
plan is about, pointed the other way.

**5.0 before 6.0.** Voice is additive and independent, but it needs CPU
headroom on yoda that the memory backfill also wants, and connected memories
change what a voice session recalls. Shipping recall changes first means the
voice work is tested against the recall behaviour it will live with.

## 4.1 breakdown

| Step | Deliverable | State |
|------|-------------|-------|
| 1.1 | `pipelines/*.yaml` config + loader (`src/core/pipeline.{h,cpp}`) | done |
| 1.2 | `write_structured` / `read_structured` tools, schema-enforced | done |
| 1.3 | `merge_rankings` tool (Borda count in code, not in-context) | done |
| 1.4 | Retrofit the five VoC-pipeline agents onto 1.1–1.3 | done |
| 4.1 | `book-editor`: style ruleset out of the prompt into a style file | done |
| 4.2 | `astro-ph-summarizer`: fold into `rss-reader` as a configured feed | done |
| 5.1 | Archetype defaults documented in `agents/README.md` | done |

## 4.2 — per-user publications

**Why now.** The deferred items in 4.2 were held back for want of real demand.
This one acquired it on 2026-09-14: a second account exists (`daniela`), so
"any user can publish their own newsletter" stopped being hypothetical. The
other two (channel adapter, `mcp-builder`) still have no second consumer and
stay deferred.

**The goal.** `curator` loses its absolute `workspace_dir`
(`/home/julio/Documents/X_posts`) and resolves per-user like every other
agent, so publishing is something an account does rather than something the
installation does.

**What must not happen on the way.** Moving that directory wholesale into the
user workspace is the obvious version of this change and it is wrong twice:

1. **It would publish the credentials.** The directory holds `.env` (Gmail +
   LinkedIn) and `subscribers.txt`. Today they sit outside every workspace, and
   `curator` has no `read_file` — nothing the model drives can reach them.
   Inside `<workspace>/<user_id>/`, they land in the confinement root of every
   agent that account runs, and `funes`, `operator`, `file-reviewer` and
   `book-editor` all have `read_file`. "Read x_posts/.env" would print live
   credentials into a chat transcript, and `/api/upload` and the files pane
   would reach them too.
2. **It would split the publication.** `publications/ai-pulse.yaml` is
   installation-global, and `dedup_against_last_issues` reads the *caller's*
   `issues/` directory. Two accounts could each publish `ai-pulse` on the same
   morning, with independent dedup histories, to one subscriber list. The
   absolute path is currently what prevents that; removing it without settling
   ownership first replaces isolation with a split brain that a subscriber
   discovers, not a test.

**Order of work.**

| Step | Deliverable |
|------|-------------|
| 1 | Decide ownership: a publication belongs to a user (an `owner` in the YAML, or per-user publication configs) rather than to the install. Everything else follows from this. |
| 2 | Split the publish directory by what each thing *is*: working files (harvest pools, issue JSON, run records, rendered artifacts) are per-user and model-visible; `.env` and `subscribers.txt` are publication-owned and resolved by the **tool**, never by a path the model can name — the same rule `publications/*.yaml` and `funes.local` already follow. |
| 3 | Per-user sending identity. A second publisher either brings their own SMTP/LinkedIn credentials or sends from the owner's — a decision, not a default. |
| 4 | Drop `workspace_dir` from `curator.yaml`. Smallest step, and last. |
| 5 | Update `run_publication.sh` and the LinkedIn cron, which read `runs/<pub>/<date>.json` at a fixed path and will need to know whose. A live cron that publishes real things. |

**A category this exposed, worth writing down.** Funes isolates its own data —
memories, turns, stored results, workspace files — in SQL and in `fs_guard`.
It cannot isolate a *third-party account* reached through an MCP server with
installation-wide credentials. `gmail-assistant` (`IMAP_USER` in
`funes.local`, one mailbox) and `whatsapp-assistant` (one bridge store, one
phone number) are both in that class: granting either to a second account
hands over the first account's inbox or chats, however well-isolated Funes's
own storage is. Per-user credentials for those is its own piece of work, not
part of this one. Until then they stay off a member's agent allowlist —
`daniela`'s is `funes, researcher, operator, rss-reader, file-reviewer,
book-editor`.

Note `--agents` is an allowlist with no deny form, so that list freezes at
today's roster: a newly added agent will not reach a restricted account until
somebody grants it. That is the safer default for a member account, but it is
a maintenance cost, not an accident.

## 5.0 breakdown

| Step | Deliverable | State |
|------|-------------|-------|
| 1 | `memory_links` + `memory_link_judgments` + `memories_fts` in `migrate()` | done |
| 2 | `link_memories` / `unlink_memories` / `links_of` / `count_links` | done |
| 3 | Recall widening: one-hop expansion + term-match fill, append-only | done |
| 4 | `backfill_links()` + the background pass that feeds it | done |
| 5 | UI string tables (`ui/i18n/*.json`), `t(key)`, `data-i18n` | done |
| 6 | `users.locale` + `PUT /api/me/locale`, detected from `navigator.language` | done |
| 7 | Agent reply locale — a runtime instruction in `agent.cpp`, not per-YAML | done |
| 8 | `memories.lang`, detected at `remember()` time (`lang_detect.h`) | done |

Cross-lingual recall needs no code: a multilingual embedding model already
matches a Spanish query to an English memory, and `memories.lang` is
descriptive — nothing filters on it, so a misdetection costs a wrong label
rather than a lost memory.

Phases 1–4 are additive and safe by construction: with no links and no term
matches, recall returns exactly what 4.x returned. The link backfill is the
only slow operation in 5.0 — one model call per candidate pair, capped and
resumable, off by default.

Grounding checks for `content-writer` (the `evidence`-quote pattern from
`publish_issue`, applied to article claims) are listed in the plan under
phase 1 but land in 4.2: they need a fetch-cache the writer agent does not
have yet, and the pipeline primitive is useful without them.
