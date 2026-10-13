<script setup>
// A form from an OGraf graphic's JSON schema (GDD, docs/ograf.md §8): basic types, the GDD types
// (multi-line, select, colours, percentage, duration), objects as fieldsets, arrays as lists.
// Anything else falls back to a JSON editor for that property (GDD's graceful degradation).
import { computed, ref, watch } from "vue";
import { defaultsFor } from "../schema.js";

defineOptions({ name: "SchemaForm" });
const props = defineProps({
  schema: { type: Object, required: true },
  modelValue: { default: undefined },
  name: { type: String, default: "" },
  required: { type: Boolean, default: false },
});
const emit = defineEmits(["update:modelValue"]);

const s = computed(() => props.schema || {});
const type = computed(() => (Array.isArray(s.value.type) ? s.value.type[0] : s.value.type));
const gdd = computed(() => s.value.gddType || "");
const label = computed(() => s.value.label || s.value.title || props.name);
const value = computed(() => props.modelValue);
const set = (v) => emit("update:modelValue", v);

const kind = computed(() => {
  const t = type.value;
  if (t === "object") return s.value.properties ? "object" : "json";
  if (t === "array") {
    const items = s.value.items || {};
    if (items.enum) return "multiselect";
    return items.type ? "array" : "json";
  }
  if (t === "boolean") return "boolean";
  if (t === "number" || t === "integer") return "number";
  if (t === "string") {
    if (s.value.enum) return "select";
    if (gdd.value.startsWith("multi-line")) return "textarea";
    if (gdd.value === "color-rrggbb") return "color";
    return "text";
  }
  return "json";
});

const entries = computed(() =>
  Object.entries(s.value.properties || {}).sort(([, a], [, b]) => (a.order ?? 1e9) - (b.order ?? 1e9)),
);
const isRequired = (key) => (s.value.required || []).includes(key);
const objectValue = computed(() => (value.value && typeof value.value === "object" && !Array.isArray(value.value) ? value.value : {}));
const setKey = (key, v) => set({ ...objectValue.value, [key]: v });

const list = computed(() => (Array.isArray(value.value) ? value.value : []));
const setItem = (i, v) => set(list.value.map((x, j) => (j === i ? v : x)));
const addItem = () => set([...list.value, defaultsFor(s.value.items || {})]);
const removeItem = (i) => set(list.value.filter((_, j) => j !== i));
const moveItem = (i, d) => {
  const next = [...list.value];
  const j = i + d;
  if (j < 0 || j >= next.length) return;
  [next[i], next[j]] = [next[j], next[i]];
  set(next);
};

const unit = computed(() => (gdd.value === "percentage" ? "%" : gdd.value === "duration-ms" ? "ms" : ""));
const setNumber = (text) => {
  if (text === "") return set(undefined);
  const n = Number(text);
  set(type.value === "integer" ? Math.round(n) : n);
};

// JSON fallback: edited as text, sent when it parses.
const jsonText = ref("");
const jsonError = ref("");
watch(
  () => props.modelValue,
  (v) => {
    if (kind.value !== "json") return;
    try {
      if (JSON.stringify(JSON.parse(jsonText.value || "null")) === JSON.stringify(v ?? null)) return;
    } catch {
      /* the user is typing */
      return;
    }
    jsonText.value = v === undefined ? "" : JSON.stringify(v, null, 2);
  },
  { immediate: true },
);
function onJson(text) {
  jsonText.value = text;
  try {
    set(text.trim() ? JSON.parse(text) : undefined);
    jsonError.value = "";
  } catch (e) {
    jsonError.value = e.message;
  }
}
const id = `sf-${Math.random().toString(36).slice(2, 9)}`;
</script>

<template>
  <fieldset v-if="kind === 'object'" class="bare schema-object">
    <legend v-if="label">{{ label }}<span v-if="required" class="req">*</span></legend>
    <p v-if="s.description && name" class="note">{{ s.description }}</p>
    <SchemaForm
      v-for="[key, child] in entries"
      :key="key"
      :schema="child"
      :name="key"
      :required="isRequired(key)"
      :model-value="objectValue[key]"
      @update:model-value="(v) => setKey(key, v)"
    />
  </fieldset>

  <div v-else-if="kind === 'array'" class="schema-array">
    <label>{{ label }}<span v-if="required" class="req">*</span></label>
    <div v-for="(item, i) in list" :key="i" class="schema-item">
      <SchemaForm :schema="s.items" :name="`${i + 1}`" :model-value="item" @update:model-value="(v) => setItem(i, v)" />
      <div class="row tight">
        <button class="btn secondary small" :disabled="i === 0" @click="moveItem(i, -1)">↑</button>
        <button class="btn secondary small" :disabled="i === list.length - 1" @click="moveItem(i, 1)">↓</button>
        <button class="btn danger small" @click="removeItem(i)">Remove</button>
      </div>
    </div>
    <button class="btn secondary small" @click="addItem">Add</button>
  </div>

  <div v-else-if="kind === 'boolean'" class="schema-field">
    <label class="inline"><input type="checkbox" :checked="!!value" @change="set($event.target.checked)" /> {{ label }}</label>
  </div>

  <div v-else class="schema-field">
    <label :for="id">{{ label }}<span v-if="required" class="req">*</span><span v-if="unit" class="muted"> ({{ unit }})</span></label>
    <select v-if="kind === 'select'" :id="id" :value="value ?? ''" @change="set($event.target.value)">
      <option v-if="!required" value="">–</option>
      <option v-for="(opt, i) in s.enum" :key="opt" :value="opt">{{ s.gddOptions?.labels?.[opt] ?? s.enumNames?.[i] ?? opt }}</option>
    </select>
    <select v-else-if="kind === 'multiselect'" :id="id" multiple :value="list" @change="set([...$event.target.selectedOptions].map((o) => o.value))">
      <option v-for="opt in s.items.enum" :key="opt" :value="opt">{{ opt }}</option>
    </select>
    <textarea v-else-if="kind === 'textarea'" :id="id" :value="value ?? ''" rows="3" @input="set($event.target.value)"></textarea>
    <div v-else-if="kind === 'color'" class="row tight">
      <input type="color" :value="/^#[0-9a-f]{6}$/i.test(value || '') ? value : '#000000'" @input="set($event.target.value)" />
      <input :id="id" :value="value ?? ''" placeholder="#RRGGBB" @input="set($event.target.value)" />
    </div>
    <input
      v-else-if="kind === 'number'"
      :id="id"
      type="number"
      :min="s.minimum"
      :max="s.maximum"
      :step="type === 'integer' ? 1 : 'any'"
      :value="value ?? ''"
      @input="setNumber($event.target.value)"
    />
    <input v-else-if="kind === 'text'" :id="id" :value="value ?? ''" :maxlength="s.maxLength" @input="set($event.target.value)" />
    <template v-else>
      <textarea :id="id" class="code short" spellcheck="false" :value="jsonText" @input="onJson($event.target.value)"></textarea>
      <div v-if="jsonError" class="msg err">{{ jsonError }}</div>
    </template>
    <p v-if="s.description" class="note">{{ s.description }}</p>
  </div>
</template>
