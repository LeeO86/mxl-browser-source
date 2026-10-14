#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Where the time of a frame goes (docs/performance.md, M5): reads GET /api/v1/debug/frame-trace
(a URL or a saved JSON file) and splits every frame into its stages:

    BeginFrame posted (tick) -> sent on the UI thread -> OnPaint -> copied -> conversion starts ->
    converted -> committed at a tick

Each paint is paired with the newest BeginFrame posted before its OnPaint, and each tick's commit
with the frame it wrote. For every repeated grain (a held frame) it says why: the paint of the
grain's BeginFrame arrived but was converted after the tick (and by how much, stage by stage), or
no paint arrived at all before the next BeginFrame.

With the internal frame clock (BROWSER_FRAME_CLOCK=internal) Chromium's BeginFrames are not in the
trace; they are at T(n) + phase, and each paint carries the newest of them before it. "render" is
then measured from that BeginFrame time, the "anchor" events show when the clock was locked, and
"paint phase" is where OnPaint falls in the period (ms after T(n)).

    frame_trace.py http://127.0.0.1:8460 [--json out.json]
    frame_trace.py trace.json
"""
import argparse
import bisect
import json
import statistics
import sys
import urllib.request


def load(source):
    if source.startswith("http"):
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(source.rstrip("/") + "/api/v1/debug/frame-trace", timeout=30) as r:
            return json.loads(r.read())
    with open(source) as f:
        return json.load(f)


def pct(values, q):
    if not values:
        return None
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))]


def summary(name, values):
    if not values:
        return f"{name:<28} -"
    ms = [v / 1e6 for v in values]
    return (f"{name:<28} n={len(ms):<6} mean {statistics.mean(ms):6.2f}  p50 {pct(ms, .5):6.2f}  p95 {pct(ms, .95):6.2f}"
            f"  p99 {pct(ms, .99):6.2f}  max {max(ms):6.2f} ms")


def analyse(trace):
    period = trace["period_ns"]
    internal = trace.get("frame_clock") == "internal"
    phase = trace.get("phase_ns", 0)
    ticks, posted, sent, commits, anchors = [], [], [], [], []
    frames = {}  # paint sequence -> {"in", "stored", "start", "bf", "end"}
    for ns, kind, a, b in trace["events"]:
        if kind == "tick":
            ticks.append(ns)
        elif kind == "bf_posted":
            posted.append(ns)
        elif kind == "bf_sent":
            sent.append(ns)
        elif kind == "paint":
            frames.setdefault(a, {}).update({"in": b, "stored": ns})
        elif kind == "convert_start":
            frames.setdefault(a, {}).update({"start": ns, "bf": b})
        elif kind == "convert_end":
            frames.setdefault(a, {})["end"] = ns
        elif kind == "commit":
            commits.append((ns, a, b))
        elif kind == "anchor":
            anchors.append((ns, a, b))
    ticks.sort(); posted.sort(); sent.sort()
    if internal and ticks:
        # Chromium's BeginFrames: the grid T(n) + phase over the traced ticks.
        first = (ticks[0] - phase) // period * period + phase
        posted = list(range(first, ticks[-1], period))
        sent = posted
    t0 = max(ticks[0] if ticks else 0, posted[0] if posted else 0)

    def stage_values(f):
        out = {}
        bf = f.get("bf")
        if not bf or "in" not in f or "end" not in f:
            return None
        if internal:
            out["render"] = f["in"] - bf
        else:
            i = bisect.bisect_left(sent, bf)
            if i < len(sent) and sent[i] <= f["in"]:
                out["dispatch"] = sent[i] - bf
                out["render"] = f["in"] - sent[i]
        out["copy"] = f["stored"] - f["in"]
        out["wait"] = f["start"] - f["stored"]
        out["convert"] = f["end"] - f["start"]
        out["ready"] = f["end"] - bf
        # The deadline: the first tick after an external BeginFrame (posted by the tick); for
        # Chromium's own BeginFrame the first tick at least a period less 2 ms after it.
        j = bisect.bisect_left(ticks, bf + period - 2_000_000) if internal else bisect.bisect_right(ticks, bf)
        if j < len(ticks):
            out["slack"] = ticks[j] - f["end"]
        return out

    names = ("dispatch", "render", "copy", "wait", "convert", "ready", "slack")
    stages = {k: [] for k in names}
    late_stages = {k: [] for k in names}
    by_bf = {}
    paints_per_bf = {}
    for seq, f in sorted(frames.items()):
        if f.get("bf"):
            paints_per_bf[f["bf"]] = paints_per_bf.get(f["bf"], 0) + 1
        v = stage_values(f)
        if v is None or f["bf"] < t0:
            continue
        by_bf[f["bf"]] = (f, v)
        for k, x in v.items():
            stages[k].append(x)
        if v.get("slack", 0) < 0:
            for k, x in v.items():
                late_stages[k].append(x)
    held = late = nopaint = 0
    for ns, index, bf in commits:
        if bf != 0 or ns < t0 + 2 * period:
            continue
        held += 1
        # The BeginFrame this grain waited for: posted at the previous tick.
        i = bisect.bisect_left(posted, ns - period // 2) - 1
        want = posted[i] if i >= 0 else None
        if want in by_bf:
            late += 1
        else:
            nopaint += 1
    unpainted = sum(1 for p in posted if p >= t0 and p not in paints_per_bf)
    paint_phase = [(f["in"] - phase) % period for f in frames.values() if "in" in f and f["in"] >= t0]
    return {
        "internal": internal, "phase_ms": phase / 1e6, "anchors": anchors, "paint_phase": paint_phase,
        "period_ms": period / 1e6, "commits": len(commits), "fresh": sum(1 for c in commits if c[2] != 0), "held": held,
        "held_late_paint": late, "held_no_paint": nopaint, "paints": sum(1 for f in frames.values() if "in" in f),
        "converted": sum(1 for f in frames.values() if "end" in f), "begin_frames": len(posted),
        "begin_frames_without_paint": unpainted, "stages": stages, "late_stages": late_stages,
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("source")
    p.add_argument("--json")
    args = p.parse_args()
    trace = load(args.source)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(trace, f)
    r = analyse(trace)
    print(f"period {r['period_ms']} ms: {r['commits']} grains, {r['fresh']} fresh, {r['held']} held "
          f"({r['held_late_paint']} with the paint converted after the tick, {r['held_no_paint']} without a paint); "
          f"{r['paints']} paints, {r['converted']} converted")
    print(f"{r['begin_frames']} BeginFrames, {r['begin_frames_without_paint']} answered by no converted paint")
    if r["internal"]:
        print(f"internal frame clock, phase {r['phase_ms']} ms; anchors (TAI ns, target, call - target):")
        for ns, target, called in r["anchors"]:
            print(f"  {ns} {target} {'late, not anchored' if called == 0 else f'{(called - target) / 1e3:+.1f} us'}")
        print("  " + summary("paint phase (after T+phase)", r["paint_phase"]))
    for title, key in (("all converted frames (ms from the BeginFrame posted at the tick; slack = next tick - converted):", "stages"),
                       ("frames converted after their tick:", "late_stages")):
        print(title)
        for k in ("dispatch", "render", "copy", "wait", "convert", "ready", "slack"):
            print("  " + summary(k, r[key][k]))


if __name__ == "__main__":
    sys.exit(main())
