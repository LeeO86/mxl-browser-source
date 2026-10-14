<script setup>
// OGraf graphics (docs/ograf.md §8): the library (upload, list, delete), the 8 layers with their
// on-air state, and a form from the selected graphic's schema. Everything goes through the OGraf
// Server API at /ograf/v1 and our /api/v1/ograf routes. The `ograf` widget is this component with
// `widget` set (no upload or delete) and an optional layer filter.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { act, api, live } from "../api.js";
import { dataLabel, defaultsFor, missingRequired } from "../schema.js";
import SchemaForm from "./SchemaForm.vue";

const props = defineProps({
  widget: { type: Boolean, default: false },
  layers: { type: Array, default: () => [] }, // layer numbers to show; empty = all 8
});

const TARGET = "/ograf/v1/renderers/main/target/graphicInstance";
const msg = ref({ kind: "", text: "" });
const graphics = ref([]);
const manifests = ref({}); // id → manifest
const source = ref(null);
const selectedLayer = ref(props.layers[0] || 1);
const selectedGraphic = ref("");
const formData = ref({});
const skipAnimation = ref(false);
const actionPayloads = ref({});
const upload = ref({ busy: false, report: null });
const confirmMode = ref(false);

const allLayers = computed(() => {
  const list = live.ograf?.length ? live.ograf : Array.from({ length: 8 }, (_, i) => ({ layer: i + 1, state: "empty" }));
  return [...list].filter((l) => !props.layers.length || props.layers.includes(l.layer)).sort((a, b) => b.layer - a.layer);
});
const layer = computed(() => allLayers.value.find((l) => l.layer === selectedLayer.value) || { layer: selectedLayer.value, state: "empty" });
const loaded = computed(() => layer.value.state !== "empty");
const graphicId = computed(() => (loaded.value ? layer.value.graphicId : selectedGraphic.value));
const manifest = computed(() => manifests.value[graphicId.value] || null);
const schema = computed(() => manifest.value?.schema || null);
const missing = computed(() => (schema.value ? missingRequired(schema.value, formData.value) : []));
const stepCount = computed(() => manifest.value?.stepCount ?? 1);
const steps = computed(() => stepCount.value > 1 || stepCount.value === -1);
const modeOgraf = computed(() => source.value?.mode === "ograf");

const STATE = { empty: "init", loading: "init", ready: "degraded", on: "onair", stopped: "init", error: "off" };
const stateLabel = (l) => (l.state === "on" ? "ON AIR" : l.state);

async function loadManifest(id) {
  if (!id || manifests.value[id]) return;
  try {
    const res = await api.get(`/ograf/v1/graphics/${encodeURIComponent(id)}`);
    manifests.value = { ...manifests.value, [id]: res.graphic };
  } catch {
    /* unlisted (replaced or deleted): the layer still runs it */
  }
}

async function refresh() {
  const res = await act(msg, () => api.get("/ograf/v1/graphics"));
  graphics.value = res?.graphics || [];
  if (!selectedGraphic.value && graphics.value.length) selectedGraphic.value = graphics.value[0].id;
  source.value = await act(msg, () => api.get("/api/v1/source"));
  if (!live.ograf?.length) live.ograf = (await act(msg, () => api.get("/api/v1/ograf/layers")))?.layers || [];
}

// The form shows the loaded graphic's data, or the selected graphic's defaults for a Load.
watch(
  [() => layer.value.graphicInstanceId, graphicId],
  async () => {
    await loadManifest(graphicId.value);
    formData.value = loaded.value ? structuredClone(layer.value.data ?? {}) : (defaultsFor(schema.value) ?? {});
    actionPayloads.value = Object.fromEntries((manifest.value?.customActions || []).map((a) => [a.id, defaultsFor(a.schema) ?? {}]));
  },
  { immediate: true },
);

const target = (n) => ({ layer: n });

