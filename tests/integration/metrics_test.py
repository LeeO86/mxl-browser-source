#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Right after start, /metrics has the event counters of SPEC §12 at 0 (1.3.3): before, a counter
had no series until its first event. Run on a fresh container, before any other test. Standard
library only.

    metrics_test.py [host:port]
"""
import sys
import urllib.request

ZERO = [
    "page_hangs_total",
    "preview_frames_total",
    "preview_frames_dropped_total",
    "popups_blocked_total",
    "downloads_blocked_total",
    'renderer_crashes_total{reason="crashed"}',
    'renderer_crashes_total{reason="hung"}',
    'js_dialogs_total{type="alert"}',
    'permission_denied_total{type="camera"}',
    'navigation_blocked_total{reason="loopback"}',
    'navigation_blocked_total{reason="service_worker"}',
    'ograf_actions_total{action="play",result="ok"}',
    'ograf_uploads_total{result="ok"}',
    'ograf_restores_total{result="ok"}',
    'interaction_events_total{result="ok",type="pointer"}',
    'interaction_events_total{result="dropped",type="any"}',
]
PRESENT = ['page_loads_total{result="ok"}', 'page_loads_total{result="error"}', 'console_messages_total{level="error"}',
           'audio_stream_events_total{event="started"}']


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:8160"
    with urllib.request.urlopen(f"http://{host}/metrics", timeout=5) as response:
        lines = response.read().decode().splitlines()
    values = {line.rsplit(" ", 1)[0].removeprefix("mxl_browser_source_"): float(line.rsplit(" ", 1)[1]) for line in lines if line and not line.startswith("#")}
    missing = [name for name in ZERO + PRESENT if name not in values]
    assert not missing, f"not on /metrics right after start: {missing}"
    nonzero = {name: values[name] for name in ZERO if values[name] != 0}
    assert not nonzero, f"not 0 right after start: {nonzero}"
    print(f"OK: {len(ZERO)} counters at 0 and {len(PRESENT)} more present right after start")


if __name__ == "__main__":
    main()
