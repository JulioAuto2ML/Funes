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

from ko import conditions as C, gsm8k, models, popqa, runner, stats, tools as T, validity  # noqa: E402
from ko.llm import Client, LLMError                                    # noqa: E402


def cmd_fetch(a):
    if a.task == "gsm8k":
        p, commit = gsm8k.fetch(commit=a.commit)
        print(f"{p} commit={commit} sha256={gsm8k.sha256(p)} problems={gsm8k.validate(p)}")
        return
    p = popqa.fetch()
    print(f"{p} sha256={popqa.sha256(p)}")




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


def cmd_gsm8k(a):
    """H2's fact-free arm (ko/gsm8k.py): closed / store / tool on word problems, the PopQA store as the distractor."""
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    gpath = Path(a.gsm8k_data)
    if not gpath.exists():
        sys.exit(f"{gpath} missing: run `run.py fetch --task gsm8k`")
    try:
        ghash = gsm8k.check_pinned(gpath)
    except ValueError as e:
        sys.exit(str(e))
    problems_all = gsm8k.load(gpath)
    problems = gsm8k.pilot_sample(problems_all) if a.pilot else gsm8k.sample(problems_all, a.gsm8k_n, a.gsm8k_seed)
    conds = a.conditions or list(gsm8k.H2_CONDITIONS)
    bad = [c for c in conds if c not in gsm8k.CONDITIONS]
    if bad:
        sys.exit(f"GSM8K runs {', '.join(gsm8k.CONDITIONS)}, not {', '.join(bad)}")
    data = Path(a.data)
    if not data.exists():
        sys.exit(f"{data} missing: run `run.py fetch` (the PopQA triples are the distractor store)")
    all_items = popqa.load(data)
    client = Client(a.llm_url, allow_local=a.allow_local)
    try:
        info = client.server_info()
    except LLMError as e:
        sys.exit(f"cannot reach {a.llm_url}: {e}")
    try:
        locked = models.verify(info["model_file"], models_dir=Path(a.models_dir), expect=a.expect,
                               name_only=a.lock_name_only)
    except models.ModelRefused as e:
        sys.exit(f"{a.llm_url}: {e} Not running: a swap is a logged, undone step, never a side effect of a run.")
    try:
        adapters = models.verify_adapters(client.lora_adapters(), base_id=locked["id"], adapters_dir=Path(a.adapters_dir),
                                          expect=a.expect_adapter)
    except models.ModelRefused as e:
        sys.exit(f"{a.llm_url}: {e} Not running.")
    print(f"serving {locked['id']} [{locked['lineage']}] {info['model_file']} ({info['quant']}); "
          f"{len(problems)} problems, max_tokens {a.max_tokens}{' (PILOT)' if a.pilot else ''}")
    store = runner.build_store(all_items)
    meta = {"dataset": "gsm8k-test", "dataset_sha256": ghash, "dataset_seed": a.gsm8k_seed,
            "pilot": a.pilot, "model_tag": a.model_tag, "server": info, "adapters": adapters,
            "model": {k: locked[k] for k in ("id", "repo", "file", "sha256", "size", "quant", "lineage", "verified")},
            "git": runner.git_state(), "date": dt.date.today().isoformat(), "k": C.K, "max_steps": C.MAX_STEPS,
            "temperature": 0.0, "store_size": len(store), "store": "popqa-all-triples (distractors only)"}
    for rep in range(a.repeats):
        for cond in conds:
            path = gsm8k.cell_path(runner.RESULTS, a.model_tag, cond, rep, n=len(problems),
                                   seed=gsm8k.PILOT_SEED if a.pilot else a.gsm8k_seed, pilot=a.pilot)
            if path.exists() and not a.force:
                print(f"skip {path.name} (exists)")
                continue
            ctx = C.Context(client=client, store=store, seed=a.seed + rep, notes=T.Notes())
            ctx.max_tokens = a.max_tokens
            print(f"{cond} repeat {rep} seed {ctx.seed}")
            res = gsm8k.run_cell(cond=cond, problems=problems, ctx=ctx, rep=rep, meta=meta)
            runner.write_cell(path, res)
            print(f"  -> {path.name} {res['summary']}")