async function load() {
  const res = await act(msg, () =>
    api.post(`${TARGET}/load`, { renderTarget: target(selectedLayer.value), graphicId: selectedGraphic.value, params: { data: formData.value } }),
  );
  if (res) msg.value = { kind: res.statusCode < 400 ? "ok" : "err", text: `Loaded on layer ${selectedLayer.value} (load() answered ${res.statusCode}).` };
}
async function action(verb, params = {}, n = selectedLayer.value) {
  const l = allLayers.value.find((x) => x.layer === n);
  if (!l?.graphicInstanceId) return;
  const res = await act(msg, () =>
    api.post(`${TARGET}/${verb}`, { renderTarget: target(n), graphicInstanceId: l.graphicInstanceId, params: { skipAnimation: skipAnimation.value, ...params } }),
  );
  if (res) msg.value = { kind: res.statusCode < 400 ? "ok" : "err", text: `${verb} on layer ${n}: ${res.statusCode}${res.statusMessage ? " " + res.statusMessage : ""}` };
}
const play = (n) => action("playAction", {}, n);
const next = (n) => action("playAction", { delta: 1 }, n);
const stop = (n) => action("stopAction", {}, n);
const update = () => action("updateAction", { data: formData.value });
const custom = (id) => action(`customActions/${encodeURIComponent(id)}`, { payload: actionPayloads.value[id] ?? {} });
const actionMissing = (a) => (a.schema ? missingRequired(a.schema, actionPayloads.value[a.id]) : []);
const clear = (n) =>
  act(msg, () => api.put(`${TARGET}/clear`, { filters: [{ renderTarget: target(n) }] }), `Layer ${n} cleared.`);

async function onFile(ev) {
  const file = ev.target.files?.[0];
  ev.target.value = "";
  await uploadFile(file);
}
const dragging = ref(false);
async function onDrop(ev) {
  dragging.value = false;
  if (props.widget) return;
  await uploadFile(ev.dataTransfer?.files?.[0]);
}
async function uploadFile(file) {
  if (!file || upload.value.busy) return;
  upload.value = { busy: true, report: null };
  try {
    const res = await api.upload("/api/v1/ograf/graphics", file);
    upload.value = { busy: false, report: { ok: true, graphics: res.graphics } };
    manifests.value = {};
    await refresh();
  } catch (e) {
    upload.value = { busy: false, report: { ok: false, error: e.message, manifests: e.data?.manifests || [] } };
  }
}
async function remove(g) {
  const used = (g.v_inUse || []).map((u) => u.layer);
  const force = used.length > 0;
  await act(msg, () => api.del(`/ograf/v1/graphics/${encodeURIComponent(g.id)}${force ? "?force=true" : ""}`), `Deleted ${g.name}.`);
  await refresh();
}
const confirmDelete = ref("");

async function setMode(mode, force = false) {
  const res = await act(msg, () => api.patch(`/api/v1/source${force ? "?force=true" : ""}`, { mode }), `Mode ${mode}.`);
  if (res) source.value = res;
  confirmMode.value = false;
}

const thumb = (g) => {
  const t = g.thumbnails?.[0]?.file;
  return t ? `/ograf/v1/graphics/${encodeURIComponent(g.id)}/thumbnail?file=${encodeURIComponent(t)}` : "";
};
const inUseText = (g) =>
  (g.v_inUse || []).map((u) => (u.current ? `on layer ${u.layer}` : `replaced, in use on layer ${u.layer}`)).join(", ");

// The output picture above the layers: /api/v1/preview.jpg once a second while this is shown.
const previewUrl = ref("");
let previewTimer = 0;
let previewAlive = true;
async function pollPreview() {
  try {
    const url = URL.createObjectURL(await api.blob("/api/v1/preview.jpg"));
    if (previewUrl.value) URL.revokeObjectURL(previewUrl.value);
    previewUrl.value = url;
  } catch {
    /* no frame yet (503) */
  }
  if (previewAlive) previewTimer = setTimeout(pollPreview, 1000);
}

onMounted(() => {
  refresh();
  pollPreview();
});
onUnmounted(() => {
  previewAlive = false;
  clearTimeout(previewTimer);
  if (previewUrl.value) URL.revokeObjectURL(previewUrl.value);
});
</script>

