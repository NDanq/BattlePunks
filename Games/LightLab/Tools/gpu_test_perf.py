#!/usr/bin/env python3
"""
gpu_test_perf.py -- runs the GpuTest level's benchmark (LightLab, [LightLab]
Benchmark) under a set of configurations and summarises each run.

    run      launch LightLab once per configuration, each writing <out>/<name>.csv
    summary  read every CSV in <out> into <out>/summary.json: per run, per leg
             of the path, the mean and 95th percentile of each column

Configurations (--suite):

    occlusion   [Rendering] OcclusionCulling on and off, alternated, --repeat times
    animation   the herd at 0, 64, 256 and 1024 foxes; 256 and 1024 with the
                [Animation] budget off; 1024 with the update not parallel
    all         both

Each run flies the same path a frame at a time (LightLabGpuTest.h), unlimited
frame rate, "stat unit" and "stat frame" on. Run it from a build whose
binaries are optimised (RelWithDebInfo):

    python Games/LightLab/Tools/gpu_test_perf.py run --suite all --out perf
    python Games/LightLab/Tools/gpu_test_perf.py summary --out perf

Shots (an editor build's binaries, --shots): each leg's middle frame with
occlusion culling on and off, animation frozen so they compare:

    python Games/LightLab/Tools/gpu_test_perf.py shots --out perf \\
        --binaries Games/LightLab/Binaries/Linux_RelWithDebInfo
"""

import argparse
import csv
import json
import statistics
import bisect
import subprocess
import threading
import time
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parent.parent

# The GPU's actual clock (Intel xe, Linux), sampled while each run plays: an
# integrated GPU's clock follows its power budget, so a run's GPU times are
# read against the clock it ran at. Absent elsewhere: nothing is sampled.
GPU_FREQ = Path("/sys/class/drm/card0/device/tile0/gt0/freq0/act_freq")
DEFAULT_BINARIES = GAME_DIR / "Binaries" / "Linux_RelWithDebInfo_Client"

BASE = ["--Game.StartLevel=GpuTest", "--Display.FrameRateLimit=0", "--Stats.Frame=1", "--Stats.Unit=1"]
BUDGET_OFF = ["--Animation.FullRateDistance=0", "--Animation.OffscreenSkipFrames=0", "--Animation.FreezeDistance=0"]


def suite(name, repeat):
    runs = []
    if name in ("occlusion", "all"):
        for index in range(repeat):
            runs.append((f"occlusion_on_{index}", ["--Rendering.OcclusionCulling=1"]))
            runs.append((f"occlusion_off_{index}", ["--Rendering.OcclusionCulling=0"]))
    if name in ("animation", "all"):
        for count in (0, 64, 256, 1024):
            runs.append((f"herd_{count}", [f"--LightLab.HerdCount={count}"]))
        for count in (256, 1024):
            runs.append((f"herd_{count}_nobudget", [f"--LightLab.HerdCount={count}"] + BUDGET_OFF))
        runs.append(("herd_1024_serial", ["--LightLab.HerdCount=1024", "--Animation.ParallelMin=0"]))
    return runs


def launch(binaries, out, name, extra, timeout=900):
    csv_path = out / f"{name}.csv"
    log_path = out / f"{name}.log"
    # Config strings hold 127 characters (CoreConfig.h's CONFIG_STRING_MAX): a longer path is cut short.
    if len(str(csv_path)) > 127 or len(str(out / f"{name}")) > 127:
        raise SystemExit(f"{csv_path}: over 127 characters, which [LightLab] Benchmark cannot hold; use a shorter --out")
    command = [str(binaries / "LightLab")] + BASE + [f"--LightLab.Benchmark={csv_path}"] + extra
    print(f"{name}: {' '.join(extra)}", flush=True)
    samples, stop = [], threading.Event()

    def sample():
        while not stop.is_set():
            try:
                samples.append((time.time(), int(GPU_FREQ.read_text())))
            except (OSError, ValueError):
                return
            time.sleep(0.05)

    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()
    with open(log_path, "w") as log:
        result = subprocess.run(command, cwd=binaries, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    stop.set()
    sampler.join()
    if samples:
        (out / f"{name}.freq").write_text(json.dumps(samples))
    if result.returncode != 0 or not csv_path.exists():
        print(f"  failed ({result.returncode}); see {log_path}")


def run(args):
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name, extra in suite(args.suite, args.repeat):
        launch(Path(args.binaries).resolve(), out, name, extra)


def shots(args):
    out = Path(args.out).resolve()
    for name, flag in (("on", "1"), ("off", "0")):
        folder = out / f"shots_{name}"
        folder.mkdir(parents=True, exist_ok=True)
        launch(Path(args.binaries).resolve(), out, f"shots_{name}",
               [f"--Rendering.OcclusionCulling={flag}", f"--LightLab.BenchmarkShots={folder}", "--LightLab.BenchmarkFreeze=1"])


def gpu_clock(rows, samples):
    """Each row's GPU clock: the sample nearest its wall time (rows without one get none)."""
    if not samples or "wall_s" not in rows[0]:
        return
    times = [t for t, _ in samples]
    for row in rows:
        index = min(bisect.bisect_left(times, float(row["wall_s"])), len(samples) - 1)
        row["gpu_mhz"] = str(samples[index][1])


def summarise_rows(rows):
    columns = [c for c in rows[0].keys() if c not in ("frame", "leg", "wall_s")]
    out = {"frames": len(rows)}
    for column in columns:
        values = sorted(float(r[column]) for r in rows)
        out[column] = {"mean": statistics.fmean(values), "p95": values[min(len(values) - 1, int(0.95 * len(values)))]}
    return out


def summary(args):
    out = Path(args.out).resolve()
    result = {}
    for path in sorted(out.glob("*.csv")):
        rows = [r for r in csv.DictReader(open(path)) if r["leg"] != "Warmup"]
        if not rows:
            continue
        freq = path.with_suffix(".freq")
        gpu_clock(rows, json.loads(freq.read_text()) if freq.exists() else None)
        legs = {}
        for row in rows:
            legs.setdefault(row["leg"], []).append(row)
        result[path.stem] = {"all": summarise_rows(rows), **{leg: summarise_rows(v) for leg, v in legs.items()}}
    (out / "summary.json").write_text(json.dumps(result, indent=1))
    print(f"{out / 'summary.json'}: {len(result)} runs")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("run", "summary", "shots"))
    parser.add_argument("--out", required=True)
    parser.add_argument("--suite", default="all", choices=("occlusion", "animation", "all"))
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--binaries", default=str(DEFAULT_BINARIES))
    args = parser.parse_args()
    {"run": run, "summary": summary, "shots": shots}[args.command](args)


if __name__ == "__main__":
    main()
