"""image_prompts.py: the backstop for items the curator gave no image prompt.

The 9B curator treated `image_prompt` as skippable and wrote none on 2026-10-13.
These cover filling the gaps from the LLM — and, as much, what must NOT happen:
no item is ever lost, an existing prompt is never overwritten, and an LLM that
is down or rambling leaves that item text-only rather than failing the run.
"""

import json
import tempfile
import unittest
from pathlib import Path

import image_prompts as ip

ISSUE = {"items": [
    {"n": 1, "text": "Chips fall on AI financing worries.", "image_prompt": "A cracked piggy bank"},
    {"n": 2, "text": "Pentagon puts $350M into quantum computing."},
    {"n": 3, "text": "Nicolas Cage refuses an AI waiver."},
]}


class Merged(unittest.TestCase):
    def test_issue_prompt_wins_and_extras_fill_the_gaps(self):
        got = ip.merged(ISSUE, {1: "ignored", 2: "A glowing cube"})
        self.assertEqual(got, [(1, "A cracked piggy bank"), (2, "A glowing cube")])

    def test_blank_extras_are_ignored(self):
        self.assertEqual(ip.merged({"items": [{"text": "x"}]}, {1: "  "}), [])


class Instruction(unittest.TestCase):
    def test_the_backstop_asks_for_photographic_detail_not_a_medium(self):
        text = ip.SYSTEM.lower()
        for word in ("lighting", "material", "camera"):
            self.assertIn(word, text)
        self.assertIn("never depict a person", text)      # the safety rule survives


class Clean(unittest.TestCase):
    def test_strips_wrappers_and_labels(self):
        self.assertEqual(ip.clean('"Prompt: A paper boat on a circuit-board sea."'),
                         "A paper boat on a circuit-board sea.")
        self.assertEqual(ip.clean("**Scene:** A lighthouse made of keys\n"),
                         "A lighthouse made of keys")

    def test_rejects_empty_oversized_and_chatter(self):
        self.assertIsNone(ip.clean("   "))
        self.assertIsNone(ip.clean("x" * (ip.MAX_CHARS + 1)))
        self.assertIsNone(ip.clean("Sure! Here is a prompt for you: A boat"))
        self.assertIsNone(ip.clean("see https://example.com/a.png"))


class Backfill(unittest.TestCase):
    def test_asks_only_for_the_items_without_a_prompt(self):
        asked = []

        def ask(text):
            asked.append(text)
            return "A tidy scene"
        extra = ip.backfill(ISSUE, {}, ask)
        self.assertEqual(sorted(extra), [2, 3])
        self.assertEqual(asked, ["Pentagon puts $350M into quantum computing.",
                                 "Nicolas Cage refuses an AI waiver."])

    def test_does_not_mutate_its_inputs(self):
        before = {2: "kept"}
        extra = ip.backfill(ISSUE, before, lambda t: "A tidy scene")
        self.assertEqual(before, {2: "kept"})
        self.assertEqual(extra[2], "kept")          # existing extra not re-asked
        self.assertIn(3, extra)

    def test_one_llm_failure_leaves_that_item_text_only(self):
        def ask(text):
            if "Pentagon" in text:
                raise RuntimeError("llm down")
            return "A tidy scene"
        extra = ip.backfill(ISSUE, {}, ask, log=lambda m: None)
        self.assertEqual(sorted(extra), [3])

    def test_an_unusable_answer_leaves_the_item_text_only(self):
        extra = ip.backfill(ISSUE, {}, lambda t: "Sure! Here is a prompt: boat",
                            log=lambda m: None)
        self.assertEqual(extra, {})


class NoPeople(unittest.TestCase):
    def test_detects_people_words_but_not_hands_or_objects(self):
        for bad in ("a dark shadowy silhouette in the background",
                    "a lone figure walks past", "two workers at a console",
                    "a portrait of a woman", "a crowd gathers"):
            self.assertTrue(ip.mentions_person(bad), bad)
        for ok in ("a human hand pushes a contract away",
                   "a server rack in a concrete cavern",
                   "a robotic arm sorts parcels"):
            self.assertFalse(ip.mentions_person(ok), ok)

    def test_a_person_in_the_first_answer_gets_one_retry_with_a_reminder(self):
        asked = []

        def ask(text):
            asked.append(text)
            return ("A shadowy figure by a vault" if len(asked) == 1
                    else "A steel vault door ajar in a dim hall")
        extra = ip.backfill({"items": [{"text": "Bank hacked."}]}, {}, ask,
                            log=lambda m: None)
        self.assertEqual(extra, {1: "A steel vault door ajar in a dim hall"})
        self.assertEqual(len(asked), 2)
        self.assertIn("no people", asked[1].lower())

    def test_a_person_in_both_answers_leaves_the_post_without_an_image(self):
        extra = ip.backfill({"items": [{"text": "Bank hacked."}]}, {},
                            lambda t: "A man at a desk", log=lambda m: None)
        self.assertEqual(extra, {})


class Sidecar(unittest.TestCase):
    def test_round_trip_and_union_with_the_issue(self):
        with tempfile.TemporaryDirectory() as d:
            path = ip.sidecar_path(Path(d), "2026-10-13")
            ip.write_sidecar(path, ISSUE, {2: "A glowing cube"})
            self.assertEqual(json.loads(path.read_text()),
                             [{"n": 1, "prompt": "A cracked piggy bank"},
                              {"n": 2, "prompt": "A glowing cube"}])
            # Only the backfilled ones come back as extras: the issue's own
            # prompts are the issue's business.
            self.assertEqual(ip.load_extra(Path(d), "2026-10-13", ISSUE), {2: "A glowing cube"})

    def test_missing_or_corrupt_sidecar_means_no_extras(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(ip.load_extra(Path(d), "2026-10-13", ISSUE), {})
            ip.sidecar_path(Path(d), "2026-10-13").write_text("{not json")
            self.assertEqual(ip.load_extra(Path(d), "2026-10-13", ISSUE), {})


if __name__ == "__main__":
    unittest.main()
