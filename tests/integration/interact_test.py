#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Interaction test (SPEC §17): drives interact.html through /api/v1/interact and checks the
page's reports on /api/v1/events. Standard library only.

    interact_test.py [host:port]
"""
import base64
import json
import os
import socket
import struct
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
    out, buf = [], _partial.pop(sock, b"")
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
    _partial[sock] = buf
    return out


def main():
    host, port = (sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:8160").split(":")
    port = int(port)
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


if __name__ == "__main__":
    main()
