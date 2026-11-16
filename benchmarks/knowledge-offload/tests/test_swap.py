import datetime as dt
import importlib.util
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ko_swap", HERE.parents[2] / "scripts" / "ko_swap.py")
K = importlib.util.module_from_spec(spec)
spec.loader.exec_module(K)

CRONTAB = (HERE.parents[2] / "scripts" / "yoda.crontab").read_text()
WINDOWS = K.blocked_windows(CRONTAB)
MON = dt.datetime(2026, 11, 16)          # a Monday
SUN = dt.datetime(2026, 11, 22)


def at(day, h, m=0):
    return day.replace(hour=h, minute=m)


def test_every_size_gets_the_same_flags_but_the_model():
    a, b = K.server_args(Path("/m/Qwen3.5-0.8B-IQ4_XS.gguf")), K.server_args(Path("/m/Qwen3.5-27B-IQ4_XS.gguf"))
    strip = lambda x: [t for t in x if not t.endswith(".gguf")]
    assert strip(a) == strip(b)
    assert "-ctk" not in a and a[a.index("--ctx-size") + 1] == "8192" and "--mmproj" not in a
    assert '{"enable_thinking":false}' in a and "--jinja" in a and a[a.index("--parallel") + 1] == "1"


def test_q4_kv_fallback_applies_to_every_size():
    for f in ("Qwen3.5-0.8B-IQ4_XS.gguf", "Qwen3.5-27B-IQ4_XS.gguf"):
        a = K.server_args(Path(f), "q4_0")
        assert a[a.index("-ctk") + 1] == "q4_0" and a[a.index("-ctv") + 1] == "q4_0"


def test_kv_choice_defaults_to_f16_and_reads_the_recorded_decision(tmp_path):
    assert K.kv_choice(tmp_path / "none") == "f16"
    (tmp_path / "kv").write_text("q4_0\n")
    assert K.kv_choice(tmp_path / "kv") == "q4_0"
    (tmp_path / "kv").write_text("garbage")
    assert K.kv_choice(tmp_path / "kv") == "f16"


def test_windows_come_from_the_crontab_and_the_funes_jobs():
    assert K.blocking(at(MON, 6, 40), WINDOWS) == "news"
    assert K.blocking(at(MON, 9, 10), WINDOWS) == "AI Pulse issue"          # 15 min lead before 09:00
    assert K.blocking(at(MON, 9, 50), WINDOWS) == "run_images.sh"           # 09:30 + 25
    assert K.blocking(at(MON, 10, 0), WINDOWS) is None
    assert K.blocking(at(MON, 14, 0), WINDOWS) is None                      # post_tweet is not a GPU job
    assert K.blocking(at(MON + dt.timedelta(days=1), 10, 20), WINDOWS) == "AI Pulse ES issue"
    assert K.blocking(at(SUN, 13, 10), WINDOWS) == "weekly_carousel.py"
    assert K.blocking(at(SUN, 9, 10), WINDOWS) is None                      # no issue on Sunday


def test_next_block_finds_the_start_with_its_lead():
    t, what = K.next_block(at(MON, 10, 0), WINDOWS)
    assert what == "news" and t == dt.datetime(2026, 11, 17, 6, 15)
    t, what = K.next_block(at(SUN, 7, 0), WINDOWS)
    assert what == "weekly_carousel.py" and t == dt.datetime(2026, 11, 22, 12, 45)


class Proc:
    def __init__(self, polls, rc=0):
        self.polls, self.rc, self.pid, self.terminated = list(polls), rc, 4242, False

    def poll(self):
        return self.polls.pop(0) if self.polls else self.rc

    def terminate(self):
        self.terminated = True
        self.polls = [-15]

    def kill(self): pass

    @property
    def returncode(self): return self.rc


def harness(tmp_path, monkeypatch, *, now, child_polls=(None, 0), health=True, verify_ok=True):
    for name, val in (("PID_FILE", tmp_path / "ko.pid"), ("PROD_PID", tmp_path / "prod.pid"), ("LOG_DIR", tmp_path),
                      ("SWAP_LOG", tmp_path / "swaps.log"), ("KV_FILE", tmp_path / "kv")):
        monkeypatch.setattr(K, name, val)
    sh_calls, spawned, clock = [], [], {"t": now}

    def spawn(args, **kw):
        p = Proc([None, None] if args[0].endswith("llama-server") else list(child_polls),
                 rc=0)
        spawned.append((args, p))
        return p

    def sleep(s):
        clock["t"] += dt.timedelta(seconds=s)

    def verify(lock_id):
        if not verify_ok:
            raise SystemExit("not in models.lock")
        return Path("/m/x.gguf")
    sw = K.Swap(now=lambda: clock["t"], sleep=sleep, healthy=lambda timeout=2.0: health, spawn=spawn,
                sh=lambda cmd, **kw: sh_calls.append(cmd), verify=verify, windows=WINDOWS)
    return sw, sh_calls, spawned


