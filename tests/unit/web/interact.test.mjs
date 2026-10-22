// Unit tests for web/src/interact.js (the /api/v1/interact client, SPEC §7): the preview rate it asks
// for follows the control state. Runs against a fake WebSocket; api.js (Vue) is replaced by a stub.
// Run: node --test tests/unit/web/interact.test.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

const source = readFileSync(new URL("../../../web/src/interact.js", import.meta.url), "utf8").replace(
  'import { wsUrl } from "./api.js";',
  "const wsUrl = (path) => `ws://test${path}`;",
);
const { InteractClient } = await import(`data:text/javascript,${encodeURIComponent(source)}`);

class FakeSocket {
  static OPEN = 1;
  static last = null;
  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.sent = [];
    FakeSocket.last = this;
  }
  send(text) {
    this.sent.push(JSON.parse(text));
  }
  close() {
    this.readyState = 3;
    this.onclose?.();
  }
  open() {
    this.readyState = FakeSocket.OPEN;
    this.onopen();
  }
  state(controller, enabled = controller === "self") {
    this.onmessage({ data: JSON.stringify({ type: "state", interact: { enabled, controller } }) });
  }
  previews() {
    return this.sent.filter((m) => m.type === "preview");
  }
}
globalThis.WebSocket = FakeSocket;

function connected() {
  const client = new InteractClient({});
  client.connect();
  const ws = FakeSocket.last;
  ws.open();
  return { client, ws };
}

test("asks for 25 fps while it controls the page and for the default after release", () => {
  const { client, ws } = connected();
  ws.state("none");
  assert.deepEqual(ws.previews(), [], "a watcher keeps the default without asking");
  ws.state("self");
  ws.state("self"); // a repeated state (expires_in_s) does not resend
  assert.deepEqual(ws.previews(), [{ type: "preview", fps: 25 }]);
  ws.state("none"); // toggle off or timeout
  assert.deepEqual(ws.previews(), [{ type: "preview", fps: 25 }, { type: "preview" }]);
  client.close();
});

test("drops back to the default when another session takes over", () => {
  const { client, ws } = connected();
  ws.state("self");
  ws.state("other");
  assert.deepEqual(ws.previews(), [{ type: "preview", fps: 25 }, { type: "preview" }]);
  client.close();
});

test("asks again on a new socket after a reconnect", async () => {
  const { client, ws } = connected();
  ws.state("self");
  ws.readyState = 3;
  ws.onclose(); // the connection drops; the client reconnects after 500 ms
  await new Promise((resolve) => setTimeout(resolve, 600));
  const again = FakeSocket.last;
  assert.notEqual(again, ws);
  again.open();
  again.state("self"); // the new session took control again
  assert.deepEqual(again.previews(), [{ type: "preview", fps: 25 }]);
  client.close();
});
