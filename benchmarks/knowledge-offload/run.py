#!/usr/bin/env python3
"""Knowledge-offload harness (E0). Subcommands:

  fetch      download PopQA into data/ (one file, hash recorded in results)
  run        dataset x model x condition x repeat -> results/*.json
  summarize  aggregate results/*.json: acc + 95% CI, repeat spread, paired vs closed

Inference goes to yoda's llama-server only. See README.md.
"""
import argparse
import datetime as dt
import shutil
import signal
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from ko import conditions as C, popqa, runner, stats, tools as T       # noqa: E402
from ko.llm import Client, LLMError                                    # noqa: E402


def cmd_fetch(a):
    p = popqa.fetch()
    print(f"{p} sha256={popqa.sha256(p)}")


VARIANT_CONDITIONS = ("tool", "tool+contract", "web", "web+cache")   # the ones that choose a tool


def build_pool(a, all_items):
    """The full-pool funes memory, built once: every PopQA triple, the same list
    `store` is built from. The embeddings are the :8081 model's, so the pool is
    keyed by its content and reused for every run and model size. Returns
    (directory, texts)."""
    import hashlib
    from ko.funes import FunesInstance
    texts = runner.pool_texts(all_items)
    key = hashlib.sha256("\n".join(texts).encode()).hexdigest()[:10]
    d = HERE / "data" / "pools" / f"popqa-{len(texts)}-{key}"
    if (d / "account.json").exists():
        print(f"funes: reusing pool {d.name}")
        return d, texts
    print(f"funes: building pool {d.name} ({len(texts)} facts, embeddings from {a.embed_url}) ...")
    tmp = d.with_name(d.name + ".tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    inst = FunesInstance(a.llm_url, a.embed_url, a.funes_bin).__enter__()
    try:
        inst.load_facts(texts, log=print)
        inst.save_pool(tmp)
    finally:
        inst.__exit__(None, None, None)
    tmp.replace(d)
    return d, texts


def cmd_run(a):
    # A killed run must still stop its scratch Funes and delete its scratch pool.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    data = Path(a.data)
    if not data.exists():
        sys.exit(f"{data} missing: run `run.py fetch`")
    all_items = popqa.load(data)
    sampler = popqa.stratified_sample if a.sample == "stratified" else popqa.sample
    items = sampler(all_items, a.n, a.dataset_seed)
    candidates = popqa.candidate_sets(all_items)
    client = Client(a.llm_url, allow_local=a.allow_local)
    try:
        info = client.server_info()
    except LLMError as e:
        sys.exit(f"cannot reach {a.llm_url}: {e}")
    if a.expect_model.lower() not in info["model_file"].lower():
        sys.exit(f"{a.llm_url} serves {info['model_file']!r}, expected {a.expect_model!r}. "
                 "Not running: a swap is a logged, undone step, never a side effect of a run.")
    print(f"serving {info['model_file']} ({info['quant']}) at {info['endpoint']}")
    store = runner.build_store(all_items)
    meta_base = {"dataset": "popqa", "dataset_sha256": popqa.sha256(data),
                 "dataset_seed": a.dataset_seed, "sample": a.sample, "model_tag": a.model_tag, "server": info,
                 "git": runner.git_state(), "date": dt.date.today().isoformat(),
                 "k": C.K, "max_steps": C.MAX_STEPS, "temperature": 0.0,
                 "store_size": len(store)}
    web = T.Web(T.tavily_key())
    meta_base["search_backend"] = web.backend + ("+tavily-fallback" if web.key else "")
    blocked = []
    funes_inst = None
    pool_n = None                       # None = the E0 plumbing pool (sample + distractors)
    try:
        if "funes" in a.conditions:
            from ko.funes import FunesInstance
            try:
                if a.funes_pool == "full":
                    pool_dir, texts = build_pool(a, all_items)
                    pool_n = len(texts)
                    funes_inst = FunesInstance(a.llm_url, a.embed_url, a.funes_bin,
                                               pool=pool_dir).__enter__()
                else:
                    funes_inst = FunesInstance(a.llm_url, a.embed_url, a.funes_bin).__enter__()
                    gold = {i.id for i in items}
                    texts = runner.pool_texts(items) + runner.pool_texts(
                        [i for i in all_items if i.id not in gold][:a.funes_distractors])
                    print(f"funes: loading {len(texts)} facts into a scratch pool ...")
                    funes_inst.load_facts(texts)
                meta_base["funes_pool"] = len(texts)
            except C.ConditionUnavailable as e:
                blocked.append(("funes", str(e)))
                funes_inst = None
        for rep in range(a.repeats):
            for cond in a.conditions:
                var = a.prompt_variant if cond in VARIANT_CONDITIONS else "default"
                path = runner.cell_path(a.model_tag, a.n, cond, rep, sample=a.sample,
                                        dataset_seed=a.dataset_seed, pool=pool_n, variant=var)
                if path.exists() and not a.force:
                    print(f"skip {path.name} (exists)")
                    continue
                if cond == "funes" and funes_inst is None:
                    continue
                ctx = C.Context(client=client, store=store, seed=a.seed + rep, web=web,
                                notes=T.Notes(), funes=funes_inst, variant=var)
                print(f"{cond} repeat {rep} seed {ctx.seed}")
                try:
                    res = runner.run_cell(
                        condition=cond, items=items, ctx=ctx, rep=rep, meta={**meta_base, "prompt_variant": var},
                        candidates=candidates,
                        payloads_to=lambda p, cond=cond, rep=rep: runner.payload_dir(
                            a.model_tag, cond, rep, p, sample=a.sample, dataset_seed=a.dataset_seed,
                            variant=var))
                except C.ConditionUnavailable as e:
                    blocked.append((cond, str(e)))
                    continue
                runner.write_cell(path, res)
                print(f"  -> {path.name} {res['summary']}")
    finally:
        if funes_inst:
            funes_inst.__exit__(None, None, None)
    for cond, why in dict.fromkeys(blocked):
        print(f"BLOCKED {cond}: {why}")


def cell_label(r: dict) -> str:
    """What a result row is called in a table: the condition, plus the things
    that make it a different experiment — a funes pool other than the default,
    a stratified sample."""
    lab = r["condition"]
    if r.get("prompt_variant", "default") != "default":
        lab += "/" + r["prompt_variant"]
    if lab == "funes" and r.get("funes_pool"):
        lab += f"@{r['funes_pool']}"
    if r.get("sample", "random") != "random":
        lab += f" [{r['sample']} s{r['dataset_seed']}]"
    return lab


def with_hedge(r: dict, candidates: dict | None, by_id: dict) -> None:
    """Old cells predate the flag; it is computed from the stored prediction."""
    from ko.scoring import named_candidates
    if candidates is None:
        return
    for x in r["answers"]:
        if "hedge" not in x and x["id"] in by_id:
            it = by_id[x["id"]]
            x["hedge"] = named_candidates(x["prediction"], candidates.get((it.subj, it.prop), ())) >= 2


def cmd_summarize(a):
    import json
    candidates, by_id = None, {}
    if Path(a.data).exists():
        all_items = popqa.load(Path(a.data))
        candidates, by_id = popqa.candidate_sets(all_items), {i.id: i for i in all_items}
    cells = {}
    for f in sorted((HERE / "results").glob(a.glob)):
        r = json.loads(f.read_text())
        with_hedge(r, candidates, by_id)
        cells.setdefault((r["model_tag"], cell_label(r)), []).append(r)
    print(f"{'model':<20}{'condition':<26}{'reps':>4}{'acc (95% CI)':>24}{'sd':>7}{'range':>7}"
          f"{'acc-hedge':>10}{'hedge':>7}{'ms/ans':>9}{'tok/ans':>9}")
    for (tag, cond), reps in sorted(cells.items()):
        ps = ["pass1", "pass2"] if cond.startswith("web+cache") else ["pass1"]
        for ps_ in ps:
            if ps_ not in reps[0]["summary"]:
                continue
            accs = [r["summary"][ps_]["acc"] for r in reps]
            rows0 = [x for x in reps[0]["answers"] if x["pass"] == int(ps_[-1])]
            lo, hi = stats.bootstrap_ci([float(x["contains"]) for x in rows0], a.resamples)
            sp = stats.spread(accs)
            pa = reps[0]["summary"][ps_]["per_answer"]
            if all("hedge" in x for x in rows0):
                nh = f"{sum(x['contains'] and not x['hedge'] for x in rows0) / len(rows0):>10.3f}"
                hr = f"{sum(x['hedge'] for x in rows0) / len(rows0):>7.3f}"
            else:
                nh, hr = f"{'-':>10}", f"{'-':>7}"
            print(f"{tag:<20}{cond + ('' if ps_ == 'pass1' else ' p2'):<26}{len(reps):>4}"
                  f"{sp['mean']:>11.3f} [{lo:.3f},{hi:.3f}]{sp['sd']:>7.3f}{sp['range']:>7.3f}"
                  f"{nh}{hr}{pa['wall_ms']:>9.0f}{pa['prompt_tokens'] + pa['completion_tokens']:>9.0f}")
    # paired: each condition against closed, repeat 0, same questions (same sample only)
    for tag in sorted({t for t, _ in cells}):
        for (t, cond), reps in sorted(cells.items()):
            if t != tag or cond.split(" [")[0] == "closed":
                continue
            suffix = cond[cond.index(" ["):] if " [" in cond else ""
            base = cells.get((tag, "closed" + suffix))
            if not base:
                continue
            b = {x["id"]: x["contains"] for x in base[0]["answers"]}
            c = {x["id"]: x["contains"] for x in reps[0]["answers"] if x["pass"] == 1}
            ids = [i for i in b if i in c]
            if not ids:
                continue
            d, (lo, hi) = stats.paired_bootstrap([float(c[i]) for i in ids],
                                                 [float(b[i]) for i in ids], a.resamples)
            m = stats.mcnemar([c[i] for i in ids], [b[i] for i in ids])
            print(f"  {tag} {cond} - closed: {d:+.3f} [{lo:+.3f},{hi:+.3f}]  McNemar p={m['p']:.3g}")
    if a.by_bucket:
        by_bucket(cells)


def by_bucket(cells: dict) -> None:
    """acc (contains) per popularity bucket, repeat 0, pass 1; n in brackets.
    A bucket with a handful of items is a rumour, not a number: look at n."""
    buckets = sorted({x["pop_bucket"] for reps in cells.values() for x in reps[0]["answers"]})
    print(f"\n{'model':<20}{'condition':<26}" + "".join(f"{b:>14}" for b in buckets))
    for (tag, cond), reps in sorted(cells.items()):
        rows = [x for x in reps[0]["answers"] if x["pass"] == 1]
        cols = []
        for b in buckets:
            rb = [x for x in rows if x["pop_bucket"] == b]
            cols.append(f"{sum(x['contains'] for x in rb) / len(rb):.3f} [{len(rb)}]" if rb else "-")
        print(f"{tag:<20}{cond:<26}" + "".join(f"{c:>14}" for c in cols))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fetch").set_defaults(fn=cmd_fetch)
    r = sub.add_parser("run")
    r.add_argument("--model-tag", required=True, help="label in file names, e.g. qwen3.8-9b-q8")
    r.add_argument("--expect-model", required=True,
                   help="substring the served model file must contain; refuses otherwise")
    r.add_argument("--conditions", nargs="+", default=C.CONDITIONS, choices=C.CONDITIONS)
    r.add_argument("--repeats", type=int, default=3)
    r.add_argument("--n", type=int, default=1000)
    r.add_argument("--dataset-seed", type=int, default=0)
    r.add_argument("--seed", type=int, default=0, help="decoding seed; repeat k uses seed+k")
    r.add_argument("--data", default=str(popqa.DEFAULT_PATH))
    r.add_argument("--llm-url", default="http://yoda:8080")
    r.add_argument("--embed-url", default="http://yoda:8081")
    r.add_argument("--allow-local", action="store_true")
    r.add_argument("--funes-bin", default=None)
    r.add_argument("--funes-pool", choices=["full", "distractors"], default="full",
                   help="full: every PopQA triple, as `store` has (built once, reused); "
                        "distractors: the E0 plumbing pool, sample + --funes-distractors")
    r.add_argument("--funes-distractors", type=int, default=2000,
                   help="with --funes-pool distractors: non-sample facts also loaded")
    r.add_argument("--prompt-variant", choices=C.VARIANTS, default="default",
                   help="system prompt of tool, tool+contract, web, web+cache; search_first says to "
                        "call the tool before answering. Other conditions ignore it.")
    r.add_argument("--sample", choices=["random", "stratified"], default="random",
                   help="stratified: equal items per popularity bucket (use a new --dataset-seed)")
    r.add_argument("--force", action="store_true")
    r.set_defaults(fn=cmd_run)
    s = sub.add_parser("summarize")
    s.add_argument("--glob", default="popqa-*.json")
    s.add_argument("--resamples", type=int, default=10_000)
    s.add_argument("--by-bucket", action="store_true", help="also: acc per popularity bucket")
    s.add_argument("--data", default=str(popqa.DEFAULT_PATH),
                   help="PopQA file, to compute `hedge` for cells that predate it")
    s.set_defaults(fn=cmd_summarize)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