<template>
  <div v-if="source && !modeOgraf" class="warnbox">
    The source shows a web page (mode page). OGraf graphics need the renderer.
    <button class="btn small" @click="setMode('ograf')">Switch to OGraf</button>
  </div>

  <div class="grid two">
    <div class="panel">
      <div class="og-preview">
        <img v-if="previewUrl" :src="previewUrl" alt="Output preview" />
        <span v-else class="muted small">No preview yet</span>
      </div>
      <h3>
        Layers <span class="muted small">(8 is on top)</span>
        <label class="inline right-btn plain"><input v-model="skipAnimation" type="checkbox" /> skip animation</label>
      </h3>
      <table class="layers">
        <tbody>
          <tr v-for="l in allLayers" :key="l.layer" :class="{ selected: l.layer === selectedLayer }" @click="selectedLayer = l.layer">
            <td class="num">{{ l.layer }}</td>
            <td><span class="pill" :class="STATE[l.state]">{{ stateLabel(l) }}</span></td>
            <td class="ellipsis">
              <template v-if="l.state !== 'empty'">
                {{ l.graphicName }}
                <span class="muted small">{{ dataLabel(manifests[l.graphicId]?.schema, l.data) }}</span>
                <span v-if="l.step !== null && l.step !== undefined" class="muted small"> · step {{ l.step + 1 }}<template v-if="l.stepCount > 0"> of {{ l.stepCount }}</template></span>
                <span v-if="l.pending" class="muted small"> · …</span>
                <div v-if="l.error" class="msg err inline-msg">{{ l.error }}</div>
              </template>
            </td>
            <td class="nowrap right" @click.stop>
              <template v-if="l.state !== 'empty'">
                <button class="btn small" :disabled="!modeOgraf" @click="play(l.layer)">Play</button>
                <button v-if="l.stepCount > 1 || l.stepCount === -1" class="btn secondary small" :disabled="!modeOgraf" @click="next(l.layer)">Next</button>
                <button class="btn secondary small" :disabled="!modeOgraf" @click="stop(l.layer)">Stop</button>
                <button class="btn danger small" @click="clear(l.layer)">Clear</button>
              </template>
            </td>
          </tr>
        </tbody>
      </table>
      <div v-if="msg.text" class="msg" :class="msg.kind">{{ msg.text }}</div>
      <div v-if="!widget && modeOgraf" class="actions">
        <template v-if="confirmMode">
          <span class="small">Leaving OGraf mode clears every layer.</span>
          <button class="btn danger small" @click="setMode('page', true)">Clear and show the page</button>
          <button class="btn secondary small" @click="confirmMode = false">Cancel</button>
        </template>
        <button v-else class="btn secondary small" @click="allLayers.some((l) => l.state !== 'empty') ? (confirmMode = true) : setMode('page')">Back to the web page</button>
      </div>
    </div>

    <div class="panel">
      <h3>Layer {{ selectedLayer }} <span v-if="manifest" class="muted small">· {{ manifest.name }}</span></h3>
      <template v-if="!loaded">
        <label for="og-graphic">Graphic</label>
        <select id="og-graphic" v-model="selectedGraphic">
          <option v-for="g in graphics" :key="g.id" :value="g.id">{{ g.name }} ({{ g.id }})</option>
        </select>
      </template>
      <SchemaForm v-if="schema" v-model="formData" :schema="schema" />
      <p v-else-if="manifest" class="note">This graphic has no data.</p>
      <div v-if="missing.length" class="msg err">Required: {{ missing.join(", ") }}</div>
      <div class="actions">
        <button v-if="!loaded" class="btn" :disabled="!selectedGraphic || !modeOgraf || missing.length > 0" @click="load">Load</button>
        <template v-else>
          <button class="btn secondary" :disabled="!modeOgraf || missing.length > 0" @click="update">Update</button>
          <button class="btn" :disabled="!modeOgraf" @click="play(selectedLayer)">Play</button>
          <button v-if="steps" class="btn secondary" :disabled="!modeOgraf" @click="next(selectedLayer)">Next step</button>
          <button class="btn secondary" :disabled="!modeOgraf" @click="stop(selectedLayer)">Stop</button>
        </template>
      </div>
      <template v-if="loaded && manifest?.customActions?.length">
        <h3>Custom actions</h3>
        <div v-for="a in manifest.customActions" :key="a.id" class="custom-action">
          <SchemaForm v-if="a.schema" v-model="actionPayloads[a.id]" :schema="a.schema" :name="a.name" />
          <div v-if="actionMissing(a).length" class="msg err">Required: {{ actionMissing(a).join(", ") }}</div>
          <div class="actions">
            <span class="muted small grow">{{ a.description }}</span>
            <button class="btn secondary small" :disabled="!modeOgraf || actionMissing(a).length > 0" @click="custom(a.id)">{{ a.name }}</button>
          </div>
        </div>
      </template>
    </div>
  </div>

  <div v-if="!widget" class="panel" :class="{ dropping: dragging }" @dragover.prevent="dragging = true" @dragleave.self="dragging = false" @drop.prevent="onDrop">
    <h3>
      Library
      <button class="btn secondary small right-btn" @click="refresh">Refresh</button>
    </h3>
    <div class="row tight">
      <label class="btn" :class="{ disabled: upload.busy }">
        {{ upload.busy ? "Uploading…" : "Upload ZIP" }}
        <input type="file" accept=".zip,application/zip" class="sr-only" :disabled="upload.busy" @change="onFile" />
      </label>
      <span class="muted small">Or drop it here. One or more <code>*.ograf.json</code> graphics per ZIP; a ZIP is taken as a whole or not at all; the same id replaces a graphic.</span>
    </div>
    <div v-if="upload.report" class="msg" :class="upload.report.ok ? 'ok' : 'err'">
      <template v-if="upload.report.ok">
        Added: {{ upload.report.graphics.map((g) => g.name + (g.replaced ? " (replaced)" : "")).join(", ") }}
        <div v-for="g in upload.report.graphics.filter((x) => x.warnings.length)" :key="g.id" class="muted">{{ g.id }}: {{ g.warnings.join("; ") }}</div>
      </template>
      <template v-else>
        {{ upload.report.error }}
        <div v-for="m in upload.report.manifests.filter((x) => x.errors.length)" :key="m.path">{{ m.path }}: {{ m.errors.join("; ") }}</div>
      </template>
    </div>
    <div class="library">
      <div v-if="!graphics.length" class="muted">No graphics yet.</div>
      <div v-for="g in graphics" :key="g.id" class="graphic" :class="{ selected: g.id === selectedGraphic && !loaded }" @click="selectedGraphic = g.id">
        <div class="thumb"><img v-if="thumb(g)" :src="thumb(g)" alt="" /><span v-else class="muted small">no thumbnail</span></div>
        <div class="gname">{{ g.name }}</div>
        <div class="muted small">{{ g.id }}<template v-if="g.version"> · v{{ g.version }}</template><template v-if="(g.stepCount ?? 1) !== 1"> · {{ g.stepCount === -1 ? "steps" : g.stepCount + " steps" }}</template></div>
        <div v-if="inUseText(g)" class="small">{{ inUseText(g) }}</div>
        <div v-if="g.v_warnings?.length" class="small warn" :title="g.v_warnings.join('\n')">{{ g.v_warnings.length }} warning(s)</div>
        <div class="row tight" @click.stop>
          <template v-if="confirmDelete === g.id">
            <button class="btn danger small" @click="remove(g); confirmDelete = ''">{{ (g.v_inUse || []).length ? "Clear its layers and delete" : "Delete" }}</button>
            <button class="btn secondary small" @click="confirmDelete = ''">Cancel</button>
          </template>
          <button v-else class="btn secondary small" @click="confirmDelete = g.id">Delete</button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.layers tr { cursor: pointer; }
