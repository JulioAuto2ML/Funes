# docs/

| File | Purpose |
|---|---|
| [ROADMAP.md](ROADMAP.md) | What is being built next, and in what order |
| [julio-README.md](julio-README.md) | The personal half: newsletter, VoC pipeline, WhatsApp/Gmail/TV agents, how they are deployed, and the "don't ask the model for what a tool can supply" incident write-up |
| [ARTICLE.md](ARTICLE.md) | "The Harness Pattern" — published article on agent harness engineering, written about this installation |
| `funes-article.pdf` | PDF render of the article |
| [generalization-plan.md](generalization-plan.md) | Design analysis of the agents and pipelines: where the same problem got solved twice, once generally and once not. Most of it shipped in 4.1 |
| [os-plan.md](os-plan.md) | Funes in the OS: grants as kernel policy, the machine as a memory owner, effects as proposals. Phase 0 shipped in 7.0 |
| [memory-plan.md](memory-plan.md) | Memory architecture: what the production pool holds, six defects, and a phased plan (eval set, bounded writes, taint, fact extraction, supersession) |
| [tv-ideas.md](tv-ideas.md) | Backlog of ideas for the living-room TV |
| `screenshot.png` | UI screenshot, referenced by the root README |

Setup, configuration, and usage are covered in the root
[README.md](../README.md). Architecture and internals live in
[CLAUDE.md](../CLAUDE.md) and the per-directory READMEs (`src/core/`,
`src/server/`, `agents/`, etc.).
