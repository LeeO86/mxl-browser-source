#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Interaction test (SPEC §17): drives interact.html through /api/v1/interact and checks the
page's reports on /api/v1/events: clicks, keys, text, IME, wheel, then text selection (drag, Ctrl+C,
double and triple click), then a client that stops reading (it must not slow the controller's preview)
and the preview rates (watcher, controller, after take-over and release). With a container and its
domain directory it also reads the selection highlight from the newest video grain (`docker exec …
mxl-bs-grain-reader`, `DOCKER='sudo docker'` if needed). Standard library only.

    interact_test.py [host:port] [container domain-dir]
"""
import base64
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import urllib.request


def ws_connect(host, port, path):
    sock = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += sock.recv(1)
    assert b" 101 " in head.split(b"\r\n")[0], head
    return sock


def ws_send(sock, obj):
    payload = json.dumps(obj).encode()
    mask = os.urandom(4)
    header = bytes([0x81])
    if len(payload) < 126:
        header += bytes([0x80 | len(payload)])
    else:
        header += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
    sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


_partial = {}  # socket -> bytes of a frame that had not fully arrived when ws_messages returned


def ws_messages(sock, seconds):
    """Text messages received within `seconds` (binary frames are skipped)."""
    return ws_read(sock, seconds)[0]


def ws_read(sock, seconds):
    """Text messages and the number of binary frames (previews) received within `seconds`."""
    out, frames, buf = [], 0, _partial.pop(sock, b"")
    end = time.time() + seconds
    sock.settimeout(0.2)
    while time.time() < end:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            pass
        while len(buf) >= 2:
            opcode, length, offset = buf[0] & 0x0F, buf[1] & 0x7F, 2
            if length == 126:
                if len(buf) < 4:
                    break
                length, offset = struct.unpack(">H", buf[2:4])[0], 4
            elif length == 127:
                if len(buf) < 10:
                    break
                length, offset = struct.unpack(">Q", buf[2:10])[0], 10
            if len(buf) < offset + length:
                break
            payload, buf = buf[offset:offset + length], buf[offset + length:]
            if opcode == 0x1:
                out.append(json.loads(payload))
            elif opcode == 0x2:
                frames += 1
    _partial[sock] = buf
    return out, frames


def get_json(host, port, path):
    with urllib.request.urlopen(f"http://{host}:{port}{path}", timeout=5) as response:
        return json.loads(response.read())


def grain_pixel(host, port, container, domain, x, y):
    """Y, Cb, Cr (10 bit) of one pixel of the newest video grain."""
    fmt = get_json(host, port, "/api/v1/info")["format"]
    flow = get_json(host, port, "/api/v1/nmos")["senders"]["video"]["flow_id"]
    out = subprocess.run([*os.environ.get("DOCKER", "docker").split(), "exec", container, "mxl-bs-grain-reader", "sample", "--domain", domain,
                          "--flow", flow, "--width", str(fmt["width"]), "--height", str(fmt["height"]), "--x", str(x), "--y", str(y)],
                         capture_output=True, text=True, check=True).stdout
    return {k: int(v) for k, v in re.findall(r"\b(Y|Cb|Cr)=(\d+)", out)}


def selection(host, port, control, events, grains):
    """interact.html's text line (y 640–700): drag over it, copy it, then double and triple click.
    `grains`: (container, domain) to check that the highlight (magenta) reaches the output."""
    text = "Selectable text for the drag test"

    def pointer(action, x, clicks=1):
        ws_send(control, {"type": "pointer", "action": action, "x": x / 1920, "y": 670 / 1080, "button": "left", "clicks": clicks})

    def reported(key):
        return [d[key] for d in (m["data"] for m in ws_messages(events, 1.5) if m.get("type") == "event") if key in d]

    def magenta():
        time.sleep(0.5)  # the video delay
        px = grain_pixel(host, port, *grains, 200, 670)
        return px["Cb"] > 800 and px["Cr"] > 850, px

    ws_send(control, {"type": "preview", "fps": 1})  # this socket is not read here
    if grains:
        painted, px = magenta()
        assert not painted, f"highlight before any selection: {px}"
    # Every move carries the held button, or Chromium ends the drag at the first move.
    pointer("down", 100)
    for x in range(200, 1700, 100):
        ws_send(control, {"type": "pointer", "action": "move", "x": x / 1920, "y": 670 / 1080})
    pointer("up", 1650)
    dragged = reported("selection")
    assert dragged and dragged[-1] == text, f"drag selected {dragged}"
    if grains:
        painted, px = magenta()
        assert painted, f"selection not painted in the output: {px}"
    for action in ("down", "up"):
        ws_send(control, {"type": "key", "action": action, "code": "KeyC", "key": "c", "modifiers": ["ctrl"]})
    copied = reported("copied")
    assert copied and copied[-1] == text, f"Ctrl+C copied {copied}"
    for clicks in (1, 2):
        pointer("down", 150, clicks)
        pointer("up", 150, clicks)
    word = reported("selection")
    assert word and word[-1].strip() == "Selectable", f"double click selected {word}"
    pointer("down", 150, 3)
    pointer("up", 150, 3)
    line = reported("selection")
    assert line and line[-1].strip() == text, f"triple click selected {line}"
    print(f"OK: drag selected and Ctrl+C copied {text!r}{', highlight in the grain' if grains else ''}; double click {word[-1]!r}, triple click the line")


def preview_rates(host, port):
    """SPEC §7.3: a watcher gets BROWSER_PREVIEW_FPS (10), the controller up to 25 when it asks; the
    higher rate ends with control (toggle off, take-over), also when the session does not ask."""
    a = ws_connect(host, port, "/api/v1/interact")
    b = ws_connect(host, port, "/api/v1/interact")

    def fps(sock):
        ws_read(sock, 1)  # frames still queued at the old rate
        return ws_read(sock, 3)[1] / 3

    watcher = fps(a)
    ws_send(a, {"type": "interact", "enable": True, "take": True})
    ws_send(a, {"type": "preview", "fps": 25})
    controller = fps(a)
    ws_send(b, {"type": "interact", "enable": True, "take": True})
    ws_send(b, {"type": "preview", "fps": 25})
    taken_over = fps(a)
    ws_send(b, {"type": "interact", "enable": False})
    ws_send(b, {"type": "preview"})
    released = fps(b)
    rates = f"watcher {watcher:.1f}, controller {controller:.1f}, after take-over {taken_over:.1f}, after release {released:.1f} fps"
    assert 7 <= watcher <= 12, rates
    assert controller >= 15, rates
    assert 7 <= taken_over <= 12, rates
    assert 7 <= released <= 12, rates
    print(f"OK: preview {rates}")
    a.close()
    b.close()


def metric(host, port, name):
    with urllib.request.urlopen(f"http://{host}:{port}/metrics", timeout=5) as response:
        text = response.read().decode()
    return sum(float(line.split()[-1]) for line in text.splitlines() if line.startswith(f"mxl_browser_source_{name}"))


def stuck_client(host, port, observer):
    """A client that stops reading (`observer`, unread since the start) must not slow the controller's
    preview: its frames are dropped, and 5 s after its socket filled up the server closes it (SPEC §7.3)."""
    c = ws_connect(host, port, "/api/v1/interact")
    ws_send(c, {"type": "interact", "enable": True, "take": True})
    ws_send(c, {"type": "preview", "fps": 25})
    ws_read(c, 1)
    frames, seconds, sessions = 0, 0, 2
    while seconds < 60 and sessions > 1:  # until only `c` is left
        frames += ws_read(c, 1)[1]
        seconds += 1
        sessions = get_json(host, port, "/api/v1/status")["interact"]["sessions"]
    rate = frames / seconds
    dropped = metric(host, port, "preview_frames_dropped_total")
    print(f"controller {rate:.1f} fps for {seconds} s next to an unread client; {dropped:.0f} frames dropped for it; sessions {sessions}")
    assert rate >= 15, "the unread client slowed the controller"
    assert dropped > 0, "no preview frame dropped for the unread client"
    assert sessions == 1, "the unread client was not closed"
    print("OK: an unread client does not slow the controller's preview and is closed")
    observer.close()
    c.close()


def main():
    host, port = (sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:8160").split(":")
    port = int(port)
    grains = tuple(sys.argv[2:4]) if len(sys.argv) > 3 else None
    request = urllib.request.Request(f"http://{host}:{port}/api/v1/source/navigate", method="POST",
                                     data=json.dumps({"url": "https://templates.local/interact.html"}).encode(),
                                     headers={"Content-Type": "application/json"})
    urllib.request.urlopen(request, timeout=5).read()
    time.sleep(3)
    events = ws_connect(host, port, "/api/v1/events")
    control = ws_connect(host, port, "/api/v1/interact")
    observer = ws_connect(host, port, "/api/v1/interact")
    ws_send(control, {"type": "hello", "client": "test", "protocol": 1})

    # An observer cannot inject input.
    ws_send(observer, {"type": "pointer", "action": "down", "x": 0.15, "y": 0.18, "button": "left", "clicks": 1})
    rejected = [m for m in ws_messages(observer, 1) if m.get("type") == "error"]
    assert rejected and rejected[0]["code"] == "not_controller", rejected

    ws_send(control, {"type": "interact", "enable": True})
    ws_send(control, {"type": "focus", "focused": True})
    sent = time.time()
    # Click the button (centre 300,200 of 1920×1080).
    for action in ("down", "up"):
        ws_send(control, {"type": "pointer", "action": action, "x": 300 / 1920, "y": 200 / 1080, "button": "left", "clicks": 1, "seq": 1})
    # Click the field, type ASCII and non-ASCII, scroll.
    for action in ("down", "up"):
        ws_send(control, {"type": "pointer", "action": action, "x": 500 / 1920, "y": 440 / 1080, "button": "left", "clicks": 1})
    for ch, code in (("a", "KeyA"), ("b", "KeyB")):
        ws_send(control, {"type": "key", "action": "down", "code": code, "key": ch})
        ws_send(control, {"type": "key", "action": "up", "code": code, "key": ch})
    ws_send(control, {"type": "text", "text": "ü你"})
    # IME: a composition that is committed, then one that is cancelled.
    ws_send(control, {"type": "ime", "action": "composition", "text": "にほ", "selection": [2, 2]})
    ws_send(control, {"type": "ime", "action": "commit", "text": "日本"})
    ws_send(control, {"type": "ime", "action": "composition", "text": "テ"})
    ws_send(control, {"type": "ime", "action": "cancel"})
    ws_send(control, {"type": "wheel", "x": 0.5, "y": 0.5, "dx": 0, "dy": 120})

    reports = [m["data"] for m in ws_messages(events, 3) if m.get("type") == "event"]
    acks = [m for m in ws_messages(control, 0.5) if m.get("type") == "ack"]
    print("reports", reports)
    clicked = [r for r in reports if "clicked" in r]
    texts = [r["text"] for r in reports if "text" in r]
    wheels = [r for r in reports if "wheel" in r]
    assert clicked, "button click not reported"
    assert texts and texts[-1] == "abü你日本", texts
    assert wheels and wheels[-1]["wheel"] == 1, f"wheel down not reported: {wheels}"
    print(f"OK: click, keys, text (ab + ü你), IME (にほ → 日本 committed, テ cancelled), wheel; {len(acks)} acks; within {time.time() - sent:.1f} s")
    selection(host, port, control, events, grains)
    control.close()
    stuck_client(host, port, observer)
    preview_rates(host, port)


if __name__ == "__main__":
    main()
