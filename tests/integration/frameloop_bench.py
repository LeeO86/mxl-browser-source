#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Frame-loop benchmark (docs/performance.md, M5): late paints and held frames with n continuously
animated elements, as one page (templates/frameloop/index.html?n=N) or as n OGraf layers of the lab
graphic tests/integration/ograf/anim (the same elements at the same places), or (mode `ebu`) the
first n EBU examples on n layers as in the M4 layer test. Every element changes
on every frame, so every repeated grain while it runs is a held frame.

For each run it records the deltas of the grain counters and of every `*_seconds` histogram over the
window, the container's CPU, and the host's state before, during and after (load average, `docker
stats` of every container, GPU utilisation and memory), and appends one JSON line to the output.

    frameloop_bench.py [--base http://127.0.0.1:8460] [--container ograf-mbs] [--gpu 0]
                       [--seconds 180] [--warmup 15] [--out frameloop.jsonl] [--label text] [--trace-dir dir]
                       page|ograf|ebu N [N ...]

Standard library only. Copies templates/frameloop into the container's /config/templates (so an
image without the page works too) and uploads the lab graphic once.
"""
import argparse
import io
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FRAMELOOP = os.path.join(REPO, "templates", "frameloop")
TARGET = "/ograf/v1/renderers/main/target/graphicInstance"
# Mode "ebu": the EBU examples in the order of the M4 layer test (docs/performance.md); the form
# graphic is static once played, the others animate (some only in their in-animation).
EBU = ["mbs-form", "scoreboard", "l3rd-name", "minimal-example", "minimal-logo", "804a74b8-0076-48b1-a804-4119ba888640",
       "96123dd7-6e6a-4a0e-8084-4c4b4a2db0c3", "renderer-test"]
PREFIX = "mxl_browser_source_"
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # the lab's proxy variables never apply


def request(base, method, path, body=None, raw=None):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    headers = {"Content-Type": "application/zip" if raw is not None else "application/json"}
    req = urllib.request.Request(base + path, method=method, data=data, headers=headers)
    try:
        with OPENER.open(req, timeout=60) as response:
            text = response.read().decode()
            return response.status, json.loads(text) if text.startswith(("{", "[")) else text
    except urllib.error.HTTPError as e:
        text = e.read().decode()
        return e.code, json.loads(text) if text.startswith("{") else text


def metrics(base):
    out = {}
    for line in request(base, "GET", "/metrics")[1].splitlines():
        if line.startswith(PREFIX):
            name, value = line.rsplit(" ", 1)
            out[name[len(PREFIX):]] = float(value)
    return out


def histograms(before, after):
    """Deltas of every *_seconds histogram: {name: {"count", "mean_ms", "buckets": {le: n}}}."""
    out = {}
    for key, value in after.items():
        m = re.match(r'(\w+_seconds)_bucket\{(?:(.*),)?le="([^"]+)"\}', key)
        if not m or m.group(2):
            continue  # histograms with labels (ograf actions) are not frame timing
        name, le = m.group(1), m.group(3)
        out.setdefault(name, {"buckets": {}})["buckets"][le] = value - before.get(key, 0)
    for name, h in out.items():
        count = after.get(name + "_count", 0) - before.get(name + "_count", 0)
        total = after.get(name + "_sum", 0) - before.get(name + "_sum", 0)
        h["count"] = count
        h["mean_ms"] = round(1000 * total / count, 3) if count else None
    return out


def host_state(container, gpu):
    state = {"time": time.strftime("%H:%M:%S"), "load": open("/proc/loadavg").read().split()[:3]}
    try:
        out = subprocess.run(["docker", "stats", "--no-stream", "--format", "{{.Name}};{{.CPUPerc}};{{.MemUsage}}"],
                             capture_output=True, text=True, timeout=60).stdout
        state["containers"] = {n: c for n, c, _ in (line.split(";") for line in out.strip().splitlines())}
        state["cpu_pct"] = float(state["containers"].get(container, "-1%").rstrip("%"))
    except Exception as e:  # noqa: BLE001
        state["containers"] = str(e)
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=index,utilization.gpu,memory.used", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=20).stdout
        state["gpus"] = {i.strip(): {"util": int(u), "mem_mib": int(m)} for i, u, m in (l.split(",") for l in out.strip().splitlines())}
        state["gpu"] = state["gpus"].get(str(gpu))
    except Exception as e:  # noqa: BLE001
        state["gpus"] = str(e)
    return state


def sample_value(prop):
    if "default" in prop:
        return prop["default"]
    if "enum" in prop:
        return prop["enum"][0]
    kind = prop.get("type")
    if kind == "object":
        return {k: sample_value(v) for k, v in prop.get("properties", {}).items()}
    if kind == "array":
        return []
    return {"string": "frame loop", "number": 1, "integer": 1, "boolean": True}.get(kind)


def anim_zip():
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED) as z:
        for f in ("anim.ograf.json", "graphic.mjs"):
            z.write(os.path.join(HERE, "ograf", "anim", f), "anim/" + f)
        z.write(os.path.join(FRAMELOOP, "anim.js"), "anim/anim.js")
        for f in os.listdir(os.path.join(FRAMELOOP, "lib")):
            z.write(os.path.join(FRAMELOOP, "lib", f), "anim/lib/" + f)
    return b.getvalue()


def clear_layers(base):
    request(base, "PUT", TARGET + "/clear", {"filters": [{"renderTarget": {"layer": n}} for n in range(1, 9)]})


def setup(args, mode, n):
    base = args.base
    if mode == "page":
        if request(base, "GET", "/api/v1/source")[1].get("mode") == "ograf":
            clear_layers(base)
        subprocess.run(["docker", "exec", args.container, "mkdir", "-p", "/config/templates"], check=True)
        subprocess.run(["docker", "cp", FRAMELOOP, args.container + ":/config/templates/"], check=True, capture_output=True)
        code, body = request(base, "PATCH", "/api/v1/source?force=true",
                             {"mode": "page", "url": f"https://templates.local/frameloop/index.html?n={n}"})
        assert code == 200, body
        return
    code, body = request(base, "GET", "/ograf/v1/graphics/mbs-anim")
    if code != 200 and mode == "ograf":
        code, body = request(base, "POST", "/api/v1/ograf/graphics", raw=anim_zip())
        assert code in (200, 201), body
    if request(base, "GET", "/api/v1/source")[1].get("mode") != "ograf":
        code, body = request(base, "PATCH", "/api/v1/source", {"mode": "ograf"})
        assert code == 200, body
        time.sleep(3)
    clear_layers(base)
    for layer in range(1, n + 1):
        graphic, data = "mbs-anim", {"slot": layer - 1}
        if mode == "ebu":
            graphic = EBU[layer - 1]
            schema = request(base, "GET", "/ograf/v1/graphics/" + graphic)[1]["graphic"].get("schema") or {}
            data = {k: sample_value(v) for k, v in schema.get("properties", {}).items()}
        code, body = request(base, "POST", TARGET + "/load",
                             {"renderTarget": {"layer": layer}, "graphicId": graphic, "params": {"data": data}})
        assert code == 200 and body.get("statusCode", 200) < 400, body
        code, body = request(base, "POST", TARGET + "/playAction",
                             {"renderTarget": {"layer": layer}, "graphicInstanceId": body["graphicInstanceId"], "params": {}})
        assert code == 200, body


def run(args, mode, n):
    setup(args, mode, n)
    time.sleep(args.warmup)
    host_before = host_state(args.container, args.gpu)
    s0, m0 = request(args.base, "GET", "/api/v1/status")[1], metrics(args.base)
    time.sleep(args.seconds / 2)
    host_during = host_state(args.container, args.gpu)
    time.sleep(args.seconds / 2)
    s1, m1 = request(args.base, "GET", "/api/v1/status")[1], metrics(args.base)
    host_after = host_state(args.container, args.gpu)
    g0, g1 = s0["grains"], s1["grains"]
    d = {k: g1[k] - g0[k] for k in ("video", "repeated", "missed", "late_paints", "begin_frames")}
    h = histograms(m0, m1)
    paints = h.get("paint_latency_seconds", {}).get("count", 0)
    result = {
        "label": args.label, "mode": mode, "n": n, "seconds": args.seconds, "grains": d["video"], "paints": paints,
        "held": d["repeated"], "held_pct": round(100 * d["repeated"] / max(1, d["video"]), 3),
        "late_paints": d["late_paints"], "late_pct": round(100 * d["late_paints"] / max(1, d["video"]), 3),
        "missed": d["missed"], "begin_frames": d["begin_frames"], "page_state": s1["page"]["state"],
        "render": s1.get("render"), "histograms": h, "host": [host_before, host_during, host_after],
    }
    with open(args.out, "a") as f:
        f.write(json.dumps(result) + "\n")
    if args.trace_dir:  # the newest frame-loop events (images with GET /api/v1/debug/frame-trace)
        code, trace = request(args.base, "GET", "/api/v1/debug/frame-trace")
        if code == 200:
            os.makedirs(args.trace_dir, exist_ok=True)
            with open(os.path.join(args.trace_dir, f"{args.label or 'run'}-{mode}-{n}.json"), "w") as f:
                json.dump(trace, f)
    cpu = [x.get("cpu_pct") for x in result["host"]]
    print(f"{args.label} {mode} n={n}: {d['video']} grains, {paints} paints, held {d['repeated']} ({result['held_pct']} %), "
          f"late {d['late_paints']} ({result['late_pct']} %), missed {d['missed']}, paint mean {h.get('paint_latency_seconds', {}).get('mean_ms')} ms, "
          f"cpu {cpu}, load {[x['load'][0] for x in result['host']]}, gpu {[x.get('gpu') for x in result['host']]}", flush=True)
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--base", default="http://127.0.0.1:8460")
    p.add_argument("--container", default="ograf-mbs")
    p.add_argument("--gpu", default="0")
    p.add_argument("--seconds", type=float, default=180)
    p.add_argument("--warmup", type=float, default=15)
    p.add_argument("--out", default="frameloop.jsonl")
    p.add_argument("--label", default="")
    p.add_argument("--trace-dir", default="")
    p.add_argument("mode", choices=["page", "ograf", "ebu"])
    p.add_argument("n", type=int, nargs="+")
    args = p.parse_args()
    for n in args.n:
        run(args, args.mode, n)
    if args.mode != "page":
        clear_layers(args.base)


if __name__ == "__main__":
    sys.exit(main())
