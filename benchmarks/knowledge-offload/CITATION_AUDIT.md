# Citation audit, 2026-11-22

Task from the coordinator (julio approved): read every paper cited in the two drafts, record what it says,
whether our sentence matched, and how it was read. No web search was used.

## How the papers were read

- **Tools.** (1) `get_paper_content` (alphaXiv report) for LMLM (2505.15962) and, earlier the same day, KBLaM
  (2410.10450). (2) For all 20 papers below, `curl https://arxiv.org/pdf/<id>` to a scratch directory, then
  `pdftotext -layout`. The PDFs are the arXiv full texts. They are kept out of the repository (copyright, size).
- **Depth.** For each paper I read the title block, abstract and introduction in full, and read the method, results
  and limitation passages that bear on the sentence we wrote (searched by keyword and then read in context).
  I did **not** read every page of every paper line by line. Where the verdict rests on the abstract and
  introduction only, the table says so.
- **Not checked.** Whether any reported number in these papers is itself correct. Whether other, uncited papers
  say the same thing earlier or better: no literature search was made, so no priority claim is made.

## Result per citation

"Old" is the sentence in the drafts at commit 513b173. "Verdict": OK = matches; FIX = reworded; MOVED = the
paper is relevant but to a different sentence than the one it was cited for.

| id | paper (first author) | what the paper says (from the text) | old sentence | verdict | read |
|---|---|---|---|---|---|
| 2505.15962 | Pre-training Limited Memory LMs with Internal and External Knowledge (Zhao et al., Cornell) | LMLM: pre-training with fact values masked from the loss, so the model learns to emit lookup calls (entity, relation) to an external database of triples; lower perplexity, near-parity on NLU tasks, TOFU unlearning by deleting rows, and higher FactScore/T-REx/PopQA than a same-size standard model (a 382M LMLM +28.1 points PopQA accuracy). Experiments are at ≤ 382M parameters. | "pre-train with factual knowledge moved out of the parameters into an external database that the model queries" | OK; added that they also report PopQA | abstract, intro, method, results (PDF and report) |
| 2607.07707 | Co-LMLM (Feldman et al., Cornell) | Pre-trained model that interleaves vector retrieval queries with next-token prediction and copies returned knowledge instead of memorising it; at 360M it reports lower perplexity than models trained on 40× more data and SimpleQA-verified performance in line with gpt-4o-mini. | "externalises knowledge with continuous queries and reports the goal of higher performance at smaller scales" | FIX: it reports results, not only a goal | abstract, intro, one table |
| 2607.00605 | Auditing Forgetting in LMLMs (Raeesi, Roed, Berkeley) | Audit of deletion-based unlearning in LMLMs (12,228 deletions): parametric leakage near zero; surviving correctness comes from near-neighbour retrieval (0.7% to 13.6%). | "a related audit studies forgetting in LMLMs" | OK | abstract, intro |
| 2607.12831 | Knowledgeless LMs (Cohen et al., HPI) | Pre-trains on text with named entities anonymised; closed-book recall collapses, contextual QA and robustness to noisy evidence improve (up to 20–25% relative). It changes the pre-training data, not an external store. | "other recent members of this line" (no description) | FIX: described what it does | abstract, intro |
| 2605.29075 | Knowledge Offloading, KOFF (Galliamov, Choenni, Titov) | Splits a frozen pretrained LLM into a pruned shared backbone plus domain- or language-specific memory modules (LoRA adapters, learned KV caches); Llama and Qwen 3B–14B; 12–20% of capacity moved while perplexity and MMLU are largely kept. The memories hold specialised (domain, language) knowledge, not single facts. | "decomposes a model into a sparse backbone and memory modules" | OK, but added what is offloaded and the evaluation (perplexity, MMLU) | abstract, intro |
| 2606.26807 | KARLA (Crespin, Suchanek, Holzenberger, Télécom Paris) | LoRA fine-tuning (rank 64, all attention and MLP projections) of **Qwen3-Base 0.6B–4B** on a synthetic corpus generated from a KB (YAGO, PrimeKG), interleaved with relation-specific query tokens; the model learns to emit the query, the KB value is spliced in. Reports that Qwen 0.6B with KARLA beats larger models on factual accuracy, handles unseen subjects, and follows KB edits that contradict its parametric memory. | "KARLA ... other recent members of this line" (a model change from the start of training) | **FIX, material**: KARLA is a post-hoc LoRA on released Qwen bases, i.e. the closest published neighbour of E5. Paper 2's claim that MemLLM is "the closest published relative of E5" was wrong. Paper 2 related work and gap sentence rewritten | abstract, intro, method, setup, tables |
| 2610.25425 | Mixture-of-Memories (Lu et al., MBZUAI) | Frozen base LM plus two neural memory experts (latent and lexical) mixed by a per-token router, and an external explicit memory for later updates; improves QA on Mistral-7B and Llama-3.1-8B and lowers latency vs RAG. | "other recent members of this line" | FIX: it is memory-augmented modelling on a frozen model, not pre-training externalisation | abstract, intro |
| 2608.14290 | Intern-S2-Mobius (Intern-S2-Mobius Team, Shanghai AI Lab) | An architecture with a globally shared feed-forward Memory that stores knowledge vectors and attention Reasoners that query it; a 7B trained from scratch matches a Transformer baseline with 62.6% of the data, and a Qwen3.5-35B continued-pretrain is about 4× faster end to end. The knowledge store is inside the model. | "other recent members of this line" (externalising) | FIX: separation of knowledge and reasoning inside one architecture, no external database | abstract, intro |
| 2410.10450 | KBLaM (Wang, Isazawa, Mikaelyan, Hensman; JHU, Microsoft) | Encodes KB triples as key–value vectors with linear adapters trained on synthetic data and injects them through a rectangular attention mechanism; cost linear in KB size; more than 10K triples in an 8B model on one A100; "comparable to in-context learning"; learns to refuse when the KB lacks the answer. | "reports parity with in-context learning at linear cost and a learned refusal" | OK ("parity" softened to "comparable") | abstract, intro, method, results (PDF and report) |
| 2404.11672 | MemLLM (Modarressi et al., LMU) | Fine-tunes Mistral-7B with LoRA to read from and write to an explicit structured (relation triple) memory with API calls; better language modelling and knowledge-intensive results; editing. | "fine-tunes a model to use an explicit read-write memory" | OK | abstract, intro, training setup |
| 2603.17781 | Facts as First-Class Objects: Knowledge Objects (Zahn, Chana et al.) | Compares in-context memory with hash-addressed fact tuples with O(1) retrieval; in-context is exact within the window (100% from 10 to 7,000 facts, Claude Sonnet 4.5) but overflows at 8,000 facts and loses facts under summarisation; KOs are exact at much lower cost. | "benchmark in-context memory against structured fact objects for persistent LLM memory" | OK | abstract, intro |
| 2212.10511 | When Not to Trust Language Models (Mallen et al.) | Introduces PopQA (14k questions on long-tail entities); LMs struggle on low-popularity facts, retrieval helps there, scaling mainly improves popular knowledge; proposes retrieving only when needed (adaptive retrieval). | "showed that parametric recall fails on the long tail ... Adaptive retrieval and Self-RAG-style work asks when to retrieve" | FIX: the adaptive-retrieval proposal is Mallen et al.'s own; "Self-RAG" was never read and was removed | abstract, intro, adaptive-retrieval section |
| 2202.05262 | Locating and Editing Factual Associations in GPT, ROME (Meng et al.) | Causal tracing finds mid-layer MLPs mediating factual recall; rank-one edit of those weights; introduces the CounterFact dataset. | "CounterFact-style editing (ROME ...)" | OK | abstract, intro |
| 2210.07229 | Mass-Editing Memory in a Transformer, MEMIT (Meng et al.) | Edits thousands of associations at once in GPT-J (6B) and GPT-NeoX (20B). | same | OK | abstract, intro |
| 2609.35312 | MemoReason (Tighidet et al., BNP Paribas, Sorbonne) | Pairs factual reasoning tasks with fictitious versions (entities swapped) to measure how parametric memory affects contextual reasoning: drops of up to 15.7% in the fictitious setting (a memorisation bias), but the models rarely give the real answer there, so direct recall shortcuts are not the main failure. | "ROME and MEMIT and MemoReason motivate a conflict experiment" | **MOVED/FIX**: MemoReason is about memory versus reasoning, not about knowledge conflicts or editing; moved to the memory-versus-reasoning paragraph | abstract, intro |
| 2411.13504 | Disentangling Memory and Reasoning Ability in LLMs (Jin et al.) | An inference format with `<memory>` and `<reason>` tokens that makes the model separate recall steps from reasoning steps; it improves benchmark accuracy and interpretability. It is a prompting/training format, not a localisation inside the network. | "Jin et al. ... separate recall from reasoning inside the network" | **FIX**: not a layer or circuit analysis | abstract, intro |
| 2510.02091 | Demystifying the Roles of LLM Layers in Retrieval, Knowledge, and Reasoning (Song et al.) | Systematic study of layer pruning: knowledge and retrieval concentrate in shallow components; reasoning relies on deeper layers; conclusions depend on the evaluation protocol. | "layer-localisation papers separate recall from reasoning" | OK | abstract, intro |
| 2510.03366 | Disentangling Recall and Reasoning ... Layer-wise Attention and Activation Analysis (Fartale et al.) | Activation patching, head and neuron analysis on Qwen, LLaMA-3 and Mistral: early and middle layers mainly support recall, deeper layers and some MLP paths support reasoning; ablating recall circuits selectively hurts recall. | same | OK | abstract, intro |
| 2503.23084 | The Reasoning-Memorization Interplay ... Single Direction (Hong et al.) | Finds linear features (LiReFs) in the residual stream that govern the balance between reasoning and memorisation; intervening on them shifts the behaviour. Analysis is by direction, not by layer. | same ("by layer") | FIX: a direction in the residual stream, not a set of layers | abstract, intro, figure 1 |
| 2507.18178 | Decoupling Knowledge and Reasoning in LLMs (Yang, Gao, Wu, Tsinghua) | Prompts fast versus slow thinking to separate knowledge from reasoning over 15 LLMs; finds knowledge mainly in lower layers and reasoning in higher layers; scaling helps knowledge more. | not cited ("not verified") | Now read; added to the layer paragraph | abstract |

