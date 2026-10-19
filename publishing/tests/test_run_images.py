"""run_images.sh: the GPU swap.

What matters is order and the trap. Funes' model must be stopped before ComfyUI
starts, ComfyUI must be gone before the model comes back, and the model must
come back no matter how the middle went. Every collaborator is a fake that
appends to one shared log, so the test reads the sequence of events.
"""

import json
import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "run_images.sh"
DAY = "2026-07-30"
PNG = b"\x89PNG\r\n\x1a\n" + b"0" * 8


class Env:
    def __init__(self, tmp, status="sent", prompts=True, images=(),
                 generate_rc=0, vram_frees=True, comfy_starts=True):
        self.dir = Path(tmp)
        self.log = self.dir / "events.log"
        self.log.write_text("")
        pub = self.dir / "work"
        (pub / "runs" / "ai-pulse").mkdir(parents=True)
        (pub / "issues" / "ai-pulse").mkdir(parents=True)
        if status:
            (pub / "runs" / "ai-pulse" / f"{DAY}.json").write_text(
                json.dumps({"status": status}))
        items = [{"n": 1, "text": "a", **({"image_prompt": "p1"} if prompts else {})},
                 {"n": 2, "text": "b", **({"image_prompt": "p2"} if prompts else {})}]
        (pub / "issues" / "ai-pulse" / f"{DAY}.json").write_text(
            json.dumps({"items": items}))
        imgdir = pub / "images" / "ai-pulse" / DAY
        imgdir.mkdir(parents=True)
        for n in images:
            (imgdir / f"{n}.png").write_bytes(PNG)
        self.pub = pub

        self.ctl = self._script("ctl.sh", f'echo "ctl $*" >> {self.log}\n')
        self.comfy = self._script("comfy.py", f"""\
            import http.server, signal, sys
            log = open("{self.log}", "a", buffering=1)
            class H(http.server.BaseHTTPRequestHandler):
                def do_GET(self):
                    self.send_response(200); self.end_headers(); self.wfile.write(b"{{}}")
                def log_message(self, *a): pass
            def bye(*a):
                log.write("comfy-down\\n"); sys.exit(0)
            signal.signal(signal.SIGTERM, bye)
            {"" if comfy_starts else "sys.exit(3)"}
            srv = http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H)
            log.write("comfy-up\\n")
            srv.serve_forever()
            """, shebang="#!/usr/bin/env python3\n")
        self.generate = self._script(
            "generate.sh", f'echo "generate $*" >> {self.log}\nexit {generate_rc}\n')
        # VRAM: "used" starts high and drops once the LLM has been stopped.
        flag = self.dir / "llm_stopped"
        self.vram = self._script(
            "vram.sh",
            f'if grep -q "ctl stop" {self.log}; then echo '
            f'{"100" if vram_frees else "14000"}; else echo 14000; fi\n')
        self.backfill = self._script(
            "backfill.sh",
            f'echo backfill >> {self.log}\n'
            f'python3 - <<PY\nimport json,pathlib\n'
            f'p=pathlib.Path("{self.pub}/issues/ai-pulse/{DAY}.json")\n'
            f'd=json.loads(p.read_text())\n'
            f'[i.update(image_prompt="filled") for i in d["items"]]\n'
            f'p.write_text(json.dumps(d))\nPY\n')
        self.health = self._script(
            "health.sh",
            f'[ "$(grep -c "ctl start" {self.log})" -ge 1 ]\n')

    def _script(self, name, body, shebang="#!/usr/bin/env bash\n"):
        path = self.dir / name
        path.write_text(shebang + textwrap.dedent(body))
        path.chmod(0o755)
        return path

    def run(self, **extra):
        env = dict(os.environ,
                   FUNES_PUBLISH_DIR=str(self.pub),
                   RUN_DATE=DAY,
                   LLAMA_CTL=str(self.ctl),
                   COMFY_CMD=f"exec python3 {self.comfy} 18999",
                   COMFY_PORT="18999",
                   GENERATE_CMD=f"{self.generate}",
                   VRAM_USED_MB_CMD=str(self.vram),
                   HEALTH_CMD=str(self.health),
                   LOCK_FILE=str(self.dir / "lock"),
                   WAIT_SENT_S="2", POLL_S="1", VRAM_WAIT_S="3",
                   COMFY_WAIT_S="10", LLM_WAIT_S="5", GENERATE_TIMEOUT_S="20",
                   **{"BACKFILL_CMD": "true",   # never reach for a real LLM in a test
                      "LLAMA_PID_DIR": str(self.dir / "no-pids"),   # not this host's
                      **extra})
        p = subprocess.run(["bash", str(SCRIPT), "ai-pulse"], env=env,
                           capture_output=True, text=True, timeout=60)
        events = [l for l in self.log.read_text().splitlines() if l.strip()]
        return p, events


