#!/usr/bin/env python3
"""E5 LoRA training (route C): bf16 base, LoRA on every linear layer, loss on assistant tokens only.

  python3 e5_train.py --rows lookup_L1_n1000.jsonl anchors_L1.jsonl --model-dir data/e5/qwen3.5-2B-bf16 \
      --out data/e5/run_x --rank 16 --alpha 32 --lr 2e-4 --epochs 2 [--probe-steps 30]

The example for each row is built exactly as gates 9a/9b checked it (`e5_check_template`: same Jinja template, same
tokenizer offsets, same spans); nothing here renders a conversation another way. Examples are shuffled once with
`--seed` and batched by `--batch` with no padding trick (right padding, labels -100 on pad and on masked tokens).
`--probe-steps N` stops after N optimizer steps and writes only `probe.json` (seconds per step, tokens per second,
peak VRAM): a throughput probe, not a training run; no adapter is saved.
Needs torch + transformers + peft (the `venv-train` environment); the pure functions are tested without them.
"""
import argparse
import json
import random
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import e5_check_template as C  # noqa: E402

IGNORE = -100


def build_example(tpl, tok, row: dict, max_len: int) -> dict | None:
    """{"input_ids", "labels"} for one row, or None when it does not fit `max_len` (never truncated: a cut
    conversation would train on a call or an answer that lost its end)."""
    spans, full = C.trained_spans(tpl, row)
    mt = C.masked_tokens(tok, full, spans)
    if mt["straddling_tokens"]:
        raise ValueError(f"{row['id']}: tokens straddle a trained span edge: {mt['straddling_tokens'][:3]}")
    if len(mt["ids"]) > max_len or not any(mt["train"]):
        return None
    return {"input_ids": mt["ids"], "labels": [i if t else IGNORE for i, t in zip(mt["ids"], mt["train"])]}


def collate(batch: list[dict], pad_id: int) -> dict:
    """Right-pad to the longest example; attention 0 and label IGNORE on the padding. Plain lists (tensors are the loop's job)."""
    n = max(len(b["input_ids"]) for b in batch)
    ids = [b["input_ids"] + [pad_id] * (n - len(b["input_ids"])) for b in batch]
    lab = [b["labels"] + [IGNORE] * (n - len(b["labels"])) for b in batch]
    att = [[1] * len(b["input_ids"]) + [0] * (n - len(b["input_ids"])) for b in batch]
    return {"input_ids": ids, "labels": lab, "attention_mask": att}


def batches(examples: list[dict], batch: int, epochs: int, seed: int) -> list[list[dict]]:
    """The whole schedule up front: per-epoch shuffle from `seed`, the last short batch of an epoch kept. Same seed, same order."""
    out = []
    for e in range(epochs):
        order = list(range(len(examples)))
        random.Random(seed * 1000 + e).shuffle(order)
        for i in range(0, len(order), batch):
            out.append([examples[j] for j in order[i:i + batch]])
    return out


def masked_lm_loss(model, t: dict):
    """Mean cross-entropy over the trained positions only, the same number the library's loss gives (mean over labels != IGNORE),
    but the vocabulary projection runs on those positions alone: at 248k tokens a full-sequence fp32 logits tensor of a batch of
    8 is 5 GB and ran the probe out of memory (2026-11-20)."""
    import torch
    inner = model.base_model.model                       # peft wrapper -> the causal LM
    h = inner.model(input_ids=t["input_ids"], attention_mask=t["attention_mask"]).last_hidden_state[:, :-1]
    y = t["labels"][:, 1:]
    keep = y != IGNORE
    logits = inner.lm_head(h[keep]).float()
    return torch.nn.functional.cross_entropy(logits, y[keep])


def read_rows(files: list[str]) -> list[dict]:
    rows = []
    for f in files:
        rows += [json.loads(l) for l in Path(f).read_text(encoding="utf-8").splitlines() if l.strip()]
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", required=True, nargs="+")
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rank", type=int, default=16)
    ap.add_argument("--alpha", type=int, default=32)
    ap.add_argument("--dropout", type=float, default=0.0)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--epochs", type=int, default=2)
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--max-len", type=int, default=1024)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--probe-steps", type=int, default=0)
    a = ap.parse_args(argv)

    import torch
    from peft import LoraConfig, get_peft_model
    from tokenizers import Tokenizer
    from transformers import AutoModelForCausalLM

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    md = Path(a.model_dir)
    tpl, tok = C.load_template(md), Tokenizer.from_file(str(md / "tokenizer.json"))
    pad_id = tok.token_to_id("<|endoftext|>")
    rows = read_rows(a.rows)
    exs = [build_example(tpl, tok, r, a.max_len) for r in rows]
    kept = [e for e in exs if e is not None]
    print(f"rows {len(rows)}; examples {len(kept)}; dropped for length/empty mask {len(rows) - len(kept)}; "
          f"tokens {sum(len(e['input_ids']) for e in kept)}; trained tokens {sum(sum(l != IGNORE for l in e['labels']) for e in kept)}", flush=True)
    sched = batches(kept, a.batch, a.epochs, a.seed)
    total = a.probe_steps or len(sched)

    torch.manual_seed(a.seed)
    model = AutoModelForCausalLM.from_pretrained(str(md), dtype=torch.bfloat16, device_map={"": 0})
    model.gradient_checkpointing_enable()
    model.enable_input_require_grads()
    cfg = LoraConfig(r=a.rank, lora_alpha=a.alpha, lora_dropout=a.dropout, target_modules="all-linear", task_type="CAUSAL_LM")
    model = get_peft_model(model, cfg)
    model.print_trainable_parameters()
    params = [p for p in model.parameters() if p.requires_grad]
    opt = torch.optim.AdamW(params, lr=a.lr, weight_decay=0.0)
    sch = torch.optim.lr_scheduler.LambdaLR(opt, lambda s: min(1.0, (s + 1) / max(1, total // 20)) * max(0.0, 1 - s / total))
    model.train()
    log, t0, toks = [], time.time(), 0
    torch.cuda.reset_peak_memory_stats()
    for step, b in enumerate(sched[:total], 1):
        c = collate(b, pad_id)
        t = {k: torch.tensor(v, device="cuda") for k, v in c.items()}
        loss = masked_lm_loss(model, t)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(params, 1.0)
        opt.step(); sch.step(); opt.zero_grad(set_to_none=True)
        toks += int(t["attention_mask"].sum())
        log.append({"step": step, "loss": float(loss), "t": round(time.time() - t0, 2)})
        if step % 10 == 0 or step == total:
            print(f"step {step}/{total} loss {float(loss):.4f} {time.time() - t0:.0f}s", flush=True)
    wall = time.time() - t0
    stats = {"steps": total, "wall_s": round(wall, 2), "s_per_step": round(wall / total, 3), "tokens_per_s": round(toks / wall, 1),
             "peak_vram_gib": round(torch.cuda.max_memory_allocated() / 2 ** 30, 2), "batch": a.batch, "max_len": a.max_len,
             "rank": a.rank, "alpha": a.alpha, "lr": a.lr, "epochs": a.epochs, "schedule_steps": len(sched),
             "examples": len(kept), "estimated_full_run_s": round(wall / total * len(sched)), "estimated": True}
    (out / "log.json").write_text(json.dumps(log))
    if a.probe_steps:
        (out / "probe.json").write_text(json.dumps(stats, indent=1))
        print(json.dumps(stats))
        return 0
    model.save_pretrained(str(out / "adapter"))
    (out / "train.json").write_text(json.dumps({**stats, "estimated": False}, indent=1))
    print(json.dumps({**stats, "estimated": False}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
