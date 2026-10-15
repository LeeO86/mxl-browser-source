// Unit tests for web/ograf/renderer.html (the OGraf layer renderer, docs/ograf.md §3), run against a
// minimal fake DOM: each iframe gets a fake layer document whose load() the test controls.
// Run: node --test tests/unit/web/renderer.test.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

const html = readFileSync(new URL("../../../web/ograf/renderer.html", import.meta.url), "utf8");
const script = html.slice(html.indexOf("<script>") + "<script>".length, html.indexOf("</script>"));

// A renderer in its own fake window. `loadImpl(args)` answers each layer document's load().
function renderer(loadImpl) {
  const frames = [];
  const document = {
    body: { appendChild() {} },
    createElement() {
      const frame = {
        style: {},
        onload: null,
        contentWindow: null,
        setAttribute() {},
        replaceWith() {},
        set src(url) {
          if (!url.startsWith("/renderer/layer.html")) return;
          setTimeout(() => {
            frame.contentWindow = {
              ografLayer: {
                element: {},
                load: (args) => loadImpl(args),
                dispose: async () => undefined,
                action: async () => ({ statusCode: 200 }),
              },
            };
            if (frame.onload) frame.onload();
          }, 1);
        },
      };
      frames.push(frame);
      return frame;
    },
  };
  const window = {};
  new Function("window", "document", script)(window, document);
  return window.ografRenderer;
}

const never = () => new Promise(() => {});

test("a second load on a layer rejects the first one at once instead of leaving it pending", async () => {
  let first = true;
  const r = renderer(() => {
    if (first) {
      first = false;
      return never(); // the first graphic's load() never answers
    }
    return Promise.resolve({ statusCode: 200, ok: "second" });
  });
  const one = r.call(1, "load", { data: "a" });
  await new Promise((resolve) => setTimeout(resolve, 20)); // the first document is loaded, load() is pending
  const two = r.call(1, "load", { data: "b" });
  await assert.rejects(one, /loaded again or cleared/);
  assert.deepEqual(await two, { statusCode: 200, ok: "second" });
  // Actions wait for the second load, not the first.
  assert.deepEqual(await r.call(1, "playAction", {}), { statusCode: 200 });
});

test("a load still waiting for its document is rejected by the next load", async () => {
  const r = renderer(() => Promise.resolve(undefined));
  const one = r.call(2, "load", {}); // its document is not loaded yet
  const two = r.call(2, "load", {});
  await assert.rejects(one, /loaded again or cleared/);
  assert.deepEqual(await two, { statusCode: 200 });
});

test("a superseded load that never answers ends at once; other layers are not touched", async () => {
  let calls = 0;
  const r = renderer(() => (++calls === 1 ? never() : Promise.resolve(undefined)));
  const hanging = r.call(3, "load", {});
  await new Promise((resolve) => setTimeout(resolve, 20));
  const other = r.call(4, "load", {});
  const next = r.call(3, "load", {});
  await assert.rejects(hanging, /loaded again or cleared/);
  assert.deepEqual(await next, { statusCode: 200 });
  assert.deepEqual(await other, { statusCode: 200 });
  assert.deepEqual(await r.call(3, "dispose", {}), { statusCode: 200 });
});