.layers tr.selected td { background: var(--panel2); }
.library { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: .7rem; margin-top: .7rem; }
.graphic { border: 1px solid var(--border); border-radius: 6px; padding: .5rem; cursor: pointer; display: flex; flex-direction: column; gap: .25rem; }
.graphic.selected { border-color: var(--accent); }
.thumb { aspect-ratio: 16 / 9; background: repeating-conic-gradient(#555 0 25%, #777 0 50%) 0 0 / 16px 16px; display: flex; align-items: center; justify-content: center; border-radius: 4px; overflow: hidden; }
.thumb img { max-width: 100%; max-height: 100%; }
.gname { font-weight: 600; }
.warn { color: var(--warn); }
.custom-action { border-top: 1px solid var(--border); padding-top: .4rem; margin-top: .4rem; }
.og-preview { aspect-ratio: 16 / 9; background: repeating-conic-gradient(#555 0 25%, #777 0 50%) 0 0 / 16px 16px; border-radius: 4px; overflow: hidden; display: flex; align-items: center; justify-content: center; margin-bottom: .7rem; }
.og-preview img { width: 100%; height: 100%; object-fit: contain; display: block; }
h3 .plain { text-transform: none; letter-spacing: normal; font-weight: 400; }
.dropping { outline: 2px dashed var(--accent); outline-offset: -4px; }
</style>
