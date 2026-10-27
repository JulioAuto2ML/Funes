import json
import tempfile
import unittest
from pathlib import Path

import generate_images as gi


ISSUE = {"items": [
    {"n": 1, "text": "a", "image_prompt": "A milk bottle wired to a circuit board"},
    {"n": 2, "text": "b"},
    {"n": 3, "text": "c", "image_prompt": "   "},
    {"n": 4, "text": "d", "image_prompt": "A robot arm sorting parcels"},
]}


class PromptsFor(unittest.TestCase):
    def test_only_items_with_a_real_prompt(self):
        self.assertEqual([n for n, _ in gi.prompts_for(ISSUE)], [1, 4])

    def test_slot_number_is_position_not_n_field(self):
        issue = {"items": [{"text": "x", "image_prompt": "p"}]}
        self.assertEqual(gi.prompts_for(issue), [(1, "p")])

    def test_no_items(self):
        self.assertEqual(gi.prompts_for({}), [])


class HouseStyle(unittest.TestCase):
    def test_style_asks_for_a_realistic_futuristic_photograph(self):
        style = gi.STYLE.lower()
        for word in ("photorealistic", "futuristic", "lighting", "lens"):
            self.assertIn(word, style)

    def test_style_does_not_ask_for_the_old_cartoon_look(self):
        style = gi.STYLE.lower()
        for word in ("flat", "cartoon", "illustration", "gradient"):
            self.assertNotIn(word, style)

    def test_negative_pushes_away_from_cartoons(self):
        for word in ("cartoon", "illustration", "vector"):
            self.assertIn(word, gi.NEGATIVE.lower())


class Workflow(unittest.TestCase):
    def test_prompt_gets_the_house_style_appended(self):
        p = gi.full_prompt("A robot arm")
        self.assertTrue(p.startswith("A robot arm"))
        self.assertIn(gi.STYLE, p)

    def test_workflow_wires_prompt_seed_and_size(self):
        wf = gi.build_workflow("hello", seed=7, width=1280, height=672)
        by_type = {}
        for node in wf.values():
            by_type.setdefault(node["class_type"], []).append(node)
        self.assertEqual(by_type["KSampler"][0]["inputs"]["seed"], 7)
        latent = by_type["EmptyLatentImage"][0]["inputs"]
        self.assertEqual((latent["width"], latent["height"]), (1280, 672))
        positives = [n for n in by_type["TextEncodeQwenImage21"]
                     if n["inputs"]["prompt"] == "hello"]
        self.assertEqual(len(positives), 1)

    def test_every_link_points_at_a_node_that_exists(self):
        wf = gi.build_workflow("x", seed=1)
        for node in wf.values():
            for v in node["inputs"].values():
                if isinstance(v, list):
                    self.assertIn(v[0], wf)


class FakeClient:
    def __init__(self, fail_on=()):
        self.fail_on, self.calls = set(fail_on), []

    def render(self, prompt, seed):
        self.calls.append(prompt)
        if any(f in prompt for f in self.fail_on):
            raise RuntimeError("boom")
        return b"\x89PNG-fake"


