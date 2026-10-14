// Helpers for OGraf graphic schemas (GDD, docs/ograf.md §8). Plain functions without Vue, so
// tests/unit/web/schema.test.mjs can run them with `node --test`.

const typeOf = (schema) => (Array.isArray(schema?.type) ? schema.type[0] : schema?.type);

/** The schema's defaults as a value: `default` where given, objects built from their properties. */
export function defaultsFor(schema) {
  if (!schema || typeof schema !== "object") return undefined;
  if (schema.default !== undefined) return structuredClone(schema.default);
  const type = typeOf(schema);
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

/**
 * The editor a property gets: object, array, multiselect, boolean, select, number, percentage,
 * textarea, color, color-alpha, text, or json (the fallback for anything else).
 */
export function fieldKind(schema) {
  const s = schema || {};
  const t = typeOf(s);
  const gdd = typeof s.gddType === "string" ? s.gddType : "";
  if (t === "object") return s.properties ? "object" : "json";
  if (t === "array") {
    const items = s.items || {};
    if (Array.isArray(items.enum)) return "multiselect";
    return items.type ? "array" : "json";
  }
  if (t === "boolean") return "boolean";
  // An enum is a select whatever its type (GDD `select` uses integers, e.g. the EBU weather graphic).
  if (Array.isArray(s.enum) && (t === "string" || t === "number" || t === "integer")) return "select";
  if (t === "number" || t === "integer") return gdd === "percentage" ? "percentage" : "number";
  if (t === "string") {
    if (gdd.startsWith("multi-line")) return "textarea";
    if (gdd === "color-rrggbb") return "color";
    if (gdd === "color-rrggbbaa") return "color-alpha";
    return "text";
  }
  return "json";
}

/** The label of an enum option: GDD `gddOptions.labels`, then `enumNames`, then the value. */
export function optionLabel(schema, value, index) {
  const labels = schema?.gddOptions?.labels;
  if (labels && typeof labels === "object" && labels[String(value)] !== undefined) return String(labels[String(value)]);
  if (Array.isArray(schema?.enumNames) && schema.enumNames[index] !== undefined) return String(schema.enumNames[index]);
  return String(value);
}

const isEmpty = (v) => v === undefined || v === null || v === "";

/**
 * Required fields that are empty, as "a.b" / "list[0].c" paths (the form keeps Load, Update and the
 * custom action off then). A required boolean is never missing: unchecked is a valid false.
 */
export function missingRequired(schema, value, path = "") {
  const out = [];
  const type = typeOf(schema);
  if (type === "array" && schema.items && Array.isArray(value)) {
    value.forEach((item, i) => out.push(...missingRequired(schema.items, item, `${path.replace(/\.$/, "")}[${i}].`)));
    return out;
  }
  if (type !== "object" || !schema.properties) return out;
  const v = value && typeof value === "object" && !Array.isArray(value) ? value : {};
  for (const key of schema.required || []) {
    if (typeOf(schema.properties[key]) === "boolean") continue;
    if (isEmpty(v[key])) out.push(path + key);
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
    .filter(([key, v]) => typeof v === "string" && v.trim() && !props[key]?.hidden)
    .slice(0, 2)
    .map(([, v]) => v.trim())
    .join(" · ");
}
