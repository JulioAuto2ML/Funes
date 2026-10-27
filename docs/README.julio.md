# docs/

| File | Purpose |
|---|---|
| [ARTICLE.md](ARTICLE.md) | "The Harness Pattern" — published article on agent harness engineering, written about this installation |
| `funes-article.pdf` | PDF render of the article |
| [generalization-plan.md](generalization-plan.md) | Design analysis of the agents and pipelines in this repository: where the same problem got solved twice, once generally and once not |

All three lived in the core's `docs/` until 5.1. They moved here because they
describe *this* installation and *these* pipelines, not the harness: the
article names the machine it runs on and carries a byline, and the
generalization plan is an analysis of the eleven agents that are now here.
Most of what the plan proposed shipped in 4.1 — the pipeline primitive,
`write_structured`/`read_structured`, `merge_rankings` — and moved here with
them.

For the harness itself, see the core's [README](../../Funes/README.md) and
`CLAUDE.md`.
