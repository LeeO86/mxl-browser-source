#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reads an audio trace saved by cadence_bench.py (<label>-audiotrace.json: the function's
/api/v1/debug/audio-trace polled during the run, the media position and the CPU load) and says
how Chromium delivered the audio (docs/performance.md, M7):

- delivery: the delay of each packet after its capture time (arrival - pts, both wall time), its
  quantiles, and the stalls: a pause between packets longer than --stall ms, with how much audio
  came in the burst after it and whether the burst kept its pts (late) or jumped (lost);
- pts discontinuities: jumps and overlaps in the packet times, the gap fill the function put in;
- the FIFO per grain: underruns (with the stall that caused each), the fill and the target over
  time, frames dropped as late;
- the media position of each event (video.currentTime) and the load on the container's CPUs
  from other processes at that second.

    audio_trace.py [--stall 40] [--detail] [--all] FILE...

Standard library only.
"""
import argparse
import bisect
import json

TAI_UTC = 37  # TAI - UTC in seconds (the bench's wall clock is UTC)


def quantiles(values, qs=(0.5, 0.9, 0.99, 1.0)):
    if not values:
        return {}
    v = sorted(values)
    return {q: v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))] for q in qs}


def analyse(doc, stall_ms=40.0, whole=False):
    events = doc["events"]
    if not whole and doc.get("t0"):
        # The trace reaches about 100 s back: keep the measured run (the bench's t0 is UTC).
        events = [e for e in events if e[0] / 1e9 - TAI_UTC >= doc["t0"]]
    packets = [e for e in events if e[1] == "packet"]
    ticks = [e for e in events if e[1] == "tick"]
    pos = doc.get("positions") or []
    cpu = [r for r in doc.get("cpu") or [] if isinstance(r, list)]

    def position(tai_ns):
        """Media position (s) at TAI `tai_ns`, from the 2 s samples."""
        if not pos:
            return None
        wall = tai_ns / 1e9 - TAI_UTC
        i = bisect.bisect_left([p[0] for p in pos], wall)
        p = pos[min(max(i, 0), len(pos) - 1)]
        return round(p[1] + (wall - p[0]), 1)

    def other_load(tai_ns):
        if not cpu:
            return None
        wall = tai_ns / 1e9 - TAI_UTC
        i = min(bisect.bisect_left([r[0] for r in cpu], wall), len(cpu) - 1)
        return round(max(0.0, cpu[i][1] - cpu[i][2]), 2)

    out = {"packets": len(packets), "ticks": len(ticks)}
    if not packets:
        return out
    # Delivery delay: arrival minus capture. CEF's pts is wall time (UTC) in whole ms, so the arrival
    # is TAI - 37 s. The constant part is Chromium's own buffering; above the minimum is lateness.
    delay = [p[0] / 1e6 - TAI_UTC * 1000 - p[2] for p in packets]
    base = min(delay)
    late = [d - base for d in delay]
    out["delay_ms"] = {"min": round(base, 1), **{f"p{int(q * 100)}": round(v + base, 1) for q, v in quantiles(late).items()}}

    # Stalls: pauses between packet arrivals, and the burst after them.
    stalls = []
    for i in range(1, len(packets)):
        gap = (packets[i][0] - packets[i - 1][0]) / 1e6
        if gap < stall_ms:
            continue
        # The burst: the packets that arrive within 5 ms of each other after the pause.
        j = i
        while j + 1 < len(packets) and (packets[j + 1][0] - packets[j][0]) / 1e6 < 5:
            j += 1
        burst = packets[i:j + 1]
        expected = packets[i - 1][2] + packets[i - 1][3] / 48
        pts_jump = packets[i][2] - expected  # > 0: packets were lost (pts moved on); ~0: they came late
        stalls.append({
            "tai": packets[i][0], "position": position(packets[i][0]), "pause_ms": round(gap, 1),
            "burst_packets": len(burst), "burst_ms": round(sum(p[3] for p in burst) / 48, 1),
            "pts_jump_ms": round(pts_jump, 1), "late_ms": round(late[i], 1), "fill_before": packets[i - 1][4],
            "gap_fill_ms": round(sum(p[5] for p in burst) / 48, 1), "other_load_cores": other_load(packets[i][0]),
        })
    out["stalls"] = stalls

    # pts discontinuities against the frame count.
    jumps = []
    for i in range(1, len(packets)):
        d = packets[i][2] - (packets[i - 1][2] + packets[i - 1][3] / 48)
        if abs(d) > 2.5:
            jumps.append({"tai": packets[i][0], "position": position(packets[i][0]), "pts_delta_ms": round(d, 1),
                          "gap_fill_ms": round(packets[i][5] / 48, 1), "arrival_gap_ms": round((packets[i][0] - packets[i - 1][0]) / 1e6, 1)})
    out["pts_jumps"] = jumps
    out["gap_fill_ms_total"] = round(sum(p[5] for p in packets) / 48, 1)

    # Grains: underruns (the underrun total grew), fill, target, late drops.
    under = []
    for i in range(1, len(ticks)):
        d = ticks[i][4] - ticks[i - 1][4]
        if d > 0:
            if under and ticks[i][0] - under[-1]["tai_end"] <= 25_000_000:
                under[-1]["frames"] += d
                under[-1]["tai_end"] = ticks[i][0]
            else:
                under.append({"tai": ticks[i][0], "tai_end": ticks[i][0], "frames": d, "position": position(ticks[i][0]),
                              "fill_before": ticks[i][2], "target": ticks[i][3], "other_load_cores": other_load(ticks[i][0])})
    for u in under:
        u["ms"] = round(u.pop("frames") / 48, 1)
        # The stall that caused it: the last one that started up to 1 s before.
        cause = [s for s in stalls if u["tai"] - 1_000_000_000 <= s["tai"] <= u["tai_end"] + 500_000_000]
        u["stall"] = max(cause, key=lambda s: s["pause_ms"]) if cause else None
    out["underruns"] = under
    out["underrun_ms_total"] = round(sum(u["ms"] for u in under), 1)
    out["realigned_ms"] = round((ticks[-1][5] - ticks[0][5]) / 48, 1) if ticks else 0
    fills = [t[2] / 48 for t in ticks]
    out["fill_ms"] = {f"p{int(q * 100)}": round(v, 1) for q, v in quantiles(fills, (0.01, 0.5, 0.99)).items()}
    targets = [t[3] for t in ticks]
    out["target_ms"] = {"min": round(min(targets) / 48, 1), "max": round(max(targets) / 48, 1)} if targets else {}
    # Target over time: (position, target ms) at every change.
    changes, last = [], None
    for t in ticks:
        if last is None or abs(t[3] - last) >= 480:  # every 10 ms of change
            changes.append([position(t[0]), round(t[3] / 48, 1)])
            last = t[3]
    out["target_changes"] = changes[:60]
    if ticks:
        span = (ticks[-1][0] - ticks[0][0]) / 1e9
        out["target_mean_ms"] = round(sum((ticks[i + 1][0] - ticks[i][0]) * ticks[i][3] for i in range(len(ticks) - 1)) / 1e9 / max(span, 1e-9) / 48, 1)
    if cpu:
        loads = sorted(max(0.0, r[1] - r[2]) for r in cpu)
        out["other_load_cores"] = {"median": loads[len(loads) // 2], "p95": loads[int(0.95 * (len(loads) - 1))], "max": loads[-1]}
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("files", nargs="+")
    p.add_argument("--stall", type=float, default=40.0, help="a pause between packets longer than this (ms) is a stall")
    p.add_argument("--detail", action="store_true", help="every stall and pts jump, not only the summary")
    p.add_argument("--all", action="store_true", help="the whole trace, also before the run started (page load, warm-up)")
    args = p.parse_args()
    for path in args.files:
        with open(path) as f:
            r = analyse(json.load(f), args.stall, args.all)
        print(f"== {path}")
        print(f"packets {r['packets']}, grains {r['ticks']}, delay {r.get('delay_ms')}, fill {r.get('fill_ms')}, target {r.get('target_ms')} "
              f"(mean {r.get('target_mean_ms')}), gap fill {r.get('gap_fill_ms_total')} ms, realigned {r.get('realigned_ms')} ms, "
              f"other load on the cpuset {r.get('other_load_cores')}")
        stalls = r.get("stalls", [])
        print(f"stalls >= {args.stall} ms: {len(stalls)}; pts jumps: {len(r.get('pts_jumps', []))}; "
              f"underruns: {len(r.get('underruns', []))}, {r.get('underrun_ms_total')} ms")
        for u in r.get("underruns", []):
            s = u["stall"] or {}
            print(f"  underrun at {u['position']} s: {u['ms']} ms, fill before {u['fill_before'] / 48:.1f} ms, target {u['target'] / 48:.0f} ms, "
                  f"load {u['other_load_cores']}; stall {s.get('pause_ms')} ms then {s.get('burst_packets')} packets "
                  f"({s.get('burst_ms')} ms) pts jump {s.get('pts_jump_ms')} ms, late {s.get('late_ms')} ms")
        if args.detail:
            for s in stalls:
                print(f"  stall {s}")
            for j in r.get("pts_jumps", []):
                print(f"  pts {j}")
            print(f"  target changes {r.get('target_changes')}")


if __name__ == "__main__":
    main()
