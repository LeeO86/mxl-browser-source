#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Template calls, back and forward, renderer crash, page hang and the DevTools tunnel (SPEC §17)
against a running container. Kills the renderer with `docker exec <container> kill -9`
(`DOCKER='sudo docker'` if needed). Standard library only.

    ops_test.py [host:port] [container] [devtools]

`devtools` also checks /devtools (the container must run with BROWSER_DEVTOOLS=true).
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

from interact_test import ws_connect, ws_messages, ws_send

HOST, PORT = (sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:8160").split(":")
CONTAINER = sys.argv[2] if len(sys.argv) > 2 else "mxl-browser-source"
BASE = f"http://{HOST}:{PORT}"


def call(method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(BASE + path, method=method, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        text = response.read().decode()
        return response.status, (json.loads(text) if text.startswith(("{", "[")) else text)


def status():
    return call("GET", "/api/v1/status")[1]


def metric(name):
    text = call("GET", "/metrics")[1]
    return sum(float(line.rsplit(" ", 1)[1]) for line in text.splitlines()
               if line.startswith("mxl_browser_source_" + name) and not line.startswith("#"))


def wait_state(states, seconds):
    """Seconds until page.state is one of `states` (None on timeout)."""
    start = time.time()
    while time.time() - start < seconds:
        if status()["page"]["state"] in states:
            return time.time() - start
        time.sleep(0.1)
    return None


def navigate(page):
    url = f"https://templates.local/{page}"
    call("POST", "/api/v1/source/navigate", {"url": url})
    end = time.time() + 10
    while time.time() < end:
        s = status()["page"]
        if s["url"] == url and s["state"] == "loaded":
            return
        time.sleep(0.1)
    raise AssertionError(f"{page} does not load")


def docker(*args):
    return subprocess.run([*os.environ.get("DOCKER", "docker").split(), "exec", CONTAINER, *args], capture_output=True, text=True).stdout


def renderer_pids():
    script = 'for p in /proc/[0-9]*; do tr "\\0" " " < $p/cmdline 2>/dev/null | grep -q -- "--type=renderer" && echo ${p#/proc/}; done'
    return docker("sh", "-c", script).split()


def templates():
    navigate("lower-third.html")
    events = ws_connect(HOST, int(PORT), "/api/v1/events")
    assert call("POST", "/api/v1/template/update", {"data": json.dumps({"name": "Template", "title": "update()"})}) == (202, {"queued": True})
    call("POST", "/api/v1/template/play")
    played = [m["data"] for m in ws_messages(events, 2) if m.get("type") == "event"]
    assert {"played": True} in played, played
    call("POST", "/api/v1/template/invoke", {"function": "update", "args": ["invoked"]})
    call("POST", "/api/v1/template/stop")
    print(f"templates: update, play (page reported {played}), invoke, stop accepted")


def wait_page(check, seconds=10):
    """True when check(status page) holds within `seconds`."""
    end = time.time() + seconds
    while time.time() < end:
        if check(status()["page"]):
            return True
        time.sleep(0.1)
    return False


def history():
    navigate("blank.html")
    navigate("bars.html")
    assert wait_page(lambda p: p["can_go_back"] and not p["can_go_forward"]), status()["page"]
    assert call("POST", "/api/v1/source/back") == (202, {"queued": True})
    assert wait_page(lambda p: p["url"].endswith("/blank.html") and p["state"] == "loaded" and p["can_go_forward"]), status()["page"]
    call("POST", "/api/v1/source/forward")
    assert wait_page(lambda p: p["url"].endswith("/bars.html") and p["state"] == "loaded" and not p["can_go_forward"]), status()["page"]
    print("history: back to blank.html, forward to bars.html; can_go_back and can_go_forward follow")


def crash():
    navigate("counter.html")
    before = status()["grains"]
    crashes = metric("renderer_crashes_total")
    pids = renderer_pids()
    assert pids, "no renderer process"
    docker("kill", "-9", *pids)
    crashed = wait_state(("crashed",), 5)
    recovered = wait_state(("loaded",), 15)
    after = status()["grains"]
    assert crashed is not None and recovered is not None, "no crash/recovery seen"
    assert metric("renderer_crashes_total") == crashes + 1
    assert after["missed"] == before["missed"], (before, after)
    print(f"crash: state crashed after {crashed:.1f} s, loaded {recovered:.1f} s later; "
          f"{after['video'] - before['video']} grains written meanwhile, missed +0")


def hang():
    navigate("hang.html")
    before = status()["grains"]
    hangs = metric("page_hangs_total")
    killed = metric('renderer_crashes_total{reason="hung"}')
    hung = wait_state(("hung", "crashed"), 15)
    assert hung is not None, "hang not detected"
    assert wait_state(("crashed",), 3) is not None, "a hang continues as a crash"
    assert metric('renderer_crashes_total{reason="hung"}') == killed + 1
    # The page hangs again 3 s after every reload: leave it for a blank page.
    call("POST", "/api/v1/source/navigate", {"url": "https://templates.local/blank.html"})
    recovered = wait_state(("loaded",), 15)
    after = status()["grains"]
    assert recovered is not None and metric("page_hangs_total") >= hangs + 1
    assert after["missed"] == before["missed"], (before, after)
    print(f"hang: detected {hung:.1f} s after the load (3 s page delay + timeout), renderer killed, blank.html loaded {recovered:.1f} s later; missed +0")


def devtools():
    pages = call("GET", "/devtools/json/list")[1]
    page = next(p for p in pages if p.get("type") == "page")
    assert f"{HOST}:{PORT}/devtools/page/" in page["webSocketDebuggerUrl"], page
    assert page["devtoolsFrontendUrl"].startswith("/devtools/inspector.html?ws="), page
    sock = ws_connect(HOST, int(PORT), "/devtools/page/" + page["id"])
    ws_send(sock, {"id": 1, "method": "Runtime.evaluate", "params": {"expression": "6*7"}})
    answer = [m for m in ws_messages(sock, 2) if m.get("id") == 1]
    assert answer and answer[0]["result"]["result"]["value"] == 42, answer
    front = urllib.request.urlopen(BASE + page["devtoolsFrontendUrl"], timeout=10)
    assert front.status == 200 and b"<html" in front.read().lower()
    print(f"devtools: {len(pages)} targets, Runtime.evaluate through the tunnel = 42, frontend served")


if __name__ == "__main__":
    templates()
    history()
    crash()
    hang()
    if "devtools" in sys.argv[3:]:
        devtools()
    print("OK")
