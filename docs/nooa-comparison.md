# NOOA (arXiv 2607.20709) vs. Funes

Evaluation of NVIDIA's "Object-Oriented Agents" paper against Funes' YAML agent
definitions and C++ native tool registry. Written 2026-07-30.

## The two architectures

NOOA: an agent is a Python class. Methods with `...` bodies become LLM-driven
loops (CodeAct REPL by default), type annotations are validated I/O contracts,
docstrings are prompts, fields are model-visible state, and arguments pass by
reference as live Python objects. Orchestration is ordinary Python code.

Funes: declarative YAML personas (name, prompt, tool allowlist, model settings,
`max_steps`, `require_tools`) over a fixed C++ tool registry. One JSON tool
call per round; everything — arguments, results, delegation — flows through the
transcript as text. In the paper's own Table 7 taxonomy, Funes lands in the
"text in / text out, shell-tool" family with OpenCode and PI — the harnesses
NOOA beats on every benchmark.

## Where NOOA's critique hits Funes

**Everything is serialized through the transcript.** Every tool result becomes
text in history; large results eat context, then `compress_context` lossily
folds them into a summary. The paper measures exactly this cost: NOOA reaches
82.2% on SWE-bench Verified with ~1.1M tokens/task vs PI's 78.2% at 2.2M,
because values stay live in the REPL and the prompt carries only bounded
previews (`list(len=100, [:5]=[...])`). Funes has no equivalent — a large
`web_fetch` result is either fully in context or gone.

**No typed output.** A Funes agent's final answer is free text.
`require_tools` (completion_contract.h) is Funes' version of what the paper
calls "validated termination" — its trace analysis identifies this as a key
difference vs harnesses where any no-tool-call reply ends the run (77% of
OpenCode's failed Terminal-Bench trials quit within ten steps). Funes
independently converged on the right idea, but in a coarser form: it checks
*that* named tools succeeded, not that the final output is well-formed. NOOA
validates a typed return value and bounces failures back into the loop.

**Loop engineering by prompt prose.** The funes.yaml "HARD RULE: after
delegating the newsletter, do NOT also call web_search" paragraph is a
symptom: orchestration deficiencies patched with prompt text because YAML
cannot express control flow. In NOOA that constraint is three lines of
ordinary Python. Prompt rules are hopes; code is guarantees.

**One tool call per round.** Batch work costs N round trips and N× context
growth. A CodeAct model writes a `for` loop.

**Memory is passive.** Funes auto-stores every exchange, dedups only on exact
(agent, text), and never consolidates or forgets — the store grows
monotonically. The paper measures +11.8 RHAE points for its agent-curated
memory (importance scores, spontaneous injection via activation ranking,
asynchronous reflection that merges/abstracts/prunes) over flat file notes.
Funes' `remember`/`recall` are the right primitives; the curation layer above
them is missing.

## Where Funes is right and the paper undersells it

**Security surface.** CodeAct means arbitrary model-written Python executing
in-process. To contain it for ARC-AGI-3 the paper needed kernel-enforced
Landlock sandboxes, seccomp network blocks, per-cell OS workers, privilege
drops, and an 18-pass red-team audit. Funes' fixed tool surface with
`fs_guard`/`net_guard` is auditable in an afternoon. For a persistent personal
assistant running on the user's own machine, that trade is defensible.

**Declarative definitions are data.** `agent-builder` can safely generate a
new agent at runtime because a YAML file cannot do anything a registered tool
can't already do. An agent-builder emitting executable Python is a
categorically different risk.

**Small/local models.** Funes targets llama.cpp at `localhost:8080`. The
paper's own Table 2 shows small/efficient models passing only 70.8% of NOOA's
stress tests — batch bookkeeping, REPL iteration, and multi-step harness
discipline are exactly where they fail (sentiment_batch: 40%). Plain JSON tool
calls are the most heavily trained interface in existence. NOOA's headline
gains are demonstrated on GPT-5.5 and Opus 4.6; the paper is silent on
30B-class local models beyond Nemotron Nano's 91.6% on the *easy* suite.

**Simplicity.** ~850 lines of core C++, one binary, no Python runtime. NOOA is
a framework: strategies, ContextManager, EventManager, MemoryManager.

## Verdict

The paper's three strongest ideas — pass-by-reference to escape transcript
bloat, typed return validation, and curated memory with consolidation — target
real weaknesses in Funes. But its headline architecture (the model writes
code) is the wrong default for Funes' deployment target: local models,
personal machine, minimal sandbox.

Portable wins, in rough order of value:

1. **Typed final answers.** Extend `require_tools` toward validating a
   structured result (e.g. a JSON schema for the final message on agents whose
   output feeds another agent). Funes is halfway there.
2. **Bounded previews + result store.** Cap what a tool result contributes to
   the transcript; keep the full value addressable via a `get_result(id)`
   tool. This is pass-by-reference re-implemented at the tool-call layer — no
   REPL required.
3. **Memory consolidation.** A periodic reflection pass: merge near-duplicates,
   re-score, prune. Runs offline, uses the existing store.

## Open question

Delegation passes a single string each way, and the specialist's turn is never
persisted. If the researcher fetches 50KB and funes needs it, the only
channels are re-serialization into the answer or the shared memory store. Is
that isolation a feature, or Funes' own version of the transcript-serialization
problem the paper is attacking?