class Swap(unittest.TestCase):
    def test_the_whole_sequence_in_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, ev = Env(tmp).run()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(ev[0], "ctl stop qwen9b")
        self.assertEqual(ev[1], "comfy-up")
        self.assertTrue(ev[2].startswith("generate"))
        self.assertEqual(ev[3:], ["comfy-down", "ctl start qwen9b"])

    def test_it_swaps_whichever_model_is_serving_the_port(self):
        # 2026-10-18: Funes had been moved to qwen27b the evening before. The
        # fixed qwen9b stopped nothing (its pid file was stale), the VRAM never
        # freed, and no images were made.
        with tempfile.TemporaryDirectory() as tmp:
            e = Env(tmp)
            pids = e.dir / "pids"
            pids.mkdir()
            (pids / "llama_qwen9b.pid").write_text("999999\n")            # stale
            other = subprocess.Popen(["python3", "-c", "import time; time.sleep(60)",
                                      "--port", "8081", "--host", "x"])
            served = subprocess.Popen(["python3", "-c", "import time; time.sleep(60)",
                                       "--port", "8080", "--host", "x"])
            try:
                (pids / "llama_embed.pid").write_text(f"{other.pid}\n")
                (pids / "llama_qwen27b.pid").write_text(f"{served.pid}\n")
                p, ev = e.run(LLAMA_PID_DIR=str(pids))
            finally:
                other.kill(); served.kill()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(ev[0], "ctl stop qwen27b")
        self.assertEqual(ev[-1], "ctl start qwen27b")

    def test_the_model_comes_back_when_generation_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, ev = Env(tmp, generate_rc=7).run()
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(ev[-2:], ["comfy-down", "ctl start qwen9b"])

    def test_the_model_comes_back_when_comfy_never_starts(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, ev = Env(tmp, comfy_starts=False).run()
        self.assertNotEqual(p.returncode, 0)
        self.assertNotIn("generate", " ".join(ev))
        self.assertEqual(ev[-1], "ctl start qwen9b")

    def test_no_generation_when_the_vram_never_frees(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, ev = Env(tmp, vram_frees=False).run()
        self.assertEqual(p.returncode, 4)
        self.assertNotIn("comfy-up", ev)
        self.assertEqual(ev, ["ctl stop qwen9b", "ctl start qwen9b"])

    def test_comfy_is_really_gone_afterwards(self):
        with tempfile.TemporaryDirectory() as tmp:
            Env(tmp).run()
            gone = subprocess.run(["pgrep", "-f", "comfy.py 18999"],
                                  capture_output=True).returncode != 0
        self.assertTrue(gone)


class TheLock(unittest.TestCase):
    def test_a_daemon_started_by_the_run_does_not_keep_the_lock(self):
        # `launch_llama.sh start` backgrounds llama-server from inside this
        # script. If that child inherits the lock's file descriptor it holds the
        # lock for as long as the model runs, and every later run exits 6.
        with tempfile.TemporaryDirectory() as tmp:
            e = Env(tmp)
            e.ctl.write_text(
                f'#!/usr/bin/env bash\necho "ctl $*" >> {e.log}\n'
                f'if [ "$1" = start ]; then sleep 40 >/dev/null 2>&1 & echo $! > {e.dir}/daemon.pid; fi\n'
                f'exit 0\n')
            first, _ = e.run()
            try:
                second, ev = e.run()
            finally:
                try:
                    os.kill(int((e.dir / "daemon.pid").read_text()), 15)
                except (OSError, ValueError):
                    pass
        self.assertEqual(first.returncode, 0, first.stdout)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)


class Backfill(unittest.TestCase):
    def test_backfill_runs_while_the_model_is_still_up(self):
        with tempfile.TemporaryDirectory() as tmp:
            e = Env(tmp, prompts=False)
            p, ev = e.run(BACKFILL_CMD=f"{e.backfill}")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(ev[0], "backfill")           # before ctl stop
        self.assertEqual(ev[1], "ctl stop qwen9b")

    def test_a_failed_backfill_does_not_stop_the_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            e = Env(tmp)
            e.backfill.write_text(f'#!/usr/bin/env bash\necho backfill >> {e.log}\nexit 9\n')
            p, ev = e.run(BACKFILL_CMD=f"{e.backfill}")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("ctl stop qwen9b", ev)


class Cancelling(unittest.TestCase):
    def test_sigterm_mid_render_restores_the_model_promptly(self):
        # Bash holds a trap until the foreground command finishes, so a render
        # run in the foreground would keep the model stopped for the whole batch.
        import signal, time
        with tempfile.TemporaryDirectory() as tmp:
            e = Env(tmp)
            e.generate.write_text(
                f'#!/usr/bin/env bash\necho "generate" >> {e.log}\nsleep 60\n')
            env = dict(os.environ, FUNES_PUBLISH_DIR=str(e.pub), RUN_DATE=DAY,
                       LLAMA_CTL=str(e.ctl),
                       COMFY_CMD=f"exec python3 {e.comfy} 18999", COMFY_PORT="18999",
                       GENERATE_CMD=str(e.generate), VRAM_USED_MB_CMD=str(e.vram),
                       HEALTH_CMD=str(e.health), LOCK_FILE=str(e.dir / "lock"),
                       LLAMA_PID_DIR=str(e.dir / "no-pids"),
                       BACKFILL_CMD="true",
                       WAIT_SENT_S="2", POLL_S="1", VRAM_WAIT_S="3", COMFY_WAIT_S="10",
                       LLM_WAIT_S="5", GENERATE_TIMEOUT_S="120")
            proc = subprocess.Popen(["bash", str(SCRIPT), "ai-pulse"], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            deadline = time.time() + 20
            while "generate" not in e.log.read_text() and time.time() < deadline:
                time.sleep(0.2)
            proc.send_signal(signal.SIGTERM)
            started = time.time()
            proc.wait(timeout=30)
            took = time.time() - started
            events = [l for l in e.log.read_text().splitlines() if l.strip()]
        self.assertLess(took, 15)
        self.assertEqual(events[-2:], ["comfy-down", "ctl start qwen9b"])


class LeavesTheGpuAlone(unittest.TestCase):
    def assert_untouched(self, p, ev, code):
        self.assertEqual(p.returncode, code, p.stdout + p.stderr)
        self.assertEqual(ev, [])

    def test_an_issue_that_was_not_sent(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assert_untouched(*Env(tmp, status="blocked").run(), 2)

    def test_no_run_record_at_all(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assert_untouched(*Env(tmp, status=None).run(), 2)

    def test_an_issue_with_no_prompts(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assert_untouched(*Env(tmp, prompts=False).run(), 0)

    def test_every_image_already_exists(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assert_untouched(*Env(tmp, images=(1, 2)).run(), 0)


if __name__ == "__main__":
    unittest.main()