class Generate(unittest.TestCase):
    def test_writes_one_file_per_prompt(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            res = gi.generate(ISSUE, out, FakeClient())
            self.assertEqual(sorted(p.name for p in out.iterdir()), ["1.png", "4.png"])
            self.assertEqual(res, {"made": [1, 4], "skipped": [], "failed": [], "aborted": []})

    def test_one_failure_does_not_stop_the_batch(self):
        with tempfile.TemporaryDirectory() as d:
            res = gi.generate(ISSUE, Path(d), FakeClient(fail_on=["milk"]))
            self.assertEqual(res["failed"], [1])
            self.assertEqual(res["made"], [4])

    def test_existing_images_are_not_regenerated(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            (out / "1.png").write_bytes(b"old")
            client = FakeClient()
            res = gi.generate(ISSUE, out, client)
            self.assertEqual(res["skipped"], [1])
            self.assertEqual((out / "1.png").read_bytes(), b"old")
            self.assertEqual(len(client.calls), 1)


class CircuitBreaker(unittest.TestCase):
    ISSUE5 = {"items": [{"text": "x", "image_prompt": f"p{i}"} for i in range(5)]}

    def test_two_failures_in_a_row_abort_the_batch(self):
        # A wedged ComfyUI answers every remaining item with a long timeout; the
        # model is offline for all of it. Better to stop and post text-only.
        with tempfile.TemporaryDirectory() as d:
            client = FakeClient(fail_on=["p"])
            res = gi.generate(self.ISSUE5, Path(d), client)
            self.assertEqual(len(client.calls), gi.MAX_CONSECUTIVE_FAILURES)
            self.assertEqual(res["failed"], [1, 2])
            self.assertEqual(res["aborted"], [3, 4, 5])

    def test_a_success_resets_the_count(self):
        with tempfile.TemporaryDirectory() as d:
            client = FakeClient(fail_on=["p0", "p2", "p4"])
            res = gi.generate(self.ISSUE5, Path(d), client)
            self.assertEqual(res["failed"], [1, 3, 5])
            self.assertEqual(res["made"], [2, 4])
            self.assertEqual(res["aborted"], [])


class ErrorText(unittest.TestCase):
    def test_comfy_error_is_reduced_to_its_message(self):
        status = {"status_str": "error", "messages": [
            ["execution_start", {}],
            ["execution_error", {"exception_message": "CUDA out of memory. " * 50,
                                 "current_inputs": {"samples": ["tensor(" + "9" * 5000 + ")"]}}]]}
        text = gi.describe_comfy_error(status)
        self.assertIn("CUDA out of memory", text)
        self.assertLess(len(text), 400)
        self.assertNotIn("9999", text)

    def test_an_unrecognised_status_still_gives_something_short(self):
        self.assertLess(len(gi.describe_comfy_error({"status_str": "error"} | {"x": "y" * 9000})), 400)


class Pending(unittest.TestCase):
    def test_counts_prompts_with_no_image_yet(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            self.assertEqual(gi.pending(ISSUE, out), [1, 4])
            (out / "1.png").write_bytes(b"x")
            self.assertEqual(gi.pending(ISSUE, out), [4])

    def test_nothing_pending_for_an_issue_without_prompts(self):
        self.assertEqual(gi.pending({"items": [{"text": "a"}]}, Path("/nonexistent")), [])

    def test_cli_pending_prints_the_count_and_needs_no_server(self):
        # run_images.sh asks this before touching the GPU, with ComfyUI down.
        with tempfile.TemporaryDirectory() as d:
            issue = Path(d) / "issues" / "ai-pulse" / "2026-07-30.json"
            issue.parent.mkdir(parents=True)
            issue.write_text(json.dumps(ISSUE))
            import contextlib, io
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = gi.main(["2026-07-30", "--dir", d, "--pending"])
            self.assertEqual((rc, out.getvalue().strip()), (0, "2"))

    def test_cli_pending_for_a_missing_issue_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(gi.main(["2026-07-30", "--dir", d, "--pending"]), 1)


class WithBackfilledPrompts(unittest.TestCase):
    def test_extras_fill_gaps_in_prompts_for_and_pending(self):
        self.assertEqual([n for n, _ in gi.prompts_for(ISSUE, {2: "A cube", 1: "ignored"})],
                         [1, 2, 4])
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(gi.pending(ISSUE, Path(d), {2: "A cube"}), [1, 2, 4])

    def test_generate_renders_a_backfilled_slot(self):
        with tempfile.TemporaryDirectory() as d:
            client = FakeClient()
            res = gi.generate(ISSUE, Path(d), client, extra={2: "A glowing cube"})
            self.assertEqual(res["made"], [1, 2, 4])
            self.assertIn("A glowing cube", " ".join(client.calls))

    def test_cli_backfill_writes_the_sidecar_and_counts(self):
        import contextlib, io
        with tempfile.TemporaryDirectory() as d:
            issue = Path(d) / "issues" / "ai-pulse" / "2026-07-30.json"
            issue.parent.mkdir(parents=True)
            issue.write_text(json.dumps(ISSUE))

            class Fake:
                def __init__(self, *a, **k): pass
                def ask(self, text): return "A tidy scene"
            real, gi.ip.LlmClient = gi.ip.LlmClient, Fake
            try:
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    rc = gi.main(["2026-07-30", "--dir", d, "--backfill"])
            finally:
                gi.ip.LlmClient = real
            self.assertEqual(rc, 0)
            rows = json.loads((Path(d) / "image_prompts_2026-07-30.json").read_text())
            self.assertEqual([r["n"] for r in rows], [1, 2, 3, 4])
            # and another publication's backfill lands in a file of its own
            issue_es = Path(d) / "issues" / "ai-pulse-es" / "2026-07-30.json"
            issue_es.parent.mkdir(parents=True)
            issue_es.write_text(json.dumps(ISSUE))
            real, gi.ip.LlmClient = gi.ip.LlmClient, Fake
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    gi.main(["2026-07-30", "--dir", d, "--publication", "ai-pulse-es",
                             "--backfill"])
            finally:
                gi.ip.LlmClient = real
            self.assertTrue((Path(d) / "image_prompts_ai-pulse-es_2026-07-30.json").exists())
            # --pending now sees the backfilled slots too.
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                gi.main(["2026-07-30", "--dir", d, "--pending"])
            self.assertEqual(out.getvalue().strip().splitlines()[-1], "4")

    def test_cli_backfill_survives_an_unreachable_llm(self):
        import contextlib, io
        with tempfile.TemporaryDirectory() as d:
            issue = Path(d) / "issues" / "ai-pulse" / "2026-07-30.json"
            issue.parent.mkdir(parents=True)
            issue.write_text(json.dumps(ISSUE))

            class Down:
                def __init__(self, *a, **k): pass
                def ask(self, text): raise OSError("connection refused")
            real, gi.ip.LlmClient = gi.ip.LlmClient, Down
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    rc = gi.main(["2026-07-30", "--dir", d, "--backfill"])
            finally:
                gi.ip.LlmClient = real
            self.assertEqual(rc, 0)       # garnish: never a reason to fail the run


class Paths(unittest.TestCase):
    def test_image_dir_layout(self):
        self.assertEqual(gi.image_dir(Path("/x"), "ai-pulse", "2026-10-12"),
                         Path("/x/images/ai-pulse/2026-10-12"))


if __name__ == "__main__":
    unittest.main()
