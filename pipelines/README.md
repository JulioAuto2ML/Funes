# pipelines/

One YAML per multi-stage pipeline. A pipeline is a sequence of stages that hand
work to each other as files: where a stage's entries go, what they are called,
and what shape they have.

This is the same move `publications/` made for the newsletter, applied to the
other place the codebase had it wrong. The rule both come from:

> **Don't ask the model for what a tool can supply.** The model decides which
> topic is worth debating and what the article says. Paths, dates, filenames
> and field shapes have a checkable right answer, so they belong in code.

## Why this exists

The voice-of-customer chain (`voc-researcher` → `council-chair` →
`council-panelist` ×3 → `content-writer` → `mvp-builder`) used to carry its
file conventions as a paragraph in each of five system prompts:

```
6. `write_file` each topic as a JSON file to `topics/<date>-<slug>.json` …
```

Which made the convention *advice*. A slug that drifted by one character, a
required field the model dropped, an object where an array belonged — each
produces a file that exists, so the stage reports success and the next stage
finds nothing where it looked. The failure surfaces at the end of a five-step
pipeline, as an empty article.

Now the agent names a pipeline, a stage and a slug; `write_structured` derives
the path and validates the content, and a violation is refused with the field
named and **nothing written**. Half the convention stopped being the model's
job, and the half that is still its job fails loudly.

## Format

```yaml
id: voc
description: One line, shown to the model when it lists the pipeline
workspace: data/voc-pipeline      # relative to the caller's workspace

stages:
  - name: topic                   # what a tool call addresses it by
    description: One line
    dir: topics                   # relative to the pipeline workspace
    filename: "{date}-{slug}.json" # {date} and {slug} are substituted
    schema:                        # json stages only
      type: object
      required: [topic, status]
      properties:
        topic:  {type: string}
        status: {type: string, enum: [pending, debated, dropped]}

  - name: article
    dir: articles
    filename: "{slug}/draft.md"
    format: text                   # no schema: prose has no shape to check
```

Rejected at load, not at run time, because the mistake is in the file and the
error should say so:

- no `stages:`, a stage with no `name`, or two stages with the same name
- a `dir` or `workspace` that climbs out with `..` or starts at `/`
- a `filename` with neither `{date}` nor `{slug}` — every entry would
  overwrite the last one
- a `schema:` on a `format: text` stage

`schema:` is the same subset of JSON Schema the agent runtime already uses for
`answer_schema:` (type, required, properties, items, enum, minItems/maxItems) —
see `src/core/answer_schema.h`. One validator, so a stage contract and an
answer contract fail the same way and read the same way to the small local
models that have to act on the error.

## Tools

- `write_structured(pipeline, stage, slug, content, date?)` — validates, then
  writes. The slug is sanitized (`"Local AI Compliance!"` and
  `local-ai-compliance` are the same entry), the date defaults to today, and
  the path is derived. Returns the path it wrote so the model can quote it
  without having had to construct it.
- `read_structured(pipeline, stage, slug?)` — the entry, or with no slug a
  listing of the stage *and* a restatement of its required shape. Reading an
  empty stage is not an error: "has this been done already?" is a normal first
  question.
- `merge_rankings(rankings)` — Borda count over several ranked lists, with
  fuzzy title matching and a report of any list it could not read. Lives here
  in spirit: `council-chair`'s prompt used to ask a 7B model to do this
  arithmetic in-context.

## Adding a pipeline

Drop a YAML in here. The config is read per call, so a new pipeline or a new
stage is live with no restart and no rebuild — only the sentence in the tool
description listing configured pipelines is a startup snapshot.

Then point an agent at it: give it `write_structured` / `read_structured` in
its `tools:` and name the pipeline and stage in its prompt. A new pipeline
needs no new agent, and a new stage needs no new tool.

Where the files actually land: `<FUNES_WORKSPACE_DIR>/<user_id>/<workspace>/`,
resolved through `fs_guard::workspace_for` like every other workspace path —
so a pipeline's entries are per-account by construction, and an agent's own
`workspace_dir` does not have to match the pipeline's for the stages to agree
on where things are.
