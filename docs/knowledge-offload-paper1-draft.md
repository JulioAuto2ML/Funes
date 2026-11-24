# Facts Outside the Weights: How Much Model Does It Take to Know Facts?

*Draft 2, 2026-11-24. Paper 1 of 2 (inference only; the companion paper 2 changes the model).* Author: Julio Rodriguez Martino (independent). Code, data and notebook: `benchmarks/knowledge-offload/`.

Every number in this paper comes from a results table committed in the repository. Where a result does not exist yet, the text says so with a marker in square brackets (TBD, OPEN or UNVERIFIED); Appendix B.5 lists them all. File hashes, the process notes and the drafting notes are in the appendices, so the body reads without them.

**Scope.** This paper answers one question with models used *as released*: where should a fact live (in the weights, in the prompt, in a local store, on the web, in a deployed agent's memory) for a model of a given size to know it? It makes no claim about whether a model that is built or tuned for lookup can be smaller, about knowledge-removed models, about a lookup LoRA, or about an agent that fits a given memory budget. Those are paper 2's questions and appear here only as pointers.

---

## In plain words

**What we asked.** A small language model knows far fewer facts than a big one. Is the cure a bigger model, or a better place to keep the facts? We took one model family in five sizes, put the same 1,000 factual questions to each, and changed one thing at a time: where the fact lives.

**What we found.**
- When the model is handed its facts, size matters much less. The 0.8B model with a local store of facts scores 0.921. The 27B model answering from memory scores 0.431.
- Letting the model fetch the facts itself is less reliable. Whether it calls the lookup tool does not follow size, and it changes with one sentence of the prompt. A rule in the harness that forces one lookup repairs the 9B (0.347 to 0.991). It does nothing for the 0.8B, which never calls the tool.
- The agent we deploy does much worse than the plain store at 0.8B and 2B. From 4B up it is close to it.
- The web is a poor substitute for the local store, and a cache of past web answers did not close the gap.
- On a task that needs no facts (grade-school maths), adding the store leaves the size curve alone, as a negative control should.

**What we could not show.** That forcing the lookup costs nothing where the model already looks up: we could not bring it inside two points. Anything about conflicts between the store and the weights, or about knowing when to look up, because those experiments were not run. Anything beyond one model family, one machine and one run per cell.

## Abstract

Small language models answer long-tail factual questions badly. Bigger models do better mostly because they store more facts. We ask where a fact should live for a model of a given size to know it.

We run one model family (Qwen3.5: 0.8, 2, 4, 9 and 27B parameters, 4-bit quantised, with 8-bit controls up to 9B) on 1,000 unambiguous PopQA questions in one fixed harness. The facts are in the weights (`closed`), in the prompt (`oracle`, `store`), behind a tool the model may call, on the web, or in the memory of a deployed agent.

Giving the model its facts cuts the slope of accuracy against size to 0.27 of the closed-book slope (95% CI 0.21 to 0.35). The 0.8B model with a local store (0.921) beats the 27B model closed-book (0.431) by 0.490 (CI 0.457 to 0.523).

The tool route is harder. Whether a model calls the lookup tool, when the prompt only says "use the tools when they help", does not follow size (lookup rate at 0.8, 2, 4, 9, 27B: 0.000, 0.000, 0.875, 0.001, 0.887). A harness contract that forces one lookup makes the small models usable (9B: 0.347 without, 0.991 with). We could not show that forcing the lookup costs no accuracy where the model already looks up: equivalence within two points was not shown (pooled p = 0.135). The 9B's skip depends on wording. Removing the sentence that asks for a short answer takes its lookup rate from 0.001 to 0.732 (CI 0.704 to 0.759). Reordering the two sentences, or describing the tool, leaves it at 0.000 (one greedy run per cell, descriptive).

The deployed agent's own loop, with its memory holding the same triples, is far below the plain store at 0.8B and 2B (−0.285 and −0.256). From 4B up its point estimates are within 0.05 of the store. Its runs fetched outside content on 18.1% of the questions at 0.8B, against at most 0.7% from 4B up (one repeat, descriptive).

On a reasoning task that needs no facts (500 GSM8K problems) the same store leaves the size curve intact (slope ratio 1.04, CI 0.959 to 1.124), as a negative control should. Irrelevant retrieved facts cost between 0.8 and 3.6 points at every size and quantisation.

H1 and H2 survive Holm correction (adjusted p ≤ 0.0003 over the three hypotheses this paper tests; ≤ 0.0006 over the pre-registered six). H5 does not (0.135 and 0.541). The family was narrowed on 2026-11-22, after the data. Conflicts and when to look up were not run. Negative and exploratory results are reported as such.

---

## The map: arms, hypotheses and experiments

Think of an exam. In a *closed-book* exam the student answers from memory. In an *open-book* exam the right page is on the desk. In between there is a library: the student may walk over and look something up, or may just guess. Our experiment is that exam, run on language models of five sizes from one family (Qwen3.5: 0.8, 2, 4, 9 and 27 billion parameters, written 0.8B to 27B).

The questions come from PopQA, a set of factual questions about people, places and works ("Who directed this film?"). The *store* is a local database of the 14,267 facts (subject, relation, object) that PopQA's questions were made from. Every arm answers the same 1,000 questions. What changes between arms is where the fact lives.

| arm | the exam analogy | what happens | where |
|---|---|---|---|
| `closed` | closed book | the model answers from its weights | §5.1 |
| `oracle` | the right page, handed over | the question's own fact is put in the prompt | §5.1 |
| `store` | a clerk puts the best search results on the desk | the harness retrieves the best-matching facts and puts them in the prompt | §5.1 |
| `tool` | the student may go to the library | the model has a `lookup` tool and decides whether to call it; its *lookup rate* is the share of questions on which it did | §5.3 |
| `tool+contract` | the proctor refuses the answer sheet until the student has been to the library | like `tool`, but the harness does not accept an answer until `lookup` has worked | §5.4 |
| `web` | the Internet instead of the library | the model has a `web_search` tool | §5.5 |
| `web+writeback` | the student keeps notes on what they found | the harness saves web answers to a cache, then asks the questions again | §5.6 |
| `funes` | the student at work in a real office | the same questions go through the deployed agent (Funes) with its own prompts, tools and a memory holding the same facts; a *funes row* is the result line for this arm | §5.8 |

We wrote the hypotheses before running the sweep. This paper tests three of them and reports three more as exploratory:

- **H1.** With the store, accuracy grows less than half as fast with size as it does closed-book (§5.1).
- **H2.** On a task that needs no facts, the store should not change the size curve. This is the negative control for H1 (§5.7).
- **H5.** Forcing the lookup costs nothing where the model already looks up, and cuts wrong answers where it did not (§5.4).
- **H6** (exploratory): a write-back cache cuts web calls (§5.6). **H8** (exploratory): the lookup rate rises with size (§5.3).
- **H3** (conflicts between store and weights) was not run. **H4** (knowledge-removed models) and **H7** (an agent fits a smaller model) are paper 2's.

The experiments are numbered. E0 was the pilot, run on a different 9B file that is kept out of every curve (§3.2). E1 is the main sweep. E2 (conflicts), E3 (when to look up) and E4 (a messy store) were not run or built. E1b and E5 to E7 belong to paper 2.

**Other terms.**
- *Quantisation*: storing weights in fewer bits. `IQ4_XS` is about 4 bits per weight, `Q8_0` is 8 bits. *Greedy decoding*: always taking the most likely next token, so a run can be repeated.
- *Contract*: the rule in `tool+contract`. *Taint*: a run is tainted when it fetched content from outside the local store (a web page, a search result, another agent's answer).
- *Holm correction*: a step-down adjustment of p-values when several hypotheses are tested together. *TOST*: two one-sided tests, used to show that a difference is smaller than a stated margin (equivalence).

---

## 1. Introduction

A language model's parameters carry two things at once: how to do things (follow an instruction, call a tool, add two numbers) and what is true (who directed this film, which country this village is in). The second is expensive to store in weights, stale the day after training, impossible to inspect or edit, and the part a small model is worst at. A deployed agent can keep facts in three places: in the weights, in a local store the agent queries (a database of what the user and the agent have recorded), or on the web. This paper measures what each placement buys and what it costs, in a working harness and in one deployed agent, across five model sizes of one family.

The question is *where a fact should live for a model of a given size to know it.* We hold the question set, the prompts, the decoding and the serving flags fixed; we vary model size, quantisation and where the fact lives; and we report every comparison paired, on the same 1,000 questions. Whether a smaller model can then do the rest of an agent's job in less video memory, and whether a model can be changed so that it needs fewer facts, are separate questions that need other experiments; they are the subject of the companion paper and are **not answered here**.

**Contributions.**
1. A size curve in which the only thing that changes between conditions is where the fact lives (§5.1): the store slope is 0.27 of the closed-book slope (IQ4_XS) and 0.21 (Q8_0), and the smallest model with the store is above the largest model without it.
2. A measurement of what the tool route adds and what it hides (§5.3–5.4): with a lookup tool and a neutral prompt, accuracy depends mostly on whether the model decides to call the tool, which is not a function of size in our data and moves with one sentence of the prompt; a harness-side contract (the named tool must succeed before an answer is accepted) removes that dependence at a cost we could not bound to two points.
3. A description of the outside-content price: web search is far below the local store (§5.5), and a write-back cache did not recover it (§5.6).
4. The same questions run through the deployed agent's own loop (§5.8) with the security-relevant counts the plan promised (how often a run fetched outside content), and a cost table with the quantities that were measured (§6).
5. A harness, a hash-locked model list and every per-question result, enough to rebuild each table without a GPU (§8).

What this paper does *not* claim: that a 0.8B model "is" a 27B model with a database, that any result here is a law of size, that the findings carry to other model families, anything about conflicts between the store and the weights (not run), or anything about models that were changed (paper 2).

## 2. Related work

**The question and the short answer.** What has been done about moving facts out of a model's weights, and where does this paper sit? Most of the related work changes how the model is trained or built. We change nothing in the model. We vary where the fact lives and measure what that does at five sizes.

Our reading of the papers below is limited. For each one we read the abstract and the introduction in full text (arXiv PDF) and the passages that bear on our sentence; we did not read every page. The audit of what each paper says is in `benchmarks/knowledge-offload/CITATION_AUDIT.md`. We describe each paper by what it does and do not claim to have surveyed the field.

**Models pre-trained to look facts up.** Limited Memory Language Models (LMLM, Zhao et al., arXiv:2505.15962) mask the values of entity-level facts out of the pre-training loss and train the model to emit a lookup call instead, against a database of triples. They report lower perplexity, unchanged scores on language-understanding tasks, unlearning by deleting database rows, and higher factual precision than a same-size standard model, including on PopQA (a 382M LMLM gains 28.1 points of PopQA accuracy); their models are at most 382M parameters. Co-LMLM (Feldman et al., arXiv:2607.07707) replaces the textual lookup with a continuous vector query and reports, at 360M parameters, lower perplexity than models trained on 40 times more data. An audit of deletion in LMLMs (Raeesi and Roed, arXiv:2607.00605) finds that after a fact is deleted the model rarely returns it from its parameters, and that what survives comes mostly from near-neighbour retrieval. These works change how the model is pre-trained. We take released models as they are and change where the fact lives; what they would mean for a model trained this way is not tested here.

**Other ways to separate knowledge from the model.** Knowledgeless Language Models (Cohen et al., arXiv:2607.12831) pre-train on text with named entities anonymised, which collapses closed-book recall and improves answers that use provided evidence. Knowledge Offloading (KOFF, Galliamov, Choenni and Titov, arXiv:2605.29075) splits a pretrained model into a pruned shared backbone and domain- or language-specific memories (LoRA adapters and learned key-value caches), largely preserving perplexity and MMLU on Llama and Qwen models of 3B to 14B. Mixture-of-Memories (Lu et al., arXiv:2610.25425) adds two neural memory experts and an explicit external memory to a frozen model. Intern-S2-Mobius (arXiv:2608.14290) builds a shared feed-forward memory and attention "reasoners" into one architecture; its knowledge store is inside the model. KARLA (Crespin, Suchanek and Holzenberger, arXiv:2606.26807) is the closest to the work of paper 2 and is discussed there. None of these is an inference-time comparison of where a fact lives across model sizes.

**Knowledge supplied through attention or an explicit memory.** KBLaM (Wang et al., arXiv:2410.10450) keeps the model frozen, encodes each knowledge-base triple as a key and a value vector with trained linear adapters, and injects them through a modified attention. It handles more than 10,000 triples in an 8B model at a cost linear in the size of the base, performs comparably to in-context learning, and learns to refuse when the base does not hold the answer. MemLLM (Modarressi et al., arXiv:2404.11672) fine-tunes Mistral-7B with LoRA to read and write an explicit memory of relation triples. Knowledge Objects (Zahn, Chana et al., arXiv:2603.17781) compare facts kept in the prompt with hash-addressed fact tuples: in the prompt, a frontier model is exact up to 7,000 facts but overflows at 8,000 and loses facts when the context is summarised.

**Popularity, scale and when to retrieve.** PopQA (Mallen et al., arXiv:2212.10511) is our question set. The authors found that language models fail on low-popularity entities, that retrieval helps most there, and that scaling mainly improves recall of popular facts. They also propose retrieving only when the subject's popularity is below a threshold. Our lookup-rate results (§5.3) measure a related but different thing: whether a model calls a lookup tool by itself under a given prompt.

**Editing facts in the weights.** ROME (Meng et al., arXiv:2202.05262) locates factual recall in mid-layer feed-forward modules, edits a single association by a rank-one update and introduces the CounterFact dataset; MEMIT (Meng et al., arXiv:2210.07229) edits thousands of associations at once in GPT-J and GPT-NeoX. They motivate an experiment on conflicts between a store and the weights, which this paper does not contain (§5.9, not run).

**Memory and reasoning as separable abilities.** Several papers ask whether recalling and reasoning use different parts of a model. Jin et al. (arXiv:2411.13504) train a model to mark its steps as `<memory>` or `<reason>`. Layer studies find knowledge and retrieval in shallow layers and reasoning in deeper ones (Song et al., arXiv:2510.02091; Fartale et al., arXiv:2510.03366; Yang, Gao and Wu, arXiv:2507.18178). Hong et al. (arXiv:2503.23084) find linear directions in the residual stream that govern the balance between reasoning and memorisation. MemoReason (Tighidet et al., arXiv:2609.35312) swaps real entities for fictitious ones and finds that reasoning accuracy falls by up to 15.7%, so familiar facts help reasoning, though models rarely give the real answer in the fictitious version. Our negative control (§5.7) asks a simpler question: whether a fact store changes the size curve of a task that needs no facts.

**What we add.** We run one paired, size-swept comparison of where a fact lives, in one controlled harness and then through one deployed agent's own loop, with the count of how often a run fetched outside content. We do not claim that this has not been done: `[UNVERIFIED: no literature search beyond the papers above was made; web search is not permitted in this track]`. The controlled harness (§3.5) and the deployed agent (§5.8) are different measurements and are never pooled.

## 3. Setup

**The question and the short answer.** What exactly was run? One question set, five model sizes, eight arms, one GPU. The map in the front matter gives the idea; this section is the exact specification.

### 3.1 Questions
PopQA (Mallen et al.) test set, from which we keep the **unambiguous** items: a (subject, property) pair that has exactly one candidate object over all 14,267 triples in the file (E0: of 1,000 random items, 142 were ambiguous; every hedged answer in E0's `store` condition, 81 of 142, was on homonym subjects; `NOTEBOOK.md`, D5). A sample of **1,000** is drawn stratified by popularity bucket (dataset seed 1). Bucket counts: 1e0: 16, 1e1: 195, 1e2: 195, 1e3: 195, 1e4: 195, 1e5: 194, 1e6: 8, 1e7: 2 (`E1_TABLE.md`). Buckets of 16, 8 and 2 questions are not read.

### 3.2 Models
Qwen3.5 at 0.8, 2, 4, 9 and 27B, as GGUF files from `unsloth`, in two precisions. `IQ4_XS` is used at every size and gives the main curve. `Q8_0` is a control at 0.8, 2, 4 and 9B. The 27B at Q8_0 does not fit the 16 GB card, so the control curve has four points (D11, notebook). Every file is pinned in `models.lock` by repository, file name, size and sha256, and the runner refuses a file whose bytes do not match (`ko/models.py`). The model's identity is a file, not a name.

All sizes are served the same way: llama.cpp `llama-server` with `--jinja --reasoning-budget 0 --chat-template-kwargs enable_thinking=false`, context 8,192, one slot, f16 KV cache and no prompt cache (`scripts/ko_swap.py`; notebook). Every request carries `enable_thinking: false` and `tool_choice: auto`, with temperature 0 and `max_tokens` 128. File sizes and the chat-template check are in §8 and Appendix D.

*Lineage note (a correction we made, kept in the paper's record).* Our first pilot (E0) used a nominally Qwen3.8 9B file that turned out to be a community distillation; its numbers are reported only as E0 and never mixed into the size curve (`models.lock` records it as `community-distill (inferred)`).

### 3.3 Conditions
All conditions answer the same question with the same one-sentence system prompt (`SYSTEM`: "You answer factual questions. Reply with only the answer: a short phrase, with no explanation and no full sentence.") unless stated.

| condition | where the fact lives | what the model does |
|---|---|---|
| `closed` | the weights | answers |
| `oracle` | the prompt | the question's own triple is given to it |
| `store` | a local store, retrieved by the harness | BM25 over all 14,267 triples; top results are put in the prompt |
| `tool` | a local store, queried by the model | a `lookup` tool it may call; default prompt = `SYSTEM` + "Use the tools you are given when they help." (`SYSTEM_TOOL`); up to 4 steps |
| `tool+contract` | same | `tool`, plus the harness refuses a text answer until `lookup` has succeeded; at most 2 nudges ("You have not used {tool} yet. Call it before you answer.") |
| `web` | the web | a `web_search` tool (Parallel's search, keyless MCP; Tavily as a fallback) |
| `web+writeback` | the web, then a local cache | after each answer that used `web_search`, the harness stores "Q … A … (per url)" as tainted; a second pass repeats the questions |
| `funes` | the deployed agent's memory | the production agent loop (the question sent through `/api/chat` of a scratch Funes instance with a throwaway account and the 14,267-triple store loaded as its memory pool, `ko/funes.py`); IQ4_XS only, one repeat per size (§5.8) |

Prompt variants: `search_first` (the prompt tells the model to look up first; used for E1's tool cells), the default prompt above (used for H5/H8), `abstain_ok` / `abstain_default` (the default tool prompt plus an abstention sentence, ablated arm only, D16), and the three pre-registered default-style variants `tool_first`, `tool_free`, `tool_describe` (§5.3, App. A).

**The `store` condition is at ceiling by construction** (§7): the PopQA questions are templated from the very triples that the store holds.

### 3.4 The ablated arm
The first 200 sample questions again, with the question's own triple removed from retrieval (one filter, no second store). Outcomes: *correct* (answered from the weights), *abstained*, *hallucinated* (a wrong answer). This arm exists for H5b.

### 3.5 Funes as the testbed
Funes is a self-hosted agent whose memory is a SQLite store with vector search, full-text search and links, run by a single C++ agent loop with a recall step, a tool-calling loop against the model, and several safety layers (a completion contract, a tool budget, loop detection, a gate that holds side-effecting calls after outside content has been read; `src/core/README.md`). The experiments of §5.1–5.7 do **not** run that loop: they use a purpose-built harness (`benchmarks/knowledge-offload`) so that every condition differs in one thing. The `funes` rows (§5.8) are the exception: the same 1,000 questions asked of the agent itself, on a scratch instance with a throwaway account whose memory holds the same 14,267 triples, so that the controlled harness and the deployed loop can be put side by side and not confused. `[TBD: a diagram of the two paths; not drawn]`.

### 3.6 Hardware and windows
One AMD RX 9060 XT (16 GB, ROCm) on the machine that also serves the production model; each size was served in a swap window, with the production 27B stopped and restored after every block by `scripts/ko_swap.py`, which refuses to start while a cron job is due and ends a run 17 minutes before the next window. The model is never run on a laptop CPU. Measured run times of the 36-cell default-prompt job: queue 11:26 to 18:44 on 2026-11-17, 7.3 h wall, 4.91 h of cell time (`results/queue_h5h8.log`).

## 4. Methods

**The question and the short answer.** How do we decide that a difference is real? Every arm answers the same 1,000 questions, so we compare arms question by question (paired), put 95% bootstrap intervals on every difference, and correct the p-values of the hypotheses together (Holm). Greedy decoding makes one run per cell repeatable, so our intervals cover the choice of questions only, not run-to-run noise.

### 4.1 Primary metric and its hierarchy
**`contains` on the 1,000 unambiguous items** is primary: the answer, normalised, contains the gold object. `exact` and `contains ∧ ¬hedge` (a hedge is an answer that names two or more candidate objects of the same subject) are secondary; on this sample the three coincide to within the table's precision, because unambiguous items rarely admit a list. The choice was made after seeing that on the full, ambiguous sample `contains` credits a list while hedge-adjusted scoring penalises it, so comparing conditions there compares how they handle ambiguity, not knowledge (D5; E0: `store` .980 hedge-unadjusted vs .905 adjusted). Ambiguous items are reported separately and are exploratory (E0: 142 items).

### 4.2 Paired design and uncertainty
Every condition answers the same questions, so comparisons are paired: McNemar (exact) for accuracy differences, a paired bootstrap for the size of a difference. All intervals are 95% percentile bootstraps over questions, 10,000 resamples, and one resample is shared across sizes and conditions so that every difference and slope is paired (`stats_e1.py`; E0's tables used 2,000 resamples). One run of each cell: greedy decoding is deterministic here: two repeats of the five harness conditions were bit-identical in E0 (0 of 1,000 predictions differ) and a 100-item pilot at the 9B IQ4 for three conditions found 0 of 100 predictions different (notebook).

**The `funes` rows.** They may vary between runs: the agent loop's own temperature is not the harness's. Source check (2026-11-22): `agents/funes.yaml` sets no `temperature`, and the agent loop's default is 0.2 (`LLMClient::temperature_ = 0.2f`, `src/core/llm_client.h:159`; `agent_config.h:89-97`), whereas the cell files record the harness's `temperature: 0.0`. `[UNVERIFIED: that the served `funes` runs actually used 0.2: read in source, not in a request log]`.

**We therefore report no run-to-run variance for the main cells; the intervals are sampling intervals over questions only** (§7).

### 4.3 Hypotheses and the Holm family

In short: H1 and H2 hold under either correction below. H5 holds under neither.

H1–H8 were written in the plan and committed before the E1 sweep (plan, "Hypotheses, stated before running"; reworded where noted by date in the plan and by decisions D1–D16 in the notebook). The **pre-registered confirmatory family is H1, H2, H3, H4, H5 and H7 (m = 6)**; H6 and H8 are exploratory and outside it. H3, H4 and H7 were never tested and enter Holm as p = 1.

**The cut into two papers was decided on 2026-11-22, after the data of this paper existed.** This paper tests H1, H2 and H5 (H3 would be a fourth if E2 is run, Appendix B.2). It therefore reports two adjusted p-values for each, and the reader should hold to the **conservative one (m = 6, as pre-registered)**; the m = 3 value is what correcting only over the tests actually run gives, and exists because the family was narrowed after seeing results, which is exactly why it is not the headline. H4 and H7 belong to the companion paper, which does the same with its own tests.

Equivalence tests (H5a) use TOST with a ±2 point margin; equivalence is declared only when the TOST p is below the Holm threshold. Raw p-values that reach the bootstrap's resolution are printed as ≤ 1e-4; adjusted values built from them are upper bounds.

| test (IQ4_XS) | raw p | adjusted, m = 3 (H1, H2, H5), computed here by the Holm step-down | adjusted, m = 6 (`HOLM_TABLE.md`) |
|---|---|---|---|
| H1 (store slope < ½ closed slope; 0.8B+store ≥ 27B closed) | ≤ 1e-4 | ≤ 0.0003 | 0.0006 |
| H2 (GSM8K, store slope ≥ ½ closed slope) | ≤ 1e-4 | ≤ 0.0003 | 0.0006 |
| H5 (forcing costs nothing where the model already looks up; hallucination) | 0.135 | 0.135 | 0.541 |

On Q8_0 H1 is not fully testable (no 27B Q8_0 cell: p = 1, so adjusted 1 in both families), H5a has no eligible size (p = 1), and H2 is ≤ 1e-4 (adjusted ≤ 0.0003 and 0.0006) (`HOLM_TABLE.md`, sha256 `528b993e…`).

### 4.4 The abstention outcome
Abstention is decided by a regular expression (`ko/ablate.py`). Its agreement with 50 hand labels, half of which the regex called abstained, is **49 of 50 (98.0%)**; the one disagreement is a `closed` answer ("Oregon") the regex called hallucinated and the human called correct (`results/abstain_labels.tsv`; `E1_TABLE.md`, `H5H8_TABLE.md`). An earlier claim of 50/50 was circular (the regex had been tuned on the same answers) and was withdrawn (notebook 9e20a44). The check is one labeller on 50 answers; `[TBD: second labeller, 200 answers, item 4 of Appendix B.3]`.

### 4.5 Validity rules
A cell whose tool backend failed is marked INVALID and excluded from every difference, slope and bucket (`ko/validity.py`, `results/invalid_cells.json`). The harness stops a cell on any failed search (exit 4) and refuses to write it. Data and decisions that changed after seeing results are labelled as such in the notebook; any analysis added after seeing results is called *exploratory* here.

### 4.6 Search backends and the search budget
`web` uses Parallel's keyless search MCP (five excerpts per search, about 3.5 kB each as measured in the first probe, cut to 600 characters per excerpt in the model's view) with Tavily as fallback. In E1 every search of every valid web cell was served by Parallel. Parallel's USD 20 welcome credit ran out during the 27B cell: **1,661 of its 2,086 searches returned `402 Insufficient credit`** (`WEB_AUDIT.md`); Tavily's remaining quota is reserved for production Funes, so no web cell is rerun until the author approves a written budget.

### 4.7 How the work was organised
The experiments were designed, coded, run and analysed by language-model sessions (Claude Code) working under one human, the author, with no shared chat context. A session knew only what was in the repository and in its brief. The roles, the rules of work, the files that carried information between sessions, the failures and the limits of the arrangement are in Appendix C, with the evidence for each statement.


## 5. Results

All numbers in this section are measured, from the files named. Intervals are 95%.

### 5.1 Supplying the facts flattens the size curve (H1)

**In words.** If the model is handed its facts, does it still matter how big it is? Much less. A small student with the right page on the desk beats a big student working from memory. With the store, accuracy rises 0.27 as fast with size as it does closed-book (95% CI 0.21 to 0.35), and the 0.8B model with the store (0.921) beats the 27B model without it (0.431) by 0.490 (CI 0.457 to 0.523).

*Source: `results/E1_TABLE.md`; n = 1,000, `contains`, IQ4_XS.*

| | 0.8B | 2B | 4B | 9B | 27B |
|---|---|---|---|---|---|
| `closed` | 0.213 [0.188, 0.238] | 0.243 [0.217, 0.270] | 0.320 [0.292, 0.349] | 0.350 [0.321, 0.379] | 0.431 [0.400, 0.461] |
| `oracle` | 0.972 | 0.995 | 0.997 | 0.987 | 0.997 |
| `store` | 0.921 [0.904, 0.937] | 0.971 [0.960, 0.981] | 0.984 [0.976, 0.991] | 0.980 [0.971, 0.988] | 0.991 [0.985, 0.996] |

- Slope of accuracy against log10(parameters): `closed` +0.146 per decade [0.127, 0.165]; `store` +0.040 [0.031, 0.049]; ratio **0.27 [0.21, 0.35]** (Q8_0: 0.21 [0.13, 0.31]); the pre-registered threshold (ratio < 0.5, upper bound below 0.5) is met on both quantisations.
- **0.8B + `store` vs 27B `closed`** (IQ4_XS): 0.921 vs 0.431, Δ = +0.490 [+0.457, +0.523]; 510 questions only the 0.8B-with-store got right, 20 only the 27B closed-book (McNemar p = 5.2e-124).
- `store − closed` per size shrinks with size: +0.708, +0.728, +0.664, +0.630, +0.560 (0.8 to 27B).
- By popularity: closed-book accuracy is lowest in the middle of the popularity range (e.g. 9B: 0.118 at bucket 1e2, 0.773 at 1e5) and the store lifts every bucket to 0.96 or above at 4B and up. Buckets 1e0, 1e6, 1e7 are too small to read.
- **Holm.** H1 raw p ≤ 1e-4 (the bootstrap's resolution); Holm-adjusted (m = 6, three tests with p = 1; H2 enters with its GSM8K p, §5.7) 0.0006 (`results/HOLM_TABLE.md`, as of e4432cd; hash in Appendix D). **H1 on Q8_0 is not testable in full** (no 27B Q8_0 cell), so its H1b half is p = 1 there.

*Caveats that belong to this result.* (i) `store` is at ceiling by construction (§7). (ii) `oracle − store` is the reading-and-retrieval cost and shrinks with size (+0.051 at 0.8B, +0.006 at 27B), so part of the `store` slope is retrieval, not knowledge. (iii) The slope is fitted on five points of one family. (iv) The CI on the slope resamples questions only.

### 5.2 What the store costs: reading capacity

**In words.** Does a small model read a retrieved fact as well as one it is handed? Almost. At 0.8B the retrieved version scores 0.051 below the handed-over one, and from 9B up the gap is within noise.
At the smallest size, giving the model the exact triple (`oracle` 0.972) is better than letting the harness retrieve it (`store` 0.921; difference +0.051 [0.035, 0.067], 60 vs 9 discordant). From 9B up the difference is within noise (9B +0.007 [−0.003, +0.017]; 27B +0.006 [0.000, +0.013]). Pre-registered handling: a gap above 10 points with retrieval fine would have been reported as a reading-capacity limit; the observed gap is 5.1 points. **Retrieval hit:** the gold triple was among the retrieved results for **0.993 of the 1,000 questions at every one of the five sizes** (computed on 2026-11-22 from the per-answer `retrieval_hit` field of the five committed `store` cells; the pre-registered kill criterion was < 0.9, so the store's retrieval is not what limits these cells). This number is in the notebook entry of 2026-11-22 and is not in `E1_TABLE.md`. `[TBD: E4 store shape, only if a messier store is wanted; no evidence that retrieval is the limit]`.

### 5.3 When the model looks up on its own (H8, exploratory)

**In words.** Given a lookup tool and a neutral prompt, does the model use it? That depends on the checkpoint and on the wording, not on size. The lookup rates at 0.8, 2, 4, 9 and 27B are 0.000, 0.000, 0.875, 0.001 and 0.887 (IQ4_XS). The 9B sits between two models that look up and almost never does, until we delete one sentence from the prompt (0.732).

*Source: `results/H5H8_TABLE.md`, default prompt, 1,000 questions per cell.*

| lookup rate, `tool`, default prompt | 0.8B | 2B | 4B | 9B | 27B |
|---|---|---|---|---|---|
| IQ4_XS | 0.000 | 0.000 | 0.875 | 0.001 | 0.887 |
| Q8_0 | 0.006 | 0.000 | 0.574 | 0.004 | – |

- **H8 as pre-registered (lookup rate rises with size) is not supported.** The slope is +0.485 per decade [0.474, 0.496] (IQ4_XS) and +0.153 [0.144, 0.163] (Q8_0), but the profile is non-monotone: the 9B is at 0.001 between the 4B (0.875) and the 27B (0.887). A slope through these points describes the fit, not a size law, and we do not report it as one.
- When a model does not call the tool, its accuracy equals its own `closed` accuracy (9B IQ4: 0.346 on 999 questions; `closed` 0.350) and its answers are short (E0/H5H8 audit: mean 3.9 completion tokens, no tool-call markup in any prediction at any size, measured in the notebook audit entry).
- Under `search_first` the same 9B looks up 0.898 (IQ4_XS) and 1.000 (Q8_0) (`E1_TABLE.md`): the tool path works for this model, so the default-prompt skip is not a broken harness.
- The 4B and the 27B skip most when the entity is popular: 4B IQ4 lookup 190 of 195 at popularity bucket 1e1 and 101 of 194 at 1e5 (computed from the cell files, notebook audit entry). The decision is partly calibrated to what the model knows.
- Quantisation changes it: 4B IQ4_XS 0.875 versus Q8_0 0.574; of the 4B's shared lookups, 301 questions were looked up only by IQ4 and none only by Q8.
- The chat template of the 0.8–9B files is byte-identical and the 27B differs by one character (§3.2), and the serving flags are identical across sizes; so the difference between 4B and 9B is not a template or flag difference. What remains is the weights and the prompt. **Not excluded:** the server's own tool-call parsing (same binary for all) and the one-character 27B template difference.

**Pre-registered variants (stages A and B).** To test whether the 9B's skip is a property of the checkpoint or of one wording, three variants of the default prompt were fixed before any run (`NOTEBOOK.md`, entry "step 2 of the 9B plan", approved by the author and frozen): `tool_first` (the two sentences in the other order), `tool_free` (the sentence asking for a short answer removed), `tool_describe` (the tool described, not ordered). Stage A: 9B and 4B IQ4_XS. Stage B: 4B and 9B Q8_0. Stages C (27B) and D (2B) were not run. Each cell is 1,000 questions, greedy, one repeat; intervals are question bootstraps; there are no p-values and the variants are outside the Holm family.

*Source: `results/VARIANTS_TABLE.md`, 12 of 12 cells, `verify_variants.py` exit 0 (notebook, 2026-11-18).*

| lookup rate [95% CI] | default (baseline) | `tool_first` | `tool_free` | `tool_describe` |
|---|---|---|---|---|
| 9B IQ4_XS | 0.001 | 0.000 [0.000, 0.000] | **0.732** [0.704, 0.759] | 0.000 [0.000, 0.000] |
| 9B Q8_0 | 0.004 | 0.044 [0.032, 0.057] | **0.763** [0.737, 0.789] | 0.001 [0.000, 0.003] |
| 4B IQ4_XS | 0.875 | 0.811 [0.787, 0.835] | 0.911 [0.893, 0.929] | 0.977 [0.967, 0.986] |
| 4B Q8_0 | 0.574 | 0.480 [0.449, 0.510] | 0.762 [0.735, 0.788] | 0.602 [0.572, 0.632] |

| `contains` accuracy [95% CI] | default | `tool_first` | `tool_free` | `tool_describe` |
|---|---|---|---|---|
| 9B IQ4_XS | 0.347 | 0.335 [0.306, 0.364] | 0.933 [0.917, 0.948] | 0.345 [0.316, 0.374] |
| 9B Q8_0 | 0.350 | 0.377 [0.347, 0.407] | 0.913 [0.895, 0.930] | 0.342 [0.313, 0.371] |
| 4B IQ4_XS | 0.975 | 0.945 [0.931, 0.959] | 0.985 [0.977, 0.992] | 0.984 [0.976, 0.991] |
| 4B Q8_0 | 0.791 | 0.719 [0.691, 0.747] | 0.924 [0.907, 0.940] | 0.826 [0.802, 0.849] |

(Baselines are the committed default cells, `H5H8_TABLE.md`, not re-run; the accuracy baselines are those quoted in the coordinator's notebook entry of 2026-11-17.)

**The pre-registered readings, applied as written (notebook, "step 2 of the 9B plan", copied at the foot of `VARIANTS_TABLE.md`).**

| reading | condition as fixed | holds? | on which cells |
|---|---|---|---|
| R1 | a variant takes 9B IQ4 lookup to ≥ 0.5 | **holds** | `tool_free` 0.732 (CI lower bound 0.704) |
| R2 | 9B ≤ 0.05 under all three **and** 4B IQ4 ≥ 0.5 under all three | **does not hold** | the 4B part holds (min 0.811); the 9B part fails: `tool_free` 0.732 |
| R3 | 4B IQ4 falls below 0.5 under some variant | **does not hold** | 4B IQ4 minimum is 0.811 (`tool_first`) |
| R4 | `tool_free` moves lookup and the others do not | **holds**, read on 9B IQ4 | `tool_free` 0.732; `tool_first` 0.000, `tool_describe` 0.000, baseline 0.001 |
| R5 | nothing moves the 9B but `search_first` does | **does not hold** | `tool_free` moves it |

Two judgments, stated so they can be disputed. (i) **R3 is read as written: 4B IQ4_XS.** The 4B Q8_0 cell under `tool_first` is 0.480 (CI [0.449, 0.510]), below 0.5, but that is a different checkpoint (Q8_0, baseline 0.574); R3 names IQ4_XS and was fixed with that scope, so it is not extended. The Q8_0 number is reported in the table, not turned into a reading. (ii) **R1 and R4 hold together** (both say the 9B's skip depends on wording; R4 adds that, among the three, only `tool_free` moves it). R4 is read on 9B IQ4_XS, the cell the variants were designed for. Read cell by cell, "moves" (the CI excludes the baseline) is also true for 9B Q8_0 `tool_first` (0.004 → 0.044) and for the 4B `tool_first` and `tool_describe`; those are small or in the other direction (the 4B IQ4_XS falls to 0.811 under `tool_first`, rises to 0.977 under `tool_describe`), and none reverses the 4B's behaviour.

**What the table says, and no more.**
- The 9B's skip is not one fixed property of the checkpoint: with the format sentence removed it looks up on 0.732 (IQ4_XS) and 0.763 (Q8_0) of the questions. "The default prompt" is therefore a wording, and H8 is reported as a range over four wordings of the 9B (0.000 to 0.732 at IQ4_XS). We make **no statement about size**.
- Of the three variants, only the one that removes the format sentence moves the 9B; ordering the sentences differently and describing the tool do not (IQ4_XS 0.000 and 0.000; Q8_0 0.044 and 0.001). This is a pattern among the three wordings run, in two quantisations of one model. We do not claim why: one intervention (deleting a sentence) is the only one that works, and we did not test other deletions or other wordings.
- `search_first` still looks up more on the 9B IQ4_XS (0.898, `E1_TABLE.md`) than any of the three variants (point estimates; the intervals were not compared).
- In all eight 4B cells (four wordings, two quantisations) the lookup rate is 0.480 or more, and at IQ4_XS 0.811 or more. The 4B's quantisation gap holds under every wording shown (IQ4_XS above Q8_0: 0.875 vs 0.574, 0.811 vs 0.480, 0.911 vs 0.762, 0.977 vs 0.602; the CIs do not overlap at any of the four). This is a **post-hoc description**: the draft of 2026-11-17 proposed two quantisation readings (A.Q1/A.Q2) that were never copied into the notebook before the data, and so they are dropped as readings (App. A).
- **`tool_free` accuracy is not comparable with the other cells'.** Without the short-answer sentence the answers are sentences: mean 50.6 completion tokens against 3.9 (9B IQ4_XS), and in the 268 answers without a lookup the mean is 25.3 words, exact-match is 0 and `contains` is 204 (0.761). `contains` rewards a longer answer that mentions the gold string, and we did not measure how often that happens by chance. In those 268, 142 fall in popularity bucket 1e5 (the most popular; 136 of the 142 contain the gold string), against 194 questions in that bucket overall: the model skips what it is likely to know, as in the 4B and the 27B above, but the 0.761 is not an estimate of what the 9B knows closed-book (0.350). What the cell does support is the lookup rate (0.732) and, among the 732 looked-up answers, 729 contain the gold string (0.996; exact 0 here too).
- Share of answers at the 128-token proxy limit is 0.000 to 0.012 in every cell (`VARIANTS_TABLE.md`), so truncation does not drive these rates; the proxy sums the completion tokens of all calls in an answer and so is imprecise.

**Not shown.** Any mechanism; any wording other than these three and `search_first`; any family other than Qwen3.5; sizes 0.8B, 2B and 27B under the variants (not run); anything about models with knowledge removed.

### 5.4 Forcing the lookup: the contract (H5)

**In words.** If we make the model look up, does it help, and does it cost accuracy? It helps a great deal where the model would have skipped the lookup (9B: 0.347 to 0.991). It does nothing at 0.8B, where the model never calls the tool even when nudged. Where the model already looks up, forcing gained about 1.7 points, and we could neither show that this is equivalent to zero (within two points) nor assert it as a difference after correction.

*Sources: `H5H8_TABLE.md`; `HOLM_TABLE.md`.*

- **Accuracy (answerable questions).** Without the contract, the default `tool` accuracy is 0.191 / 0.245 / 0.975 / 0.347 / 0.979 at 0.8 / 2 / 4 / 9 / 27B (IQ4_XS), which is the lookup rate's shadow; with the contract it is 0.190 / 0.374 / 0.991 / 0.991 / 0.996. The contract makes the 9B go from 0.347 to 0.991 (+0.644 [0.615, 0.674], 644 vs 0 discordant). At 0.8B it does nothing, because the model never calls the tool even when nudged (lookup rate 0.000). At 2B IQ4_XS it works for a fifth (0.212) of the questions only; at 2B Q8_0 it works for 0.958.
- **H5a (equivalence within ±2 points where the model already looks up): not shown.** Eligible sizes are those where the contract's lookup rate is ≥ 0.99 and the plain tool's is ≥ 0.8 (D12): IQ4_XS 4B (+0.016 [+0.009, +0.024], 16 vs 0 discordant, TOST p = 0.185) and 27B (+0.017 [+0.009, +0.025], 17 vs 0, TOST p = 0.26). Pooled IQ4_XS: +0.017 [+0.011, +0.022], TOST p = 0.135; Holm-adjusted 0.541 (0.676 before H2 had data). For Q8_0 **there is no eligible size**, so H5a cannot be computed there. In words: where the tool is already used, forcing it gained about 1.7 points, in the same direction at both sizes, and our data neither show equivalence within two points nor a difference we could assert after correction. Equivalence at this margin needs more power than 1,000 questions at these accuracies give.
- **H5b (hallucination on absent facts, ablated arm, prompt with the abstention clause).** The contract lowers the share of wrong answers at the sizes where the plain tool did not look up: 2B IQ4_XS −0.270 [−0.340, −0.200] (exact sign test p = 2.59e-13); 9B IQ4_XS −0.325 [−0.405, −0.245] (p = 9.04e-13); 9B Q8_0 −0.280 [−0.360, −0.200]; 2B Q8_0 −0.125 [−0.185, −0.065]. Where the plain tool already looked up (4B, 27B IQ4_XS) the difference is nil (−0.005 at both). Pooled over sizes: IQ4_XS 167 discordant, 21 contract-more vs 146 tool-more, exact p = 3.12e-24; Q8_0 146 discordant, 28 vs 118, p = 2.4e-14. **These pooled p-values treat the same 200 questions at five sizes as independent strata and are optimistic**; H5's Holm entry is the *maximum* of H5a and H5b (0.135 for IQ4_XS), which is why H5 does not survive correction (Holm 0.676).
- **What it means.** The contract's effect on hallucination is the effect of making a model that would not look up do so. Where the model looks up on its own, the contract changes nothing measurable. This is a result about prompt-elicited tool use (§5.3), not about a property the contract has by itself.
- The earlier E1 table (prompt `search_first`) shows the same at 2B IQ4_XS (accuracy 0.816 vs 0.959, +0.143 [+0.122, +0.166]); we report the default-prompt cells as the H5 test because D12 pre-registered them.

### 5.5 Web as the store

**In words.** Is a web search as good as a local store? No. At 0.8B it is 0.485 below the store and at 9B 0.338 below (`search_first` prompt), though above closed-book. The 27B web cell is invalid because the search credit ran out, so it is shown only on its 424 clean questions.

*Sources: `E1_TABLE.md`, `WEB_AUDIT.md`.*

- `web/search_first`, `contains`, n = 1,000: 0.8B 0.436 [0.405, 0.467]; 9B 0.642 [0.612, 0.671]. Against `store` the loss is −0.485 [−0.519, −0.451] at 0.8B and −0.338 [−0.368, −0.308] at 9B; against `closed` the gain is +0.223 [+0.191, +0.256] and +0.292 [+0.261, +0.324].
- **The 27B `web` cell is INVALID** (1,661 of 2,086 searches failed with a credit error). The first failed search was at question 446 in sample order; every one of the 527 questions searched from there on had at least one failed search. We report the 27B only on the **424 questions whose searches all succeeded**, labelled partial and not comparable to a full cell: 27B 0.748 against 9B 0.623 on the same 424 questions (`WEB_AUDIT.md`). We do not stitch search backends inside a cell.
- *Why web is below the store, measured, not explained.* When the gold object was in the excerpts the model saw, the answer was right 0.765 of the time at 0.8B (362 questions) and 0.761 at 9B (502 questions); when it was not, 0.000 and 0.054 (109 and 184 questions). The model's view cuts excerpts at 600 characters; the gold was in the full excerpts but cut away in 48 (0.8B) and 70 (9B) of the searched questions. The search tool's yield, not the model's reading, is the first limit. `[TBD: rerun at 2B/4B and the full 27B cell; needs the search budget, item 3 of Appendix B.3]`.
- For the record, E0 (distill, not part of the curve): `funes@3000` 0.912 vs `store` 0.992 on 858 unambiguous items (`E0_TABLE_unambiguous.md`).

### 5.6 Write-back (H6, exploratory, negative)

**In words.** If the agent saves its web answers, does it stop searching? No. H6 fails: in the second pass the model made 45% as many searches as in the first (the bar was 10%) and was still below plain `web`.

H6 was demoted to exploratory after E0 (`web+cache` pass 2 − `store` = −0.403; the model-driven cache never formed, four `remember` calls in 1,000). With a harness-side write-back at 9B IQ4_XS: pass 1 is **below** plain `web` by −0.219 [−0.249, −0.191] (25 vs 244 discordant); pass 2 is above pass 1 by +0.039 [+0.023, +0.055] but still below plain `web` by −0.180 [−0.208, −0.153]. In pass 1 the first tool called was `recall` on 892 of 1,000 questions and none on 108, and the model searched far less than in plain `web` (180 searched questions, 181 searches, against 686 and 686; `WEB_AUDIT.md`). The pre-registered criterion "pass-2 accuracy ≥ pass-1 − 2 points and pass-2 web calls ≤ 10% of pass 1" is met on accuracy (+0.039) but not on calls (81 searches in pass 2 against 181 in pass 1: 45%, not ≤ 10%), so **H6 fails**. Wrong answers are cached too (by design). The write-back arm confounds the tool list (it adds `recall`) with the cache; the control `web+recall-empty` that would separate them was never run (code not written; D15(2)) `[TBD: decide whether write-back stays in the paper; if it stays, the control and a capture of what `recall` returned are needed]`.

### 5.7 Fact-free reasoning (H2)

**In words.** Does adding the store change the size curve of a task that needs no facts? No. The slope ratio is 1.04 (CI 0.959 to 1.124), as a negative control should give. This supports reading the flattening in §5.1 as a property of facts and not of the harness (our reading, not a pre-registered claim).

*Sources: `results/GSM8K_TABLE.md` and `results/HOLM_TABLE.md`, as regenerated in commit e4432cd; design in `NOTEBOOK.md`, "H2 / GSM8K arm: design for implementation" (2026-11-18, written before any cell ran); run and reading in the entries of 2026-11-20. Cells `results/gsm8k-n500-s1-*.json`, 23 of 23 verified by `verify_gsm8k.py --with-tool`.*

**Design (frozen before the run).** 500 GSM8K test problems (1,319 in the file; `random.Random(1)`; the same 500 in every cell), greedy, one repeat. Two conditions with one system prompt: `closed` (the problem only) and `store` (the PopQA `store` condition unchanged: five facts from the 14,267-triple PopQA store, none of which a word problem needs, then the problem). The reply is visible step-by-step reasoning (reasoning budget 0, so no hidden channel), `max_tokens` 512. Primary metric: the last `#### N` line, else the last number in the reply, numerically equal to gold; a reply with no number is wrong. A strict-only accuracy (no fallback) is reported beside it. Pilot (20 problems outside the sample, `closed`): 0 of 20 at the limit on the 9B IQ4_XS, so 512 stayed (notebook, 2026-11-19). The IQ4_XS curve is 0.8, 2, 4, 9, 27B; the Q8_0 curve 0.8, 2, 4, 9B.

| `contains`-equivalent accuracy (`correct`), 500 problems | 0.8B | 2B | 4B | 9B | 27B |
|---|---|---|---|---|---|
| IQ4_XS `closed` | 0.436 | 0.726 | 0.910 | 0.934 | 0.952 |
| IQ4_XS `store` | 0.412 | 0.690 | 0.884 | 0.922 | 0.940 |
| Q8_0 `closed` | 0.534 | 0.710 | 0.922 | 0.932 | – |
| Q8_0 `store` | 0.506 | 0.682 | 0.898 | 0.924 | – |

(95% intervals per cell are in `GSM8K_TABLE.md`; they are about ±0.04 at 0.8B and ±0.02 at 27B.)

**H2, by the pre-registered rule: confirmed on both quantisations.** The slope of accuracy on log10(parameters in B) is 0.326 [0.299, 0.354] for `closed` and 0.339 [0.310, 0.367] for `store` on IQ4_XS; the ratio ρ = store/closed is **1.04 [0.959, 1.124]**. On Q8_0 the slopes are 0.406 [0.365, 0.449] and 0.424 [0.380, 0.469], ρ = **1.04 [0.937, 1.164]**. The rule was: confirmed if the CI lower bound of ρ is ≥ 0.5, refuted if the upper bound is < 0.5. Both lower bounds are above 0.5 and also contain 1, so the original wording "store slope ≈ closed slope" is also met. The raw one-sided p (H0: store slope − ½·closed slope ≤ 0) is at the bootstrap's resolution, ≤ 1e-4, per quantisation; Holm-adjusted (m = 6, `HOLM_TABLE.md`) 0.0006 on both.

**What this is and is not.** `store` here is `closed` plus five irrelevant facts, so a ratio near 1 is what one expects if the store does nothing for reasoning. **Our reading (not a pre-registered claim): H2 works as a negative control for H1.** It shows that the flattening of the size curve on PopQA (§5.1, ratio 0.27) is not something the harness would produce on any task: where the facts are not what limits the answer, the store does not flatten the curve. It is not a discovery about reasoning. In particular we do not claim that a store helps or does not help reasoning; the store here is irrelevant by construction.

- **Ceiling.** The 27B `closed` is 0.952, above the 0.95 flag in the design. Registered rule unchanged; exploratory sensitivity (`GSM8K_TABLE.md`): without the 27B, ρ = 1.03 [0.937, 1.123], p ≤ 1e-4, same verdict. Accuracy is 0.91–0.95 from the 4B up, so on this sample the size gain on reasoning is mostly below 4B (`closed` 4B → 27B: +0.042); the plan's expectation that "size still helps" holds mainly there. Q8_0 has no 27B cell.
- **Format drift (2B Q8_0 `closed`).** Strict 0.74, fallback 0.22, none 0.04; primary accuracy 0.710, strict-only 0.572 (flagged "fallback-heavy" in the table; 2B Q8_0 `store` is 0.18, under the 0.20 threshold). The fallback answers are right 69 of 108 against 286 of 371 for the strict ones, so the last-number rule recovers most of them at some cost in noise (notebook, 2026-11-20; computed from the cell files, not in the table). On Q8_0 with strict-only scoring ρ = 1.03 [0.93, 1.14]: same verdict (notebook, same entry, own bootstrap, not `stats_gsm8k.py`). `[UNVERIFIED: how many fallback answers took a number that was not the answer]`.
- **The store lowers accuracy slightly (post-hoc, descriptive).** `store` is below `closed` at all nine size × quantisation pairs, by 0.008 to 0.036 (per-size paired intervals in the table; several touch 0, the 4B Q8_0 one, [−0.046, −0.004], does not). Pooled over the nine pairs there are 359 problems solved only by `closed` and 260 only by `store`; the pairs are not independent (same 500 problems, both quantisations) and we give no p-value. Reading: irrelevant retrieved context costs a point or two at one repeat; the gains of §5.1 are net of that cost. It is not part of H2's rule.
- **`tool` stage (exploratory, post-hoc; the five IQ4_XS sizes were the code session's choice, not the design's).** Accuracy 0.504, 0.710, 0.902, 0.942, 0.950 for 0.8 to 27B. The lookup rate was read afterwards: 8 of 500 answers at 0.8B, 2 at 2B and none at 4B, 9B and 27B (notebook, 2026-11-20). So the 0.8B `tool` cell above its `closed` cell (0.504 vs 0.436) is not retrieval; it is `closed` under a different system prompt with a tool schema in context (mean completion tokens 241 vs 147; more answers with no number, 33 vs 15). The cause is not measured. No hypothesis is attached to this stage and it enters no test.
- **Not shown.** Reasoning tasks other than GSM8K (`[TBD: BBH or another suite, if wanted]`); other prompts (one frozen prompt, no variants as in §5.3); anything about a store that holds relevant facts for a reasoning task; n other than 500 (cost; each cell's standard error is at most 2.2 points); more than one repeat.

### 5.8 The deployed agent: the `funes` rows

**In words.** Does the deployed agent do as well as the controlled harness on the same questions? Not at 0.8B and 2B, where it is 0.285 and 0.256 below the plain store. From 4B up its point estimates are within 0.05 of it. At 0.8B its runs fetched outside content on 0.181 of the questions, against at most 0.007 from 4B up.

*Source: `results/FUNES_TABLE.md`, produced by `stats_funes.py` from the five cells `popqa-stratified1u-n1000-qwen3.5-<size>-iq4_xs-funes-pool14267-r0.json` (run 2026-11-18, verified 2026-11-19; the script itself refuses to run unless each cell has the same 1,000 ids as its `store` cell, a sha256-verified planned model, no adapter and a 14,267-triple pool; notebook 2026-11-22).*

The same 1,000 questions go through the agent's own loop (recall from its memory, then its tool loop, safety layers and persistence, as deployed) instead of through the harness. Differences are paired against the `store` cell of the same size, 95% bootstrap over questions, 10,000 resamples, seed 0. **Descriptive and outside the Holm family** (decision D1: one repeat, no hypothesis).

| size | `store` | `funes` | `funes` − `store` [95% CI] | only `store` right | only `funes` right | errors | answers with any tool call | with `recall` | with `web_search` | taint rate |
|---|---|---|---|---|---|---|---|---|---|---|
| 0.8B | 0.921 | 0.636 | −0.285 [−0.318, −0.253] | 320 | 35 | 4 | 0.281 | 92 | 179 | 0.181 |
| 2B | 0.971 | 0.715 | −0.256 [−0.286, −0.227] | 272 | 16 | 11 | 0.372 | 301 | 78 | 0.078 |
| 4B | 0.984 | 0.967 | −0.017 [−0.030, −0.004] | 32 | 15 | 0 | 0.508 | 501 | 6 | 0.006 |
| 9B | 0.980 | 0.931 | −0.049 [−0.067, −0.032] | 66 | 17 | 1 | 0.068 | 62 | 7 | 0.007 |
| 27B | 0.991 | 0.985 | −0.006 [−0.015, +0.003] | 13 | 7 | 1 | 0.020 | 15 | 6 | 0.006 |

*Any tool call* is the harness's `lookup_rate` field computed on the agent's tool list (every tool, not only its memory). *Taint rate* is the harness's own flag (`ko/funes.py`): the run called a tool that brings outside content into the context (`web_search`, `web_fetch` or `delegate_to_agent`). *Errors* are counted wrong, not re-run: 17 of the 5,000 answers were context-overflow refusals (0.8B 4, 2B 11, 4B 0, 9B 1, 27B 1). The table file also lists accuracy by popularity bucket (1e1–1e5) and wall time per cell (2,334 s at 0.8B to 13,863 s at 27B).

What the table says, and no more.
- **The deployed loop is not the controlled harness.** At 0.8B and 2B it is 0.285 and 0.256 below the plain store; from 4B up it is within 0.05 (4B −0.017, 9B −0.049, 27B −0.006, the last interval including zero). Which part of the loop accounts for the gap (its own prompt, its recall step, the tools it exposes) was not separated; `funes` is one bundle in every cell here. We make no causal claim.
- **The share of answers with any tool call does not rise with size either** (0.281, 0.372, 0.508, 0.068, 0.020). This is any tool, not only the memory lookup: the agent's tools are its own (`recall`, `web_search`, and in a few answers `remember`, `machine_recall`, `incidents`, `host_changes`, file tools). At 0.8B, 179 of the 281 answers with a tool call used `web_search`, which is the source of that size's taint rate; from 4B up `recall` is the main tool (501, 62 and 15 answers at 4B, 9B, 27B). The 9B and 27B rarely call any tool and the 27B still answers well, while the 9B loses 0.049 to the store. How much of `funes`'s accuracy comes from its own recall step before the tool loop (the notebook of 2026-11-15 says recall is injected automatically) was not measured here.
- **Outside content reaches the small models' runs.** On the questions of a closed, local PopQA store, `funes` at 0.8B ran a web search, a page fetch or a delegation on 0.181 of them (179 `web_search` answers and 2 `web_fetch`) and at 2B on 0.078; from 4B up on 0.006 or 0.007. Each such run is the case the effects gate in the agent exists for (`CLAUDE.md`, layer 10); this table counts it and does not say whether the gate held anything. This is the security-cost number the plan promised, for the questions of this set only.
- **The sample is the controlled sample.** These are PopQA questions on a store that contains their answers; they do not measure the agent on messy personal memory (E4 was not built, §7).
- `[UNVERIFIED: the sampling temperature of these runs (0.2 by source, 0.0 in the cell meta, §4.2)]`. One repeat per size; the varying part is not measured.
- E0, on a different (distilled) 9B kept out of every curve, had `funes@3000` at 0.912 against `store` 0.992 (§5.5); it is a different checkpoint and is not comparable to the 9B row above.

### 5.9 Conflicts and edits (H3) and knowing when to look up (E3): not run

**In words.** These two experiments were not run, so the paper has nothing to say about them.
`[TBD: E2: context-faithfulness and parametric override per size, logistic regression of override on log(params); supersede versus contradict in Funes]`. `[TBD: E3: lookup rate per kind of question (answerable, weights-only, unanswerable), abstention, accuracy when the store is wrong or noisy]`. Neither exists in code or data; they are listed in Appendix B.3 for the author to decide. What is measured today about abstention is only §5.4's ablated arm. This paper makes **no claim about H3** (it is not in this paper's family).

## 6. Cost

**The question and the short answer.** What did it cost? Latency and the taint rate are measured. VRAM and energy are not.

What is measured, per condition, on the machine of §3.6.
- **Latency** (wall time per cell, `results/*.json` `wall_s`): baseline `tool` cells 77–2,703 s and `tool/search_first` cells 125–2,946 s (`search_first` is slower where it looks up; `H5H8_TABLE.md` and `VARIANTS_TABLE.md` for the rest); the 36-cell default-prompt job took 7.3 h of wall time and 4.91 h of cell time (§3.6). The `funes` cells took 2,334 s (0.8B), 2,254 s (2B), 3,368 s (4B), 4,206 s (9B) and 13,863 s (27B) for 1,000 questions each (`FUNES_TABLE.md`), i.e. 2.3 to 13.9 s per question, against 72, 93, 140, 166 and 447 s for the harness's `store` cells of §5.1 (`wall_s` of the five committed cells, 0.8B to 27B), i.e. 0.07 to 0.45 s per question.
- **Safety-related counts.** Taint rate (share of questions on which the agent's run fetched outside content): 0.181, 0.078, 0.006, 0.007, 0.006 at 0.8, 2, 4, 9, 27B (§5.8). Web: USD 20 of search credit was consumed and the 27B cell ran out of it (§4.6, §5.5); cost per search not measured.
- **Not measured:** **VRAM per condition** (the harness does not record it); the cost of the gate's held proposals; the energy cost. `[TBD: VRAM capture, item 11 of Appendix B.3]`. The plan's contribution sentence promised VRAM, latency, privacy and safety-gating costs; at the time of writing latency and the taint rate are measured, the rest is not.

## 7. Limitations

**The question and the short answer.** What should a reader discount? Everything below. The largest are one family, one machine, one repeat, and a templated question set on which the store is at ceiling by construction.

Written early on purpose; each is a thing a reader should discount.

1. **One GPU, one machine.** All cells ran on one consumer card shared with a production model, in windows. Wall times are specific to it.
2. **One model family.** Qwen3.5 only. Findings about size are findings about this family's five checkpoints, not about "size". The 4B/9B contrast in lookup behaviour is the clearest sign that checkpoint-specific behaviour dominates.
3. **Two quantisations, and one is partial.** IQ4_XS on every size; Q8_0 as a control on four sizes. Quantisation changed lookup rates materially (§5.3), so size and quantisation are not independent in any cross-size statement.
4. **Greedy, one repeat.** Results are deterministic given the server; intervals are over questions only. We did not measure variance across servers, drivers or sampling.
5. **PopQA is templated.** The questions are generated from the same triples that populate the store, so `store` and `oracle` are at ceiling by construction (retrieval is trivial, and the store contains exactly the facts the questions ask). The H1 slope ratio is therefore about *supplying* facts, not about *finding* them in a messy store. The messy-store case is what the deployed agent has and what E4 would measure; it was not run, and the `funes` rows (§5.8) use the same templated store.
6. **The metric is lenient.** `contains` credits longer answers that include the gold; hedging (a list of candidates) is a rare phenomenon on unambiguous items but not zero. The abstention regex agrees 49/50 with one labeller.
7. **The lookup tool is simple.** One tool, one schema, a local BM25 store. Different tool descriptions and schemas would change §5.3; the variants measure only prompt text.
8. **The harness is not the deployed agent, and the deployed agent was run once.** The `funes` rows (§5.8) exercise the agent's own loop on the five checkpoints, one repeat each, on a store that holds the answers; their sampling temperature is `[UNVERIFIED]`; and they do not measure the agent on its real, messy memory.
9. **Search backend split.** Parallel only for all valid web cells; Tavily is reserved for production. The 27B web cell is partial (424 clean questions), and no web cell exists at 2B or 4B. Parallel's excerpt quality is a confound for every web number.
10. **Post-hoc elements.** The sample, the metric hierarchy (D5), H5's reframing (D12–D14) and H8's reframing were decided after E0 and, for H5/H8, after E1; each is dated in the notebook. The Holm family fixes m = 6 with three empty entries (H3, H4, H7) as p = 1; **the cut into two papers was decided on 2026-11-22, after the data, and this paper's own family (m = 3) is reported next to the pre-registered one (§4.3)**.
11. **Hash evidence.** The GGUF files are tied to the repositories by their LFS sha256 and verified by the runner; the template hashes were read from the file headers. We did not check the llama-server build's tool-call parsing across sizes beyond using the same binary.

12. **Not run, and not claimed.** Conflicts between the store and the weights (H3/E2), when to look up (E3), a messy store (E4), a second model family, and anything about changing the model or about a memory budget. Their absence is a limit of this paper's scope, not a negative result.

## 8. Reproducibility

**Released with the paper:** the harness (`benchmarks/knowledge-offload`: `run.py`, `ko/`, the statistics scripts `stats_e0.py`, `stats_e1.py`, `stats_holm.py`, `stats_h5h8.py`, `stats_variants.py`, `stats_gsm8k.py`, `stats_funes.py`, `web_payload_audit.py`, `label_abstain.py`), `models.lock` (repo, file, size, sha256, quant, lineage for every model), the per-question result JSON for every committed cell (`results/popqa-stratified1u-*.json`; each records model, quant, commit, seed, date, endpoint), the 50 hand labels, and every table. A figure can be rebuilt from the JSON without a GPU. The test suite of the harness is hermetic (no model, no network); 141 tests passed at the variants commit (notebook) and 277 on 2026-11-22 (notebook; includes the tests of paper 2's code).

**Not released with the paper:** the raw web search payloads (`results/payloads/`, gitignored and held on the benchmark machine only: they contain third-party page text, and the web numbers cannot be re-scored without them). `[TBD: decide what is released; either a scrubbed derivative or the claim "web numbers are not re-scorable from the release"]`. No data from the production memory pool is ever used or released.

**How a run is made:** one script per size, `scripts/ko_swap.py`, stops the production model, starts the named size with identical flags, checks the file's sha256 against `models.lock` before stopping production, runs the cells (resumable), and restores the production model in a `finally` block; a run refuses to start in a cron window and ends 17 minutes before the next one. **Swap windows** are recorded in `results/queue*.log` and `ko_swap-*.log`.

**What the paper's numbers depend on and was not independently verified (marked):**
- `[UNVERIFIED]` GPU **power cap of 119 W** and the thermal state during runs: the coordinator mentioned a 119 W cap in an instruction for this draft, but no committed file in the repository states it. Not reported as a fact until a log (`rocm-smi`) is committed. `[TBD: coordinator reads rocm-smi power cap and driver/ROCm/llama.cpp build versions on the benchmark machine and records them in the notebook; item 10 of Appendix B.3]`.
- llama.cpp build hash, ROCm version, kernel: `[TBD: not recorded in any committed table; the ko_swap logs may contain the build line, to be read]`.
- Determinism: asserted from E0 (0 of 1,000, five conditions) and the E1 pilot (0 of 100 differ, three conditions, 9B IQ4_XS); not re-checked for every cell.

**Exact file identity (moved from §3.2).** The 27B Q8_0 file is 28,595,763,104 bytes. The chat template of all eight 0.8 to 9B files is byte-identical (7,816 characters); the 27B file's template differs by one character (7,817) (notebook, coordinator entry "template route (i)"). The template hashes and the hash of every source table are in Appendix D.

---

## Appendix A. Conclusions pre-written for the variants (written 2026-11-17, before any variant result existed; kept and deleted on 2026-11-18)

*Purpose: so that the text cannot be bent to the result. The readings are R1–R5 as fixed in `NOTEBOOK.md` ("step 2 of the 9B plan"); reading aids with intervals, not tests, outside Holm. `VARIANTS_TABLE.md` (sha256 `20aa71c5…`) was then read; §5.3 applies each reading as written. The two branches whose condition holds (A.R1 and A.R4, which are compatible) are kept below with the numbers filled in; the other branches are deleted and each deletion is logged in `NOTEBOOK.md` (entry of 2026-11-18, Opus) with the cells that decided it.*

**A.R1: a variant takes the 9B IQ4_XS lookup to ≥ 0.5. (Holds: `tool_free`, 0.732 [0.704, 0.759].)**
> "The 9B's lookup skip is a property of one wording, not of the checkpoint: under `tool_free` it looked up 0.732 of questions (95% CI 0.704 to 0.759), against 0.001 under the default prompt. The default prompt therefore is not one prompt but a family, and the lookup propensity of a model under 'the default prompt' is a range over wordings. We report H8 as a range over four wordings (the default and the three variants) and make no statement about size."
→ §5.3 reports the range; the abstract carries one sentence on it.

**A.R4: only `tool_free` moves lookup (read on 9B IQ4_XS). (Holds: `tool_free` 0.732; `tool_first` 0.000; `tool_describe` 0.000; baseline 0.001.)**
> "Of three changes to the default prompt, only removing the short-answer sentence moved the 9B IQ4_XS (to 0.732); reordering the sentences and describing the tool did not (0.000, 0.000). The same pattern holds for the 9B Q8_0 (0.763; 0.044; 0.001). Whether the sentence suppresses the lookup by itself, or in some interaction with the rest of the prompt, was not tested; neither was whether `search_first` works because it replaces that sentence `[UNVERIFIED: not tested]`. Answers under `tool_free` are sentences (mean 50.6 completion tokens against 3.9), so `contains` accuracy under it is not comparable with the other cells (exact-match is 0 there); the share of answers at the 128-token proxy limit is 0.012 or less in every cell."
→ the paper does not state a mechanism. (The pre-registered reading R4 says "the format sentence is the mechanism"; the paper words it more weakly than the notebook, because the three variants contain one deletion and no test of why it works. See the notebook entry of 2026-11-18.)

**Deleted, logged in the notebook:** A.R2 (9B part fails), A.R3 (4B IQ4_XS minimum 0.811), A.R5 (`tool_free` moves the 9B), A.None (R1 and R4 apply), and the proposed readings A.Q1/A.Q2 (never in the frozen notebook entry; the quantisation comparison is reported in §5.3 as post-hoc description, with the cells shown).

**What none of the branches may say** (fixed in the notebook): any mechanism beyond the wording tested; any other wording, family or size not run; and anything about knowledge-removed models.


---

## Appendix B. Drafting notes (remove before submission)

### B.1 Status of the sections

| § | section | status |
|---|---|---|
| Abstract, 1 | | drafted; H3/E2 is out of scope here (see 0.2) |
| 2 | Related work | rewritten after the full-text audit (B.4) |
| 3, 4 | Setup, methods | drafted in full; `funes` rows now described |
| 4.7, App. C | How the work was organised | drafted; one TBD (the coordinator's effort level); moved to Appendix C on 2026-11-24 |
| 5.1–5.7 | H1, store cost, H8 and variants, H5, web, write-back, H2 | drafted with measured numbers |
| 5.8 | The deployed agent (`funes` rows) | **new, 2026-11-22**: five cells, `FUNES_TABLE.md` |
| 6–8 | Cost, limitations, reproducibility | drafted; VRAM is the one unmeasured cost |

### B.2 Decisions of scope that are julio's, and where they stand
- **The cut into two papers** was decided on 2026-11-22. Consequence for the statistics, stated in §4.3: the pre-registered family is m = 6 (H1, H2, H3, H4, H5, H7); this paper corrects over the hypotheses it tests, which changes the family after seeing data, and therefore reports **both** adjusted p-values.
- **E2/H3 (conflicts) and E3 (when to look up)** were not run. This paper takes them as `[TBD]` and lists them as out of scope, not as results. Whether they are run before submission is julio's decision; this draft does not decide it. If they are run, H3 enters this paper's family (m = 4) and §5.9 is filled.
- **H7 (does it fit a smaller model) is not promised here.** The title's original question (how much model does a local *agent* need) is paper 2's.

### B.3 What must be measured or done before submission, and who
| # | item | blocks | who | state |
|---|---|---|---|---|
| 1 | E2 (conflicts) and E3 | §5.9, H3 | julio decides; Sonnet code; coordinator GPU | **not run, not designed in code**; estimated 2–4 h and 4–8 h of GPU (notebook, 2026-11-22; estimates) |
| 2 | Stage C (27B) / D (2B) of the prompt variants | §5.3 | coordinator | not run; the rules R3/R5 did not require it |
| 3 | 27B `web` full cell, 2B/4B `web`, control `web+recall-empty` | §5.5, §5.6 | **needs a written search budget approved by julio** | cost per search not measured; about 2,100 searches for the 27B (2.2 per searched question, measured) |
| 4 | Second labeller and more than 50 labels for the abstention regex | §4.4 | julio | 50 labels exist |
| 5 | A bootstrap with 20,000 resamples that also resamples the dependence between sizes (H1) | §5.1 | Sonnet | no GPU |
| 6 | Request-level capture of the tools block as served | §5.3 mechanism wording | coordinator | not measured |
| 7 | Temperature actually used by the `funes` rows (0.2 by source, 0.0 in the cell meta) | §4.2, §5.8 | coordinator | `[UNVERIFIED]` |
| 8 | Run-to-run noise outside greedy; a second server or machine | §7 | coordinator | not measured |
| 9 | A second model family | §7 | julio decides | not measured |
| 10 | Environment record: llama.cpp build, ROCm, kernel, power cap (the 119 W mentioned in an instruction is in no committed file) | §8 | coordinator (read-only `rocm-smi`) | `[UNVERIFIED]` |
| 11 | VRAM per condition | §6 | coordinator | not recorded by the harness |
| 12 | Per-question web payloads: what is released | §8 | julio | held on the benchmark machine only |

### B.4 Citations
Audit of 2026-11-22 (`benchmarks/knowledge-offload/CITATION_AUDIT.md`). All 20 papers named in §2 were downloaded as arXiv PDFs and converted to text (`curl` and `pdftotext`; no search engine, no Tavily). KBLaM (2410.10450) and LMLM (2505.15962) were also read through the alphaXiv report. For each paper the title block, abstract and introduction were read in full, and the method, results and limitation passages that bear on our sentence were read in context. Not every page of every paper was read. Sentences that did not match their source were rewritten (Co-LMLM, Knowledgeless, Mixture-of-Memories, Mobius, Jin et al., Hong et al., MemoReason, the adaptive-retrieval attribution to PopQA); the "Self-RAG-style" mention was removed because that paper was never read. Yang, Gao and Wu (2507.18178), previously not cited, was read and is now cited. AtlasKV was not found and is not cited `[UNVERIFIED: AtlasKV, no id, no source read]`. The novelty statement stays `[UNVERIFIED]` because no literature search was made.


### B.5 Index of markers

Every marker in the body, with the section it sits in (marker text shortened, brackets removed so that a grep counts only the real ones). Counts by grep of the whole file, 2026-11-24: 13 TBD, 0 OPEN, 12 UNVERIFIED; they include the mentions in the status and to-do tables of this appendix.

- 2. Related work: UNVERIFIED: no literature search beyond the papers above was made; web search is not permitted in this track
- 3.5 Funes as the testbed: TBD: a diagram of the two paths; not drawn
- 4.2 Paired design and uncertainty: UNVERIFIED: that the served `funes` runs actually used 0.2: read in source, not in a request log
- 4.4 The abstention outcome: TBD: second labeller, 200 answers, item 4 of Appendix B.3
- 5.2 What the store costs: reading capacity: TBD: E4 store shape, only if a messier store is wanted; no evidence that retrieval is the limit
- 5.5 Web as the store: TBD: rerun at 2B/4B and the full 27B cell; needs the search budget, item 3 of Appendix B.3
- 5.6 Write-back (H6, exploratory, negative): TBD: decide whether write-back stays in the paper; if it stays, the control and a capture of what `recall` returned are needed
- 5.7 Fact-free reasoning (H2): UNVERIFIED: how many fallback answers took a number that was not the answer
- 5.7 Fact-free reasoning (H2): TBD: BBH or another suite, if wanted
- 5.8 The deployed agent: the funes rows: UNVERIFIED: the sampling temperature of these runs (0.2 by source, 0.0 in the cell meta, §4.2)
- 5.9 Conflicts and edits (H3) and knowing when to l: TBD: E2: context-faithfulness and parametric override per size, logistic regression of override on log(params); supersede versus contradict in Funes
- 5.9 Conflicts and edits (H3) and knowing when to l: TBD: E3: lookup rate per kind of question (answerable, weights-only, unanswerable), abstention, accuracy when the store is wrong or noisy
- 6. Cost: TBD: VRAM capture, item 11 of Appendix B.3
- 7. Limitations: UNVERIFIED
- 8. Reproducibility: TBD: decide what is released; either a scrubbed derivative or the claim "web numbers are not re-scorable from the release"
- 8. Reproducibility: UNVERIFIED
- 8. Reproducibility: TBD: coordinator reads rocm-smi power cap and driver/ROCm/llama.cpp build versions on the benchmark machine and records them in the notebook; item 10 of Appendi
- 8. Reproducibility: TBD: not recorded in any committed table; the ko_swap logs may contain the build line, to be read
- Appendix A. Conclusions pre-written for the varian: UNVERIFIED: not tested
- B.2 Decisions of scope that are julio's, and where: TBD
- B.3 What must be measured or done before submissio: UNVERIFIED
- B.3 What must be measured or done before submissio: UNVERIFIED
- B.4 Citations: UNVERIFIED: AtlasKV, no id, no source read
- B.4 Citations: UNVERIFIED


## Appendix C. How the work was organised: sessions, roles, rules and files

*Moved from §4.7 on 2026-11-24. It describes the protocol so that it can be repeated and criticised. Terms: the *coordinator* is the author's main session; *design / audit* is the Opus session that wrote pre-registrations and checked the others; *code and statistics* is the Sonnet session that wrote the harness.*

The experiments were designed, coded, run and analysed by a set of language-model sessions (Claude Code) working under one human, the author, with **no shared chat context**: a session knew only what was in the repository and in the brief it was given. This subsection describes the protocol so that it can be repeated and criticised. Every statement cites a file, a commit or a notebook entry on branch `feat/knowledge-offload-e0`; nothing here comes from memory. Evidence is the notebook (`benchmarks/knowledge-offload/NOTEBOOK.md`, 2026-11-15 to 2026-11-17), `git log` (55 commits touching `benchmarks/knowledge-offload` or `docs/knowledge-offload-plan.md` at the time this subsection was written; later commits, including the whole of E5, are not counted here) and the plan.

**Roles (plan, "Which Claude for which part"; notebook "Who does what").**

| role | model · effort | what it did on the branch | forbidden |
|---|---|---|---|
| coordinator (the author's main thread) | model-driven session Sonnet 5.5 (as reported by the coordinator on 2026-11-17, message to Opus; not recorded in a committed file) · medium (effort stated by the author on 2026-11-17, in chat; not recorded in a committed file) | assigns tasks, starts and stops sessions, launches GPU jobs in a window the author chooses, writes briefs, relays messages (e.g. the template read, the variants queue decision, entries headed "coordinator") | doing another role's task silently |
| design / audit | Opus 5.5 · high | pre-registrations, decisions D5–D16, the lineage error report, the audit of all sessions, the 9B variant design; **no code, no GPU** | code, models, yoda |
| code and statistics | Sonnet 5.5 · medium | harness (`run.py`, `ko/`), `ko_swap.py`, `models.lock` gate, stats scripts, queue and verifier scripts, tests (hermetic, 141 passing at the variants commit) | touching yoda, taking a task assigned to another |
| early review | Fable 5.1 · high | `DECISION_BRIEF.md` and decisions D1–D4 (entry 2026-11-15 "pre-E1 decisions") | code |
| GPU | **no model** | only a job launched through `scripts/ko_swap.py` in a window the author chose; the script refuses a second swap, refuses a cron window, and restores the production model in a `finally` block | a session starting, stopping or swapping a model on its own |
| the human | the author | scope and budget decisions (Parallel credit, Tavily reserved for production, whether knowledge-removed models enter), approval of the three variant wordings, **the 50 hand labels** (`results/abstain_labels.tsv`, dc5d669), the windows | – |

Why the split: the plan assigns the model to the part where an error is expensive (design, where "a reasoning mistake costs weeks") and keeps the part that must be reproducible free of a model ("a run that needs a model to babysit it is a run that isn't reproducible"). The rule that only the coordinator touches yoda exists because the GPU is shared with the production model and a swap has side effects (probes paused, guardian timer stopped, both to be undone: notebook "yoda run state").

**Rules of work (notebook, section "Rules of work", the author, commit 017185e, 2026-11-17 09:20).**
1. *Golden rule: nobody assumes anything in order to move on; everything is measured.* A number in a decision, report or commit message has a source the writer ran; "done" is checked against the real state, never against another session's report; estimates stay marked "estimated" until a run replaces them; if a measurement is impossible now, say so and stop on that point.
2. One task per session; tasks are assigned by the coordinator; a session that finds a task done or open reports it and stops.
3. One branch. `git pull --rebase origin feat/knowledge-offload-e0` before every commit, push right after, never force, commit only your own files; results JSON and tables are committed, `results/payloads/`, `*pilot*`, `*smoke*` are not.
4. Each session reads the notebook's "Now" block and its last three entries at the start, and at the end updates "Now" and adds a dated entry containing *what was measured (with the command)*, *what was claimed but not measured*, *what it leaves open*.
(Before the rules were written the practice was informal; two of the failures in the list below predate them.)

**Channels: what carried information between sessions, and what did not.**
- **The plan** (`docs/knowledge-offload-plan.md`): hypotheses, experiments, rules; its commit date is the pre-registration. Edited by the design role and by a code session only "to record a result".
- **The notebook**: a "Now" block (state, what is not run, what is invalid, pending decisions) and dated entries, each with a header *date · step · topic · role · model · effort*; 28 entries to 2026-11-17. Decisions carry identifiers **D1–D16** (D1–D4 Fable, D5–D8 and D9/D10 Opus, D11–D16 Opus after E1; D9 was superseded by D10 the same day and says so; the last decision on the branch is D16, confirmed by the coordinator). Code sessions end their entry with "Open for Opus"; the design role answers those items in its own entry (audit entry, "Answers to Sonnet's 'Open for Opus'"), which is how a question crossed from one session to another without a shared context.
- **Results as files**: one JSON per cell (`results/popqa-stratified1u-*.json`: 128 committed when this subsection was written, 161 on 2026-11-22 with the `funes` and E5 cells), recording model id, quantisation, commit, seed, date and endpoint; tables that list the **sha256 of every input file** (`H5H8_TABLE.md`, "Input files (sha256)"). Tables are regenerated by a script from the JSON, never typed.
- **`models.lock`** (repo, file, size, sha256, quant, lineage per model) and the runner gate that refuses a served file whose hash is not in it (commit 3cd759b): the model's identity is a file, not a name.
- **Queue scripts and logs** (`results/queue*.sh`, `queue*.log`, `ko_swap-*.log`): what was run, in which window, with which retries; scripts are resumable, take a `DEADLINE`, and have a `DRY=1` mode.
- **`verify_*.py` as the gate between a run and its analysis** (`verify_h5h8.py`: 36 of 36 cells before `stats_h5h8.py` was run; `verify_variants.py`: exit 0 only with 12 of 12). A missing cell prints "missing", never a number.
- **Briefs and messages**: a session received a brief at start and, between sessions, cross-session messages. **Neither is on the branch.** What survives of them is their effect in a notebook entry; consequently, anything a session needed had to be written into a file, and anything only said in a message was lost to the next reader. `DECISION_BRIEF.md` is the one brief-style document that is committed.
- **Not shared**: the chat context of every session. An audit session could not read others' tool arguments; it used the notebook, commits, `git status` and the result JSON as evidence (audit entry, "Sources").

**What went wrong, and what the files caught or repaired.** Counted from the notebook; none softened.
1. **Wrong model lineage (E0).** E0 ran on a community distillation labelled "Qwen3.8 9B" in the plan, tags and notebook; about 6 of about 12 GPU hours were wasted (about 2 h of deterministic repeats and about 3 h of `funes` repeats that D1 had already judged unnecessary were avoidable regardless; the rest is the error itself). It **was not caught by the protocol**: `--expect-model 9B` compared a name, no result recorded a hash, and the review sessions "reasoned from the harness's labels". It surfaced because the author asked what to download. Repair: `models.lock`, a sha256 gate, hash/repo/lineage in every cell's metadata, "one curve = one lineage = one quant producer", and a rule that the first review item for any model is "what is this file" (notebook "ERROR REPORT", 2026-11-16; 3cd759b). One more consequence of the same assumption: the design role's own D9 repeated it for a day before D10 superseded it.
2. **A circular check reported as validation.** The abstain regex was reported as agreeing 50 of 50 with labels (01b461a, 2026-11-17 08:55); the labels were not independent. Withdrawn 26 minutes later (9e20a44, "human labels still empty"); the author hand-labelled 50 answers: 49 of 50 agree (dc5d669). The earlier E1 table carried the withdrawn number for that interval.
3. **A silently invalid cell.** The 27B `web` cell ran on an exhausted search account: 1,661 of 2,086 searches returned a 402 error that the harness's `tool_loop` turned into tool text, so no error was recorded. An E1 notebook entry first reported the 27B's lower `web` accuracy as a finding ("not monotone in size"), with the caveat that it needed the payloads read. A payload audit (`web_payload_audit.py`, 5a940c0) found the failures; the harness was changed to stop a cell on any failed search (exit 4; ed0ac4f) and the cell is flagged INVALID in every table.
4. **Statements not repaired at their original line.** The audit found four contradictions fixed only in later entries, and a "Now" block that still described the default-prompt cell as "not run" after D14 (audit entry, (b) and (c)); the notebook is append-only by convention, so a reader must read to the end.
5. **Rule slips, disclosed by the sessions themselves:** (a) a code session ran `git fetch` in the yoda checkout, refs only, and said so; (b) the design session ran the E1 queue from its own session with the author's authorisation and caused a 10-minute idle gap (16:21–16:31), stated in the entry header; (c) the first 27B load test reported "does not fit" because the prompt was 24.9k tokens against an 8,192 context (fixed in 2220cc2; a `ko_swap run` option was also swallowed, fixed in 06f34d3); (d) the variants code was written by the coordinator because a code session could not be started, which is a role deviation, recorded in that entry's header ("coordinator did it"). The audit entry (c) reports no violation after the rules were written that its author could find; its author did not inspect yoda.
6. **A GPU test took production down.** The first LMLM test (2026-11-20 11:38) was started with the 27B still serving; about 40 s in the kernel's out-of-memory killer ended `funes`, both WhatsApp bridges and the monitor, and the machine stopped answering until the author rebooted it at 14:49. The cause was measured afterwards (the card's 17.1 GB was full, `vram.log`); the free VRAM before the run had been measured as about 2.1 GB and the test needs about 2.6 GB. After the reboot a PID-file-based watchdog did not restart the language-model servers, and one WhatsApp bridge needed re-pairing. The run produced no result; the second run, with the author's explicit OK to stop the 27B, did. Cost: about 3 hours of production unavailable (11:39 to 14:49) `[UNVERIFIED: time to full recovery]`. The notebook entry says the author had said "ya podemos correr" (Spanish: "we can run now") a day before the window he had set; it does not say whether the 27B was to be stopped first.
Total: three errors that touched published numbers or GPU hours (items 1–3), one set of four stale or contradicted statements (item 4), four disclosed slips (item 5, a–d), and one production outage caused by a GPU test (item 6). Only the lineage error has a measured cost in GPU hours (about 6); the outage's duration is measured from the journal and the reboot time, the cost of the others was not measured.

**What the arrangement can and cannot support (limits).**
1. **Authorship per session is not in git.** Every one of the 55 commits carries the same git author (the author's identity). Commit trailers name a *model* (46 `Claude Sonnet 5.5`, 9 `Claude Opus 5.5`), not a session or a role; attribution to a role comes only from the notebook headers, which are self-reported.
2. **The coordinator is itself a model.** Its reports were not trusted: the audit re-derived lookup rates, nudge counts and accuracies from the committed JSON and found them equal to `H5H8_TABLE.md`, and did not inspect yoda itself; the yoda-side facts it could not see (checkout HEAD, restored services) it lists as "not measured".
3. **No independent human review of the code is recorded on the branch.** The checks are the hermetic tests, the verifiers and cross-reading between sessions; they are checks by models of models.
4. **One human made the scope and budget decisions** (whether to run web cells, the search budget, which wording of the abstention clause, whether knowledge-removed models enter, which windows), and the only independent labels are his 50.
5. **Messages and briefs are not archived**, so the protocol cannot be replayed from the repository alone; only its products can.
6. **The same family reviewed itself.** All sessions were Claude models; shared blind spots are not controlled for. The lineage error is an example of a blind spot shared by every reviewer.

**To repeat the protocol:** (i) put identity in a lock file and gate the run on it; (ii) write hypotheses and decisions with identifiers before the data; (iii) keep results as files with hashes and regenerate tables from them; (iv) put a verifier between a run and its analysis; (v) end every session by writing what was measured, what was only claimed, and what is open; (vi) assign GPU work only through a script that restores the production state; (vii) have a different session audit the notebook against the files before closing sessions.


*(The work of paper 2, the E5 training, its data builder and its own failures, is organised by the same protocol and is told in that paper; this subsection stops at 2026-11-20 for the inference experiments.)*

## Appendix D. Source files and hashes

Every table cited in the body was generated by a script from the committed per-question results. The sha256 of each is below, so a reader can check that the file in the repository is the one the numbers came from.

| file (`benchmarks/knowledge-offload/results/`) | used in | sha256 |
|---|---|---|
| `E1_TABLE.md` | §5.1, §5.3 | `a0fd650c5f68c63a459b65d00d67be471c24ba528b8d0e9d79734dfa94436771` |
| `HOLM_TABLE.md` | §4.3, §5.1, §5.7 | `528b993e9be2e334afd6ba437d904e507c30ceaf6fa9b3f7492e80238e48fd90` (as of commit e4432cd; the earlier table, `a731f5ea…`, had H2 as p = 1) |
| `H5H8_TABLE.md` | §5.3, §5.4 | `ed1a2f8af2e8cf5091dda78a80e390bc1629fc2842940d7f2043dc8738e376a2` |
| `VARIANTS_TABLE.md` | §5.3, Appendix A | `20aa71c57cc848d707dd984891d74d5f875a23b423198c3a8a03a2746f49185d` |
| `WEB_AUDIT.md` | §5.5, §5.6 | `f42815287233ebd327e0d0165a764069f08849007e2a810b61da8f6dcd276883` |
| `E0_TABLE_unambiguous.md` | §5.5 | `3df7a962aed1aca12ba25c09fc42ec19c56eae7d48e097d138200a0476b23077` |
| `GSM8K_TABLE.md` | §5.7 | `dcb45572ba1d23cc75cffbf1e14a0a0e2806f38c6561d38a4482e1689556dd3c` |
| `FUNES_TABLE.md` | §5.8 | `be43d88a6d2147ef72ac8e4b67a03c4bb6f191b78d38dbc073f2fb8b4ad5d159` |
| GSM8K test file (1,319 problems; we draw 500 with `random.Random(1)`) | §5.7 | `3730d312f6e3440559ace48831e51066acaca737f6eabec99bccb9e4b3c39d14` |
| chat template of the 0.8 to 9B files (7,816 characters) | §3.2, §8 | `7f0e529032c25183bcd66c7f238da2d377f43be754a94e2725a58c4e16d2ed67` |
| chat template of the 27B file (7,817 characters) | §3.2, §8 | `e60df41481b6ad20571cda7e2e1290cfee25670f1b50973f74576c9354c66336` |
