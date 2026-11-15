#!/usr/bin/env python3
"""Knowledge-offload harness (E0). Subcommands:

  fetch      download PopQA into data/ (one file, hash recorded in results)
  run        dataset x model x condition x repeat -> results/*.json
  summarize  aggregate results/*.json: acc + 95% CI, repeat spread, paired vs closed

Inference goes to yoda's llama-server only. See README.md.
"""
import argparse
import datetime as dt
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


def cmd_run(a):
    # A killed run must still stop its scratch Funes and delete its scratch pool.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    data = Path(a.data)
    if not data.exists():
        sys.exit(f"{data} missing: run `run.py fetch`")
    all_items = popqa.load(data)
    items = popqa.sample(all_items, a.n, a.dataset_seed)
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
                 "dataset_seed": a.dataset_seed, "model_tag": a.model_tag, "server": info,
                 "git": runner.git_state(), "date": dt.date.today().isoformat(),
                 "k": C.K, "max_steps": C.MAX_STEPS, "temperature": 0.0,
                 "store_size": len(store)}
    web = T.Web(T.tavily_key())
    meta_base["search_backend"] = web.backend + ("+tavily-fallback" if web.key else "")
    blocked = []
    funes_inst = None
    try:
        if "funes" in a.conditions:
            from ko.funes import FunesInstance
            try:
                funes_inst = FunesInstance(a.llm_url, a.embed_url, a.funes_bin).__enter__()
                gold = {i.id for i in items}
                texts = [f"{i.subj} | {i.prop} | {i.obj}" for i in items] + [
                    f"{i.subj} | {i.prop} | {i.obj}" for i in all_items if i.id not in gold][:a.funes_distractors]
                print(f"funes: loading {len(texts)} facts into a scratch pool ...")
                funes_inst.load_facts(texts)
                meta_base["funes_pool"] = len(texts)
            except C.ConditionUnavailable as e:
                blocked.append(("funes", str(e)))
                funes_inst = None
        for rep in range(a.repeats):
            for cond in a.conditions:
                path = runner.cell_path(a.model_tag, a.n, cond, rep)
                if path.exists() and not a.force:
                    print(f"skip {path.name} (exists)")
                    continue
                if cond == "funes" and funes_inst is None:
                    continue
                ctx = C.Context(client=client, store=store, seed=a.seed + rep, web=web,
                                notes=T.Notes(), funes=funes_inst)
                print(f"{cond} repeat {rep} seed {ctx.seed}")
                try:
                    res = runner.run_cell(condition=cond, items=items, ctx=ctx, rep=rep,
                                          meta=meta_base)
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


def cmd_summarize(a):
    import json
    cells = {}
    for f in sorted((HERE / "results").glob(a.glob)):
        r = json.loads(f.read_text())
        cells.setdefault((r["model_tag"], r["condition"]), []).append(r)
    print(f"{'model':<22}{'condition':<15}{'reps':>4}{'acc (95% CI)':>24}{'sd':>7}{'range':>7}"
          f"{'ms/ans':>9}{'tok/ans':>9}")
    for (tag, cond), reps in sorted(cells.items()):
        ps = ["pass1", "pass2"] if cond == "web+cache" else ["pass1"]
        for ps_ in ps:
            if ps_ not in reps[0]["summary"]:
                continue
            accs = [r["summary"][ps_]["acc"] for r in reps]
            first = [x["contains"] for x in reps[0]["answers"] if x["pass"] == int(ps_[-1])]
            lo, hi = stats.bootstrap_ci([float(x) for x in first], a.resamples)
            sp = stats.spread(accs)
            pa = reps[0]["summary"][ps_]["per_answer"]
            print(f"{tag:<22}{cond + ('' if ps_ == 'pass1' else ' p2'):<15}{len(reps):>4}"
                  f"{sp['mean']:>11.3f} [{lo:.3f},{hi:.3f}]{sp['sd']:>7.3f}{sp['range']:>7.3f}"
                  f"{pa['wall_ms']:>9.0f}{pa['prompt_tokens'] + pa['completion_tokens']:>9.0f}")
    # paired: each condition against closed, repeat 0, same questions
    for tag in sorted({t for t, _ in cells}):
        base = cells.get((tag, "closed"))
        if not base:
            continue
        b = {x["id"]: x["contains"] for x in base[0]["answers"]}
        for (t, cond), reps in sorted(cells.items()):
            if t != tag or cond == "closed":
                continue
            c = {x["id"]: x["contains"] for x in reps[0]["answers"] if x["pass"] == 1}
            ids = [i for i in b if i in c]
            d, (lo, hi) = stats.paired_bootstrap([float(c[i]) for i in ids],
                                                 [float(b[i]) for i in ids], a.resamples)
            m = stats.mcnemar([c[i] for i in ids], [b[i] for i in ids])
            print(f"  {tag} {cond} - closed: {d:+.3f} [{lo:+.3f},{hi:+.3f}]  McNemar p={m['p']:.3g}")


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
    r.add_argument("--funes-distractors", type=int, default=2000,
                   help="non-sample PopQA facts also loaded into the scratch pool")
    r.add_argument("--force", action="store_true")
    r.set_defaults(fn=cmd_run)
    s = sub.add_parser("summarize")
    s.add_argument("--glob", default="popqa-*.json")
    s.add_argument("--resamples", type=int, default=10_000)
    s.set_defaults(fn=cmd_summarize)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