def cmd_run(a):
    if a.task == "gsm8k":
        return cmd_gsm8k(a)
    # A killed run must still stop its scratch Funes and delete its scratch pool.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    data = Path(a.data)
    if not data.exists():
        sys.exit(f"{data} missing: run `run.py fetch`")
    all_items = popqa.load(data)
    sampler = popqa.stratified_sample if a.sample == "stratified" else popqa.sample
    candidates = popqa.candidate_sets(all_items)
    # The store below is always every triple; only the questions are narrowed.
    pool_items = popqa.unambiguous(all_items, candidates) if a.exclude_ambiguous else all_items
    items = sampler(pool_items, a.n, a.dataset_seed)
    arm = "ablated" if a.ablated else "main"
    if a.ablated:
        # H5b: the first 200 questions of the same sample, own triple withheld from retrieval.
        if a.prompt_variant not in ("abstain_ok", "abstain_default"):
            sys.exit("--ablated needs --prompt-variant abstain_ok or abstain_default (a way out that is not a guess)")
        items = items[:a.ablate_n]
        a.conditions = a.conditions or (list(C.VARIANT_APPLIES["abstain_default"]) if a.prompt_variant == "abstain_default"
                                        else list(C.ABLATED_CONDITIONS))
        if a.prompt_variant == "abstain_default" and set(a.conditions) - set(C.VARIANT_APPLIES["abstain_default"]):
            sys.exit("abstain_default applies to tool and tool+contract only (closed/store: use abstain_ok)")
        bad = [c for c in a.conditions if c not in C.ABLATED_CONDITIONS]
        if bad:
            sys.exit(f"--ablated runs {', '.join(C.ABLATED_CONDITIONS)}, not {', '.join(bad)}")
    a.conditions = a.conditions or list(C.CONDITIONS)
    print(f"questions: {len(items)} of {len(pool_items)} eligible ({len(all_items)} triples in the store)"
          + (", ambiguous (homonym) subjects excluded" if a.exclude_ambiguous else ""))
    client = Client(a.llm_url, allow_local=a.allow_local)
    try:
        info = client.server_info()
    except LLMError as e:
        sys.exit(f"cannot reach {a.llm_url}: {e}")
    try:
        locked = models.verify(info["model_file"], models_dir=Path(a.models_dir), expect=a.expect,
                               name_only=a.lock_name_only)
    except models.ModelRefused as e:
        sys.exit(f"{a.llm_url}: {e} Not running: a swap is a logged, undone step, never a side effect of a run.")
    try:
        adapters = models.verify_adapters(client.lora_adapters(), base_id=locked["id"], adapters_dir=Path(a.adapters_dir),
                                          expect=a.expect_adapter)
    except models.ModelRefused as e:
        sys.exit(f"{a.llm_url}: {e} Not running.")
    print(f"serving {locked['id']} [{locked['lineage']}] {info['model_file']} ({info['quant']}) at {info['endpoint']}, "
          f"verified by {locked['verified']}")
    store = runner.build_store(all_items)
    meta_base = {"dataset": "popqa", "dataset_sha256": popqa.sha256(data),
                 "dataset_seed": a.dataset_seed, "sample": a.sample,
                 "exclude_ambiguous": a.exclude_ambiguous, "arm": arm, "model_tag": a.model_tag, "server": info, "adapters": adapters,
                 "model": {k: locked[k] for k in ("id", "repo", "file", "sha256", "size", "quant", "lineage", "verified")},
                 "git": runner.git_state(), "date": dt.date.today().isoformat(),
                 "k": C.K, "max_steps": C.MAX_STEPS, "temperature": 0.0,
                 "store_size": len(store)}
    # Parallel only: Tavily is reserved for the newsletter, so there is no key to fall back on.
    web = T.Web(None, backend="parallel")
    meta_base["search_backend"] = "parallel-keyed" if web.parallel_key else "parallel-anonymous"
    blocked = []
    search_blocked = False
    search_failed = False
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
                var = C.variant_for(cond, a.prompt_variant)
                path = runner.cell_path(a.model_tag, len(items), cond, rep, sample=a.sample,
                                        dataset_seed=a.dataset_seed, pool=pool_n, variant=var,
                                        unambiguous=a.exclude_ambiguous, arm=arm)
                if path.exists() and not a.force:
                    print(f"skip {path.name} (exists)")
                    continue
                if cond == "funes" and funes_inst is None:
                    continue
                ctx = C.Context(client=client, store=store, seed=a.seed + rep, web=web,
                                notes=T.Notes(), funes=funes_inst, variant=var,
                                ablated=a.ablated)
                if cond in C.WEB_CONDITIONS and (search_blocked or search_failed):
                    print(f"skip {path.name}: search blocked or failing earlier in this run")
                    continue
                print(f"{cond} repeat {rep} seed {ctx.seed}")
                rl0, s0, e0 = web.rate_limited, web.searches, web.search_errors
                try:
                    res = runner.run_cell(
                        condition=cond, items=items, ctx=ctx, rep=rep, meta={**meta_base, "prompt_variant": var},
                        candidates=candidates,
                        payloads_to=lambda p, cond=cond, rep=rep: runner.payload_dir(
                            a.model_tag, cond, rep, p, sample=a.sample, dataset_seed=a.dataset_seed,
                            variant=var, unambiguous=a.exclude_ambiguous, arm=arm))
                except C.ConditionUnavailable as e:
                    blocked.append((cond, str(e)))
                    continue
                except T.SearchFailed as e:
                    search_failed = True
                    blocked.append((cond, f"SEARCH FAILED (status {e.status}): {e}; {web.search_errors - e0} error(s) in "
                                          f"{web.searches - s0} searches, last: {web.error_log[-1]['text'] if web.error_log else '-'}; "
                                          f"cell not written, rerun resumes it"))
                    continue
                except T.SearchBlocked as e:
                    search_blocked = True
                    blocked.append((cond, f"{e}; {web.rate_limited - rl0} 429s in {web.searches - s0} searches, "
                                          f"cell not written, rerun resumes it"))
                    continue
                res["search"] = {"rate_limited": web.rate_limited - rl0, "searches": web.searches - s0,
                                 "errors": web.search_errors - e0, "error_log": web.error_log[len(web.error_log) - (web.search_errors - e0):][:5]}
                runner.write_cell(path, res)
                print(f"  -> {path.name} {res['summary']}")
    finally:
        if funes_inst:
            funes_inst.__exit__(None, None, None)
    for cond, why in dict.fromkeys(blocked):
        print(f"BLOCKED {cond}: {why}")
    if search_failed:
        sys.exit(4)          # Parallel answered with an error (402 credit, MCP isError, not JSON, no reply)
    if search_blocked:
        sys.exit(3)          # a watcher can tell "stopped by Parallel" from "finished"


