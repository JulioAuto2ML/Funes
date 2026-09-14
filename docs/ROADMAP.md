# Roadmap

Three improvement plans exist. This file says which order they run in and why.
It does not restate them — each links to its own document.

Current release: **4.0** (multi-user, `project(Funes VERSION 4.0.0)`).

| Release | Theme | Source plan | Status |
|---------|-------|-------------|--------|
| **4.1** | Generalization: config over prose, tools over model arithmetic | [generalization-plan.md](generalization-plan.md) phases 1, 4, 5 | in progress |
| **4.2** | Generic channel adapter + self-service pointed at MCP | [generalization-plan.md](generalization-plan.md) phases 2, 3 | deferred (demand-driven) |
| **5.0** | Connected memories + localization | v5 plan (8 phases, ~13–17 days) | planned |
| **6.0** | Voice: STT + TTS sidecars | v6 voice research | researched |

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

Grounding checks for `content-writer` (the `evidence`-quote pattern from
`publish_issue`, applied to article claims) are listed in the plan under
phase 1 but land in 4.2: they need a fetch-cache the writer agent does not
have yet, and the pipeline primitive is useful without them.
