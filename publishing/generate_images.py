#!/usr/bin/env python3
"""
generate_images.py — one LinkedIn illustration per item of a day's issue.

Usage:
    python3 generate_images.py [YYYY-MM-DD] [--dir PATH] [--publication ID]
                               [--server URL] [--only N]
    python3 generate_images.py [YYYY-MM-DD] [--dir PATH] --pending
    python3 generate_images.py [YYYY-MM-DD] [--dir PATH] --backfill [--llm-url URL]
    python3 generate_images.py --prompt "text" --out file.png [--server URL]

Reads each item's `image_prompt` from the issue JSON, renders it through a
running ComfyUI server (Qwen Image 2.1) and writes
<dir>/images/<publication>/<date>/<n>.png — the path post_tweet.py looks in.

This script only talks to a server that is already up. Freeing the GPU, starting
ComfyUI and giving the GPU back to the LLM are run_images.sh's job, so a crash
here can never leave Funes without its model.

An image is garnish: a prompt that fails to render is logged and skipped, the
rest of the batch carries on, and post_tweet.py posts text-only for the gap.

Exit codes: 0 done (even if some items failed), 1 usage/input error,
            2 the server could not be reached.
"""

import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from datetime import date
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from publish_newsletter import issue_dir   # noqa: E402
import image_prompts as ip                 # noqa: E402

DEFAULT_SERVER = "http://127.0.0.1:8189"

# 1.9:1, the shape LinkedIn feeds show uncropped. Multiples of 16.
WIDTH, HEIGHT = 1280, 672

# Added here, not asked of the model: one look for the whole series, and the
# scene the model writes can then concentrate on what the picture is *of*.
# (Changed 2026-10-13 from flat editorial illustration to a photographic look.)
STYLE = ("Photorealistic cinematic photograph, futuristic high-tech aesthetic, "
         "dramatic volumetric lighting, shallow depth of field, ultra-detailed "
         "materials and reflections, sleek glass and brushed metal, cool teal and "
         "deep blue tones with a subtle warm accent light, shot on a full-frame "
         "camera with a 35mm lens, sharp focus, 8k. No text or lettering, no "
         "logos, no faces.")
NEGATIVE = ("cartoon, illustration, drawing, painting, flat colors, vector art, "
            "anime, toy-like render, text, letters, numbers, watermark, logo, "
            "signature, face, portrait, blurry, low quality")

# Model files, as named in ~/ComfyUI-Shared/models.
UNET = "qwen-image-2.1-UC-fp8.safetensors"
CLIP = "qwenImage21TextEncoderOriginal_v10_3241874.safetensors"
VAE = "qwen_image_2.1_vae_bf16.safetensors"
STEPS, CFG = 25, 1.0

# A cold first image took ~170 s end to end on yoda; anything past this is a
# wedged server, not a slow render.
RENDER_TIMEOUT_S = 300
# The GPU is lent by the LLM for the duration. Two failures running means
# ComfyUI is unwell (the 2026-10-12 rehearsal: one out-of-memory error, then two
# ten-minute timeouts with Funes offline), so stop and return the GPU.
MAX_CONSECUTIVE_FAILURES = 2


# ── Pure parts ────────────────────────────────────────────────────────────────

def image_dir(work_dir: Path, publication: str, day: str) -> Path:
    return work_dir / "images" / publication / day


def prompts_for(issue: dict, extra: dict = None) -> list:
    """[(slot, prompt)] for the items that carry a usable prompt — their own, or
    one backfilled by image_prompts.py. The slot is the item's position, because
    that is the number cron hands post_tweet.py."""
    return ip.merged(issue, extra or {})


def pending(issue: dict, out_dir: Path, extra: dict = None) -> list:
    """Slots that have a prompt but no image yet."""
    return [slot for slot, _ in prompts_for(issue, extra)
            if not (out_dir / f"{slot}.png").exists()]


def full_prompt(subject: str) -> str:
    return f"{subject.rstrip('. ')}. {STYLE}"


