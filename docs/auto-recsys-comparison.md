# Auto-RecSys (arXiv:2609.10922) vs. Funes

Evaluation of Meta's "Auto-RecSys" paper against Funes' agent harness, for a
future version. Written 2026-09-11.

## The two systems

Auto-RecSys: an autonomous-research harness for iterating on industry-scale
recommendation models, where a single training run takes days and depends on
distributed GPU infrastructure. Architecture: an orchestration layer drives a
finite state machine (ideating → implementing → validating → training →
analyzing, with a debug branch) and routes each stage to a specialist agent;
a centralized memory store — a directory of JSON/JSONL files on shared
storage — holds per-idea state, model "playbooks," and an append-only
experiment history; sessions are disposable and can resume on a different
server by replaying a trajectory log. Its self-evolving core is a dual loop:
the *Execution Evolution Loop* distills session trajectories into per-model
playbooks (dead ends, proven strategies, pipeline recipes, written as
markdown "DO NOT" directives); the *Idea Evolution Loop* keeps a ranked idea
backlog and an outcome log so ideation doesn't repeat already-tried
directions.

Funes: one C++17 binary, one SQLite file, no distributed servers, no
multi-day training runs — the coordination problem Auto-RecSys's whole
architecture exists to solve doesn't occur here. Agents are already
declarative YAML personas over a fixed native tool registry (`agents/*.yaml`
+ `src/core/tools/*.cpp`), which is the same "cognitive-procedural
separation" the paper names as one of its three harness designs. Memory is a
single `MemoryStore` recall pool per user (`src/core/memory.cpp`), ranked by
`source_weight` and periodically consolidated, but with no notion of
per-agent operational knowledge kept apart from conversational recall.

## Where Auto-RecSys's critique hits Funes

**No dead-end memory.** `tool_budget.cpp`, `agent.cpp`'s loop detection, and
`run_outcome.cpp` are all good at *stopping* a bad run, but nothing writes
down *why* it went bad so the next run of the same agent avoids repeating it.
Auto-RecSys's playbook is exactly this negative-knowledge store — an error
paired with its root cause and fix, phrased as an instruction the model can
follow directly — and Funes has no equivalent.

**Procedural and declarative memory are the same pool.** `Memory::source`
(`user`/`tool`/`auto`/`consolidated`) all land in one semantically-ranked
recall set. A fix for a recurring MCP-server timeout competes for the same
top-k slot as a user's stored birthday. The paper keeps these deliberately
separate — playbooks are read in full at session start, not retrieved by
similarity — for exactly this reason.

**No outcome ledger for repeated agent workflows.** The newsletter curator
(`publishing/`, the `curator` agent) runs on a schedule and could accumulate
"this kind of item under-performs" the way Auto-RecSys's
`experiment_history.jsonl` does. Today nothing records verdicts across runs;
each run starts blind to prior ones.

## Where Funes is right and the paper undersells it

**Distributed state is solving a problem Funes doesn't have.** The
registry.json, per-idea state files, and multi-server handoff protocol exist
because Auto-RecSys's sessions and servers are disposable mid-task. Funes'
one binary + one SQLite file already gives atomic writes and a single source
of truth for free; building a state-machine/registry layer on top would be
infrastructure for a coordination problem that doesn't exist at this scale.

**The Idea Evolution Loop assumes a portfolio of concurrent, comparable
experiments against a shared baseline.** Funes has no analogous
repeated-experimentation workflow outside the newsletter curator, so a
general ranked-backlog mechanism would be speculative infrastructure for a
use case that doesn't yet exist — worth revisiting only if a second workflow
like it shows up.

**Funes' permission model is already more auditable than the paper's
checkpoint toggle.** Auto-RecSys's interactive/autonomous switch gates on
agent-judged confidence; `permissions.cpp` gates by role and tool allowlist,
enforced in four separate places (see `CLAUDE.md`'s Permissions section).
Coarser, but inspectable without reading a trajectory log.

## Verdict

The one portable idea is the **playbook**: a per-agent, per-user
natural-language store of dead ends and proven strategies, read at agent
init and updated after runs that needed tool-error recovery. Everything else
in the paper — the state machine, the cross-server registry, the idea
backlog — solves Meta's coordination problem, not Funes'.

Portable win, scoped for a future version:

1. **Agent playbooks.** New `agent_playbooks` table (`user_id`, `agent_name`,
   `content`, `updated_at`), own class (`src/core/playbook.cpp`) alongside
   `MemoryStore`/`UserStore` — not folded into the vec-backed recall pool,
   which is the wrong shape for a small markdown blob read in full rather
   than retrieved by similarity. `FunesAgent::run` (`agent.cpp`) injects an
   agent's playbook into its system prompt when non-empty and under a size
   cap; `run_outcome.cpp` appends a dead-end entry (error pattern + fix) when
   a run required recovery from a tool error. Opt-in per agent via a new
   `playbook: true` YAML key — pilot on whichever shipped agent hits
   MCP/shell errors most often (the WhatsApp autoresponder is the likely
   candidate) before enabling it generally. Extend
   `tests/test_user_isolation.cpp` to cover the new table: no cross-user
   read/write, same as every other per-user store.

Everything else — proxy-model screening, cross-model knowledge transfer,
validation-gated playbook updates — is Auto-RecSys tuning its own idea loop
at Meta's scale, not something Funes has earned the need for yet.
