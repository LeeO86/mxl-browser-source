#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""OGraf soak (docs/ograf.md §12, M4) against a running container in mode `ograf`: 8 layers, each in a
loop of load / play / update / next / stop / custom / clear through /ograf/v1, over every listed graphic
except the lab ones (`mbs-*`, but `mbs-form` stays). Once a minute it writes one CSV row: grains,
repeats, misses, late paints, the ograf_* metrics, CEF RSS, GPU memory (nvidia-smi), the JS heap of the
renderer process (performance.memory, posted through window.mxlBrowserSource.post), the container's CPU
and memory, console errors, and the actions run and failed since the last row. Standard library only.

    ograf_soak.py [host:port] [container] [minutes] [out.csv] [gpu-index]
"""
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

from interact_test import ws_connect, ws_messages

HOST, PORT = (sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:8160").split(":")
CONTAINER = sys.argv[2] if len(sys.argv) > 2 else "mxl-browser-source"
MINUTES = float(sys.argv[3]) if len(sys.argv) > 3 else 120
CSV = sys.argv[4] if len(sys.argv) > 4 else "ograf-soak.csv"
GPU = sys.argv[5] if len(sys.argv) > 5 else "0"
BASE = f"http://{HOST}:{PORT}"
TARGET = "/ograf/v1/renderers/main/target/graphicInstance"
PAUSE = 0.6  # seconds between two actions on one layer
DOCKER = os.environ.get("DOCKER", "docker").split()

lock = threading.Lock()
counts = {}    # (action, result) -> n, reset every row
totals = {}    # (action, result) -> n
failures = []  # (time, layer, graphic, action, code, detail)
stop = threading.Event()


def request(method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, method=method, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            text = response.read().decode()
            return response.status, json.loads(text) if text.startswith(("{", "[")) else text
    except urllib.error.HTTPError as e:
        text = e.read().decode()
        return e.code, json.loads(text) if text.startswith("{") else text
    except Exception as e:  # noqa: BLE001 - a soak counts every failure
        return 0, str(e)


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
    return {"string": "soak", "number": 1, "integer": 1, "boolean": True}.get(kind)


def sample_data(schema, text):
    data = {k: sample_value(v) for k, v in (schema or {}).get("properties", {}).items()}
    for k, v in (schema or {}).get("properties", {}).items():
        if v.get("type") == "string" and "enum" not in v and not v.get("gddType", "").startswith(("color", "file")):
            data[k] = text[: v.get("maxLength", 60)]
    return data


def graphics():
    out = []
    for g in request("GET", "/ograf/v1/graphics")[1]["graphics"]:
        if g["id"].startswith("mbs-") and g["id"] != "mbs-form":
            continue
        manifest = request("GET", "/ograf/v1/graphics/" + g["id"])[1]["graphic"]
        out.append(manifest)
    return out


def act(layer, manifest, action, verb, params, instance=None):
    body = {"renderTarget": {"layer": layer}, "params": params}
    if verb == "load":
        body = {"renderTarget": {"layer": layer}, "graphicId": manifest["id"], "params": params}
    elif instance:
        body["graphicInstanceId"] = instance
    if verb == "clear":
        code, answer = request("PUT", TARGET + "/clear", {"filters": [{"renderTarget": {"layer": layer}}]})
    else:
        code, answer = request("POST", TARGET + "/" + verb, body)
    inner = answer.get("statusCode", 200) if isinstance(answer, dict) else 0
    result = "ok" if code == 200 and inner < 400 else "fail"
    with lock:
        counts[(action, result)] = counts.get((action, result), 0) + 1
        totals[(action, result)] = totals.get((action, result), 0) + 1
        if result == "fail":
            failures.append((time.strftime("%H:%M:%S"), layer, manifest["id"], action, f"{code}/{inner}", str(answer)[:200]))
    time.sleep(PAUSE)
    return answer if isinstance(answer, dict) else {}


def layer_loop(layer, library):
    i = layer - 1
    while not stop.is_set():
        m = library[i % len(library)]
        i += 1
        schema = m.get("schema")
        loaded = act(layer, m, "load", "load", {"data": sample_data(schema, f"Layer {layer} pass {i}")})
        instance = loaded.get("graphicInstanceId")
        if not instance:
            continue
        act(layer, m, "play", "playAction", {}, instance)
        act(layer, m, "update", "updateAction", {"data": sample_data(schema, f"Layer {layer} update {i}")}, instance)
        if m.get("stepCount", 1) != 1:
            act(layer, m, "next", "playAction", {"delta": 1}, instance)
        for custom in m.get("customActions", []):
            params = {"payload": sample_data(custom.get("schema"), "ticker")} if custom.get("schema") else {}
            act(layer, m, "custom", "customActions/" + custom["id"], params, instance)
        act(layer, m, "stop", "stopAction", {}, instance)
        act(layer, m, "clear", "clear", {})


def metrics():
    text = request("GET", "/metrics")[1]
    out = {}
    for line in text.splitlines():
        if line.startswith("#") or not line.startswith("mxl_browser_source_"):
            continue
        name, value = line.rsplit(" ", 1)
        out[name[len("mxl_browser_source_"):]] = float(value)
    return out


def msum(m, prefix):
    return sum(v for k, v in m.items() if k == prefix or k.startswith(prefix + "{"))


def gpu_memory_mib():
    try:
        out = subprocess.run(["nvidia-smi", "-i", GPU, "--query-gpu=memory.used,utilization.gpu", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=10).stdout.strip()
        used, util = (x.strip() for x in out.split(","))
        return int(used), int(util)
    except Exception:  # noqa: BLE001
        return -1, -1


def container_stats():
    out = subprocess.run([*DOCKER, "stats", "--no-stream", "--format", "{{.CPUPerc}};{{.MemUsage}}", CONTAINER],
                         capture_output=True, text=True, timeout=30).stdout.strip()
    try:
        cpu, mem = out.split(";")
        used = mem.split("/")[0].strip()
        number, unit = re.match(r"([\d.]+)\s*([KMGT]?i?B)", used).groups()
        scale = {"B": 1, "KiB": 1 / 1024, "MiB": 1, "GiB": 1024, "TiB": 1024 * 1024}.get(unit, 1)
        return float(cpu.rstrip("%")), round(float(number) * scale, 1)
    except Exception:  # noqa: BLE001
        return -1, -1


def js_heap_mib():
    try:
        sock = ws_connect(HOST, int(PORT), "/api/v1/events")
        ws_messages(sock, 0.3)
        js = ("window.mxlBrowserSource && performance.memory && window.mxlBrowserSource.post("
              "{soakHeap:{used:performance.memory.usedJSHeapSize,total:performance.memory.totalJSHeapSize}})")
        request("POST", "/api/v1/source/execute", {"js": js})
        for m in ws_messages(sock, 2):
            data = m.get("data", {}) if m.get("type") == "event" else {}
            if "soakHeap" in data:
                sock.close()
                return round(data["soakHeap"]["used"] / 1048576, 1), round(data["soakHeap"]["total"] / 1048576, 1)
        sock.close()
    except Exception:  # noqa: BLE001
        pass
    return -1, -1


COLUMNS = ["time", "minute", "video_grains", "repeated", "missed", "late_paints", "begin_frames", "page_state",
           "cef_rss_mib", "gpu_mem_mib", "gpu_util", "js_heap_used_mib", "js_heap_total_mib", "cpu_pct", "mem_mib",
           "actions_ok", "actions_fail", "ograf_actions_ok_total", "ograf_actions_error_total", "ograf_actions_timeout_total",
           "ograf_layers_on", "ograf_layers_error", "ograf_graphics", "ograf_restores_total", "navigation_blocked_ograf_local",
           "console_errors_new", "console_last_error"]


def row(minute, seen_console):
    s = request("GET", "/api/v1/status")[1]
    m = metrics()
    gpu, util = gpu_memory_mib()
    used, total = js_heap_mib()
    cpu, mem = container_stats()
    console = request("GET", "/api/v1/console")[1].get("messages", [])
    new = [c for c in console if c.get("level") == "error" and (c["time"], c["message"]) not in seen_console]
    seen_console.update((c["time"], c["message"]) for c in console)
    with lock:
        ok = sum(v for (a, r), v in counts.items() if r == "ok")
        fail = sum(v for (a, r), v in counts.items() if r == "fail")
        counts.clear()
    g = s["grains"]
    return [time.strftime("%H:%M:%S"), minute, g["video"], g["repeated"], g["missed"], g["late_paints"], g["begin_frames"],
            s["page"]["state"], round(msum(m, "cef_processes_resident_bytes") / 1048576, 1), gpu, util, used, total, cpu, mem,
            ok, fail, sum(v for k, v in m.items() if k.startswith("ograf_actions_total") and 'result="ok"' in k),
            sum(v for k, v in m.items() if k.startswith("ograf_actions_total") and 'result="error"' in k),
            sum(v for k, v in m.items() if k.startswith("ograf_actions_total") and 'result="timeout"' in k),
            m.get('ograf_layers{state="on"}', 0), m.get('ograf_layers{state="error"}', 0), m.get("ograf_graphics", 0),
            msum(m, "ograf_restores_total"), m.get('navigation_blocked_total{reason="ograf_local"}', 0),
            len(new), (new[-1]["message"][:120].replace(",", ";").replace("\n", " ") if new else "")]


def main():
    library = graphics()
    print("graphics:", ", ".join(g["id"] for g in library), flush=True)
    seen_console = set()
    with open(CSV, "w") as f:
        f.write(",".join(COLUMNS) + "\n")
        f.write(",".join(map(str, row(0, seen_console))) + "\n")
        f.flush()
        threads = [threading.Thread(target=layer_loop, args=(n, library), daemon=True) for n in range(1, 9)]
        for t in threads:
            t.start()
        start = time.time()
        minute = 0
        while minute < MINUTES:
            minute += 1
            time.sleep(max(0, start + 60 * minute - time.time()))
            r = row(minute, seen_console)
            f.write(",".join(map(str, r)) + "\n")
            f.flush()
            print(" ".join(f"{c}={v}" for c, v in zip(COLUMNS, r) if c in ("time", "minute", "missed", "late_paints", "repeated", "cef_rss_mib", "gpu_mem_mib", "js_heap_used_mib", "actions_ok", "actions_fail")), flush=True)
        stop.set()
        for t in threads:
            t.join(timeout=60)
    request("PUT", TARGET + "/clear", {"filters": [{}]})
    print("totals:", json.dumps({f"{a}/{r}": v for (a, r), v in sorted(totals.items())}))
    print(f"failures: {len(failures)}")
    for f in failures[:200]:
        print("  ", *f)


if __name__ == "__main__":
    main()
