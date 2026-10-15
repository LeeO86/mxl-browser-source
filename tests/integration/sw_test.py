#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Service workers and the URL policy (SPEC §14.3, §17) against a running container. CEF gives a
page's service worker no browser, so its requests reached none of the URL policy's handlers (1.3.0).
Standard library only.

    sw_test.py host:port off
        A container with BROWSER_SERVICE_WORKERS=false (default): templates.local/service-worker.html,
        a secure context, sees no navigator.serviceWorker, nor does its iframe.

    sw_test.py host:port off-served <host_ip> <server_port>
        A container with BROWSER_SERVICE_WORKERS=false and the settings of `on` below. The page this
        script serves (a secure context by the flag) sees no navigator.serviceWorker; its
        cross-origin iframe (templates.local, its own renderer) posts that it sees none either; a
        shared worker's fetch of an allowed URL is refused and counted (reason service_worker).

    sw_test.py host:port on <host_ip> <server_port>
        A container with BROWSER_SERVICE_WORKERS=true, BROWSER_URL_ALLOW=<host_ip>, no registry and
        --unsafely-treat-insecure-origin-as-secure=http://<host_ip>:<server_port>. This script serves
        the page and its worker on <server_port>. The worker fetches an allowed URL (must arrive),
        loopback and the function's own port, and imports a script from loopback (all refused and
        counted in navigation_blocked_total); a fetch event of the page does the same.
"""
import http.server
import json
import sys
import threading
import time
import urllib.parse

from interact_test import ws_connect, ws_messages
from ops_test import HOST, PORT, call, metric, navigate, status, wait_state

PAGE = """<!doctype html><title>sw-probe</title><body><script>
const post = (o) => { if (window.mxlBrowserSource) window.mxlBrowserSource.post(o); };
(async () => {
  try { await navigator.serviceWorker.register("/sw.js"); } catch (e) { post({sw: "register failed: " + e}); return; }
  const reg = await navigator.serviceWorker.ready;
  navigator.serviceWorker.onmessage = (e) => post({sw: e.data});
  reg.active.postMessage("go");
  setTimeout(async () => post({fetchEvent: await (await fetch("/via-fetch-event")).text()}), 1000);
})();
</script>"""

WORKER = """
try { importScripts("%(loopback)s/imp.js?from=sw"); } catch (e) {}
const targets = {allowed: "%(origin)s/probe?t=allowed&from=sw", loopback: "%(loopback)s/probe?t=loopback&from=sw",
                 own_port: "%(own)s/api/v1/info?from=sw"};
