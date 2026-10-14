#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Video cadence and audio continuity bench (docs/performance.md, M6): plays a URL (a test clip
through templates/video.html, tone.html, a YouTube embed) and measures for --seconds after
--warmup:

- the content cadence on the MXL output: `grain_reader counter --out` reads the clip's bar code
  (its video frame number) from every grain; joined with the frame trace (polled every 20 s, so
  the whole run is covered) it says, per grain, whether the function committed a fresh frame or
  repeated one, and whether the content advanced as an ideal sampler of a clip at --content-fps
  would on the output rate: content repeats and skips on fresh grains are Chromium's choice of
  video frame, repeats on held grains are a missing paint;
- the function's counters (repeated grains, late paints, missed grains, BeginFrames, frame clock
  phase error, audio underruns, gaps), the frame trace summary (tests/tools/frame_trace.py);
- the audio on the MXL output: `grain_reader audio` (gaps below -40 dBFS in 1 ms windows,
  phase jumps of the 1 kHz tone);
- Chromium's view through DevTools (BROWSER_DEVTOOLS=true): getVideoPlaybackQuality(),
  requestVideoFrameCallback steps (video.html), YouTube's stats for nerds, and optionally a trace
  of the categories audio, media and gpu.capture (--chromium-trace S) with the event counts, among
  them the loopback's "GenerateMoreAudio Is Behind";
- CPU per process type (browser, gpu, renderer, audio and network utilities) from /proc in the
  container, the container's CPU, load average and GPU (as frameloop_bench.py);
- optionally nvidia-smi polled every second during the run (--smi).

