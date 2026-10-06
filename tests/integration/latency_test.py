#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Click → picture (SPEC §2.5, §17): clicks interact.html's button through /api/v1/interact; each
click toggles a marker square, and the image's grain reader (`docker exec`, `DOCKER='sudo docker'`
if needed) finds the first grain that shows it. Latency = T(grain) − TAI time of the click (both on
this host's CLOCK_TAI, so run it on the container's host). The ack's grain must not be later than
that grain. Standard library only.

    latency_test.py [host:port] [container] [domain-dir] [clicks]
"""
import os
import re
import statistics
import subprocess
import sys
import time

from interact_test import ws_connect, ws_messages, ws_send
from ops_test import CONTAINER, HOST, PORT, call, navigate

DOMAIN = sys.argv[3] if len(sys.argv) > 3 else "/Volumes/mxl/browser-source"
CLICKS = int(sys.argv[4]) if len(sys.argv) > 4 else 10
BUDGET_MS = 150


def main():
    info = call("GET", "/api/v1/info")[1]["format"]
    num, den = (int(v) for v in info["rate"].split("/"))
    flow = call("GET", "/api/v1/nmos")[1]["senders"]["video"]["flow_id"]
    navigate("interact.html")
    control = ws_connect(HOST, int(PORT), "/api/v1/interact")
    ws_send(control, {"type": "hello", "client": "latency_test", "protocol": 1})
    ws_send(control, {"type": "interact", "enable": True})
    ws_send(control, {"type": "preview", "fps": 1, "width": 160})  # acks queue behind preview JPEGs
    time.sleep(1)
    acks = {}
    latencies = []
    for seq in range(1, CLICKS + 1):
        reader = subprocess.Popen([*os.environ.get("DOCKER", "docker").split(), "exec", CONTAINER, "mxl-bs-grain-reader", "change", "--domain", DOMAIN,
                                   "--flow", flow, "--width", str(info["width"]), "--x", "1700", "--y", "200", "--seconds", "3"],
                                  stdout=subprocess.PIPE, text=True)
        time.sleep(0.5)  # the reader has its head grain
        sent = time.clock_gettime_ns(time.CLOCK_TAI)
        for action in ("down", "up"):
            ws_send(control, {"type": "pointer", "action": action, "x": 300 / 1920, "y": 200 / 1080, "button": "left", "clicks": 1, "seq": seq})
        out, _ = reader.communicate(timeout=10)
        found = re.search(r"CHANGE grain=(\d+)", out)
        assert found, f"click {seq}: no grain shows it ({out.strip()})"
        grain = int(found.group(1))
        end = time.time() + 2
        while seq not in acks and time.time() < end:
            for m in ws_messages(control, 0.2):
                if m.get("type") == "ack":
                    acks.setdefault(m["seq"], []).append(m["grain"])
        assert seq in acks and max(acks[seq]) <= grain, f"click {seq}: ack grain {acks.get(seq)} after the first grain showing it ({grain})"
        latency = (grain * den * 1_000_000_000 // num - sent) / 1e6
        latencies.append(latency)
        time.sleep(0.3)
    print(f"click -> picture over {CLICKS} clicks: median {statistics.median(latencies):.0f} ms, max {max(latencies):.0f} ms "
          f"(T(grain) - click; budget {BUDGET_MS} ms); every ack grain <= the first grain showing the click")
    assert max(latencies) <= BUDGET_MS, latencies
    print("OK")


if __name__ == "__main__":
    main()