def seed_for(day: str, slot: int) -> int:
    return zlib.crc32(f"{day}:{slot}".encode())


def build_workflow(prompt: str, seed: int, width: int = WIDTH,
                   height: int = HEIGHT) -> dict:
    """ComfyUI API-format graph: Qwen Image 2.1, text to image."""
    return {
        "1": {"class_type": "UNETLoader",
              "inputs": {"unet_name": UNET, "weight_dtype": "default"}},
        "2": {"class_type": "CLIPLoader",
              "inputs": {"clip_name": CLIP, "type": "qwen_image",
                         "device": "default"}},
        "3": {"class_type": "VAELoader", "inputs": {"vae_name": VAE}},
        "4": {"class_type": "TextEncodeQwenImage21",
              "inputs": {"clip": ["2", 0], "prompt": prompt,
                         "negative_prompt": "", "resolution": 1024}},
        "5": {"class_type": "TextEncodeQwenImage21",
              "inputs": {"clip": ["2", 0], "prompt": NEGATIVE,
                         "negative_prompt": "", "resolution": 1024}},
        "6": {"class_type": "EmptyLatentImage",
              "inputs": {"width": width, "height": height, "batch_size": 1}},
        "7": {"class_type": "KSampler",
              "inputs": {"model": ["1", 0], "positive": ["4", 0],
                         "negative": ["5", 0], "latent_image": ["6", 0],
                         "seed": seed, "steps": STEPS, "cfg": CFG,
                         "sampler_name": "euler", "scheduler": "simple",
                         "denoise": 1.0}},
        "8": {"class_type": "VAEDecode",
              "inputs": {"samples": ["7", 0], "vae": ["3", 0]}},
        "9": {"class_type": "SaveImage",
              "inputs": {"images": ["8", 0], "filename_prefix": "funes"}},
    }


def describe_comfy_error(status: dict, limit: int = 300) -> str:
    """ComfyUI's error status carries the whole graph's tensors. The cron log
    wants the one line that says what went wrong."""
    for message in status.get("messages", []):
        if len(message) == 2 and message[0] == "execution_error":
            body = message[1]
            text = f"{body.get('node_type', '?')}: {body.get('exception_message', '')}"
            return " ".join(text.split())[:limit]
    return " ".join(str(status.get("status_str", "error")).split())[:limit]


def generate(issue: dict, out_dir: Path, client, day: str = "", only=None,
             extra: dict = None, log=lambda m: print(m, flush=True)) -> dict:
    """Render every prompt that has no image yet. One failure never stops the
    batch — the result says what happened to each slot."""
    result = {"made": [], "skipped": [], "failed": [], "aborted": []}
    streak = 0
    out_dir.mkdir(parents=True, exist_ok=True)
    for slot, prompt in prompts_for(issue, extra):
        if only is not None and slot != only:
            continue
        if streak >= MAX_CONSECUTIVE_FAILURES:
            log(f"[{slot}] not tried: {streak} failures in a row, giving the GPU back")
            result["aborted"].append(slot)
            continue
        target = out_dir / f"{slot}.png"
        if target.exists():
            log(f"[{slot}] already there, skipping")
            result["skipped"].append(slot)
            continue
        started = time.monotonic()
        try:
            png = client.render(full_prompt(prompt), seed_for(day, slot))
        except Exception as e:                      # noqa: BLE001 — logged, not hidden
            log(f"[{slot}] FAILED: {e}")
            result["failed"].append(slot)
            streak += 1
            continue
        streak = 0
        target.write_bytes(png)
        log(f"[{slot}] ok in {time.monotonic() - started:.0f}s -> {target}")
        result["made"].append(slot)
    return result


# ── ComfyUI HTTP client ───────────────────────────────────────────────────────