One JSON line per run goes to --out, raw files (counter, audio, traces) to --dir.

    cadence_bench.py [--base http://127.0.0.1:8460] [--container ograf-mbs] [--domain /Volumes/mxl/ograf-mbs]
                     [--seconds 120] [--warmup 15] [--content-fps 50] [--label text] [--out runs.jsonl] [--dir raw]
                     [--smi] [--chromium-trace 30] [--no-navigate] [--reader path] URL

Standard library only.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.request
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
from frameloop_bench import histograms, host_state, metrics, request  # noqa: E402
from interact_test import ws_connect, ws_messages, ws_send  # noqa: E402
import frame_trace  # noqa: E402

DOCKER = os.environ.get("DOCKER", "docker").split()


# ---------------------------------------------------------------------------------------------
# DevTools (page target, through the function's /devtools/ proxy)

class DevTools:
    def __init__(self, base):
        m = re.match(r"https?://([^:/]+):(\d+)", base)
        self.host, self.port = m.group(1), int(m.group(2))
        self.base = base
        self.next_id = 0

    def page(self):
        code, pages = request(self.base, "GET", "/devtools/json/list")
        if code != 200:
            return None
        return next((p for p in pages if p.get("type") == "page"), None)

    def connect(self):
        page = self.page()
        return ws_connect(self.host, self.port, "/devtools/page/" + page["id"]) if page else None

    def call(self, sock, method, params=None, seconds=10):
        self.next_id += 1
        ws_send(sock, {"id": self.next_id, "method": method, "params": params or {}})
        end = time.time() + seconds
        while time.time() < end:
            for m in ws_messages(sock, 0.5):
                if m.get("id") == self.next_id:
                    return m
        return None

    def evaluate(self, expression):
        sock = self.connect()
        if sock is None:
            return None
        try:
            answer = self.call(sock, "Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True})
            return ((answer or {}).get("result") or {}).get("result", {}).get("value")
        finally:
            sock.close()

    def trace(self, seconds, categories="audio,media,gpu.capture"):
        """Event counts by name of a trace of `seconds` (Tracing domain of the page target)."""
        sock = self.connect()
        if sock is None:
            return {"error": "no page target"}
        counts, events = {}, 0
        try:
            started = self.call(sock, "Tracing.start", {"categories": categories, "transferMode": "ReportEvents"})
            if not started or "error" in started:
                return {"error": (started or {}).get("error", "no answer")}
            end = time.time() + seconds
            done = False

            def take(messages):
                nonlocal events, done
                for m in messages:
                    if m.get("method") == "Tracing.dataCollected":
                        for e in m["params"]["value"]:
                            events += 1
                            name = e.get("cat", "") + ":" + e.get("name", "")
                            counts[name] = counts.get(name, 0) + 1
                    elif m.get("method") == "Tracing.tracingComplete":
                        done = True

            while time.time() < end:
                take(ws_messages(sock, 1))
            self.next_id += 1
            ws_send(sock, {"id": self.next_id, "method": "Tracing.end"})
            limit = time.time() + 30
            while not done and time.time() < limit:
                take(ws_messages(sock, 1))
        finally:
            sock.close()
        top = dict(sorted(counts.items(), key=lambda kv: -kv[1])[:60])
        behind = sum(v for k, v in counts.items() if "Behind" in k)
        return {"seconds": seconds, "categories": categories, "events": events, "generate_more_audio_behind": behind, "counts": top}


PAGE_STATS = r"""(() => {
  const out = {url: location.href};
  if (window.mbsVideoStats) Object.assign(out, window.mbsVideoStats());
  const v = document.querySelector('video');
  if (v) {
    const q = v.getVideoPlaybackQuality();
    Object.assign(out, {total: q.totalVideoFrames, dropped: q.droppedVideoFrames, currentTime: v.currentTime, paused: v.paused,
      width: v.videoWidth, height: v.videoHeight, muted: v.muted, volume: v.volume});
  }
  const p = document.querySelector('#movie_player');
  if (p && p.getStatsForNerds) { try { out.nerds = p.getStatsForNerds(); } catch (e) { out.nerds = String(e); } }
  if (p && p.getPlaybackQuality) { try { out.quality = p.getPlaybackQuality(); } catch (e) {} }
  if (p && p.getVideoData) { try { out.videoData = p.getVideoData(); } catch (e) {} }
  if (p && p.getPlayerState) { try { out.playerState = p.getPlayerState(); } catch (e) {} }
  return out;
})()"""


# requestAnimationFrame timestamps are the BeginFrame's frame time. Mapped to wall time through
# performance.timeOrigin (UTC; TAI = UTC + 37 s), their position in the house period shows the
# frame clock's phase (and, with external BeginFrames, the jitter of the call time). The mapping's
# own offset is fixed per document and below a millisecond; the timestamps are coarsened to 100 us.
RAF_PHASE = r"""new Promise(done => { const t = []; const f = ts => { t.push(performance.timeOrigin + ts); if (t.length < 200) requestAnimationFrame(f); else done(t); }; requestAnimationFrame(f); })"""


def raf_phase(devtools, period_ns):
    stamps = devtools.evaluate(RAF_PHASE)
    if not stamps:
        return None
    import cmath
    import math
    p = period_ns / 1e6
    phases = [((round(ms * 1e6) + 37_000_000_000) % period_ns) / 1e6 for ms in stamps]
    centre = cmath.phase(sum(cmath.exp(2j * math.pi * x / p) for x in phases)) / (2 * math.pi) * p % p
    rel = sorted((x - centre + p / 2) % p - p / 2 for x in phases)  # around the circular mean
    steps = sorted(round(b - a, 2) for a, b in zip(stamps, stamps[1:]))
    q = lambda f: rel[min(len(rel) - 1, int(f * len(rel)))]  # noqa: E731
    return {"centre_ms": round(centre, 2) % p, "p5_ms": round((centre + q(0.05)) % p, 2), "p95_ms": round((centre + q(0.95)) % p, 2),
            "spread_ms": round(q(0.95) - q(0.05), 2), "step_ms": {"min": steps[0], "median": steps[len(steps) // 2], "max": steps[-1]}}


# ---------------------------------------------------------------------------------------------
# Container helpers

def dexec(container, *cmd, timeout=None, check=False):
    return subprocess.run([*DOCKER, "exec", container, *cmd], capture_output=True, text=True, timeout=timeout, check=check)


PROC_SCRIPT = r"""for p in /proc/[0-9]*; do
  [ -r $p/stat ] || continue
  set -- $(cut -d' ' -f14,15 $p/stat 2>/dev/null)
  echo "$(basename $p) $1 $2 $(tr '\0' ' ' < $p/cmdline | cut -c1-400)"
done"""


def process_kind(cmdline):
    if "--type=gpu-process" in cmdline:
        return "gpu"
    if "--type=renderer" in cmdline:
        return "renderer"
    if "--type=utility" in cmdline:
        m = re.search(r"--utility-sub-type=([\w.]+)", cmdline)
        sub = m.group(1) if m else "utility"
        return "audio" if "audio" in sub.lower() else "network" if "network" in sub.lower() else "utility:" + sub
    if "--type=zygote" in cmdline:
        return "zygote"
    if "--type=" in cmdline:
        return re.search(r"--type=([\w-]+)", cmdline).group(1)
    if "mxl-browser-source" in cmdline:
        return "function"
    if "grain-reader" in cmdline:
        return "reader"
    return "other"


def proc_times(container):
    """{pid: (kind, utime + stime in clock ticks)}"""
    out = dexec(container, "sh", "-c", PROC_SCRIPT, timeout=30).stdout
    procs = {}
    for line in out.splitlines():
        parts = line.split(" ", 3)
        if len(parts) < 4 or not parts[1].isdigit():
            continue
        procs[parts[0]] = (process_kind(parts[3]), int(parts[1]) + int(parts[2]))
    return procs


def cpu_by_kind(before, after, seconds, hz=100):
    kinds = {}
    for pid, (kind, ticks) in after.items():
        if kind == "reader":
            continue
        start = before.get(pid, (kind, 0))[1]  # a process started during the run counts from 0
        kinds[kind] = kinds.get(kind, 0) + (ticks - start) / hz / seconds
    return {k: round(v, 3) for k, v in sorted(kinds.items())}


# ---------------------------------------------------------------------------------------------
# Content cadence

def cadence(counter, fresh, content_fps, output_fps):
    """counter: [(grain, value)], fresh: {grain: bool} from the trace's commits. An ideal sampler of
    a clip at content_fps on output_fps advances by floor(m) or ceil(m) per grain (m = ratio), and
    the rarer step never twice in a row (at m = 1/2 the steps alternate strictly)."""
    m = Fraction(content_fps).limit_denominator(1001) / Fraction(output_fps).limit_denominator(1001)
    lo, hi = int(m), -(-m.numerator // m.denominator)
    alternate = m - lo == Fraction(1, 2)
    rare = None if lo == hi or alternate else (hi if m - lo < Fraction(1, 2) else lo)
    steps, out = {}, {"repeats": 0, "skips": 0, "repeats_fresh": 0, "skips_fresh": 0, "repeats_held": 0, "backwards": 0,
                      "grains": 0, "unknown": 0, "first_violations": []}
    prev_step = None
    for (g0, v0), (g1, v1) in zip(counter, counter[1:]):
        if g1 != g0 + 1:
            prev_step = None
            continue
        step = v1 - v0
        out["grains"] += 1
        if step < 0 or step > 1000:
            out["backwards"] += 1  # a loop or a reload
            prev_step = None
            continue
        steps[step] = steps.get(step, 0) + 1
        is_fresh = fresh.get(g1)
        if is_fresh is None:
            out["unknown"] += 1
        kind = None
        if step < lo:
            kind = "repeats"
        elif step > hi:
            kind = "skips"
        elif prev_step is not None and step == prev_step and (alternate or step == rare):
            kind = "repeats" if step == lo else "skips"
        if kind:
            out[kind] += 1 if kind == "repeats" else max(1, step - hi)
            if kind == "repeats":
                out["repeats_fresh" if is_fresh else "repeats_held"] += 1
            elif is_fresh:
                out["skips_fresh"] += 1
            if len(out["first_violations"]) < 40:
                out["first_violations"].append([g1, v0, v1, kind, bool(is_fresh)])
        prev_step = step
    out["steps"] = {str(k): v for k, v in sorted(steps.items())}
    out["ideal_steps"] = [lo, hi]
    return out


# ---------------------------------------------------------------------------------------------

def poll_trace(base, merged, stop):
    while True:
        code, trace = request(base, "GET", "/api/v1/debug/frame-trace")
        if code == 200:
            for k in ("period_ns", "frame_clock", "phase_ns"):
                merged[k] = trace.get(k, merged.get(k))
            seen = merged.setdefault("_seen", set())
            for e in trace["events"]:
                key = tuple(e)
                if key not in seen:
                    seen.add(key)
                    merged.setdefault("events", []).append(e)
        if stop.wait(20):
            break


def poll_smi(gpu, log, stop):
    while not stop.wait(1):
        out = subprocess.run(["nvidia-smi", "-i", str(gpu), "--query-gpu=utilization.gpu,memory.used", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True).stdout.strip()
        log.append([time.time(), out])


def main():
    p = argparse.ArgumentParser()
    p.add_argument("url")
    p.add_argument("--base", default="http://127.0.0.1:8460")
    p.add_argument("--container", default="ograf-mbs")
    p.add_argument("--domain", default="/Volumes/mxl/ograf-mbs")
    p.add_argument("--gpu", default="0")
    p.add_argument("--seconds", type=int, default=120)
    p.add_argument("--warmup", type=int, default=15)
    p.add_argument("--content-fps", default="50")
    p.add_argument("--label", default="")
    p.add_argument("--out", default="cadence.jsonl")
    p.add_argument("--dir", default="raw")
    p.add_argument("--smi", action="store_true", help="poll nvidia-smi every second during the run")
    p.add_argument("--chromium-trace", type=int, default=0, help="seconds of Chromium tracing (audio, media, gpu.capture)")
    p.add_argument("--no-navigate", action="store_true")
    p.add_argument("--no-audio", action="store_true")
    p.add_argument("--reader", default="mxl-bs-grain-reader", help="the grain reader inside the container")
    args = p.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    tag = re.sub(r"[^\w.-]+", "_", args.label or "run")

    info = request(args.base, "GET", "/api/v1/info")[1]
    fmt = info["format"]
    output_fps = Fraction(fmt.get("rate", "50/1"))
    senders = request(args.base, "GET", "/api/v1/nmos")[1]["senders"]
    video_flow, audio_flow = senders["video"]["flow_id"], senders.get("audio", {}).get("flow_id")

    if not args.no_navigate:
        code, body = request(args.base, "POST", "/api/v1/source/navigate", {"url": args.url})
        assert code in (200, 202), (code, body)
    time.sleep(args.warmup)

    devtools = DevTools(args.base)
    page_before = devtools.evaluate(PAGE_STATS)
    host_before = host_state(args.container, args.gpu)
    procs0 = proc_times(args.container)
    s0, m0 = request(args.base, "GET", "/api/v1/status")[1], metrics(args.base)
    t0 = time.time()

    stop = threading.Event()
    merged = {}
    smi_log = []
    threads = [threading.Thread(target=poll_trace, args=(args.base, merged, stop))]
    if args.smi:
        threads.append(threading.Thread(target=poll_smi, args=(args.gpu, smi_log, stop)))
    for t in threads:
        t.start()
    readers = [subprocess.Popen([*DOCKER, "exec", args.container, args.reader, "counter", "--domain", args.domain, "--flow", video_flow,
                                 "--width", str(fmt.get("width", 1920)), "--seconds", str(args.seconds), "--out", f"/tmp/{tag}-counter.txt"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)]
    if audio_flow and not args.no_audio:
        readers.append(subprocess.Popen([*DOCKER, "exec", args.container, args.reader, "audio", "--domain", args.domain, "--flow", audio_flow,
                                         "--seconds", str(args.seconds), "--out", f"/tmp/{tag}-audio.f32"],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    chromium = None
    if args.chromium_trace:
        time.sleep(min(10, args.seconds / 4))
        chromium = devtools.trace(args.chromium_trace)
    host_during = host_state(args.container, args.gpu)
    outputs = [r.communicate()[0] for r in readers]
    seconds = time.time() - t0
    s1, m1 = request(args.base, "GET", "/api/v1/status")[1], metrics(args.base)
    procs1 = proc_times(args.container)
    host_after = host_state(args.container, args.gpu)
    stop.set()
    for t in threads:
        t.join()
    page_after = devtools.evaluate(PAGE_STATS)
    raf = raf_phase(devtools, int(1e9 / output_fps))

    # Raw files.
    subprocess.run([*DOCKER, "cp", f"{args.container}:/tmp/{tag}-counter.txt", os.path.join(args.dir, f"{tag}-counter.txt")], capture_output=True)
    if len(readers) > 1:
        subprocess.run([*DOCKER, "cp", f"{args.container}:/tmp/{tag}-audio.f32", os.path.join(args.dir, f"{tag}-audio.f32")], capture_output=True)
        with open(os.path.join(args.dir, f"{tag}-audio.txt"), "w") as f:
            f.write(outputs[1])
    dexec(args.container, "rm", "-f", f"/tmp/{tag}-counter.txt", f"/tmp/{tag}-audio.f32")
    merged.pop("_seen", None)
    merged["events"] = sorted(merged.get("events", []))
    with open(os.path.join(args.dir, f"{tag}-trace.json"), "w") as f:
        json.dump(merged, f)

    # Analysis.
    counter = []
    try:
        with open(os.path.join(args.dir, f"{tag}-counter.txt")) as f:
            counter = [tuple(int(x) for x in line.split()) for line in f if line.strip()]
    except OSError:
        pass
    fresh = {e[2]: e[3] != 0 for e in merged["events"] if e[1] == "commit"}
    cad = cadence(counter, fresh, args.content_fps, output_fps)
    ft = frame_trace.analyse(merged) if merged["events"] else {}
    audio = {}
    if len(readers) > 1:
        m = re.search(r"RESULT (.*)", outputs[1])
        audio = dict(kv.split("=") for kv in m.group(1).split()) if m else {"error": outputs[1][-500:]}
    g0, g1 = s0["grains"], s1["grains"]
    d = {k: g1[k] - g0[k] for k in ("video", "repeated", "missed", "late_paints", "begin_frames")}
    a0, a1 = s0["audio"], s1["audio"]
    counters = {k: round(m1.get(k, 0) - m0.get(k, 0), 4) for k in m1
                if k.startswith(("audio_underruns", "audio_overruns", "audio_resets", "audio_gap", "audio_underrun_seconds", "audio_realigned",
                                 "audio_stream_events", "frame_clock_locks", "frame_clock_late"))}
    result = {
        "label": args.label, "url": args.url, "seconds": round(seconds, 1), "content_fps": args.content_fps, "output_fps": str(output_fps),
        "frame_clock": s1.get("frame_clock"), "grains": d["video"], "held": d["repeated"],
        "held_pct": round(100 * d["repeated"] / max(1, d["video"]), 3), "late_paints": d["late_paints"], "missed": d["missed"],
        "begin_frames": d["begin_frames"],
        "begin_frames_without_paint": ft.get("begin_frames_without_paint"), "held_no_paint": ft.get("held_no_paint"),
        "held_late_paint": ft.get("held_late_paint"), "trace_commits": ft.get("commits"),
        "paint_phase_ms": None if not ft.get("paint_phase") else
        {q: round(sorted(ft["paint_phase"])[int(q * (len(ft["paint_phase"]) - 1))] / 1e6, 2) for q in (0.05, 0.5, 0.95)},
        "anchors": ft.get("anchors"), "raf_phase": raf, "cadence": cad,
        "audio_flow": audio, "audio_counters": counters, "audio_status": {"before": a0, "after": a1},
        "cpu_cores": cpu_by_kind(procs0, procs1, seconds), "page": {"before": page_before, "after": page_after},
        "chromium_trace": chromium, "smi_polls": len(smi_log), "histograms": histograms(m0, m1),
        "host": [host_before, host_during, host_after],
    }
    with open(args.out, "a") as f:
        f.write(json.dumps(result, default=str) + "\n")
    dropped = None
    if page_before and page_after and "dropped" in (page_after or {}) and "dropped" in (page_before or {}):
        dropped = [page_after["dropped"] - page_before["dropped"], page_after["total"] - page_before["total"]]
    print(f"{args.label}: {d['video']} grains, held {d['repeated']} ({result['held_pct']} %), late {d['late_paints']}, missed {d['missed']}, "
          f"bf-no-paint {ft.get('begin_frames_without_paint')}; content steps {cad['steps']} repeats {cad['repeats']} "
          f"(fresh {cad['repeats_fresh']}, held {cad['repeats_held']}) skips {cad['skips']} (fresh {cad['skips_fresh']}); "
          f"phase error {(s1.get('frame_clock') or {}).get('phase_error_ms')} ms; audio {audio}; counters {counters}; "
          f"dropped {dropped}; raf {raf}; cpu {result['cpu_cores']}; behind {(chromium or {}).get('generate_more_audio_behind')}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
