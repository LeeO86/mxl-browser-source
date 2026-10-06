#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dialogs, popups, downloads and permissions (SPEC §4.5, §17) against a running container: each
page is clicked through /api/v1/interact (a real user gesture), the reactions are checked on
/api/v1/events and /metrics, and the output must not stall (grains keep coming, none missed).
Standard library only.

    pages_test.py [host:port]
"""
import time

from interact_test import ws_connect, ws_messages, ws_send
from ops_test import HOST, PORT, metric, navigate, status


def page_events(sock, seconds):
    """Page posts and app events (`data` of type event) and dialog events."""
    out = []
    for m in ws_messages(sock, seconds):
        if m.get("type") == "event":
            out.append(m["data"])
        elif m.get("type") == "dialog":
            out.append({"dialog": m["kind"], "result": m["result"]})
    return out


def click(control):
    for action in ("down", "up"):
        ws_send(control, {"type": "pointer", "action": action, "x": 0.5, "y": 0.5, "button": "left", "clicks": 1})


class Output:
    """Grains written and missed while a page runs."""

    def __enter__(self):
        self.start, self.before = time.time(), status()["grains"]
        return self

    def __exit__(self, failed, *exc):
        if failed:
            return False
        after, seconds = status()["grains"], time.time() - self.start
        self.rate = (after["video"] - self.before["video"]) / seconds
        assert after["missed"] == self.before["missed"], (self.before, after)
        assert self.rate > 45, f"output stalled: {self.rate:.1f} grains/s"


def dialogs(events, control):
    counts = {k: metric(f'js_dialogs_total{{type="{k}"}}') for k in ("alert", "confirm", "prompt", "beforeunload")}
    with Output() as out:
        navigate("dialogs.html")
        seen = page_events(events, 2)
        assert {"dialog": "alert", "result": "dismissed"} in seen, seen
        assert {"dialog": "confirm", "result": "cancelled"} in seen, seen
        assert {"dialog": "prompt", "result": "cancelled"} in seen, seen
        assert {"dialogs": {"confirm": False, "prompt": None}} in seen, seen
        click(control)
        seen = page_events(events, 2)
        assert {"file_chooser": "opened"} in seen and {"file_dialog": "cancelled"} in seen, seen
        navigate("blank.html")  # after the click the page asks before unload
        seen = page_events(events, 1)
        assert {"dialog": "beforeunload", "result": "accepted"} in seen, seen
    for k in counts:
        assert metric(f'js_dialogs_total{{type="{k}"}}') == counts[k] + 1, k
    print(f"dialogs: alert dismissed, confirm and prompt cancelled, file chooser cancelled, beforeunload accepted; {out.rate:.1f} grains/s, missed +0")


def popup(events, control):
    blocked = metric("popups_blocked_total")
    with Output() as out:
        navigate("popup.html")
        click(control)
        seen = page_events(events, 2)
        popups = [e["popup"] for e in seen if "popup" in e and "opened" in e]
        assert all(e["opened"] is False for e in seen if "opened" in e) and len(popups) == 2, seen
        assert status()["page"]["url"] == "https://templates.local/popup.html"
    assert metric("popups_blocked_total") == blocked + 2
    print(f"popup: window.open and target=_blank blocked ({', '.join(popups)}), page stays; {out.rate:.1f} grains/s, missed +0")


def download(events, control):
    blocked = metric("downloads_blocked_total")
    with Output() as out:
        navigate("download.html")
        click(control)
        seen = page_events(events, 2)
        assert any("download_blocked" in e for e in seen), seen
        assert status()["page"]["url"] == "https://templates.local/download.html"
    assert metric("downloads_blocked_total") == blocked + 1
    print(f"download: denied and counted, page stays; {out.rate:.1f} grains/s, missed +0")


def permissions(events, control):
    kinds = ("camera", "microphone", "geolocation", "notifications", "clipboard", "midi", "screen", "other")
    before = {k: metric(f'permission_denied_total{{type="{k}"}}') for k in kinds}
    with Output() as out:
        navigate("permissions.html")
        click(control)
        seen = page_events(events, 7)  # geolocation times out after 5 s at most
        results = {e["permission"]: (e["result"], e["detail"]) for e in seen if "permission" in e}
        assert set(results) == {"media", "geolocation", "notifications", "clipboard", "midi"}, results
        assert all(r[0] == "denied" for r in results.values()), results
    counted = {k: int(metric(f'permission_denied_total{{type="{k}"}}') - before[k]) for k in kinds}
    counted = {k: v for k, v in counted.items() if v}
    print(f"permissions: all denied {results}; counted {counted}; {out.rate:.1f} grains/s, missed +0")


if __name__ == "__main__":
    events = ws_connect(HOST, int(PORT), "/api/v1/events")
    control = ws_connect(HOST, int(PORT), "/api/v1/interact")
    ws_send(control, {"type": "hello", "client": "pages_test", "protocol": 1})
    ws_send(control, {"type": "interact", "enable": True})
    ws_send(control, {"type": "focus", "focused": True})
    dialogs(events, control)
    popup(events, control)
    download(events, control)
    permissions(events, control)
    print("OK")
