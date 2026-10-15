#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""OGraf (docs/ograf.md) against a running container: upload (the probe graphic and the EBU
examples), mode ograf, 8 layers, load/play/update/custom/stop through the Server API, the layer
state, network=local, the template API refused, renderer crash → restore to the same step, mode
change refused while layers are loaded, delete with force. The output must keep its grains.
Standard library only.

    ograf_test.py [host:port] [container] [ebu-examples-dir]
"""
import io
import json
import os
import sys
import time
import urllib.error
import urllib.request
import zipfile

from interact_test import ws_connect, ws_messages
from ops_test import BASE, HOST, PORT, docker, metric, renderer_pids, status
from pages_test import Output

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = sys.argv[3] if len(sys.argv) > 3 else ""
TARGET = "/ograf/v1/renderers/main/target/graphicInstance"


def request(method, path, body=None, raw=None, query=""):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    headers = {"Content-Type": "application/zip" if raw is not None else "application/json"}
    req = urllib.request.Request(BASE + path + query, method=method, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            text = response.read().decode()
            return response.status, json.loads(text) if text else {}
    except urllib.error.HTTPError as e:
        text = e.read().decode()
        return e.code, json.loads(text) if text.startswith("{") else text


def zip_dir(path):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(path):
            for f in files:
                full = os.path.join(root, f)
                z.write(full, os.path.relpath(full, os.path.dirname(path)))  # one top folder, as people zip
    return out.getvalue()


def probes(sock, seconds):
    return [m["data"]["probe"] for m in ws_messages(sock, seconds) if m.get("type") == "event" and "probe" in m.get("data", {})]


def layers():
    return request("GET", "/api/v1/ograf/layers")[1]["layers"]


def wait(check, seconds, what):
    end = time.time() + seconds
    while time.time() < end:
        if check():
            return
        time.sleep(0.25)
    raise AssertionError("timed out waiting for " + what)


def renderer_ok():
    return request("GET", "/ograf/v1/renderers/main")[1]["renderer"]["status"]["status"] == "OK"


def upload():
    code, body = request("POST", "/api/v1/ograf/graphics", raw=zip_dir(os.path.join(HERE, "ograf", "probe")))
    assert code == 200, body
    code, body = request("POST", "/api/v1/ograf/graphics", raw=b"not a zip")
    assert code == 422, body
    count, ids = 1, {"mbs-probe"}
    if EXAMPLES:
        for name in sorted(os.listdir(EXAMPLES)):
            path = os.path.join(EXAMPLES, name)
            if os.path.isdir(path):
                code, body = request("POST", "/api/v1/ograf/graphics", raw=zip_dir(path))
                assert code == 200, (name, body)
                count += len(body["graphics"])
                ids |= {g["id"] for g in body["graphics"]}
    listed = {g["id"] for g in request("GET", "/ograf/v1/graphics")[1]["graphics"]}
    assert ids <= listed, (ids - listed, listed)  # the library may hold others (a lab run)
    print(f"upload: {count} graphics (probe + {count - 1} EBU examples), a broken ZIP refused with 422")


def eight_layers(events):
    code, _ = request("PATCH", "/api/v1/source", {"mode": "ograf"})
    assert code == 200
    wait(renderer_ok, 30, "the renderer page")
    instances = {}
    with Output() as out:
        for n in range(1, 9):
            code, body = request("POST", TARGET + "/load", {"renderTarget": {"layer": n}, "graphicId": "mbs-probe", "params": {"data": {"text": f"L{n}"}}})
            assert code == 200 and body["statusCode"] == 200, body
            instances[n] = body["graphicInstanceId"]
        for n in range(1, 9):
            code, body = request("POST", TARGET + "/playAction", {"renderTarget": {"layer": n}, "graphicInstanceId": instances[n], "params": {}})
            assert code == 200 and body["currentStep"] == 0, body
        seen = probes(events, 1)
        assert sum(1 for p in seen if p["call"] == "load") == 8, seen
        load = next(p for p in seen if p["call"] == "load")
        assert load["renderType"] == "realtime" and load["renderCharacteristics"]["resolution"]["width"] > 0, load
        assert all(l["state"] == "on" and l["step"] == 0 for l in layers())

        code, body = request("POST", TARGET + "/updateAction", {"renderTarget": {"layer": 3}, "graphicInstanceId": instances[3], "params": {"data": {"color": "#00ff00"}}})
        assert code == 200, body
        assert layers()[2]["data"] == {"text": "L3", "color": "#00ff00"}, layers()[2]
        code, body = request("POST", TARGET + "/playAction", {"renderTarget": {"layer": 3}, "graphicInstanceId": instances[3], "params": {"delta": 1}})
        assert code == 200 and body["currentStep"] == 1, body
        code, body = request("POST", TARGET + "/playAction", {"renderTarget": {"layer": 3}, "graphicInstanceId": "wrong", "params": {}})
        assert code == 404, body

        # network=local: a graphic cannot reach outside the library.
        blocked = metric('navigation_blocked_total{reason="ograf_local"}')
        code, body = request("POST", TARGET + "/customActions/fetch", {"renderTarget": {"layer": 8}, "graphicInstanceId": instances[8], "params": {"payload": {"url": "https://example.com/"}}})
        assert code == 200 and body["statusCode"] == 502, body
        assert metric('navigation_blocked_total{reason="ograf_local"}') > blocked

        code, _ = request("POST", "/api/v1/template/play", {})
        assert code == 409
    print(f"8 layers: load, play, update (merged), next step, custom action; network=local blocked a fetch; template API 409; {out.rate:.1f} grains/s, missed +0")
    return instances


def crash_restores(events, instances):
    before = layers()
    probes(events, 1)  # drop the events from before the crash
    pids = renderer_pids()
    assert pids, "no renderer process"
    docker("kill", "-9", *pids)
    wait(lambda: status()["page"]["state"] in ("crashed", "loading"), 10, "the crash")
    wait(renderer_ok, 30, "the renderer page after the crash")
    wait(lambda: all(l["state"] == b["state"] for l, b in zip(layers(), before)), 20, "the restore")
    after = layers()
    assert after[2]["step"] == 1 and after[2]["data"]["color"] == "#00ff00", after[2]
    replays = [p for p in probes(events, 1) if p["call"] == "playAction"]
    assert replays and all(p["skipAnimation"] for p in replays), replays
    print(f"crash: renderer killed, 8 layers restored to their steps without animation (layer 3 at step {after[2]['step']})")


def ebu_renderer_test():
    if not EXAMPLES:
        return
    code, body = request("POST", TARGET + "/load", {"renderTarget": {"layer": 1}, "graphicId": "renderer-test", "params": {"data": {}}})
    if code == 404:
        listed = [g["id"] for g in request("GET", "/ograf/v1/graphics")[1]["graphics"]]
        raise AssertionError(f"renderer-test not in the library: {listed}")
    assert code == 200 and body["statusCode"] < 400, body
    instance = body["graphicInstanceId"]
    for verb, params in (("playAction", {}), ("updateAction", {"data": {"message": "x"}}), ("playAction", {}), ("stopAction", {})):
        code, body = request("POST", TARGET + "/" + verb, {"renderTarget": {"layer": 1}, "graphicInstanceId": instance, "params": params})
        assert code == 200, (verb, body)
    print("EBU renderer-test: load, play, update, play, stop answered")


def leave_mode():
    code, body = request("PATCH", "/api/v1/source", {"mode": "page"})
    assert code == 409, body
    code, body = request("PATCH", "/api/v1/source", {"mode": "page"}, query="?force=true")
    assert code == 200, body
    assert all(l["state"] == "empty" for l in layers())
    code, body = request("DELETE", "/ograf/v1/graphics/mbs-probe", query="?force=true")
    assert code == 200, body
    assert request("GET", "/ograf/v1/graphics/mbs-probe")[0] == 404
    print("mode page: refused while layers held graphics, forced: layers cleared; the probe deleted")


def main():
    events = ws_connect(HOST, int(PORT), "/api/v1/events")
    upload()
    instances = eight_layers(events)
    crash_restores(events, instances)
    ebu_renderer_test()
    leave_mode()
    print("ograf tests passed")


if __name__ == "__main__":
    main()
