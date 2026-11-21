"""E5 evaluation plan (E5_DESIGN.md section 2): which cells exist, how each is run, what each file must say.

One declarative list, `cells()`, read by `verify_e5.py` (is every cell here, whole and clean) and printed as run.py
command lines by `python3 e5_eval.py commands`. Nothing here talks to a server. The cells are the ordinary harness
cells of the base rows, run on a server that has an adapter applied; the adapter is named on the command line
(`--expect-adapter`) and the runner refuses a server whose applied adapter is not that one, so a file's `adapters`
meta is what says which arm it is. The model tag carries the arm (`...-l1`, `...-l0`) so no file name collides with
a base cell. `paraphrased` is the one new item source: set P, the first 200 sample items re-asked with other wording.
"""
import argparse
import dataclasses
import json
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import popqa  # noqa: E402

P_N = 200
SAMPLE_ARGS = ["--sample", "stratified", "--dataset-seed", "1", "--exclude-ambiguous", "--n", "1000", "--repeats", "1"]
GSM8K_N = 500


@dataclass(frozen=True)
class Cell:
    tag: str               # --model-tag
    base: str              # models.lock id of the served base
    adapter: str           # models.lock adapter id
    task: str              # "popqa" | "gsm8k"
    condition: str
    variant: str = "default"
    arm: str = "main"      # "main" | "ablated"
    paraphrase: bool = False

    @property
    def n(self) -> int:
        if self.task == "gsm8k":
            return GSM8K_N
        return P_N if (self.paraphrase or self.arm == "ablated") else 1000

    def server(self) -> tuple[str, str]:
        return self.base, self.adapter

    def argv(self, p_file: str = "data/e5/build_n1000/paraphrase_P.jsonl") -> list[str]:
        base = ["run.py", "run", "--model-tag", self.tag, "--expect", self.base, "--expect-adapter", self.adapter,
                "--conditions", self.condition]
        if self.task == "gsm8k":
            return base + ["--task", "gsm8k", "--repeats", "1"]
        out = base + SAMPLE_ARGS
        if self.variant != "default":
            out += ["--prompt-variant", self.variant]
        if self.arm == "ablated":
            out += ["--ablated"]
        if self.paraphrase:
            out += ["--paraphrase", p_file]
        return out


def _arm_cells(size: str, arm: str, adapter: str, base: str, tag: str, full: bool) -> list[Cell]:
    c = [Cell(tag, base, adapter, "popqa", "tool"),
         Cell(tag, base, adapter, "popqa", "tool", paraphrase=True),
         Cell(tag, base, adapter, "gsm8k", "closed")]
    if full:        # L1 only: closed-book cost, prompt wordings, abstention arm
        c += [Cell(tag, base, adapter, "popqa", "closed"),
              Cell(tag, base, adapter, "popqa", "tool", variant="tool_free"),
              Cell(tag, base, adapter, "popqa", "tool", variant="tool_describe"),
              Cell(tag, base, adapter, "popqa", "tool", variant="abstain_default", arm="ablated")]
    return c


def cells() -> list[Cell]:
    out = []
    for size in ("2b", "0.8b"):
        base = f"qwen3.5-{size}-iq4_xs"
        for arm in ("l1", "l0"):
            out += _arm_cells(size, arm, f"{arm}-b-{size}", base, f"{base}-{arm}", full=arm == "l1")
    out.append(Cell("qwen3.5-2b-q8_0-l1", "qwen3.5-2b-q8_0", "l1-b-2b", "popqa", "tool"))     # quantisation transfer
    return out


def servers() -> list[tuple[str, str]]:
    """The distinct (base, adapter) servers the plan needs, in plan order."""
    return list(dict.fromkeys(c.server() for c in cells()))


def paraphrased(items: list[popqa.Item], path: Path) -> tuple[list[popqa.Item], dict]:
    """The first rows of `items` with the question text of the P file; ids, aliases, subject and relation untouched.
    Refuses a file that is not exactly those items in that order (a P built for another sample must not run)."""
    rows = [json.loads(l) for l in Path(path).read_text(encoding="utf-8").splitlines() if l.strip()]
    if len(rows) > len(items):
        raise SystemExit(f"{path}: {len(rows)} rows but the sample has {len(items)} items")
    out = []
    for r, it in zip(rows, items):
        if r["id"] != it.id or r["prop"] != it.prop or list(r["aliases"]) != list(it.aliases):
            raise SystemExit(f"{path}: row {r['id']} is not sample item {it.id} ({it.prop}); P was built for another sample")
        if r["question"] == it.question or it.subj not in r["question"]:
            raise SystemExit(f"{path}: row {r['id']} question {r['question']!r} is the original, or lost the subject {it.subj!r}")
        out.append(dataclasses.replace(it, question=r["question"]))
    import hashlib
    return out, {"file": Path(path).name, "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(), "n": len(out)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["commands", "servers", "file"])
    ap.add_argument("--base")
    ap.add_argument("--adapter")
    a = ap.parse_args(argv)
    if a.what == "file":                      # the GGUF file name of a locked base or adapter
        import json as _j
        d = _j.loads((HERE / "models.lock").read_text())
        print({m["id"]: m["file"] for m in d["models"]}[a.base] if a.base else {m["id"]: m["file"] for m in d["adapters"]}[a.adapter])
        return 0
    if a.what == "servers":
        for b, ad in servers():
            print(b, ad)
        return 0
    for c in cells():
        if (a.base is None or c.base == a.base) and (a.adapter is None or c.adapter == a.adapter):
            print(" ".join(c.argv()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
