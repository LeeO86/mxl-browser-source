// Helpers for OGraf graphic schemas (GDD, docs/ograf.md §8).

/** The schema's defaults as a value: `default` where given, objects built from their properties. */
export function defaultsFor(schema) {
  if (!schema || typeof schema !== "object") return undefined;
  if (schema.default !== undefined) return structuredClone(schema.default);
  const type = Array.isArray(schema.type) ? schema.type[0] : schema.type;
  if (type === "object" && schema.properties) {
    const out = {};
    for (const [key, child] of Object.entries(schema.properties)) {
      const v = defaultsFor(child);
      if (v !== undefined) out[key] = v;
    }
    return out;
  }
  if (type === "array") return [];
  return undefined;
}

/** Required fields that are empty, as "a.b" paths (the form keeps Load and Update off then). */
export function missingRequired(schema, value, path = "") {
  const out = [];
  const type = Array.isArray(schema?.type) ? schema.type[0] : schema?.type;
  if (type !== "object" || !schema.properties) return out;
  const v = value && typeof value === "object" ? value : {};
  for (const key of schema.required || []) {
    if (v[key] === undefined || v[key] === "" || v[key] === null) out.push(path + key);
  }
  for (const [key, child] of Object.entries(schema.properties)) {
    if (v[key] !== undefined) out.push(...missingRequired(child, v[key], `${path}${key}.`));
  }
  return out;
}

/** A short label for a loaded graphic: its first non-hidden string fields. */
export function dataLabel(schema, data) {
  if (!data || typeof data !== "object") return "";
  const props = schema?.properties || {};
  return Object.entries(data)
    .filter(([key, v]) => typeof v === "string" && v && !props[key]?.hidden)
    .slice(0, 2)
    .map(([, v]) => v)
    .join(" · ");
}