class ComfyClient:
    def __init__(self, base_url: str):
        self.base = base_url.rstrip("/")

    def _json(self, path, payload=None):
        data = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(
            self.base + path, data=data,
            headers={"Content-Type": "application/json"} if data else {})
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)

    def reachable(self) -> bool:
        try:
            self._json("/system_stats")
            return True
        except (urllib.error.URLError, OSError):
            return False

    def render(self, prompt: str, seed: int) -> bytes:
        queued = self._json("/prompt", {
            "prompt": build_workflow(prompt, seed),
            "client_id": uuid.uuid4().hex})
        pid = queued["prompt_id"]
        deadline = time.monotonic() + RENDER_TIMEOUT_S
        while time.monotonic() < deadline:
            entry = self._json(f"/history/{pid}").get(pid)
            if entry:
                status = entry.get("status", {})
                if status.get("status_str") == "error":
                    raise RuntimeError(f"ComfyUI: {describe_comfy_error(status)}")
                for node in entry.get("outputs", {}).values():
                    for img in node.get("images", []):
                        q = urllib.parse.urlencode(
                            {"filename": img["filename"],
                             "subfolder": img.get("subfolder", ""),
                             "type": img.get("type", "output")})
                        with urllib.request.urlopen(f"{self.base}/view?{q}",
                                                    timeout=60) as r:
                            return r.read()
                raise RuntimeError("finished without an image")
            time.sleep(2)
        raise TimeoutError(f"no image after {RENDER_TIMEOUT_S}s")


# ── CLI ───────────────────────────────────────────────────────────────────────

def _take(args, flag):
    if flag not in args:
        return None
    i = args.index(flag)
    if i + 1 >= len(args):
        raise SystemExit(f"ERROR: {flag} needs a value")
    value = args[i + 1]
    del args[i:i + 2]
    return value


def main(args) -> int:
    args = list(args)
    server = _take(args, "--server") or DEFAULT_SERVER
    client = ComfyClient(server)
    free_prompt, free_out = _take(args, "--prompt"), _take(args, "--out")
    explicit_dir = _take(args, "--dir")
    publication = _take(args, "--publication") or "ai-pulse"
    only = _take(args, "--only")

    ask_pending = "--pending" in args
    if ask_pending:
        args.remove("--pending")
    ask_backfill = "--backfill" in args
    if ask_backfill:
        args.remove("--backfill")
    llm_url = _take(args, "--llm-url") or ip.DEFAULT_LLM_URL

    if not ask_pending and not ask_backfill and not client.reachable():
        print(f"ERROR: no ComfyUI at {server}")
        return 2

    if free_prompt:                                  # one-off, for trying a prompt
        if not free_out:
            print("ERROR: --prompt needs --out")
            return 1
        Path(free_out).write_bytes(client.render(full_prompt(free_prompt), 1))
        print(f"wrote {free_out}")
        return 0

    day = args[0] if args else date.today().isoformat()
    work_dir = issue_dir(explicit_dir)
    issue_path = work_dir / "issues" / publication / f"{day}.json"
    if not issue_path.exists():
        print(f"ERROR: no issue at {issue_path}")
        return 1
    issue = json.loads(issue_path.read_text(encoding="utf-8"))
    if ask_backfill:
        # Best effort by design: whatever happens here, the run goes on and the
        # posts without a prompt go out text-only.
        extra = ip.backfill(issue, ip.load_extra(work_dir, day, issue, publication),
                            ip.LlmClient(llm_url).ask)
        try:
            ip.write_sidecar(ip.sidecar_path(work_dir, day, publication), issue, extra)
        except OSError as e:
            print(f"WARNING: could not write the prompts file: {e}")
        print(f"{len(ip.merged(issue, extra))} of {len(issue.get('items', []))} items have a prompt")
        return 0
    extra = ip.load_extra(work_dir, day, issue, publication)
    if ask_pending:
        print(len(pending(issue, image_dir(work_dir, publication, day), extra)))
        return 0
    result = generate(issue, image_dir(work_dir, publication, day), client,
                      day=day, only=int(only) if only else None, extra=extra)
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
