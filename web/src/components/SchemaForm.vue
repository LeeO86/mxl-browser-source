<script setup>
// A form from an OGraf graphic's JSON schema (GDD, docs/ograf.md §8): basic types, the GDD types
// (multi-line, select and select-multiple, colours, percentage, duration), objects as fieldsets,
// arrays as lists. `hidden` properties are not shown (their value is kept), `order` sorts.
// Anything else falls back to a JSON editor for that property (GDD's graceful degradation).
import { computed, ref, watch } from "vue";
import { defaultsFor, fieldKind, optionLabel } from "../schema.js";

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
const gdd = computed(() => (typeof s.value.gddType === "string" ? s.value.gddType : ""));
const label = computed(() => s.value.label || s.value.title || props.name);
const value = computed(() => props.modelValue);
const set = (v) => emit("update:modelValue", v);
const kind = computed(() => fieldKind(s.value));
const empty = computed(() => value.value === undefined || value.value === null || value.value === "");

const entries = computed(() =>
  Object.entries(s.value.properties || {})
    .filter(([, child]) => !child?.hidden)
    .sort(([, a], [, b]) => (a.order ?? 1e9) - (b.order ?? 1e9)),
);
const isRequired = (key) => (s.value.required || []).includes(key);
const objectValue = computed(() => (value.value && typeof value.value === "object" && !Array.isArray(value.value) ? value.value : {}));
const setKey = (key, v) => {
  const next = { ...objectValue.value };
  if (v === undefined) delete next[key];
  else next[key] = v;
  set(next);
};

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

// Selects keep the option's index as the DOM value, so integer and number enums stay numbers.
const options = computed(() => (kind.value === "multiselect" ? s.value.items.enum : s.value.enum) || []);
const optionText = (opt, i) => optionLabel(kind.value === "multiselect" ? { ...s.value.items, gddOptions: s.value.gddOptions ?? s.value.items.gddOptions } : s.value, opt, i);
const selectedIndex = computed(() => options.value.findIndex((o) => o === value.value));
const onSelect = (text) => set(text === "" ? undefined : options.value[Number(text)]);
const onMultiSelect = (el) => set([...el.selectedOptions].map((o) => options.value[Number(o.value)]));

const unit = computed(() => (gdd.value === "duration-ms" ? "ms" : ""));
const setNumber = (text) => {
  if (text === "") return set(undefined);
  const n = Number(text);
  if (!Number.isFinite(n)) return;
  set(type.value === "integer" ? Math.round(n) : n);
};
const percentText = computed(() => (typeof value.value === "number" ? `${+(value.value * 100).toFixed(2)} %` : ""));

// Colours: the picker edits #rrggbb; with RRGGBBAA the alpha pair from the text field is kept.
const colorRgb = computed(() => (/^#[0-9a-f]{6}/i.test(value.value || "") ? value.value.slice(0, 7).toLowerCase() : "#000000"));
const onPicker = (rgb) => {
  if (kind.value === "color-alpha") {
    const alpha = /^#[0-9a-f]{8}$/i.test(value.value || "") ? value.value.slice(7, 9) : "ff";
    set(rgb.toLowerCase() + alpha.toLowerCase());
  } else set(rgb.toLowerCase());
};
const colorPattern = computed(() => (kind.value === "color-alpha" ? /^#[0-9a-f]{8}$/ : /^#[0-9a-f]{6}$/));
const colorBad = computed(() => !empty.value && !colorPattern.value.test(value.value));

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
  <fieldset v-if="kind === 'object'" class="bare schema-object" :class="{ nested: !!name }">
    <legend v-if="label">{{ label }}<span v-if="required" class="req" title="required">*</span></legend>
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
    <label>{{ label }}<span v-if="required" class="req" title="required">*</span></label>
    <div v-for="(item, i) in list" :key="i" class="schema-item">
      <SchemaForm :schema="s.items" :name="`${i + 1}`" :model-value="item" @update:model-value="(v) => setItem(i, v)" />
      <div class="row tight">
        <button class="btn secondary small" type="button" :disabled="i === 0" @click="moveItem(i, -1)">↑</button>
        <button class="btn secondary small" type="button" :disabled="i === list.length - 1" @click="moveItem(i, 1)">↓</button>
        <button class="btn danger small" type="button" @click="removeItem(i)">Remove</button>
      </div>
    </div>
    <button class="btn secondary small" type="button" @click="addItem">Add</button>
  </div>

  <div v-else-if="kind === 'boolean'" class="schema-field">
    <label class="inline"><input type="checkbox" :checked="!!value" @change="set($event.target.checked)" /> {{ label }}</label>
    <p v-if="s.description" class="note">{{ s.description }}</p>
  </div>

  <div v-else class="schema-field" :class="{ missing: required && empty }">
    <label :for="id">{{ label }}<span v-if="required" class="req" title="required">*</span><span v-if="unit" class="muted"> ({{ unit }})</span></label>
    <select v-if="kind === 'select'" :id="id" :value="selectedIndex < 0 ? '' : String(selectedIndex)" @change="onSelect($event.target.value)">
      <option v-if="!required || selectedIndex < 0" value="" :disabled="required">–</option>
      <option v-for="(opt, i) in options" :key="i" :value="String(i)">{{ optionText(opt, i) }}</option>
    </select>
    <select v-else-if="kind === 'multiselect'" :id="id" multiple @change="onMultiSelect($event.target)">
      <option v-for="(opt, i) in options" :key="i" :value="String(i)" :selected="list.includes(opt)">{{ optionText(opt, i) }}</option>
    </select>
    <textarea v-else-if="kind === 'textarea'" :id="id" :value="value ?? ''" rows="3" :maxlength="s.maxLength" @input="set($event.target.value)"></textarea>
    <div v-else-if="kind === 'color' || kind === 'color-alpha'" class="row tight">
      <input type="color" :value="colorRgb" :aria-label="`${label} picker`" @input="onPicker($event.target.value)" />
      <input :id="id" class="colortext" :value="value ?? ''" :placeholder="kind === 'color-alpha' ? '#rrggbbaa' : '#rrggbb'" @input="set($event.target.value.trim().toLowerCase() || undefined)" />
      <span v-if="colorBad" class="msg err inline-msg">{{ kind === "color-alpha" ? "#rrggbbaa" : "#rrggbb" }}</span>
    </div>
    <div v-else-if="kind === 'percentage'" class="row tight">
      <input :id="id" type="number" step="0.01" :min="s.minimum" :max="s.maximum" :value="value ?? ''" @input="setNumber($event.target.value)" />
      <span class="muted small">{{ percentText }} (1 = 100 %)</span>
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
    <input v-else-if="kind === 'text'" :id="id" :value="value ?? ''" :maxlength="s.maxLength" :placeholder="gdd.startsWith('file-path') ? 'path relative to the graphic' : ''" @input="set($event.target.value)" />
    <template v-else>
      <textarea :id="id" class="code short" spellcheck="false" :value="jsonText" @input="onJson($event.target.value)"></textarea>
      <div v-if="jsonError" class="msg err">{{ jsonError }}</div>
    </template>
    <p v-if="s.description" class="note">{{ s.description }}</p>
  </div>
</template>