def cell_label(r: dict) -> str:
    """What a result row is called in a table: the condition, plus the things
    that make it a different experiment — a funes pool other than the default,
    a stratified sample."""
    lab = r["condition"]
    if r.get("prompt_variant", "default") != "default":
        lab += "/" + r["prompt_variant"]
    if lab == "funes" and r.get("funes_pool"):
        lab += f"@{r['funes_pool']}"
    if r.get("arm") == "ablated":
        lab += " ablated"
    if r.get("sample", "random") != "random":
        lab += f" [{r['sample']} s{r['dataset_seed']}{' unamb' if r.get('exclude_ambiguous') else ''}]"
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
        r["_file"] = f.name
        cells.setdefault((r["model_tag"], cell_label(r)), []).append(r)
    notes = validity.annotations(HERE / "results")
    print(f"{'model':<20}{'condition':<26}{'reps':>4}{'acc (95% CI)':>24}{'sd':>7}{'range':>7}"
          f"{'acc-hedge':>10}{'hedge':>7}{'ms/ans':>9}{'tok/ans':>9}")
    flagged = []
    for (tag, cond), reps in sorted(cells.items()):
        ps = ["pass1", "pass2"] if cond.split("/")[0].split(" ")[0] in C.TWO_PASS else ["pass1"]
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
            st, why = validity.status(reps[0], reps[0]["_file"], notes)
            if st != "ok" and ps_ == "pass1":
                flagged.append((tag, cond, st, why))
            print(f"{tag:<20}{(cond + ('' if ps_ == 'pass1' else ' p2') + ('' if st == 'ok' else ' [' + st + ']')):<26}{len(reps):>4}"
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
    for tag, cond, st, why in flagged:
        print(f"  {st}: {tag} {cond}: {why}")
    writeback_report(cells)
    if a.by_bucket:
        by_bucket(cells)


def writeback_report(cells: dict) -> None:
    """H6 as pre-registered: pass-2 accuracy against pass 1, and how many web searches
    pass 2 still made. Same questions, so the accuracy difference is paired."""
    for (tag, cond), reps in sorted(cells.items()):
        if cond.split("/")[0].split(" ")[0] != "web+writeback":
            continue
        rows = reps[0]["answers"]
        p1 = {x["id"]: x["contains"] for x in rows if x["pass"] == 1}
        p2 = {x["id"]: x["contains"] for x in rows if x["pass"] == 2}
        ids = [i for i in p1 if i in p2]
        if not ids:
            continue
        d, (lo, hi) = stats.paired_bootstrap([float(p2[i]) for i in ids], [float(p1[i]) for i in ids], 10_000)
        m = stats.mcnemar([p2[i] for i in ids], [p1[i] for i in ids])
        s1, s2 = (reps[0]["summary"][p].get("searches_per_answer", 0.0) for p in ("pass1", "pass2"))
        print(f"  {tag} {cond} pass2 - pass1: {d:+.3f} [{lo:+.3f},{hi:+.3f}] McNemar p={m['p']:.3g}; "
              f"web searches per answer {s1:.2f} -> {s2:.2f}" + (f" (ratio {s2 / s1:.2f})" if s1 else ""))


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
    f = sub.add_parser("fetch")
    f.add_argument("--task", choices=["popqa", "gsm8k"], default="popqa")
    f.add_argument("--commit", default=None, help="gsm8k: commit of openai/grade-school-math (default: master now)")
    f.set_defaults(fn=cmd_fetch)
    r = sub.add_parser("run")
    r.add_argument("--task", choices=["popqa", "gsm8k"], default="popqa")
    r.add_argument("--pilot", action="store_true", help="gsm8k: the 20 pilot problems, written to gsm8k-pilot-* files")
    r.add_argument("--max-tokens", type=int, default=gsm8k.MAX_TOKENS, help="gsm8k: reply limit (512; 1024 only by the pilot rule)")
    r.add_argument("--gsm8k-data", default=str(gsm8k.DEFAULT_PATH))
    r.add_argument("--gsm8k-n", type=int, default=gsm8k.N)
    r.add_argument("--gsm8k-seed", type=int, default=gsm8k.DATASET_SEED)
    r.add_argument("--model-tag", required=True, help="label in file names, e.g. qwen3.8-9b-q8")
    r.add_argument("--expect", default=None, metavar="LOCK_ID",
                   help="optional models.lock id the served model must be; the sha256 gate runs regardless")
    r.add_argument("--expect-adapter", default=None, metavar="ADAPTER_ID",
                   help="models.lock adapter the server must have applied (E5); an applied adapter without this is refused")
    r.add_argument("--adapters-dir", default=str(models.DEFAULT_MODELS_DIR / "adapters"),
                   help="where the served adapter GGUFs are, to check their sha256")
    r.add_argument("--models-dir", default=str(models.DEFAULT_MODELS_DIR),
                   help="where the server's model files are, to hash the one being served")
    r.add_argument("--lock-name-only", action="store_true",
                   help="accept a locked file name when its bytes are not reachable from here (recorded in meta)")
    r.add_argument("--conditions", nargs="+", default=None, choices=C.CONDITIONS + C.EXTRA_CONDITIONS)
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
                   help="system prompt of tool, tool+contract, web, web+cache, web+writeback; search_first says to "
                        "call the tool before answering. Other conditions ignore it.")
    r.add_argument("--sample", choices=["random", "stratified"], default="random",
                   help="stratified: equal items per popularity bucket (use a new --dataset-seed)")
    r.add_argument("--exclude-ambiguous", action="store_true",
                   help="draw the questions only from items whose subject+property has a single object over "
                        "all 14,267 triples (E1's primary sample); the store keeps every triple")
    r.add_argument("--ablated", action="store_true",
                   help="H5b: first --ablate-n questions of the sample, each one's own triple withheld "
                        "from retrieval; needs --prompt-variant abstain_ok (search_first tool prompt) or abstain_default (default tool prompt, tool and tool+contract only); closed/store/tool/tool+contract")
    r.add_argument("--ablate-n", type=int, default=200)
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