async function tryFetch(u) { try { await fetch(u, {mode: "no-cors"}); return "through"; } catch (e) { return "blocked"; } }
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (e) => e.waitUntil(self.clients.claim()));
self.addEventListener("message", async (e) => {
  const out = {};
  for (const [name, url] of Object.entries(targets)) out[name] = await tryFetch(url);
  e.source.postMessage(out);
});
self.addEventListener("fetch", (e) => {
  if (new URL(e.request.url).pathname === "/via-fetch-event")
    e.respondWith(tryFetch("%(loopback)s/probe?t=loopback&from=sw-fetch-event").then((r) => new Response(r)));
});
"""


OFF_PAGE = """<!doctype html><title>sw-off</title><body><script>
const post = (o) => { if (window.mxlBrowserSource) window.mxlBrowserSource.post(o); };
post({top: {serviceWorker: "serviceWorker" in navigator, secure: window.isSecureContext}});
const frame = document.createElement("iframe");
frame.src = "https://templates.local/service-worker.html";
document.body.appendChild(frame);
const shared = new SharedWorker("/shared.js");
shared.port.onmessage = (e) => post({shared: e.data});
shared.port.start();
</script>"""

SHARED = """onconnect = async (e) => {
  let r; try { await fetch("/probe?t=allowed&from=shared", {mode: "no-cors"}); r = "through"; } catch (err) { r = "blocked"; }
  e.ports[0].postMessage(r);
};"""


def serve(port, worker, page=PAGE):
    hits = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            path = urllib.parse.urlparse(self.path).path
            hits.append(self.path)
            routes = {"/": (page, "text/html"), "/sw.js": (worker, "text/javascript"), "/shared.js": (SHARED, "text/javascript")}
            body, kind = routes.get(path, ("// ok", "text/javascript"))
            data = body.encode()
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return hits


def posts(sock, seconds, until):
    """Page posts until `until(collected)` holds or `seconds` pass."""
    out, end = {}, time.time() + seconds
    while time.time() < end and not until(out):
        for m in ws_messages(sock, 0.5):
            if m.get("type") == "event" and isinstance(m.get("data"), dict):
                out.update(m["data"])
    return out


def off():
    sock = ws_connect(HOST, int(PORT), "/api/v1/events")
    navigate("service-worker.html")
    got = posts(sock, 10, lambda o: "serviceWorkers" in o)
    assert got.get("serviceWorkers") == {"serviceWorker": False, "iframe": False, "secure": True}, got
    print("service workers off: no navigator.serviceWorker in a secure page or its iframe")


def off_served(host_ip, server_port):
    origin = f"http://{host_ip}:{server_port}"
    hits = serve(server_port, "", OFF_PAGE)
    assert wait_state(("loaded",), 30) is not None, "the start page does not load"
    before = metric('navigation_blocked_total{reason="service_worker"}')
    sock = ws_connect(HOST, int(PORT), "/api/v1/events")
    call("POST", "/api/v1/source/navigate", {"url": origin + "/"})
    got = posts(sock, 20, lambda o: {"top", "serviceWorkers", "shared"} <= o.keys())
    assert got.get("top") == {"serviceWorker": False, "secure": True}, got
    # templates.local in an iframe of another site: its own renderer, its own context.
    assert got.get("serviceWorkers") == {"serviceWorker": False, "iframe": False, "secure": True}, got
    assert got.get("shared") == "blocked", got
    assert not [h for h in hits if "from=shared" in h], hits
    after = metric('navigation_blocked_total{reason="service_worker"}')
    assert after >= before + 1, (before, after)
    print("service workers off: none in a secure page or its cross-origin iframe; a shared worker's fetch "
          f"refused and counted (service_worker +{after - before:.0f})")


def on(host_ip, server_port):
    origin = f"http://{host_ip}:{server_port}"
    hits = serve(server_port, WORKER % {"origin": origin, "loopback": f"http://127.0.0.1:{server_port}", "own": f"http://{host_ip}:{PORT}"})
    assert wait_state(("loaded",), 30) is not None, "the start page does not load"
    reasons = ("loopback", "own_port", "service_worker")
    before = {r: metric(f'navigation_blocked_total{{reason="{r}"}}') for r in reasons}
    sock = ws_connect(HOST, int(PORT), "/api/v1/events")
    call("POST", "/api/v1/source/navigate", {"url": origin + "/"})
    got = posts(sock, 20, lambda o: "sw" in o and "fetchEvent" in o)
    assert got.get("sw") == {"allowed": "through", "loopback": "blocked", "own_port": "blocked"}, (got, status()["page"])
    assert got.get("fetchEvent") == "blocked", got
    assert any("t=allowed&from=sw" in h for h in hits), hits  # the worker runs and may fetch what the policy allows
    leaked = [h for h in hits if "t=loopback" in h or h.startswith("/imp.js")]
    assert not leaked, leaked
    after = {r: metric(f'navigation_blocked_total{{reason="{r}"}}') for r in reasons}
    # The worker's import, its fetch and the fetch event's fetch to loopback; its fetch to the own port.
    assert after["loopback"] >= before["loopback"] + 3, (before, after)
    assert after["own_port"] >= before["own_port"] + 1, (before, after)
    assert after["service_worker"] == before["service_worker"], (before, after)
    print(f"service workers on: the worker's allowed fetch arrived; loopback (import, fetch, fetch event) and own port refused "
          f"and counted (loopback +{after['loopback'] - before['loopback']:.0f}, own_port +{after['own_port'] - before['own_port']:.0f})")


if __name__ == "__main__":
    if sys.argv[2] == "off":
        off()
    elif sys.argv[2] == "off-served":
        off_served(sys.argv[3], int(sys.argv[4]))
    else:
        on(sys.argv[3], int(sys.argv[4]))
    print("OK")
