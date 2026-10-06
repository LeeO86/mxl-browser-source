#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""IS-05 sender enable (SPEC §8.2, §17) against a running container: every sender is disabled and
enabled through the Connection API (active document, /api/v1/nmos, sender_enabled). The video flow
is read with the image's grain reader (`docker exec`, `DOCKER='sudo docker'` if needed): no new
grains while disabled. Then one sender stays disabled across `docker restart`. Standard library only.

    nmos_test.py [host:port] [container] [domain-dir] [nmos-port]

`domain-dir` is MXL_OUTPUT_DOMAIN_DIR inside the container; without it the flow is not read.
"""
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

from ops_test import BASE, CONTAINER, HOST, call, metric

DOMAIN = sys.argv[3] if len(sys.argv) > 3 else ""
NMOS = f"http://{HOST}:{sys.argv[4] if len(sys.argv) > 4 else 3312}/x-nmos/connection/v1.2/single/senders/"


def connection(method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(NMOS + path, method=method, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read().decode())


def enable(sender, on):
    staged = connection("PATCH", f"{sender}/staged", {"master_enable": on, "activation": {"mode": "activate_immediate"}})
    assert staged["master_enable"] is on, staged


def check(name, s, on):
    assert connection("GET", f"{s['id']}/active")["master_enable"] is on, name
    assert call("GET", "/api/v1/nmos")[1]["senders"][name]["enabled"] is on, name
    assert metric(f'sender_enabled{{sender="{name}"}}') == (1 if on else 0), name


def grains_read(flow, seconds=2):
    """Grains of `flow` the reader got within `seconds` (None without a domain)."""
    if not DOMAIN:
        return None
    width = call("GET", "/api/v1/info")[1]["format"]["width"]
    out = subprocess.run([*os.environ.get("DOCKER", "docker").split(), "exec", CONTAINER, "mxl-bs-grain-reader", "counter", "--domain", DOMAIN,
                          "--flow", flow, "--width", str(width), "--seconds", str(seconds)], capture_output=True, text=True).stdout
    found = re.search(r"RESULT grains=(\d+)", out)
    assert found, out
    return int(found.group(1))


def wait_ready(seconds=60):
    end = time.time() + seconds
    while time.time() < end:
        try:
            with urllib.request.urlopen(BASE + "/readyz", timeout=2) as response:
                if response.status == 200:
                    return
        except OSError:
            pass
        time.sleep(1)
    raise AssertionError("not ready after restart")


def toggle(name, s):
    enable(s["id"], False)
    check(name, s, False)
    off = grains_read(s["flow_id"]) if name == "video" else None
    enable(s["id"], True)
    check(name, s, True)
    on = grains_read(s["flow_id"]) if name == "video" else None
    if off is not None:
        # The reader starts with the head grain, which was written before the disable: at most 1.
        assert off <= 1 and on >= 90, (off, on)
    reads = f"; grains read in 2 s: {off} disabled, {on} enabled" if off is not None else ""
    print(f"{name}: disable and enable through IS-05, active document, /api/v1/nmos and sender_enabled agree{reads}")


def restart(name, s):
    enable(s["id"], False)
    subprocess.run([*os.environ.get("DOCKER", "docker").split(), "restart", CONTAINER], check=True, capture_output=True)
    wait_ready()
    check(name, s, False)
    others = {n: v for n, v in call("GET", "/api/v1/nmos")[1]["senders"].items() if n != name}
    assert all(v["enabled"] for v in others.values()), others
    enable(s["id"], True)
    check(name, s, True)
    print(f"restart: {name} stays disabled (active document, /api/v1/nmos, sender_enabled), the other senders enabled; enabled again")


if __name__ == "__main__":
    senders = call("GET", "/api/v1/nmos")[1]["senders"]
    for name, s in senders.items():
        check(name, s, True)
        toggle(name, s)
    last = list(senders)[-1]
    restart(last, senders[last])
    print("OK")
