import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import e5_anchor_answers as A


class FakeReply:
    def __init__(self, text, fr):
        self.text, self.finish_reason = text, fr

        class U:
            def as_dict(self):
                return {"completion_tokens": 7}
        self.usage = U()


class FakeClient:
    def __init__(self):
        self.calls = []

    def server_info(self):
        return {"model": "fake"}

    def chat(self, messages, seed=0, max_tokens=0):
        self.calls.append((messages, max_tokens))
        return FakeReply("12 #### 12", "stop")


def test_pick_is_deterministic_sorted_and_bounded():
    assert A.pick(100, 10) == A.pick(100, 10) == sorted(A.pick(100, 10))
    assert len(set(A.pick(100, 10))) == 10


def test_answer_one_uses_the_closed_prompt_and_limit():
    c = FakeClient()
    r = A.answer_one(c, "q?")
    assert r == {"text": "12 #### 12", "finish_reason": "stop", "completion_tokens": 7}
    msgs, mt = c.calls[0]
    assert msgs[0]["role"] == "system" and msgs[1] == {"role": "user", "content": "q?"} and mt == 512


def test_resume_skips_rows_on_disk(tmp_path, monkeypatch):
    prompts = tmp_path / "p.jsonl"
    prompts.write_text("".join(json.dumps({"question": f"q{i}"}) + "\n" for i in range(20)))
    out = tmp_path / "o.jsonl"
    fake = FakeClient()
    monkeypatch.setattr(A, "Client", lambda *a, **k: fake)
    assert A.main(["--prompts", str(prompts), "--out", str(out), "--n", "5"]) == 0
    first = out.read_text()
    assert len(fake.calls) == 5
    assert A.main(["--prompts", str(prompts), "--out", str(out), "--n", "5"]) == 0
    assert len(fake.calls) == 5 and out.read_text() == first
    assert {json.loads(l)["idx"] for l in first.splitlines()} == set(A.pick(20, 5))
