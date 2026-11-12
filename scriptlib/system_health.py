#!/usr/bin/env python3
"""One screen of yoda's health, read-only: load, memory, pressure, disks,
GPU memory, temperatures, the llama-server processes and failed user units.

Everything is read from /proc and /sys (plus one systemctl call), so it costs
nothing and needs no model. DATA_DIR (set by the manifest) is the disk Funes
keeps memory.db and the newsletter on.
"""
import glob
import os
import subprocess


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def gib(n):
    return f"{n / 2**30:.1f} GiB"


def section(title, lines):
    print(f"## {title}")
    for line in lines or ["(unavailable)"]:
        print(f"  {line}")
    print()


def load():
    la = read("/proc/loadavg")
    up = read("/proc/uptime")
    out = []
    if la:
        out.append("load (1/5/15 min): " + " ".join(la.split()[:3]) + f"  on {os.cpu_count()} CPUs")
    if up:
        s = float(up.split()[0])
        out.append(f"up {int(s // 86400)} d {int(s % 86400 // 3600)} h")
    return out


def memory():
    info = {}
    for line in (read("/proc/meminfo") or "").splitlines():
        key, _, rest = line.partition(":")
        if rest.strip().endswith("kB"):
            info[key] = int(rest.split()[0]) * 1024
    if "MemTotal" not in info:
        return []
    out = [f"RAM: {gib(info['MemTotal'] - info.get('MemAvailable', 0))} used of {gib(info['MemTotal'])}"
           f" ({gib(info.get('MemAvailable', 0))} available)"]
    if info.get("SwapTotal"):
        out.append(f"swap: {gib(info['SwapTotal'] - info.get('SwapFree', 0))} used of {gib(info['SwapTotal'])}")
    return out


def pressure():
    out = []
    for kind in ("cpu", "memory", "io"):
        text = read(f"/proc/pressure/{kind}")
        if not text:
            continue
        some = text.splitlines()[0].split()
        vals = dict(p.split("=") for p in some[1:] if "=" in p)
        out.append(f"{kind}: some avg10={vals.get('avg10')}% avg60={vals.get('avg60')}% avg300={vals.get('avg300')}%")
    return out


def disks():
    out = []
    seen = set()
    for label, path in (("/", "/"), ("home", os.path.expanduser("~")), ("data", os.environ.get("DATA_DIR", ""))):
        if not path:
            continue
        try:
            st = os.statvfs(path)
        except OSError:
            continue
        key = (st.f_fsid, st.f_blocks)
        if key in seen:
            continue
        seen.add(key)
        total = st.f_blocks * st.f_frsize
        free = st.f_bavail * st.f_frsize
        pct = 100 * (total - free) / total if total else 0
        out.append(f"{label} ({path}): {gib(total - free)} used of {gib(total)}, {gib(free)} free ({pct:.0f}% full)")
    return out


def gpu():
    out = []
    for card in sorted(glob.glob("/sys/class/drm/card[0-9]/device")):
        used, total = read(card + "/mem_info_vram_used"), read(card + "/mem_info_vram_total")
        busy = read(card + "/gpu_busy_percent")
        if used and total:
            line = f"{os.path.basename(os.path.dirname(card))}: VRAM {gib(int(used))} used of {gib(int(total))}"
            if busy is not None:
                line += f", {busy}% busy"
            out.append(line)
    return out


def temperatures():
    out = []
    for hw in sorted(glob.glob("/sys/class/hwmon/hwmon*")):
        name = read(hw + "/name") or os.path.basename(hw)
        temps = [int(t) / 1000 for t in (read(p) for p in glob.glob(hw + "/temp*_input")) if t and t.lstrip("-").isdigit()]
        if temps:
            out.append(f"{name}: {max(temps):.0f} °C")
    return out


def llama():
    out = []
    for comm_path in glob.glob("/proc/[0-9]*/comm"):
        if read(comm_path) != "llama-server":
            continue
        pid = comm_path.split("/")[2]
        cmd = (read(f"/proc/{pid}/cmdline") or "").split("\0")
        port = cmd[cmd.index("--port") + 1] if "--port" in cmd and cmd.index("--port") + 1 < len(cmd) else "?"
        model = next((os.path.basename(cmd[i + 1]) for i, a in enumerate(cmd[:-1]) if a in ("-m", "--model")), "?")
        rss = next((int(l.split()[1]) * 1024 for l in (read(f"/proc/{pid}/status") or "").splitlines()
                    if l.startswith("VmRSS:")), 0)
        out.append(f"pid {pid} port {port}: {model} (RSS {gib(rss)})")
    return out or ["no llama-server running"]


def failed_units():
    try:
        r = subprocess.run(["systemctl", "--user", "--failed", "--plain", "--no-legend", "--no-pager"],
                           capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as e:
        return [f"systemctl: {e}"]
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    return lines or ["none"]


def main():
    section("Load", load())
    section("Memory", memory())
    section("Pressure (PSI)", pressure())
    section("Disks", disks())
    section("GPU", gpu())
    section("Temperatures", temperatures())
    section("llama-server", llama())
    section("Failed user units", failed_units())


if __name__ == "__main__":
    main()