## Sentences that changed (summary)

1. Paper 1 and 2, "models that externalise knowledge": four of the "members of this line" are different in kind
   (Knowledgeless: data change; MoM: frozen model plus memory experts; Mobius: architecture with the store inside;
   KOFF: domain and language memories). Each is now described by what it does.
2. Paper 2, "closest published relative of E5 is MemLLM": replaced. KARLA (LoRA on Qwen3-Base 0.6B–4B, query tokens,
   KB values spliced in, unseen subjects) is at least as close, and MemLLM also uses LoRA (Mistral-7B). The novelty
   statement in paper 2 now says what E5 adds and what it does not.
3. Both papers, Jin et al. (2411.13504) and Hong et al. (2503.23084): not layer analyses; reworded.
4. Both papers, MemoReason: not a knowledge-conflict paper; moved.
5. Paper 1, "Self-RAG-style work": removed (never read). Adaptive retrieval is credited to Mallen et al. (2212.10511),
   who propose it.
6. Co-LMLM: reports measured results; said so.

## Still not verified

- AtlasKV: not found, not cited.
- Whether a lookup LoRA trained with a no-call control and held-out relations, as in E5, has been reported before.
  KARLA tests unseen subjects (and reports PopQA accuracy); its Limitations section says the predicate vocabulary
  is closed by construction and that new predicates need additional training. We found, in the text we read, no
  no-call control and no held-out-relation test, which is what E5 adds. No broader search was made.
- Everything in these papers beyond the sections read for the sentence at hand.
