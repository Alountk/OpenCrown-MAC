#!/usr/bin/env python3
"""Summarises the frame statistics of one or more dcp_run logs (run with DCP_STATS=1) so that runs can be compared:
windows below 55 and 45 fps, the slowest frame times, the host render cost and the BC7 decodes done on the CPU.

  python3 tests/perf_report.py [--from-vblank N] run1.log [run2.log ...]      --selftest checks the parser and the numbers

Each `stats` line is a window of about 30 vblanks: "stats v<vblank> <fps> fps <ms> ms/frame | replay <ms> ms <n> draws". A single stall
of 140 ms shows up as one window near 47 fps, so the counts of low windows are the figure to watch, more than the mean."""
import argparse, re, sys

STATS = re.compile(r"stats v(\d+)\s+([\d.]+) fps\s+([\d.]+) ms/frame \| replay ([\d.]+) ms\s+(\d+) draws")
BC7 = re.compile(r"BC7 decode on the CPU (\d+)x(\d+) took ([\d.]+) ms")


def parse(lines):
    """-> (windows [(vblank, fps, frame_ms, replay_ms, draws)], bc7 decodes [ms])"""
    wins, bc7 = [], []
    for line in lines:
        m = STATS.search(line)
        if m:
            wins.append((int(m[1]), float(m[2]), float(m[3]), float(m[4]), int(m[5])))
            continue
        m = BC7.search(line)
        if m:
            bc7.append(float(m[3]))
    return wins, bc7


def percentile(values, p):
    if not values:
        return 0.0
    v = sorted(values)
    return v[min(len(v) - 1, int(p / 100 * len(v)))]


def summarise(wins, bc7, from_vblank=0):
    w = [x for x in wins if x[0] >= from_vblank]
    n = len(w)
    if n == 0:
        return None
    frame = [x[2] for x in w]
    replay = [x[3] for x in w]
    return {
        "windows": n,
        "below55": sum(x[1] < 55 for x in w), "below45": sum(x[1] < 45 for x in w), "below30": sum(x[1] < 30 for x in w),
        "min_fps": min(x[1] for x in w),
        "frame_p50": percentile(frame, 50), "frame_p95": percentile(frame, 95), "frame_p99": percentile(frame, 99), "frame_max": max(frame),
        "replay_mean": sum(replay) / n, "replay_max": max(replay),
        "bc7_count": len(bc7), "bc7_total_ms": sum(bc7), "bc7_max_ms": max(bc7, default=0.0),
    }


def report(name, s):
    if s is None:
        return f"{name}: no stats lines (was it run with DCP_STATS=1?)"
    pct = lambda k: f"{s[k]:4d} ({100 * s[k] / s['windows']:4.1f}%)"
    return (f"{name}\n  windows {s['windows']}   below 55 fps {pct('below55')}   below 45 {pct('below45')}   below 30 {pct('below30')}   min {s['min_fps']:.1f} fps\n"
            f"  frame ms  p50 {s['frame_p50']:.1f}  p95 {s['frame_p95']:.1f}  p99 {s['frame_p99']:.1f}  max {s['frame_max']:.1f}\n"
            f"  render cost per frame  mean {s['replay_mean']:.2f} ms  max {s['replay_max']:.1f} ms\n"
            f"  BC7 decodes on the CPU  {s['bc7_count']} (total {s['bc7_total_ms']:.0f} ms, longest {s['bc7_max_ms']:.0f} ms)")


def selftest():
    text = ["stats v100 60.0 fps  16.70 ms/frame | replay 1.00 ms  100 draws",
            "render: BC7 decode on the CPU 2048x2048 took 36.5 ms (frame hitch)",
            "stats v130 47.0 fps  21.30 ms/frame | replay 9.00 ms  120 draws",
            "stats v160 25.0 fps  40.00 ms/frame | replay 30.00 ms  120 draws",
            "stats v190 60.0 fps  16.60 ms/frame | replay 1.00 ms  100 draws", "noise"]
    wins, bc7 = parse(text)
    assert len(wins) == 4 and bc7 == [36.5]
    s = summarise(wins, bc7)
    assert (s["windows"], s["below55"], s["below45"], s["below30"]) == (4, 2, 1, 1), s
    assert s["min_fps"] == 25.0 and s["frame_max"] == 40.0 and s["replay_max"] == 30.0
    assert abs(s["replay_mean"] - 10.25) < 1e-9 and s["bc7_count"] == 1 and s["bc7_max_ms"] == 36.5
    assert summarise(wins, bc7, from_vblank=150)["windows"] == 2 and summarise([], []) is None
    assert percentile([1, 2, 3, 4], 50) == 3 and percentile([], 95) == 0.0
    print("perf_report: self-test passed")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("logs", nargs="*")
    p.add_argument("--from-vblank", type=int, default=0, help="ignore the windows before this vblank (the start up)")
    p.add_argument("--selftest", action="store_true")
    o = p.parse_args()
    if o.selftest:
        return selftest()
    if not o.logs:
        p.error("give at least one log")
    for path in o.logs:
        with open(path, errors="replace") as f:
            wins, bc7 = parse(f)
        print(report(path, summarise(wins, bc7, o.from_vblank)))


if __name__ == "__main__":
    sys.exit(main())
