// Unit tests for web/src/schema.js (the OGraf schema form helpers, docs/ograf.md §8).
// Run: node --test tests/unit/web/schema.test.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { dataLabel, defaultsFor, fieldKind, missingRequired, optionLabel } from "../../../web/src/schema.js";

const form = JSON.parse(readFileSync(new URL("../../integration/ograf/form/form.ograf.json", import.meta.url)));
const schema = form.schema;
const p = schema.properties;

test("defaults come from `default`, objects are built from their properties", () => {
  const d = defaultsFor(schema);
  assert.equal(d.headline, undefined);
  assert.equal(d.level, 2);
  assert.deepEqual(d.tags, ["b"]);
  assert.equal(d.secret, "kept");
  assert.deepEqual(d.person, { name: "Anna" });
  assert.deepEqual(d.rows, [{ label: "one", value: 1 }]);
  assert.equal(defaultsFor(null), undefined);
  // a copy, not the manifest's own value
  d.tags.push("c");
  assert.deepEqual(p.tags.default, ["b"]);
});

test("every GDD type gets its editor, unknown types fall back to JSON", () => {
  const kinds = Object.fromEntries(Object.entries(p).map(([k, s]) => [k, fieldKind(s)]));
  assert.deepEqual(kinds, {
    headline: "text", body: "textarea", kind: "select", level: "select", tags: "multiselect", color: "color",
    shade: "color-alpha", opacity: "percentage", hold: "number", logo: "text", live: "boolean", secret: "text",
    person: "object", rows: "array", extra: "json",
  });
  assert.equal(fieldKind({ type: "null" }), "json");
  assert.equal(fieldKind({ type: ["integer", "null"] }), "number");
  assert.equal(fieldKind({ type: "array" }), "json");
});

test("option labels: gddOptions.labels, then enumNames, then the value", () => {
  assert.equal(optionLabel(p.level, 3, 2), "High");
  assert.equal(optionLabel(p.kind, "sport", 1), "Sport");
  assert.equal(optionLabel({ enum: ["x"], enumNames: ["Ex"] }, "x", 0), "Ex");
  assert.equal(optionLabel({ enum: [7] }, 7, 0), "7");
});

test("required fields: empty strings and missing keys count, nested objects and list items too", () => {
  assert.deepEqual(missingRequired(schema, defaultsFor(schema)), ["headline", "kind"]);
  const ok = { ...defaultsFor(schema), headline: "H", kind: "news" };
  assert.deepEqual(missingRequired(schema, ok), []);
  assert.deepEqual(missingRequired(schema, { ...ok, headline: "" }), ["headline"]);
  assert.deepEqual(missingRequired(schema, { ...ok, person: { role: "x" } }), ["person.name"]);
  assert.deepEqual(missingRequired(schema, { ...ok, rows: [{ label: "a" }, { value: 2 }] }), ["rows[1].label"]);
  // a required boolean is never missing: unchecked is false
  assert.deepEqual(missingRequired({ type: "object", required: ["b"], properties: { b: { type: "boolean" } } }, {}), []);
  // custom action payloads
  assert.deepEqual(missingRequired(form.customActions[1].schema, defaultsFor(form.customActions[1].schema)), ["text"]);
});

test("the layer label shows the first visible strings", () => {
  assert.equal(dataLabel(schema, { secret: "s", headline: "  Hi ", body: "B", kind: "news" }), "Hi · B");
  assert.equal(dataLabel(schema, null), "");
});