def launches(sh_calls):
    return [c[1:] for c in sh_calls]


def test_run_swaps_runs_and_restores_the_production_model(tmp_path, monkeypatch):
    sw, sh, spawned = harness(tmp_path, monkeypatch, now=at(MON, 10, 0))
    assert sw.run("qwen3.5-9b-iq4_xs", ["python3", "run.py"]) == 0
    assert launches(sh) == [["stop", "qwen27b"], ["stop", "qwen9b"], ["start", "qwen27b"]]
    assert spawned[0][0][0].endswith("llama-server") and spawned[1][0] == ["python3", "run.py"]


def test_restore_happens_when_the_command_fails(tmp_path, monkeypatch):
    sw, sh, spawned = harness(tmp_path, monkeypatch, now=at(MON, 10, 0), child_polls=(None, 3))
    orig = sw.spawn

    def spawn(args, **kw):
        p = orig(args, **kw)
        if args[0] == "python3":
            p.rc = 3
        return p
    sw.spawn = spawn
    assert sw.run("qwen3.5-9b-iq4_xs", ["python3", "run.py"]) == 3
    assert launches(sh)[-1] == ["start", "qwen27b"]


def test_restore_happens_when_the_server_never_gets_healthy(tmp_path, monkeypatch):
    sw, sh, _ = harness(tmp_path, monkeypatch, now=at(MON, 10, 0), health=False)
    with pytest.raises(SystemExit):                         # restore itself gives up: production not healthy
        sw.run("qwen3.5-9b-iq4_xs", ["python3", "run.py"])
    assert ["start", "qwen27b"] in launches(sh)


def test_refuses_inside_a_window_and_touches_nothing(tmp_path, monkeypatch):
    sw, sh, spawned = harness(tmp_path, monkeypatch, now=at(MON, 9, 20))
    with pytest.raises(SystemExit, match="needs the GPU"):
        sw.run("qwen3.5-9b-iq4_xs", ["python3", "run.py"])
    assert sh == [] and spawned == []


def test_max_minutes_refuses_a_run_that_would_hit_the_next_window(tmp_path, monkeypatch):
    sw, sh, spawned = harness(tmp_path, monkeypatch, now=at(MON, 10, 0))
    with pytest.raises(SystemExit, match="inside the requested"):
        sw.run("qwen3.5-9b-iq4_xs", ["python3", "run.py"], max_minutes=24 * 60)
    assert sh == [] and spawned == []


def test_an_unlocked_model_is_refused_before_production_is_stopped(tmp_path, monkeypatch):
    sw, sh, spawned = harness(tmp_path, monkeypatch, now=at(MON, 10, 0), verify_ok=False)
    with pytest.raises(SystemExit, match="models.lock"):
        sw.run("whatever", ["python3", "run.py"])
    assert not any(c[1] == "stop" for c in sh)


def test_a_window_about_to_open_stops_the_run_and_restores(tmp_path, monkeypatch):
    # Monday 18:00 .. next block Tue 06:15 is far; start 06:10 Tuesday instead: block opens in 5 min
    sw, sh, spawned = harness(tmp_path, monkeypatch, now=dt.datetime(2026, 11, 17, 5, 55), child_polls=[None] * 100)
    # 5:55 is already within 15 min of 06:30's lead (06:15)? 06:15 - 05:55 = 20 min: allowed to start
    assert sw.run("qwen3.5-9b-iq4_xs", ["python3", "run.py"]) == 75
    child = spawned[1][1]
    assert child.terminated and launches(sh)[-1] == ["start", "qwen27b"]


def test_identity_check_counts_differing_predictions():
    sys.path.insert(0, str(HERE.parent))
    import identity_check as I
    a = {"answers": [{"id": "1", "pass": 1, "prediction": "x"}, {"id": "2", "pass": 1, "prediction": "y"}]}
    b = {"answers": [{"id": "1", "pass": 1, "prediction": "x"}, {"id": "2", "pass": 1, "prediction": "z"}]}
    assert I.compare(a, a)["differ"] == 0 and I.compare(a, b)["differ"] == 1
    assert I.compare(a, {"answers": a["answers"][:1]})["only_in_one"] == 1
