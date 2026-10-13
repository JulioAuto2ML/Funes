"""
image_prompts.py — illustration prompts the curator did not write.

`publish_issue` takes an `image_prompt` per item, but the 9B model that runs the
curator treated it as skippable and sent none (2026-10-13). So the prompts are
backstopped here: before the GPU is lent to ComfyUI, the LLM is still up, and one
short, single-purpose call per item is far more reliable than one more field in a
28-step tool loop.

The issue JSON is the source of truth and is never edited. What this adds goes
into `image_prompts_<date>.json` — the same file `publish_issue` writes — as the
union of the issue's own prompts and the backfilled ones. An item that still has
no prompt (LLM down, or an answer that fails `clean`) simply gets no image;
nothing here can fail the run or lose an item.
"""

import json
import re
import urllib.request
from pathlib import Path

# Same bound the C++ side puts on a prompt (kMaxImagePromptChars).
MAX_CHARS = 600

DEFAULT_LLM_URL = "http://127.0.0.1:8080"

SYSTEM = (
    "You write picture descriptions for a text-to-image model, to illustrate a "
    "news post. Describe a concrete visual metaphor for the story as a "
    "photograph: the subject, the setting, the materials and surfaces, the "
    "lighting and atmosphere, and the camera angle or framing. Rules: never "
    "depict a person or a face (use objects, animals, machines or architecture); "
    "no logos or brand names; no words, letters or numbers in the picture. Do not "
    "name an art style or medium — a photorealistic futuristic look is added for "
    "you. Answer with the description only: two or three sentences, no preamble.")

_LABEL = re.compile(r"^\s*(image prompt|prompt|scene|illustration|description)\s*:\s*",
                    re.IGNORECASE)
_CHATTER = re.compile(r"^(sure|certainly|of course|okay|ok|here\b|i['’ ]|as an ai)",
                      re.IGNORECASE)


# The prompt rules say no people; a 9B model wrote "a dark, shadowy silhouette"
# anyway (2026-10-13). A real face or body next to a real headline reads as a
# claim, so this is checked rather than trusted. "Hand" and "arm" are allowed.
_PERSON = re.compile(
    r"\b(silhouettes?|figures?|persons?|people|peoples|man|men|woman|women|"
    r"child|children|kids?|crowds?|workers?|employees?|faces?|portraits?|"
    r"eyes?|ceo|president|executives?|crew|passengers?|pilots?|staff)\b", re.IGNORECASE)

REMINDER = ("\n(Reminder: at most three sentences and 450 characters. No people, "
            "figures, faces, eyes or silhouettes at all — describe only objects, "
            "machines, buildings or landscapes.)")


def mentions_person(text: str) -> bool:
    return bool(_PERSON.search(text or ""))


def sidecar_path(work_dir: Path, day: str) -> Path:
    return work_dir / f"image_prompts_{day}.json"


def merged(issue: dict, extra: dict) -> list:
    """[(slot, prompt)]: the issue's own prompt for a slot wins, else the extra."""
    out = []
    for slot, item in enumerate(issue.get("items", []), start=1):
        prompt = (item.get("image_prompt") or "").strip() or (extra.get(slot) or "").strip()
        if prompt:
            out.append((slot, prompt))
    return out


def clean(text) -> "str | None":
    """A model's answer reduced to a prompt, or None if it is not usable."""
    if not isinstance(text, str):
        return None
    t = " ".join(text.replace("**", "").split())
    t = t.strip("\"'“”`").strip()
    t = _LABEL.sub("", t).strip("\"'“”`").strip()
    if not t or len(t) > MAX_CHARS or _CHATTER.match(t) or "http" in t.lower():
        return None
    # Quoted words are lettering the image model will try to draw ("STOP" sign),
    # and the rule is no words in the picture. Rejecting sends it to the retry.
    if re.search(r'["\u201c\u201d]', t):
        return None
    return t


def backfill(issue: dict, extra: dict, ask, log=lambda m: print(m, flush=True)) -> dict:
    """A new extras dict: `extra` plus a prompt for every item that has none.
    `ask(post_text) -> str` may raise; that item is then left without one."""
    result = dict(extra)
    for slot, item in enumerate(issue.get("items", []), start=1):
        if (item.get("image_prompt") or "").strip() or slot in result:
            continue
        try:
            post = item.get("text", "")
            prompt = clean(ask(post))
            if prompt is None or mentions_person(prompt):
                why = "was not usable" if prompt is None else "pictured a person"
                log(f"[{slot}] the answer {why}; asking once more")
                prompt = clean(ask(post + REMINDER))
                if prompt is not None and mentions_person(prompt):
                    log(f"[{slot}] still a person; this post goes without an image")
                    continue
        except Exception as e:                       # noqa: BLE001 — logged, not hidden
            log(f"[{slot}] no prompt written: {e}")
            continue
        if prompt is None:
            log(f"[{slot}] the model's answer was not usable; this post goes without an image")
            continue
        result[slot] = prompt
        log(f"[{slot}] backfilled: {prompt}")
    return result


def write_sidecar(path: Path, issue: dict, extra: dict) -> None:
    rows = [{"n": slot, "prompt": prompt} for slot, prompt in merged(issue, extra)]
    path.write_text(json.dumps(rows, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def load_extra(work_dir: Path, day: str, issue: dict) -> dict:
    """The backfilled prompts on disk — only for slots whose item has no prompt
    of its own. Missing or corrupt means none: this is garnish, not a gate."""
    try:
        rows = json.loads(sidecar_path(work_dir, day).read_text(encoding="utf-8"))
        extra = {int(r["n"]): str(r["prompt"]).strip() for r in rows}
    except (OSError, ValueError, KeyError, TypeError):
        return {}
    items = issue.get("items", [])
    return {slot: p for slot, p in extra.items()
            if p and 1 <= slot <= len(items) and not (items[slot - 1].get("image_prompt") or "").strip()}


class LlmClient:
    """The local llama-server, OpenAI-compatible. Thinking is off: a reasoning
    model's `<think>` block is not a picture description."""

    def __init__(self, base_url: str = DEFAULT_LLM_URL, timeout: int = 120):
        self.base, self.timeout = base_url.rstrip("/"), timeout

    def ask(self, post_text: str) -> str:
        body = json.dumps({
            "messages": [{"role": "system", "content": SYSTEM},
                         {"role": "user", "content": f"News post: {post_text}"}],
            "temperature": 0.7, "max_tokens": 200,
            "chat_template_kwargs": {"enable_thinking": False}}).encode()
        req = urllib.request.Request(self.base + "/v1/chat/completions", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            return json.load(r)["choices"][0]["message"]["content"]
